// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioUI : ModuleRules
{
	public HyperAIStudioUI(ReadOnlyTargetRules Target) : base(Target)
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
			"UMG",
			"UMGEditor",
			"UnrealEd",
			"SlateCore",
			"MovieScene",
			"ModelViewViewModel",
			"ModelViewViewModelBlueprint"
		});
	}
}
