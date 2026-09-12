// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudio : ModuleRules
{
	public HyperAIStudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"DeveloperSettings",
			"Engine",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"BlueprintGraph",
			"ContentBrowser",
			"GraphEditor",
			"HTTP",
			"InputCore",
			"Json",
			"JsonUtilities",
			"LevelEditor",
			"ModelContextProtocol",
			"ModelContextProtocolEngine",
			"Projects",
			"Settings",
			"Sockets",
			"Terminal",
			"ToolsetRegistry",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
