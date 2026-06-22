// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SoftBodyActor.generated.h"

class USoftBodyComponent;

/** Convenience Actor: drag into a level to get a GPU-simulated soft body. */
UCLASS()
class SOFTBODYSIM_API ASoftBodyActor : public AActor
{
	GENERATED_BODY()

public:
	ASoftBodyActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SoftBody")
	TObjectPtr<USoftBodyComponent> SoftBody;
};
