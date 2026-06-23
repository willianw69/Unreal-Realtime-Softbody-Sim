// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodySceneViewExtension.h"

#include "FXRenderingUtils.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Containers/StridedView.h"

// Render-thread-only cache. The view extension (render thread) writes it and the soft
// body dispatch (render thread) reads it, so no synchronization is needed.
static FSoftBodyGDFCache GSoftBodyGDFCache;

// Held for the lifetime of the registration.
static TSharedPtr<FSoftBodySceneViewExtension, ESPMode::ThreadSafe> GSoftBodyViewExtension;

void SoftBodyGDF::EnsureRegistered()
{
	check(IsInGameThread());
	if (!GSoftBodyViewExtension.IsValid())
	{
		GSoftBodyViewExtension = FSceneViewExtensions::NewExtension<FSoftBodySceneViewExtension>();
	}
}

const FSoftBodyGDFCache& SoftBodyGDF::Get()
{
	return GSoftBodyGDFCache;
}

FSoftBodySceneViewExtension::FSoftBodySceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FSoftBodySceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// The GDF parameter data is shared across the scene's views; one view is enough.
	const TConstStridedView<FSceneView> Views = MakeStridedView(0, &InView, 1);
	const FGlobalDistanceFieldParameterData* Data = UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData(Views);

	if (Data && Data->NumGlobalSDFClipmaps > 0 && InView.ViewUniformBuffer.IsValid())
	{
		GSoftBodyGDFCache.Data = *Data;
		GSoftBodyGDFCache.PreViewTranslation = FVector3f(InView.ViewMatrices.GetPreViewTranslation());
		GSoftBodyGDFCache.ViewUniformBuffer = InView.ViewUniformBuffer;
		GSoftBodyGDFCache.bValid = true;
	}
	else
	{
		GSoftBodyGDFCache.bValid = false;
		GSoftBodyGDFCache.ViewUniformBuffer.SafeRelease();
	}
}
