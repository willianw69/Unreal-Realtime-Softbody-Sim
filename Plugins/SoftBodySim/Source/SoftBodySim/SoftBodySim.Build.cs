// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SoftBodySim : ModuleRules
{
	public SoftBodySim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore"   // EKeys (mouse-drag picking, SB-M3)
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",     // IPluginManager, to locate our Shaders/ directory
			"RenderCore",   // FGlobalShader, RDG (FRDGBuilder), FComputeShaderUtils
			"RHI",          // FRHIGPUBufferReadback, buffer descriptors
			"Renderer"      // mirrors ClothSim; available for later distance-field collision (SB-M4)
		});
	}
}
