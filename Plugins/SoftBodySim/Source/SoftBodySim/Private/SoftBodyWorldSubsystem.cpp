// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyWorldSubsystem.h"
#include "SoftBodyComponent.h"
#include "SoftBodyResources.h"

#include "RenderingThread.h"

void USoftBodyWorldSubsystem::RegisterComponent(USoftBodyComponent* Component)
{
	if (Component)
	{
		RegisteredComponents.AddUnique(Component);
	}
}

void USoftBodyWorldSubsystem::UnregisterComponent(USoftBodyComponent* Component)
{
	RegisteredComponents.RemoveAll([Component](const TWeakObjectPtr<USoftBodyComponent>& C)
	{
		return !C.IsValid() || C.Get() == Component;
	});
}

void USoftBodyWorldSubsystem::Tick(float DeltaTime)
{
	// Gather the bodies opting into inter-body collision, dropping any that went away.
	TArray<TSharedPtr<FSoftBodyRenderResources>> Bodies;
	float Thickness = 0.0f;
	float Stiffness = 0.0f;
	int32 Iterations = 1;

	for (int32 i = RegisteredComponents.Num() - 1; i >= 0; --i)
	{
		USoftBodyComponent* C = RegisteredComponents[i].Get();
		if (!C)
		{
			RegisteredComponents.RemoveAtSwap(i);
			continue;
		}
		if (!C->bInterBodyCollision)
		{
			continue;
		}
		TSharedPtr<FSoftBodyRenderResources> Res = C->GetRenderResources();
		if (!Res.IsValid())
		{
			continue;
		}

		Bodies.Add(Res);
		Thickness  = FMath::Max(Thickness, C->InterBodyThickness);
		Stiffness  = FMath::Max(Stiffness, C->InterBodyStiffness);
		Iterations = FMath::Max(Iterations, C->InterBodyIterations);
	}

	if (Bodies.Num() < 2 || Thickness <= 0.0f)
	{
		return; // need at least two participating bodies
	}

	// Enqueue after the per-body sims (which were enqueued during their component ticks),
	// so it corrects this frame's committed positions.
	ENQUEUE_RENDER_COMMAND(SoftBodyInterBody)(
		[Bodies = MoveTemp(Bodies), Thickness, Stiffness, Iterations]
		(FRHICommandListImmediate& RHICmdList)
		{
			SoftBodyCompute::DispatchInterBody_RenderThread(RHICmdList, Bodies, Thickness, Stiffness, Iterations);
		});
}
