// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoftBodySimModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h" // AddShaderSourceDirectoryMapping

#define LOCTEXT_NAMESPACE "FSoftBodySimModule"

void FSoftBodySimModule::StartupModule()
{
	// Map the virtual shader directory "/SoftBodySim" -> <Plugin>/Shaders.
	// In .usf/.cpp we then reference shaders as "/SoftBodySim/Private/SBPredict.usf".
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SoftBodySim"));
	check(Plugin.IsValid());

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/SoftBodySim"), ShaderDir);
}

void FSoftBodySimModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSoftBodySimModule, SoftBodySim)
