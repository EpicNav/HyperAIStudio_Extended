// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioLevelDesign : ModuleRules
{
	public HyperAIStudioLevelDesign(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EngineSettings",
			"HyperAIStudio",
			"ImageCore",
			"NavigationSystem",
			"RenderCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
