// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPaper2D : ModuleRules
{
	public HyperAIStudioPaper2D(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"Paper2D",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
