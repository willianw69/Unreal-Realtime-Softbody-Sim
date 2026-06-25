// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "DynamicMeshBuilder.h"           // FDynamicMeshVertex, FDynamicMeshIndexBuffer32
#include "LocalVertexFactory.h"
#include "StaticMeshResources.h"          // FStaticMeshVertexBuffers
#include "MaterialShared.h"               // FMaterialRelevance

class UMeshComponent;
class UMaterialInterface;

/**
 * Scene proxy for the soft body surface mesh.
 *
 * Modeled on the engine's FProceduralMeshSceneProxy (single section). Topology is
 * static (the lattice boundary never re-triangulates), so the index buffer is built
 * once. Each frame the component pushes fresh vertex positions/normals from the GPU
 * sim (delivered via a small CPU readback) and we update the position + tangent vertex
 * buffers with a lock+memcpy. Standard FLocalVertexFactory rendering then gives us
 * correct lighting, shadows, and material support for free.
 *
 * Ported from FClothMeshSceneProxy (RT_ClothSim).
 */
class FSoftBodyMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FSoftBodyMeshSceneProxy(
		UMeshComponent* Component,
		const TArray<FDynamicMeshVertex>& InVertices,
		const TArray<uint32>& InIndices,
		UMaterialInterface* InMaterial);

	virtual ~FSoftBodyMeshSceneProxy();

	SIZE_T GetTypeHash() const override;

	/** Replace positions + tangent basis with new per-vertex data, then upload. */
	void UpdateVertices_RenderThread(
		FRHICommandListBase& RHICmdList,
		const TArray<FVector3f>& Positions,
		const TArray<FVector3f>& Normals,
		const TArray<FVector3f>& Tangents);

	/** Replace the index buffer (topology change from a cut, SB-M10). Reallocates the
	 *  index buffer since the triangle count changes. */
	void UpdateIndices_RenderThread(const TArray<uint32>& NewIndices);

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;
	virtual uint32 GetMemoryFootprint() const override;

private:
	FStaticMeshVertexBuffers VertexBuffers;
	FDynamicMeshIndexBuffer32 IndexBuffer;
	FLocalVertexFactory VertexFactory;

	UMaterialInterface* Material = nullptr;
	FMaterialRelevance MaterialRelevance;

	int32 NumVerts = 0;
};
