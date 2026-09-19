// Games by Hyper 2026.

#include "HyperAIStudioLevelDesignToolset.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLevelDesignGate.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLevelDesign, Log, All);

namespace HyperAIStudio::LevelDesign::Private
{
	namespace Gate = HyperAIStudio::LevelDesign::Gate;

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

	int64 EstimatePieceBytes(const FHyperAILevelDesignPiece& Piece)
	{
		return 48 + EstimateBytes(Piece.Name) + EstimateBytes(Piece.Shape) + EstimateBytes(Piece.Role)
			+ EstimateBytes(Piece.Location) + EstimateBytes(Piece.Size);
	}

	int64 EstimateIssueBytes(const FHyperAILevelDesignIssue& Issue)
	{
		return 32 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Subject)
			+ EstimateBytes(Issue.Message) + EstimateBytes(Issue.Location);
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	/** Every piece a non-delete op names must exist after the plan ran. */
	bool NamedPiecesExist(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, FString& OutMissing)
	{
		TSet<FString> Remaining;
		for (const FHyperAILevelDesignOp& Op : Ops)
		{
			if (Op.Kind == TEXT("delete")) Remaining.Remove(Op.Name.ToLower());
			else Remaining.Add(Op.Name.ToLower());
		}
		for (const FHyperAILevelDesignPiece& Piece : Gate::ReadPieces(World))
		{
			Remaining.Remove(Piece.Name.ToLower());
		}
		OutMissing = FString::Join(Remaining.Array(), TEXT(", "));
		return Remaining.IsEmpty();
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioLevelDesignEditOpsPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioLevelDesign, Log, TEXT("Edit phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioLevelDesignMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioLevelDesignMutationResultPayload, ESPMode::ThreadSafe>();
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Blockout edits run on the game thread."));
		}
		UWorld* World = Gate::GetEditorWorld();
		if (!World || World->GetPathName() != Payload.LevelPath)
		{
			return Finish(FailedOutcome, TEXT("level_changed"), TEXT("A different level is open than the plan was made for; nothing more was changed."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Apply:
		{
			if (GEditor && GEditor->PlayWorld)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("play_in_editor_running"),
					TEXT("Play In Editor started after planning; stop it and apply again."));
			}
			if (Gate::ComputeRevision(*World) != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The blockout changed after planning; nothing was applied. Inspect again."));
			}
			int32 Changed = 0;
			FString Error;
			if (!Gate::ApplyOps(*World, Payload.Ops, Payload.MaxStepHeight, Payload.MaxWalkableSlopeDegrees, Changed, Error))
			{
				return Finish(Changed == 0 ? EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect
					: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"), Error);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied; %d actors added or removed."), Payload.Ops.Num(), Changed), TEXT("applied"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			FString Missing;
			if (!NamedPiecesExist(*World, Payload.Ops, Missing))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("pieces_missing"),
					TEXT("These pieces are missing after the edit: ") + Missing + TEXT(". Undo reverts it."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("Every named piece is in place."), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
			// Map packages only: a deleted piece under One File Per Actor is a pending-delete package only a map save reaches.
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The blockout changed but the level did not save; it remains in the open level."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Level saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh blockout captured for verification."), TEXT("completed"), Gate::ComputeRevision(*World));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Blockout plans have no compile phase."));
		}
	}
}

FString FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioLevelDesign.HyperAIStudioLevelDesignToolset");
}

TArray<FString> FHyperAIStudioLevelDesignContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, ValidateToolName};
}

bool FHyperAIStudioLevelDesignContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

TArray<FString> FHyperAIStudioLevelDesignContracts::GetCapabilities()
{
	return {
		TEXT("player_metrics_from_pawn_defaults_with_recommended_sizes"),
		TEXT("blocks_walkable_ramps_step_sized_stairs_and_markers_as_one_undo_step"),
		TEXT("floor_clearance_ceiling_and_cover_checks"),
		TEXT("navmesh_routes_start_to_goals_with_corridor_width"),
		TEXT("spawn_sightlines"),
		TEXT("top_down_preview_with_routes_markers_and_issues"),
		TEXT("unsupported:jump_route_checks_encounter_space_sizing_and_custom_meshes")};
}

bool FHyperAIStudioLevelDesignContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioLevelDesignContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("level_design.inspect.payload.v1|editor_world|pawn_class|output_bytes"));
	return Value;
}

FString FHyperAIStudioLevelDesignContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("level_design.edit_ops.payload.v1|level_path|base_revision|ordered_closed_ops|step_height|slope|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioLevelDesignContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("level_design.validate.payload.v1|editor_world|pawn_class|max_sightline|preview|output_bytes"));
	return Value;
}

FString FHyperAIStudioLevelDesignContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("level_design.inspect.result.v1|level|revision|player_metrics|pieces|bounded"));
	return Value;
}

FString FHyperAIStudioLevelDesignContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("level_design.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioLevelDesignContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("level_design.validate.result.v1|metrics|issues|routes|sightlines|navigation|preview|bounded"));
	return Value;
}

FString FHyperAIStudioLevelDesignInspectPayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioLevelDesignInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::LevelDesign::Private::ClampBytes(64 + 2ll * Request.PawnClassPath.Len());
}

FString FHyperAIStudioLevelDesignValidatePayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioLevelDesignValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignValidatePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::LevelDesign::Private::ClampBytes(80 + 2ll * Request.PawnClassPath.Len());
}

FString FHyperAIStudioLevelDesignEditOpsPayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::EditOpsPayloadTypeId;
}

FString FHyperAIStudioLevelDesignEditOpsPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::EditOpsPayloadSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::LevelDesign::Private::EstimateBytes;
	int64 Size = 160 + EstimateBytes(LevelPath) + EstimateBytes(BaseRevision) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAILevelDesignOp& Op : Ops)
	{
		Size += 32 + EstimateBytes(Op.Kind) + EstimateBytes(Op.Name) + EstimateBytes(Op.Role) + EstimateBytes(Op.Location)
			+ EstimateBytes(Op.Size) + EstimateBytes(Op.Value);
	}
	return HyperAIStudio::LevelDesign::Private::ClampBytes(Size);
}

FString FHyperAIStudioLevelDesignEditOpsPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioLevelDesignEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioLevelDesignEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioLevelDesignEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->LevelPath = LevelPath;
	Clone->BaseRevision = BaseRevision;
	Clone->Ops = Ops;
	Clone->MaxStepHeight = MaxStepHeight;
	Clone->MaxWalkableSlopeDegrees = MaxWalkableSlopeDegrees;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioLevelDesignInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::InspectResultTypeId;
}

FString FHyperAIStudioLevelDesignInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::LevelDesign::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.LevelPath) + EstimateBytes(Report.Revision);
	for (const FHyperAILevelDesignPiece& Piece : Report.Pieces) Size += EstimatePieceBytes(Piece);
	return ClampBytes(Size);
}

FString FHyperAIStudioLevelDesignValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::ValidateResultTypeId;
}

FString FHyperAIStudioLevelDesignValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::LevelDesign::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.PreviewImagePath);
	for (const FHyperAILevelDesignIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	Size += 96ll * (Report.Routes.Num() + Report.Sightlines.Num());
	return ClampBytes(Size);
}

FString FHyperAIStudioLevelDesignMutationResultPayload::GetTypeId() const
{
	return FHyperAIStudioLevelDesignContracts::MutationResultTypeId;
}

FString FHyperAIStudioLevelDesignMutationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::MutationResultSchemaFingerprint();
}

int32 FHyperAIStudioLevelDesignMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::LevelDesign::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioLevelDesignContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.level_design.blockout.ue58");
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

FHyperAILevelDesignInspectReport FHyperAIStudioLevelDesignContracts::Inspect(const FHyperAILevelDesignInspectRequest& Request)
{
	using namespace HyperAIStudio::LevelDesign::Private;
	FHyperAILevelDesignInspectReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Level design inspection runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes || Request.PawnClassPath.Len() > 512)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed bounds, or pawn_class_path is too long."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	FString Error;
	if (!Gate::MeasurePlayer(*World, Request.PawnClassPath, Report.Metrics, Error))
	{
		return Reject(TEXT("invalid_pawn_class"), Error);
	}
	Report.LevelPath = World->GetPathName();
	Report.Revision = Gate::ComputeRevision(*World);
	int64 Budget = 2048;
	for (FHyperAILevelDesignPiece& Piece : Gate::ReadPieces(*World))
	{
		const int64 Bytes = EstimatePieceBytes(Piece);
		if (Budget + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		Budget += Bytes;
		Report.Pieces.Add(MoveTemp(Piece));
	}
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = FString::Printf(TEXT("Player %.0f cm tall, %.0f cm wide, steps %.0f, jumps %.0f up and %.0f across (%s). %d blockout pieces."),
		Report.Metrics.PlayerHeight, Report.Metrics.CapsuleRadius * 2.0f, Report.Metrics.MaxStepHeight, Report.Metrics.JumpHeight,
		Report.Metrics.MaxJumpGap, *Report.Metrics.Source, Report.Pieces.Num());
	return Report;
}

FHyperAILevelDesignValidateReport FHyperAIStudioLevelDesignContracts::Validate(const FHyperAILevelDesignValidateRequest& Request)
{
	using namespace HyperAIStudio::LevelDesign::Private;
	FHyperAILevelDesignValidateReport Report;
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
		return Reject(TEXT("game_thread_required"), TEXT("Level design validation runs on the game thread."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes || Request.PawnClassPath.Len() > 512
		|| !(Request.MaxSightline >= 500.0f && Request.MaxSightline <= 100000.0f))
	{
		return Reject(TEXT("invalid_bounds"), TEXT("Bounds are closed: max_output_bytes 16-128 KB, max_sightline 500-100000 cm."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	FString Error;
	if (!Gate::MeasurePlayer(*World, Request.PawnClassPath, Report.Metrics, Error))
	{
		return Reject(TEXT("invalid_pawn_class"), Error);
	}
	Report.LevelPath = World->GetPathName();
	Gate::CheckLayout(*World, Report.Metrics, Request.MaxSightline, Report);
	FString PreviewNote;
	if (Request.bCapturePreview && !Gate::WritePreview(*World, Report, Report.PreviewImagePath, PreviewNote))
	{
		PreviewNote = TEXT(" No preview: ") + PreviewNote;
	}
	// Issues past the output budget are counted but not listed.
	int64 Budget = 4096 + 96ll * (Report.Routes.Num() + Report.Sightlines.Num());
	int32 Kept = 0;
	for (; Kept < Report.Issues.Num(); ++Kept)
	{
		Budget += EstimateIssueBytes(Report.Issues[Kept]);
		if (Budget > Request.MaxOutputBytes) break;
	}
	if (Kept < Report.Issues.Num())
	{
		Report.Issues.SetNum(Kept);
		Report.bTruncated = true;
	}
	Report.bOk = true;
	Report.Status = Report.ErrorCount > 0 ? TEXT("problems") : TEXT("playable");
	Report.Diagnostic = FString::Printf(TEXT("%d errors, %d warnings, %d routes checked%s.%s"),
		Report.ErrorCount, Report.WarningCount, Report.Routes.Num(),
		Report.bNavigationReady ? TEXT("") : TEXT(" (no ready navmesh)"), *PreviewNote);
	return Report;
}

FString FHyperAIStudioLevelDesignContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioLevelDesignEditOpsPayload& Payload)
{
	using HyperAIStudio::LevelDesign::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.level_design.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.LevelPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, FString::Printf(TEXT("%.3f|%.3f"), Payload.MaxStepHeight, Payload.MaxWalkableSlopeDegrees));
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAILevelDesignOp& Op : Payload.Ops)
	{
		AppendToken(Canonical, Op.Kind);
		AppendToken(Canonical, Op.Name);
		AppendToken(Canonical, Op.Role);
		AppendToken(Canonical, Op.Location);
		AppendToken(Canonical, Op.Size);
		AppendToken(Canonical, Op.Value);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAILevelBlockoutApplyPlanReport FHyperAIStudioLevelDesignContracts::BuildPlan(const FHyperAILevelBlockoutApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::LevelDesign::Private;
	FHyperAILevelBlockoutApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
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
		return Reject(TEXT("game_thread_required"), TEXT("Blockout planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
		|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), TEXT("A non-dry submission needs a valid operation_id and the plan_hash its dry run returned."));
	}
	if (!IsCanonicalSha256(Request.ExpectedRevision))
	{
		return Reject(TEXT("invalid_revision"), TEXT("Pass expected_revision from hyper_level_design_inspect."));
	}
	if (Request.Ops.IsEmpty() || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs 1 to %d ops."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes || Request.PawnClassPath.Len() > 512)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed bounds, or pawn_class_path is too long."));
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		return Reject(TEXT("no_editor_world"), TEXT("No level is open in the editor."));
	}
	if (GEditor && GEditor->PlayWorld)
	{
		return Reject(TEXT("play_in_editor_running"), TEXT("Stop Play In Editor first: edits made now would not reach the running game."));
	}
	Report.LevelPath = World->GetPathName();
	Report.BaseRevision = Gate::ComputeRevision(*World);
	if (Report.BaseRevision != Request.ExpectedRevision)
	{
		return Reject(TEXT("stale_revision"), TEXT("The blockout changed after inspection; inspect again.")
			+ FHyperAIStudioAgentActivityLog::DescribeLastChange(Report.LevelPath));
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		return Reject(TEXT("level_never_saved"), TEXT("This level has never been saved, so there is nowhere to save the blockout; save the level first or leave bSave off."));
	}
	FHyperAIPlayerMetrics Metrics;
	FString Error;
	if (!Gate::MeasurePlayer(*World, Request.PawnClassPath, Metrics, Error))
	{
		return Reject(TEXT("invalid_pawn_class"), Error);
	}
	if (!Gate::ValidateOps(*World, Request.Ops, Metrics.MaxStepHeight, Metrics.MaxWalkableSlopeDegrees, Report.Notes, Error))
	{
		return Reject(TEXT("invalid_op"), Error);
	}

	const TSharedRef<FHyperAIStudioLevelDesignEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioLevelDesignEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->LevelPath = Report.LevelPath;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->Ops = Request.Ops;
	Payload->MaxStepHeight = Metrics.MaxStepHeight;
	Payload->MaxWalkableSlopeDegrees = Metrics.MaxWalkableSlopeDegrees;
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
	Trusted.EffectTarget = Report.LevelPath;
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
	const TSharedRef<FHyperAIStudioLevelDesignFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioLevelDesignFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError)
		|| !Prepare.bPrepared)
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
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops, blockout, or player metrics). Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Report.LevelPath;
		Summary.PlanHash = Payload->SemanticFingerprint;
		for (const FHyperAILevelDesignOp& Op : Request.Ops)
		{
			Summary.Effects.Add(FString::Printf(TEXT("%s %s%s%s"), *Op.Kind, *Op.Name,
				Op.Role.IsEmpty() ? TEXT("") : *(TEXT(" (") + Op.Role + TEXT(")")),
				Op.Location.IsEmpty() ? TEXT("") : *(TEXT(" at ") + Op.Location)));
		}
		if (Request.bSave)
		{
			Summary.Effects.Add(TEXT("save the level, including any other unsaved changes in it"));
		}
		Summary.Touches.Add(Report.LevelPath);
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
	Report.Diagnostic = TEXT("The edit runs over the next editor ticks. Poll hyper_operation_status until it is terminal, then hyper_level_design_validate.");
	return Report;
}

FHyperAILevelDesignInspectReport UHyperAIStudioLevelDesignToolset::hyper_level_design_inspect(const FHyperAILevelDesignInspectRequest& Request)
{
	return FHyperAIStudioLevelDesignContracts::Inspect(Request);
}

FHyperAILevelBlockoutApplyPlanReport UHyperAIStudioLevelDesignToolset::hyper_level_blockout_apply_plan(const FHyperAILevelBlockoutApplyPlanRequest& Request)
{
	return FHyperAIStudioLevelDesignContracts::BuildPlan(Request);
}

FHyperAILevelDesignValidateReport UHyperAIStudioLevelDesignToolset::hyper_level_design_validate(const FHyperAILevelDesignValidateRequest& Request)
{
	return FHyperAIStudioLevelDesignContracts::Validate(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioLevelDesignDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioLevelDesignContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioLevelDesignDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioLevelDesignContracts;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetDescriptor();
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety,
		const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output,
		const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Level design adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLevelDesignInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLevelDesignInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioLevelDesignInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioLevelDesignValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioLevelDesignValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioLevelDesignValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit,
		Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioLevelDesignEditOpsPayload& Typed = static_cast<const FHyperAIStudioLevelDesignEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::LevelDesign::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Level design adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioLevelDesignFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioLevelDesignContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioLevelDesignFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	using Contracts = FHyperAIStudioLevelDesignContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId
		|| Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed blockout request schema.");
		return false;
	}
	const FHyperAIStudioLevelDesignEditOpsPayload& Typed = static_cast<const FHyperAIStudioLevelDesignEditOpsPayload&>(Request);
	if (Typed.LevelPath.IsEmpty() || Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the level path or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.LevelPath;
	return true;
}

bool FHyperAIStudioLevelDesignFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	namespace Gate = HyperAIStudio::LevelDesign::Gate;
	using Contracts = FHyperAIStudioLevelDesignContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh blockout verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId
		|| Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioLevelDesignEditOpsPayload& Typed = static_cast<const FHyperAIStudioLevelDesignEditOpsPayload&>(Request);
	const FHyperAIStudioLevelDesignMutationResultPayload& TypedResult = static_cast<const FHyperAIStudioLevelDesignMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	UWorld* World = Gate::GetEditorWorld();
	if (!World || World->GetPathName() != CanonicalTarget)
	{
		OutError = TEXT("The edited level is no longer open.");
		return false;
	}
	const FString ContentKey = Gate::ComputeRevision(*World);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The blockout changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave && World->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The level still has unsaved changes after the save phase.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::LevelDesign::Private::AppendToken(Canonical, TEXT("hyperai.level_design.edit-ops-postcondition.v1"));
	HyperAIStudio::LevelDesign::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::LevelDesign::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioLevelDesignRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioLevelDesignRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioLevelDesignRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioLevelDesignRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioLevelDesignToolset::StaticClass(), FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioLevelDesignRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioLevelDesignRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioLevelDesignContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioLevelDesign, Verbose, TEXT("Level design source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioLevelDesignDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioLevelDesignContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = HyperAIStudio::LevelDesign::Gate::GetEditorWorld() != nullptr;
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("no_editor_world");
		Observation.Diagnostic = Observation.bReady ? TEXT("A level is open in the editor.") : TEXT("No level is open in the editor.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioLevelDesignToolset::StaticClass(), FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioLevelDesignRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioLevelDesignToolset::StaticClass(), FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioLevelDesign, Error, TEXT("Level design adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
