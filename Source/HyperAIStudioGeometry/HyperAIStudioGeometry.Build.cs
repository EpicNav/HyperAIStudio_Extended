// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioGeometry : ModuleRules
{
	public HyperAIStudioGeometry(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"GeometryScriptingCore",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
