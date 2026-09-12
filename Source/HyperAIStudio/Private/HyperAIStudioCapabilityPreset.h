// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioSettings.h"

/** Epic toolset wrapper and the already-enabled domain plugins it requires in automatic mode. */
struct FHyperAIStudioAutomaticToolsetRule
{
	FString ToolsetPlugin;
	TArray<FString> RequiredDomainPlugins;
};

/** UE 5.8 plugin-set resolver. It never enables or disables plugins. */
class FHyperAIStudioCapabilityPresetResolver final
{
public:
	static const TArray<FString>& GetFoundationPlugins();
	static const TArray<FString>& GetAllToolsetsDependencies();
	static const TArray<FString>& GetOptionalExtraPlugins();
	static const TArray<FHyperAIStudioAutomaticToolsetRule>& GetAutomaticToolsetRules();
	static TArray<FString> GetDomainPlugins(EHyperAIStudioCapabilityDomain Domain);
	static TArray<FString> ResolveDesiredPlugins(const UHyperAIStudioSettings& Settings);
	/** Pure overload used to resolve the recommended project-plugin preset without changing prerequisites. */
	static TArray<FString> ResolveDesiredPlugins(
		const UHyperAIStudioSettings& Settings,
		const TSet<FString>& EnabledPlugins);
	/** Minimal descriptors to edit. Everything enables the aggregator, not 21 independent project entries. */
	static TArray<FString> ResolvePluginsToEnable(const UHyperAIStudioSettings& Settings);
	/** Pure overload used by tests and callers that already have the enabled plugin set. */
	static TArray<FString> ResolvePluginsToEnable(
		const UHyperAIStudioSettings& Settings,
		const TSet<FString>& EnabledPlugins);
};
