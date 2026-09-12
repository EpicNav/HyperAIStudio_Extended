// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioGameplayAI : ModuleRules
{
	public HyperAIStudioGameplayAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This is the explicitly loaded base facade only. StateTree, EQS, SmartObjects,
		// GameplayBehaviors, and GameplayInteractions stay in prerequisite-gated future
		// variant modules and are not linked or advertised as implemented here.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"AIModule",
			"AIGraph",
			"BehaviorTreeEditor",
			"UnrealEd",
			"Projects"
		});
	}
}
