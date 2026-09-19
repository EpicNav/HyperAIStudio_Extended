// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioLighting : ModuleRules
{
	public HyperAIStudioLighting(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ImageCore",
			"RenderCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
