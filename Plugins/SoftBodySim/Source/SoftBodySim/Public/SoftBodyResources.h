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
	float  StiffScale = 1.0f;  // PBD path: per-constraint relative stiffness
	float  Softness = 0.0f;    // XPBD path: [0..1] paint weight (0 firm, 1 soft → compliance)
};

/**
 * One per-tetrahedron volume constraint, as the GPU solver sees it (SB-M2).
 * I0..I3 are the 4 particle indices; the solver preserves the tet's signed rest
 * volume. StiffScale is a per-constraint relative stiffness multiplied by the global
 * Stiffness * VolumeStiffness uniforms. Must match the HLSL `FVolumeConstraint` struct
 * in SBSolveVolume.usf (24 bytes, tight scalar layout — structured buffers pack tightly,
 * the 16-byte alignment trap is for constant buffers only).
 */
struct FGPUVolumeConstraint
{
	uint32 I0 = 0;
	uint32 I1 = 0;
	uint32 I2 = 0;
	uint32 I3 = 0;
	float  RestVolume = 0.0f;
	float  StiffScale = 1.0f;
};

/** Contiguous [Start, Count) span of one color within a sorted constraint buffer. */
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

	// Lattice dimensions — needed by self-collision to decompose a flat index into (x,y,z)
	// and skip a particle's lattice 1-ring neighbours (SB-M4).
	int32   ResX = 0;
	int32   ResY = 0;
	int32   ResZ = 0;

	float   DeltaTime = 1.0f / 60.0f;
	FVector3f Gravity = FVector3f(0.0f, 0.0f, -980.0f);
	float   Damping = 0.1f;

	// XPBD/PBD solver controls.
	int32   Substeps = 2;          // split DeltaTime for stability (biggest quality lever)
	int32   SolverIterations = 8;  // constraint relaxation passes per substep
	float   Stiffness = 1.0f;      // [0,1] global distance-correction scale (PBD path)
	float   VolumeStiffness = 1.0f;// [0,1] per-tet volume-correction scale (SB-M2)

	// Distance solver method (SB-M7). XPBD gives iteration-count-independent stiffness via
	// per-constraint compliance, so weight-painted softness is robust at any iters/substeps.
	bool    bUseXPBD = true;
	float   XpbdGlobalCompliance = 0.0f;   // baseline compliance everywhere (0 = rigid; higher = softer)
	float   XpbdSoftCompliance = 0.001f;   // extra compliance added at full paint weight (white)

	// Mouse drag (SB-M3) — pull one grabbed particle toward a world-space cursor target.
	bool      bGrabActive = false;
	int32     GrabIndex = -1;
	FVector3f GrabTarget = FVector3f::ZeroVector;
	float     GrabStiffness = 0.8f; // [0,1] firmness of the attachment

	// Collision — world-space sphere/capsule colliders rebuilt each frame (SB-M4).
	TArray<FGPUCollider> Colliders;
	float   Friction = 0.3f;

	// Distance-field collision (SB-M8) — collide against ANY scene mesh via the Global
	// Distance Field, in addition to the analytic colliders + ground.
	bool    bUseDistanceFieldCollision = false;
	float   DFThickness = 2.0f; // contact shell thickness (cm)

	// Built-in ground plane — infinite floor at world Z = GroundZ (normal +Z).
	bool    bGroundPlane = true;
	float   GroundZ = 0.0f;

	// Self-collision (SB-M4) — body vs itself via a GPU spatial hash grid.
	bool    bSelfCollision = false;
	float   SelfThickness = 20.0f;       // min separation between non-adjacent particles (cm)
	float   SelfStiffness = 1.0f;        // [0,1] repulsion strength
	int32   SelfCollisionIterations = 2; // repulsion passes per substep (deeper compression)
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

	// Per-tet volume constraints + their own graph coloring (SB-M2). Second constraint
	// set: same color-sorted, race-free per-color dispatch scheme as the distance edges.
	TRefCountPtr<FRDGPooledBuffer> VolumeConstraintsBuffer;
	int32 NumVolumeConstraints = 0;
	TArray<FSoftBodyColorRange> VolumeColorRanges;

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
	 *  Distance and volume constraints must already be sorted by color; their ColorRanges
	 *  index into them. */
	void InitResources_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FSoftBodyRenderResources>& Resources,
		const TArray<FVector3f>& InitialPositions,
		const TArray<FVector3f>& InitialVelocities,
		const TArray<float>& InitialInvMasses,
		const TArray<FGPUConstraint>& Constraints,
		const TArray<FSoftBodyColorRange>& ColorRanges,
		const TArray<FGPUVolumeConstraint>& VolumeConstraints,
		const TArray<FSoftBodyColorRange>& VolumeColorRanges);

	/** Per-frame: run the substep pipeline and kick a position readback. */
	void Dispatch_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FSoftBodyRenderResources>& Resources,
		const FSoftBodyParams& Params);
}
