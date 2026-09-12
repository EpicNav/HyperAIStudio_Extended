// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioNiagara : ModuleRules
{
	public HyperAIStudioNiagara(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"Niagara",
			"NiagaraEditor",
			"ToolsetRegistry"
		});
	}
}
