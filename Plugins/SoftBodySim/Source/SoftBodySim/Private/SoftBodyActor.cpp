// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodyActor.h"
#include "SoftBodyComponent.h"

ASoftBodyActor::ASoftBodyActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SoftBody = CreateDefaultSubobject<USoftBodyComponent>(TEXT("SoftBody"));
	RootComponent = SoftBody;
}
