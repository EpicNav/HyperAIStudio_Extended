// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioNetworking : ModuleRules
{
	public HyperAIStudioNetworking(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Only always-present UE editor/runtime surfaces are linked. OnlineSubsystem providers
		// are observed only when already loaded through Engine's no-load interface seam.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EngineSettings",
			"AssetRegistry",
			"BlueprintGraph",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
