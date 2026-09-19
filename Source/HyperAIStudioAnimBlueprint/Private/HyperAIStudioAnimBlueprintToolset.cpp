// Games by Hyper 2026.

#include "HyperAIStudioAnimBlueprintToolset.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAnimBlueprintCookbook.h"
#include "HyperAIStudioAnimBlueprintGate.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimBlueprint, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioAnimBlueprint"

namespace HyperAIStudio::AnimBlueprint::Private
{
	namespace Gate = HyperAIStudio::AnimBlueprint::Gate;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	int64 EstimateBytes(const FString& Value)
	{
		return 16 + 2ll * Value.Len();
	}

	int64 EstimateMachineBytes(const FHyperAIAnimBlueprintMachine& Machine)
	{
		int64 Size = 48 + EstimateBytes(Machine.Name);
		for (const FHyperAIAnimBlueprintState& State : Machine.States)
		{
			Size += 48 + EstimateBytes(State.Name) + EstimateBytes(State.Player) + EstimateBytes(State.Asset)
				+ EstimateBytes(State.XVariable) + EstimateBytes(State.YVariable);
		}
		for (const FHyperAIAnimBlueprintTransition& Transition : Machine.Transitions)
		{
			Size += 32 + EstimateBytes(Transition.From) + EstimateBytes(Transition.To) + EstimateBytes(Transition.Rule);
		}
		return Size;
	}

	int64 EstimateIssueBytes(const FHyperAIAnimBlueprintIssue& Issue)
	{
		return 32 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Subject) + EstimateBytes(Issue.Message);
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioAnimBlueprintEditOpsPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioAnimBlueprint, Log, TEXT("Edit phase %d finished %s in %.1f ms. %s"), static_cast<int32>(Phase), *Status,
				(FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioAnimBlueprintMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioAnimBlueprintMutationResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->ContentKey = ContentKey;
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Animation Blueprint edits run on the game thread."));
		}
		UAnimBlueprint* Blueprint = Gate::Resolve(Payload.TargetPath, /*bLoad=*/false);
		if (bApply)
		{
			FString Error;
			USkeleton* Skeleton = nullptr;
			UClass* Parent = nullptr;
			if (Payload.bCreate)
			{
				Skeleton = Gate::ResolveSkeleton(Payload.SkeletonPath);
				Parent = Gate::ResolveParentClass(Payload.ParentClassPath, Error);
				if (Blueprint || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Payload.TargetPath)) || !Skeleton || !Parent)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("create_precondition_changed"),
						TEXT("The target appeared, or the skeleton or parent class went away, after planning; nothing was created."));
				}
			}
			else if (!Blueprint || Gate::ComputeRevision(*Blueprint) != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The Animation Blueprint changed or unloaded after planning; nothing was applied."));
			}
			else if (Gate::IsOpenInEditor(*Blueprint))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_open_in_editor"),
					TEXT("The Animation Blueprint was opened in its editor after planning; nothing was applied."));
			}
			{
				const FScopedTransaction Transaction(Payload.bCreate
					? LOCTEXT("CreateAnimBlueprint", "HyperAI: Create Animation Blueprint")
					: LOCTEXT("EditAnimBlueprint", "HyperAI: Edit Animation Blueprint"));
				if (Payload.bCreate)
				{
					Blueprint = Gate::Create(Payload.TargetPath, *Skeleton, *Parent, Error);
					if (!Blueprint)
					{
						return Finish(EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect, TEXT("create_failed"), Error);
					}
				}
				if (!Gate::ApplyOps(*Blueprint, Payload.Ops, Error))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"), Error);
				}
			}
			// Compiled here, outside the undo step: this phase has the budget for it, and the executor schedules a
			// separate compile phase only for plans that also save.
			FString FirstError;
			int32 Errors = 0;
			int32 Warnings = 0;
			if (!Gate::Compile(*Blueprint, FirstError, Errors, Warnings))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("compile_failed"),
					FString::Printf(TEXT("%d compile errors, first: %s The edit is in memory and was not saved; undo reverts it."), Errors, *FirstError));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%s%d ops applied, compiled with %d warnings."), Payload.bCreate ? TEXT("Created; ") : TEXT(""),
					Payload.Ops.Num(), Warnings), TEXT("applied"));
		}
		if (!Blueprint)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), TEXT("The Animation Blueprint is no longer loaded."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
			if (Blueprint->Status == BS_Error)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("compile_error"),
					TEXT("The Blueprint is in an error state after the edit; undo reverts it."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("The Blueprint compiles."), TEXT("validated"));
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SavePackages({Blueprint->GetOutermost()}, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The edit applied but the package did not save; it remains in memory."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh graph captured for verification."), TEXT("completed"), Gate::ComputeRevision(*Blueprint));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

FString FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAnimBlueprint.HyperAIStudioAnimBlueprintToolset");
}

TArray<FString> FHyperAIStudioAnimBlueprintContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, ValidateToolName};
}

bool FHyperAIStudioAnimBlueprintContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioAnimBlueprintContracts::GetCapabilities()
{
	return {
		TEXT("create_anim_blueprint_for_skeleton"),
		TEXT("bool_and_float_variables_with_per_frame_drivers_speed_is_falling_is_crouching"),
		TEXT("state_machines_driving_the_output_pose"),
		TEXT("states_playing_sequences_or_blend_spaces_bound_to_variables"),
		TEXT("transitions_with_variable_rules_or_time_remaining_and_blend_times"),
		TEXT("compile_with_errors_reported_as_one_undo_step"),
		TEXT("checks_entry_reachability_dead_ends_hysteresis_blend_times_skeletons"),
		TEXT("unsupported:linked_anim_layers_pose_search_retargeting_montage_slots_and_custom_nodes")};
}

bool FHyperAIStudioAnimBlueprintContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (Character < 0x20 || Character == 0x7F) return false;
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty() && FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioAnimBlueprintContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioAnimBlueprintContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("anim_blueprint.inspect.payload.v1|exact_target|optional_load|output_bytes"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("anim_blueprint.edit_ops.payload.v1|one_target|base_revision_or_create|skeleton|parent|ordered_closed_ops|compile|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("anim_blueprint.validate.payload.v1|exact_target|output_bytes"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("anim_blueprint.inspect.result.v1|revision|skeleton|parent|compile_status|variables_drivers|machines_states_transitions|bounded"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("anim_blueprint.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("anim_blueprint.validate.result.v1|compile_status|issues|counts|bounded"));
	return Value;
}

FString FHyperAIStudioAnimBlueprintInspectPayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::InspectPayloadTypeId; }
FString FHyperAIStudioAnimBlueprintInspectPayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::InspectPayloadSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::AnimBlueprint::Private::ClampBytes(64 + 2ll * Request.TargetPath.Len());
}

FString FHyperAIStudioAnimBlueprintValidatePayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::ValidatePayloadTypeId; }
FString FHyperAIStudioAnimBlueprintValidatePayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::ValidatePayloadSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintValidatePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::AnimBlueprint::Private::ClampBytes(64 + 2ll * Request.TargetPath.Len());
}

FString FHyperAIStudioAnimBlueprintEditOpsPayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::EditOpsPayloadTypeId; }
FString FHyperAIStudioAnimBlueprintEditOpsPayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::EditOpsPayloadSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::AnimBlueprint::Private::EstimateBytes;
	int64 Size = 160 + EstimateBytes(TargetPath) + EstimateBytes(BaseRevision) + EstimateBytes(SkeletonPath)
		+ EstimateBytes(ParentClassPath) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAIAnimBlueprintOp& Op : Ops)
	{
		Size += 32 + EstimateBytes(Op.Kind) + EstimateBytes(Op.Machine) + EstimateBytes(Op.Name) + EstimateBytes(Op.To) + EstimateBytes(Op.Asset)
			+ EstimateBytes(Op.Rule) + EstimateBytes(Op.Value) + EstimateBytes(Op.XVariable) + EstimateBytes(Op.YVariable);
	}
	return HyperAIStudio::AnimBlueprint::Private::ClampBytes(Size);
}
FString FHyperAIStudioAnimBlueprintEditOpsPayload::GetSemanticFingerprint() const { return SemanticFingerprint; }
TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioAnimBlueprintEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioAnimBlueprintEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioAnimBlueprintEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->bCreate = bCreate;
	Clone->SkeletonPath = SkeletonPath;
	Clone->ParentClassPath = ParentClassPath;
	Clone->Ops = Ops;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioAnimBlueprintInspectResultPayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::InspectResultTypeId; }
FString FHyperAIStudioAnimBlueprintInspectResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::InspectResultSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::AnimBlueprint::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.Revision);
	for (const FHyperAIAnimBlueprintVariable& Variable : Report.Variables) Size += 32 + EstimateBytes(Variable.Name) + EstimateBytes(Variable.Driver);
	for (const FHyperAIAnimBlueprintMachine& Machine : Report.Machines) Size += EstimateMachineBytes(Machine);
	return ClampBytes(Size);
}

FString FHyperAIStudioAnimBlueprintValidateResultPayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::ValidateResultTypeId; }
FString FHyperAIStudioAnimBlueprintValidateResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::ValidateResultSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::AnimBlueprint::Private;
	int64 Size = 256 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic);
	for (const FHyperAIAnimBlueprintIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return ClampBytes(Size);
}

FString FHyperAIStudioAnimBlueprintMutationResultPayload::GetTypeId() const { return FHyperAIStudioAnimBlueprintContracts::MutationResultTypeId; }
FString FHyperAIStudioAnimBlueprintMutationResultPayload::GetSchemaFingerprint() const { return FHyperAIStudioAnimBlueprintContracts::MutationResultSchemaFingerprint(); }
int32 FHyperAIStudioAnimBlueprintMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::AnimBlueprint::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioAnimBlueprintContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.anim_blueprint.graph.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId, EditOpsPayloadTypeId, EditOpsPayloadSchemaFingerprint(),
			MutationResultTypeId, MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({ValidateToolName, ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIAnimBlueprintInspectReport FHyperAIStudioAnimBlueprintContracts::Inspect(const FHyperAIAnimBlueprintInspectRequest& Request)
{
	using namespace HyperAIStudio::AnimBlueprint::Private;
	FHyperAIAnimBlueprintInspectReport Report;
	Report.Capabilities = GetCapabilities();
	Report.TargetPath = Request.TargetPath;
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Animation Blueprint inspection runs on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath) || Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass one canonical /Game object path, e.g. /Game/Characters/ABP_Hero.ABP_Hero, and max_output_bytes 16-128 KB."));
	}
	const UAnimBlueprint* Blueprint = Gate::Resolve(Request.TargetPath, Request.bLoad);
	if (!Blueprint)
	{
		const bool bExists = FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath));
		return Reject(bExists ? TEXT("target_not_loaded") : TEXT("target_not_found"), bExists
			? FString(TEXT("The asset exists but is not a loaded Animation Blueprint; inspect with bLoad."))
			: FString(TEXT("No Animation Blueprint exists there; create one with apply_plan bCreate.")));
	}
	Report.Revision = Gate::ComputeRevision(*Blueprint);
	Report.Skeleton = Blueprint->TargetSkeleton ? Blueprint->TargetSkeleton->GetPathName() : FString();
	Report.ParentClass = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
	Report.CompileStatus = Gate::CompileStatusOf(*Blueprint);
	Report.bOpenInEditor = Gate::IsOpenInEditor(*Blueprint);
	Report.bPackageDirty = Blueprint->GetOutermost()->IsDirty();
	TArray<FHyperAIAnimBlueprintMachine> Machines;
	Gate::Read(*Blueprint, Report.Variables, Machines);
	int64 Budget = 2048;
	for (const FHyperAIAnimBlueprintVariable& Variable : Report.Variables) Budget += 32 + EstimateBytes(Variable.Name) + EstimateBytes(Variable.Driver);
	for (FHyperAIAnimBlueprintMachine& Machine : Machines)
	{
		const int64 Bytes = EstimateMachineBytes(Machine);
		if (Budget + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		Budget += Bytes;
		Report.Machines.Add(MoveTemp(Machine));
	}
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = FString::Printf(TEXT("%d variables, %d state machines, compile status %s%s."), Report.Variables.Num(), Report.Machines.Num(),
		*Report.CompileStatus, Report.bOpenInEditor ? TEXT("; open in its editor, so close it before applying edits") : TEXT(""));
	return Report;
}

FHyperAIAnimBlueprintValidateReport FHyperAIStudioAnimBlueprintContracts::Validate(const FHyperAIAnimBlueprintValidateRequest& Request)
{
	using namespace HyperAIStudio::AnimBlueprint::Private;
	FHyperAIAnimBlueprintValidateReport Report;
	Report.Capabilities = GetCapabilities();
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Animation Blueprint validation runs on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath) || Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_request"), TEXT("Pass one canonical /Game object path and max_output_bytes 16-128 KB."));
	}
	const UAnimBlueprint* Blueprint = Gate::Resolve(Request.TargetPath, /*bLoad=*/false);
	if (!Blueprint)
	{
		return Reject(TEXT("target_not_loaded"), TEXT("The Animation Blueprint is not loaded; inspect it with bLoad first."));
	}
	Report.CompileStatus = Gate::CompileStatusOf(*Blueprint);
	int64 Budget = 1024;
	for (const FHyperAIAnimBlueprintIssue& Issue : Gate::Check(*Blueprint))
	{
		(Issue.Severity == TEXT("error") ? Report.ErrorCount : Report.WarningCount) += 1;
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (Budget + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		Budget += Bytes;
		Report.Issues.Add(Issue);
	}
	Report.bOk = true;
	Report.Status = Report.ErrorCount > 0 ? TEXT("invalid") : Report.WarningCount > 0 ? TEXT("valid_with_warnings") : TEXT("valid");
	Report.Diagnostic = FString::Printf(TEXT("%d errors, %d warnings; compile status %s."), Report.ErrorCount, Report.WarningCount, *Report.CompileStatus);
	return Report;
}

FString FHyperAIStudioAnimBlueprintContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioAnimBlueprintEditOpsPayload& Payload)
{
	using HyperAIStudio::AnimBlueprint::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.anim_blueprint.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bCreate ? TEXT("create") : TEXT("edit"));
	AppendToken(Canonical, Payload.SkeletonPath);
	AppendToken(Canonical, Payload.ParentClassPath);
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAIAnimBlueprintOp& Op : Payload.Ops)
	{
		for (const FString* Field : {&Op.Kind, &Op.Machine, &Op.Name, &Op.To, &Op.Asset, &Op.Rule, &Op.Value, &Op.XVariable, &Op.YVariable})
		{
			AppendToken(Canonical, *Field);
		}
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIAnimBlueprintApplyPlanReport FHyperAIStudioAnimBlueprintContracts::BuildPlan(const FHyperAIAnimBlueprintApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::AnimBlueprint::Private;
	FHyperAIAnimBlueprintApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.TargetPath = Request.TargetPath;
	Report.Capabilities = GetCapabilities();
	auto Reject = [&](const FString& Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Animation Blueprint planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId) || !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), TEXT("A non-dry submission needs a valid operation_id and the plan_hash its dry run returned."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (Request.bCreate ? !Request.ExpectedRevision.IsEmpty() : !IsCanonicalSha256(Request.ExpectedRevision)))
	{
		return Reject(TEXT("invalid_target_or_revision"), TEXT("Pass a canonical /Game object path, plus the revision from inspect when editing, or none when bCreate."));
	}
	if ((Request.Ops.IsEmpty() && !Request.bCreate) || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs up to %d ops, and at least one unless it creates."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.SkeletonPath.Len() > MaxPathCharacters || Request.ParentClassPath.Len() > MaxPathCharacters)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes or a path is outside the closed bounds."));
	}

	const UAnimBlueprint* Blueprint = Gate::Resolve(Request.TargetPath, /*bLoad=*/false);
	const USkeleton* Skeleton = nullptr;
	FString Error;
	if (Request.bCreate)
	{
		if (Blueprint || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath)))
		{
			return Reject(TEXT("target_exists"), TEXT("An asset already exists at target_path; inspect and edit it instead."));
		}
		Skeleton = Gate::ResolveSkeleton(Request.SkeletonPath);
		if (!Skeleton)
		{
			return Reject(TEXT("invalid_skeleton"), TEXT("Creating needs skeleton_path naming a Skeleton asset."));
		}
		if (!Gate::ResolveParentClass(Request.ParentClassPath, Error))
		{
			return Reject(TEXT("invalid_parent_class"), Error);
		}
	}
	else
	{
		if (!Blueprint)
		{
			return Reject(TEXT("target_not_loaded"), TEXT("The Animation Blueprint is not loaded; inspect it with bLoad first."));
		}
		Report.BaseRevision = Gate::ComputeRevision(*Blueprint);
		if (Report.BaseRevision != Request.ExpectedRevision)
		{
			return Reject(TEXT("stale_revision"), TEXT("The Animation Blueprint changed after inspection; inspect it again.")
				+ FHyperAIStudioAgentActivityLog::DescribeLastChange(Request.TargetPath));
		}
		if (Gate::IsOpenInEditor(*Blueprint))
		{
			return Reject(TEXT("target_open_in_editor"), TEXT("Close the Animation Blueprint's editor tab first, so its graphs change in one place."));
		}
		Skeleton = Blueprint->TargetSkeleton;
	}
	if (!Gate::ValidateOps(Blueprint, Skeleton, Request.Ops, Error))
	{
		return Reject(TEXT("invalid_op"), Error);
	}

	const TSharedRef<FHyperAIStudioAnimBlueprintEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioAnimBlueprintEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->bCreate = Request.bCreate;
	Payload->SkeletonPath = Request.bCreate ? Request.SkeletonPath : FString();
	Payload->ParentClassPath = Request.bCreate ? Request.ParentClassPath : FString();
	Payload->Ops = Request.Ops;
	Payload->bSave = Request.bSave;
	Payload->SemanticFingerprint = ComputeEditOpsSemanticFingerprint(*Payload);
	if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("payload_bound_exceeded"), TEXT("The ops exceed the bounded request size; split them into smaller plans."));
	}

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = MutationToolName;
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Request.TargetPath;
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxOpsPerPlan + 8;
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	Trusted.bCompileOnce = false;
	Trusted.bSaveOnce = Request.bSave;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioAnimBlueprintFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioAnimBlueprintFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError) || !Prepare.bPrepared)
	{
		FString Diagnostic = PrepareError.IsEmpty() ? Prepare.Status.Diagnostic : PrepareError;
		if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
		{
			Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
		}
		return Reject(Prepare.Status.StatusCode.IsEmpty() ? FString(TEXT("prepare_failed")) : Prepare.Status.StatusCode, Diagnostic);
	}
	Report.bTrustedPrepared = true;
	Report.PlanHash = Payload->SemanticFingerprint;
	Report.AuthorizationPlanHash = Prepare.PlanHash;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = TEXT("Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops or graph state). Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.TargetPath;
		Summary.PlanHash = Payload->SemanticFingerprint;
		if (Request.bCreate) Summary.Effects.Add(TEXT("create the Animation Blueprint for ") + Request.SkeletonPath);
		for (const FHyperAIAnimBlueprintOp& Op : Request.Ops)
		{
			TArray<FString> Parts = {Op.Kind, Op.Machine, Op.Name};
			if (!Op.To.IsEmpty()) Parts.Add(TEXT("-> ") + Op.To);
			if (!Op.Rule.IsEmpty()) Parts.Add(TEXT("when ") + Op.Rule);
			if (!Op.Asset.IsEmpty()) Parts.Add(FPackageName::ObjectPathToObjectName(Op.Asset));
			if (!Op.Value.IsEmpty()) Parts.Add(Op.Value);
			Summary.Effects.Add(FString::Join(Parts.FilterByPredicate([](const FString& Part) { return !Part.IsEmpty(); }), TEXT(" ")));
		}
		Summary.Effects.Add(Request.bSave ? TEXT("compile and save") : TEXT("compile"));
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
	Report.Diagnostic = TEXT("The edit and compile run over the next editor ticks. Poll hyper_operation_status until it is terminal, then hyper_anim_blueprint_validate.");
	return Report;
}

FHyperAIAnimBlueprintInspectReport UHyperAIStudioAnimBlueprintToolset::hyper_anim_blueprint_inspect(const FHyperAIAnimBlueprintInspectRequest& Request)
{
	return FHyperAIStudioAnimBlueprintContracts::Inspect(Request);
}

FHyperAIAnimBlueprintApplyPlanReport UHyperAIStudioAnimBlueprintToolset::hyper_anim_blueprint_apply_plan(const FHyperAIAnimBlueprintApplyPlanRequest& Request)
{
	return FHyperAIStudioAnimBlueprintContracts::BuildPlan(Request);
}

FHyperAIAnimBlueprintValidateReport UHyperAIStudioAnimBlueprintToolset::hyper_anim_blueprint_validate(const FHyperAIAnimBlueprintValidateRequest& Request)
{
	return FHyperAIStudioAnimBlueprintContracts::Validate(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioAnimBlueprintDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioAnimBlueprintContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioAnimBlueprintDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioAnimBlueprintContracts;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetDescriptor();
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety, const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output, const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty() && Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0 || Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Animation Blueprint adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read, Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioAnimBlueprintInspectResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioAnimBlueprintInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioAnimBlueprintInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read, Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioAnimBlueprintValidateResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioAnimBlueprintValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioAnimBlueprintValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit, Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioAnimBlueprintEditOpsPayload& Typed = static_cast<const FHyperAIStudioAnimBlueprintEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::AnimBlueprint::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Animation Blueprint adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioAnimBlueprintFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioAnimBlueprintContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioAnimBlueprintFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using Contracts = FHyperAIStudioAnimBlueprintContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId || Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed Animation Blueprint request schema.");
		return false;
	}
	const FHyperAIStudioAnimBlueprintEditOpsPayload& Typed = static_cast<const FHyperAIStudioAnimBlueprintEditOpsPayload&>(Request);
	if (!Contracts::IsCanonicalProjectObjectPath(Typed.TargetPath) || Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetPath;
	return true;
}

bool FHyperAIStudioAnimBlueprintFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result, FString& OutPostconditionHash, FString& OutError)
{
	using Contracts = FHyperAIStudioAnimBlueprintContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh Animation Blueprint verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId || Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioAnimBlueprintEditOpsPayload& Typed = static_cast<const FHyperAIStudioAnimBlueprintEditOpsPayload&>(Request);
	const FHyperAIStudioAnimBlueprintMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioAnimBlueprintMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const UAnimBlueprint* Blueprint = HyperAIStudio::AnimBlueprint::Gate::Resolve(CanonicalTarget, /*bLoad=*/false);
	if (!Blueprint)
	{
		OutError = TEXT("The edited Animation Blueprint is no longer loaded.");
		return false;
	}
	const FString ContentKey = HyperAIStudio::AnimBlueprint::Gate::ComputeRevision(*Blueprint);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The Animation Blueprint changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave && Blueprint->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The Animation Blueprint still has unsaved changes after the save phase.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::AnimBlueprint::Private::AppendToken(Canonical, TEXT("hyperai.anim_blueprint.edit-ops-postcondition.v1"));
	HyperAIStudio::AnimBlueprint::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::AnimBlueprint::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioAnimBlueprintRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	// The apply tool tells agents to read this first, so it exists as soon as the tools can.
	if (!HyperAIStudio::AnimBlueprint::EnsureCookbook())
	{
		UE_LOG(LogHyperAIStudioAnimBlueprint, Warning, TEXT("Could not write %s."), *HyperAIStudio::AnimBlueprint::GetCookbookPath());
	}
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioAnimBlueprintRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioAnimBlueprintRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioAnimBlueprintRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(UHyperAIStudioAnimBlueprintToolset::StaticClass(), FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioAnimBlueprintRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioAnimBlueprintRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioAnimBlueprintContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioAnimBlueprint, Verbose, TEXT("Animation Blueprint source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioAnimBlueprintDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioAnimBlueprintContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("AnimGraph")) && GEditor != nullptr;
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("anim_graph_unavailable");
		Observation.Diagnostic = Observation.bReady ? TEXT("Animation Blueprint graph editing is available.") : TEXT("The AnimGraph editor module is not loaded.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioAnimBlueprintToolset::StaticClass(), FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioAnimBlueprintRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioAnimBlueprintToolset::StaticClass(), FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	ProbePublisher.Stop();
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome = FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed && Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioAnimBlueprint, Error, TEXT("Animation Blueprint adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
