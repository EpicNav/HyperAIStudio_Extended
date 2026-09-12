// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioDynamicMaterial : ModuleRules
{
	public HyperAIStudioDynamicMaterial(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RHI",
			"AssetRegistry",
			"HyperAIStudio",
			"Projects",
			"ToolsetRegistry",
			"DynamicMaterial",
			"DynamicMaterialEditor",
			"UnrealEd"
		});
	}
}
