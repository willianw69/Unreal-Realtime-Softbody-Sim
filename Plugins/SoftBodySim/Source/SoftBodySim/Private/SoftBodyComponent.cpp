// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyComponent.h"
#include "SoftBodyResources.h"
#include "SoftBodyMeshProxy.h"
#include "SoftBodySceneViewExtension.h"     // GDF snapshot registration (SB-M8)

#include "DrawDebugHelpers.h"
#include "DynamicMeshBuilder.h"            // FDynamicMeshVertex
#include "Engine/Engine.h"                 // GEngine on-screen debug
#include "Engine/StaticMesh.h"             // UStaticMesh (embedded custom mesh, SB-M5)
#include "Engine/World.h"
#include "GameFramework/PlayerController.h" // mouse pick / deproject (SB-M3)
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"
#include "StaticMeshResources.h"           // FStaticMeshLODResources CPU read (SB-M5)

USoftBodyComponent::USoftBodyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	CastShadow = true;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

//////////////////////////////////////////////////////////////////////////
// Setup
//////////////////////////////////////////////////////////////////////////

void USoftBodyComponent::BeginPlay()
{
	Super::BeginPlay();

	// Register the view extension that snapshots the Global Distance Field (SB-M8).
	SoftBodyGDF::EnsureRegistered();

	InitializeSimulation();

	// We now have geometry: rebuild bounds and recreate the (previously empty) proxy.
	UpdateBounds();
	MarkRenderStateDirty();

	// Show the cursor so the player can click-drag the body (SB-M3).
	if (bEnableMouseDrag)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				PC->bShowMouseCursor = true;
				PC->bEnableClickEvents = true;
				PC->bEnableMouseOverEvents = true;
			}
		}
	}
}

void USoftBodyComponent::BuildTets()
{
	Tets.Reset();
	Tets.Reserve((ResX - 1) * (ResY - 1) * (ResZ - 1) * 6);

	// 6-tet Kuhn / Freudenthal decomposition of each cube cell, all sharing the cell's
	// main diagonal (v000 -> v111). Using the SAME diagonal direction for every cell
	// makes the split face-conforming, so adjacent cells share faces without cracks.
	for (int32 Z = 0; Z < ResZ - 1; ++Z)
	{
		for (int32 Y = 0; Y < ResY - 1; ++Y)
		{
			for (int32 X = 0; X < ResX - 1; ++X)
			{
				const uint32 V000 = (uint32)LatticeIndex(X,     Y,     Z);
				const uint32 V100 = (uint32)LatticeIndex(X + 1, Y,     Z);
				const uint32 V010 = (uint32)LatticeIndex(X,     Y + 1, Z);
				const uint32 V110 = (uint32)LatticeIndex(X + 1, Y + 1, Z);
				const uint32 V001 = (uint32)LatticeIndex(X,     Y,     Z + 1);
				const uint32 V101 = (uint32)LatticeIndex(X + 1, Y,     Z + 1);
				const uint32 V011 = (uint32)LatticeIndex(X,     Y + 1, Z + 1);
				const uint32 V111 = (uint32)LatticeIndex(X + 1, Y + 1, Z + 1);

				Tets.Add(FSoftBodyTet{ V000, V100, V110, V111 });
				Tets.Add(FSoftBodyTet{ V000, V110, V010, V111 });
				Tets.Add(FSoftBodyTet{ V000, V010, V011, V111 });
				Tets.Add(FSoftBodyTet{ V000, V011, V001, V111 });
				Tets.Add(FSoftBodyTet{ V000, V001, V101, V111 });
				Tets.Add(FSoftBodyTet{ V000, V101, V100, V111 });
			}
		}
	}
}

void USoftBodyComponent::BuildBoundarySurface()
{
	Triangles.Reset();
	UV0.Reset();
	UV0.SetNumUninitialized(NumParticles);

	// Simple planar UVs (X,Y normalized); the lit material rarely needs more for M1.
	for (int32 i = 0; i < NumParticles; ++i)
	{
		const int32 X = i % ResX;
		const int32 Y = (i / ResX) % ResY;
		UV0[i] = FVector2f(
			(ResX > 1) ? (float)X / (float)(ResX - 1) : 0.0f,
			(ResY > 1) ? (float)Y / (float)(ResY - 1) : 0.0f);
	}

	// Emit one triangle with the winding that makes its area-weighted normal (computed
	// as Cross(E2,E1), the cloth M9 convention) point along OutwardDir. With smooth
	// per-vertex normals + a two-sided material this lights the box's outer surface.
	auto AddTri = [&](uint32 A, uint32 B, uint32 C, const FVector3f& Outward)
	{
		const FVector3f E1 = InitialLocalPositions[B] - InitialLocalPositions[A];
		const FVector3f E2 = InitialLocalPositions[C] - InitialLocalPositions[A];
		const FVector3f FaceN = FVector3f::CrossProduct(E2, E1);
		if (FVector3f::DotProduct(FaceN, Outward) >= 0.0f)
		{
			Triangles.Add(A); Triangles.Add(B); Triangles.Add(C);
		}
		else
		{
			Triangles.Add(A); Triangles.Add(C); Triangles.Add(B);
		}
	};

	auto AddQuad = [&](uint32 C00, uint32 C10, uint32 C11, uint32 C01, const FVector3f& Outward)
	{
		AddTri(C00, C10, C11, Outward);
		AddTri(C00, C11, C01, Outward);
	};

	// -Z and +Z faces (vary X,Y).
	for (int32 Y = 0; Y < ResY - 1; ++Y)
	{
		for (int32 X = 0; X < ResX - 1; ++X)
		{
			AddQuad(
				(uint32)LatticeIndex(X,     Y,     0),
				(uint32)LatticeIndex(X + 1, Y,     0),
				(uint32)LatticeIndex(X + 1, Y + 1, 0),
				(uint32)LatticeIndex(X,     Y + 1, 0),
				FVector3f(0, 0, -1));

			AddQuad(
				(uint32)LatticeIndex(X,     Y,     ResZ - 1),
				(uint32)LatticeIndex(X + 1, Y,     ResZ - 1),
				(uint32)LatticeIndex(X + 1, Y + 1, ResZ - 1),
				(uint32)LatticeIndex(X,     Y + 1, ResZ - 1),
				FVector3f(0, 0, 1));
		}
	}

	// -Y and +Y faces (vary X,Z).
	for (int32 Z = 0; Z < ResZ - 1; ++Z)
	{
		for (int32 X = 0; X < ResX - 1; ++X)
		{
			AddQuad(
				(uint32)LatticeIndex(X,     0, Z),
				(uint32)LatticeIndex(X + 1, 0, Z),
				(uint32)LatticeIndex(X + 1, 0, Z + 1),
				(uint32)LatticeIndex(X,     0, Z + 1),
				FVector3f(0, -1, 0));

			AddQuad(
				(uint32)LatticeIndex(X,     ResY - 1, Z),
				(uint32)LatticeIndex(X + 1, ResY - 1, Z),
				(uint32)LatticeIndex(X + 1, ResY - 1, Z + 1),
				(uint32)LatticeIndex(X,     ResY - 1, Z + 1),
				FVector3f(0, 1, 0));
		}
	}

	// -X and +X faces (vary Y,Z).
	for (int32 Z = 0; Z < ResZ - 1; ++Z)
	{
		for (int32 Y = 0; Y < ResY - 1; ++Y)
		{
			AddQuad(
				(uint32)LatticeIndex(0, Y,     Z),
				(uint32)LatticeIndex(0, Y + 1, Z),
				(uint32)LatticeIndex(0, Y + 1, Z + 1),
				(uint32)LatticeIndex(0, Y,     Z + 1),
				FVector3f(-1, 0, 0));

			AddQuad(
				(uint32)LatticeIndex(ResX - 1, Y,     Z),
				(uint32)LatticeIndex(ResX - 1, Y + 1, Z),
				(uint32)LatticeIndex(ResX - 1, Y + 1, Z + 1),
				(uint32)LatticeIndex(ResX - 1, Y,     Z + 1),
				FVector3f(1, 0, 0));
		}
	}
}

void USoftBodyComponent::BuildConstraints(
	TArray<FGPUConstraint>& OutConstraints,
	TArray<FSoftBodyColorRange>& OutColorRanges) const
{
	OutConstraints.Reset();
	OutColorRanges.Reset();

	// --- 1. Gather unique tet edges -----------------------------------------
	// Each tet has 6 edges; many are shared between the 6 tets of a cell and between
	// neighbouring cells. Dedup with a 64-bit (min,max) key. Rest length comes from the
	// actual rest geometry (so cube edges, face diagonals and the body diagonal all get
	// the correct length).
	static const int32 EdgeTable[6][2] = { {0,1},{0,2},{0,3},{1,2},{1,3},{2,3} };

	TArray<FGPUConstraint> Edges;
	Edges.Reserve(Tets.Num() * 6);

	TSet<uint64> SeenEdges;
	SeenEdges.Reserve(Tets.Num() * 6);

	auto TetVert = [](const FSoftBodyTet& T, int32 k) -> uint32
	{
		switch (k) { case 0: return T.V0; case 1: return T.V1; case 2: return T.V2; default: return T.V3; }
	};

	for (const FSoftBodyTet& T : Tets)
	{
		for (int32 e = 0; e < 6; ++e)
		{
			uint32 A = TetVert(T, EdgeTable[e][0]);
			uint32 B = TetVert(T, EdgeTable[e][1]);
			if (A == B)
			{
				continue;
			}
			const uint32 Lo = FMath::Min(A, B);
			const uint32 Hi = FMath::Max(A, B);
			const uint64 Key = ((uint64)Lo << 32) | (uint64)Hi;
			bool bAlready = false;
			SeenEdges.Add(Key, &bAlready);
			if (bAlready)
			{
				continue;
			}

			const float Rest = (InitialLocalPositions[Lo] - InitialLocalPositions[Hi]).Size();
			// Per-edge softness from the painted weights of its endpoints. StiffScale drives
			// the PBD path (SB-M6); Softness drives the XPBD path's compliance (SB-M7).
			const float AvgW = bHasWeightData ? 0.5f * (ParticleWeights[Lo] + ParticleWeights[Hi]) : 0.0f;
			const float Stiff = bHasWeightData ? WeightToStiffScale(AvgW) : 1.0f;
			Edges.Add(FGPUConstraint{ Lo, Hi, Rest, Stiff, AvgW });
		}
	}

	if (Edges.Num() == 0)
	{
		return;
	}

	// --- 2. Greedy graph coloring -------------------------------------------
	// Two constraints conflict if they share a particle. Assign each edge the smallest
	// color not yet used by either endpoint, so within a color no particle is touched
	// twice -> the GPU can project a whole color in parallel with no data races.
	TArray<int32> EdgeColor;
	EdgeColor.SetNumUninitialized(Edges.Num());

	TArray<TSet<int32>> UsedColorsAt;
	UsedColorsAt.SetNum(NumParticles);

	int32 NumColors = 0;
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const int32 A = (int32)Edges[e].IndexA;
		const int32 B = (int32)Edges[e].IndexB;

		int32 Color = 0;
		while (UsedColorsAt[A].Contains(Color) || UsedColorsAt[B].Contains(Color))
		{
			++Color;
		}

		EdgeColor[e] = Color;
		UsedColorsAt[A].Add(Color);
		UsedColorsAt[B].Add(Color);
		NumColors = FMath::Max(NumColors, Color + 1);
	}

	// --- 3. Bucket edges into a color-sorted buffer + ranges ----------------
	TArray<int32> CountPerColor;
	CountPerColor.Init(0, NumColors);
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		++CountPerColor[EdgeColor[e]];
	}

	OutColorRanges.SetNum(NumColors);
	int32 Running = 0;
	for (int32 c = 0; c < NumColors; ++c)
	{
		OutColorRanges[c].Start = Running;
		OutColorRanges[c].Count = CountPerColor[c];
		Running += CountPerColor[c];
	}

	// Stable scatter into the sorted positions using a per-color write cursor.
	OutConstraints.SetNumUninitialized(Edges.Num());
	TArray<int32> Cursor;
	Cursor.SetNumUninitialized(NumColors);
	for (int32 c = 0; c < NumColors; ++c)
	{
		Cursor[c] = OutColorRanges[c].Start;
	}
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const int32 c = EdgeColor[e];
		OutConstraints[Cursor[c]++] = Edges[e];
	}
}

void USoftBodyComponent::BuildVolumeConstraints(
	TArray<FGPUVolumeConstraint>& OutConstraints,
	TArray<FSoftBodyColorRange>& OutColorRanges) const
{
	OutConstraints.Reset();
	OutColorRanges.Reset();
	if (Tets.Num() == 0)
	{
		return;
	}

	// --- 1. One volume constraint per tet, with its signed rest volume ------
	// V0 = (1/6) dot(e1, e2 x e3) from the rest lattice (e_k = p_k - p_0). The sign is
	// consistent across tets (same vertex ordering from BuildTets), so C = V - V0 is well
	// defined regardless of the absolute sign.
	TArray<FGPUVolumeConstraint> Vols;
	Vols.Reserve(Tets.Num());
	for (const FSoftBodyTet& T : Tets)
	{
		const FVector3f& P0 = InitialLocalPositions[T.V0];
		const FVector3f& P1 = InitialLocalPositions[T.V1];
		const FVector3f& P2 = InitialLocalPositions[T.V2];
		const FVector3f& P3 = InitialLocalPositions[T.V3];
		const FVector3f E1 = P1 - P0;
		const FVector3f E2 = P2 - P0;
		const FVector3f E3 = P3 - P0;
		const float RestVol = (1.0f / 6.0f) * FVector3f::DotProduct(E1, FVector3f::CrossProduct(E2, E3));

		// Per-tet stiffness from the painted weights of its four vertices (SB-M6).
		const float Stiff = bHasWeightData
			? WeightToStiffScale(0.25f * (ParticleWeights[T.V0] + ParticleWeights[T.V1] + ParticleWeights[T.V2] + ParticleWeights[T.V3]))
			: 1.0f;
		Vols.Add(FGPUVolumeConstraint{ T.V0, T.V1, T.V2, T.V3, RestVol, Stiff });
	}

	// --- 2. Greedy graph coloring -------------------------------------------
	// Two tets conflict if they share ANY of their 4 vertices. Assign each tet the
	// smallest color free on all 4 endpoints, so within a color the (4-wide) writes are
	// disjoint -> race-free per-color GPU dispatch.
	TArray<int32> TetColor;
	TetColor.SetNumUninitialized(Vols.Num());

	TArray<TSet<int32>> UsedColorsAt;
	UsedColorsAt.SetNum(NumParticles);

	int32 NumColors = 0;
	for (int32 t = 0; t < Vols.Num(); ++t)
	{
		const FGPUVolumeConstraint& V = Vols[t];
		const uint32 Idx[4] = { V.I0, V.I1, V.I2, V.I3 };

		int32 Color = 0;
		for (;;)
		{
			bool bClash = false;
			for (int32 k = 0; k < 4; ++k)
			{
				if (UsedColorsAt[Idx[k]].Contains(Color))
				{
					bClash = true;
					break;
				}
			}
			if (!bClash)
			{
				break;
			}
			++Color;
		}

		TetColor[t] = Color;
		for (int32 k = 0; k < 4; ++k)
		{
			UsedColorsAt[Idx[k]].Add(Color);
		}
		NumColors = FMath::Max(NumColors, Color + 1);
	}

	// --- 3. Bucket tets into a color-sorted buffer + ranges -----------------
	TArray<int32> CountPerColor;
	CountPerColor.Init(0, NumColors);
	for (int32 t = 0; t < Vols.Num(); ++t)
	{
		++CountPerColor[TetColor[t]];
	}

	OutColorRanges.SetNum(NumColors);
	int32 Running = 0;
	for (int32 c = 0; c < NumColors; ++c)
	{
		OutColorRanges[c].Start = Running;
		OutColorRanges[c].Count = CountPerColor[c];
		Running += CountPerColor[c];
	}

	OutConstraints.SetNumUninitialized(Vols.Num());
	TArray<int32> Cursor;
	Cursor.SetNumUninitialized(NumColors);
	for (int32 c = 0; c < NumColors; ++c)
	{
		Cursor[c] = OutColorRanges[c].Start;
	}
	for (int32 t = 0; t < Vols.Num(); ++t)
	{
		const int32 c = TetColor[t];
		OutConstraints[Cursor[c]++] = Vols[t];
	}
}

// Barycentric weights of point P inside tet (A,B,C,D): returns (wA,wB,wC,wD). The point
// is inside the tet iff all four are >= 0. Returns a sentinel of all -1 for a degenerate tet.
static FVector4f TetBarycentric(
	const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D, const FVector3f& P)
{
	const FVector3f V0 = B - A;
	const FVector3f V1 = C - A;
	const FVector3f V2 = D - A;
	const FVector3f V3 = P - A;

	const float Denom = FVector3f::DotProduct(V0, FVector3f::CrossProduct(V1, V2));
	if (FMath::Abs(Denom) < 1e-8f)
	{
		return FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
	}
	const float Inv = 1.0f / Denom;
	const float Wb = FVector3f::DotProduct(V3, FVector3f::CrossProduct(V1, V2)) * Inv;
	const float Wc = FVector3f::DotProduct(V0, FVector3f::CrossProduct(V3, V2)) * Inv;
	const float Wd = FVector3f::DotProduct(V0, FVector3f::CrossProduct(V1, V3)) * Inv;
	const float Wa = 1.0f - Wb - Wc - Wd;
	return FVector4f(Wa, Wb, Wc, Wd);
}

bool USoftBodyComponent::ReadSourceMesh()
{
	MeshRestPositions.Reset();
	MeshTriangles.Reset();
	MeshUV0.Reset();
	NumMeshVerts = 0;

	if (!SourceMesh)
	{
		return false;
	}

	const FStaticMeshRenderData* RD = SourceMesh->GetRenderData();
	if (!RD || RD->LODResources.Num() == 0)
	{
		return false;
	}

	const FStaticMeshLODResources& LOD = RD->LODResources[0];
	const FPositionVertexBuffer& PVB = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& SVB = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const FColorVertexBuffer& CVB = LOD.VertexBuffers.ColorVertexBuffer;

	const int32 NumV = (int32)PVB.GetNumVertices();
	if (NumV == 0)
	{
		return false;
	}

	const bool bHasUV = (SVB.GetNumVertices() == (uint32)NumV) && (SVB.GetNumTexCoords() > 0);
	// Painted vertex colors → per-vertex weight (R channel). Many meshes have no colors.
	const bool bHasColor = (CVB.GetNumVertices() == (uint32)NumV);

	MeshRestPositions.SetNumUninitialized(NumV);
	MeshUV0.SetNumUninitialized(NumV);
	MeshVertWeights.Reset();
	if (bHasColor)
	{
		MeshVertWeights.SetNumUninitialized(NumV);
	}
	for (int32 i = 0; i < NumV; ++i)
	{
		MeshRestPositions[i] = PVB.VertexPosition(i);
		MeshUV0[i] = bHasUV ? SVB.GetVertexUV(i, 0) : FVector2f::ZeroVector;
		if (bHasColor)
		{
			MeshVertWeights[i] = CVB.VertexColor(i).R / 255.0f;
		}
	}

	LOD.IndexBuffer.GetCopy(MeshTriangles);

	if (MeshTriangles.Num() < 3)
	{
		MeshRestPositions.Reset();
		MeshUV0.Reset();
		return false;
	}

	NumMeshVerts = NumV;
	return true;
}

void USoftBodyComponent::BuildEmbedding()
{
	EmbedTet.SetNumUninitialized(NumMeshVerts);
	EmbedWeights.SetNumUninitialized(NumMeshVerts);

	const int32 CellsX = ResX - 1;
	const int32 CellsY = ResY - 1;

	for (int32 v = 0; v < NumMeshVerts; ++v)
	{
		const FVector3f P = MeshRestPositions[v];

		// Base cage cell for this vertex (clamped); search a 3x3x3 block around it so a
		// vertex near a cell boundary or just outside (padding/concavity) still binds.
		const int32 Bx = FMath::Clamp((int32)FMath::FloorToInt((P.X - CageMin.X) / CageCellSize.X), 0, CellsX - 1);
		const int32 By = FMath::Clamp((int32)FMath::FloorToInt((P.Y - CageMin.Y) / CageCellSize.Y), 0, CellsY - 1);
		const int32 Bz = FMath::Clamp((int32)FMath::FloorToInt((P.Z - CageMin.Z) / CageCellSize.Z), 0, ResZ - 2);

		int32   BestTet = 0;
		FVector4f BestW(0.25f, 0.25f, 0.25f, 0.25f);
		float   BestMin = -FLT_MAX; // maximise the minimum weight → best containment

		for (int32 dz = -1; dz <= 1; ++dz)
		for (int32 dy = -1; dy <= 1; ++dy)
		for (int32 dx = -1; dx <= 1; ++dx)
		{
			const int32 Cx = Bx + dx;
			const int32 Cy = By + dy;
			const int32 Cz = Bz + dz;
			if (Cx < 0 || Cy < 0 || Cz < 0 || Cx >= CellsX || Cy >= CellsY || Cz >= ResZ - 1)
			{
				continue;
			}

			// Cell → tet base (matches BuildTets iteration order: Z outer, Y, X inner, 6/cell).
			const int32 CellLinear = (Cz * CellsY + Cy) * CellsX + Cx;
			const int32 TetBase = CellLinear * 6;

			for (int32 t = 0; t < 6; ++t)
			{
				const FSoftBodyTet& T = Tets[TetBase + t];
				const FVector4f W = TetBarycentric(
					InitialLocalPositions[T.V0], InitialLocalPositions[T.V1],
					InitialLocalPositions[T.V2], InitialLocalPositions[T.V3], P);

				const float MinW = FMath::Min(FMath::Min(W.X, W.Y), FMath::Min(W.Z, W.W));
				if (MinW > BestMin)
				{
					BestMin = MinW;
					BestTet = TetBase + t;
					BestW = W;
				}
				if (MinW >= 0.0f)
				{
					// Fully contained — can't do better than this.
					dx = dy = dz = 2; // break all three loops
					break;
				}
			}
		}

		// If the best tet doesn't strictly contain the vertex (slightly outside), clamp the
		// weights to the tet's convex hull so it stays glued instead of flying off.
		if (BestMin < 0.0f)
		{
			FVector4f W = BestW;
			W.X = FMath::Max(W.X, 0.0f);
			W.Y = FMath::Max(W.Y, 0.0f);
			W.Z = FMath::Max(W.Z, 0.0f);
			W.W = FMath::Max(W.W, 0.0f);
			const float Sum = W.X + W.Y + W.Z + W.W;
			BestW = (Sum > KINDA_SMALL_NUMBER) ? (W * (1.0f / Sum)) : FVector4f(0.25f, 0.25f, 0.25f, 0.25f);
		}

		EmbedTet[v] = BestTet;
		EmbedWeights[v] = BestW;
	}
}

void USoftBodyComponent::BuildParticleWeights()
{
	ParticleWeights.Reset();
	bHasWeightData = false;

	// Only active with a mesh that has painted colors and the feature enabled.
	if (!bMeshMode || !bWeightPaintStiffness || MeshVertWeights.Num() != NumMeshVerts || NumMeshVerts == 0)
	{
		return;
	}

	// Each cage particle inherits the paint weight of the nearest mesh vertex (the closest
	// painted surface point). Brute-force nearest at init — fine at demo cage/mesh sizes.
	ParticleWeights.SetNumUninitialized(NumParticles);
	for (int32 p = 0; p < NumParticles; ++p)
	{
		const FVector3f Pp = InitialLocalPositions[p];
		float BestDistSq = FLT_MAX;
		float BestW = 1.0f;
		for (int32 v = 0; v < NumMeshVerts; ++v)
		{
			const float DistSq = FVector3f::DistSquared(Pp, MeshRestPositions[v]);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestW = MeshVertWeights[v];
			}
		}
		ParticleWeights[p] = bInvertWeightPaint ? (1.0f - BestW) : BestW;
	}

	bHasWeightData = true;
}

float USoftBodyComponent::WeightToStiffScale(float Weight) const
{
	if (!bHasWeightData)
	{
		return 1.0f;
	}
	// Weight 0 (firm) → FirmStiffnessScale; weight 1 (soft) → SoftStiffnessScale.
	return FMath::Lerp(FirmStiffnessScale, SoftStiffnessScale, FMath::Clamp(Weight, 0.0f, 1.0f));
}

void USoftBodyComponent::InitializeSimulation()
{
	ResX = FMath::Clamp(ResX, 2, 64);
	ResY = FMath::Clamp(ResY, 2, 64);
	ResZ = FMath::Clamp(ResZ, 2, 64);
	NumParticles = ResX * ResY * ResZ;

	// Surface particles (any on a face of the box) — the only ones the mouse can grab.
	BoundaryParticles.Reset();
	for (int32 Z = 0; Z < ResZ; ++Z)
	{
		for (int32 Y = 0; Y < ResY; ++Y)
		{
			for (int32 X = 0; X < ResX; ++X)
			{
				const bool bOnSurface =
					X == 0 || X == ResX - 1 ||
					Y == 0 || Y == ResY - 1 ||
					Z == 0 || Z == ResZ - 1;
				if (bOnSurface)
				{
					BoundaryParticles.Add(LatticeIndex(X, Y, Z));
				}
			}
		}
	}

	// A custom mesh (SB-M5) turns the lattice into a box CAGE auto-fit to the mesh bounds;
	// otherwise we simulate/render the default centered box.
	bMeshMode = ReadSourceMesh();

	// --- Cage local-space layout -------------------------------------------
	InitialLocalPositions.Reset();
	InitialLocalPositions.SetNumUninitialized(NumParticles);

	if (bMeshMode)
	{
		// Bounding box of the mesh, padded so surface verts sit inside the cage.
		FBox MeshBox(ForceInit);
		for (const FVector3f& P : MeshRestPositions)
		{
			MeshBox += FVector(P);
		}
		const FVector Pad = MeshBox.GetSize() * CagePadding;
		MeshBox.Min -= Pad;
		MeshBox.Max += Pad;
		// Guard against a flat axis (zero size → divide-by-zero in the cell mapping).
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (MeshBox.Max[Axis] - MeshBox.Min[Axis] < 1.0f)
			{
				MeshBox.Min[Axis] -= 0.5f;
				MeshBox.Max[Axis] += 0.5f;
			}
		}

		const FVector Size = MeshBox.GetSize();
		CageMin = FVector3f(MeshBox.Min);
		CageCellSize = FVector3f(Size.X / (ResX - 1), Size.Y / (ResY - 1), Size.Z / (ResZ - 1));
		EffectiveSpacing = FMath::Min3(CageCellSize.X, CageCellSize.Y, CageCellSize.Z);

		for (int32 Z = 0; Z < ResZ; ++Z)
		for (int32 Y = 0; Y < ResY; ++Y)
		for (int32 X = 0; X < ResX; ++X)
		{
			InitialLocalPositions[LatticeIndex(X, Y, Z)] = CageMin +
				FVector3f(X * CageCellSize.X, Y * CageCellSize.Y, Z * CageCellSize.Z);
		}
	}
	else
	{
		// Centered box in X/Y about the component origin, base at local Z = 0.
		const float HalfX = 0.5f * (ResX - 1) * Spacing;
		const float HalfY = 0.5f * (ResY - 1) * Spacing;
		CageMin = FVector3f(-HalfX, -HalfY, 0.0f);
		CageCellSize = FVector3f(Spacing, Spacing, Spacing);
		EffectiveSpacing = Spacing;

		for (int32 Z = 0; Z < ResZ; ++Z)
		for (int32 Y = 0; Y < ResY; ++Y)
		for (int32 X = 0; X < ResX; ++X)
		{
			InitialLocalPositions[LatticeIndex(X, Y, Z)] =
				FVector3f(X * Spacing - HalfX, Y * Spacing - HalfY, Z * Spacing);
		}
	}

	// --- World state from the cage layout ----------------------------------
	TArray<FVector3f> Positions;   // world space (sim runs in world space)
	TArray<FVector3f> Velocities;
	TArray<float>     InvMasses;
	Positions.SetNumUninitialized(NumParticles);
	Velocities.SetNumUninitialized(NumParticles);
	InvMasses.SetNumUninitialized(NumParticles);

	const FTransform& Xform = GetComponentTransform();
	for (int32 Z = 0; Z < ResZ; ++Z)
	for (int32 Y = 0; Y < ResY; ++Y)
	for (int32 X = 0; X < ResX; ++X)
	{
		const int32 i = LatticeIndex(X, Y, Z);
		Positions[i] = FVector3f(Xform.TransformPosition(FVector(InitialLocalPositions[i])));
		Velocities[i] = FVector3f::ZeroVector;

		const bool bPinned =
			(Anchor == ESoftBodyAnchor::TopFace && Z == ResZ - 1) ||
			(Anchor == ESoftBodyAnchor::BottomFace && Z == 0);
		InvMasses[i] = bPinned ? 0.0f : 1.0f;
	}

	BuildTets();
	if (bMeshMode)
	{
		BuildEmbedding();         // bind the mesh verts to the cage tets
		BuildParticleWeights();   // sample painted weights onto the cage (SB-M6)
	}
	else
	{
		BuildBoundarySurface();   // box-surface render geometry
		ParticleWeights.Reset();
		bHasWeightData = false;
	}

	// Generous local bounds so the body isn't frustum-culled as it sags/jiggles.
	FBox Box(ForceInit);
	for (const FVector3f& P : InitialLocalPositions)
	{
		Box += FVector(P);
	}
	const float Margin = (ResX + ResY + ResZ) * EffectiveSpacing;
	Box = Box.ExpandBy(Margin);
	LocalBounds = FBoxSphereBounds(Box);

	// Explicit distance constraints + graph coloring for the Gauss-Seidel solver.
	TArray<FGPUConstraint>      Constraints;
	TArray<FSoftBodyColorRange> ColorRanges;
	BuildConstraints(Constraints, ColorRanges);
	NumConstraintsBuilt = Constraints.Num();
	NumColorsBuilt      = ColorRanges.Num();

	// Per-tet volume constraints + their own graph coloring (SB-M2).
	TArray<FGPUVolumeConstraint> VolumeConstraints;
	TArray<FSoftBodyColorRange>  VolumeColorRanges;
	BuildVolumeConstraints(VolumeConstraints, VolumeColorRanges);
	NumVolumeConstraintsBuilt = VolumeConstraints.Num();
	NumVolumeColorsBuilt      = VolumeColorRanges.Num();

	RenderResources = MakeShared<FSoftBodyRenderResources>();

	TSharedPtr<FSoftBodyRenderResources> Resources = RenderResources;
	ENQUEUE_RENDER_COMMAND(SoftBodySimInit)(
		[Resources, Positions = MoveTemp(Positions), Velocities = MoveTemp(Velocities), InvMasses = MoveTemp(InvMasses),
		 Constraints = MoveTemp(Constraints), ColorRanges = MoveTemp(ColorRanges),
		 VolumeConstraints = MoveTemp(VolumeConstraints), VolumeColorRanges = MoveTemp(VolumeColorRanges)]
		(FRHICommandListImmediate& RHICmdList)
		{
			SoftBodyCompute::InitResources_RenderThread(RHICmdList, Resources, Positions, Velocities, InvMasses,
				Constraints, ColorRanges, VolumeConstraints, VolumeColorRanges);
		});
}

//////////////////////////////////////////////////////////////////////////
// Primitive / mesh interface
//////////////////////////////////////////////////////////////////////////

FBoxSphereBounds USoftBodyComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (NumParticles <= 0)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(1.0f), 1.0f);
	}
	return LocalBounds.TransformBy(LocalToWorld);
}

UMaterialInterface* USoftBodyComponent::GetMaterial(int32 ElementIndex) const
{
	return SoftBodyMaterial;
}

void USoftBodyComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (SoftBodyMaterial)
	{
		OutMaterials.Add(SoftBodyMaterial);
	}
}

FPrimitiveSceneProxy* USoftBodyComponent::CreateSceneProxy()
{
	if (NumParticles <= 0 || InitialLocalPositions.Num() != NumParticles)
	{
		return nullptr; // not initialised yet (e.g. editor, before BeginPlay)
	}

	// Render either the embedded custom mesh (SB-M5) or the box boundary surface.
	const TArray<FVector3f>& RestPos = bMeshMode ? MeshRestPositions : InitialLocalPositions;
	const TArray<uint32>&    RenderTris = bMeshMode ? MeshTriangles : Triangles;
	const TArray<FVector2f>& RenderUV = bMeshMode ? MeshUV0 : UV0;
	const int32 NumV = RestPos.Num();
	if (NumV == 0 || RenderTris.Num() == 0)
	{
		return nullptr;
	}

	// Seed the proxy with the initial (rest) mesh; normals computed from its triangles.
	TArray<FVector3f> SeedNormals, SeedTangents;
	ComputeNormalsTangents(RestPos, RenderTris, SeedNormals, SeedTangents);

	TArray<FDynamicMeshVertex> Vertices;
	Vertices.SetNumUninitialized(NumV);
	for (int32 i = 0; i < NumV; ++i)
	{
		FDynamicMeshVertex& V = Vertices[i];
		V.Position = RestPos[i];
		V.TextureCoordinate[0] = RenderUV[i];
		V.TangentX = SeedTangents[i];
		V.TangentZ = SeedNormals[i];
		V.Color = FColor::White;
	}

	return new FSoftBodyMeshSceneProxy(this, Vertices, RenderTris, SoftBodyMaterial);
}

//////////////////////////////////////////////////////////////////////////
// Per-frame
//////////////////////////////////////////////////////////////////////////

void USoftBodyComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!RenderResources.IsValid() || NumParticles <= 0)
	{
		return;
	}

	// Mouse pick/drag — updates GrabbedIndex / CurrentGrabTarget for the params below.
	UpdateMouseGrab();

	FSoftBodyParams Params;
	Params.NumParticles     = NumParticles;
	Params.ResX             = ResX;
	Params.ResY             = ResY;
	Params.ResZ             = ResZ;
	Params.DeltaTime        = FixedTimeStep;
	Params.Gravity          = FVector3f(Gravity);
	Params.Damping          = Damping;
	Params.Substeps         = Substeps;
	Params.SolverIterations = SolverIterations;
	Params.Stiffness        = Stiffness;
	Params.VolumeStiffness  = VolumeStiffness;
	Params.bUseXPBD            = bUseXPBD;
	Params.XpbdGlobalCompliance = XpbdGlobalCompliance;
	Params.XpbdSoftCompliance   = XpbdSoftCompliance;
	Params.Friction         = Friction;
	Params.bGroundPlane     = bGroundPlane;
	Params.GroundZ          = GroundHeight;

	// Distance-field collision (SB-M8).
	Params.bUseDistanceFieldCollision = bUseDistanceFieldCollision;
	Params.DFThickness                = DistanceFieldThickness;

	// Self-collision (SB-M4).
	Params.bSelfCollision          = bSelfCollision;
	Params.SelfThickness           = SelfCollisionScale * EffectiveSpacing;
	Params.SelfStiffness           = SelfCollisionStiffness;
	Params.SelfCollisionIterations = SelfCollisionIterations;

	// Sphere/capsule colliders (SB-M4) — build world-space from the authored local slots.
	const FTransform& ColliderXform = GetComponentTransform();
	Params.Colliders.Reserve(Colliders.Num());
	for (const FSoftBodyCollider& C : Colliders)
	{
		FGPUCollider G;
		G.Radius   = C.Radius;
		G.Friction = Friction;

		if (C.Type == ESoftBodyColliderType::Sphere)
		{
			const FVector World = ColliderXform.TransformPosition(C.Center);
			G.A = FVector3f(World);
			G.B = G.A; // degenerate capsule == sphere
		}
		else // Capsule: endpoints = center ± (axis * halfHeight), local then to world
		{
			const FVector Axis = C.Rotation.RotateVector(FVector::UpVector);
			const FVector LocalA = C.Center + Axis * C.HalfHeight;
			const FVector LocalB = C.Center - Axis * C.HalfHeight;
			G.A = FVector3f(ColliderXform.TransformPosition(LocalA));
			G.B = FVector3f(ColliderXform.TransformPosition(LocalB));
		}
		Params.Colliders.Add(G);
	}

	// Mouse grab (SB-M3) — world-space target pulled toward by the GPU grab pass.
	Params.bGrabActive   = bIsGrabbing && GrabbedIndex != INDEX_NONE;
	Params.GrabIndex     = GrabbedIndex;
	Params.GrabTarget    = FVector3f(CurrentGrabTarget);
	Params.GrabStiffness = GrabStiffness;

	// Fixed-timestep accumulator (frame-rate independent).
	TimeAccumulator += DeltaTime;
	TimeAccumulator = FMath::Min(TimeAccumulator, FixedTimeStep * MaxStepsPerFrame);

	int32 Steps = 0;
	while (TimeAccumulator >= FixedTimeStep && Steps < MaxStepsPerFrame)
	{
		TSharedPtr<FSoftBodyRenderResources> Resources = RenderResources;
		ENQUEUE_RENDER_COMMAND(SoftBodySimDispatch)(
			[Resources, Params](FRHICommandListImmediate& RHICmdList)
			{
				SoftBodyCompute::Dispatch_RenderThread(RHICmdList, Resources, Params);
			});

		TimeAccumulator -= FixedTimeStep;
		++Steps;
	}

	UpdateMeshFromSimulation();

	if (bDrawDebugPoints)
	{
		DrawDebug();
	}
	if (bDrawColliders)
	{
		DrawColliders();
	}

	// Grab feedback: a sphere at the cursor target + a line to the grabbed particle.
	if (bIsGrabbing && GrabbedIndex != INDEX_NONE)
	{
		if (UWorld* World = GetWorld())
		{
			DrawDebugSphere(World, CurrentGrabTarget, 6.0f, 12, FColor::Yellow, false, -1.0f, SDPG_World, 0.5f);

			FScopeLock Lock(&RenderResources->PositionCopyCS);
			if (RenderResources->bHasPositionData && RenderResources->PositionCopy.IsValidIndex(GrabbedIndex))
			{
				DrawDebugLine(World, FVector(RenderResources->PositionCopy[GrabbedIndex]), CurrentGrabTarget,
					FColor::Yellow, false, -1.0f, SDPG_World, 0.5f);
			}
		}
	}

	if (bShowStats && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("SoftBody  cage=%dx%dx%d  particles=%d  tets=%d%s%s"),
				ResX, ResY, ResZ, NumParticles, Tets.Num(),
				bMeshMode ? *FString::Printf(TEXT("  | embedded mesh verts=%d"), NumMeshVerts) : TEXT(""),
				bHasWeightData ? TEXT("  | weight-paint stiffness ON") : TEXT("")));

		// Total solve dispatches per substep = iters * (distance colors + volume colors).
		const int32 SolveDispatches = SolverIterations * (NumColorsBuilt + NumVolumeColorsBuilt);
		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 2, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("  [%s]  dist: constraints=%d colors=%d  |  vol: tets=%d colors=%d  |  substeps=%d iters=%d  dispatches/substep=%d"),
				bUseXPBD ? TEXT("XPBD") : TEXT("PBD"),
				NumConstraintsBuilt, NumColorsBuilt, NumVolumeConstraintsBuilt, NumVolumeColorsBuilt,
				Substeps, SolverIterations, SolveDispatches));
	}

	// SB-M8 diagnostic: report whether the Global Distance Field snapshot is reaching us.
	if (bUseDistanceFieldCollision && GEngine)
	{
		const FSoftBodyGDFCache& Cache = SoftBodyGDF::Get();
		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 3, 0.0f,
			Cache.bValid ? FColor::Green : FColor::Red,
			FString::Printf(TEXT("SoftBody GDF: valid=%d  clipmaps=%d  (needs 'Generate Mesh Distance Fields')"),
				Cache.bValid ? 1 : 0,
				Cache.bValid ? Cache.Data.NumGlobalSDFClipmaps : 0));
	}
}

void USoftBodyComponent::ComputeNormalsTangents(
	const TArray<FVector3f>& InPositions,
	const TArray<uint32>& InTriangles,
	TArray<FVector3f>& OutNormals,
	TArray<FVector3f>& OutTangents) const
{
	const int32 Count = InPositions.Num();
	OutNormals.Init(FVector3f::ZeroVector, Count);
	OutTangents.Init(FVector3f::ZeroVector, Count);

	// Area-weighted face normals accumulated to each vertex. Cross(E2,E1) (not E1,E2) so
	// the smooth normal agrees with UE's left-handed front-face winding, which is what
	// makes a TWO-SIDED material shade correctly (cloth M9 lesson).
	for (int32 t = 0; t + 2 < InTriangles.Num(); t += 3)
	{
		const uint32 I0 = InTriangles[t];
		const uint32 I1 = InTriangles[t + 1];
		const uint32 I2 = InTriangles[t + 2];

		const FVector3f E1 = InPositions[I1] - InPositions[I0];
		const FVector3f E2 = InPositions[I2] - InPositions[I0];
		const FVector3f FaceN = FVector3f::CrossProduct(E2, E1);

		OutNormals[I0] += FaceN;
		OutNormals[I1] += FaceN;
		OutNormals[I2] += FaceN;
	}

	for (int32 i = 0; i < Count; ++i)
	{
		const FVector3f N = OutNormals[i].GetSafeNormal(SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
		OutNormals[i] = N;

		// Arbitrary tangent perpendicular to N (no normal-mapping for M1, so direction is
		// cosmetic; just keep it stable and orthonormal).
		FVector3f T = FVector3f::CrossProduct(N, FVector3f(0.0f, 1.0f, 0.0f));
		if (T.SizeSquared() < KINDA_SMALL_NUMBER)
		{
			T = FVector3f::CrossProduct(N, FVector3f(1.0f, 0.0f, 0.0f));
		}
		OutTangents[i] = T.GetSafeNormal(SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
	}
}

void USoftBodyComponent::UpdateMeshFromSimulation()
{
	FSoftBodyMeshSceneProxy* Proxy = static_cast<FSoftBodyMeshSceneProxy*>(SceneProxy);
	if (!Proxy || !RenderResources.IsValid())
	{
		return;
	}

	// Copy the latest world-space positions out of the readback buffer, to local space,
	// and track their local-space bounding box this frame.
	FBox LocalBox(ForceInit);
	{
		FScopeLock Lock(&RenderResources->PositionCopyCS);
		if (!RenderResources->bHasPositionData || RenderResources->PositionCopy.Num() != NumParticles)
		{
			return; // readback not ready yet
		}

		const FTransform WorldToLocal = GetComponentTransform().Inverse();
		LocalPositions.SetNumUninitialized(NumParticles);
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const FVector World(RenderResources->PositionCopy[i]);
			LocalPositions[i] = FVector3f(WorldToLocal.TransformPosition(World));
			LocalBox += FVector(LocalPositions[i]);
		}
	}

	// Bounds follow the deformed body so it isn't frustum-culled (vanishing) once it falls /
	// is dragged / pushed far from the actor origin. The cage encloses the embedded mesh, so
	// the cage's box covers the rendered mesh too. Push the new bounds to the render thread.
	LocalBounds = FBoxSphereBounds(LocalBox.ExpandBy(EffectiveSpacing));
	UpdateBounds();
	MarkRenderTransformDirty();

	if (bMeshMode)
	{
		// Reconstruct each mesh vertex from its cage tet via the barycentric weights.
		MeshLocalPositions.SetNumUninitialized(NumMeshVerts);
		for (int32 v = 0; v < NumMeshVerts; ++v)
		{
			const FSoftBodyTet& T = Tets[EmbedTet[v]];
			const FVector4f& W = EmbedWeights[v];
			MeshLocalPositions[v] =
				W.X * LocalPositions[T.V0] +
				W.Y * LocalPositions[T.V1] +
				W.Z * LocalPositions[T.V2] +
				W.W * LocalPositions[T.V3];
		}

		ComputeNormalsTangents(MeshLocalPositions, MeshTriangles, MeshNormals, MeshTangents);

		ENQUEUE_RENDER_COMMAND(SoftBodyMeshUpdate)(
			[Proxy, Positions = MeshLocalPositions, Normals = MeshNormals, Tangents = MeshTangents]
			(FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdateVertices_RenderThread(RHICmdList, Positions, Normals, Tangents);
			});
		return;
	}

	ComputeNormalsTangents(LocalPositions, Triangles, LocalNormals, LocalTangents);

	// Hand the new vertex data to the proxy on the render thread.
	ENQUEUE_RENDER_COMMAND(SoftBodyMeshUpdate)(
		[Proxy, Positions = LocalPositions, Normals = LocalNormals, Tangents = LocalTangents]
		(FRHICommandListImmediate& RHICmdList)
		{
			Proxy->UpdateVertices_RenderThread(RHICmdList, Positions, Normals, Tangents);
		});
}

void USoftBodyComponent::DrawColliders()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& Xform = GetComponentTransform();
	const FColor Color = FColor::Yellow;

	for (const FSoftBodyCollider& C : Colliders)
	{
		if (C.Type == ESoftBodyColliderType::Sphere)
		{
			const FVector Center = Xform.TransformPosition(C.Center);
			DrawDebugSphere(World, Center, C.Radius, 16, Color, false, -1.0f, SDPG_World, 0.5f);
		}
		else // Capsule
		{
			const FVector Center = Xform.TransformPosition(C.Center);
			// UE's DrawDebugCapsule half-height is centre->tip (includes the hemisphere),
			// while our HalfHeight is the segment half-length, so add the radius.
			const FQuat Rot = (Xform.GetRotation() * C.Rotation.Quaternion());
			DrawDebugCapsule(World, Center, C.HalfHeight + C.Radius, C.Radius, Rot, Color, false, -1.0f, SDPG_World, 0.5f);
		}
	}
}

void USoftBodyComponent::UpdateMouseGrab()
{
	// Default to "not grabbing"; any early-out below leaves the body free.
	auto ClearGrab = [&]()
	{
		bIsGrabbing = false;
		GrabbedIndex = INDEX_NONE;
	};

	if (!bEnableMouseDrag || !RenderResources.IsValid())
	{
		ClearGrab();
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		ClearGrab();
		return;
	}

	// Released (or never pressed): drop the grab so the body springs back.
	if (!PC->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		ClearGrab();
		return;
	}

	// Cursor world ray.
	FVector RayOrigin, RayDir;
	if (!PC->DeprojectMousePositionToWorld(RayOrigin, RayDir))
	{
		return; // keep any existing grab; just can't update the ray this frame
	}
	RayDir = RayDir.GetSafeNormal();

	// On the first frame of a press, pick the boundary particle whose world position lies
	// closest to the click ray (and is in front of the camera).
	if (!bIsGrabbing)
	{
		FScopeLock Lock(&RenderResources->PositionCopyCS);
		if (!RenderResources->bHasPositionData || RenderResources->PositionCopy.Num() != NumParticles)
		{
			return; // readback not ready yet
		}

		float BestDistSq = TNumericLimits<float>::Max();
		int32 BestIndex = INDEX_NONE;
		float BestDepth = 0.0f;

		for (int32 Idx : BoundaryParticles)
		{
			const FVector P(RenderResources->PositionCopy[Idx]);
			const float Depth = FVector::DotProduct(P - RayOrigin, RayDir);
			if (Depth <= 0.0f)
			{
				continue; // behind the camera
			}
			const FVector Closest = RayOrigin + RayDir * Depth;
			const float DistSq = FVector::DistSquared(P, Closest);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestIndex = Idx;
				BestDepth = Depth;
			}
		}

		const float PickRadius = EffectiveSpacing * GrabPickRadiusScale;
		if (BestIndex != INDEX_NONE && BestDistSq <= PickRadius * PickRadius)
		{
			GrabbedIndex = BestIndex;
			GrabDepth = BestDepth;
			bIsGrabbing = true;
		}
		else
		{
			return; // clicked empty space — nothing grabbed
		}
	}

	// While held: keep the target at the picked depth, following the cursor in the view plane.
	if (bIsGrabbing && GrabbedIndex != INDEX_NONE)
	{
		CurrentGrabTarget = RayOrigin + RayDir * GrabDepth;
	}
}

void USoftBodyComponent::DrawDebug()
{
	UWorld* World = GetWorld();
	if (!World || !RenderResources.IsValid())
	{
		return;
	}

	FScopeLock Lock(&RenderResources->PositionCopyCS);
	if (!RenderResources->bHasPositionData)
	{
		return;
	}

	// Optionally tint each cage particle by its painted weight (firm = blue → soft = red)
	// so the transferred weight paint is visible.
	const bool bWeightTint = bVisualizeWeights && bHasWeightData && ParticleWeights.Num() == RenderResources->PositionCopy.Num();

	for (int32 i = 0; i < RenderResources->PositionCopy.Num(); ++i)
	{
		FColor Color = FColor::Cyan;
		if (bWeightTint)
		{
			const float W = ParticleWeights[i];
			Color = FMath::Lerp(FLinearColor(0.0f, 0.35f, 1.0f), FLinearColor(1.0f, 0.0f, 0.0f), W).ToFColor(false);
		}
		DrawDebugPoint(World, FVector(RenderResources->PositionCopy[i]), DebugPointSize, Color, false, -1.0f, SDPG_World);
	}
}

void USoftBodyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (RenderResources.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(SoftBodySimRelease)(
			[Resources = MoveTemp(RenderResources)](FRHICommandListImmediate&) mutable
			{
				Resources.Reset();
			});
	}

	Super::EndPlay(EndPlayReason);
}
