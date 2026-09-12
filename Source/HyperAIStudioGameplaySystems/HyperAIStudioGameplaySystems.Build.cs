// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioGameplaySystems : ModuleRules
{
	public HyperAIStudioGameplaySystems(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"GameFeatures",
			"HyperAIStudio",
			"MassSpawner",
			"Projects",
			"ToolsetRegistry",
			"WorldConditions"
		});
	}
}
