// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "SceneView.h"                      // FViewUniformShaderParameters, TUniformBufferRef
#include "GlobalDistanceFieldParameters.h" // FGlobalDistanceFieldParameterData

/**
 * Render-thread snapshot of the scene's Global Distance Field, captured by the scene
 * view extension and consumed by the soft body collision pass (SB-M8). The GDF is owned
 * by the renderer and only handed to us during scene rendering, so we cache it and let
 * the (separately-enqueued) soft body sim read it. A 1-frame lag is harmless — the GDF
 * clipmap atlas is a persistent allocation. Ported from RT_ClothSim (FClothGDFCache).
 */
struct FSoftBodyGDFCache
{
	bool      bValid = false;
	FGlobalDistanceFieldParameterData Data;
	FVector3f PreViewTranslation = FVector3f::ZeroVector; // world -> translated world

	// The GDF shader header transitively references the engine View uniform buffer
	// (ResolvedView), so the collision pass must bind a valid View even though the GDF
	// inputs themselves come from `Data`. We snapshot it here.
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer;
};

namespace SoftBodyGDF
{
	/** Register the view extension once (call from the game thread). */
	void EnsureRegistered();

	/** Latest cached GDF data. Render-thread access only. */
	const FSoftBodyGDFCache& Get();
}

/**
 * Minimal view extension whose only job is to copy the scene's Global Distance Field
 * parameters (and the view's pre-view translation) into the cache each frame.
 */
class FSoftBodySceneViewExtension : public FSceneViewExtensionBase
{
public:
	FSoftBodySceneViewExtension(const FAutoRegister& AutoRegister);

	//~ ISceneViewExtension (no-ops except the GDF capture)
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	// Captured after the base pass, by which point the renderer has built the GDF.
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
};
