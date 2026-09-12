// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioTextureGraph : ModuleRules
{
	public HyperAIStudioTextureGraph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Texture Graph's public headers pull in continuable and std::function code built with exceptions.
		bEnableExceptions = true;
		bDisableAutoRTFMInstrumentation = true;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"RenderCore",
			"TextureGraph",
			"TextureGraphEngine",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
