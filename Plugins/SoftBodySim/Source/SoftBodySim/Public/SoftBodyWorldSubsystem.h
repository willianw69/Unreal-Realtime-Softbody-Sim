// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "SoftBodyWorldSubsystem.generated.h"

class USoftBodyComponent;

/**
 * Coordinates collision BETWEEN soft body actors (SB-M9).
 *
 * Each USoftBodyComponent simulates independently (its own buffers + dispatch). This
 * world subsystem registers every live component and, once per frame after they've all
 * ticked, enqueues a single GPU pass that gathers every participating body's particles
 * into a shared spatial hash and repels particles of different bodies apart — writing
 * the corrections back into each body's position buffer. It's a post-sim positional
 * correction, so it doesn't disturb each body's own substep loop.
 *
 * Only components with bInterBodyCollision = true participate; at least two are needed.
 */
UCLASS()
class SOFTBODYSIM_API USoftBodyWorldSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	void RegisterComponent(USoftBodyComponent* Component);
	void UnregisterComponent(USoftBodyComponent* Component);

	//~ FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(USoftBodyWorldSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return RegisteredComponents.Num() > 0; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

private:
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<USoftBodyComponent>> RegisteredComponents;
};
