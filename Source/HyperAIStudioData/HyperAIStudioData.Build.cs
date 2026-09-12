// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioData : ModuleRules
{
	public HyperAIStudioData(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
