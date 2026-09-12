// Games by Hyper 2026.

#include "HyperAIStudioNiagaraToolset.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraValidationRuleSet.h"
#include "NiagaraValidationRules.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNiagara, Log, All);

namespace HyperAIStudio::Niagara::Private
{
	constexpr int32 MaxParameterDataBytes = 64 * 1024;
	constexpr int32 MaxIssueSummaryChars = 512;
	constexpr int32 MaxIssueDescriptionChars = 1024;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	FString ObjectIdentity(const UObject* Object)
	{
		if (!Object)
		{
			return TEXT("null");
		}
		return Object->GetClass()->GetPathName() + TEXT("|") + Object->GetPathName();
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ') || Character == 0x7f)
			{
				return true;
			}
		}
		return false;
	}

	FString TypeId(const FNiagaraTypeDefinition& Type)
	{
		const UObject* TypeObject = Type.GetClass()
			? static_cast<const UObject*>(Type.GetClass())
			: Type.GetStruct()
				? static_cast<const UObject*>(Type.GetStruct())
				: static_cast<const UObject*>(Type.GetEnum());
		return Type.GetFName().ToString() + TEXT("|") + ObjectIdentity(TypeObject);
	}

	FString TypeFingerprint(const FNiagaraTypeDefinition& Type)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.type.v1"));
		AppendToken(Canonical, TypeId(Type));
		AppendToken(Canonical, Type.IsDataInterface() ? TEXT("data_interface") : TEXT("not_data_interface"));
		AppendToken(Canonical, Type.IsUObject() ? TEXT("uobject") : TEXT("not_uobject"));
		AppendToken(Canonical, FString::FromInt(Type.GetSize()));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString DefaultFingerprint(
		const FNiagaraUserRedirectionParameterStore& Store,
		const FNiagaraVariable& Variable,
		bool& bOutComplete)
	{
		bOutComplete = true;
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.user-default.v1"));
		AppendToken(Canonical, Variable.GetName().ToString());
		AppendToken(Canonical, TypeFingerprint(Variable.GetType()));
		if (Variable.GetType().IsDataInterface())
		{
			AppendToken(Canonical, ObjectIdentity(Store.GetDataInterface(Variable)));
		}
		else if (Variable.GetType().IsUObject())
		{
			AppendToken(Canonical, ObjectIdentity(Store.GetUObject(Variable)));
		}
		else
		{
			const int32 Size = Variable.GetSizeInBytes();
			const uint8* Data = Store.GetParameterData(Variable);
			if (Size < 0 || Size > MaxParameterDataBytes || (Size > 0 && !Data))
			{
				bOutComplete = false;
				return FString();
			}
			AppendToken(Canonical, Size > 0 ? BytesToHex(Data, Size) : FString());
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	void AddIssue(
		TArray<FHyperAINiagaraIssue>& Issues,
		const FString& Code,
		const FString& Severity,
		const FString& Summary,
		const FString& Description = FString(),
		const FString& RuleClass = FString(),
		const FString& SourcePath = FString())
	{
		if (Issues.Num() >= FHyperAIStudioNiagaraContracts::MaxIssues)
		{
			return;
		}
		FHyperAINiagaraIssue Issue;
		Issue.Code = Code.Left(96);
		Issue.Severity = Severity;
		Issue.RuleClass = RuleClass.Left(256);
		Issue.SourcePath = SourcePath.Left(FHyperAIStudioNiagaraContracts::MaxPathCharacters);
		Issue.Summary = Summary.Left(MaxIssueSummaryChars);
		Issue.Description = Description.Left(MaxIssueDescriptionChars);
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.issue.v1"));
		AppendToken(Canonical, Issue.Code);
		AppendToken(Canonical, Issue.Severity);
		AppendToken(Canonical, Issue.RuleClass);
		AppendToken(Canonical, Issue.SourcePath);
		AppendToken(Canonical, Issue.Summary);
		AppendToken(Canonical, Issue.Description);
		Issue.StableId = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		Issues.Add(MoveTemp(Issue));
	}

	int32 EstimateIssueBytes(const FHyperAINiagaraIssue& Issue)
	{
		return 256 + 2 * (Issue.Code.Len() + Issue.Severity.Len() + Issue.StableId.Len()
			+ Issue.RuleClass.Len() + Issue.SourcePath.Len() + Issue.Summary.Len()
			+ Issue.Description.Len());
	}

	int32 EstimateParameterBytes(const FHyperAINiagaraUserParameterIdentity& Parameter)
	{
		return 256 + 2 * (Parameter.VariableGuid.Len() + Parameter.Name.Len()
			+ Parameter.TypeId.Len() + Parameter.TypeFingerprint.Len()
			+ Parameter.DefaultFingerprint.Len() + Parameter.MetadataChangeId.Len());
	}

	int32 EstimateCapabilityBytes(const FHyperAINiagaraCapabilityStatus& Capability)
	{
		int64 Size = 256 + 2ll * Capability.Family.Len() + 2ll * Capability.State.Len()
			+ 2ll * Capability.Remediation.Len();
		for (const FString& Item : Capability.SupportedCases) Size += 32 + 2ll * Item.Len();
		for (const FString& Item : Capability.DelegatedEpicCallables) Size += 32 + 2ll * Item.Len();
		for (const FString& Item : Capability.UnsupportedCases) Size += 32 + 2ll * Item.Len();
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateCapabilitiesBytes(const TArray<FHyperAINiagaraCapabilityStatus>& Capabilities)
	{
		int64 Size = 32;
		for (const FHyperAINiagaraCapabilityStatus& Capability : Capabilities)
		{
			Size += EstimateCapabilityBytes(Capability);
		}
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateHealthBytes(const FHyperAINiagaraHealthRecord& Health)
	{
		const int64 Size = 512 + 2ll * Health.TargetPath.Len() + 2ll * Health.ClassPath.Len()
			+ 2ll * Health.Revision.Len() + 2ll * Health.DiskExistence.Len()
			+ 2ll * Health.PackageSavedHash.Len() + 2ll * Health.AssetGuid.Len();
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	FString CanonicalGuid(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
	}

	FNiagaraEditorModule* GetLoadedNiagaraEditorModule()
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor")))
		{
			return nullptr;
		}
		return FModuleManager::Get().GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
	}

	TSharedPtr<FNiagaraSystemViewModel> GetExistingSystemViewModel(UNiagaraSystem* System)
	{
		FNiagaraEditorModule* EditorModule = GetLoadedNiagaraEditorModule();
		return EditorModule && System
			? EditorModule->GetExistingViewModelForSystem(System)
			: TSharedPtr<FNiagaraSystemViewModel>();
	}

	bool AddScriptBounded(
		UNiagaraScript* Script,
		TSet<UNiagaraScript*>& Seen,
		TArray<UNiagaraScript*>& Scripts)
	{
		if (!Script || Seen.Contains(Script))
		{
			return true;
		}
		if (Scripts.Num() >= FHyperAIStudioNiagaraContracts::MaxScripts)
		{
			return false;
		}
		Seen.Add(Script);
		Scripts.Add(Script);
		return true;
	}

	bool HasAuthoritativeCompileStatus(const ENiagaraScriptUsage Usage)
	{
		switch (Usage)
		{
		case ENiagaraScriptUsage::SystemSpawnScript:
		case ENiagaraScriptUsage::SystemUpdateScript:
		case ENiagaraScriptUsage::ParticleSpawnScript:
		case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated:
		case ENiagaraScriptUsage::ParticleUpdateScript:
		case ENiagaraScriptUsage::ParticleEventScript:
		case ENiagaraScriptUsage::ParticleSimulationStageScript:
		case ENiagaraScriptUsage::ParticleGPUComputeScript:
			return true;
		default:
			return false;
		}
	}

	bool CollectReachableScripts(
		UNiagaraSystem& System,
		TArray<UNiagaraScript*>& OutScripts)
	{
		TSet<UNiagaraScript*> Seen;
		if (!AddScriptBounded(System.GetSystemSpawnScript(), Seen, OutScripts)
			|| !AddScriptBounded(System.GetSystemUpdateScript(), Seen, OutScripts)
			|| System.ScratchPadScripts.Num()
				> FHyperAIStudioNiagaraContracts::MaxScripts - OutScripts.Num())
		{
			return false;
		}
		for (UNiagaraScript* Script : System.ScratchPadScripts)
		{
			if (!AddScriptBounded(Script, Seen, OutScripts)) return false;
		}
		if (System.GetEmitterHandles().Num() > FHyperAIStudioNiagaraContracts::MaxEmitters)
		{
			return false;
		}
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (!Data)
			{
				return false;
			}
			if (!AddScriptBounded(Data->EmitterSpawnScriptProps.Script, Seen, OutScripts)
				|| !AddScriptBounded(Data->EmitterUpdateScriptProps.Script, Seen, OutScripts)
				|| !AddScriptBounded(Data->SpawnScriptProps.Script, Seen, OutScripts)
				|| !AddScriptBounded(Data->UpdateScriptProps.Script, Seen, OutScripts)
				|| !AddScriptBounded(Data->GetGPUComputeScript(), Seen, OutScripts)
				|| Data->EventHandlerScriptProps.Num()
					> FHyperAIStudioNiagaraContracts::MaxScripts - OutScripts.Num())
			{
				return false;
			}
			for (const FNiagaraEventScriptProperties& Event : Data->EventHandlerScriptProps)
			{
				if (!AddScriptBounded(Event.Script, Seen, OutScripts)) return false;
			}
			if (Data->GetSimulationStages().Num()
				> FHyperAIStudioNiagaraContracts::MaxScripts - OutScripts.Num())
			{
				return false;
			}
			for (UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
			{
				if (!Stage || !AddScriptBounded(Stage->Script, Seen, OutScripts)) return false;
			}
			if (Data->ScratchPads)
			{
				if (Data->ScratchPads->Scripts.Num()
					> FHyperAIStudioNiagaraContracts::MaxScripts - OutScripts.Num())
				{
					return false;
				}
				for (UNiagaraScript* Script : Data->ScratchPads->Scripts)
				{
					if (!AddScriptBounded(Script, Seen, OutScripts)) return false;
				}
			}
			if (Data->ParentScratchPads)
			{
				if (Data->ParentScratchPads->Scripts.Num()
					> FHyperAIStudioNiagaraContracts::MaxScripts - OutScripts.Num())
				{
					return false;
				}
				for (UNiagaraScript* Script : Data->ParentScratchPads->Scripts)
				{
					if (!AddScriptBounded(Script, Seen, OutScripts)) return false;
				}
			}
		}
		return true;
	}

	bool CollectReachableGraphs(
		const TArray<UNiagaraScript*>& Scripts,
		TArray<UNiagaraGraph*>& OutGraphs,
		int32& OutNodeCount)
	{
		TSet<UNiagaraGraph*> Seen;
		OutNodeCount = 0;
		int32 MetadataCount = 0;
		int32 PinCount = 0;
		for (UNiagaraScript* Script : Scripts)
		{
			const UNiagaraScriptSource* Source = Script
				? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
			UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
			if (!Graph || Seen.Contains(Graph))
			{
				continue;
			}
			const int32 GraphMetadataCount = Graph->GetAllMetaData().Num();
			if (OutGraphs.Num() >= FHyperAIStudioNiagaraContracts::MaxGraphs
				|| Graph->Nodes.Num() > FHyperAIStudioNiagaraContracts::MaxGraphNodes - OutNodeCount
				|| GraphMetadataCount > FHyperAIStudioNiagaraContracts::MaxGraphMetadata
					- MetadataCount)
			{
				return false;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const int32 NodePinCount = Node ? Node->Pins.Num() : 0;
				if (NodePinCount > FHyperAIStudioNiagaraContracts::MaxGraphPins - PinCount)
				{
					return false;
				}
				PinCount += NodePinCount;
			}
			Seen.Add(Graph);
			OutGraphs.Add(Graph);
			OutNodeCount += Graph->Nodes.Num();
			MetadataCount += GraphMetadataCount;
		}
		return true;
	}

	bool IsNativeValidationRuleClass(const UClass* Class)
	{
		static const TSet<const UClass*> Allowed = {
			UNiagaraValidationRule_NoWarmupTime::StaticClass(),
			UNiagaraValidationRule_NoEvents::StaticClass(),
			UNiagaraValidationRule_FixedGPUBoundsSet::StaticClass(),
			UNiagaraValidationRule_EmitterCount::StaticClass(),
			UNiagaraValidationRule_RendererCount::StaticClass(),
			UNiagaraValidationRule_BannedRenderers::StaticClass(),
			UNiagaraValidationRule_Lightweight::StaticClass(),
			UNiagaraValidationRule_BannedModules::StaticClass(),
			UNiagaraValidationRule_BannedDataInterfaces::StaticClass(),
			UNiagaraValidationRule_RendererSortingEnabled::StaticClass(),
			UNiagaraValidationRule_GpuUsage::StaticClass(),
			UNiagaraValidationRule_RibbonRenderer::StaticClass(),
			UNiagaraValidationRule_InvalidEffectType::StaticClass(),
			UNiagaraValidationRule_HasEffectType::StaticClass(),
			UNiagaraValidationRule_LWC::StaticClass(),
			UNiagaraValidationRule_NoOpaqueRenderMaterial::StaticClass(),
			UNiagaraValidationRule_NoFixedDeltaTime::StaticClass(),
			UNiagaraValidationRule_SimulationStageBudget::StaticClass(),
			UNiagaraValidationRule_TickDependencyCheck::StaticClass(),
			UNiagaraValidationRule_UserDataInterfaces::StaticClass(),
			UNiagaraValidationRule_SingletonModule::StaticClass(),
			UNiagaraValidationRule_NoMapForOnCpu::StaticClass(),
			UNiagaraValidationRule_ModuleSimTargetRestriction::StaticClass(),
			UNiagaraValidationRule_MaterialUsage::StaticClass(),
			UNiagaraValidationRule_RequireLatestParentEmitterVersion::StaticClass(),
			UNiagaraValidationRule_RequireParentEmitter::StaticClass()};
		return Class && Allowed.Contains(Class);
	}

	bool AddStackEntriesBounded(
		UNiagaraStackViewModel* StackViewModel,
		TSet<UNiagaraStackEntry*>& Seen,
		TArray<UNiagaraStackModuleItem*>& OutModules)
	{
		if (!StackViewModel || !StackViewModel->GetRootEntry())
		{
			return false;
		}
		TArray<UNiagaraStackEntry*> Pending{StackViewModel->GetRootEntry()};
		while (!Pending.IsEmpty())
		{
			UNiagaraStackEntry* Entry = Pending.Pop(EAllowShrinking::No);
			if (!Entry || Seen.Contains(Entry))
			{
				continue;
			}
			if (Seen.Num() >= FHyperAIStudioNiagaraContracts::MaxStackEntries)
			{
				return false;
			}
			Seen.Add(Entry);
			if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(Entry))
			{
				OutModules.Add(Module);
			}
			TArray<UNiagaraStackEntry*> Children;
			Entry->GetUnfilteredChildren(Children);
			if (Pending.Num() > FHyperAIStudioNiagaraContracts::MaxStackEntries - Seen.Num()
				|| Children.Num() > FHyperAIStudioNiagaraContracts::MaxStackEntries
					- Seen.Num() - Pending.Num())
			{
				return false;
			}
			Pending.Append(Children);
		}
		return true;
	}

	bool NativeValidationTraversalWithinBounds(UNiagaraSystem& System)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System.GetEmitterHandles();
		if (Handles.Num() > FHyperAIStudioNiagaraContracts::MaxEmitters)
		{
			return false;
		}
		int32 VersionCount = 0;
		int32 RendererCount = 0;
		int32 SimulationStageCount = 0;
		int32 EventHandlerCount = 0;
		TSet<const UNiagaraEmitter*> SeenEmitters;
		for (const FNiagaraEmitterHandle& Handle : Handles)
		{
			const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			const UNiagaraEmitter* Emitter = Instance.Emitter;
			if (!Emitter)
			{
				return false;
			}
			if (SeenEmitters.Contains(Emitter))
			{
				continue;
			}
			SeenEmitters.Add(Emitter);
			const TArray<FNiagaraAssetVersion> Versions = Emitter->GetAllAvailableVersions();
			if (Versions.Num() > FHyperAIStudioNiagaraContracts::MaxEmitterVersions
				- VersionCount)
			{
				return false;
			}
			VersionCount += Versions.Num();
			for (const FNiagaraAssetVersion& Version : Versions)
			{
				const FVersionedNiagaraEmitterData* Data =
					Emitter->GetEmitterData(Version.VersionGuid);
				if (!Data
					|| Data->GetRenderers().Num()
						> FHyperAIStudioNiagaraContracts::MaxRenderers - RendererCount
					|| Data->GetSimulationStages().Num()
						> FHyperAIStudioNiagaraContracts::MaxSimulationStages
							- SimulationStageCount
					|| Data->GetEventHandlers().Num()
						> FHyperAIStudioNiagaraContracts::MaxScripts - EventHandlerCount)
				{
					return false;
				}
				RendererCount += Data->GetRenderers().Num();
				SimulationStageCount += Data->GetSimulationStages().Num();
				EventHandlerCount += Data->GetEventHandlers().Num();
			}
		}
		return true;
	}

	bool GraphContainsParameterName(
		const TArray<UNiagaraGraph*>& Graphs,
		const FString& Name,
		bool& bOutFound)
	{
		bOutFound = false;
		const FName TargetName(*Name);
		static const FName ParameterPinSubCategory(TEXT("ParameterPin"));
		for (const UNiagaraGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : Graph->GetAllMetaData())
			{
				if (Pair.Key.GetName() == TargetName)
				{
					bOutFound = true;
					return true;
				}
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinType.PinSubCategory == ParameterPinSubCategory
						&& UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin).GetName() == TargetName)
					{
						bOutFound = true;
						return true;
					}
				}
			}
		}
		return true;
	}

	const FHyperAINiagaraUserParameterIdentity* FindParameterByGuid(
		const TArray<FHyperAINiagaraUserParameterIdentity>& Parameters,
		const FString& Guid)
	{
		return Parameters.FindByPredicate([&Guid](const FHyperAINiagaraUserParameterIdentity& Item)
		{
			return Item.VariableGuid == Guid;
		});
	}

	bool ParseCursor(
		const FString& Cursor,
		const FString& ExpectedSeal,
		int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty())
		{
			return true;
		}
		FString Seal;
		FString OffsetText;
		if (Cursor.Len() > FHyperAIStudioNiagaraContracts::MaxCursorCharacters
			|| !Cursor.Split(TEXT("."), &Seal, &OffsetText, ESearchCase::CaseSensitive,
				ESearchDir::FromEnd)
			|| Seal != ExpectedSeal || OffsetText.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : OffsetText)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
		}
		OutOffset = FCString::Atoi(*OffsetText);
		return OutOffset >= 0;
	}

	FString CursorSeal(const FString& TargetPath, const FString& Revision, int32 PageSize)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.inspect-cursor.v1"));
		AppendToken(Canonical, TargetPath);
		AppendToken(Canonical, Revision);
		AppendToken(Canonical, FString::FromInt(PageSize));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}
}

FString FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioNiagara.HyperAIStudioNiagaraToolset");
}

const TArray<FHyperAIStudioNiagaraManifestEntry>& FHyperAIStudioNiagaraContracts::GetManifest()
{
	static const TArray<FHyperAIStudioNiagaraManifestEntry> Manifest = {
		{TEXT("hyper_niagara_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_niagara_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_niagara_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioNiagaraContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioNiagaraManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioNiagaraManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name))
		{
			return false;
		}
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":"))
		|| HyperAIStudio::Niagara::Private::HasControlCharacter(Path))
	{
		return false;
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason))
	{
		return false;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioNiagaraContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioNiagaraContracts::IsUserParameterName(const FString& Name)
{
	return Name.Len() > 5 && Name.Len() <= MaxNameCharacters
		&& Name.StartsWith(TEXT("User."), ESearchCase::CaseSensitive)
		&& !Name.EndsWith(TEXT(".")) && !Name.Contains(TEXT(".."))
		&& !Name.Contains(TEXT(" ")) && !Name.Contains(TEXT("/"))
		&& !Name.Contains(TEXT("\\")) && !Name.Contains(TEXT(":"))
		&& !HyperAIStudio::Niagara::Private::HasControlCharacter(Name);
}

bool FHyperAIStudioNiagaraContracts::ValidateRenameIdentityAgainstSnapshot(
	const TArray<FHyperAINiagaraUserParameterIdentity>& Parameters,
	const FHyperAINiagaraUserParameterRename& Rename,
	const FHyperAINiagaraUserParameterIdentity*& OutParameter,
	FString& OutError)
{
	using namespace HyperAIStudio::Niagara::Private;
	OutParameter = nullptr;
	OutError.Reset();
	FGuid ExpectedGuid;
	if (!IsUserParameterName(Rename.OldName) || !IsUserParameterName(Rename.NewName)
		|| Rename.OldName == Rename.NewName
		|| !IsCanonicalSha256(Rename.ExpectedTypeFingerprint)
		|| !FGuid::ParseExact(Rename.ExpectedVariableGuid,
			EGuidFormats::DigitsWithHyphensLower, ExpectedGuid)
		|| !ExpectedGuid.IsValid()
		|| CanonicalGuid(ExpectedGuid) != Rename.ExpectedVariableGuid)
	{
		OutError = TEXT("invalid_rename_identity");
		return false;
	}
	OutParameter = FindParameterByGuid(Parameters, Rename.ExpectedVariableGuid);
	if (!OutParameter)
	{
		OutError = TEXT("variable_guid_not_found");
		return false;
	}
	if (OutParameter->Name != Rename.OldName
		|| OutParameter->TypeFingerprint != Rename.ExpectedTypeFingerprint)
	{
		OutError = TEXT("variable_identity_mismatch");
		OutParameter = nullptr;
		return false;
	}
	if (Parameters.ContainsByPredicate([&](const FHyperAINiagaraUserParameterIdentity& Item)
		{
			return Item.Name == Rename.NewName;
		}))
	{
		OutError = TEXT("destination_parameter_exists");
		OutParameter = nullptr;
		return false;
	}
	return true;
}

bool FHyperAIStudioNiagaraContracts::AreFullReferenceDomainsProven(
	const bool bExposedStore,
	const bool bGraphMetadataAndPins,
	const bool bEditorOnlyParameterAdapter,
	const bool bUserParameterBindings,
	const bool bRendererAttributeBindings,
	const bool bSystemEmitterHandleRenameClosure,
	const bool bReflectionAndCollectionsBoundedBeforeMaterialization)
{
	return bExposedStore && bGraphMetadataAndPins && bEditorOnlyParameterAdapter
		&& bUserParameterBindings && bRendererAttributeBindings
		&& bSystemEmitterHandleRenameClosure
		&& bReflectionAndCollectionsBoundedBeforeMaterialization;
}

FString FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
	UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists: return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist: return TEXT("does_not_exist");
	default: return TEXT("unknown");
	}
}

FString FHyperAIStudioNiagaraContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.inspect.payload.v1|exact_loaded_target|page_size|revision_cursor|work_ms|output_bytes"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::RenamePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.rename.payload.v1|one_target|base_revision|guid|old_name|new_name|type|default|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.validate.payload.v1|exact_loaded_target|optional_revision|closed_policy|issues|work_ms|output_bytes"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.inspect.result.v1|health|revision|parameter_identity_page|capabilities|issues|bounded"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.rename.result.v1|phase|revision|valid|bounded"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.validate.result.v1|complete|valid|policy|revision|native_rule_issues|counts|bounded"));
	return Value;
}

const UNiagaraValidationRuleSet* FHyperAIStudioNiagaraContracts::ResolveAlreadyLoadedRuleSet(
	const TSoftObjectPtr<UNiagaraValidationRuleSet>& RuleSet)
{
	// TSoftObjectPtr::Get is lookup-only. This seam has no blocking-load,
	// streaming, asset-tool, or editor-open fallback.
	return RuleSet.Get();
}

bool FHyperAIStudioNiagaraContracts::IsSealedNativeValidationRuleClass(const UClass* Class)
{
	return HyperAIStudio::Niagara::Private::IsNativeValidationRuleClass(Class);
}

FString FHyperAIStudioNiagaraInspectPayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioNiagaraInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraInspectPayload::GetBoundedByteSize() const
{
	const int64 Size = 128 + 2ll * Request.TargetPath.Len() + 2ll * Request.Cursor.Len();
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraValidatePayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioNiagaraValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraValidatePayload::GetBoundedByteSize() const
{
	const int64 Size = 160 + 2ll * Request.TargetPath.Len()
		+ 2ll * Request.ExpectedRevision.Len() + 2ll * Request.Policy.Len();
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraRenamePayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::RenamePayloadTypeId;
}

FString FHyperAIStudioNiagaraRenamePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::RenamePayloadSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraRenamePayload::GetBoundedByteSize() const
{
	const int64 Size = 256 + 2ll * TargetPath.Len() + 2ll * BaseRevision.Len()
		+ 2ll * ExpectedVariableGuid.Len() + 2ll * OldName.Len() + 2ll * NewName.Len()
		+ 2ll * TypeFingerprint.Len() + 2ll * DefaultFingerprint.Len()
		+ 2ll * SemanticFingerprint.Len();
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraRenamePayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioNiagaraRenamePayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioNiagaraRenamePayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioNiagaraRenamePayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->ExpectedVariableGuid = ExpectedVariableGuid;
	Clone->OldName = OldName;
	Clone->NewName = NewName;
	Clone->TypeFingerprint = TypeFingerprint;
	Clone->DefaultFingerprint = DefaultFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioNiagaraInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::InspectResultTypeId;
}

FString FHyperAIStudioNiagaraInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraInspectResultPayload::GetBoundedByteSize() const
{
	int64 Size = 1024 + 2ll * Report.Status.Len() + 2ll * Report.Diagnostic.Len()
		+ 2ll * Report.Revision.Len() + 2ll * Report.NextCursor.Len()
		+ HyperAIStudio::Niagara::Private::EstimateHealthBytes(Report.Health)
		+ HyperAIStudio::Niagara::Private::EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAINiagaraUserParameterIdentity& Parameter : Report.UserParameters)
	{
		Size += HyperAIStudio::Niagara::Private::EstimateParameterBytes(Parameter);
	}
	for (const FHyperAINiagaraIssue& Issue : Report.Issues)
	{
		Size += HyperAIStudio::Niagara::Private::EstimateIssueBytes(Issue);
	}
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::ValidateResultTypeId;
}

FString FHyperAIStudioNiagaraValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraValidateResultPayload::GetBoundedByteSize() const
{
	int64 Size = 1024 + 2ll * Report.Status.Len() + 2ll * Report.Diagnostic.Len()
		+ 2ll * Report.Policy.Len() + 2ll * Report.Revision.Len()
		+ HyperAIStudio::Niagara::Private::EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAINiagaraIssue& Issue : Report.Issues)
	{
		Size += HyperAIStudio::Niagara::Private::EstimateIssueBytes(Issue);
	}
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraMutationResultPayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::MutationResultTypeId;
}

FString FHyperAIStudioNiagaraMutationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::MutationResultSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraMutationResultPayload::GetBoundedByteSize() const
{
	const int64 Size = 96 + 2ll * Phase.Len() + 2ll * Revision.Len();
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

TArray<FHyperAINiagaraCapabilityStatus> FHyperAIStudioNiagaraContracts::GetCapabilityMatrix()
{
	FHyperAINiagaraCapabilityStatus Niagara;
	Niagara.bIndependentValidationImplemented = false;
	Niagara.SupportedCases = {
		TEXT("exact_loaded_system_health_and_complete_cas"),
		TEXT("paged_stable_user_parameter_identity_tokens"),
		TEXT("source_candidate_loaded_native_validation_without_soft_loads"),
		TEXT("single_user_parameter_rename_partial_preflight"),
		TEXT("typed_async_mutation_contract_without_sync_execution")};
	Niagara.DelegatedEpicCallables = {
		TEXT("GetAssetDiscoveryInfo"), TEXT("FindNiagaraScripts"),
		TEXT("GetNiagaraScriptDigest"), TEXT("ConstructNiagaraBPWrapperFromSystem"),
		TEXT("ConstructNiagaraBPWrapperFromComponent"), TEXT("SetSystem"),
		TEXT("GetUserVariables(system)"), TEXT("SetVariable"), TEXT("GetVariable"),
		TEXT("UEnum_Info"), TEXT("GetAvailableDynamicInputs"), TEXT("CreateNiagaraSystem"),
		TEXT("GetSystemSchema"), TEXT("GetEmitterSchema"), TEXT("GetRendererSchema"),
		TEXT("GetDataInterfaceSchema"), TEXT("GetStackInputSchema"), TEXT("GetModuleSchema"),
		TEXT("GetDynamicInputSchema"), TEXT("GetModuleSchemaFromAsset"),
		TEXT("GetDynamicInputSchemaFromAsset"), TEXT("GetSystemSummary"),
		TEXT("GetEmitterSummary"), TEXT("GetEmitterTopology"),
		TEXT("GetScriptStackTopology"), TEXT("GetModuleTopology"),
		TEXT("GetStackInputTopology"), TEXT("GetUserVariables(component)"),
		TEXT("GetSystemData"), TEXT("GetEmitterData"), TEXT("GetRendererData"),
		TEXT("GetStackInputData"), TEXT("GetEmitterInputValues"),
		TEXT("GetScriptStackInputValues"), TEXT("GetModuleInputValues"),
		TEXT("GetDynamicInputChain"), TEXT("GetSystemDependencies"),
		TEXT("SetSystemData"), TEXT("SetEmitterData"), TEXT("SetRendererData"),
		TEXT("AddUserVariables"), TEXT("RemoveUserVariables"), TEXT("AddEmitter"),
		TEXT("RemoveEmitter"), TEXT("AddRenderer"), TEXT("RemoveRenderer"),
		TEXT("AddModule"), TEXT("RemoveModule"), TEXT("SetModuleEnabled"),
		TEXT("AddSetParametersModule"), TEXT("AddSetParameterEntry"),
		TEXT("RemoveSetParameterEntry"), TEXT("SetStackInputData"),
		TEXT("GetSystemCompileState"), TEXT("GetStackIssues"),
		TEXT("ApplyStackIssueFix")};
	Niagara.UnsupportedCases = {
		TEXT("target_load_or_editor_open"),
		TEXT("raw_hlsl_python_lua_or_script_text"),
		TEXT("scratchpad_authoring"),
		TEXT("stateless_emitter_authoring_that_tryloads_default_material"),
		TEXT("stateless_emitter_complete_cas_or_native_validation"),
		TEXT("module_reorder_through_unexported_editor_api"),
		TEXT("event_or_simulation_stage_or_version_authoring"),
		TEXT("data_channel_creation_without_closed_public_api"),
		TEXT("arbitrary_validation_fix_delegate_execution"),
		TEXT("custom_validation_rule_execution"),
		TEXT("native_rule_execution_not_hard_bounded"),
		TEXT("full_reference_inventory_incomplete"),
		TEXT("broad_asset_scan_capture_or_dependency_walk"),
		TEXT("runtime_simulation_screenshot_or_actor_spawn"),
		TEXT("generic_reflection_property_dispatch")};
	Niagara.State = TEXT("source_candidate_full_reference_inventory_incomplete_native_rule_execution_not_hard_bounded_async_host_required");
	Niagara.Remediation = TEXT("Use Epic NiagaraToolsets for delegated primitives. Rename dry-run remains blocked until a bounded immutable inventory covers editor-only parameters, user bindings, renderer attribute bindings, and system/emitter rename closure before any reflection result is materialized. Loaded native validation stays dev-only until closed value-snapshot analyzers replace virtual rule execution. Mutation execution also requires the shared pinned begin/poll/cancel continuation host.");
	return {Niagara};
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioNiagaraContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.niagara.loaded_native.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_niagara_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_niagara_apply_plan"), MutationVariantId,
			RenamePayloadTypeId, RenamePayloadSchemaFingerprint(), MutationResultTypeId,
			MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_niagara_validate"), ValidateVariantId,
			ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(), ValidateResultTypeId,
			ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioNiagaraContracts::CaptureExact(
	const FString& TargetPath,
	int32 MaxWorkMs,
	FHyperAIStudioNiagaraValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Niagara::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	const double Started = FPlatformTime::Seconds();
	const int32 WorkMs = FMath::Clamp(MaxWorkMs, 1, MaxReadGameThreadMs);
	auto DeadlineExceeded = [&]()
	{
		return (FPlatformTime::Seconds() - Started) * 1000.0 > WorkMs;
	};
	auto Fail = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		OutStatus = Status;
		OutDiagnostic = Diagnostic;
		return false;
	};
	if (!IsInGameThread())
	{
		return Fail(TEXT("game_thread_required"),
			TEXT("Exact loaded Niagara inspection is permitted only on the Unreal game thread."));
	}
	if (!IsCanonicalProjectObjectPath(TargetPath))
	{
		return Fail(TEXT("invalid_target_path"),
			TEXT("Target must be one canonical top-level /Game object path."));
	}
	const FSoftObjectPath SoftPath(TargetPath);
	UObject* LoadedObject = SoftPath.ResolveObject();
	if (!LoadedObject)
	{
		return Fail(TEXT("target_not_loaded"),
			TEXT("The exact Niagara System is not loaded; HyperAI never loads it."));
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadedObject);
	if (!System || System->GetPathName() != TargetPath)
	{
		return Fail(TEXT("target_class_mismatch"),
			TEXT("The exact loaded target is not a UNiagaraSystem or derived class."));
	}
	UPackage* Package = System->GetOutermost();
	if (!Package || Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor))
	{
		return Fail(TEXT("unsupported_target_package"),
			TEXT("Transient and PIE Niagara Systems are outside the persisted CAS contract."));
	}

	FHyperAINiagaraHealthRecord& Health = OutSnapshot.Health;
	Health.TargetPath = TargetPath;
	Health.ClassPath = System->GetClass()->GetPathName();
	Health.bLoaded = true;
	Health.bPackageDirty = Package->IsDirty();
	Health.bWasLoadedFromDisk = System->HasAnyFlags(RF_WasLoaded);
	Health.AssetGuid = CanonicalGuid(System->GetAssetGuid());
	Health.bSystemValid = System->IsValid();
	Health.bReadyToRun = System->IsReadyToRun();
	Health.bExistingSystemViewModel = GetExistingSystemViewModel(System).IsValid();

	bool bComplete = true;
	if (Health.AssetGuid.IsEmpty())
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("system_asset_guid_unavailable"),
			TEXT("error"), TEXT("The persisted Niagara System has no stable nonzero asset guid."));
	}
	if (Health.bPackageDirty)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("dirty_loaded_state_not_cas_complete"),
			TEXT("error"),
			TEXT("A dirty loaded Niagara System cannot claim an exact persisted CAS revision."));
	}
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
	FAssetPackageData PackageData;
	if (AssetRegistry)
	{
		PackageState = AssetRegistry->TryGetAssetPackageData(
			Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
	}
	Health.DiskExistence = PackageState == UE::AssetRegistry::EExists::Exists
		&& !Health.bWasLoadedFromDisk
		? TEXT("unknown") : ClassifyAssetRegistryExistence(PackageState);
	Health.bExistsOnDisk = PackageState == UE::AssetRegistry::EExists::Exists
		&& Health.bWasLoadedFromDisk;
	if (!AssetRegistry || PackageState != UE::AssetRegistry::EExists::Exists
		|| !Health.bWasLoadedFromDisk)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("asset_registry_presence_unproven"),
			TEXT("error"), TEXT("Exact on-disk package/object provenance is not proven."),
			TEXT("The non-blocking package-data lookup must return Exists and the exact canonical top-level UObject must carry RF_WasLoaded; Unknown is never treated as absence."), FString(), TargetPath);
	}
	else
	{
		Health.DiskSize = PackageData.DiskSize;
		Health.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
		if (Health.DiskSize <= 0 || PackageData.GetPackageSavedHash().IsZero()
			|| Health.PackageSavedHash.IsEmpty())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("saved_package_identity_unavailable"),
				TEXT("error"), TEXT("Saved package hash or disk size is unavailable."),
				FString(), FString(), TargetPath);
		}
	}

	Health.bCompileActive = System->HasActiveCompilations()
		|| System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
	Health.bCompileStale = Health.bCompileActive;

	const TArray<FNiagaraEmitterHandle>& Emitters = System->GetEmitterHandles();
	Health.EmitterCount = Emitters.Num();
	if (Emitters.Num() > MaxEmitters)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("emitter_bound_exceeded"),
			TEXT("error"), TEXT("Emitter count exceeds the closed inspection bound."));
	}
	TSet<FGuid> EmitterIds;
	TArray<FString> EmitterEvidence;
	for (const FNiagaraEmitterHandle& Handle : Emitters)
	{
		if (EmitterEvidence.Num() >= MaxEmitters)
		{
			break;
		}
		const FGuid Id = Handle.GetId();
		if (!Id.IsValid() || EmitterIds.Contains(Id))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("invalid_or_duplicate_emitter_id"),
				TEXT("error"), TEXT("Emitter handles require unique nonzero stable ids."));
		}
		EmitterIds.Add(Id);
		FString Entry;
		AppendToken(Entry, CanonicalGuid(Id));
		AppendToken(Entry, Handle.GetName().ToString());
		AppendToken(Entry, Handle.GetIsEnabled() ? TEXT("enabled") : TEXT("disabled"));
		AppendToken(Entry, FString::FromInt(static_cast<int32>(Handle.GetEmitterMode())));
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		if (!Instance.Emitter || !Instance.Version.IsValid())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("emitter_version_identity_incomplete"),
				TEXT("error"), TEXT("Every emitter handle requires one loaded emitter and stable version guid."));
		}
		AppendToken(Entry, ObjectIdentity(Instance.Emitter));
		AppendToken(Entry, CanonicalGuid(Instance.Version));
		EmitterEvidence.Add(MoveTemp(Entry));
	}
	EmitterEvidence.Sort();

	const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	const bool bHasSystemEditorData =
		Cast<UNiagaraSystemEditorData>(System->GetEditorData()) != nullptr;
	if (!bHasSystemEditorData)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("system_editor_data_unavailable"),
			TEXT("error"), TEXT("Loaded Niagara System editor data is unavailable."));
	}
	TArray<FNiagaraVariable> Parameters;
	Health.ParameterCount = Store.Num();
	if (Store.Num() > MaxParameters
		|| Store.GetParameterDataArray().Num() > MaxParameterDataBytes
		|| Store.GetUObjects().Num() > MaxParameters
		|| Store.GetDataInterfaces().Num() > MaxParameters)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("parameter_bound_exceeded"),
			TEXT("error"), TEXT("User-parameter count or value bytes exceed the closed CAS bound."));
	}
	else
	{
		Store.GetParameters(Parameters);
	}
	Parameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		const FString AKey = A.GetName().ToString() + TEXT("|") + TypeId(A.GetType());
		const FString BKey = B.GetName().ToString() + TEXT("|") + TypeId(B.GetType());
		return AKey < BKey;
	});
	TSet<FName> ParameterNames;
	TSet<FGuid> ParameterGuids;
	for (const FNiagaraVariable& Parameter : Parameters)
	{
		if (OutSnapshot.Parameters.Num() >= MaxParameters)
		{
			break;
		}
		FHyperAINiagaraUserParameterIdentity Identity;
		Identity.Name = Parameter.GetName().ToString();
		Identity.TypeId = TypeId(Parameter.GetType()).Left(512);
		Identity.TypeFingerprint = TypeFingerprint(Parameter.GetType());
		const UNiagaraScriptVariable* ScriptVariable = bHasSystemEditorData
			? FNiagaraEditorUtilities::UserParameters::FindScriptVariableForUserParameter(
				Parameter, *System) : nullptr;
		const FGuid VariableGuid = ScriptVariable
			? ScriptVariable->Metadata.GetVariableGuid() : FGuid();
		Identity.VariableGuid = CanonicalGuid(VariableGuid);
		Identity.MetadataChangeId = ScriptVariable
			? CanonicalGuid(ScriptVariable->GetChangeId()) : FString();
		bool bDefaultComplete = false;
		Identity.DefaultFingerprint = DefaultFingerprint(Store, Parameter, bDefaultComplete);
		if (!ScriptVariable || !VariableGuid.IsValid() || !bDefaultComplete
			|| Identity.TypeId.IsEmpty()
			|| !IsCanonicalSha256(Identity.TypeFingerprint)
			|| !IsCanonicalSha256(Identity.DefaultFingerprint)
			|| Identity.MetadataChangeId.IsEmpty()
			|| !IsUserParameterName(Identity.Name)
			|| ParameterNames.Contains(Parameter.GetName())
			|| ParameterGuids.Contains(VariableGuid))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("user_parameter_identity_incomplete"),
				TEXT("error"), TEXT("Every exposed user parameter requires a unique name, metadata guid, type, and bounded default fingerprint."),
				FString(), FString(), TargetPath);
		}
		ParameterNames.Add(Parameter.GetName());
		if (VariableGuid.IsValid())
		{
			ParameterGuids.Add(VariableGuid);
		}
		OutSnapshot.Parameters.Add(MoveTemp(Identity));
	}

	TArray<UNiagaraScript*> Scripts;
	if (!CollectReachableScripts(*System, Scripts))
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("script_bound_exceeded"),
			TEXT("error"), TEXT("Reachable Niagara script count exceeds the closed CAS bound."));
	}
	Health.ScriptCount = Scripts.Num();
	Health.bCompileStateKnown = true;
	int32 AuthoritativeCompileScriptCount = 0;
	for (const UNiagaraScript* Script : Scripts)
	{
		if (!Script || !HasAuthoritativeCompileStatus(Script->GetUsage()))
		{
			continue;
		}
		++AuthoritativeCompileScriptCount;
		switch (Script->GetVMExecutableData().LastCompileStatus)
		{
		case ENiagaraScriptCompileStatus::NCS_Dirty:
			Health.bCompileStale = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_Error:
			Health.bCompileHasErrors = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
			Health.bCompileHasWarnings = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			break;
		default:
			Health.bCompileStateKnown = false;
			break;
		}
	}
	Health.bCompileStateKnown &= AuthoritativeCompileScriptCount > 0;
	if (!Health.bCompileStateKnown)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("compile_state_unavailable"),
			TEXT("error"), TEXT("Bounded loaded-script compile state is unknown or being created."),
			FString(), FString(), TargetPath);
	}
	TArray<UNiagaraGraph*> Graphs;
	int32 GraphNodeCount = 0;
	if (!CollectReachableGraphs(Scripts, Graphs, GraphNodeCount))
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("graph_bound_exceeded"),
			TEXT("error"), TEXT("Reachable graph or graph-node count exceeds the closed CAS bound."));
	}
	Health.GraphCount = Graphs.Num();
	Health.GraphNodeCount = GraphNodeCount;

	TArray<FString> ScriptEvidence;
	for (const UNiagaraScript* Script : Scripts)
	{
		FString Entry;
		AppendToken(Entry, ObjectIdentity(Script));
		AppendToken(Entry, FString::FromInt(static_cast<int32>(Script->GetUsage())));
		AppendToken(Entry, CanonicalGuid(Script->GetUsageId()));
		AppendToken(Entry, CanonicalGuid(Script->GetBaseChangeID()));
		ScriptEvidence.Add(MoveTemp(Entry));
	}
	ScriptEvidence.Sort();
	TArray<FString> GraphEvidence;
	for (const UNiagaraGraph* Graph : Graphs)
	{
		FString Entry;
		AppendToken(Entry, ObjectIdentity(Graph));
		AppendToken(Entry, CanonicalGuid(Graph->GetChangeID()));
		AppendToken(Entry, FString::FromInt(Graph->Nodes.Num()));
		GraphEvidence.Add(MoveTemp(Entry));
	}
	GraphEvidence.Sort();

	if (DeadlineExceeded())
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("inspection_deadline_exceeded"),
			TEXT("error"), TEXT("The exact target exceeded the requested game-thread work budget."));
	}

	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.niagara.loaded-system-revision.v1"));
	AppendToken(Canonical, Health.TargetPath);
	AppendToken(Canonical, Health.ClassPath);
	AppendToken(Canonical, Health.DiskExistence);
	AppendToken(Canonical, Health.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("not_loaded_from_disk"));
	AppendToken(Canonical, Health.PackageSavedHash);
	AppendToken(Canonical, FString::Printf(TEXT("%lld"), Health.DiskSize));
	AppendToken(Canonical, Health.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
	AppendToken(Canonical, Health.AssetGuid);
	AppendToken(Canonical, Health.bCompileStateKnown ? TEXT("compile_known") : TEXT("compile_unknown"));
	AppendToken(Canonical, Health.bCompileActive ? TEXT("compile_active") : TEXT("compile_idle"));
	AppendToken(Canonical, Health.bCompileStale ? TEXT("compile_stale") : TEXT("compile_fresh"));
	AppendToken(Canonical, Health.bCompileHasErrors ? TEXT("compile_errors") : TEXT("compile_no_errors"));
	AppendToken(Canonical, Health.bCompileHasWarnings ? TEXT("compile_warnings") : TEXT("compile_no_warnings"));
	AppendToken(Canonical, Health.bSystemValid ? TEXT("system_valid") : TEXT("system_invalid"));
	AppendToken(Canonical, Health.bReadyToRun ? TEXT("ready_to_run") : TEXT("not_ready_to_run"));
	AppendToken(Canonical, Health.bExistingSystemViewModel ? TEXT("existing_vm") : TEXT("no_existing_vm"));
	AppendToken(Canonical, FString::FromInt(Health.EmitterCount));
	AppendToken(Canonical, FString::FromInt(Health.ParameterCount));
	AppendToken(Canonical, FString::FromInt(Health.ScriptCount));
	AppendToken(Canonical, FString::FromInt(Health.GraphCount));
	AppendToken(Canonical, FString::FromInt(Health.GraphNodeCount));
	for (const FString& Entry : EmitterEvidence) AppendToken(Canonical, Entry);
	for (const FHyperAINiagaraUserParameterIdentity& Parameter : OutSnapshot.Parameters)
	{
		AppendToken(Canonical, Parameter.VariableGuid);
		AppendToken(Canonical, Parameter.Name);
		AppendToken(Canonical, Parameter.TypeFingerprint);
		AppendToken(Canonical, Parameter.DefaultFingerprint);
		AppendToken(Canonical, Parameter.MetadataChangeId);
	}
	for (const FString& Entry : ScriptEvidence) AppendToken(Canonical, Entry);
	for (const FString& Entry : GraphEvidence) AppendToken(Canonical, Entry);
	Health.Revision = bComplete
		? FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical) : FString();
	if (bComplete && !IsCanonicalSha256(Health.Revision))
	{
		bComplete = false;
		Health.Revision.Reset();
		AddIssue(OutSnapshot.CaptureIssues, TEXT("revision_hash_bound_exceeded"),
			TEXT("error"), TEXT("Exact Niagara revision evidence exceeded the shared bounded hash input."));
	}
	Health.bRevisionComplete = bComplete;
	OutSnapshot.bComplete = Health.bRevisionComplete;
	OutStatus = OutSnapshot.bComplete ? TEXT("ok") : TEXT("revision_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured one exact loaded Niagara System without loading or scanning assets.")
		: TEXT("The loaded target was observed, but complete CAS evidence was not proven.");
	return true;
}

FHyperAINiagaraInspectReport FHyperAIStudioNiagaraContracts::Inspect(
	const FHyperAINiagaraInspectRequest& Request)
{
	using namespace HyperAIStudio::Niagara::Private;
	FHyperAINiagaraInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsCanonicalProjectObjectPath(Request.TargetPath))
	{
		return Reject(TEXT("invalid_target_path"),
			TEXT("target_path must be one canonical top-level /Game object path."));
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("page_size, cursor, game-thread, or output bounds are outside the closed contract."));
	}

	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.Health = Snapshot.Health;
	Report.Revision = Snapshot.Health.Revision;
	Report.bRevisionComplete = Snapshot.Health.bRevisionComplete;
	Report.bFreshCapture = true;
	Report.Status = CaptureStatus;
	Report.Diagnostic = CaptureDiagnostic;
	Report.bOk = Snapshot.bComplete;
	int64 EstimatedBytes = 1024ll + EstimateHealthBytes(Report.Health)
		+ EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAINiagaraIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bOk = false;
			Report.bTruncated = true;
			Report.Status = TEXT("output_budget_exceeded");
			Report.Diagnostic = TEXT("Capture diagnostics cannot fit the requested closed output budget; no page cursor is issued.");
			return Report;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}

	const FString Seal = CursorSeal(Request.TargetPath, Report.Revision, Request.PageSize);
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Seal, Offset)
		|| Offset > Snapshot.Parameters.Num()
		|| (!Request.Cursor.IsEmpty() && !Snapshot.bComplete))
	{
		Report.bOk = false;
		Report.Status = TEXT("invalid_or_stale_cursor");
		Report.Diagnostic = TEXT("The cursor is malformed, out of range, or not bound to this exact complete revision.");
		Report.UserParameters.Reset();
		return Report;
	}

	const int32 End = FMath::Min(Snapshot.Parameters.Num(), Offset + Request.PageSize);
	int32 NextOffset = Offset;
	for (int32 Index = Offset; Index < End; ++Index)
	{
		const int32 ItemBytes = EstimateParameterBytes(Snapshot.Parameters[Index]);
		if (EstimatedBytes + ItemBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		Report.UserParameters.Add(Snapshot.Parameters[Index]);
		EstimatedBytes += ItemBytes;
		NextOffset = Index + 1;
	}
	if (End > Offset && NextOffset == Offset)
	{
		Report.bOk = false;
		Report.bTruncated = true;
		Report.Status = TEXT("output_budget_exceeded");
		Report.Diagnostic = TEXT("One parameter identity cannot fit the requested output budget; no non-progress cursor is issued.");
		Report.NextCursor.Reset();
		return Report;
	}
	if (NextOffset < Snapshot.Parameters.Num())
	{
		Report.bTruncated = true;
		if (Snapshot.bComplete)
		{
			Report.NextCursor = Seal + TEXT(".") + FString::FromInt(NextOffset);
		}
	}
	return Report;
}

FHyperAINiagaraValidateReport FHyperAIStudioNiagaraContracts::Validate(
	const FHyperAINiagaraValidateRequest& Request)
{
	using namespace HyperAIStudio::Niagara::Private;
	FHyperAINiagaraValidateReport Report;
	Report.Policy = Request.Policy;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsCanonicalProjectObjectPath(Request.TargetPath))
	{
		return Reject(TEXT("invalid_target_path"),
			TEXT("target_path must be one canonical top-level /Game object path."));
	}
	if (Request.Policy != TEXT("authoring") && Request.Policy != TEXT("runtime_ready"))
	{
		return Reject(TEXT("unsupported_validation_policy"),
			TEXT("policy must be authoring or runtime_ready."));
	}
	if ((!Request.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Request.ExpectedRevision))
		|| Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds_or_revision"),
			TEXT("revision, issue, game-thread, or output bounds are outside the closed contract."));
	}
	if (!FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("Native Niagara validation is source-candidate evidence only; virtual rule execution is not hard latency/memory bounded."));
	}

	const double Started = FPlatformTime::Seconds();
	auto DeadlineExceeded = [&]()
	{
		return (FPlatformTime::Seconds() - Started) * 1000.0 > Request.MaxGameThreadMs;
	};
	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.Revision = Snapshot.Health.Revision;
	Report.bRevisionComplete = Snapshot.Health.bRevisionComplete;
	Report.bFreshCapture = true;
	bool bComplete = Snapshot.bComplete;
	int64 EstimatedBytes = 1024ll + EstimateCapabilitiesBytes(Report.Capabilities);
	auto AddBoundedReportIssue = [&](const FHyperAINiagaraIssue& Issue)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
		const int32 ItemBytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= Request.MaxIssues
			|| EstimatedBytes + ItemBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			bComplete = false;
			return;
		}
		Report.Issues.Add(Issue);
		EstimatedBytes += ItemBytes;
	};
	for (const FHyperAINiagaraIssue& Issue : Snapshot.CaptureIssues)
	{
		AddBoundedReportIssue(Issue);
	}
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Report.Revision)
	{
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("stale_revision"), TEXT("error"),
			TEXT("The exact loaded Niagara System no longer matches expected_revision."));
		AddBoundedReportIssue(Temp[0]);
		bComplete = false;
	}
	if (Snapshot.Health.bCompileActive || Snapshot.Health.bCompileStale)
	{
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("compile_not_terminal_or_stale"), TEXT("error"),
			TEXT("Independent validation never waits for or flushes a Niagara compile."));
		AddBoundedReportIssue(Temp[0]);
		bComplete = false;
	}
	if (!bComplete || Report.bTruncated || DeadlineExceeded())
	{
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("Exact revision, terminal compile, issue/output bounds, or the game-thread deadline were not proven; no native rule was inspected or executed.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).ResolveObject());
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = GetExistingSystemViewModel(System);
	if (!System || !ViewModel.IsValid())
	{
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("existing_system_view_model_required"), TEXT("error"),
			TEXT("Loaded-native Niagara validation requires the system to already be open in Niagara Editor."),
			TEXT("HyperAI does not construct a data-processing view model because UE 5.8 initialization can compile and synchronously load default validation rule sets."));
		AddBoundedReportIssue(Temp[0]);
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("No existing Niagara System ViewModel was available; nothing was loaded or created.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}
	if (!NativeValidationTraversalWithinBounds(*System))
	{
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("native_validation_structure_bound_exceeded"), TEXT("error"),
			TEXT("Emitter versions, renderers, simulation stages, or event handlers exceed the sealed native-validation bounds."));
		AddBoundedReportIssue(Temp[0]);
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("Native rule traversal bounds were not proven; no validation rule executed.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}

	struct FRuleInvocation
	{
		const UNiagaraValidationRule* Rule = nullptr;
		UNiagaraStackEntry* Source = nullptr;
	};
	TArray<FRuleInvocation> Invocations;
	auto AddRules = [&](TConstArrayView<TObjectPtr<UNiagaraValidationRule>> Rules,
		UNiagaraStackEntry* Source)
	{
		for (const UNiagaraValidationRule* Rule : Rules)
		{
			if (Rule && Rule->IsEnabled())
			{
				if (Invocations.Num() >= MaxValidationRules)
				{
					bComplete = false;
					return false;
				}
				Invocations.Add({Rule, Source});
			}
		}
		return true;
	};

	const UNiagaraEditorSettings* Settings = GetDefault<UNiagaraEditorSettings>();
	if (!Settings)
	{
		bComplete = false;
	}
	else if (Settings->DefaultValidationRuleSets.Num() > MaxValidationRules)
	{
		bComplete = false;
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("default_rule_set_bound_exceeded"), TEXT("error"),
			TEXT("Configured default Niagara validation rule sets exceed the closed bound."));
		AddBoundedReportIssue(Temp[0]);
	}
	else
	{
		for (const TSoftObjectPtr<UNiagaraValidationRuleSet>& SoftRuleSet
			: Settings->DefaultValidationRuleSets)
		{
			if (SoftRuleSet.IsNull())
			{
				continue;
			}
			const UNiagaraValidationRuleSet* RuleSet = ResolveAlreadyLoadedRuleSet(SoftRuleSet);
			if (!RuleSet)
			{
				bComplete = false;
				TArray<FHyperAINiagaraIssue> Temp;
				AddIssue(Temp, TEXT("default_rule_set_unloaded"), TEXT("error"),
					TEXT("A configured default Niagara validation rule set is not already loaded."),
					TEXT("Validation fails closed instead of resolving an unloaded soft reference."), FString(), SoftRuleSet.ToString());
				AddBoundedReportIssue(Temp[0]);
				continue;
			}
			if (!AddRules(RuleSet->ValidationRules, nullptr)) break;
		}
	}
	if (UNiagaraEffectType* EffectType = System->GetEffectType())
	{
		if (EffectType->ValidationRules.Num() > MaxValidationRules
			|| EffectType->ValidationRuleSets.Num() > MaxValidationRules)
		{
			bComplete = false;
		}
		else if (AddRules(EffectType->ValidationRules, nullptr))
		{
			for (const UNiagaraValidationRuleSet* RuleSet : EffectType->ValidationRuleSets)
			{
				if (RuleSet && !AddRules(RuleSet->ValidationRules, nullptr)) break;
			}
		}
	}

	TSet<UNiagaraStackEntry*> SeenStackEntries;
	TArray<UNiagaraStackModuleItem*> Modules;
	bool bStacksWithinBounds = AddStackEntriesBounded(
		ViewModel->GetSystemStackViewModel(), SeenStackEntries, Modules);
	const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& EmitterViewModels =
		ViewModel->GetEmitterHandleViewModels();
	if (EmitterViewModels.Num() > MaxEmitters)
	{
		bStacksWithinBounds = false;
	}
	else
	{
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterViewModel
			: EmitterViewModels)
		{
			if (EmitterViewModel->GetIsEnabled())
			{
				bStacksWithinBounds &= AddStackEntriesBounded(
					EmitterViewModel->GetEmitterStackViewModel(), SeenStackEntries, Modules);
			}
		}
	}
	if (!bStacksWithinBounds)
	{
		bComplete = false;
		TArray<FHyperAINiagaraIssue> Temp;
		AddIssue(Temp, TEXT("stack_entry_bound_exceeded"), TEXT("error"),
			TEXT("Existing Niagara stack entries exceed the closed validation bound."));
		AddBoundedReportIssue(Temp[0]);
	}
	else
	{
		for (UNiagaraStackModuleItem* Module : Modules)
		{
			if (!Module || !Module->GetIsEnabled())
			{
				continue;
			}
			if (UNiagaraScript* Script = Module->GetModuleNode().FunctionScript)
			{
				if (!AddRules(Script->ValidationRules, Module)) break;
			}
		}
	}

	for (const FRuleInvocation& Invocation : Invocations)
	{
		if (!IsNativeValidationRuleClass(Invocation.Rule->GetClass()))
		{
			bComplete = false;
			TArray<FHyperAINiagaraIssue> Temp;
			AddIssue(Temp, TEXT("custom_validation_rule_unsupported"), TEXT("error"),
				TEXT("An enabled validation rule is outside the sealed UE 5.8 native class allowlist."),
				TEXT("Arbitrary validation UObject code is never executed."),
				Invocation.Rule->GetClass()->GetPathName(), Invocation.Rule->GetPathName());
			AddBoundedReportIssue(Temp[0]);
		}
	}
	if (!bComplete || DeadlineExceeded())
	{
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("Validation prerequisites, bounds, or loaded native rule authority are incomplete; no rule fixes ran.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}
	// UE 5.8 exposes each rule as injected virtual UObject code. The closed class
	// allowlist prevents arbitrary custom code, but CheckValidity has no cancellable
	// per-call work/result sink. This is a permanent v1 admission blocker even in dev mode.
	TArray<FHyperAINiagaraIssue> NativeBoundIssue;
	AddIssue(NativeBoundIssue, TEXT("native_rule_execution_not_hard_bounded"), TEXT("error"),
		TEXT("UE native validation-rule execution has no hard per-call latency or allocation bound."),
		TEXT("Results are capped after each virtual call and deadlines are checked between rules; admission requires replacement with closed value-snapshot analyzers."));
	AddBoundedReportIssue(NativeBoundIssue[0]);
	if (!bComplete)
	{
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("The native-rule admission blocker did not fit the closed report bounds; no rule executed.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}

	FNiagaraValidationContext Context;
	Context.ViewModel = ViewModel;
	for (const FRuleInvocation& Invocation : Invocations)
	{
		if (DeadlineExceeded())
		{
			bComplete = false;
			break;
		}
		Context.Source = Invocation.Source;
		TArray<FNiagaraValidationResult> NativeResults;
		Invocation.Rule->CheckValidity(Context, NativeResults);
		++Report.ExecutedRuleCount;
		if (DeadlineExceeded())
		{
			bComplete = false;
			break;
		}
		if (NativeResults.Num() > MaxInternalValidationResults)
		{
			bComplete = false;
			break;
		}
		for (const FNiagaraValidationResult& Native : NativeResults)
		{
			const FString Summary = Native.SummaryText.ToString();
			const FString Description = Native.Description.ToString();
			if (Summary.Len() > MaxIssueSummaryChars
				|| Description.Len() > MaxIssueDescriptionChars)
			{
				bComplete = false;
				continue;
			}
			const TCHAR* Severity = Native.Severity == ENiagaraValidationSeverity::Error
				? TEXT("error") : Native.Severity == ENiagaraValidationSeverity::Warning
					? TEXT("warning") : TEXT("info");
			const UObject* SourceObject = Native.SourceObject.IsValid()
				? Native.SourceObject.Get() : static_cast<UObject*>(Invocation.Source);
			TArray<FHyperAINiagaraIssue> Temp;
			AddIssue(Temp, TEXT("native_validation_result"), Severity, Summary, Description,
				Invocation.Rule->GetClass()->GetPathName(),
				SourceObject ? SourceObject->GetPathName() : Request.TargetPath);
			AddBoundedReportIssue(Temp[0]);
			// Native.Fixes and Native.Links remain call-local, never execute, and never escape.
		}
		if (!bComplete)
		{
			break;
		}
	}
	if (DeadlineExceeded())
	{
		bComplete = false;
	}
	if (!bComplete || Report.bTruncated)
	{
		Report.Status = TEXT("validation_incomplete");
		Report.Diagnostic = TEXT("Native validation exceeded a hard rule, result, text, time, issue, or output bound; PASS is impossible.");
		Report.bComplete = false;
		Report.bValid = false;
		return Report;
	}
	Report.bOk = false;
	Report.bComplete = false;
	Report.bValid = false;
	Report.Status = TEXT("validation_incomplete_native_rule_execution_not_hard_bounded");
	Report.Diagnostic = TEXT("Loaded UE-native rules ran in explicit dev evidence mode without soft loads or fix execution, but their virtual calls are not hard latency/memory bounded. This result is never admission PASS.");
	return Report;
}

FString FHyperAIStudioNiagaraContracts::ComputeRenameSemanticFingerprint(
	const FHyperAIStudioNiagaraRenamePayload& Payload)
{
	using namespace HyperAIStudio::Niagara::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.niagara.user-parameter-rename-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, TEXT("hyper_niagara_apply_plan"));
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, RenamePayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.ExpectedVariableGuid);
	AppendToken(Canonical, Payload.OldName);
	AppendToken(Canonical, Payload.NewName);
	AppendToken(Canonical, Payload.TypeFingerprint);
	AppendToken(Canonical, Payload.DefaultFingerprint);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAINiagaraApplyPlanReport FHyperAIStudioNiagaraContracts::BuildPlan(
	const FHyperAINiagaraApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Niagara::Private;
	FHyperAINiagaraApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bOk = false;
		Report.bTrustedPrepared = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Niagara rename planning requires one bounded Unreal game-thread capture."));
	}
	if (Request.bDryRun
		&& (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("Dry-run planning prohibits operation_id and expected_plan_hash."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedRevision))
	{
		return Reject(TEXT("invalid_target_or_revision"),
			TEXT("A canonical loaded target and complete exact expected_revision are required."));
	}
	if (!IsUserParameterName(Request.Rename.OldName)
		|| !IsUserParameterName(Request.Rename.NewName)
		|| Request.Rename.OldName == Request.Rename.NewName
		|| !IsCanonicalSha256(Request.Rename.ExpectedTypeFingerprint))
	{
		return Reject(TEXT("invalid_rename_identity"),
			TEXT("Rename requires distinct canonical User.* names and an exact type fingerprint."));
	}
	FGuid ExpectedGuid;
	if (!FGuid::ParseExact(Request.Rename.ExpectedVariableGuid,
		EGuidFormats::DigitsWithHyphensLower, ExpectedGuid) || !ExpectedGuid.IsValid()
		|| CanonicalGuid(ExpectedGuid) != Request.Rename.ExpectedVariableGuid)
	{
		return Reject(TEXT("invalid_variable_guid"),
			TEXT("expected_variable_guid must be one nonzero lowercase hyphenated GUID."));
	}
	if (Request.DeadlineMs < MinAsyncDeadlineMs || Request.DeadlineMs > MaxAsyncDeadlineMs
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Async deadline, game-thread work, or output bounds are outside the closed contract."));
	}
	if (!Request.ExpectedPlanHash.IsEmpty() && !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		return Reject(TEXT("invalid_expected_plan_hash"),
			TEXT("A supplied expected_plan_hash must be canonical even though v1 cannot stage it."));
	}
	if (!Request.bDryRun
		&& !FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		return Reject(TEXT("invalid_operation_id"),
			TEXT("Non-dry intent requires a valid durable-journal operation_id."));
	}

	const double Started = FPlatformTime::Seconds();
	auto DeadlineExceeded = [&]()
	{
		return (FPlatformTime::Seconds() - Started) * 1000.0
			> Request.MaxGameThreadMs;
	};
	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.BaseRevision = Snapshot.Health.Revision;
	int64 EstimatedOutputBytes = 1024ll + EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAINiagaraIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= MaxIssues
			|| EstimatedOutputBytes + IssueBytes > Request.MaxOutputBytes)
		{
			return Reject(TEXT("issue_or_output_bound_exceeded"),
				TEXT("Exact rename planning exceeded the closed issue or output bound."));
		}
		EstimatedOutputBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	if (!Snapshot.bComplete || !Snapshot.Health.bRevisionComplete)
	{
		return Reject(TEXT("revision_incomplete"),
			TEXT("Rename dry-run requires complete loaded and on-disk CAS evidence."));
	}
	if (Snapshot.Health.Revision != Request.ExpectedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The exact loaded Niagara System changed after inspection."));
	}
	if (Snapshot.Health.bPackageDirty || !Snapshot.Health.bExistsOnDisk
		|| Snapshot.Health.DiskExistence != TEXT("exists"))
	{
		return Reject(TEXT("persisted_clean_base_required"),
			TEXT("Rename preparation requires one clean saved base with proven Asset Registry presence."));
	}
	if (!Snapshot.Health.bCompileStateKnown || Snapshot.Health.bCompileActive
		|| Snapshot.Health.bCompileStale || Snapshot.Health.bCompileHasErrors)
	{
		return Reject(TEXT("compile_not_clean_terminal"),
			TEXT("Rename preparation never waits for compilation and requires a clean terminal compile."));
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(
		FSoftObjectPath(Request.TargetPath).ResolveObject());
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = GetExistingSystemViewModel(System);
	if (!System || !ViewModel.IsValid() || !Snapshot.Health.bExistingSystemViewModel)
	{
		return Reject(TEXT("existing_system_view_model_required"),
			TEXT("The exact System must already be open; HyperAI never creates or opens a Niagara ViewModel."));
	}

	const FHyperAINiagaraUserParameterIdentity* Parameter = nullptr;
	FString RenameIdentityError;
	if (!ValidateRenameIdentityAgainstSnapshot(
		Snapshot.Parameters, Request.Rename, Parameter, RenameIdentityError))
	{
		return Reject(*RenameIdentityError,
			TEXT("The asserted GUID/name/type identity or unique destination no longer matches the exact snapshot."));
	}

	TArray<UNiagaraScript*> Scripts;
	TArray<UNiagaraGraph*> Graphs;
	int32 GraphNodeCount = 0;
	if (!CollectReachableScripts(*System, Scripts)
		|| !CollectReachableGraphs(Scripts, Graphs, GraphNodeCount))
	{
		return Reject(TEXT("reference_proof_bound_exceeded"),
			TEXT("Reachable script or graph bounds prevent complete rename collision proof."));
	}
	bool bDestinationReferenced = false;
	if (!GraphContainsParameterName(Graphs, Request.Rename.NewName,
		bDestinationReferenced) || bDestinationReferenced)
	{
		return Reject(TEXT("destination_reference_collision"),
			TEXT("The destination name already occurs in an exact reachable Niagara graph."));
	}
	if (DeadlineExceeded())
	{
		return Reject(TEXT("planning_deadline_exceeded"),
			TEXT("Bounded rename preflight exceeded its game-thread work budget."));
	}
	// UE 5.8's actual rename closure also touches the editor-only parameter adapter,
	// reflected FNiagaraUserParameterBinding fields, renderer attribute bindings, and
	// System/Emitter HandleVariableRenamed domains (including renderer/simulation-stage
	// state). The public upstream discovery helpers materialize arrays and reflect fields
	// before a caller can cap them, so they cannot serve as hard-bounded proof here.
	const bool bFullReferenceInventoryComplete = AreFullReferenceDomainsProven(
		/*bExposedStore=*/true,
		/*bGraphMetadataAndPins=*/true,
		/*bEditorOnlyParameterAdapter=*/false,
		/*bUserParameterBindings=*/false,
		/*bRendererAttributeBindings=*/false,
		/*bSystemEmitterHandleRenameClosure=*/false,
		/*bReflectionAndCollectionsBoundedBeforeMaterialization=*/false);
	if (bFullReferenceInventoryComplete)
	{
		return Reject(TEXT("reference_inventory_contract_error"),
			TEXT("The v1 source cohort cannot admit a full-reference proof without a new bounded immutable inventory implementation and fixtures."));
	}
	if (Request.bDryRun)
	{
		return Reject(FullReferenceInventoryState,
			TEXT("Store and graph collision checks passed, but UE 5.8 rename also reaches editor-only parameters, user bindings, renderer attribute bindings, and system/emitter rename closure. Those domains lack a bounded immutable inventory, so dry-run validity is not claimed."));
	}
	return Reject(NonDryCallableState,
		TEXT("No Niagara mutation was prepared, staged, or submitted. In addition to the generic async continuation host, execution requires a bounded immutable full-reference inventory; upstream reflection/binding helpers materialize results before caller bounds."));
}

FHyperAINiagaraInspectReport UHyperAIStudioNiagaraToolset::hyper_niagara_inspect(
	const FHyperAINiagaraInspectRequest& Request)
{
	return FHyperAIStudioNiagaraContracts::Inspect(Request);
}

FHyperAINiagaraApplyPlanReport UHyperAIStudioNiagaraToolset::hyper_niagara_apply_plan(
	const FHyperAINiagaraApplyPlanRequest& Request)
{
	return FHyperAIStudioNiagaraContracts::BuildPlan(Request);
}

FHyperAINiagaraValidateReport UHyperAIStudioNiagaraToolset::hyper_niagara_validate(
	const FHyperAINiagaraValidateRequest& Request)
{
	return FHyperAIStudioNiagaraContracts::Validate(Request);
}

FHyperAIStudioNiagaraDomainAdapter::FHyperAIStudioNiagaraDomainAdapter()
	: Descriptor(FHyperAIStudioNiagaraContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioNiagaraDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioNiagaraDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Niagara adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_niagara_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioNiagaraContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioNiagaraContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioNiagaraContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioNiagaraInspectPayload& Typed =
			static_cast<const FHyperAIStudioNiagaraInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioNiagaraInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioNiagaraInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioNiagaraContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_niagara_validate")
		&& Context.Binding.VariantId == FHyperAIStudioNiagaraContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioNiagaraContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioNiagaraContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioNiagaraValidatePayload& Typed =
			static_cast<const FHyperAIStudioNiagaraValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioNiagaraValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioNiagaraValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioNiagaraContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_niagara_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioNiagaraContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioNiagaraContracts::RenamePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioNiagaraContracts::RenamePayloadSchemaFingerprint())
	{
		const FHyperAIStudioNiagaraRenamePayload& Typed =
			static_cast<const FHyperAIStudioNiagaraRenamePayload&>(Payload);
		if (FHyperAIStudioNiagaraContracts::ComputeRenameSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The exact typed rename semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioNiagaraContracts::NonDryCallableState,
			TEXT("No Niagara effect ran. This synchronous adapter cannot begin, poll, cancel, compile, save, validate, or reconcile a rename."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Niagara adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FHyperAIStudioNiagaraFreshVerifier::FHyperAIStudioNiagaraFreshVerifier(
	FString InOwnerAdapterFingerprint)
	: OwnerAdapterFingerprint(MoveTemp(InOwnerAdapterFingerprint))
{
}

FString FHyperAIStudioNiagaraFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return OwnerAdapterFingerprint;
}

bool FHyperAIStudioNiagaraFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != FHyperAIStudioNiagaraContracts::RenamePayloadTypeId
		|| Request.GetSchemaFingerprint()
			!= FHyperAIStudioNiagaraContracts::RenamePayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed Niagara request schema.");
		return false;
	}
	const FHyperAIStudioNiagaraRenamePayload& Typed =
		static_cast<const FHyperAIStudioNiagaraRenamePayload&>(Request);
	if (!FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(Typed.TargetPath)
		|| FHyperAIStudioNiagaraContracts::ComputeRenameSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetPath;
	return true;
}

bool FHyperAIStudioNiagaraFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	using namespace HyperAIStudio::Niagara::Private;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh Niagara verification requires the game thread.");
		return false;
	}
	const TArray<FHyperAINiagaraCapabilityStatus> Capabilities =
		FHyperAIStudioNiagaraContracts::GetCapabilityMatrix();
	if (Capabilities.Num() != 1 || !Capabilities[0].bIndependentValidationImplemented)
	{
		OutError = TEXT("native_rule_execution_not_hard_bounded");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != FHyperAIStudioNiagaraContracts::MutationResultTypeId
		|| Result.GetSchemaFingerprint()
			!= FHyperAIStudioNiagaraContracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioNiagaraRenamePayload& Typed =
		static_cast<const FHyperAIStudioNiagaraRenamePayload&>(Request);
	const FHyperAIStudioNiagaraMutationResultPayload& TypedResult =
		static_cast<const FHyperAIStudioNiagaraMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !TypedResult.bValid
		|| !FHyperAIStudioNiagaraContracts::IsCanonicalSha256(TypedResult.Revision))
	{
		OutError = TEXT("Only a terminal completed async result may enter fresh verification.");
		return false;
	}
	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	if (!FHyperAIStudioNiagaraContracts::CaptureExact(CanonicalTarget,
		FHyperAIStudioNiagaraContracts::MaxMutationGameThreadMs,
		Snapshot, Status, Diagnostic)
		|| !Snapshot.bComplete || Snapshot.Health.Revision == Typed.BaseRevision
		|| Snapshot.Health.Revision != TypedResult.Revision
		|| Snapshot.Health.bPackageDirty || Snapshot.Health.bCompileActive
		|| Snapshot.Health.bCompileStale || Snapshot.Health.bCompileHasErrors)
	{
		OutError = TEXT("Post-mutation exact saved CAS or terminal compile proof is missing.");
		return false;
	}
	const FHyperAINiagaraUserParameterIdentity* Current =
		FindParameterByGuid(Snapshot.Parameters, Typed.ExpectedVariableGuid);
	if (!Current || Current->Name != Typed.NewName
		|| Current->TypeFingerprint != Typed.TypeFingerprint
		|| Current->DefaultFingerprint != Typed.DefaultFingerprint
		|| Snapshot.Parameters.ContainsByPredicate([&](const FHyperAINiagaraUserParameterIdentity& Item)
		{
			return Item.Name == Typed.OldName;
		}))
	{
		OutError = TEXT("Fresh state does not prove one GUID-stable, type/default-preserving rename.");
		return false;
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.niagara.rename-postcondition.v1"));
	AppendToken(Canonical, Typed.SemanticFingerprint);
	AppendToken(Canonical, Snapshot.Health.Revision);
	AppendToken(Canonical, Current->VariableGuid);
	AppendToken(Canonical, Current->Name);
	AppendToken(Canonical, Current->TypeFingerprint);
	AppendToken(Canonical, Current->DefaultFingerprint);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!FHyperAIStudioNiagaraContracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioNiagaraRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioNiagaraRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioNiagaraRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioNiagaraRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioNiagaraContracts::IsRegistrationAllowed(bDev)
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioNiagaraToolset::StaticClass(),
			FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioNiagaraRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioNiagaraRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioNiagaraContracts::IsRegistrationAllowed(bDev))
	{
		UE_LOG(LogHyperAIStudioNiagara, Verbose,
			TEXT("Niagara exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("Niagara"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor"))
		|| !HyperAIStudio::Niagara::Private::GetLoadedNiagaraEditorModule())
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioNiagaraDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioNiagara, Error,
			TEXT("Niagara adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioNiagaraContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioNiagara, Error,
			TEXT("Niagara live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("Niagara"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor"))
		&& HyperAIStudio::Niagara::Private::GetLoadedNiagaraEditorModule() != nullptr
		&& UNiagaraSystem::StaticClass() != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("Niagara and NiagaraEditor are already loaded; no module was loaded by the probe.")
		: TEXT("Niagara source cohort remains unavailable without already-loaded required modules.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioNiagara, Error,
			TEXT("Niagara live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioNiagaraToolset::StaticClass(),
		FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioNiagara, Error,
			TEXT("Niagara three-tool cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioNiagaraRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioNiagaraToolset::StaticClass(),
			FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioNiagara, Error,
				TEXT("Niagara owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		else
		{
			bOwnsToolset = false;
		}
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioNiagara, Error,
				TEXT("Niagara probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioNiagara, Error,
				TEXT("Niagara adapter rollback failed closed: %s"), *Error);
		}
		else
		{
			AdapterHandle = {};
			Adapter.Reset();
		}
	}
}
