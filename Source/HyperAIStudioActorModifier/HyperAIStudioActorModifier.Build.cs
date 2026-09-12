// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioActorModifier : ModuleRules
{
	public HyperAIStudioActorModifier(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"ActorModifierCore",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
