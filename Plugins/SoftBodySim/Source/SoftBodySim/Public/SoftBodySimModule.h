// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * SoftBodySim runtime module.
 *
 * Loads at the PostConfigInit phase (see SoftBodySim.uplugin) which is BEFORE the
 * engine starts compiling shaders. That ordering matters: in StartupModule() we
 * register a virtual shader path ("/SoftBodySim") pointing at this plugin's Shaders/
 * folder. If the module loaded later, the shader compiler would not know where to
 * find our .usf files and compilation would fail.
 */
class FSoftBodySimModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
