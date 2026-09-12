// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioWorldbuilding : ModuleRules
{
	public HyperAIStudioWorldbuilding(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// LoadingPhase=None is supplied by the owning descriptor. Water is intentionally
		// not linked: those variants stay capability-gated and are observed through the
		// already-running Asset Registry / loaded class state only.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"AssetRegistry",
			"Projects",
			"UnrealEd",
			"Landscape",
			"Foliage",
			"NavigationSystem"
		});
	}
}
