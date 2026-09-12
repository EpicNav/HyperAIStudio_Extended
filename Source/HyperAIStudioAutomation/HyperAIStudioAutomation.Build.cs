// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAutomation : ModuleRules
{
	public HyperAIStudioAutomation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AutomationController",
			"HyperAIStudio",
			"ToolsetRegistry"
		});
	}
}
