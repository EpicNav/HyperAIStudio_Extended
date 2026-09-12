// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAudio : ModuleRules
{
	public HyperAIStudioAudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The descriptor owns LoadingPhase=None. The MetaSound Frontend and its narrow
		// GraphCore value/type API are linked so persisted documents can be inspected
		// and validated through Epic's public interface. Every other audio plugin remains optional,
		// name-projected, and must already be enabled and loaded for its typed variants.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"AssetRegistry",
			"Projects",
			"MetasoundFrontend",
			// Direct link ownership for Metasound::FLiteral and the polymorphic
			// type helpers used by the document validator. MetasoundFrontend
			// exposes their headers transitively, but UBT does not infer this
			// module's import library from those includes.
			"MetasoundGraphCore"
		});
	}
}
