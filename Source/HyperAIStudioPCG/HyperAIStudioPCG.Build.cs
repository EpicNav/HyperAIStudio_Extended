// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPCG : ModuleRules
{
	public HyperAIStudioPCG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"PCG",
			"PCGEditor",
			"ToolsetRegistry"
		});
	}
}
