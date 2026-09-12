// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioLiveProduction : ModuleRules
{
	public HyperAIStudioLiveProduction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately no LiveLink, RemoteControl, DMX, Avalanche, or nDisplay
		// dependency. The generated pack gate is any_of, so a monolithic adapter
		// may not assume any one optional family is present.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
