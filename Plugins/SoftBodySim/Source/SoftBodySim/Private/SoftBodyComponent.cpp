// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyComponent.h"
#include "SoftBodyResources.h"
#include "SoftBodyMeshProxy.h"
#include "SoftBodySceneViewExtension.h"     // GDF snapshot registration (SB-M8)
#include "SoftBodyWorldSubsystem.h"         // inter-body collision registration (SB-M9)

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

	// Master switch (evaluated here): leave the body completely inert when disabled — no
	// sim resources, no proxy, no inter-body registration. Set bActive before Play.
	if (!bActive)
	{
		return;
	}

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

	// Register for inter-body collision coordination (SB-M9).
	if (UWorld* World = GetWorld())
	{
		if (USoftBodyWorldSubsystem* Sub = World->GetSubsystem<USoftBodyWorldSubsystem>())
		{
			Sub->RegisterComponent(this);
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

void USoftBodyComponent::BuildTetBoundarySurface()
{
	Triangles.Reset();

	// Planar UVs per particle (built once; same convention as the box surface).
	if (UV0.Num() != NumParticles)
	{
		UV0.SetNumUninitialized(NumParticles);
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const int32 X = i % ResX;
			const int32 Y = (i / ResX) % ResY;
			UV0[i] = FVector2f(
				(ResX > 1) ? (float)X / (float)(ResX - 1) : 0.0f,
				(ResY > 1) ? (float)Y / (float)(ResY - 1) : 0.0f);
		}
	}

	// A face shared by two surviving tets is interior; a face used by exactly one surviving
	// tet is on the boundary. Cutting removes (breaks) tets, so faces that were interior
	// become boundary → the cut surface appears. Key = sorted vertex triple.
	const int64 K = FMath::Max(1, NumParticles);
	auto FaceKey = [K](uint32 a, uint32 b, uint32 c) -> uint64
	{
		uint32 s0 = a, s1 = b, s2 = c;
		if (s0 > s1) Swap(s0, s1);
		if (s1 > s2) Swap(s1, s2);
		if (s0 > s1) Swap(s0, s1);
		return ((uint64)s0 * (uint64)K + (uint64)s1) * (uint64)K + (uint64)s2;
	};

	// A tet is REMOVED from the surface only if it's genuinely severed — its 4 particles span more
	// than one connected chunk (a cut/tear crossed it). A tet whose volume constraint was merely
	// DISABLED for stability (BreakVolumeShell drops a one-tet shell around every cut/tear so the
	// PBD volume solve can't explode it) but whose 4 particles are still one chunk MUST stay, or the
	// whole shell — and any small torn-off piece that's mostly shell — would vanish. At init (no
	// breaks) everything is one component, so the full box surface is produced.
	TArray<int32> Comp;
	ComputeParticleComponents(Comp);
	const bool bHaveComp = (Comp.Num() == NumParticles);

	struct FFaceRec { uint32 A, B, C, Opp; int32 Count; };
	TMap<uint64, FFaceRec> Faces;
	Faces.Reserve(VolumeConstraintsCPU.Num() * 4);

	auto AddFace = [&](uint32 a, uint32 b, uint32 c, uint32 opp)
	{
		const uint64 Key = FaceKey(a, b, c);
		if (FFaceRec* Rec = Faces.Find(Key))
		{
			Rec->Count++;
		}
		else
		{
			Faces.Add(Key, FFaceRec{ a, b, c, opp, 1 });
		}
	};

	for (int32 t = 0; t < VolumeConstraintsCPU.Num(); ++t)
	{
		const FGPUVolumeConstraint& V = VolumeConstraintsCPU[t];
		if (bHaveComp)
		{
			const int32 C0 = Comp[V.I0];
			if (Comp[V.I1] != C0 || Comp[V.I2] != C0 || Comp[V.I3] != C0)
			{
				continue; // tet torn across a cut/tear → removed from the surface
			}
		}
		AddFace(V.I1, V.I2, V.I3, V.I0);
		AddFace(V.I0, V.I2, V.I3, V.I1);
		AddFace(V.I0, V.I1, V.I3, V.I2);
		AddFace(V.I0, V.I1, V.I2, V.I3);
	}

	Triangles.Reserve(Faces.Num() * 3);
	for (const TPair<uint64, FFaceRec>& It : Faces)
	{
		const FFaceRec& F = It.Value;
		if (F.Count != 1)
		{
			continue; // interior face (shared by two surviving tets)
		}

		// Orient so the smooth normal (Cross(E2,E1) = cross(pc-pa, pb-pa)) points AWAY from
		// the tet's opposite vertex (outward). Use rest positions for a stable winding.
		const FVector3f Pa = InitialLocalPositions[F.A];
		const FVector3f Pb = InitialLocalPositions[F.B];
		const FVector3f Pc = InitialLocalPositions[F.C];
		const FVector3f FaceN = FVector3f::CrossProduct(Pc - Pa, Pb - Pa);
		const FVector3f Outward = ((Pa + Pb + Pc) / 3.0f) - InitialLocalPositions[F.Opp];
		if (FVector3f::DotProduct(FaceN, Outward) >= 0.0f)
		{
			Triangles.Add(F.A); Triangles.Add(F.B); Triangles.Add(F.C);
		}
		else
		{
			Triangles.Add(F.A); Triangles.Add(F.C); Triangles.Add(F.B);
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

void USoftBodyComponent::EmbedPointRest(const FVector3f& P, int32& OutTet, FVector4f& OutWeights,
	const TArray<int32>* ParticleComponent, int32 RequiredComponent) const
{
	const bool bFilter = (ParticleComponent != nullptr) && (RequiredComponent != INDEX_NONE)
		&& (ParticleComponent->Num() == NumParticles);
	const TArray<int32>* PC = bFilter ? ParticleComponent : nullptr;

	const int32 CellsX = ResX - 1;
	const int32 CellsY = ResY - 1;

	// Base cage cell for this point (clamped). Search a block around it: a single cell for the
	// plain embed, but WIDER when re-binding across a cut, because the only tets in this vert's own
	// CHUNK may be a couple of cells away (its near neighbours got severed by the cut).
	const int32 Bx = FMath::Clamp((int32)FMath::FloorToInt((P.X - CageMin.X) / CageCellSize.X), 0, CellsX - 1);
	const int32 By = FMath::Clamp((int32)FMath::FloorToInt((P.Y - CageMin.Y) / CageCellSize.Y), 0, CellsY - 1);
	const int32 Bz = FMath::Clamp((int32)FMath::FloorToInt((P.Z - CageMin.Z) / CageCellSize.Z), 0, ResZ - 2);

	int32   BestTet = 0;
	FVector4f BestW(0.25f, 0.25f, 0.25f, 0.25f);
	float   BestMin = -FLT_MAX; // maximise the minimum weight → best containment

	// Filtered (post-cut, SB-M11): best tet whose 4 particles are ALL in RequiredComponent (one
	// physical chunk). A tet spanning components is torn across a cut and would blend far-apart
	// particles into a spanning sheet — so it's never a candidate.
	int32   BestCompTet = INDEX_NONE;
	FVector4f BestCompW(0.25f, 0.25f, 0.25f, 0.25f);
	float   BestCompMin = -FLT_MAX;

	// Fallback target: the single highest-weight cage PARTICLE that is in RequiredComponent. Used
	// only if NO fully-in-component tet is reachable (thin sliver / fragmented region) — we then
	// rigidly bind to this one particle (weight 1) so the vert still follows its own chunk and can
	// never blend across a tear (which is what was still spiking the caps).
	int32   BestPartTet = INDEX_NONE;
	int32   BestPartSlot = 0;
	float   BestPartW = -FLT_MAX;

	const int32 R = bFilter ? 3 : 1; // widen the search when re-binding across a cut

	for (int32 dz = -R; dz <= R; ++dz)
	for (int32 dy = -R; dy <= R; ++dy)
	for (int32 dx = -R; dx <= R; ++dx)
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

			// In-component candidate: all 4 cage particles in the vert's own chunk (intact tet).
			// Also track the best single in-component particle for the point-bind fallback.
			if (bFilter)
			{
				const int32 Idx4[4] = { T.V0, T.V1, T.V2, T.V3 };
				const float W4[4]   = { W.X, W.Y, W.Z, W.W };
				bool bInComp = true;
				for (int32 k = 0; k < 4; ++k)
				{
					if ((*PC)[Idx4[k]] == RequiredComponent)
					{
						if (W4[k] > BestPartW) { BestPartW = W4[k]; BestPartTet = TetBase + t; BestPartSlot = k; }
					}
					else
					{
						bInComp = false;
					}
				}
				if (bInComp && MinW > BestCompMin)
				{
					BestCompMin = MinW; BestCompTet = TetBase + t; BestCompW = W;
				}
			}

			// Early-out only when NOT filtering: a fully-contained tet is optimal for the plain
			// embed, but the filtered path must keep scanning for an in-component tet.
			if (!bFilter && MinW >= 0.0f)
			{
				dx = dy = dz = R + 1; // break all three loops
				break;
			}
		}
	}

	// Filtered (post-cut): bind to the best-containing INTACT tet in this vert's own chunk, using
	// CONVEX (clamped, non-negative) weights. A cut vert sits ON the cut plane, just OUTSIDE its
	// chunk's tets, so raw barycentric weights are non-convex (some negative) and would EXTRAPOLATE
	// — amplifying the loosened cut-boundary tets' motion into long spikes. Convex weights bound the
	// vert to its 4 cage particles' hull (no blow-up); the best-containing tet keeps the inset well
	// under a cell. Binding strictly within the chunk is what stops the 2nd-cut spanning sheets.
	if (bFilter && BestCompTet != INDEX_NONE)
	{
		FVector4f W = BestCompW;
		W.X = FMath::Max(W.X, 0.0f);
		W.Y = FMath::Max(W.Y, 0.0f);
		W.Z = FMath::Max(W.Z, 0.0f);
		W.W = FMath::Max(W.W, 0.0f);
		const float Sum = W.X + W.Y + W.Z + W.W;
		OutTet = BestCompTet;
		OutWeights = (Sum > KINDA_SMALL_NUMBER) ? (W * (1.0f / Sum)) : FVector4f(0.25f, 0.25f, 0.25f, 0.25f);
		return;
	}

	// No fully-in-component tet nearby: rigidly bind to the best in-component PARTICLE (weight 1).
	// This keeps the vert glued to its own chunk and stable (it just follows one real particle) —
	// crucially it NEVER blends across a tear, so it can't spike like the unfiltered torn-tet path.
	if (bFilter && BestPartTet != INDEX_NONE)
	{
		OutTet = BestPartTet;
		FVector4f W(0.0f, 0.0f, 0.0f, 0.0f);
		switch (BestPartSlot) { case 0: W.X = 1.0f; break; case 1: W.Y = 1.0f; break; case 2: W.Z = 1.0f; break; default: W.W = 1.0f; break; }
		OutWeights = W;
		return;
	}

	// Unfiltered (or no same-side tet nearby): use the best-containing tet, clamping weights to
	// its convex hull if the point sits slightly outside so it stays glued instead of flying off.
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

	OutTet = BestTet;
	OutWeights = BestW;
}

void USoftBodyComponent::BuildEmbedding()
{
	EmbedTet.SetNumUninitialized(NumMeshVerts);
	EmbedWeights.SetNumUninitialized(NumMeshVerts);

	for (int32 v = 0; v < NumMeshVerts; ++v)
	{
		EmbedPointRest(MeshRestPositions[v], EmbedTet[v], EmbedWeights[v]);
	}
}

void USoftBodyComponent::ClipMeshAlongPlane(const FVector3f& LocalP, const FVector3f& LocalN, const TArray<int32>& ParticleComponent)
{
	if (MeshLocalPositions.Num() != NumMeshVerts || NumMeshVerts == 0 || MeshTriangles.Num() < 3)
	{
		return;
	}
	const bool bHaveComp = (ParticleComponent.Num() == NumParticles);

	const int32 OrigVerts = NumMeshVerts;

	// Does a cage tet (geometric Tets[] index) span more than one chunk (torn across a cut)?
	auto TetMultiComp = [&](int32 GeomTet) -> bool
	{
		if (!Tets.IsValidIndex(GeomTet) || !bHaveComp)
		{
			return false;
		}
		const FSoftBodyTet& T = Tets[GeomTet];
		const int32 C = ParticleComponent[T.V0];
		return ParticleComponent[T.V1] != C || ParticleComponent[T.V2] != C || ParticleComponent[T.V3] != C;
	};

	// Per-particle side of THIS cut plane, in deformed local space (the SAME plane the mesh verts
	// are classified against just below — so vert side and particle side are consistent).
	TArray<int32> PartSide;
	const bool bHaveSide = bHaveComp && (LocalPositions.Num() == NumParticles);
	if (bHaveSide)
	{
		PartSide.SetNumUninitialized(NumParticles);
		for (int32 p = 0; p < NumParticles; ++p)
		{
			PartSide[p] = (FVector3f::DotProduct(LocalPositions[p] - LocalP, LocalN) >= 0.0f) ? +1 : -1;
		}
	}

	// Side of each vertex in DEFORMED space (where the user sees the body). >= 0 = "+". Drives the
	// triangle split AND which side's cage particle each vert routes to.
	TArray<float> Side;
	Side.SetNumUninitialized(OrigVerts);
	for (int32 i = 0; i < OrigVerts; ++i)
	{
		Side[i] = FVector3f::DotProduct(MeshLocalPositions[i] - LocalP, LocalN);
	}
	auto IsPlus = [&](int32 i) { return Side[i] >= 0.0f; };

	// The chunk (connected component) each ORIGINAL vert belongs to: among its embedding tet's 4
	// cage particles, take the highest-weight one ON THE VERT'S OWN SIDE of the cut — its component
	// is the vert's post-cut piece. Routing by SIDE (not just dominant weight) is essential: a vert
	// right at the cut often has its single largest weight on a particle just ACROSS the plane,
	// which would bind it to the wrong chunk and fling it across the gap (the spanning sheets).
	// Components on top of side then disambiguate chunks created by EARLIER cuts. Falls back to the
	// overall max-weight particle if none of the 4 is on the vert's side (rare).
	auto VertChunk = [&](int32 v) -> int32
	{
		if (!bHaveComp || !Tets.IsValidIndex(EmbedTet[v]))
		{
			return INDEX_NONE;
		}
		const FSoftBodyTet& T = Tets[EmbedTet[v]];
		const FVector4f& W = EmbedWeights[v];
		const int32 Idx[4] = { T.V0, T.V1, T.V2, T.V3 };
		const float Wt[4]  = { W.X, W.Y, W.Z, W.W };
		const int32 VS = IsPlus(v) ? +1 : -1;
		// Among the embedding tet's particles on the vert's side, pick the COMPONENT carrying the
		// most total weight (not just the single max-weight particle). Summing per component avoids
		// routing the vert to a tiny/singleton fragment (a particle isolated by cuts) when a real
		// chunk is also present — a singleton target has no intact tet and forced the torn-tet spike.
		int32 BestComp = INDEX_NONE; float BestCompWSum = -1.0f;
		int32 BestAny = T.V0;        float BestAnyW = W.X;
		for (int32 k = 0; k < 4; ++k)
		{
			if (Wt[k] > BestAnyW) { BestAnyW = Wt[k]; BestAny = Idx[k]; }
			if (bHaveSide && PartSide[Idx[k]] != VS) { continue; } // consider only the vert's side
			const int32 Comp = ParticleComponent[Idx[k]];
			float Sum = 0.0f;
			for (int32 j = 0; j < 4; ++j)
			{
				if (bHaveSide && PartSide[Idx[j]] != VS) { continue; }
				if (ParticleComponent[Idx[j]] == Comp) { Sum += Wt[j]; }
			}
			if (Sum > BestCompWSum) { BestCompWSum = Sum; BestComp = Comp; }
		}
		return (BestComp != INDEX_NONE) ? BestComp : ParticleComponent[BestAny];
	};
	TArray<int32> VertComponent;
	VertComponent.SetNumUninitialized(OrigVerts);
	for (int32 v = 0; v < OrigVerts; ++v)
	{
		VertComponent[v] = VertChunk(v);
	}

	// New geometry starts as a copy of the originals (kept; unused verts are harmless). New
	// seam/cap verts are appended. Rest position drives the embedding; new verts get rest
	// positions on the rest edge at the same parameter as the deformed crossing.
	TArray<FVector3f> NewRest = MeshRestPositions;
	TArray<FVector2f> NewUV   = MeshUV0;
	TArray<int32>     NewTet  = EmbedTet;
	TArray<FVector4f> NewW    = EmbedWeights;
	TArray<uint32>    NewTris;
	NewTris.Reserve(MeshTriangles.Num() * 2 + 64);

	// New verts embed into an intact tet within the TARGET CHUNK so they follow their half cleanly.
	auto AppendVert = [&](const FVector3f& R, const FVector2f& U, int32 CompTarget) -> int32
	{
		int32 Tet; FVector4f W;
		EmbedPointRest(R, Tet, W, bHaveComp ? &ParticleComponent : nullptr, CompTarget);
		const int32 Idx = NewRest.Add(R);
		NewUV.Add(U); NewTet.Add(Tet); NewW.Add(W);
		return Idx;
	};

	// Per mesh-edge intersection verts, deduped, with a separate copy per side so the two halves
	// can pull apart. Each copy is bound to ITS OWN endpoint's chunk (the + copy follows the +
	// endpoint's chunk, the − copy the − endpoint's), so the seam separates along the cut.
	const int64 K = OrigVerts;
	TMap<uint64, FIntPoint> EdgeInter; // key -> (PlusIdx, MinusIdx)
	auto GetInter = [&](int32 A, int32 B, int32& OutPlus, int32& OutMinus)
	{
		const int32 Lo = FMath::Min(A, B);
		const int32 Hi = FMath::Max(A, B);
		const uint64 Key = (uint64)Lo * (uint64)K + (uint64)Hi;
		if (FIntPoint* Found = EdgeInter.Find(Key))
		{
			OutPlus = Found->X; OutMinus = Found->Y; return;
		}
		const float SLo = Side[Lo], SHi = Side[Hi];
		float T = (FMath::Abs(SLo - SHi) > 1e-6f) ? (SLo / (SLo - SHi)) : 0.5f;
		T = FMath::Clamp(T, 0.0f, 1.0f);
		const FVector3f Rest = FMath::Lerp(MeshRestPositions[Lo], MeshRestPositions[Hi], T);
		const FVector2f UV   = FMath::Lerp(MeshUV0[Lo], MeshUV0[Hi], T);
		const int32 PlusEnd  = IsPlus(A) ? A : B; // geometric + endpoint
		const int32 MinusEnd = IsPlus(A) ? B : A;
		const int32 Pi = AppendVert(Rest, UV, VertComponent[PlusEnd]);  // + side copy → + endpoint's chunk
		const int32 Mi = AppendVert(Rest, UV, VertComponent[MinusEnd]); // − side copy → − endpoint's chunk
		EdgeInter.Add(Key, FIntPoint(Pi, Mi));
		OutPlus = Pi; OutMinus = Mi;
	};

	// Cut segments per side (one per straddling triangle) → assembled into cap loops below.
	TArray<TPair<int32, int32>> PlusSegs, MinusSegs;

	for (int32 tri = 0; tri + 2 < MeshTriangles.Num(); tri += 3)
	{
		const int32 I[3] = { (int32)MeshTriangles[tri], (int32)MeshTriangles[tri + 1], (int32)MeshTriangles[tri + 2] };
		const int32 NumPlus = (IsPlus(I[0]) ? 1 : 0) + (IsPlus(I[1]) ? 1 : 0) + (IsPlus(I[2]) ? 1 : 0);
		if (NumPlus == 3 || NumPlus == 0)
		{
			NewTris.Add(I[0]); NewTris.Add(I[1]); NewTris.Add(I[2]); // wholly one side — keep
			continue;
		}

		// Order-preserving split: walk the triangle's edges, sending each vertex to its side
		// and each crossing's intersection to BOTH sides (preserves winding).
		TArray<int32, TInlineAllocator<4>> Plus, Minus;
		int32 PInter[2] = { -1, -1 }, MInter[2] = { -1, -1 }, NInter = 0;
		for (int32 e = 0; e < 3; ++e)
		{
			const int32 A = I[e];
			const int32 B = I[(e + 1) % 3];
			const bool Ap = IsPlus(A), Bp = IsPlus(B);
			(Ap ? Plus : Minus).Add(A);
			if (Ap != Bp)
			{
				int32 Pi, Mi; GetInter(A, B, Pi, Mi);
				Plus.Add(Pi); Minus.Add(Mi);
				if (NInter < 2) { PInter[NInter] = Pi; MInter[NInter] = Mi; ++NInter; }
			}
		}

		auto FanEmit = [&](const TArray<int32, TInlineAllocator<4>>& Poly)
		{
			for (int32 k = 1; k + 1 < Poly.Num(); ++k)
			{
				NewTris.Add(Poly[0]); NewTris.Add(Poly[k]); NewTris.Add(Poly[k + 1]);
			}
		};
		FanEmit(Plus);
		FanEmit(Minus);

		if (NInter == 2)
		{
			if (PInter[0] != PInter[1]) PlusSegs.Add({ PInter[0], PInter[1] });
			if (MInter[0] != MInter[1]) MinusSegs.Add({ MInter[0], MInter[1] });
		}
	}

	// Cap the cross-section. The per-edge seam verts (degree 2) form closed loop(s); each loop
	// is a simple polygon on the cut plane, triangulated by EAR CLIPPING (handles concave shapes
	// and multiple separate loops without fanning across the gap). Cap verts reuse the seam verts,
	// which are already embedded on their side, so each cap moves coherently with its half.
	int32 NumCapTris = 0; // SB-M11 instrumentation
	TArray<uint32> CapTriVerts; // SB-M11 debug: record emitted cap tris for visualization
	auto EmitCapTri = [&](int32 A, int32 B, int32 C, const FVector3f& Target)
	{
		const FVector3f Fn = FVector3f::CrossProduct(NewRest[C] - NewRest[A], NewRest[B] - NewRest[A]);
		if (FVector3f::DotProduct(Fn, Target) >= 0.0f)
		{
			NewTris.Add(A); NewTris.Add(B); NewTris.Add(C);
		}
		else
		{
			NewTris.Add(A); NewTris.Add(C); NewTris.Add(B);
		}
		CapTriVerts.Add(A); CapTriVerts.Add(B); CapTriVerts.Add(C);
		++NumCapTris;
	};

	auto BuildCaps = [&](const TArray<TPair<int32, int32>>& Segs, const FVector3f& Target)
	{
		if (Segs.Num() == 0)
		{
			return;
		}
		// 2D basis on the cut plane (for the ear-clip area/containment tests).
		const FVector3f Up = (FMath::Abs(LocalN.X) < 0.9f) ? FVector3f(1, 0, 0) : FVector3f(0, 1, 0);
		const FVector3f U = FVector3f::CrossProduct(LocalN, Up).GetSafeNormal();
		const FVector3f V = FVector3f::CrossProduct(LocalN, U);
		auto To2D = [&](int32 Idx) { const FVector3f R = NewRest[Idx]; return FVector2f(FVector3f::DotProduct(R, U), FVector3f::DotProduct(R, V)); };

		TMap<int32, TArray<int32>> Adj;
		for (const TPair<int32, int32>& S : Segs)
		{
			Adj.FindOrAdd(S.Key).Add(S.Value);
			Adj.FindOrAdd(S.Value).Add(S.Key);
		}

		TSet<int32> Visited;
		for (const TPair<int32, int32>& S : Segs)
		{
			if (Visited.Contains(S.Key))
			{
				continue;
			}
			// Walk this loop following unvisited neighbours.
			TArray<int32> Loop;
			int32 Cur = S.Key, Prev = -1;
			for (int32 Guard = 0; Guard <= Adj.Num(); ++Guard)
			{
				Loop.Add(Cur);
				Visited.Add(Cur);
				const TArray<int32>* Nbrs = Adj.Find(Cur);
				if (!Nbrs) break;
				int32 Next = -1;
				for (int32 N : *Nbrs) { if (N != Prev && !Visited.Contains(N)) { Next = N; break; } }
				if (Next < 0) break;
				Prev = Cur; Cur = Next;
			}
			if (Loop.Num() < 3)
			{
				continue;
			}

			// Project to 2D and ear-clip (simple polygon). Make CCW first.
			const int32 N = Loop.Num();
			TArray<FVector2f> P2; P2.SetNumUninitialized(N);
			for (int32 k = 0; k < N; ++k) { P2[k] = To2D(Loop[k]); }
			float Area2 = 0.0f;
			for (int32 k = 0; k < N; ++k) { const FVector2f& a = P2[k]; const FVector2f& b = P2[(k + 1) % N]; Area2 += a.X * b.Y - b.X * a.Y; }

			TArray<int32> Rem; Rem.SetNumUninitialized(N); // working ring of loop-positions
			for (int32 k = 0; k < N; ++k) { Rem[k] = (Area2 >= 0.0f) ? k : (N - 1 - k); }

			auto Cross2 = [](const FVector2f& a, const FVector2f& b, const FVector2f& c)
			{ return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X); };
			auto InTri = [&](const FVector2f& p, const FVector2f& a, const FVector2f& b, const FVector2f& c)
			{
				const float d1 = Cross2(a, b, p), d2 = Cross2(b, c, p), d3 = Cross2(c, a, p);
				const bool HasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
				const bool HasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
				return !(HasNeg && HasPos);
			};

			int32 Guard = 0;
			while (Rem.Num() > 3 && Guard++ < N * N)
			{
				bool bClipped = false;
				const int32 M = Rem.Num();
				for (int32 i = 0; i < M; ++i)
				{
					const int32 Pa = Rem[(i + M - 1) % M], Pb = Rem[i], Pc = Rem[(i + 1) % M];
					if (Cross2(P2[Pa], P2[Pb], P2[Pc]) <= 0.0f)
					{
						continue; // reflex (CCW ring) → not an ear
					}
					bool bAnyInside = false;
					for (int32 j = 0; j < M; ++j)
					{
						const int32 Pj = Rem[j];
						if (Pj == Pa || Pj == Pb || Pj == Pc) continue;
						if (InTri(P2[Pj], P2[Pa], P2[Pb], P2[Pc])) { bAnyInside = true; break; }
					}
					if (bAnyInside) continue;
					EmitCapTri(Loop[Pa], Loop[Pb], Loop[Pc], Target);
					Rem.RemoveAt(i);
					bClipped = true;
					break;
				}
				if (!bClipped)
				{
					// Ear-clip stalled (near-degenerate / slightly non-simple 2D projection — e.g.
					// the convex-clamp inset perturbs the loop). Rather than leave a HOLE in the cap
					// (the remaining hollow patches), fan-fill the leftover ring from its first vertex.
					// May add a few thin/overlapping tris, but the material is two-sided so it just
					// closes the face. This guarantees a watertight cap.
					for (int32 k = 1; k + 1 < Rem.Num(); ++k)
					{
						EmitCapTri(Loop[Rem[0]], Loop[Rem[k]], Loop[Rem[k + 1]], Target);
					}
					Rem.Reset();
					break;
				}
			}
			if (Rem.Num() == 3)
			{
				EmitCapTri(Loop[Rem[0]], Loop[Rem[1]], Loop[Rem[2]], Target);
			}
		}
	};
	BuildCaps(PlusSegs, -LocalN); // + half's cut face looks toward the removed material (−N)
	BuildCaps(MinusSegs, LocalN);

	// Re-bind ORIGINAL verts whose tet now spans more than one chunk (torn by THIS cut or a prior
	// one → the vert would smear/sheet across the gap). Move them to an intact tet within their own
	// chunk (VertComponent). Verts whose tet is wholly in one chunk keep their binding (no pop).
	int32 NumRebinds = 0; // SB-M11 instrumentation
	for (int32 i = 0; i < OrigVerts; ++i)
	{
		if (bHaveComp && TetMultiComp(NewTet[i]) && VertComponent[i] != INDEX_NONE)
		{
			EmbedPointRest(NewRest[i], NewTet[i], NewW[i], &ParticleComponent, VertComponent[i]);
			++NumRebinds;
		}
	}

	// Commit the new geometry. Per-frame scratch (MeshLocalPositions/Normals/Tangents) is
	// resized in UpdateMeshFromSimulation; MeshVertWeights is init-only (cage stiffness) so
	// its staleness is harmless.
	MeshRestPositions = MoveTemp(NewRest);
	MeshUV0           = MoveTemp(NewUV);
	EmbedTet          = MoveTemp(NewTet);
	EmbedWeights      = MoveTemp(NewW);
	MeshTriangles     = MoveTemp(NewTris);
	NumMeshVerts      = MeshRestPositions.Num();
	DebugCapTriVerts.Append(CapTriVerts); // SB-M11 debug: caps from this cut (indices stay valid)

#if !UE_BUILD_SHIPPING
	// TEMP SB-M11 instrumentation: REST reconstruction error per vert (embedding vs rest position).
	// With convex same-side binding this is no longer exactly 0 for seam verts — it equals the
	// clamp inset, which should be SMALL (well under a cage cell); a large value would mean a vert
	// bound to a far tet (potential residual sheet). Originals far from the cut stay ~0.
	{
		float MaxRestErr = 0.0f;
		int32 WorstVert = INDEX_NONE;
		for (int32 v = 0; v < NumMeshVerts; ++v)
		{
			if (!Tets.IsValidIndex(EmbedTet[v])) { continue; }
			const FSoftBodyTet& T = Tets[EmbedTet[v]];
			const FVector4f& W = EmbedWeights[v];
			const FVector3f Rec =
				W.X * InitialLocalPositions[T.V0] + W.Y * InitialLocalPositions[T.V1] +
				W.Z * InitialLocalPositions[T.V2] + W.W * InitialLocalPositions[T.V3];
			const float Err = (Rec - MeshRestPositions[v]).Size();
			if (Err > MaxRestErr) { MaxRestErr = Err; WorstVert = v; }
		}
		// multiCompBinds = verts whose final tet spans MORE THAN ONE chunk (torn across a cut). This
		// is the spike/sheet culprit and is INVISIBLE to maxRestErr (a torn tet is intact at rest).
		// With component-routed binding it should be ~0; any residual = verts with no intact tet in
		// their chunk within the search radius. NumChunks = connected components (≈ pieces so far).
		int32 MultiCompBinds = -1;
		int32 NumChunks = -1;
		if (bHaveComp)
		{
			MultiCompBinds = 0;
			for (int32 v = 0; v < NumMeshVerts; ++v)
			{
				if (TetMultiComp(EmbedTet[v])) { ++MultiCompBinds; }
			}
			TSet<int32> Roots;
			for (int32 p = 0; p < NumParticles; ++p) { Roots.Add(ParticleComponent[p]); }
			NumChunks = Roots.Num();
		}
		UE_LOG(LogTemp, Warning,
			TEXT("[SB-M11] cut: verts %d->%d, +segs %d, -segs %d, capTris %d, rebinds %d, maxRestErr %.4f cm (vert %d), multiCompBinds %d, chunks %d"),
			OrigVerts, NumMeshVerts, PlusSegs.Num(), MinusSegs.Num(), NumCapTris, NumRebinds, MaxRestErr, WorstVert, MultiCompBinds, NumChunks);
	}
#endif
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

	// Cutting (SB-M10): keep CPU mirrors of the constraints + zeroed broken flags, and (box
	// mode) extract the render surface from the tets so a cut can open new faces.
	DistanceConstraintsCPU = Constraints;
	VolumeConstraintsCPU   = VolumeConstraints;
	DistanceBrokenCPU.Init(0u, Constraints.Num());
	VolumeBrokenCPU.Init(0u, VolumeConstraints.Num());

	if (!bMeshMode)
	{
		if (bCuttable)
		{
			BuildTetBoundarySurface(); // surviving-tet boundary (initially the full box)
		}
		else
		{
			BuildBoundarySurface();    // fixed 6 box faces
		}
	}

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

	// Seed positions: prefer the current DEFORMED verts when available (so recreating the proxy
	// after a cut doesn't flash the rest pose for a frame); otherwise the rest pose. Box mode and
	// first-time creation use rest.
	const TArray<FVector3f>& SeedPos =
		(bMeshMode && MeshLocalPositions.Num() == NumV) ? MeshLocalPositions : RestPos;

	TArray<FVector3f> SeedNormals, SeedTangents;
	ComputeNormalsTangents(SeedPos, RenderTris, SeedNormals, SeedTangents);

	TArray<FDynamicMeshVertex> Vertices;
	Vertices.SetNumUninitialized(NumV);
	for (int32 i = 0; i < NumV; ++i)
	{
		FDynamicMeshVertex& V = Vertices[i];
		V.Position = SeedPos[i];
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

	// Hold Shift to freeze the camera while dragging / cutting (global modifier).
	UpdateCameraFreeze();

	// Mouse pick/drag — updates GrabbedIndex / CurrentGrabTarget for the params below.
	UpdateMouseGrab();

	// Right-mouse cut stroke (SB-M10).
	if (bCuttable)
	{
		UpdateCut();
	}

	// Stress tearing (SB-M12): sever over-stretched links so violent pulls/drags rip the body apart.
	if (bTearable)
	{
		UpdateTear();
	}

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

	// Mouse grab (SB-M3 + cluster grab) — pull the whole grabbed region toward the cursor. Each
	// particle's target is the cursor target plus its offset within the region (rigid translation).
	Params.bGrabActive   = bIsGrabbing && GrabbedParticles.Num() > 0;
	Params.GrabStiffness = GrabStiffness;
	if (Params.bGrabActive)
	{
		const FVector3f Target(CurrentGrabTarget);
		Params.GrabPoints.Reset(GrabbedParticles.Num());
		for (int32 k = 0; k < GrabbedParticles.Num(); ++k)
		{
			FGPUGrab G;
			G.Index  = (uint32)GrabbedParticles[k];
			G.Target = Target + GrabOffsets[k];
			Params.GrabPoints.Add(G);
		}
	}

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

#if !UE_BUILD_SHIPPING
		// TEMP SB-M11 debug: draw the cut-face cap triangles (green) over the deformed mesh so we
		// can see whether the caps are generated and sit flush across the opening.
		if (bDrawDebugPoints && DebugCapTriVerts.Num() >= 3)
		{
			if (UWorld* DbgWorld = GetWorld())
			{
				const FTransform& X = GetComponentTransform();
				for (int32 t = 0; t + 2 < DebugCapTriVerts.Num(); t += 3)
				{
					const uint32 a = DebugCapTriVerts[t], b = DebugCapTriVerts[t + 1], c = DebugCapTriVerts[t + 2];
					if ((int32)a >= NumMeshVerts || (int32)b >= NumMeshVerts || (int32)c >= NumMeshVerts) { continue; }
					const FVector PA = X.TransformPosition(FVector(MeshLocalPositions[a]));
					const FVector PB = X.TransformPosition(FVector(MeshLocalPositions[b]));
					const FVector PC = X.TransformPosition(FVector(MeshLocalPositions[c]));
					DrawDebugLine(DbgWorld, PA, PB, FColor::Green, false, -1.0f, SDPG_World, 0.4f);
					DrawDebugLine(DbgWorld, PB, PC, FColor::Green, false, -1.0f, SDPG_World, 0.4f);
					DrawDebugLine(DbgWorld, PC, PA, FColor::Green, false, -1.0f, SDPG_World, 0.4f);
				}
			}
		}
#endif

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
		GrabbedParticles.Reset();
		GrabOffsets.Reset();
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
		int32 BestIndex = INDEX_NONE;   // cage particle to grab
		float BestDepth = 0.0f;

		if (bMeshMode && MeshLocalPositions.Num() == NumMeshVerts && NumMeshVerts > 0)
		{
			// Custom mesh: the grabbable surface is the EMBEDDED MESH, not the (padded, box-shaped)
			// cage boundary. Pick the deformed mesh vertex nearest the click ray, then grab the cage
			// particle that drives it (its tet's highest-weight particle). Without this, a click on
			// the visible mesh found no cage-boundary particle within reach, so dragging often failed.
			const FTransform X = GetComponentTransform();
			int32 BestVert = INDEX_NONE;
			for (int32 v = 0; v < NumMeshVerts; ++v)
			{
				const FVector P = X.TransformPosition(FVector(MeshLocalPositions[v]));
				const float Depth = FVector::DotProduct(P - RayOrigin, RayDir);
				if (Depth <= 0.0f) { continue; } // behind the camera
				const FVector Closest = RayOrigin + RayDir * Depth;
				const float DistSq = FVector::DistSquared(P, Closest);
				if (DistSq < BestDistSq) { BestDistSq = DistSq; BestVert = v; BestDepth = Depth; }
			}
			if (BestVert != INDEX_NONE && Tets.IsValidIndex(EmbedTet[BestVert]))
			{
				const FSoftBodyTet& T = Tets[EmbedTet[BestVert]];
				const FVector4f& W = EmbedWeights[BestVert];
				int32 Best = T.V0; float Bw = W.X;
				if (W.Y > Bw) { Bw = W.Y; Best = T.V1; }
				if (W.Z > Bw) { Bw = W.Z; Best = T.V2; }
				if (W.W > Bw) { Bw = W.W; Best = T.V3; }
				BestIndex = Best;
			}
		}
		else
		{
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
		}

		// Mesh picks land right on the visible surface (tiny dist); the box-cage pick uses the cage
		// spacing. Allow a generous radius for the mesh path so clicks near the surface still catch.
		const float PickRadius = EffectiveSpacing * GrabPickRadiusScale * (bMeshMode ? 4.0f : 1.0f);
		if (BestIndex != INDEX_NONE && BestDistSq <= PickRadius * PickRadius)
		{
			GrabbedIndex = BestIndex;
			GrabDepth = BestDepth;
			bIsGrabbing = true;

			// Gather the dragged REGION: every cage particle within GrabRadiusScale of the pick,
			// storing its world offset from the primary so the cluster translates rigidly with the
			// cursor. Pulling a patch (not one vertex) makes the whole body easy to drag.
			GrabbedParticles.Reset();
			GrabOffsets.Reset();
			const FVector Center(RenderResources->PositionCopy[GrabbedIndex]);
			const float GrabR = EffectiveSpacing * GrabRadiusScale;
			const float GrabRSq = GrabR * GrabR;
			for (int32 p = 0; p < NumParticles; ++p)
			{
				const FVector Pp(RenderResources->PositionCopy[p]);
				if (FVector::DistSquared(Pp, Center) <= GrabRSq)
				{
					GrabbedParticles.Add(p);
					GrabOffsets.Add(FVector3f(Pp - Center));
				}
			}
			if (GrabbedParticles.Num() == 0) // radius 0 / degenerate — at least grab the primary
			{
				GrabbedParticles.Add(GrabbedIndex);
				GrabOffsets.Add(FVector3f::ZeroVector);
			}
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

void USoftBodyComponent::UpdateCameraFreeze()
{
	// Hold SHIFT to freeze the camera, so you can DRAG or CUT (or pull) by moving the mouse without
	// the view rotating with it. This is a global modifier — independent of cut/drag — so it works
	// for every interaction. Balanced SetIgnoreLookInput (toggled only on change) keeps the engine's
	// ignore counter returning to 0; also reset in EndPlay in case we're destroyed while frozen.
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		bLookSuppressed = false; // controller gone — nothing to restore
		return;
	}

	const bool bWantFreeze = bHoldShiftToFreezeCamera
		&& (PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift));

	if (bWantFreeze && !bLookSuppressed)
	{
		PC->SetIgnoreLookInput(true);
		bLookSuppressed = true;
	}
	else if (!bWantFreeze && bLookSuppressed)
	{
		PC->SetIgnoreLookInput(false);
		bLookSuppressed = false;
	}
}

void USoftBodyComponent::UpdateCut()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !RenderResources.IsValid())
	{
		bCutStrokeActive = false;
		return;
	}

	const bool bDown = PC->IsInputKeyDown(EKeys::RightMouseButton);
	FVector RayO, RayD;
	const bool bRay = PC->DeprojectMousePositionToWorld(RayO, RayD);

	// (Camera freeze is handled globally by UpdateCameraFreeze on the Shift key, so you can hold
	// Shift and drag OR cut without the view moving — see TickComponent.)

	// Press: record the stroke start ray.
	if (bDown && !bCutStrokeActive)
	{
		if (bRay)
		{
			bCutStrokeActive = true;
			CutStrokeStartOrigin = RayO;
			CutStrokeStartDir = RayD.GetSafeNormal();
		}
		return;
	}
	// Held: draw a preview line from the stroke start to the current cursor point.
	if (bDown && bCutStrokeActive)
	{
		if (bRay)
		{
			DrawDebugLine(World, CutStrokeStartOrigin + CutStrokeStartDir * 200.0f,
				RayO + RayD.GetSafeNormal() * 200.0f, FColor::Red, false, -1.0f, SDPG_World, 1.0f);
		}
		return;
	}
	if (!bDown && !bCutStrokeActive)
	{
		return; // idle
	}

	// Release: form the cut plane (through the camera, containing the start + end rays) and slice.
	bCutStrokeActive = false;
	if (!bRay)
	{
		return;
	}
	const FVector EndDir = RayD.GetSafeNormal();
	FVector Normal = FVector::CrossProduct(CutStrokeStartDir, EndDir);
	if (Normal.SizeSquared() < 1e-6f)
	{
		return; // no real swipe (start ~ end) — ignore
	}
	Normal = Normal.GetSafeNormal();
	const FVector PlanePoint = CutStrokeStartOrigin;

	// Sever every constraint whose endpoints straddle the plane, using current world positions.
	bool bAnyCut = false;
	{
		FScopeLock Lock(&RenderResources->PositionCopyCS);
		if (!RenderResources->bHasPositionData || RenderResources->PositionCopy.Num() != NumParticles)
		{
			return; // readback not ready
		}
		const TArray<FVector3f>& P = RenderResources->PositionCopy;

		auto Side = [&](uint32 Idx) -> float
		{
			return FVector::DotProduct(FVector(P[Idx]) - PlanePoint, Normal);
		};

		for (int32 i = 0; i < DistanceConstraintsCPU.Num(); ++i)
		{
			if (DistanceBrokenCPU[i] != 0)
			{
				continue;
			}
			const FGPUConstraint& C = DistanceConstraintsCPU[i];
			if (Side(C.IndexA) * Side(C.IndexB) < 0.0f) // opposite sides → crosses the plane
			{
				DistanceBrokenCPU[i] = 1u;
				bAnyCut = true;
			}
		}
	}

	if (!bAnyCut)
	{
		return;
	}

	// Drop the volume constraint of any tet touching the new cut surface so the PBD volume solve
	// can't explode the under-constrained boundary tets (shared with tearing — see BreakVolumeShell).
	BreakVolumeShell();

	// Push the new broken flags to the GPU solver.
	{
		TSharedPtr<FSoftBodyRenderResources> Resources = RenderResources;
		ENQUEUE_RENDER_COMMAND(SoftBodyApplyCut)(
			[Resources, Dist = DistanceBrokenCPU, Vol = VolumeBrokenCPU]
			(FRHICommandListImmediate& RHICmdList)
			{
				SoftBodyCompute::UpdateBrokenState_RenderThread(RHICmdList, Resources, Dist, Vol);
			});
	}

	if (!bMeshMode)
	{
		// Box/lattice: re-extract the boundary surface from surviving tets so the cut opens up,
		// and hand the new index buffer to the proxy (SB-M10).
		BuildTetBoundarySurface();
		if (FSoftBodyMeshSceneProxy* Proxy = static_cast<FSoftBodyMeshSceneProxy*>(SceneProxy))
		{
			ENQUEUE_RENDER_COMMAND(SoftBodyCutReindex)(
				[Proxy, NewTris = Triangles](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateIndices_RenderThread(NewTris);
				});
		}
	}
	else if (MeshLocalPositions.Num() == NumMeshVerts && NumMeshVerts > 0)
	{
		// Embedded custom mesh (SB-M11): split the render mesh along the cut plane (in local
		// space) and cap it, then recreate the proxy with the new geometry (vertex count grows).

		// Connected components of cage particles over the SURVIVING distance constraints (= the
		// physical chunks after ALL cuts so far). Each mesh vert is then re-bound to a tet inside
		// its own chunk, so a tet torn across an earlier cut can never blend two chunks (the 2nd-cut
		// spanning-sheet bug).
		TArray<int32> ParticleComponent;
		ComputeParticleComponents(ParticleComponent);

		const FTransform WorldToLocal = GetComponentTransform().Inverse();
		const FVector3f LocalP(WorldToLocal.TransformPosition(PlanePoint));
		const FVector3f LocalN = FVector3f(WorldToLocal.TransformVector(Normal)).GetSafeNormal();
		if (!LocalN.IsNearlyZero())
		{
			ClipMeshAlongPlane(LocalP, LocalN, ParticleComponent);
			MarkRenderStateDirty(); // rebuild the proxy from the new Mesh* arrays via CreateSceneProxy
		}
	}
}

void USoftBodyComponent::ComputeParticleComponents(TArray<int32>& OutComponent) const
{
	// Union-find (path halving) over the SURVIVING distance constraints → one id per connected
	// chunk. Shared by the cut (SB-M11) and tear (SB-M12) render paths to bind verts to their chunk.
	OutComponent.SetNumUninitialized(NumParticles);
	for (int32 p = 0; p < NumParticles; ++p) { OutComponent[p] = p; }
	auto Find = [&](int32 X) -> int32
	{
		while (OutComponent[X] != X)
		{
			OutComponent[X] = OutComponent[OutComponent[X]];
			X = OutComponent[X];
		}
		return X;
	};
	for (int32 i = 0; i < DistanceConstraintsCPU.Num(); ++i)
	{
		if (DistanceBrokenCPU[i] != 0) { continue; }
		const FGPUConstraint& C = DistanceConstraintsCPU[i];
		const int32 RA = Find((int32)C.IndexA), RB = Find((int32)C.IndexB);
		if (RA != RB) { OutComponent[RA] = RB; }
	}
	for (int32 p = 0; p < NumParticles; ++p) { OutComponent[p] = Find(p); }
}

bool USoftBodyComponent::BreakVolumeShell()
{
	// A particle that has lost a distance constraint (across ALL cuts/tears) sits on a cut/tear
	// surface. A tet touching one is under-constrained: it can invert once unsupported, and the PBD
	// volume solve then OVER-corrects the inverted tet and launches its particles outward (the
	// radial spikes that vanished at VolumeStiffness=0). So we drop the volume constraint of every
	// tet touching such a particle — a one-tet shell around each cut/tear gives up volume
	// preservation (still held by its distance constraints) while the interior keeps its jelly.
	if (DistanceBrokenCPU.Num() != DistanceConstraintsCPU.Num() || NumParticles <= 0)
	{
		return false;
	}
	TArray<uint8> CutParticle;
	CutParticle.Init(0, NumParticles);
	for (int32 i = 0; i < DistanceConstraintsCPU.Num(); ++i)
	{
		if (DistanceBrokenCPU[i] != 0)
		{
			const FGPUConstraint& C = DistanceConstraintsCPU[i];
			CutParticle[C.IndexA] = 1;
			CutParticle[C.IndexB] = 1;
		}
	}

	bool bAny = false;
	for (int32 t = 0; t < VolumeConstraintsCPU.Num(); ++t)
	{
		if (VolumeBrokenCPU[t] != 0) { continue; }
		const FGPUVolumeConstraint& V = VolumeConstraintsCPU[t];
		if (CutParticle[V.I0] || CutParticle[V.I1] || CutParticle[V.I2] || CutParticle[V.I3])
		{
			VolumeBrokenCPU[t] = 1u;
			bAny = true;
		}
	}
	return bAny;
}

void USoftBodyComponent::UpdateTear()
{
	if (!RenderResources.IsValid() || DistanceConstraintsCPU.Num() == 0)
	{
		return;
	}

	// Throttle the O(constraints) strain scan to bound its CPU cost.
	if ((TearFrameCounter++ % FMath::Max(1, TearCheckInterval)) != 0)
	{
		return;
	}

	const float Thresh = FMath::Max(1.1f, TearStrainThreshold);
	const float ThreshSq = FMath::Square(Thresh);
	// The dragged region resists tearing more (the grab pins it hard, so its boundary links are the
	// most stretched and would rip off instantly otherwise). It's NOT immune: a violent enough pull
	// still exceeds this relaxed threshold and tears the dragged patch away. 1.0 = no extra resistance.
	const float GrabThreshSq = FMath::Square(Thresh * FMath::Max(1.0f, GrabTearResistance));

	// Identify the grabbed region + its 1-ring (these use the relaxed threshold below).
	TArray<uint8> Protected;
	const bool bProtect = bIsGrabbing && GrabbedParticles.Num() > 0;
	if (bProtect)
	{
		// Protect the whole grabbed region and its 1-ring (read grabbed flags from a separate array
		// so neighbours don't cascade past one ring).
		TArray<uint8> Grabbed; Grabbed.Init(0, NumParticles);
		for (int32 g : GrabbedParticles) { if (g >= 0 && g < NumParticles) { Grabbed[g] = 1; } }
		Protected = Grabbed;
		for (const FGPUConstraint& C : DistanceConstraintsCPU)
		{
			if (Grabbed[C.IndexA]) { Protected[C.IndexB] = 1; }
			if (Grabbed[C.IndexB]) { Protected[C.IndexA] = 1; }
		}
	}

	// Sever every link stretched past RestLength * threshold, using the latest readback positions.
	bool bAnyTear = false;
	{
		FScopeLock Lock(&RenderResources->PositionCopyCS);
		if (!RenderResources->bHasPositionData || RenderResources->PositionCopy.Num() != NumParticles)
		{
			return; // readback not ready
		}
		const TArray<FVector3f>& P = RenderResources->PositionCopy;

		for (int32 i = 0; i < DistanceConstraintsCPU.Num(); ++i)
		{
			if (DistanceBrokenCPU[i] != 0) { continue; }
			const FGPUConstraint& C = DistanceConstraintsCPU[i];
			if (C.RestLength <= KINDA_SMALL_NUMBER) { continue; }
			// Links in the grabbed region use the relaxed (harder) threshold; the rest use the normal one.
			const bool bInGrab = bProtect && (Protected[C.IndexA] || Protected[C.IndexB]);
			const float UseThreshSq = bInGrab ? GrabThreshSq : ThreshSq;
			const float LenSq = FVector3f::DistSquared(P[C.IndexA], P[C.IndexB]);
			if (LenSq > UseThreshSq * C.RestLength * C.RestLength) // stretched past the tear threshold
			{
				DistanceBrokenCPU[i] = 1u;
				bAnyTear = true;
			}
		}
	}

	if (!bAnyTear)
	{
		return;
	}

	// Same anti-blow-up guard as cutting, then push the new broken flags to the GPU solver.
	BreakVolumeShell();
	{
		TSharedPtr<FSoftBodyRenderResources> Resources = RenderResources;
		ENQUEUE_RENDER_COMMAND(SoftBodyApplyTear)(
			[Resources, Dist = DistanceBrokenCPU, Vol = VolumeBrokenCPU]
			(FRHICommandListImmediate& RHICmdList)
			{
				SoftBodyCompute::UpdateBrokenState_RenderThread(RHICmdList, Resources, Dist, Vol);
			});
	}

	// Update the render surface for the new tear.
	if (!bMeshMode)
	{
		// Box/lattice: re-extract the boundary from surviving tets so the tear opens up (SB-M10 path).
		BuildTetBoundarySurface();
		if (FSoftBodyMeshSceneProxy* Proxy = static_cast<FSoftBodyMeshSceneProxy*>(SceneProxy))
		{
			ENQUEUE_RENDER_COMMAND(SoftBodyTearReindex)(
				[Proxy, NewTris = Triangles](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateIndices_RenderThread(NewTris);
				});
		}
	}
	else if (MeshLocalPositions.Num() == NumMeshVerts && NumMeshVerts > 0)
	{
		// Embedded mesh: re-route verts to their chunk and rip the surface open along the tear.
		TArray<int32> ParticleComponent;
		ComputeParticleComponents(ParticleComponent);
		TearMeshToComponents(ParticleComponent);
		MarkRenderStateDirty();
	}
}

void USoftBodyComponent::TearMeshToComponents(const TArray<int32>& ParticleComponent)
{
	// Rip the embedded surface open along an arbitrary tear (no plane, no caps): bind each vert to a
	// tet in its own chunk, and drop any triangle whose 3 verts ended up in different chunks so the
	// mesh visibly separates. Reuses the SB-M11 component embedding (convex, in-chunk, point-bind
	// fallback) so torn pieces follow their chunk without spikes.
	if (ParticleComponent.Num() != NumParticles || NumMeshVerts == 0 || MeshTriangles.Num() < 3)
	{
		return;
	}

	// Chunk per vert = the component carrying the most total barycentric weight in its tet (so a
	// vert near a tear follows the side it mostly belongs to). Plane-free variant of SB-M11 routing.
	auto VertChunk = [&](int32 v) -> int32
	{
		if (!Tets.IsValidIndex(EmbedTet[v])) { return INDEX_NONE; }
		const FSoftBodyTet& T = Tets[EmbedTet[v]];
		const FVector4f& W = EmbedWeights[v];
		const int32 Idx[4] = { (int32)T.V0, (int32)T.V1, (int32)T.V2, (int32)T.V3 };
		const float Wt[4]  = { W.X, W.Y, W.Z, W.W };
		int32 BestComp = ParticleComponent[Idx[0]]; float BestSum = -1.0f;
		for (int32 k = 0; k < 4; ++k)
		{
			const int32 Comp = ParticleComponent[Idx[k]];
			float Sum = 0.0f;
			for (int32 j = 0; j < 4; ++j) { if (ParticleComponent[Idx[j]] == Comp) { Sum += Wt[j]; } }
			if (Sum > BestSum) { BestSum = Sum; BestComp = Comp; }
		}
		return BestComp;
	};

	TArray<int32> VertComponent;
	VertComponent.SetNumUninitialized(NumMeshVerts);
	for (int32 v = 0; v < NumMeshVerts; ++v) { VertComponent[v] = VertChunk(v); }

	auto TetMultiComp = [&](int32 GeomTet) -> bool
	{
		if (!Tets.IsValidIndex(GeomTet)) { return false; }
		const FSoftBodyTet& T = Tets[GeomTet];
		const int32 C = ParticleComponent[T.V0];
		return ParticleComponent[T.V1] != C || ParticleComponent[T.V2] != C || ParticleComponent[T.V3] != C;
	};

	// Rebind verts whose tet straddles a tear to an intact tet in their own chunk.
	for (int32 v = 0; v < NumMeshVerts; ++v)
	{
		if (VertComponent[v] != INDEX_NONE && TetMultiComp(EmbedTet[v]))
		{
			EmbedPointRest(MeshRestPositions[v], EmbedTet[v], EmbedWeights[v], &ParticleComponent, VertComponent[v]);
		}
	}

	// Keep only triangles whose 3 verts are in the same chunk; drop the rest → the surface rips open.
	TArray<uint32> NewTris;
	NewTris.Reserve(MeshTriangles.Num());
	for (int32 tri = 0; tri + 2 < MeshTriangles.Num(); tri += 3)
	{
		const int32 A = (int32)MeshTriangles[tri], B = (int32)MeshTriangles[tri + 1], Cc = (int32)MeshTriangles[tri + 2];
		const int32 Ca = VertComponent[A], Cb = VertComponent[B], Cd = VertComponent[Cc];
		if (Ca != INDEX_NONE && Ca == Cb && Cb == Cd)
		{
			NewTris.Add(A); NewTris.Add(B); NewTris.Add(Cc);
		}
	}
	MeshTriangles = MoveTemp(NewTris);
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
	// Restore camera look input if we were freezing it (hold-Shift camera freeze).
	if (bLookSuppressed)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				PC->SetIgnoreLookInput(false);
			}
		}
		bLookSuppressed = false;
	}

	// Stop participating in inter-body collision before our resources are released (SB-M9).
	if (UWorld* World = GetWorld())
	{
		if (USoftBodyWorldSubsystem* Sub = World->GetSubsystem<USoftBodyWorldSubsystem>())
		{
			Sub->UnregisterComponent(this);
		}
	}

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
