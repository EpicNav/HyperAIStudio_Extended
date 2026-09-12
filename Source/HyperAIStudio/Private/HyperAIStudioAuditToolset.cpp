// Games by Hyper 2026.

#include "HyperAIStudioAuditToolset.h"

#include "Async/Async.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioAuditToolset)

namespace HyperAIStudio::Audit::Private
{
	const TSet<FString>& AllowedKinds()
	{
		static const TSet<FString> Values = {
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
		return Values;
	}

	const TArray<FString>& DefaultKinds()
	{
		static const TArray<FString> Values = {
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
		return Values;
	}

	FHyperAIStudioDiagnosticsFinding MakeFinding(
		const FString& Kind,
		const FString& Severity,
		const FString& Code,
		const FString& Subject,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsFinding Finding;
		Finding.Kind = Kind;
		Finding.Severity = Severity;
		Finding.Code = Code;
		Finding.Subject = FHyperAIStudioDiagnosticsCommon::Clip(
			Subject, FHyperAIStudioDiagnosticsCommon::MaxSubjectCharacters);
		Finding.Message = FHyperAIStudioDiagnosticsCommon::Clip(
			Message, FHyperAIStudioDiagnosticsCommon::MaxFindingMessageCharacters);
		Finding.RecordId = FHyperAIStudioDiagnosticsCommon::MakeRecordId(
			Finding.Kind, Finding.Subject, Finding.Code);
		return Finding;
	}

	void SortFindings(TArray<FHyperAIStudioDiagnosticsFinding>& Findings)
	{
		for (FHyperAIStudioDiagnosticsFinding& Finding : Findings)
		{
			Finding.Fields.Sort([](const auto& Left, const auto& Right)
			{
				return Left.Name == Right.Name ? Left.Value < Right.Value : Left.Name < Right.Name;
			});
		}
		Findings.Sort([](const auto& Left, const auto& Right)
		{
			return Left.RecordId < Right.RecordId;
		});
	}

	FHyperAIStudioDiagnosticsAuditResult InvalidResult(
		const FHyperAIStudioDiagnosticsAuditRequest& Request,
		const FString& Status,
		const FString& Code,
		const FString& Field,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsAuditResult Result;
		Result.Status = Status;
		Result.Action = Request.Action;
		Result.AuditId = Request.AuditId;
		Result.bIncomplete = Status != TEXT("invalid_request") && Status != TEXT("conflict");
		Result.CursorStatus = Request.Cursor.IsEmpty() ? TEXT("none") : TEXT("rejected");
		Result.CursorDiagnosticCode = Request.Cursor.IsEmpty() ? FString() : Code;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, Code, TEXT("error"), Field, Message);
		return Result;
	}
}

bool FHyperAIStudioAuditContracts::NormalizeStartRequest(
	const FHyperAIStudioDiagnosticsAuditRequest& Request,
	FHyperAIStudioDiagnosticsNormalizedAuditRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Audit::Private;
	OutRequest = FHyperAIStudioDiagnosticsNormalizedAuditRequest();
	if (!FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(Request.AuditId, 8, 64))
	{
		OutErrorCode = TEXT("invalid_audit_id");
		OutError = TEXT("AuditId must contain 8..64 ASCII letters, digits, underscore, or hyphen.");
		return false;
	}
	if (!Request.Cursor.IsEmpty())
	{
		OutErrorCode = TEXT("cursor_not_allowed_on_start");
		OutError = TEXT("Cursor must be empty when Action is start.");
		return false;
	}
	if (Request.MaxObjectsScanned < FHyperAIStudioDiagnoseContracts::MinObjectsScanned
		|| Request.MaxObjectsScanned > MaxObjectsScanned)
	{
		OutErrorCode = TEXT("invalid_object_scan_bound");
		OutError = TEXT("MaxObjectsScanned is outside the fixed audit range.");
		return false;
	}
	if (!FHyperAIStudioDiagnosticsCommon::NormalizeAllowlist(
		Request.Kinds,
		AllowedKinds(),
		DefaultKinds(),
		MaxKinds,
		OutRequest.Kinds,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	FString PageErrorCode;
	FString PageError;
	if (!FHyperAIStudioDiagnosticsCommon::ValidatePageBounds(
		Request.PageSize, FString(), Request.MaxOutputBytes, PageErrorCode, PageError))
	{
		OutErrorCode = PageErrorCode;
		OutError = PageError;
		return false;
	}
	OutRequest.AuditId = Request.AuditId;
	OutRequest.MaxObjectsScanned = Request.MaxObjectsScanned;
	OutRequest.RequestFingerprint = FHyperAIStudioDiagnosticsCommon::HashTokens({
		TEXT("hyperai.run-audit.request.v1"),
		FString::Join(OutRequest.Kinds, TEXT(",")),
		FString::FromInt(OutRequest.MaxObjectsScanned)
	});
	if (OutRequest.RequestFingerprint.IsEmpty())
	{
		OutErrorCode = TEXT("request_fingerprint_failed");
		OutError = TEXT("Audit request could not be fingerprinted within fixed bounds.");
		return false;
	}
	return true;
}

FHyperAIStudioDiagnosticsStoredAudit FHyperAIStudioAuditContracts::AnalyzeSnapshot(
	const FHyperAIStudioDiagnosticsValueSnapshot& Snapshot,
	const FHyperAIStudioDiagnosticsNormalizedAuditRequest& Request,
	const FString& StartedUtc)
{
	using namespace HyperAIStudio::Audit::Private;
	FHyperAIStudioDiagnosticsStoredAudit Audit;
	Audit.AuditId = Request.AuditId;
	Audit.RequestFingerprint = Request.RequestFingerprint;
	Audit.StartedUtc = StartedUtc;
	Audit.CompletedUtc = FDateTime::UtcNow().ToIso8601();
	Audit.bIncomplete = Snapshot.bIncomplete;
	Audit.bCaptureWorkBoundReached = Snapshot.bCaptureWorkBoundReached;
	Audit.Diagnostics = Snapshot.Diagnostics;
	Audit.Findings.Reserve(MaxStoredFindings);
	const auto AddFindingBounded = [&Audit](FHyperAIStudioDiagnosticsFinding&& Finding)
	{
		if (Audit.Findings.Num() >= FHyperAIStudioAuditContracts::MaxStoredFindings)
		{
			Audit.bFindingBoundReached = true;
			Audit.bIncomplete = true;
			return;
		}
		Audit.Findings.Add(MoveTemp(Finding));
	};

	if (Request.Kinds.Contains(TEXT("plugin_readiness")))
	{
		for (const FHyperAIStudioDiagnosticsPluginSnapshot& Plugin : Snapshot.Plugins)
		{
			if (Plugin.bEnabled)
			{
				continue;
			}
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("plugin_readiness"),
				Plugin.bInstalled ? TEXT("warning") : TEXT("error"),
				Plugin.bInstalled ? TEXT("required_plugin_disabled") : TEXT("required_plugin_missing"),
				Plugin.Name,
				Plugin.bInstalled
					? TEXT("Required plugin is installed but not enabled in this project.")
					: TEXT("Required plugin was not found in the engine installation."));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("installed"), Plugin.bInstalled ? TEXT("true") : TEXT("false"));
			AddFindingBounded(MoveTemp(Finding));
		}
	}

	if (Request.Kinds.Contains(TEXT("asset_registry_readiness")))
	{
		const bool bReady = Snapshot.bAssetRegistryAvailable
			&& !Snapshot.bAssetRegistryGathering
			&& Snapshot.bAssetRegistrySearchAllObserved;
		if (!bReady)
		{
			Audit.bIncomplete = true;
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("asset_registry_readiness"), TEXT("warning"), TEXT("asset_registry_incomplete"),
				TEXT("AssetRegistry"),
				TEXT("Asset Registry is unavailable, gathering, or has not observed a completed all-assets search."));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("available"), Snapshot.bAssetRegistryAvailable ? TEXT("true") : TEXT("false"));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("gathering"), Snapshot.bAssetRegistryGathering ? TEXT("true") : TEXT("false"));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("search_all_observed"), Snapshot.bAssetRegistrySearchAllObserved ? TEXT("true") : TEXT("false"));
			AddFindingBounded(MoveTemp(Finding));
		}
	}

	if (Request.Kinds.Contains(TEXT("current_map_state")))
	{
		if (!Snapshot.bEditorWorldAvailable || Snapshot.bCurrentMapDirty)
		{
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("current_map_state"),
				Snapshot.bEditorWorldAvailable ? TEXT("warning") : TEXT("error"),
				Snapshot.bEditorWorldAvailable ? TEXT("current_map_dirty") : TEXT("editor_world_unavailable"),
				Snapshot.bEditorWorldAvailable ? Snapshot.CurrentMapPackage : TEXT("EditorWorld"),
				Snapshot.bEditorWorldAvailable
					? TEXT("Current map package has unsaved changes.")
					: TEXT("No current editor world was available at audit capture time."));
			AddFindingBounded(MoveTemp(Finding));
		}
	}

	if (Request.Kinds.Contains(TEXT("loaded_blueprint_health")))
	{
		for (const FHyperAIStudioDiagnosticsBlueprintSnapshot& Blueprint : Snapshot.Blueprints)
		{
			if (Blueprint.Status == TEXT("up_to_date") && !Blueprint.bPackageDirty)
			{
				continue;
			}
			const bool bError = Blueprint.Status == TEXT("error") || Blueprint.Status == TEXT("invalid");
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("loaded_blueprint_health"), bError ? TEXT("error") : TEXT("warning"),
				bError ? TEXT("loaded_blueprint_compile_error") : TEXT("loaded_blueprint_needs_attention"),
				Blueprint.ObjectPath,
				TEXT("Loaded Blueprint is not cleanly up-to-date; unloaded Blueprints were not loaded or assessed."));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("blueprint_status"), Blueprint.Status);
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("package_dirty"), Blueprint.bPackageDirty ? TEXT("true") : TEXT("false"));
			AddFindingBounded(MoveTemp(Finding));
		}
	}

	if (Request.Kinds.Contains(TEXT("dirty_loaded_packages")))
	{
		for (const FString& Package : Snapshot.DirtyLoadedPackages)
		{
			AddFindingBounded(MakeFinding(
				TEXT("dirty_loaded_packages"), TEXT("warning"), TEXT("loaded_package_dirty"),
				Package, TEXT("Loaded project package has unsaved changes.")));
		}
	}

	SortFindings(Audit.Findings);
	if (Audit.bFindingBoundReached)
	{
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Audit.Diagnostics,
			TEXT("audit_finding_bound_reached"),
			TEXT("warning"),
			TEXT("findings"),
			TEXT("Audit findings reached the hard retained-record bound; omitted findings are explicit."));
	}
	FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
		Audit.Diagnostics,
		TEXT("audit_scope_bounded_loaded_state"),
		TEXT("info"),
		TEXT("observation_scope"),
		TEXT("This audit covers allowlisted loaded-editor state and readiness facts only; it does not run unbounded Map Check, Data Validation, asset loading, or orphan inference."));
	FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
		Audit.Diagnostics,
		TEXT("no_delete_inference"),
		TEXT("info"),
		TEXT("deletion_assessment"),
		TEXT("No orphan or safe-to-delete conclusion is inferred from this bounded audit."));
	Audit.SnapshotFingerprint = FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint(
		Audit.Findings,
		{
			TEXT("hyperai.run-audit.snapshot.v1"),
			Request.RequestFingerprint,
			Snapshot.CapturedUtc,
			Audit.bIncomplete ? TEXT("incomplete") : TEXT("complete"),
			Audit.bFindingBoundReached ? TEXT("finding_bound") : TEXT("within_finding_bound")
		});
	if (Audit.SnapshotFingerprint.IsEmpty())
	{
		Audit.bIncomplete = true;
		Audit.Status = TEXT("partial");
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Audit.Diagnostics,
			TEXT("audit_snapshot_fingerprint_failed"),
			TEXT("error"),
			TEXT("snapshot"),
			TEXT("Audit snapshot could not be fingerprinted within fixed bounds."));
	}
	else
	{
		Audit.Status = Audit.bIncomplete ? TEXT("partial") : TEXT("complete");
	}
	return Audit;
}

FHyperAIStudioDiagnosticsAuditStore& FHyperAIStudioDiagnosticsAuditStore::Get()
{
	static FHyperAIStudioDiagnosticsAuditStore Store;
	return Store;
}

void FHyperAIStudioDiagnosticsAuditStore::Startup()
{
	FScopeLock Lock(&Mutex);
	bShuttingDown = false;
}

void FHyperAIStudioDiagnosticsAuditStore::Shutdown()
{
	{
		FScopeLock Lock(&Mutex);
		bShuttingDown = true;
	}
	if (WorkerFuture.IsValid())
	{
		WorkerFuture.Wait();
	}
	FScopeLock Lock(&Mutex);
	bWorkerRunning = false;
	Audits.Reset();
	RetentionOrder.Reset();
}

FHyperAIStudioDiagnosticsAuditResult FHyperAIStudioDiagnosticsAuditStore::Execute(
	const FHyperAIStudioDiagnosticsAuditRequest& Request)
{
	using namespace HyperAIStudio::Audit::Private;
	if (Request.Action == TEXT("start"))
	{
		return Start(Request);
	}
	if (Request.Action == TEXT("status"))
	{
		return Status(Request);
	}
	return InvalidResult(
		Request, TEXT("invalid_request"), TEXT("unsupported_action"), TEXT("action"),
		TEXT("Action must be exactly start or status."));
}

FHyperAIStudioDiagnosticsAuditResult FHyperAIStudioDiagnosticsAuditStore::Start(
	const FHyperAIStudioDiagnosticsAuditRequest& Request)
{
	using namespace HyperAIStudio::Audit::Private;
	FHyperAIStudioDiagnosticsNormalizedAuditRequest Normalized;
	FString ErrorCode;
	FString Error;
	if (!FHyperAIStudioAuditContracts::NormalizeStartRequest(Request, Normalized, ErrorCode, Error))
	{
		return InvalidResult(Request, TEXT("invalid_request"), ErrorCode, TEXT("request"), Error);
	}
	if (!IsInGameThread())
	{
		return InvalidResult(
			Request, TEXT("unavailable"), TEXT("audit_requires_game_thread"), TEXT("audit"),
			TEXT("Bounded UObject capture is admitted only on Unreal's game thread."));
	}

	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			return InvalidResult(
				Request, TEXT("unavailable"), TEXT("audit_store_shutting_down"), TEXT("audit"),
				TEXT("Audit store is shutting down."));
		}
		if (const FHyperAIStudioDiagnosticsStoredAudit* Existing = Audits.Find(Normalized.AuditId))
		{
			if (Existing->RequestFingerprint != Normalized.RequestFingerprint)
			{
				return InvalidResult(
					Request, TEXT("conflict"), TEXT("audit_id_conflict"), TEXT("audit_id"),
					TEXT("AuditId is already bound to a different normalized request."));
			}
			return Project(*Existing, Request, true);
		}
		if (bWorkerRunning)
		{
			return InvalidResult(
				Request, TEXT("busy"), TEXT("audit_worker_busy"), TEXT("audit"),
				TEXT("The single bounded audit worker is busy; retry later with the same unused AuditId."));
		}
	}

	FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest CaptureRequest;
	CaptureRequest.Checks = Normalized.Kinds;
	CaptureRequest.MaxObjectsScanned = Normalized.MaxObjectsScanned;
	CaptureRequest.PageSize = FHyperAIStudioDiagnosticsCommon::MaxPageSize;
	CaptureRequest.MaxOutputBytes = FHyperAIStudioDiagnosticsCommon::MaxOutputBytes;
	CaptureRequest.RequestFingerprint = Normalized.RequestFingerprint;
	FHyperAIStudioDiagnosticsValueSnapshot Snapshot = FHyperAIStudioDiagnoseContracts::Capture(CaptureRequest);
	const FString StartedUtc = FDateTime::UtcNow().ToIso8601();

	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown || bWorkerRunning || Audits.Contains(Normalized.AuditId))
		{
			return InvalidResult(
				Request, TEXT("busy"), TEXT("audit_start_race_rejected"), TEXT("audit"),
				TEXT("Audit start state changed during bounded capture; no worker was started."));
		}
		PruneCompletedLocked();
		FHyperAIStudioDiagnosticsStoredAudit Running;
		Running.AuditId = Normalized.AuditId;
		Running.RequestFingerprint = Normalized.RequestFingerprint;
		Running.StartedUtc = StartedUtc;
		Running.Status = TEXT("running");
		Running.bIncomplete = true;
		Audits.Add(Running.AuditId, Running);
		RetentionOrder.Add(Running.AuditId);
		bWorkerRunning = true;
		WorkerFuture = Async(EAsyncExecution::ThreadPool,
			[this, Snapshot = MoveTemp(Snapshot), Normalized, StartedUtc]() mutable
			{
				FHyperAIStudioDiagnosticsStoredAudit Completed =
					FHyperAIStudioAuditContracts::AnalyzeSnapshot(Snapshot, Normalized, StartedUtc);
				Commit(MoveTemp(Completed));
			});
		return Project(Running, Request, false);
	}
}

FHyperAIStudioDiagnosticsAuditResult FHyperAIStudioDiagnosticsAuditStore::Status(
	const FHyperAIStudioDiagnosticsAuditRequest& Request)
{
	using namespace HyperAIStudio::Audit::Private;
	if (!FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(Request.AuditId, 8, 64))
	{
		return InvalidResult(
			Request, TEXT("invalid_request"), TEXT("invalid_audit_id"), TEXT("audit_id"),
			TEXT("AuditId must contain 8..64 ASCII letters, digits, underscore, or hyphen."));
	}
	FString ErrorCode;
	FString Error;
	if (!FHyperAIStudioDiagnosticsCommon::ValidatePageBounds(
		Request.PageSize, Request.Cursor, Request.MaxOutputBytes, ErrorCode, Error))
	{
		return InvalidResult(Request, TEXT("invalid_request"), ErrorCode, TEXT("request"), Error);
	}
	FScopeLock Lock(&Mutex);
	const FHyperAIStudioDiagnosticsStoredAudit* Existing = Audits.Find(Request.AuditId);
	if (!Existing)
	{
		return InvalidResult(
			Request, TEXT("not_found"), TEXT("audit_not_found"), TEXT("audit_id"),
			TEXT("No retained audit has this exact AuditId."));
	}
	// Stored audit results are immutable after the worker commits them. Project the
	// bounded page directly instead of deep-copying every retained finding per status read.
	return Project(*Existing, Request, false);
}

FHyperAIStudioDiagnosticsAuditResult FHyperAIStudioDiagnosticsAuditStore::Project(
	const FHyperAIStudioDiagnosticsStoredAudit& Audit,
	const FHyperAIStudioDiagnosticsAuditRequest& Request,
	const bool bReplay) const
{
	FHyperAIStudioDiagnosticsAuditResult Result;
	Result.Action = Request.Action;
	Result.AuditId = Audit.AuditId;
	Result.RequestFingerprint = Audit.RequestFingerprint;
	Result.SnapshotFingerprint = Audit.SnapshotFingerprint;
	Result.StartedUtc = Audit.StartedUtc;
	Result.CompletedUtc = Audit.CompletedUtc;
	Result.bAccepted = true;
	Result.bReplay = bReplay;
	Result.bComplete = Audit.Status == TEXT("complete") || Audit.Status == TEXT("partial");
	Result.bIncomplete = Audit.bIncomplete;
	Result.bCaptureWorkBoundReached = Audit.bCaptureWorkBoundReached;
	Result.bFindingBoundReached = Audit.bFindingBoundReached;
	Result.Diagnostics = Audit.Diagnostics;
	if (!Result.bComplete)
	{
		Result.Status = Request.Action == TEXT("start") ? TEXT("accepted") : TEXT("running");
		return Result;
	}
	Result.Status = Audit.Status;
	FString ErrorCode;
	FString Error;
	FHyperAIStudioDiagnosticsProjectedPage Page;
	if (!FHyperAIStudioDiagnosticsCommon::ProjectFindings(
		Audit.Findings,
		Audit.RequestFingerprint,
		Audit.SnapshotFingerprint,
		Request.PageSize,
		Request.Cursor,
		Request.MaxOutputBytes,
		Page,
		ErrorCode,
		Error))
	{
		Result.Status = TEXT("invalid_request");
		Result.CursorStatus = Page.CursorStatus;
		Result.CursorDiagnosticCode = Page.CursorDiagnosticCode;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, ErrorCode, TEXT("error"), TEXT("cursor"), Error);
		return Result;
	}
	Result.TotalRecords = Page.TotalRecords;
	Result.PageOffset = Page.PageOffset;
	Result.ReturnedRecords = Page.ReturnedRecords;
	Result.bHasMore = Page.bHasMore;
	Result.CursorStatus = Page.CursorStatus;
	Result.CursorDiagnosticCode = Page.CursorDiagnosticCode;
	Result.NextCursor = Page.NextCursor;
	Result.bOutputBudgetReached = Page.bOutputBudgetReached;
	Result.Findings = MoveTemp(Page.Findings);
	Result.bTruncated = Result.bIncomplete || Result.bFindingBoundReached
		|| Result.bHasMore || Result.bOutputBudgetReached;
	return Result;
}

void FHyperAIStudioDiagnosticsAuditStore::Commit(FHyperAIStudioDiagnosticsStoredAudit&& Audit)
{
	FScopeLock Lock(&Mutex);
	if (!bShuttingDown)
	{
		if (FHyperAIStudioDiagnosticsStoredAudit* Existing = Audits.Find(Audit.AuditId))
		{
			if (Existing->RequestFingerprint == Audit.RequestFingerprint)
			{
				*Existing = MoveTemp(Audit);
			}
		}
	}
	bWorkerRunning = false;
}

void FHyperAIStudioDiagnosticsAuditStore::PruneCompletedLocked()
{
	while (RetentionOrder.Num() >= MaxRetainedJobs)
	{
		int32 RemovableIndex = INDEX_NONE;
		for (int32 Index = 0; Index < RetentionOrder.Num(); ++Index)
		{
			const FHyperAIStudioDiagnosticsStoredAudit* Audit = Audits.Find(RetentionOrder[Index]);
			if (!Audit || Audit->Status != TEXT("running"))
			{
				RemovableIndex = Index;
				break;
			}
		}
		if (RemovableIndex == INDEX_NONE)
		{
			break;
		}
		Audits.Remove(RetentionOrder[RemovableIndex]);
		RetentionOrder.RemoveAt(RemovableIndex, 1, EAllowShrinking::No);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioDiagnosticsAuditStore::ResetForTests()
{
	Shutdown();
	Startup();
}
#endif

FHyperAIStudioDiagnosticsAuditResult UHyperAIStudioAuditToolset::hyper_run_audit(
	const FHyperAIStudioDiagnosticsAuditRequest& Request)
{
	return FHyperAIStudioDiagnosticsAuditStore::Get().Execute(Request);
}
