// Games by Hyper 2026.

#include "HyperAIStudioNiagaraToolset.h"
#include "HyperAIStudioAgentActivity.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HyperAIStudioNiagaraAssetsGate.h"
#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
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
		int32 Size = 256 + 2 * (Issue.Code.Len() + Issue.Severity.Len() + Issue.StableId.Len()
			+ Issue.RuleClass.Len() + Issue.SourcePath.Len() + Issue.Summary.Len()
			+ Issue.Description.Len());
		for (const FString& FixId : Issue.FixIds)
		{
			Size += 16 + 2 * FixId.Len();
		}
		return Size;
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

	int32 EstimateTopologyBytes(const FHyperAINiagaraEmitterTopology& Emitter)
	{
		int64 Size = 128 + 2ll * (Emitter.EmitterName.Len() + Emitter.SimTarget.Len());
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			Size += 64 + 2ll * Stack.ScriptName.Len();
			for (const FHyperAINiagaraModuleTopology& Module : Stack.Modules)
			{
				Size += 96 + 2ll * (Module.ModuleName.Len() + Module.ScriptAssetPath.Len());
				for (const FString& Input : Module.InputNames)
				{
					Size += 16 + 2ll * Input.Len();
				}
			}
		}
		for (const FHyperAINiagaraRendererTopology& Renderer : Emitter.Renderers)
		{
			Size += 48 + 2ll * Renderer.RendererClassPath.Len();
		}
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	/** Hash of saved package identity plus dirty state: stable across async compile completion. */
	FString ContentKeyOf(const UNiagaraSystem& System);

	/** The System plus every asset the plan created or edited beside it, in op order. */
	FString PlanContentKeyOf(const UNiagaraSystem& System, const TArray<FHyperAINiagaraEditOp>& Ops)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.plan-content.v1"));
		AppendToken(Canonical, ContentKeyOf(System));
		for (const FString& Path : HyperAIStudio::Niagara::AssetsGate::SideAssetPaths(Ops))
		{
			const UObject* Asset = FSoftObjectPath(Path).ResolveObject();
			const UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
			AppendToken(Canonical, Path);
			AppendToken(Canonical, Package ? LexToString(Package->GetSavedHash()) : FString(TEXT("missing")));
			AppendToken(Canonical, Package && Package->IsDirty() ? TEXT("dirty") : TEXT("clean"));
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString ContentKeyOf(const UNiagaraSystem& System)
	{
		const UPackage* Package = System.GetOutermost();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.niagara.content-key.v1"));
		AppendToken(Canonical, System.GetPathName());
		AppendToken(Canonical, Package ? LexToString(Package->GetSavedHash()) : FString());
		AppendToken(Canonical, Package && Package->IsDirty() ? TEXT("dirty") : TEXT("clean"));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	/**
	 * One typed-executor phase. Order is fixed by the core plan: Apply, Compile, Validate, Save, VerifyFresh.
	 * Any failure after Apply reports FailedAfterKnownEffect: the edit is in memory and undoable, not rolled back.
	 */
	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioNiagaraEditOpsPayload& Payload)
	{
		namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
		FHyperAIStudioDomainAdapterResult Result;
		const double PhaseStarted = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			// The executor enforces measured per-phase game-thread budgets; this shows which phase costs what.
			UE_LOG(LogHyperAIStudioNiagara, Log, TEXT("Edit phase %d finished %s in %.1f ms. %s"),
				static_cast<int32>(Phase), *Status, (FPlatformTime::Seconds() - PhaseStarted) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioNiagaraMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioNiagaraMutationResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->Revision = ContentKey;
				Output->bValid = true;
				Result.Payload = Output;
			}
			return Result;
		};
		const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
		const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		if (!IsInGameThread())
		{
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Niagara edits run on the game thread."));
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Payload.TargetPath).ResolveObject());
		if (!System)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), TEXT("The target System is no longer loaded."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Apply:
		{
			FHyperAIStudioNiagaraValueSnapshot Snapshot;
			FString Status;
			FString Diagnostic;
			if (!FHyperAIStudioNiagaraContracts::CaptureExact(Payload.TargetPath,
				FHyperAIStudioNiagaraContracts::MaxReadGameThreadMs, Snapshot, Status, Diagnostic)
				|| Snapshot.Health.Revision != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The System changed after planning; nothing was applied."));
			}
			if (GetExistingSystemViewModel(System).IsValid())
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_open_in_editor"),
					TEXT("The System was opened in the Niagara editor after planning; nothing was applied."));
			}
			int32 Applied = 0;
			TArray<FString> PerOpStatus;
			FString Error;
			if (!Gate::ApplyOps(*System, Payload.Ops, Applied, PerOpStatus, Error))
			{
				return Finish(Applied > 0
					? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect
					: EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect,
					TEXT("edit_op_failed"), Error);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Join(PerOpStatus, TEXT(", ")), TEXT("applied"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Compile:
		{
			// Never wait here: compilation takes seconds against a 250 ms phase budget. A job watches it instead,
			// so the Activity panel and hyper_operation_status show when it actually finished.
			System->RequestCompile(/*bForce=*/false);
			FHyperAIStudioAsyncJobRequest Job;
			Job.PackId = FHyperAIStudioNiagaraContracts::PackId;
			Job.ToolName = TEXT("hyper_niagara_apply_plan");
			Job.Target = Payload.TargetPath;
			Job.DeadlineMs = 5 * 60 * 1000;
			Job.PollIntervalMs = 250;
			FString JobId;
			FString JobError;
			FHyperAIStudioAsyncJobHost::Start(Job, [TargetPath = Payload.TargetPath](FString& OutProgress, FString& OutDiagnostic)
			{
				UNiagaraSystem* Compiling = Cast<UNiagaraSystem>(FSoftObjectPath(TargetPath).ResolveObject());
				if (!Compiling)
				{
					OutDiagnostic = TEXT("The System unloaded while compiling.");
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				if (Compiling->HasActiveCompilations())
				{
					OutProgress = TEXT("compiling");
					return EHyperAIStudioAsyncJobPoll::Running;
				}
				OutDiagnostic = TEXT("Compile finished; run hyper_niagara_validate for stack issues.");
				return EHyperAIStudioAsyncJobPoll::Completed;
			}, JobId, JobError);
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("compile_requested"),
				JobError.IsEmpty() ? TEXT("Compile requested; a job watches it to completion.") : JobError, TEXT("compile_requested"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			// Every Niagara edit queues a compile, and Epic refuses to read stack issues until it finishes, which can
			// take seconds. So this phase proves the edited System still captures structurally; stack issues and
			// compile results come from hyper_niagara_validate once compiling settles.
			FHyperAIStudioNiagaraValueSnapshot Snapshot;
			FString Status;
			FString Diagnostic;
			if (!FHyperAIStudioNiagaraContracts::CaptureExact(Payload.TargetPath,
				FHyperAIStudioNiagaraContracts::MaxReadGameThreadMs, Snapshot, Status, Diagnostic))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, Status,
					TEXT("The edited System no longer captures and was not saved; undo reverts it. ") + Diagnostic);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"),
				TEXT("Structure captured. Run hyper_niagara_validate after compiling finishes for stack issues."), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
		{
			TArray<UPackage*> Packages = {System->GetOutermost()};
			for (const FString& Path : HyperAIStudio::Niagara::AssetsGate::SideAssetPaths(Payload.Ops))
			{
				if (UObject* Asset = FSoftObjectPath(Path).ResolveObject())
				{
					Packages.AddUnique(Asset->GetOutermost());
				}
			}
			if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The edit applied but the package did not save; it remains in memory and can be undone."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Packages saved."), TEXT("saved"));
		}
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh content captured for verification."), TEXT("completed"), PlanContentKeyOf(*System, Payload.Ops));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
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
		TEXT("niagara.inspect.payload.v2|exact_loaded_target|include_topology|page_size|revision_cursor|work_ms|output_bytes"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.edit_ops.payload.v2|one_target|base_revision|ordered_closed_ops|compile|save|deep_clone|bounded"));
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
		TEXT("niagara.inspect.result.v2|health|revision|parameter_identity_page|emitter_topology|capabilities|issues|bounded"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.edit_ops.result.v1|phase|content_key|valid|bounded"));
	return Value;
}

FString FHyperAIStudioNiagaraContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("niagara.validate.result.v2|complete|valid|policy|revision|compile_state|stack_issues_with_fixes|counts|bounded"));
	return Value;
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

FString FHyperAIStudioNiagaraEditOpsPayload::GetTypeId() const
{
	return FHyperAIStudioNiagaraContracts::EditOpsPayloadTypeId;
}

FString FHyperAIStudioNiagaraEditOpsPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNiagaraContracts::EditOpsPayloadSchemaFingerprint();
}

int32 FHyperAIStudioNiagaraEditOpsPayload::GetBoundedByteSize() const
{
	int64 Size = 256 + 2ll * (TargetPath.Len() + BaseRevision.Len() + SemanticFingerprint.Len());
	for (const FHyperAINiagaraEditOp& Op : Ops)
	{
		Size += 64 + 2ll * (Op.Kind.Len() + Op.EmitterName.Len() + Op.ScriptName.Len() + Op.ModuleName.Len()
			+ Op.AssetPath.Len() + Op.Name.Len() + Op.ValueType.Len() + Op.Value.Len()
			+ Op.IssueId.Len() + Op.FixId.Len());
		for (const FString& InputName : Op.InputNameStack)
		{
			Size += 16 + 2ll * InputName.Len();
		}
	}
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioNiagaraEditOpsPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioNiagaraEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->Ops = Ops;
	Clone->bCompile = bCompile;
	Clone->bSave = bSave;
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
	for (const FHyperAINiagaraEmitterTopology& Emitter : Report.Emitters)
	{
		Size += HyperAIStudio::Niagara::Private::EstimateTopologyBytes(Emitter);
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
	Niagara.bIndependentValidationImplemented = true;
	Niagara.bMutationExecutionImplemented = true;
	Niagara.SupportedCases = {
		TEXT("exact_loaded_system_health_and_complete_cas"),
		TEXT("paged_stable_user_parameter_identity_tokens"),
		TEXT("emitter_script_module_input_renderer_topology"),
		TEXT("compile_state_and_stack_issues_with_fix_ids"),
		TEXT("batched_edit_ops_one_undo_step"),
		TEXT("journaled_apply_compile_validate_save_fresh_verify"),
		TEXT("effect_type_create_configure_and_assign"),
		TEXT("data_channel_create_and_inspect"),
		TEXT("sim_cache_capture_bake_and_golden_regression"),
		TEXT("effect_type_data_channel_sim_cache_inspect")};
	// Epic's NiagaraToolsets expose these as single calls. apply_plan adds batching, a revision check, one undo
	// step, and a durable journal on top of the same engine API; it does not replace them.
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
		TEXT("target_open_in_niagara_editor"),
		TEXT("destructive_removal_ops_until_server_grant_path_is_exercised"),
		TEXT("opaque_system_emitter_renderer_property_json"),
		TEXT("raw_hlsl_python_lua_or_script_text"),
		TEXT("scratchpad_authoring"),
		TEXT("module_reorder_through_unexported_editor_api"),
		TEXT("event_or_simulation_stage_or_version_authoring"),
		TEXT("module_or_function_script_authoring_no_exported_graph_api"),
		TEXT("module_script_schema_from_asset_not_exported"),
		TEXT("gpu_emitter_sim_cache_capture"),
		TEXT("link_style_stack_issue_fixes"),
		TEXT("broad_asset_scan_capture_or_dependency_walk"),
		TEXT("runtime_simulation_screenshot_or_actor_spawn"),
		TEXT("generic_reflection_property_dispatch")};
	Niagara.State = TEXT("source_candidate_external_edit_ops");
	Niagara.Remediation = TEXT("Inspect with bIncludeTopology, dry-run apply_plan, resubmit with operation_id and plan_hash, poll hyper_operation_status, then run hyper_niagara_validate once compiling finishes. Close the System's Niagara editor tab first. Planning may load referenced module scripts or emitter templates; it never loads the edited System. Removals stay on Epic's NiagaraToolsets.");
	return {Niagara};
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioNiagaraContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.niagara.external_edit.ue58");
		Value.SemanticVersion = TEXT("2.0.0");
		Value.AdapterVersion = 2;
		Value.Variants.Add({TEXT("hyper_niagara_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_niagara_apply_plan"), MutationVariantId,
			EditOpsPayloadTypeId, EditOpsPayloadSchemaFingerprint(), MutationResultTypeId,
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
	Health.EffectTypePath = HyperAIStudio::Niagara::AssetsGate::GetEffectTypePath(*System);
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
		const ENiagaraScriptCompileStatus CompileStatus = Script->GetVMExecutableData().LastCompileStatus;
		// A script not compiled for its emitter's sim target (a CPU emitter's GPU compute script, a GPU emitter's
		// particle VM scripts) never carries a status. Epic's GetSystemCompileState ranks Unknown lowest for the
		// same reason; treating it as "state unknown" rejected almost every real System.
		if (CompileStatus == ENiagaraScriptCompileStatus::NCS_Unknown)
		{
			continue;
		}
		++AuthoritativeCompileScriptCount;
		switch (CompileStatus)
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

	if (const UObject* Other = FSoftObjectPath(Request.TargetPath).ResolveObject(); Other && !Other->IsA<UNiagaraSystem>())
	{
		if (HyperAIStudio::Niagara::AssetsGate::DescribeAsset(*Other, Report.Asset))
		{
			Report.bOk = true;
			Report.bFreshCapture = true;
			Report.Status = TEXT("asset_described");
			Report.Diagnostic = TEXT("Effect types, data channels and sim caches are described, not revision-tracked.");
			return Report;
		}
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

	if (Request.bIncludeTopology)
	{
		UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).ResolveObject());
		TArray<FHyperAINiagaraEmitterTopology> Emitters;
		bool bTopologyTruncated = false;
		FString TopologyError;
		if (!IsInGameThread() || !System
			|| !HyperAIStudio::Niagara::ExternalEditGate::ReadTopology(*System, Emitters, bTopologyTruncated, TopologyError))
		{
			AddIssue(Report.Issues, TEXT("topology_unavailable"), TEXT("warning"),
				TEXT("Emitter topology could not be read."), TopologyError);
		}
		for (FHyperAINiagaraEmitterTopology& Emitter : Emitters)
		{
			const int32 EmitterBytes = EstimateTopologyBytes(Emitter);
			if (EstimatedBytes + EmitterBytes > Request.MaxOutputBytes)
			{
				bTopologyTruncated = true;
				break;
			}
			EstimatedBytes += EmitterBytes;
			Report.Emitters.Add(MoveTemp(Emitter));
		}
		Report.bTruncated |= bTopologyTruncated;
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
	auto Reject = [&](const FString& Status, const FString& Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Niagara validation reads editor state on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath))
	{
		return Reject(TEXT("invalid_target_path"),
			TEXT("target_path must be one canonical top-level /Game object path."));
	}
	if (Request.Policy == TEXT("sim_cache_capture"))
	{
		FHyperAINiagaraValidateReport Capture = HyperAIStudio::Niagara::AssetsGate::RunCapturePolicy(Request);
		Capture.Capabilities = Report.Capabilities;
		return Capture;
	}
	if (Request.Policy != TEXT("authoring") && Request.Policy != TEXT("runtime_ready"))
	{
		return Reject(TEXT("unsupported_validation_policy"),
			TEXT("policy must be authoring, runtime_ready or sim_cache_capture."));
	}
	if ((!Request.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Request.ExpectedRevision))
		|| Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds_or_revision"),
			TEXT("revision, issue, game-thread, or output bounds are outside the closed contract."));
	}

	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(CaptureStatus, CaptureDiagnostic);
	}
	Report.Revision = Snapshot.Health.Revision;
	Report.bRevisionComplete = Snapshot.Health.bRevisionComplete;
	Report.bFreshCapture = true;
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Report.Revision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The loaded Niagara System no longer matches expected_revision; inspect it again.") + FHyperAIStudioAgentActivityLog::DescribeLastChange(Request.TargetPath));
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).ResolveObject());
	HyperAIStudio::Niagara::ExternalEditGate::FDiagnostics Diagnostics;
	FString DiagnosticsError;
	if (!System || !HyperAIStudio::Niagara::ExternalEditGate::ReadDiagnostics(
		*System, Request.MaxIssues, Diagnostics, DiagnosticsError))
	{
		return Reject(TEXT("diagnostics_unavailable"),
			System ? DiagnosticsError : FString(TEXT("The target System is not loaded.")));
	}

	int64 EstimatedBytes = 1024ll + EstimateCapabilitiesBytes(Report.Capabilities);
	Report.bTruncated = Diagnostics.bTruncated;
	for (FHyperAINiagaraIssue& Issue : Diagnostics.Issues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(MoveTemp(Issue));
	}

	Report.ErrorCount = Diagnostics.ErrorCount;
	Report.WarningCount = Diagnostics.WarningCount;
	Report.InfoCount = Diagnostics.InfoCount;
	Report.bCompileStateKnown = Diagnostics.bCompileStateKnown;
	Report.bCompiling = Diagnostics.bCompiling;
	// Counts are taken before truncation, so a truncated list still yields a trustworthy verdict.
	Report.bComplete = Diagnostics.bCompileStateKnown && !Diagnostics.bCompiling;
	const bool bRuntimeReady = Request.Policy == TEXT("runtime_ready");
	Report.bValid = Report.bComplete && Report.ErrorCount == 0 && (!bRuntimeReady || Report.WarningCount == 0);
	Report.bOk = true;
	Report.Status = !Report.bComplete ? TEXT("compile_in_progress") : Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = !Report.bComplete
		? TEXT("The System is still compiling or its compile state is unknown; validate again once it finishes.")
		: Report.bValid
			? TEXT("No errors under this policy.")
			: TEXT("Issues listed below. Fixable stack issues carry fix_ids for an apply_stack_issue_fix op.");
	return Report;
}

FString FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(
	const FHyperAIStudioNiagaraEditOpsPayload& Payload)
{
	using namespace HyperAIStudio::Niagara::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.niagara.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, TEXT("hyper_niagara_apply_plan"));
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bCompile ? TEXT("compile") : TEXT("no_compile"));
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAINiagaraEditOp& Op : Payload.Ops)
	{
		AppendToken(Canonical, Op.Kind);
		AppendToken(Canonical, Op.EmitterName);
		AppendToken(Canonical, Op.ScriptName);
		AppendToken(Canonical, Op.ModuleName);
		AppendToken(Canonical, FString::FromInt(Op.InputNameStack.Num()));
		for (const FString& InputName : Op.InputNameStack)
		{
			AppendToken(Canonical, InputName);
		}
		AppendToken(Canonical, Op.AssetPath);
		AppendToken(Canonical, Op.Name);
		AppendToken(Canonical, Op.ValueType);
		AppendToken(Canonical, Op.Value);
		AppendToken(Canonical, Op.bEnabled ? TEXT("enabled") : TEXT("disabled"));
		AppendToken(Canonical, Op.IssueId);
		AppendToken(Canonical, Op.FixId);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAINiagaraApplyPlanReport FHyperAIStudioNiagaraContracts::BuildPlan(
	const FHyperAINiagaraApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Niagara::Private;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	FHyperAINiagaraApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const FString& Status, const FString& Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Niagara edit planning reads editor state on the game thread."));
	}
	if (Request.bDryRun
		&& (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun
		&& (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
			|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"),
			TEXT("A non-dry submission needs a valid operation_id and the plan_hash its dry run returned."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedRevision))
	{
		return Reject(TEXT("invalid_target_or_revision"),
			TEXT("A canonical loaded target and the exact revision from hyper_niagara_inspect are required."));
	}
	if (Request.Ops.IsEmpty() || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"),
			FString::Printf(TEXT("A plan needs between 1 and %d ops."), MaxOpsPerPlan));
	}
	// These mirror the shared typed-artifact contract bounds, which otherwise reject the plan with a generic error.
	if (Request.MaxGameThreadMs < 50
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("max_game_thread_ms must be 50-250 and max_output_bytes within the closed output bounds."));
	}
	if (Request.bCompile && !Request.bSave)
	{
		return Reject(TEXT("compile_requires_save"),
			TEXT("Compiling requires saving; set bSave true, or bCompile false to leave the edit unsaved."));
	}
	if (!Gate::IsApiAvailable())
	{
		return Reject(TEXT("external_edit_api_unavailable"),
			TEXT("This engine build does not provide UNiagaraExternalEditUtilities."));
	}
	TSet<FString> PlannedAssets;
	for (int32 Index = 0; Index < Request.Ops.Num(); ++Index)
	{
		const FHyperAINiagaraEditOp& Op = Request.Ops[Index];
		FString OpStatus;
		FString OpDiagnostic;
		// Resolving here loads referenced module scripts or emitter templates, never the System being edited.
		const bool bValid = HyperAIStudio::Niagara::AssetsGate::IsAssetOp(Op.Kind)
			? HyperAIStudio::Niagara::AssetsGate::ValidateAssetOp(Op, Request.TargetPath, PlannedAssets, OpStatus, OpDiagnostic)
			: Gate::ValidateOp(Op, /*bResolveAssets=*/true, OpStatus, OpDiagnostic);
		if (!bValid)
		{
			return Reject(OpStatus, FString::Printf(TEXT("Op %d: %s"), Index, *OpDiagnostic));
		}
		if (Op.Kind.StartsWith(TEXT("create_")) || Op.Kind == TEXT("bake_sim_cache"))
		{
			PlannedAssets.Add(Op.AssetPath);
		}
	}

	FHyperAIStudioNiagaraValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(CaptureStatus, CaptureDiagnostic);
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
				TEXT("Planning diagnostics exceeded the closed issue or output bound."));
		}
		EstimatedOutputBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	if (!Snapshot.bComplete || !Snapshot.Health.bRevisionComplete)
	{
		return Reject(TEXT("revision_incomplete"),
			TEXT("Planning requires complete loaded and on-disk revision evidence."));
	}
	if (Snapshot.Health.Revision != Request.ExpectedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The loaded Niagara System changed after inspection; inspect it again.") + FHyperAIStudioAgentActivityLog::DescribeLastChange(Request.TargetPath));
	}
	if (Snapshot.Health.bPackageDirty || !Snapshot.Health.bExistsOnDisk
		|| Snapshot.Health.DiskExistence != TEXT("exists"))
	{
		return Reject(TEXT("persisted_clean_base_required"),
			TEXT("Save or revert pending changes first: edits start from one clean saved System."));
	}
	if (!Snapshot.Health.bCompileStateKnown || Snapshot.Health.bCompileActive
		|| Snapshot.Health.bCompileStale || Snapshot.Health.bCompileHasErrors)
	{
		return Reject(TEXT("compile_not_clean_terminal"),
			TEXT("Planning never waits for compilation and needs a finished, error-free compile."));
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).ResolveObject());
	if (!System)
	{
		return Reject(TEXT("target_not_loaded"), TEXT("The target System is not loaded."));
	}
	if (GetExistingSystemViewModel(System).IsValid())
	{
		return Reject(TEXT("target_open_in_editor"),
			TEXT("Close the System's Niagara editor tab first. Edits run through a separate headless view model, and two view models over one System can overwrite each other."));
	}
	const TSharedRef<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BaseRevision = Snapshot.Health.Revision;
	Payload->Ops = Request.Ops;
	Payload->bCompile = Request.bCompile;
	Payload->bSave = Request.bSave;
	Payload->SemanticFingerprint = ComputeEditOpsSemanticFingerprint(*Payload);
	if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("payload_bound_exceeded"), TEXT("The ops exceed the bounded request size; split them into smaller plans."));
	}
	Report.SemanticFingerprint = Payload->SemanticFingerprint;

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = TEXT("hyper_niagara_apply_plan");
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Request.TargetPath;
	// Phases run one per tick; apply, compile, validate, save and verify share these operation-wide budgets.
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxOpsPerPlan;
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	// The host caps results at 256 bytes; the largest phase result ("completed" plus a hash) estimates at exactly 256.
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	Trusted.bCompileOnce = Request.bCompile;
	Trusted.bSaveOnce = Request.bSave;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioNiagaraFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioNiagaraFreshVerifier, ESPMode::ThreadSafe>(GetAdapterDescriptor().AdapterFingerprint);
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError)
		|| !Prepare.bPrepared)
	{
		FString Diagnostic = PrepareError.IsEmpty() ? Prepare.Status.Diagnostic : PrepareError;
		if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
		{
			Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
		}
		return Reject(Prepare.Status.StatusCode.IsEmpty() ? FString(TEXT("prepare_failed")) : Prepare.Status.StatusCode,
			Diagnostic);
	}

	Report.bTrustedPrepared = true;
	// The host's plan hash differs between preparations of identical intent, so review binds to the sealed semantic
	// fingerprint instead: target, base revision, ordered ops and flags. The host still seals its own staging.
	Report.PlanHash = Payload->SemanticFingerprint;
	Report.AuthorizationPlanHash = Prepare.PlanHash;
	Report.Effects.TargetCount = 1;
	Report.Effects.OpCount = Request.Ops.Num();
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bWouldTransactionOnce = true;
	Report.Effects.bWouldCompileOnce = Request.bCompile;
	Report.Effects.bWouldSaveOnce = Request.bSave;
	Report.Effects.bWouldValidateOnce = true;
	Report.Effects.bWouldFreshVerifyOnce = true;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = TEXT("Nothing changed. To apply, resubmit the same request with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("The plan changed since its dry run (ops, target state, or catalog). Dry-run again and review it."));
	}

	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = TEXT("hyper_niagara_apply_plan");
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.TargetPath;
		Summary.PlanHash = Payload->SemanticFingerprint;
		for (const FHyperAINiagaraEditOp& Op : Request.Ops)
		{
			Summary.Effects.Add(FString::Printf(TEXT("%s %s"), *Op.Kind,
				*FString::Join(TArray<FString>({Op.EmitterName, Op.ScriptName, Op.ModuleName, Op.Name}).FilterByPredicate(
					[](const FString& Part) { return !Part.IsEmpty(); }), TEXT(" / "))));
		}
		Summary.Touches.Add(Request.TargetPath);
		FString ApprovalError;
		if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, ApprovalError))
		{
			return Reject(TEXT("approval_not_queued"), ApprovalError);
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing changed yet. Poll hyper_operation_status with operation_id: it starts once approved.");
		return Report;
	}

	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic StageStatus;
	FString StageError;
	if (!FHyperAIStudioTrustedExecutionFacade::StageExact(Prepared, Request.OperationId, Receipt, StageStatus, StageError))
	{
		return Reject(StageStatus.StatusCode.IsEmpty() ? FString(TEXT("stage_failed")) : StageStatus.StatusCode,
			StageError.IsEmpty() ? StageStatus.Diagnostic : StageError);
	}
	Report.bStaged = true;

	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	FHyperAIStudioTrustedExecutionDiagnostic SubmitStatus;
	FString SubmitError;
	// Edit is tokenless; only risky safety classes need a server-issued grant.
	if (!FHyperAIStudioTrustedExecutionFacade::SubmitExact(Receipt, FString(), Submission, SubmitStatus, SubmitError))
	{
		Report.Status = SubmitStatus.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : SubmitStatus.StatusCode;
		Report.Diagnostic = SubmitError.IsEmpty() ? SubmitStatus.Diagnostic : SubmitError;
		return Report;
	}
	Report.bExecutionSubmitted = true;
	Report.bOk = true;
	Report.Status = TEXT("submitted");
	Report.Diagnostic = TEXT("The edit runs over the next editor ticks: apply, compile, validate, save, verify. Poll hyper_operation_status with operation_id until it is terminal.");
	return Report;
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
		&& Payload.GetTypeId() == FHyperAIStudioNiagaraContracts::EditOpsPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioNiagaraContracts::EditOpsPayloadSchemaFingerprint())
	{
		const FHyperAIStudioNiagaraEditOpsPayload& Typed =
			static_cast<const FHyperAIStudioNiagaraEditOpsPayload&>(Payload);
		if (FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::Niagara::Private::ExecuteEditPhase(Context.ActionKind, Typed);
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
	if (Request.GetTypeId() != FHyperAIStudioNiagaraContracts::EditOpsPayloadTypeId
		|| Request.GetSchemaFingerprint()
			!= FHyperAIStudioNiagaraContracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed Niagara request schema.");
		return false;
	}
	const FHyperAIStudioNiagaraEditOpsPayload& Typed =
		static_cast<const FHyperAIStudioNiagaraEditOpsPayload&>(Request);
	if (!FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(Typed.TargetPath)
		|| FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Typed)
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
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != FHyperAIStudioNiagaraContracts::MutationResultTypeId
		|| Result.GetSchemaFingerprint()
			!= FHyperAIStudioNiagaraContracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioNiagaraEditOpsPayload& Typed =
		static_cast<const FHyperAIStudioNiagaraEditOpsPayload&>(Request);
	const FHyperAIStudioNiagaraMutationResultPayload& TypedResult =
		static_cast<const FHyperAIStudioNiagaraMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !TypedResult.bValid
		|| !FHyperAIStudioNiagaraContracts::IsCanonicalSha256(TypedResult.Revision))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(CanonicalTarget).ResolveObject());
	if (!System)
	{
		OutError = TEXT("The edited System is no longer loaded.");
		return false;
	}
	// Content identity, not the compile-sensitive revision: an async compile may finish between capture and here.
	const FString ContentKey = PlanContentKeyOf(*System, Typed.Ops);
	if (ContentKey != TypedResult.Revision)
	{
		OutError = TEXT("The System changed between fresh capture and verification.");
		return false;
	}
	bool bAnyDirty = System->GetOutermost()->IsDirty();
	for (const FString& Path : HyperAIStudio::Niagara::AssetsGate::SideAssetPaths(Typed.Ops))
	{
		const UObject* Asset = FSoftObjectPath(Path).ResolveObject();
		bAnyDirty |= !Asset || Asset->GetOutermost()->IsDirty();
	}
	if (Typed.bSave == bAnyDirty)
	{
		OutError = Typed.bSave
			? TEXT("The System still has unsaved changes after the save phase.")
			: TEXT("The unsaved edit is no longer present in memory.");
		return false;
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.niagara.edit-ops-postcondition.v1"));
	AppendToken(Canonical, Typed.SemanticFingerprint);
	AppendToken(Canonical, ContentKey);
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
	const auto ObserveNiagaraEditor = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("Niagara"))
			&& FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor"))
			&& HyperAIStudio::Niagara::Private::GetLoadedNiagaraEditorModule() != nullptr
			&& UNiagaraSystem::StaticClass() != nullptr
			&& HyperAIStudio::Niagara::ExternalEditGate::IsApiAvailable();
		Observation.StatusCode = Observation.bReady
			? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
		Observation.Diagnostic = Observation.bReady
			? TEXT("Niagara, NiagaraEditor and the external-edit API are loaded; no module was loaded by the probe.")
			: TEXT("Niagara source cohort remains unavailable without already-loaded required modules.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult FirstObservation = ObserveNiagaraEditor();
	if (!FirstObservation.bReady || !ProbePublisher.Start(ProbeHandle, ObserveNiagaraEditor, Error))
	{
		if (Error.IsEmpty()) Error = FirstObservation.Diagnostic;
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
	ProbePublisher.Stop();
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
