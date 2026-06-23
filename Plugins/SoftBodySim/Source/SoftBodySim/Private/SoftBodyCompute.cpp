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

// Self-collision hash grid: max particle indices stored per bucket. Injected as MAX_PER_CELL.
static constexpr uint32 kMaxPerCell = 16;

// Smallest prime >= N, for the spatial-hash table size (reduces modulo clustering).
static uint32 NextPrime(uint32 N)
{
	auto IsPrime = [](uint32 X) -> bool
	{
		if (X < 2) return false;
		if (X % 2 == 0) return X == 2;
		for (uint32 d = 3; d * d <= X; d += 2)
		{
			if (X % d == 0) return false;
		}
		return true;
	};
	N = FMath::Max(N, 3u);
	if (N % 2 == 0) ++N;
	while (!IsPrime(N)) N += 2;
	return N;
}

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

// XPBD distance solve (SB-M7): like FSBSolveDistanceCS but with per-constraint compliance
// + a Lagrange-multiplier accumulator, for iteration-count-independent stiffness.
class FSBSolveDistanceXPBDCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBSolveDistanceXPBDCS);
	SHADER_USE_PARAMETER_STRUCT(FSBSolveDistanceXPBDCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConstraint>, Constraints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, Lambdas)
		SHADER_PARAMETER(uint32, ColorStart)
		SHADER_PARAMETER(uint32, ColorCount)
		SHADER_PARAMETER(float, GlobalCompliance)
		SHADER_PARAMETER(float, SoftCompliance)
		SHADER_PARAMETER(float, InvDtSq)
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

// Self-collision broadphase build: bin particles into a spatial hash grid (SB-M4).
class FSBBuildGridCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBBuildGridCS);
	SHADER_USE_PARAMETER_STRUCT(FSBBuildGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellParticles)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, TableSize)
		SHADER_PARAMETER(float, CellSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("MAX_PER_CELL"), kMaxPerCell);
	}
};

// Self-collision response: scan the 27 neighbour cells, repel close non-adjacent particles (SB-M4).
class FSBSelfCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSBSelfCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FSBSelfCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedOut)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, ResX)
		SHADER_PARAMETER(uint32, ResY)
		SHADER_PARAMETER(uint32, ResZ)
		SHADER_PARAMETER(uint32, TableSize)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, Thickness)
		SHADER_PARAMETER(float, SelfStiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("MAX_PER_CELL"), kMaxPerCell);
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
IMPLEMENT_GLOBAL_SHADER(FSBSolveDistanceXPBDCS, "/SoftBodySim/Private/SBSolveDistanceXPBD.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBSolveVolumeCS,   "/SoftBodySim/Private/SBSolveVolume.usf",   "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBGrabCS,          "/SoftBodySim/Private/SBGrab.usf",          "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBBuildGridCS,     "/SoftBodySim/Private/SBBuildGrid.usf",     "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSBSelfCollisionCS, "/SoftBodySim/Private/SBSelfCollision.usf", "MainCS", SF_Compute);
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

	// Predicted-position workspace. The colored Gauss-Seidel solve, grab and collision
	// passes operate in place on PredictedA; the self-collision gather (SB-M4) needs a
	// clean snapshot to read, so it ping-pongs into PredictedB. Both are transient
	// (recomputed every frame from Positions/Velocities).
	const FRDGBufferDesc PredictedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), Num);
	FRDGBufferRef PredictedA = GraphBuilder.CreateBuffer(PredictedDesc, TEXT("SoftBody.PredictedA"));
	FRDGBufferRef PredictedB = GraphBuilder.CreateBuffer(PredictedDesc, TEXT("SoftBody.PredictedB"));

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
	TShaderMapRef<FSBPredictCS>           PredictCS(ShaderMap);
	TShaderMapRef<FSBSolveDistanceCS>     SolveCS(ShaderMap);
	TShaderMapRef<FSBSolveDistanceXPBDCS> SolveXPBDCS(ShaderMap);
	TShaderMapRef<FSBSolveVolumeCS>       VolumeCS(ShaderMap);

	const bool bUseXPBD = Params.bUseXPBD;
	const float InvDtSq = (SubDt > 0.0f) ? (1.0f / (SubDt * SubDt)) : 0.0f;
	TShaderMapRef<FSBBuildGridCS>     BuildGridCS(ShaderMap);
	TShaderMapRef<FSBSelfCollisionCS> SelfCollisionCS(ShaderMap);
	TShaderMapRef<FSBGrabCS>          GrabCS(ShaderMap);
	TShaderMapRef<FSBCollisionCS>     CollisionCS(ShaderMap);
	TShaderMapRef<FSBFinalizeCS>      FinalizeCS(ShaderMap);

	const bool bDoGrab = Params.bGrabActive
		&& Params.GrabIndex >= 0
		&& Params.GrabIndex < Num;

	const bool bDoSelfCollision = Params.bSelfCollision && Params.SelfThickness > 0.0f;
	const uint32 SelfTableSize = NextPrime(2u * (uint32)Num);

	if (SubDt > 0.0f)
	{
		for (int32 Step = 0; Step < Substeps; ++Step)
		{
			// `Solved` tracks the buffer holding the latest positions through the substep.
			// Starts as PredictedA; the self-collision ping-pong may flip it to PredictedB.
			FRDGBufferRef Solved = PredictedA;

			// --- Predict: Positions/Velocities -> PredictedA ---
			{
				FSBPredictCS::FParameters* P = GraphBuilder.AllocParameters<FSBPredictCS::FParameters>();
				P->Positions          = GraphBuilder.CreateSRV(Positions);
				P->Velocities         = GraphBuilder.CreateSRV(Velocities);
				P->InvMasses          = InvMassesSRV;
				P->PredictedPositions = GraphBuilder.CreateUAV(PredictedA);
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
			// XPBD (SB-M7) needs a per-constraint Lagrange-multiplier accumulator that is
			// reset once per substep and carried across the iterations. Typed float buffer;
			// cleared via a uint view (0u == 0.0f bit pattern).
			FRDGBufferRef LambdaDist = nullptr;
			if (bDoConstraints && bUseXPBD)
			{
				const FRDGBufferDesc LambdaDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), Resources->NumConstraints);
				LambdaDist = GraphBuilder.CreateBuffer(LambdaDesc, TEXT("SoftBody.LambdaDist"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(LambdaDist, PF_R32_UINT), 0u);
			}

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

						if (bUseXPBD)
						{
							FSBSolveDistanceXPBDCS::FParameters* P = GraphBuilder.AllocParameters<FSBSolveDistanceXPBDCS::FParameters>();
							P->Constraints       = ConstraintsSRV;
							P->InvMasses         = InvMassesSRV;
							P->Positions         = GraphBuilder.CreateUAV(Solved);
							P->Lambdas           = GraphBuilder.CreateUAV(LambdaDist, PF_R32_FLOAT);
							P->ColorStart        = (uint32)Range.Start;
							P->ColorCount        = (uint32)Range.Count;
							P->GlobalCompliance  = Params.XpbdGlobalCompliance;
							P->SoftCompliance    = Params.XpbdSoftCompliance;
							P->InvDtSq           = InvDtSq;

							FComputeShaderUtils::AddPass(GraphBuilder,
								RDG_EVENT_NAME("SBSolveDistanceXPBD (substep %d iter %d color %d)", Step, Iter, Color),
								SolveXPBDCS, P, FComputeShaderUtils::GetGroupCount(Range.Count, kThreadGroupSize));
						}
						else
						{
							FSBSolveDistanceCS::FParameters* P = GraphBuilder.AllocParameters<FSBSolveDistanceCS::FParameters>();
							P->Constraints = ConstraintsSRV;
							P->InvMasses   = InvMassesSRV;
							P->Positions   = GraphBuilder.CreateUAV(Solved);
							P->ColorStart  = (uint32)Range.Start;
							P->ColorCount  = (uint32)Range.Count;
							P->Stiffness   = Params.Stiffness;

							FComputeShaderUtils::AddPass(GraphBuilder,
								RDG_EVENT_NAME("SBSolveDistance (substep %d iter %d color %d)", Step, Iter, Color),
								SolveCS, P, FComputeShaderUtils::GetGroupCount(Range.Count, kThreadGroupSize));
						}
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
						P->Positions         = GraphBuilder.CreateUAV(Solved);
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

			// --- Self-collision: spatial-hash broadphase + Jacobi repulsion (SB-M4) ---
			// Runs before grab/external colliders so a hard pin or solid collider still gets
			// the final say. Ping-pongs Solved -> Other so the gather reads a clean snapshot.
			if (bDoSelfCollision)
			{
				const int32 SelfIters = FMath::Max(1, Params.SelfCollisionIterations);
				for (int32 SIt = 0; SIt < SelfIters; ++SIt)
				{
					FRDGBufferRef Other = (Solved == PredictedA) ? PredictedB : PredictedA;

					// CellCounts is a TYPED uint buffer (clean ClearUAV + atomics); CellParticles
					// is a plain structured index list (never cleared, written by slot).
					const FRDGBufferDesc CountsDesc   = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SelfTableSize);
					const FRDGBufferDesc ParticleDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SelfTableSize * kMaxPerCell);
					FRDGBufferRef CellCounts    = GraphBuilder.CreateBuffer(CountsDesc,   TEXT("SoftBody.SelfCellCounts"));
					FRDGBufferRef CellParticles = GraphBuilder.CreateBuffer(ParticleDesc, TEXT("SoftBody.SelfCellParticles"));

					FRDGBufferUAVRef CellCountsUAV = GraphBuilder.CreateUAV(CellCounts, PF_R32_UINT);
					AddClearUAVPass(GraphBuilder, CellCountsUAV, 0u);

					// Build: bin particles into the hash grid.
					{
						FSBBuildGridCS::FParameters* P = GraphBuilder.AllocParameters<FSBBuildGridCS::FParameters>();
						P->PredictedIn   = GraphBuilder.CreateSRV(Solved);
						P->CellCounts    = CellCountsUAV;
						P->CellParticles = GraphBuilder.CreateUAV(CellParticles);
						P->NumParticles  = (uint32)Num;
						P->TableSize     = SelfTableSize;
						P->CellSize      = Params.SelfThickness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("SBBuildGrid (substep %d iter %d)", Step, SIt),
							BuildGridCS, P, GroupCount);
					}

					// Respond: repel close non-adjacent particles, writing the other buffer.
					{
						FSBSelfCollisionCS::FParameters* P = GraphBuilder.AllocParameters<FSBSelfCollisionCS::FParameters>();
						P->PredictedIn   = GraphBuilder.CreateSRV(Solved);
						P->InvMasses     = InvMassesSRV;
						P->CellCounts    = GraphBuilder.CreateSRV(CellCounts, PF_R32_UINT);
						P->CellParticles = GraphBuilder.CreateSRV(CellParticles);
						P->PredictedOut  = GraphBuilder.CreateUAV(Other);
						P->NumParticles  = (uint32)Num;
						P->ResX          = (uint32)Params.ResX;
						P->ResY          = (uint32)Params.ResY;
						P->ResZ          = (uint32)Params.ResZ;
						P->TableSize     = SelfTableSize;
						P->CellSize      = Params.SelfThickness;
						P->Thickness     = Params.SelfThickness;
						P->SelfStiffness = Params.SelfStiffness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("SBSelfCollision (substep %d iter %d)", Step, SIt),
							SelfCollisionCS, P, GroupCount);
					}

					Solved = Other; // corrected positions feed the next iteration / later passes
				}
			}

			// --- Grab: pull the mouse-grabbed particle toward the cursor target ---
			// After the solve (so it's the last word on that vertex), before collision (so
			// it still can't be dragged through the ground).
			if (bDoGrab)
			{
				FSBGrabCS::FParameters* P = GraphBuilder.AllocParameters<FSBGrabCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateUAV(Solved);
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
				P->PredictedPositions = GraphBuilder.CreateUAV(Solved);      // in place; one thread per particle
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
				P->PredictedPositions = GraphBuilder.CreateSRV(Solved);
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
