// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SoftBodyDemo : ModuleRules
{
	public SoftBodyDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore"
		});

		// The soft body simulation will live entirely in a SoftBodySim plugin (added next).
		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
