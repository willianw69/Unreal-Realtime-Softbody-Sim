// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyResources.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"

// Must match [numthreads(...)] in every soft body .usf. Injected as a #define.
static constexpr uint32 kThreadGroupSize = 64;

//////////////////////////////////////////////////////////////////////////
// Shader bindings: Predict -> SolveDistance (colored GS) -> Collision -> Finalize
//////////////////////////////////////////////////////////////////////////

class FSBPredictCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBPredictCS);
	SHADER_USE_PARAMETER_STRUCT(FSBPredictCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Velocities)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, SubDeltaTime)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, Damping)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Graph-colored Gauss-Seidel distance solve: one thread per constraint, one dispatch
// per color, projecting both endpoints in place.
class FSBSolveDistanceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBSolveDistanceCS);
	SHADER_USE_PARAMETER_STRUCT(FSBSolveDistanceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConstraint>, Constraints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER(uint32, ColorStart)
		SHADER_PARAMETER(uint32, ColorCount)
		SHADER_PARAMETER(float, Stiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Graph-colored Gauss-Seidel per-tet volume solve: one thread per tetrahedron, one
// dispatch per color, moving all 4 vertices in place to restore the rest volume (SB-M2).
class FSBSolveVolumeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBSolveVolumeCS);
	SHADER_USE_PARAMETER_STRUCT(FSBSolveVolumeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUVolumeConstraint>, VolumeConstraints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER(uint32, ColorStart)
		SHADER_PARAMETER(uint32, ColorCount)
		SHADER_PARAMETER(float, Stiffness)
		SHADER_PARAMETER(float, VolumeStiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Mouse-drag grab: a single thread pulls one grabbed particle toward the cursor (SB-M3).
class FSBGrabCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBGrabCS);
	SHADER_USE_PARAMETER_STRUCT(FSBGrabCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER(uint32, GrabIndex)
		SHADER_PARAMETER(FVector3f, GrabTarget)
		SHADER_PARAMETER(float, GrabStiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

class FSBCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FSBCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PrevPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollider>, Colliders)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, NumColliders)
		SHADER_PARAMETER(float, ContactOffset)
		SHADER_PARAMETER(uint32, EnableGround)
		SHADER_PARAMETER(float, GroundZ)
		SHADER_PARAMETER(float, GroundFriction)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

class FSBFinalizeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBFinalizeCS);
	SHADER_USE_PARAMETER_STRUCT(FSBFinalizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Velocities)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, InvSubDeltaTime)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSBPredictCS,       "/SoftBodySim/Private/SBPredict.usf",       "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBSolveDistanceCS, "/SoftBodySim/Private/SBSolveDistance.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBSolveVolumeCS,   "/SoftBodySim/Private/SBSolveVolume.usf",   "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBGrabCS,          "/SoftBodySim/Private/SBGrab.usf",          "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBCollisionCS,     "/SoftBodySim/Private/SBCollision.usf",     "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBFinalizeCS,      "/SoftBodySim/Private/SBFinalize.usf",      "MainCS", SF_Compute);

//////////////////////////////////////////////////////////////////////////
// Resource lifetime
//////////////////////////////////////////////////////////////////////////

FSoftBodyRenderResources::FSoftBodyRenderResources() = default;

FSoftBodyRenderResources::~FSoftBodyRenderResources()
{
	PositionReadback.Reset();
}

//////////////////////////////////////////////////////////////////////////
// Init
//////////////////////////////////////////////////////////////////////////

void SoftBodyCompute::InitResources_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FSoftBodyRenderResources>& Resources,
	const TArray<FVector3f>& InitialPositions,
	const TArray<FVector3f>& InitialVelocities,
	const TArray<float>& InitialInvMasses,
	const TArray<FGPUConstraint>& Constraints,
	const TArray<FSoftBodyColorRange>& ColorRanges,
	const TArray<FGPUVolumeConstraint>& VolumeConstraints,
	const TArray<FSoftBodyColorRange>& VolumeColorRanges)
{
	check(IsInRenderingThread());
	check(Resources.IsValid());

	const int32 Num = InitialPositions.Num();
	if (Num <= 0)
	{
		return;
	}
	Resources->NumParticles = Num;

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGBufferRef Positions = CreateStructuredBuffer(
		GraphBuilder, TEXT("SoftBody.Positions"),
		sizeof(FVector3f), Num, InitialPositions.GetData(), sizeof(FVector3f) * Num);

	FRDGBufferRef Velocities = CreateStructuredBuffer(
		GraphBuilder, TEXT("SoftBody.Velocities"),
		sizeof(FVector3f), Num, InitialVelocities.GetData(), sizeof(FVector3f) * Num);

	FRDGBufferRef InvMasses = CreateStructuredBuffer(
		GraphBuilder, TEXT("SoftBody.InvMasses"),
		sizeof(float), Num, InitialInvMasses.GetData(), sizeof(float) * Num);

	GraphBuilder.QueueBufferExtraction(Positions, &Resources->PositionsBuffer);
	GraphBuilder.QueueBufferExtraction(Velocities, &Resources->VelocitiesBuffer);
	GraphBuilder.QueueBufferExtraction(InvMasses, &Resources->InvMassBuffer);

	// Color-sorted distance constraint buffer for the Gauss-Seidel solve.
	Resources->NumConstraints = Constraints.Num();
	Resources->ColorRanges = ColorRanges;
	if (Constraints.Num() > 0)
	{
		FRDGBufferRef ConstraintsBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("SoftBody.Constraints"),
			sizeof(FGPUConstraint), Constraints.Num(),
			Constraints.GetData(), sizeof(FGPUConstraint) * Constraints.Num());
		GraphBuilder.QueueBufferExtraction(ConstraintsBuf, &Resources->ConstraintsBuffer);
	}

	// Color-sorted per-tet volume constraint buffer (SB-M2).
	Resources->NumVolumeConstraints = VolumeConstraints.Num();
	Resources->VolumeColorRanges = VolumeColorRanges;
	if (VolumeConstraints.Num() > 0)
	{
		FRDGBufferRef VolumeBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("SoftBody.VolumeConstraints"),
			sizeof(FGPUVolumeConstraint), VolumeConstraints.Num(),
			VolumeConstraints.GetData(), sizeof(FGPUVolumeConstraint) * VolumeConstraints.Num());
		GraphBuilder.QueueBufferExtraction(VolumeBuf, &Resources->VolumeConstraintsBuffer);
	}

	GraphBuilder.Execute();

	Resources->PositionReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("SoftBody.PositionReadback"));
	Resources->bInitialized = true;
}

//////////////////////////////////////////////////////////////////////////
// Per-frame dispatch
//////////////////////////////////////////////////////////////////////////

void SoftBodyCompute::Dispatch_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FSoftBodyRenderResources>& Resources,
	const FSoftBodyParams& Params)
{
	check(IsInRenderingThread());

	if (!Resources.IsValid() || !Resources->bInitialized || Resources->NumParticles <= 0)
	{
		return;
	}

	const int32 Num = Resources->NumParticles;

	// --- Consume LAST frame's readback (non-stalling) ----------------------
	if (Resources->PositionReadback->IsReady())
	{
		const uint32 NumBytes = sizeof(FVector3f) * Num;
		const FVector3f* Src = static_cast<const FVector3f*>(Resources->PositionReadback->Lock(NumBytes));
		if (Src)
		{
			FScopeLock Lock(&Resources->PositionCopyCS);
			Resources->PositionCopy.SetNumUninitialized(Num);
			FMemory::Memcpy(Resources->PositionCopy.GetData(), Src, NumBytes);
			Resources->bHasPositionData = true;
		}
		Resources->PositionReadback->Unlock();
	}

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGBufferRef Positions  = GraphBuilder.RegisterExternalBuffer(Resources->PositionsBuffer, TEXT("SoftBody.Positions"));
	FRDGBufferRef Velocities = GraphBuilder.RegisterExternalBuffer(Resources->VelocitiesBuffer, TEXT("SoftBody.Velocities"));
	FRDGBufferRef InvMasses  = GraphBuilder.RegisterExternalBuffer(Resources->InvMassBuffer, TEXT("SoftBody.InvMasses"));
	FRDGBufferSRVRef InvMassesSRV = GraphBuilder.CreateSRV(InvMasses);

	// One predicted-position workspace buffer. The colored Gauss-Seidel solve and the
	// collision pass both operate in place on it, so no ping-pong is needed. Transient:
	// recomputed every frame from Positions/Velocities, so no need to persist.
	const FRDGBufferDesc PredictedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), Num);
	FRDGBufferRef Predicted = GraphBuilder.CreateBuffer(PredictedDesc, TEXT("SoftBody.Predicted"));

	const int32 Substeps   = FMath::Max(1, Params.Substeps);
	const int32 Iterations = FMath::Max(1, Params.SolverIterations);
	const float SubDt      = Params.DeltaTime / (float)Substeps;
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Num, kThreadGroupSize);

	// Colliders are constant within a frame; build the buffer once. The collision pass
	// also handles the optional ground plane, so we still need a (dummy) collider buffer
	// bound when there are no analytic colliders but the ground plane is enabled.
	const int32 NumColliders = Params.Colliders.Num();
	FRDGBufferSRVRef CollidersSRV = nullptr;
	if (NumColliders > 0)
	{
		FRDGBufferRef CollidersBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("SoftBody.Colliders"),
			sizeof(FGPUCollider), NumColliders,
			Params.Colliders.GetData(), sizeof(FGPUCollider) * NumColliders);
		CollidersSRV = GraphBuilder.CreateSRV(CollidersBuf);
	}
	else if (Params.bGroundPlane)
	{
		const FGPUCollider Dummy; // never read: NumColliders is passed as 0
		FRDGBufferRef CollidersBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("SoftBody.CollidersDummy"),
			sizeof(FGPUCollider), 1, &Dummy, sizeof(FGPUCollider));
		CollidersSRV = GraphBuilder.CreateSRV(CollidersBuf);
	}

	const bool bDoConstraints = Resources->NumConstraints > 0
		&& Resources->ConstraintsBuffer.IsValid()
		&& Resources->ColorRanges.Num() > 0;

	FRDGBufferSRVRef ConstraintsSRV = nullptr;
	if (bDoConstraints)
	{
		FRDGBufferRef ConstraintsBuf = GraphBuilder.RegisterExternalBuffer(
			Resources->ConstraintsBuffer, TEXT("SoftBody.Constraints"));
		ConstraintsSRV = GraphBuilder.CreateSRV(ConstraintsBuf);
	}

	const bool bDoVolume = Resources->NumVolumeConstraints > 0
		&& Resources->VolumeConstraintsBuffer.IsValid()
		&& Resources->VolumeColorRanges.Num() > 0;

	FRDGBufferSRVRef VolumeSRV = nullptr;
	if (bDoVolume)
	{
		FRDGBufferRef VolumeBuf = GraphBuilder.RegisterExternalBuffer(
			Resources->VolumeConstraintsBuffer, TEXT("SoftBody.VolumeConstraints"));
		VolumeSRV = GraphBuilder.CreateSRV(VolumeBuf);
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSBPredictCS>       PredictCS(ShaderMap);
	TShaderMapRef<FSBSolveDistanceCS> SolveCS(ShaderMap);
	TShaderMapRef<FSBSolveVolumeCS>   VolumeCS(ShaderMap);
	TShaderMapRef<FSBGrabCS>          GrabCS(ShaderMap);
	TShaderMapRef<FSBCollisionCS>     CollisionCS(ShaderMap);
	TShaderMapRef<FSBFinalizeCS>      FinalizeCS(ShaderMap);

	const bool bDoGrab = Params.bGrabActive
		&& Params.GrabIndex >= 0
		&& Params.GrabIndex < Num;

	if (SubDt > 0.0f)
	{
		for (int32 Step = 0; Step < Substeps; ++Step)
		{
			// --- Predict: Positions/Velocities -> Predicted ---
			{
				FSBPredictCS::FParameters* P = GraphBuilder.AllocParameters<FSBPredictCS::FParameters>();
				P->Positions          = GraphBuilder.CreateSRV(Positions);
				P->Velocities         = GraphBuilder.CreateSRV(Velocities);
				P->InvMasses          = InvMassesSRV;
				P->PredictedPositions = GraphBuilder.CreateUAV(Predicted);
				P->NumParticles       = (uint32)Num;
				P->SubDeltaTime       = SubDt;
				P->Gravity            = Params.Gravity;
				P->Damping            = Params.Damping;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("SBPredict (substep %d)", Step),
					PredictCS, P, GroupCount);
			}

			// --- Solve: graph-colored Gauss-Seidel constraints, in place ---
			// Within a color no two constraints share a particle (race-free UAV writes);
			// each color reads the previous one's results (RDG serializes the UAV), giving
			// true Gauss-Seidel propagation. No ping-pong needed. Each iteration relaxes the
			// distance edges then the per-tet volume constraints, so they converge together.
			const int32 SolveIters = (bDoConstraints || bDoVolume) ? Iterations : 0;
			for (int32 Iter = 0; Iter < SolveIters; ++Iter)
			{
				if (bDoConstraints)
				{
					for (int32 Color = 0; Color < Resources->ColorRanges.Num(); ++Color)
					{
						const FSoftBodyColorRange& Range = Resources->ColorRanges[Color];
						if (Range.Count <= 0)
						{
							continue;
						}

						FSBSolveDistanceCS::FParameters* P = GraphBuilder.AllocParameters<FSBSolveDistanceCS::FParameters>();
						P->Constraints = ConstraintsSRV;
						P->InvMasses   = InvMassesSRV;
						P->Positions   = GraphBuilder.CreateUAV(Predicted);
						P->ColorStart  = (uint32)Range.Start;
						P->ColorCount  = (uint32)Range.Count;
						P->Stiffness   = Params.Stiffness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("SBSolveDistance (substep %d iter %d color %d)", Step, Iter, Color),
							SolveCS, P, FComputeShaderUtils::GetGroupCount(Range.Count, kThreadGroupSize));
					}
				}

				if (bDoVolume)
				{
					for (int32 Color = 0; Color < Resources->VolumeColorRanges.Num(); ++Color)
					{
						const FSoftBodyColorRange& Range = Resources->VolumeColorRanges[Color];
						if (Range.Count <= 0)
						{
							continue;
						}

						FSBSolveVolumeCS::FParameters* P = GraphBuilder.AllocParameters<FSBSolveVolumeCS::FParameters>();
						P->VolumeConstraints = VolumeSRV;
						P->InvMasses         = InvMassesSRV;
						P->Positions         = GraphBuilder.CreateUAV(Predicted);
						P->ColorStart        = (uint32)Range.Start;
						P->ColorCount        = (uint32)Range.Count;
						P->Stiffness         = Params.Stiffness;
						P->VolumeStiffness   = Params.VolumeStiffness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("SBSolveVolume (substep %d iter %d color %d)", Step, Iter, Color),
							VolumeCS, P, FComputeShaderUtils::GetGroupCount(Range.Count, kThreadGroupSize));
					}
				}
			}

			// --- Grab: pull the mouse-grabbed particle toward the cursor target ---
			// After the solve (so it's the last word on that vertex), before collision (so
			// it still can't be dragged through the ground).
			if (bDoGrab)
			{
				FSBGrabCS::FParameters* P = GraphBuilder.AllocParameters<FSBGrabCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateUAV(Predicted);
				P->GrabIndex          = (uint32)Params.GrabIndex;
				P->GrabTarget         = Params.GrabTarget;
				P->GrabStiffness      = Params.GrabStiffness;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("SBGrab (substep %d)", Step),
					GrabCS, P, FIntVector(1, 1, 1));
			}

			// --- Collision: project predicted positions out of colliders + ground ---
			if (CollidersSRV)
			{
				FSBCollisionCS::FParameters* P = GraphBuilder.AllocParameters<FSBCollisionCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateUAV(Predicted);   // in place; one thread per particle
				P->PrevPositions      = GraphBuilder.CreateSRV(Positions);   // start-of-substep position
				P->InvMasses          = InvMassesSRV;
				P->Colliders          = CollidersSRV;
				P->NumParticles       = (uint32)Num;
				P->NumColliders       = (uint32)NumColliders;
				P->ContactOffset      = 1.0f; // cm skin so the body rests just off the surface
				P->EnableGround       = Params.bGroundPlane ? 1u : 0u;
				P->GroundZ            = Params.GroundZ;
				P->GroundFriction     = Params.Friction;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("SBCollision (substep %d)", Step),
					CollisionCS, P, GroupCount);
			}

			// --- Finalize: derive velocity, commit positions ---
			{
				FSBFinalizeCS::FParameters* P = GraphBuilder.AllocParameters<FSBFinalizeCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateSRV(Predicted);
				P->InvMasses          = InvMassesSRV;
				P->Positions          = GraphBuilder.CreateUAV(Positions);
				P->Velocities         = GraphBuilder.CreateUAV(Velocities);
				P->NumParticles       = (uint32)Num;
				P->InvSubDeltaTime    = 1.0f / SubDt;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("SBFinalize (substep %d)", Step),
					FinalizeCS, P, GroupCount);
			}
		}
	}

	// Kick a fresh readback of the committed positions for next frame's render verts.
	AddEnqueueCopyPass(GraphBuilder, Resources->PositionReadback.Get(), Positions, sizeof(FVector3f) * Num);

	GraphBuilder.Execute();
}
