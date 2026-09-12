// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioCharacter : ModuleRules
{
	public HyperAIStudioCharacter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
