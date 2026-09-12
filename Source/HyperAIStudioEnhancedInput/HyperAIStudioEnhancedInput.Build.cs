// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioEnhancedInput : ModuleRules
{
	public HyperAIStudioEnhancedInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"EnhancedInput",
			"InputCore",
			"AssetRegistry",
			"GameplayTags",
			"UnrealEd"
		});
	}
}
