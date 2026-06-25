// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyMeshProxy.h"

#include "Components/MeshComponent.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Engine/Engine.h"
#include "PrimitiveViewRelevance.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "SceneInterface.h"
#include "SceneView.h"

FSoftBodyMeshSceneProxy::FSoftBodyMeshSceneProxy(
	UMeshComponent* Component,
	const TArray<FDynamicMeshVertex>& InVertices,
	const TArray<uint32>& InIndices,
	UMaterialInterface* InMaterial)
	: FPrimitiveSceneProxy(Component)
	, VertexFactory(GetScene().GetFeatureLevel(), "FSoftBodyMeshSceneProxy")
	, Material(InMaterial)
	, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
{
	NumVerts = InVertices.Num();

	// InitFromDynamicVertex takes a mutable array; copy in.
	TArray<FDynamicMeshVertex> Vertices = InVertices;
	IndexBuffer.Indices = InIndices;

	VertexBuffers.InitFromDynamicVertex(&VertexFactory, Vertices, /*NumTexCoords*/ 1);

	BeginInitResource(&VertexBuffers.PositionVertexBuffer);
	BeginInitResource(&VertexBuffers.StaticMeshVertexBuffer);
	BeginInitResource(&VertexBuffers.ColorVertexBuffer);
	BeginInitResource(&IndexBuffer);
	BeginInitResource(&VertexFactory);

	if (Material == nullptr)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}
}

FSoftBodyMeshSceneProxy::~FSoftBodyMeshSceneProxy()
{
	VertexBuffers.PositionVertexBuffer.ReleaseResource();
	VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
	VertexBuffers.ColorVertexBuffer.ReleaseResource();
	IndexBuffer.ReleaseResource();
	VertexFactory.ReleaseResource();
}

SIZE_T FSoftBodyMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FSoftBodyMeshSceneProxy::UpdateVertices_RenderThread(
	FRHICommandListBase& RHICmdList,
	const TArray<FVector3f>& Positions,
	const TArray<FVector3f>& Normals,
	const TArray<FVector3f>& Tangents)
{
	const int32 Count = FMath::Min3(Positions.Num(), Normals.Num(), Tangents.Num());
	if (Count != NumVerts)
	{
		return; // mismatched update; skip rather than corrupt buffers
	}

	for (int32 i = 0; i < NumVerts; ++i)
	{
		const FVector3f N = Normals[i];
		const FVector3f T = Tangents[i];
		const FVector3f B = FVector3f::CrossProduct(N, T); // bitangent

		VertexBuffers.PositionVertexBuffer.VertexPosition(i) = Positions[i];
		VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, T, B, N);
	}

	// Upload positions.
	{
		FPositionVertexBuffer& VB = VertexBuffers.PositionVertexBuffer;
		const uint32 Size = VB.GetNumVertices() * VB.GetStride();
		void* Dst = RHICmdList.LockBuffer(VB.VertexBufferRHI, 0, Size, RLM_WriteOnly);
		FMemory::Memcpy(Dst, VB.GetVertexData(), Size);
		RHICmdList.UnlockBuffer(VB.VertexBufferRHI);
	}

	// Upload tangent basis (normals live here).
	{
		FStaticMeshVertexBuffer& VB = VertexBuffers.StaticMeshVertexBuffer;
		void* Dst = RHICmdList.LockBuffer(VB.TangentsVertexBuffer.VertexBufferRHI, 0, VB.GetTangentSize(), RLM_WriteOnly);
		FMemory::Memcpy(Dst, VB.GetTangentData(), VB.GetTangentSize());
		RHICmdList.UnlockBuffer(VB.TangentsVertexBuffer.VertexBufferRHI);
	}
}

void FSoftBodyMeshSceneProxy::UpdateIndices_RenderThread(const TArray<uint32>& NewIndices)
{
	check(IsInRenderingThread());

	// The triangle count changes on a cut, so reallocate the index buffer. NumPrimitives is
	// re-read from IndexBuffer.Indices.Num() each frame in GetDynamicMeshElements, so it adapts.
	IndexBuffer.ReleaseResource();
	IndexBuffer.Indices = NewIndices;
	IndexBuffer.InitResource(FRHICommandListImmediate::Get());
}

void FSoftBodyMeshSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (IndexBuffer.Indices.Num() == 0 || NumVerts == 0)
	{
		return;
	}

	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

	FColoredMaterialRenderProxy* WireframeMaterialInstance = nullptr;
	if (bWireframe)
	{
		WireframeMaterialInstance = new FColoredMaterialRenderProxy(
			GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
			FLinearColor(0.0f, 0.5f, 1.0f));
		Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
	}

	FMaterialRenderProxy* MaterialProxy = bWireframe
		? (FMaterialRenderProxy*)WireframeMaterialInstance
		: Material->GetRenderProxy();

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (!(VisibilityMap & (1 << ViewIndex)))
		{
			continue;
		}

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &VertexFactory;
		Mesh.MaterialRenderProxy = MaterialProxy;
		Mesh.bWireframe = bWireframe;
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = &IndexBuffer;
		BatchElement.FirstIndex = 0;
		BatchElement.NumPrimitives = IndexBuffer.Indices.Num() / 3;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = NumVerts - 1;

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		FPrimitiveUniformShaderParametersBuilder Builder;
		BuildUniformShaderParameters(Builder);
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FSoftBodyMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

bool FSoftBodyMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

uint32 FSoftBodyMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}
