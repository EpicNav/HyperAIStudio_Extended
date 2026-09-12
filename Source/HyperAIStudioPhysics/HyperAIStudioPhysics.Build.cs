// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPhysics : ModuleRules
{
	public HyperAIStudioPhysics(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"PhysicsCore",
			"ToolsetRegistry"
		});
	}
}
