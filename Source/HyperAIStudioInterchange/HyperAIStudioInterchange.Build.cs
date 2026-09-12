// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioInterchange : ModuleRules
{
	public HyperAIStudioInterchange(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"InterchangeCore",
			"InterchangeEngine",
			"ToolsetRegistry"
		});
	}
}
