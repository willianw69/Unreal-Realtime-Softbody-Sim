// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h" // TRefCountPtr<FRDGPooledBuffer>

class FRHIGPUBufferReadback;
class FRHICommandListImmediate;

/**
 * One collider, as the GPU sees it. Both shapes are represented as a CAPSULE
 * (a line segment A-B with a radius); a SPHERE is just the degenerate case A == B.
 * Must match the HLSL `FCollider` struct in SBCollision.usf (32 bytes).
 * (Plumbed for the ground-plane drop test now; sphere/capsule colliders arrive in SB-M4.)
 */
struct FGPUCollider
{
	FVector3f A = FVector3f::ZeroVector;
	float     Radius = 0.0f;
	FVector3f B = FVector3f::ZeroVector;
	float     Friction = 0.0f; // [0..1] tangential velocity damping on contact
};

/**
 * One explicit distance constraint, as the GPU Gauss-Seidel solver sees it.
 * A and B are particle indices; the solver projects them back to RestLength apart.
 * StiffScale is a per-constraint relative stiffness multiplied by the global Stiffness
 * uniform. Must match the HLSL `FConstraint` struct in SBSolveDistance.usf (16 bytes,
 * tight layout).
 */
struct FGPUConstraint
{
	uint32 IndexA = 0;
	uint32 IndexB = 0;
	float  RestLength = 0.0f;
	float  StiffScale = 1.0f;
};

/** Contiguous [Start, Count) span of one color within the sorted constraint buffer. */
struct FSoftBodyColorRange
{
	int32 Start = 0;
	int32 Count = 0;
};

/**
 * One tetrahedron of the lattice (4 particle indices). Built at runtime by the
 * Kuhn 6-tet split of each cube cell. SB-M1 only uses these to derive deduped
 * distance constraints (edges); SB-M2 adds per-tet volume constraints from the
 * same list.
 */
struct FSoftBodyTet
{
	uint32 V0 = 0;
	uint32 V1 = 0;
	uint32 V2 = 0;
	uint32 V3 = 0;
};

struct FSoftBodyParams
{
	int32   NumParticles = 0;

	float   DeltaTime = 1.0f / 60.0f;
	FVector3f Gravity = FVector3f(0.0f, 0.0f, -980.0f);
	float   Damping = 0.1f;

	// XPBD/PBD solver controls.
	int32   Substeps = 2;          // split DeltaTime for stability (biggest quality lever)
	int32   SolverIterations = 8;  // distance-constraint relaxation passes per substep
	float   Stiffness = 1.0f;      // [0,1] correction scale

	// Collision — world-space colliders rebuilt each frame (SB-M4); empty for SB-M1.
	TArray<FGPUCollider> Colliders;
	float   Friction = 0.3f;

	// Built-in ground plane — infinite floor at world Z = GroundZ (normal +Z).
	bool    bGroundPlane = true;
	float   GroundZ = 0.0f;
};

/**
 * Render-thread-owned GPU state for one soft body instance.
 *
 * The simulation is stateful: this frame's positions/velocities are next frame's
 * input. RDG resources are transient (recreated every FRDGBuilder), so we keep
 * persistent pooled buffers here and RegisterExternalBuffer() them into the graph
 * each frame.
 *
 * Lifetime: created on the game thread, but every member is only ever touched on
 * the render thread. Held by TSharedPtr so render command lambdas can safely keep
 * it alive even if the owning component is destroyed mid-flight.
 */
struct FSoftBodyRenderResources
{
	TRefCountPtr<FRDGPooledBuffer> PositionsBuffer;
	TRefCountPtr<FRDGPooledBuffer> VelocitiesBuffer;
	TRefCountPtr<FRDGPooledBuffer> InvMassBuffer;

	// Explicit distance constraints + their graph coloring. Built once at init; static
	// for the body's lifetime. The buffer is sorted by color so each ColorRange is a
	// contiguous, race-free batch of constraints.
	TRefCountPtr<FRDGPooledBuffer> ConstraintsBuffer;
	int32 NumConstraints = 0;
	TArray<FSoftBodyColorRange> ColorRanges;

	int32 NumParticles = 0;
	bool  bInitialized = false;

	// Latest CPU-side copy of positions (non-stalling readback), guarded for the game
	// thread to build render verts + (later) mouse picking.
	TUniquePtr<FRHIGPUBufferReadback> PositionReadback;

	FCriticalSection     PositionCopyCS;
	TArray<FVector3f>    PositionCopy;
	bool                 bHasPositionData = false;

	// Declared out-of-line (defined in SoftBodyCompute.cpp). TUniquePtr<FRHIGPUBufferReadback>
	// needs the COMPLETE type to generate its destructor; only that .cpp includes it
	// (avoids C4150 against the forward declaration).
	FSoftBodyRenderResources();
	~FSoftBodyRenderResources();
};

/**
 * Compute-side entry points. All run on the render thread.
 */
namespace SoftBodyCompute
{
	/** One-time: create pooled buffers and upload the initial particle lattice + constraints.
	 *  Constraints must already be sorted by color; ColorRanges indexes into them. */
	void InitResources_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FSoftBodyRenderResources>& Resources,
		const TArray<FVector3f>& InitialPositions,
		const TArray<FVector3f>& InitialVelocities,
		const TArray<float>& InitialInvMasses,
		const TArray<FGPUConstraint>& Constraints,
		const TArray<FSoftBodyColorRange>& ColorRanges);

	/** Per-frame: run the substep pipeline and kick a position readback. */
	void Dispatch_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FSoftBodyRenderResources>& Resources,
		const FSoftBodyParams& Params);
}
