// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyComponent.h"
#include "SoftBodyResources.h"
#include "SoftBodyMeshProxy.h"

#include "DrawDebugHelpers.h"
#include "DynamicMeshBuilder.h"            // FDynamicMeshVertex
#include "Engine/Engine.h"                 // GEngine on-screen debug
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"

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

	InitializeSimulation();

	// We now have geometry: rebuild bounds and recreate the (previously empty) proxy.
	UpdateBounds();
	MarkRenderStateDirty();
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
			Edges.Add(FGPUConstraint{ Lo, Hi, Rest, 1.0f });
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

void USoftBodyComponent::InitializeSimulation()
{
	ResX = FMath::Clamp(ResX, 2, 32);
	ResY = FMath::Clamp(ResY, 2, 32);
	ResZ = FMath::Clamp(ResZ, 2, 32);
	NumParticles = ResX * ResY * ResZ;

	TArray<FVector3f> Positions;   // world space (sim runs in world space)
	TArray<FVector3f> Velocities;
	TArray<float>     InvMasses;
	Positions.Reserve(NumParticles);
	Velocities.Reserve(NumParticles);
	InvMasses.Reserve(NumParticles);
	InitialLocalPositions.Reset();
	InitialLocalPositions.Reserve(NumParticles);

	const FTransform& Xform = GetComponentTransform();

	// Lattice centered in X/Y about the component origin, base at local Z = 0.
	const float HalfX = 0.5f * (ResX - 1) * Spacing;
	const float HalfY = 0.5f * (ResY - 1) * Spacing;

	for (int32 Z = 0; Z < ResZ; ++Z)
	{
		for (int32 Y = 0; Y < ResY; ++Y)
		{
			for (int32 X = 0; X < ResX; ++X)
			{
				const FVector Local(X * Spacing - HalfX, Y * Spacing - HalfY, Z * Spacing);
				InitialLocalPositions.Add(FVector3f(Local));

				Positions.Add(FVector3f(Xform.TransformPosition(Local)));
				Velocities.Add(FVector3f::ZeroVector);

				const bool bPinned =
					(Anchor == ESoftBodyAnchor::TopFace && Z == ResZ - 1) ||
					(Anchor == ESoftBodyAnchor::BottomFace && Z == 0);
				InvMasses.Add(bPinned ? 0.0f : 1.0f);
			}
		}
	}

	BuildTets();
	BuildBoundarySurface();

	// Generous local bounds so the body isn't frustum-culled as it sags/jiggles.
	FBox Box(ForceInit);
	for (const FVector3f& P : InitialLocalPositions)
	{
		Box += FVector(P);
	}
	const float Margin = (ResX + ResY + ResZ) * Spacing;
	Box = Box.ExpandBy(Margin);
	LocalBounds = FBoxSphereBounds(Box);

	// Explicit distance constraints + graph coloring for the Gauss-Seidel solver.
	TArray<FGPUConstraint>      Constraints;
	TArray<FSoftBodyColorRange> ColorRanges;
	BuildConstraints(Constraints, ColorRanges);
	NumConstraintsBuilt = Constraints.Num();
	NumColorsBuilt      = ColorRanges.Num();

	RenderResources = MakeShared<FSoftBodyRenderResources>();

	TSharedPtr<FSoftBodyRenderResources> Resources = RenderResources;
	ENQUEUE_RENDER_COMMAND(SoftBodySimInit)(
		[Resources, Positions = MoveTemp(Positions), Velocities = MoveTemp(Velocities), InvMasses = MoveTemp(InvMasses),
		 Constraints = MoveTemp(Constraints), ColorRanges = MoveTemp(ColorRanges)]
		(FRHICommandListImmediate& RHICmdList)
		{
			SoftBodyCompute::InitResources_RenderThread(RHICmdList, Resources, Positions, Velocities, InvMasses, Constraints, ColorRanges);
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
	if (NumParticles <= 0 || Triangles.Num() == 0 || InitialLocalPositions.Num() != NumParticles)
	{
		return nullptr; // not initialised yet (e.g. editor, before BeginPlay)
	}

	// Seed the proxy with the initial (rest) mesh; normals computed from the surface.
	TArray<FVector3f> SeedNormals, SeedTangents;
	ComputeSurfaceNormalsTangents(InitialLocalPositions, SeedNormals, SeedTangents);

	TArray<FDynamicMeshVertex> Vertices;
	Vertices.SetNumUninitialized(NumParticles);
	for (int32 i = 0; i < NumParticles; ++i)
	{
		FDynamicMeshVertex& V = Vertices[i];
		V.Position = InitialLocalPositions[i];
		V.TextureCoordinate[0] = UV0[i];
		V.TangentX = SeedTangents[i];
		V.TangentZ = SeedNormals[i];
		V.Color = FColor::White;
	}

	return new FSoftBodyMeshSceneProxy(this, Vertices, Triangles, SoftBodyMaterial);
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

	FSoftBodyParams Params;
	Params.NumParticles     = NumParticles;
	Params.DeltaTime        = FixedTimeStep;
	Params.Gravity          = FVector3f(Gravity);
	Params.Damping          = Damping;
	Params.Substeps         = Substeps;
	Params.SolverIterations = SolverIterations;
	Params.Stiffness        = Stiffness;
	Params.Friction         = Friction;
	Params.bGroundPlane     = bGroundPlane;
	Params.GroundZ          = GroundHeight;

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

	if (bShowStats && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("SoftBody  lattice=%dx%dx%d  particles=%d  tets=%d"),
				ResX, ResY, ResZ, NumParticles, Tets.Num()));

		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 2, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("  constraints=%d  colors=%d  substeps=%d  iters=%d  solve dispatches/substep=%d"),
				NumConstraintsBuilt, NumColorsBuilt, Substeps, SolverIterations, SolverIterations * NumColorsBuilt));
	}
}

void USoftBodyComponent::ComputeSurfaceNormalsTangents(
	const TArray<FVector3f>& InPositions,
	TArray<FVector3f>& OutNormals,
	TArray<FVector3f>& OutTangents) const
{
	OutNormals.Init(FVector3f::ZeroVector, NumParticles);
	OutTangents.Init(FVector3f::ZeroVector, NumParticles);

	// Area-weighted face normals accumulated to each boundary vertex. Cross(E2,E1) (not
	// E1,E2) so the smooth normal agrees with UE's left-handed front-face winding, which
	// is what makes a TWO-SIDED material shade correctly (cloth M9 lesson).
	for (int32 t = 0; t < Triangles.Num(); t += 3)
	{
		const uint32 I0 = Triangles[t];
		const uint32 I1 = Triangles[t + 1];
		const uint32 I2 = Triangles[t + 2];

		const FVector3f E1 = InPositions[I1] - InPositions[I0];
		const FVector3f E2 = InPositions[I2] - InPositions[I0];
		const FVector3f FaceN = FVector3f::CrossProduct(E2, E1);

		OutNormals[I0] += FaceN;
		OutNormals[I1] += FaceN;
		OutNormals[I2] += FaceN;
	}

	for (int32 i = 0; i < NumParticles; ++i)
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

	// Copy the latest world-space positions out of the readback buffer, to local space.
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
		}
	}

	ComputeSurfaceNormalsTangents(LocalPositions, LocalNormals, LocalTangents);

	// Hand the new vertex data to the proxy on the render thread.
	ENQUEUE_RENDER_COMMAND(SoftBodyMeshUpdate)(
		[Proxy, Positions = LocalPositions, Normals = LocalNormals, Tangents = LocalTangents]
		(FRHICommandListImmediate& RHICmdList)
		{
			Proxy->UpdateVertices_RenderThread(RHICmdList, Positions, Normals, Tangents);
		});
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

	for (int32 i = 0; i < RenderResources->PositionCopy.Num(); ++i)
	{
		DrawDebugPoint(World, FVector(RenderResources->PositionCopy[i]), DebugPointSize, FColor::Cyan, false, -1.0f, SDPG_World);
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
