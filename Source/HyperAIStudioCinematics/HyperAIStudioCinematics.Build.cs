// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioCinematics : ModuleRules
{
	public HyperAIStudioCinematics(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
			"Sequencer",
			"MovieRenderPipelineCore",
			"ToolsetRegistry"
		});
	}
}
