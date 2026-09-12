// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMaterials : ModuleRules
{
	public HyperAIStudioMaterials(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"MaterialEditor",
			"RHI",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
