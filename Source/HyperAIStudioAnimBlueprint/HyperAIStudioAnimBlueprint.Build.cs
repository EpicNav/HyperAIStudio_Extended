// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAnimBlueprint : ModuleRules
{
	public HyperAIStudioAnimBlueprint(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AnimGraph",
			"AnimGraphRuntime",
			"AssetRegistry",
			"BlueprintGraph",
			"HyperAIStudio",
			"RenderCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
