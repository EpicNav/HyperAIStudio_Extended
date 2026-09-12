// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPropertyAnimation : ModuleRules
{
	public HyperAIStudioPropertyAnimation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"PropertyAnimatorCore",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
