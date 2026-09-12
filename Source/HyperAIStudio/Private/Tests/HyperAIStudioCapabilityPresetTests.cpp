// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioCapabilityPreset.h"
#include "HyperAIStudioService.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCapabilityPresetResolverTest,
	"HyperAIStudio.NativeTools.CapabilityPreset.Resolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCapabilityPresetResolverTest::RunTest(const FString& Parameters)
{
	const FHyperAIStudioPluginSyncDecision MissingDescriptor =
		FHyperAIStudioService::GetPluginSyncDecision(false, false);
	TestTrue(TEXT("A missing inactive wrapper is written once"), MissingDescriptor.bWriteProjectDescriptor);
	TestTrue(TEXT("A missing inactive wrapper requires restart"), MissingDescriptor.bRestartRequired);
	const FHyperAIStudioPluginSyncDecision PendingRestart =
		FHyperAIStudioService::GetPluginSyncDecision(false, true);
	TestFalse(TEXT("A persisted wrapper is not rewritten before restart"), PendingRestart.bWriteProjectDescriptor);
	TestTrue(TEXT("A persisted inactive wrapper still requires restart"), PendingRestart.bRestartRequired);
	const FHyperAIStudioPluginSyncDecision ActiveWrapper =
		FHyperAIStudioService::GetPluginSyncDecision(true, true);
	TestFalse(TEXT("An active wrapper is not rewritten"), ActiveWrapper.bWriteProjectDescriptor);
	TestFalse(TEXT("An active wrapper does not require restart"), ActiveWrapper.bRestartRequired);

	FHyperAIStudioStatus ReadyStatus;
	ReadyStatus.bUnrealMCPModuleAvailable = true;
	ReadyStatus.bUnrealMCPSettingsConfigured = true;
	ReadyStatus.bRequiredPluginsReady = true;
	ReadyStatus.bServerRunning = true;
	ReadyStatus.bPortListening = true;
	ReadyStatus.bToolsListReachable = true;
	ReadyStatus.bAgentFilesReady = true;
	ReadyStatus.ConfiguredAgentCount = 1;
	TestFalse(TEXT("Missing desired Epic toolsets cannot report full readiness"), ReadyStatus.IsReady());
	ReadyStatus.bDesiredCapabilityPluginsReady = true;
	TestTrue(TEXT("Full readiness includes synchronized desired Epic toolsets"), ReadyStatus.IsReady());

	TestEqual(TEXT("Persisted Core enum value remains stable"),
		static_cast<uint8>(EHyperAIStudioCapabilityPreset::Core), static_cast<uint8>(0));
	TestEqual(TEXT("Persisted Domains enum value remains stable"),
		static_cast<uint8>(EHyperAIStudioCapabilityPreset::Domains), static_cast<uint8>(1));
	TestEqual(TEXT("Persisted Everything enum value remains stable"),
		static_cast<uint8>(EHyperAIStudioCapabilityPreset::Everything), static_cast<uint8>(2));

	const TArray<FString> ExpectedFoundation = {
		TEXT("ModelContextProtocol"), TEXT("ToolsetRegistry"), TEXT("EditorToolset"),
		TEXT("MCPClientToolset"), TEXT("Terminal")
	};
	const TArray<FString>& Foundation = FHyperAIStudioCapabilityPresetResolver::GetFoundationPlugins();
	TestEqual(TEXT("Core foundation count is exact"), Foundation.Num(), ExpectedFoundation.Num());
	for (int32 Index = 0; Index < Foundation.Num() && Index < ExpectedFoundation.Num(); ++Index)
	{
		TestEqual(TEXT("Core foundation order and identity are exact"), Foundation[Index], ExpectedFoundation[Index]);
	}
	TestEqual(TEXT("Epic AllToolsets dependency list is exact"),
		FHyperAIStudioCapabilityPresetResolver::GetAllToolsetsDependencies().Num(), 21);

	const TArray<FHyperAIStudioAutomaticToolsetRule>& AutomaticRules =
		FHyperAIStudioCapabilityPresetResolver::GetAutomaticToolsetRules();
	TestEqual(TEXT("Automatic resolver covers AllToolsets plus standalone Epic wrappers"),
		AutomaticRules.Num(), 24);
	int32 UnconditionalRuleCount = 0;
	int32 PrerequisiteLinkCount = 0;
	TMap<FString, TArray<FString>> AutomaticRequirements;
	for (const FHyperAIStudioAutomaticToolsetRule& Rule : AutomaticRules)
	{
		AutomaticRequirements.Add(Rule.ToolsetPlugin, Rule.RequiredDomainPlugins);
		UnconditionalRuleCount += Rule.RequiredDomainPlugins.IsEmpty() ? 1 : 0;
		PrerequisiteLinkCount += Rule.RequiredDomainPlugins.Num();
	}
	TestEqual(TEXT("Automatic wrapper identities are unique"), AutomaticRequirements.Num(), 24);
	TestEqual(TEXT("Automatic foundation-only wrapper count is exact"), UnconditionalRuleCount, 7);
	TestEqual(TEXT("Automatic prerequisite link count is exact"), PrerequisiteLinkCount, 23);
	const auto TestRequirements = [this, &AutomaticRequirements](
		const TCHAR* Toolset,
		const TArray<FString>& Expected)
	{
		const TArray<FString>* Actual = AutomaticRequirements.Find(Toolset);
		TestNotNull(*FString::Printf(TEXT("Rule exists for %s"), Toolset), Actual);
		if (Actual != nullptr)
		{
			TestEqual(*FString::Printf(TEXT("Prerequisite count is exact for %s"), Toolset),
				Actual->Num(), Expected.Num());
			for (int32 Index = 0; Index < Actual->Num() && Index < Expected.Num(); ++Index)
			{
				TestEqual(*FString::Printf(TEXT("Prerequisite is exact for %s"), Toolset),
					(*Actual)[Index], Expected[Index]);
			}
		}
	};
	TestRequirements(TEXT("AIModuleToolset"), {});
	TestRequirements(TEXT("AnimationAssistantToolset"),
		{ TEXT("ControlRig"), TEXT("LevelSequenceEditor"), TEXT("SequencerScripting") });
	TestRequirements(TEXT("AutomationTestToolset"), {});
	TestRequirements(TEXT("ChaosClothAssetToolset"),
		{ TEXT("ChaosClothAsset"), TEXT("ChaosClothAssetEditorCore") });
	TestRequirements(TEXT("ConfigSettingsToolset"), {});
	TestRequirements(TEXT("ConversationToolset"), { TEXT("CommonConversation") });
	TestRequirements(TEXT("DataRegistryToolset"), { TEXT("DataRegistry") });
	TestRequirements(TEXT("DataflowAgent"), { TEXT("GeometryCollectionPlugin") });
	TestRequirements(TEXT("GameFeaturesToolset"), { TEXT("GameFeatures") });
	TestRequirements(TEXT("GameplayTagsToolset"), { TEXT("GameplayTagsEditor") });
	TestRequirements(TEXT("GASToolsets"), { TEXT("GameplayAbilities") });
	TestRequirements(TEXT("LiveCodingToolset"), {});
	TestRequirements(TEXT("MetaHumanGenerator"),
		{ TEXT("MetaHumanCharacter"), TEXT("MetaHumanCoreTech"), TEXT("MetaHumanSDK"), TEXT("StructUtils") });
	TestRequirements(TEXT("MVVMToolset"), { TEXT("ModelViewViewModel") });
	TestRequirements(TEXT("NiagaraToolsets"), { TEXT("Niagara") });
	TestRequirements(TEXT("PCGToolset"), { TEXT("PCG") });
	TestRequirements(TEXT("PhysicsToolsets"), {});
	TestRequirements(TEXT("PluginToolset"), { TEXT("PluginUtils") });
	TestRequirements(TEXT("SemanticSearchToolset"), { TEXT("SemanticSearch") });
	TestRequirements(TEXT("SequencerAnimMixerToolset"), { TEXT("MovieSceneAnimMixer") });
	TestRequirements(TEXT("SlateInspectorToolset"), {});
	TestRequirements(TEXT("StateTreeToolset"), { TEXT("StateTree") });
	TestRequirements(TEXT("UMGToolSet"), {});
	TestRequirements(TEXT("WorldConditionsToolset"), { TEXT("WorldConditions") });

	UHyperAIStudioSettings* Settings = NewObject<UHyperAIStudioSettings>();
	Settings->CapabilityPreset = EHyperAIStudioCapabilityPreset::Core;
	TSet<FString> EnabledPlugins;
	TArray<FString> Desired =
		FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, EnabledPlugins);
	TestEqual(TEXT("Recommended preset includes foundations plus seven foundation-only wrappers"), Desired.Num(), 12);
	TestTrue(TEXT("Recommended preset includes foundation-only UMG wrapper"), Desired.Contains(TEXT("UMGToolSet")));
	TestTrue(TEXT("Recommended preset includes foundation-only Live Coding wrapper"),
		Desired.Contains(TEXT("LiveCodingToolset")));
	TestFalse(TEXT("Recommended preset does not imply AllToolsets"), Desired.Contains(TEXT("AllToolsets")));
	TestFalse(TEXT("Recommended preset skips inactive Niagara"), Desired.Contains(TEXT("NiagaraToolsets")));
	TestFalse(TEXT("Recommended preset skips inactive PCG"), Desired.Contains(TEXT("PCGToolset")));
	TestFalse(TEXT("Recommended preset skips inactive StateTree"), Desired.Contains(TEXT("StateTreeToolset")));

	EnabledPlugins = { TEXT("Niagara"), TEXT("PCG"), TEXT("StateTree"), TEXT("GameplayAbilities") };
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, EnabledPlugins);
	TestEqual(TEXT("Four active domain plugins add exactly four matching wrappers"), Desired.Num(), 16);
	TestTrue(TEXT("Active Niagara enables its wrapper"), Desired.Contains(TEXT("NiagaraToolsets")));
	TestTrue(TEXT("Active PCG enables its wrapper"), Desired.Contains(TEXT("PCGToolset")));
	TestTrue(TEXT("Active StateTree enables its wrapper"), Desired.Contains(TEXT("StateTreeToolset")));
	TestTrue(TEXT("Active GameplayAbilities enables its wrapper"), Desired.Contains(TEXT("GASToolsets")));
	TestFalse(TEXT("Inactive GameFeatures remains disabled"), Desired.Contains(TEXT("GameFeaturesToolset")));

	EnabledPlugins = { TEXT("ModelViewViewModel") };
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, EnabledPlugins);
	TestTrue(TEXT("Active MVVM domain plugin enables its standalone wrapper"),
		Desired.Contains(TEXT("MVVMToolset")));
	TestFalse(TEXT("Inactive Chaos Cloth prerequisites do not enable its wrapper"),
		Desired.Contains(TEXT("ChaosClothAssetToolset")));

	EnabledPlugins = { TEXT("ControlRig"), TEXT("LevelSequenceEditor") };
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, EnabledPlugins);
	TestFalse(TEXT("Animation wrapper waits for every descriptor prerequisite"),
		Desired.Contains(TEXT("AnimationAssistantToolset")));
	EnabledPlugins.Add(TEXT("SequencerScripting"));
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, EnabledPlugins);
	TestTrue(TEXT("Animation wrapper activates when every descriptor prerequisite is enabled"),
		Desired.Contains(TEXT("AnimationAssistantToolset")));

	const TArray<FString> AutomaticEnablePlan =
		FHyperAIStudioCapabilityPresetResolver::ResolvePluginsToEnable(*Settings, { TEXT("Niagara") });
	TestTrue(TEXT("Automatic enable plan includes matching wrapper"),
		AutomaticEnablePlan.Contains(TEXT("NiagaraToolsets")));
	TestFalse(TEXT("Automatic enable plan never adds the domain prerequisite itself"),
		AutomaticEnablePlan.Contains(TEXT("Niagara")));

	Settings->CapabilityPreset = EHyperAIStudioCapabilityPreset::Domains;
	Settings->EnabledCapabilityDomains = {
		EHyperAIStudioCapabilityDomain::Gameplay,
		EHyperAIStudioCapabilityDomain::Gameplay,
		EHyperAIStudioCapabilityDomain::Worldbuilding
	};
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, { TEXT("Niagara") });
	TestTrue(TEXT("Selected gameplay domain is included"), Desired.Contains(TEXT("StateTreeToolset")));
	TestTrue(TEXT("Selected worldbuilding domain is included"), Desired.Contains(TEXT("PCGToolset")));
	TestFalse(TEXT("Selected Domains ignores active but unselected VFX plugins"),
		Desired.Contains(TEXT("NiagaraToolsets")));
	TestEqual(TEXT("Duplicate domain selection does not duplicate plugins"),
		Desired.FilterByPredicate([](const FString& Name) { return Name == TEXT("StateTreeToolset"); }).Num(), 1);

	Settings->CapabilityPreset = EHyperAIStudioCapabilityPreset::Everything;
	Settings->EnabledCapabilityDomains.Reset();
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings);
	TestEqual(TEXT("Everything is foundation + aggregator + exact dependency union"), Desired.Num(), 25);
	TestTrue(TEXT("Everything includes the aggregator"), Desired.Contains(TEXT("AllToolsets")));
	const TArray<FString> EverythingEnablePlan =
		FHyperAIStudioCapabilityPresetResolver::ResolvePluginsToEnable(*Settings, {});
	TestEqual(TEXT("Everything enables foundations plus one aggregator only"), EverythingEnablePlan.Num(), 6);
	TestTrue(TEXT("Everything enable plan contains the aggregator"), EverythingEnablePlan.Contains(TEXT("AllToolsets")));
	TestFalse(TEXT("Everything enable plan does not write each transitive dependency"), EverythingEnablePlan.Contains(TEXT("AIModuleToolset")));
	for (const FString& OptionalExtra : FHyperAIStudioCapabilityPresetResolver::GetOptionalExtraPlugins())
	{
		TestFalse(TEXT("Everything never implies standalone optional extras"), Desired.Contains(OptionalExtra));
	}

	Settings->bEnableMVVMToolset = true;
	Desired = FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings, {});
	TestTrue(TEXT("An explicit optional extra is included"), Desired.Contains(TEXT("MVVMToolset")));
	TestFalse(TEXT("One optional extra does not imply another"), Desired.Contains(TEXT("MetaHumanGenerator")));

	TSet<FString> Unique;
	for (const FString& PluginName : Desired)
	{
		Unique.Add(PluginName);
	}
	TestEqual(TEXT("Resolved plugin names are unique"), Unique.Num(), Desired.Num());
	return true;
}

#endif
