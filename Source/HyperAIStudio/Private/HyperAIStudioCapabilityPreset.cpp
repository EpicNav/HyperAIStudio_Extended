// Games by Hyper 2026.

#include "HyperAIStudioCapabilityPreset.h"

#include "Interfaces/IPluginManager.h"

namespace HyperAIStudio::CapabilityPreset::Private
{
	void AddUniqueOrdered(TArray<FString>& Target, const TArray<FString>& Values)
	{
		for (const FString& Value : Values)
		{
			Target.AddUnique(Value);
		}
	}

	TSet<FString> GetEnabledPluginNames()
	{
		TSet<FString> Result;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			Result.Add(Plugin->GetName());
		}
		return Result;
	}

	bool AreRequiredDomainPluginsEnabled(
		const FHyperAIStudioAutomaticToolsetRule& Rule,
		const TSet<FString>& EnabledPlugins)
	{
		for (const FString& RequiredPlugin : Rule.RequiredDomainPlugins)
		{
			if (!EnabledPlugins.Contains(RequiredPlugin))
			{
				return false;
			}
		}
		return true;
	}

	void AddAutomaticProjectToolsets(TArray<FString>& Target, const TSet<FString>& EnabledPlugins)
	{
		for (const FHyperAIStudioAutomaticToolsetRule& Rule
			: FHyperAIStudioCapabilityPresetResolver::GetAutomaticToolsetRules())
		{
			if (AreRequiredDomainPluginsEnabled(Rule, EnabledPlugins))
			{
				Target.AddUnique(Rule.ToolsetPlugin);
			}
		}
	}
}

const TArray<FString>& FHyperAIStudioCapabilityPresetResolver::GetFoundationPlugins()
{
	static const TArray<FString> Plugins = {
		TEXT("ModelContextProtocol"),
		TEXT("ToolsetRegistry"),
		TEXT("EditorToolset"),
		TEXT("MCPClientToolset"),
		TEXT("Terminal")
	};
	return Plugins;
}

const TArray<FString>& FHyperAIStudioCapabilityPresetResolver::GetAllToolsetsDependencies()
{
	static const TArray<FString> Plugins = {
		TEXT("AIModuleToolset"),
		TEXT("AnimationAssistantToolset"),
		TEXT("AutomationTestToolset"),
		TEXT("ConfigSettingsToolset"),
		TEXT("ConversationToolset"),
		TEXT("DataRegistryToolset"),
		TEXT("DataflowAgent"),
		TEXT("EditorToolset"),
		TEXT("GameFeaturesToolset"),
		TEXT("GameplayTagsToolset"),
		TEXT("GASToolsets"),
		TEXT("MCPClientToolset"),
		TEXT("NiagaraToolsets"),
		TEXT("PCGToolset"),
		TEXT("PhysicsToolsets"),
		TEXT("PluginToolset"),
		TEXT("SemanticSearchToolset"),
		TEXT("SlateInspectorToolset"),
		TEXT("StateTreeToolset"),
		TEXT("UMGToolSet"),
		TEXT("WorldConditionsToolset")
	};
	return Plugins;
}

const TArray<FString>& FHyperAIStudioCapabilityPresetResolver::GetOptionalExtraPlugins()
{
	static const TArray<FString> Plugins = {
		TEXT("ChaosClothAssetToolset"),
		TEXT("LiveCodingToolset"),
		TEXT("MetaHumanGenerator"),
		TEXT("MVVMToolset"),
		TEXT("SequencerAnimMixerToolset")
	};
	return Plugins;
}

const TArray<FHyperAIStudioAutomaticToolsetRule>&
FHyperAIStudioCapabilityPresetResolver::GetAutomaticToolsetRules()
{
	// These requirements mirror the non-foundation `Plugins` dependencies in Epic's UE 5.8
	// toolset descriptors. Empty requirements mean the toolset only depends on the foundation.
	static const TArray<FHyperAIStudioAutomaticToolsetRule> Rules = {
		{ TEXT("AIModuleToolset"), {} },
		{ TEXT("AnimationAssistantToolset"),
			{ TEXT("ControlRig"), TEXT("LevelSequenceEditor"), TEXT("SequencerScripting") } },
		{ TEXT("AutomationTestToolset"), {} },
		{ TEXT("ChaosClothAssetToolset"),
			{ TEXT("ChaosClothAsset"), TEXT("ChaosClothAssetEditorCore") } },
		{ TEXT("ConfigSettingsToolset"), {} },
		{ TEXT("ConversationToolset"), { TEXT("CommonConversation") } },
		{ TEXT("DataRegistryToolset"), { TEXT("DataRegistry") } },
		{ TEXT("DataflowAgent"), { TEXT("GeometryCollectionPlugin") } },
		{ TEXT("GameFeaturesToolset"), { TEXT("GameFeatures") } },
		{ TEXT("GameplayTagsToolset"), { TEXT("GameplayTagsEditor") } },
		{ TEXT("GASToolsets"), { TEXT("GameplayAbilities") } },
		{ TEXT("LiveCodingToolset"), {} },
		{ TEXT("MetaHumanGenerator"),
			{ TEXT("MetaHumanCharacter"), TEXT("MetaHumanCoreTech"), TEXT("MetaHumanSDK"), TEXT("StructUtils") } },
		{ TEXT("MVVMToolset"), { TEXT("ModelViewViewModel") } },
		{ TEXT("NiagaraToolsets"), { TEXT("Niagara") } },
		{ TEXT("PCGToolset"), { TEXT("PCG") } },
		{ TEXT("PhysicsToolsets"), {} },
		{ TEXT("PluginToolset"), { TEXT("PluginUtils") } },
		{ TEXT("SemanticSearchToolset"), { TEXT("SemanticSearch") } },
		{ TEXT("SequencerAnimMixerToolset"), { TEXT("MovieSceneAnimMixer") } },
		{ TEXT("SlateInspectorToolset"), {} },
		{ TEXT("StateTreeToolset"), { TEXT("StateTree") } },
		{ TEXT("UMGToolSet"), {} },
		{ TEXT("WorldConditionsToolset"), { TEXT("WorldConditions") } }
	};
	return Rules;
}

TArray<FString> FHyperAIStudioCapabilityPresetResolver::GetDomainPlugins(
	const EHyperAIStudioCapabilityDomain Domain)
{
	switch (Domain)
	{
	case EHyperAIStudioCapabilityDomain::Animation:
		return { TEXT("AnimationAssistantToolset") };
	case EHyperAIStudioCapabilityDomain::VfxAndSimulation:
		return { TEXT("NiagaraToolsets"), TEXT("DataflowAgent"), TEXT("PhysicsToolsets") };
	case EHyperAIStudioCapabilityDomain::Worldbuilding:
		return { TEXT("PCGToolset"), TEXT("WorldConditionsToolset") };
	case EHyperAIStudioCapabilityDomain::Gameplay:
		return {
			TEXT("AIModuleToolset"), TEXT("ConversationToolset"), TEXT("DataRegistryToolset"),
			TEXT("GameFeaturesToolset"), TEXT("GameplayTagsToolset"), TEXT("GASToolsets"),
			TEXT("StateTreeToolset")
		};
	case EHyperAIStudioCapabilityDomain::UiAndTest:
		return { TEXT("UMGToolSet"), TEXT("SlateInspectorToolset"), TEXT("AutomationTestToolset") };
	case EHyperAIStudioCapabilityDomain::ProjectAndPipeline:
		return { TEXT("ConfigSettingsToolset"), TEXT("PluginToolset"), TEXT("SemanticSearchToolset") };
	default:
		return {};
	}
}

TArray<FString> FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(
	const UHyperAIStudioSettings& Settings)
{
	return ResolveDesiredPlugins(Settings, HyperAIStudio::CapabilityPreset::Private::GetEnabledPluginNames());
}

TArray<FString> FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(
	const UHyperAIStudioSettings& Settings,
	const TSet<FString>& EnabledPlugins)
{
	using namespace HyperAIStudio::CapabilityPreset::Private;
	TArray<FString> Result = GetFoundationPlugins();

	if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Core)
	{
		AddAutomaticProjectToolsets(Result, EnabledPlugins);
	}
	else if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Domains)
	{
		for (const EHyperAIStudioCapabilityDomain Domain : Settings.EnabledCapabilityDomains)
		{
			AddUniqueOrdered(Result, GetDomainPlugins(Domain));
		}
	}
	else if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Everything)
	{
		Result.AddUnique(TEXT("AllToolsets"));
		AddUniqueOrdered(Result, GetAllToolsetsDependencies());
	}

	if (Settings.bEnableChaosClothAssetToolset) Result.AddUnique(TEXT("ChaosClothAssetToolset"));
	if (Settings.bEnableLiveCodingToolset) Result.AddUnique(TEXT("LiveCodingToolset"));
	if (Settings.bEnableMetaHumanGenerator) Result.AddUnique(TEXT("MetaHumanGenerator"));
	if (Settings.bEnableMVVMToolset) Result.AddUnique(TEXT("MVVMToolset"));
	if (Settings.bEnableSequencerAnimMixerToolset) Result.AddUnique(TEXT("SequencerAnimMixerToolset"));

	return Result;
}

TArray<FString> FHyperAIStudioCapabilityPresetResolver::ResolvePluginsToEnable(
	const UHyperAIStudioSettings& Settings)
{
	return ResolvePluginsToEnable(Settings, HyperAIStudio::CapabilityPreset::Private::GetEnabledPluginNames());
}

TArray<FString> FHyperAIStudioCapabilityPresetResolver::ResolvePluginsToEnable(
	const UHyperAIStudioSettings& Settings,
	const TSet<FString>& EnabledPlugins)
{
	using namespace HyperAIStudio::CapabilityPreset::Private;
	TArray<FString> Result = GetFoundationPlugins();
	if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Core)
	{
		AddAutomaticProjectToolsets(Result, EnabledPlugins);
	}
	else if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Domains)
	{
		for (const EHyperAIStudioCapabilityDomain Domain : Settings.EnabledCapabilityDomains)
		{
			AddUniqueOrdered(Result, GetDomainPlugins(Domain));
		}
	}
	else if (Settings.CapabilityPreset == EHyperAIStudioCapabilityPreset::Everything)
	{
		Result.AddUnique(TEXT("AllToolsets"));
	}

	if (Settings.bEnableChaosClothAssetToolset) Result.AddUnique(TEXT("ChaosClothAssetToolset"));
	if (Settings.bEnableLiveCodingToolset) Result.AddUnique(TEXT("LiveCodingToolset"));
	if (Settings.bEnableMetaHumanGenerator) Result.AddUnique(TEXT("MetaHumanGenerator"));
	if (Settings.bEnableMVVMToolset) Result.AddUnique(TEXT("MVVMToolset"));
	if (Settings.bEnableSequencerAnimMixerToolset) Result.AddUnique(TEXT("SequencerAnimMixerToolset"));
	return Result;
}
