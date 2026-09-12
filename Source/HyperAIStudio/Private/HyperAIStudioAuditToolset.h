// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDiagnoseToolset.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAuditToolset.generated.h"

/** Idempotent asynchronous audit start/status request. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsAuditRequest
{
	GENERATED_BODY()

	FHyperAIStudioDiagnosticsAuditRequest()
	{
		Kinds = {
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
	}

	/** start or status. */
	UPROPERTY()
	FString Action = TEXT("start");

	/** Client-generated idempotency key: 8..64 ASCII letters, digits, underscore, or hyphen. */
	UPROPERTY()
	FString AuditId;

	/** Closed allowlist used by start; ignored by status. */
	UPROPERTY()
	TArray<FString> Kinds;

	/** Per-kind loaded-object scan bound for start. Range: 64..4096. */
	UPROPERTY()
	int32 MaxObjectsScanned = 2048;

	/** Maximum findings on a completed status page. Hard maximum: 128. */
	UPROPERTY()
	int32 PageSize = 64;

	/** Opaque completed-result cursor. Must be empty for start. */
	UPROPERTY()
	FString Cursor;

	/** Approximate serialized finding budget. Range: 8192..262144 bytes. */
	UPROPERTY()
	int32 MaxOutputBytes = 128 * 1024;
};

/** Status/result envelope for a bounded value-only audit job. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsAuditResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.run-audit.v1");

	/** accepted, running, complete, partial, not_found, conflict, invalid_request, busy, or unavailable. */
	UPROPERTY()
	FString Status = TEXT("unavailable");

	UPROPERTY()
	FString Action;

	UPROPERTY()
	FString AuditId;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString SnapshotFingerprint;

	UPROPERTY()
	FString StartedUtc;

	UPROPERTY()
	FString CompletedUtc;

	UPROPERTY()
	FString ObservationScope = TEXT("bounded_loaded_editor_and_registry_readiness");

	UPROPERTY()
	bool bAccepted = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bComplete = false;

	UPROPERTY()
	bool bIncomplete = true;

	UPROPERTY()
	bool bCaptureWorkBoundReached = false;

	UPROPERTY()
	bool bFindingBoundReached = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	bool bOutputBudgetReached = false;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bHasMore = false;

	UPROPERTY()
	FString CursorStatus = TEXT("none");

	UPROPERTY()
	FString CursorDiagnosticCode;

	UPROPERTY()
	FString NextCursor;

	/** Always not_performed; this bounded audit never certifies deletion/orphan status. */
	UPROPERTY()
	FString DeletionAssessment = TEXT("not_performed");

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsFinding> Findings;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

struct FHyperAIStudioDiagnosticsNormalizedAuditRequest
{
	FString AuditId;
	TArray<FString> Kinds;
	int32 MaxObjectsScanned = 0;
	FString RequestFingerprint;
};

/** Immutable retained audit job; full findings remain value-only and hard-bounded. */
struct FHyperAIStudioDiagnosticsStoredAudit
{
	FString AuditId;
	FString RequestFingerprint;
	FString SnapshotFingerprint;
	FString Status = TEXT("running");
	FString StartedUtc;
	FString CompletedUtc;
	bool bIncomplete = true;
	bool bCaptureWorkBoundReached = false;
	bool bFindingBoundReached = false;
	TArray<FHyperAIStudioDiagnosticsFinding> Findings;
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

/** Pure audit contract/analyzer. */
class FHyperAIStudioAuditContracts final
{
public:
	static constexpr int32 MaxKinds = 5;
	static constexpr int32 MaxObjectsScanned = 4096;
	static constexpr int32 MaxStoredFindings = 512;

	static bool NormalizeStartRequest(
		const FHyperAIStudioDiagnosticsAuditRequest& Request,
		FHyperAIStudioDiagnosticsNormalizedAuditRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError);

	static FHyperAIStudioDiagnosticsStoredAudit AnalyzeSnapshot(
		const FHyperAIStudioDiagnosticsValueSnapshot& Snapshot,
		const FHyperAIStudioDiagnosticsNormalizedAuditRequest& Request,
		const FString& StartedUtc);
};

/** Bounded process-local read-job store. One pure worker runs at a time. */
class FHyperAIStudioDiagnosticsAuditStore final
{
public:
	static FHyperAIStudioDiagnosticsAuditStore& Get();

	void Startup();
	void Shutdown();

	FHyperAIStudioDiagnosticsAuditResult Execute(
		const FHyperAIStudioDiagnosticsAuditRequest& Request);

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	static constexpr int32 MaxRetainedJobs = 32;
	FHyperAIStudioDiagnosticsAuditResult Start(
		const FHyperAIStudioDiagnosticsAuditRequest& Request);
	FHyperAIStudioDiagnosticsAuditResult Status(
		const FHyperAIStudioDiagnosticsAuditRequest& Request);
	FHyperAIStudioDiagnosticsAuditResult Project(
		const FHyperAIStudioDiagnosticsStoredAudit& Audit,
		const FHyperAIStudioDiagnosticsAuditRequest& Request,
		bool bReplay) const;
	void Commit(FHyperAIStudioDiagnosticsStoredAudit&& Audit);
	void PruneCompletedLocked();

	mutable FCriticalSection Mutex;
	TMap<FString, FHyperAIStudioDiagnosticsStoredAudit> Audits;
	TArray<FString> RetentionOrder;
	TFuture<void> WorkerFuture;
	bool bWorkerRunning = false;
	bool bShuttingDown = false;
};

/** Source candidate; production registration remains admission-evidence controlled. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioAuditToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Start or poll an idempotent bounded audit whose heavy value analysis runs off the game thread. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Diagnostics")
	static FHyperAIStudioDiagnosticsAuditResult hyper_run_audit(
		const FHyperAIStudioDiagnosticsAuditRequest& Request);
};
