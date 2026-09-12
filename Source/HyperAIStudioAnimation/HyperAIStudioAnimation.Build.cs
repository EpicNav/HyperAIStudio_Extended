// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAnimation : ModuleRules
{
	public HyperAIStudioAnimation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This module deliberately links only the always-present editor animation surface.
		// ControlRig/RigVM, IKRig/Retargeter, and PoseSearch stay prerequisite-gated and
		// are never reached through reflection or a script fallback.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"ToolsetRegistry",
			"UnrealEd",
			"AnimGraph",
			"BlueprintGraph",
			"Kismet",
			"Projects"
		});
	}
}
