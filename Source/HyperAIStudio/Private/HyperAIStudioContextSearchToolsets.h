// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Delegates/Delegate.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioContextSearchToolsets.generated.h"

/** One bounded diagnostic emitted by the context/search read contracts. */
USTRUCT(BlueprintType)
struct FHyperAIReadDiagnostic
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Field;

	UPROPERTY()
	FString Message;
};

/** One projected scalar field. Collections, arbitrary object serialization, and raw file text are never returned. */
USTRUCT(BlueprintType)
struct FHyperAIReadField
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString Value;
};

/** A stable generic record used by the bounded context, scene, batch, and project-search tools. */
USTRUCT(BlueprintType)
struct FHyperAIReadRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString QueryId;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString RecordId;

	UPROPERTY()
	TArray<FHyperAIReadField> Fields;
};

/** Shared envelope with explicit freshness, paging, dirty, on-disk, and incomplete semantics. */
USTRUCT(BlueprintType)
struct FHyperAIReadReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString SnapshotUtc;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	bool bOnDiskOnly = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bIncomplete = false;

	UPROPERTY()
	bool bSourceTruncated = false;

	UPROPERTY()
	bool bOutputBudgetTruncated = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 DirtyPackageCount = 0;

	UPROPERTY()
	bool bDirtyPackageListTruncated = false;

	UPROPERTY()
	TArray<FString> DirtyPackages;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString SnapshotFingerprint;

	UPROPERTY()
	int64 SnapshotGeneration = 0;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 PageSize = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIReadRecord> Records;

	UPROPERTY()
	TArray<FHyperAIReadDiagnostic> Diagnostics;
};

USTRUCT(BlueprintType)
struct FHyperAIContextSnapshotRequest
{
	GENERATED_BODY()

	/** Empty selects the documented safe defaults. Otherwise values must come from the context projection allowlist. */
	UPROPERTY()
	TArray<FString> Fields;

	UPROPERTY()
	int32 MaxSelectedActors = 32;

	/** Asset selection is intentionally outside this hard-bounded compound snapshot; must be zero. */
	UPROPERTY()
	int32 MaxSelectedAssets = 0;

	UPROPERTY()
	int32 MaxBlueprintNodes = 32;

	UPROPERTY()
	bool bIncludeBlueprintPins = true;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAISceneInspectRequest
{
	GENERATED_BODY()

	/** selected, world, or actor. actor requires an exact already-loaded actor path. */
	UPROPERTY()
	FString Scope = TEXT("selected");

	UPROPERTY()
	FString ActorPath;

	/** Empty selects identity, transform, and component identity. */
	UPROPERTY()
	TArray<FString> Fields;

	/** Optional exact reflected scalar property names; only editor/Blueprint-visible scalar values are eligible. */
	UPROPERTY()
	TArray<FString> PropertyNames;

	UPROPERTY()
	int32 MaxActors = 64;

	UPROPERTY()
	int32 MaxComponentsPerActor = 16;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

/** One closed deterministic scene-layout operation. Ordinary actor CRUD/property edits remain Epic delegates. */
USTRUCT(BlueprintType)
struct FHyperAISceneLayoutOperation
{
	GENERATED_BODY()

	/** v1 supports only layout_grid. */
	UPROPERTY()
	FString Variant = TEXT("layout_grid");

	/** Exact already-loaded actor paths in deterministic output order. */
	UPROPERTY()
	TArray<FString> ActorPaths;

	/** One exact actor-scope scene-inspect fingerprint per path, using no scalar properties and the default 16-component bound. */
	UPROPERTY()
	TArray<FString> ExpectedActorRevisions;

	UPROPERTY()
	FVector Origin = FVector::ZeroVector;

	UPROPERTY()
	FVector Spacing = FVector(100.0, 100.0, 0.0);

	UPROPERTY()
	int32 Columns = 1;
};

USTRUCT(BlueprintType)
struct FHyperAISceneApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FHyperAISceneLayoutOperation> Operations;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	/** Required on non-dry retry/submission and must echo the exact dry-run hash. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	int32 MaxGameThreadMs = 100;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAISceneEffectSummary
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 ActorCount = 0;

	UPROPERTY()
	bool bTransactionRequired = false;

	UPROPERTY()
	bool bCompileRequired = false;

	UPROPERTY()
	bool bSaveRequired = false;
};

USTRUCT(BlueprintType)
struct FHyperAISceneApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bStaged = false;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString BaseRevision;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString PreparedContractFingerprint;

	UPROPERTY()
	FHyperAISceneEffectSummary Effects;

	UPROPERTY()
	TArray<FHyperAIReadDiagnostic> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAISceneValidateRequest
{
	GENERATED_BODY()

	/** selected, world, or actor. Validation always captures a fresh full value projection. */
	UPROPERTY()
	FString Scope = TEXT("selected");

	UPROPERTY()
	FString ActorPath;

	UPROPERTY()
	TArray<FString> PropertyNames;

	UPROPERTY()
	int32 MaxActors = 64;

	UPROPERTY()
	int32 MaxComponentsPerActor = 16;

	UPROPERTY()
	int32 MaxIssues = 32;
};

USTRUCT(BlueprintType)
struct FHyperAISceneValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString SnapshotFingerprint;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	TArray<FHyperAIReadDiagnostic> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIProjectSearchRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Query;

	/** Empty searches both asset and cpp_symbol records. */
	UPROPERTY()
	TArray<FString> Kinds;

	UPROPERTY()
	TArray<FString> Fields;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;

	/** Starts a bounded asynchronous refresh when no usable immutable index exists. */
	UPROPERTY()
	bool bStartIndexIfUnavailable = true;
};

USTRUCT(BlueprintType)
struct FHyperAIBatchReadOperation
{
	GENERATED_BODY()

	UPROPERTY()
	FString QueryId;

	/** Allowlisted values: context, scene, project_search, project_index_status. */
	UPROPERTY()
	FString Primitive;

	UPROPERTY()
	FString Scope;

	UPROPERTY()
	FString Query;

	UPROPERTY()
	FString ActorPath;

	UPROPERTY()
	TArray<FString> Kinds;

	UPROPERTY()
	TArray<FString> Fields;

	UPROPERTY()
	TArray<FString> PropertyNames;

	UPROPERTY()
	int32 MaxItems = 32;
};

USTRUCT(BlueprintType)
struct FHyperAIBatchQueryRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FHyperAIBatchReadOperation> Reads;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 131072;
};

/** State of the fixed-root project index; the asset portion stays partial until a bounded async cache is available. */
USTRUCT(BlueprintType)
struct FHyperAIProjectIndexStatus
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope = TEXT("project_on_disk_index");

	UPROPERTY()
	bool bOnDiskOnly = true;

	UPROPERTY()
	bool bIncomplete = true;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bRefreshInProgress = false;

	UPROPERTY()
	bool bRefreshRequested = false;

	UPROPERTY()
	FString StartedUtc;

	UPROPERTY()
	FString CompletedUtc;

	UPROPERTY()
	FString SnapshotFingerprint;

	UPROPERTY()
	int64 Generation = 0;

	UPROPERTY()
	int32 AssetRecordCount = 0;

	UPROPERTY()
	int32 CppFileCount = 0;

	UPROPERTY()
	int32 CppSymbolCount = 0;

	UPROPERTY()
	int32 TotalRecordCount = 0;

	UPROPERTY()
	int64 CppBytesRead = 0;

	UPROPERTY()
	int32 DirtyPackageCount = 0;

	UPROPERTY()
	bool bDirtyPackageListTruncated = false;

	UPROPERTY()
	TArray<FString> DirtyPackages;

	UPROPERTY()
	TArray<FHyperAIReadDiagnostic> Diagnostics;
};

/** Owned toolset classes publish only members of the exact generated context/search source cohort. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioBatchQueryToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Execute only the closed context/scene/project-search/status read primitives against one bounded immutable capture. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Workflow")
	static FHyperAIReadReport hyper_batch_query(const FHyperAIBatchQueryRequest& Request);
};

UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioContextSnapshotToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Capture bounded current map, viewport, selections, and safely observable selected Blueprint graph context. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Context")
	static FHyperAIReadReport hyper_context_snapshot(const FHyperAIContextSnapshotRequest& Request);
};

UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioSceneInspectToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Inspect bounded already-loaded editor actors, components, and explicitly named safe scalar properties. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Scene")
	static FHyperAIReadReport hyper_scene_inspect(const FHyperAISceneInspectRequest& Request);

	/** Seal a deterministic multi-actor layout plan; non-dry execution remains fail-closed until an admitted backend is hosted. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Scene")
	static FHyperAISceneApplyPlanReport hyper_scene_apply_plan(const FHyperAISceneApplyPlanRequest& Request);

	/** Independently recapture and validate bounded scene identity, ownership, transform, component, and scalar-property invariants. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Scene")
	static FHyperAISceneValidateReport hyper_scene_validate(const FHyperAISceneValidateRequest& Request);
};

UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioProjectSearchToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Search the latest immutable fixed-root index; asset results require the bounded async cache and arbitrary file contents are never returned. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Project")
	static FHyperAIReadReport hyper_project_search(const FHyperAIProjectSearchRequest& Request);
};

UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioProjectIndexStatusToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Poll index lifecycle, or request one bounded asynchronous refresh of the fixed project roots. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Project")
	static FHyperAIProjectIndexStatus hyper_project_index_status(bool bRequestRefresh = false);
};

/** Immutable value-only record used after all UObject capture is complete. */
struct FHyperAIContextSearchValueRecord
{
	FString QueryId;
	FString Kind;
	FString RecordId;
	FString SearchTextLower;
	TArray<FHyperAIReadField> Fields;
};

/** Value-only snapshot. Worker/pure analyzers must never carry UObject pointers. */
struct FHyperAIContextSearchValueSnapshot
{
	FString SnapshotUtc;
	FString ObservationScope;
	bool bOnDiskOnly = false;
	bool bDirty = false;
	bool bIncomplete = false;
	bool bSourceTruncated = false;
	bool bDirtyPackageListTruncated = false;
	int64 Generation = 0;
	TArray<FString> DirtyPackages;
	TArray<FHyperAIContextSearchValueRecord> Records;
	TArray<FHyperAIReadDiagnostic> Diagnostics;
};

/** Immutable result of the asynchronous project index worker. */
struct FHyperAIProjectIndexValueSnapshot
{
	FHyperAIContextSearchValueSnapshot Value;
	FString Status = TEXT("uninitialized");
	FString StartedUtc;
	FString CompletedUtc;
	FString SnapshotFingerprint;
	int32 AssetRecordCount = 0;
	int32 CppFileCount = 0;
	int32 CppSymbolCount = 0;
	int64 CppBytesRead = 0;
};

/** Same-handle bounded source read. Text is internal indexing input and is never exposed by an MCP result. */
struct FHyperAIProjectSourceReadResult
{
	FString RelativePath;
	FString Text;
	int64 BytesRead = 0;
};

struct FHyperAIContextSearchManifestEntry
{
	FString Name;
	FString Toolset;
	enum class EAdmissionState : uint8
	{
		Planned,
		SourceCandidate,
		Admitted
	} AdmissionState = EAdmissionState::Planned;
};

/** Pure bounds, normalization, fingerprint, cursor, projection, and symbol-index seams. */
class FHyperAIStudioContextSearchContracts final
{
public:
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxDiagnostics = 32;
	static constexpr int32 MaxDiagnosticCharacters = 512;
	static constexpr int32 MaxFieldNameCharacters = 64;
	static constexpr int32 MaxFieldValueCharacters = 1024;
	static constexpr int32 MaxSelectedActors = 128;
	static constexpr int32 MaxSelectedAssets = 0;
	static constexpr int32 MaxBlueprintNodes = 128;
	static constexpr int32 MaxBlueprintPins = 512;
	static constexpr int32 MaxWorldActors = 256;
	static constexpr int32 MaxWorldActorsScanned = 2048;
	static constexpr int32 MaxComponentsPerActor = 64;
	static constexpr int32 MaxProperties = 16;
	static constexpr int32 MaxBatchReads = 8;
	static constexpr int32 MaxProjectIndexRecords = 8192;
	static constexpr int32 MaxProjectAssets = 2048;
	static constexpr int32 MaxCppFiles = 2048;
	static constexpr int32 MaxCppSymbols = 6144;
	static constexpr int32 MaxCppDirectories = 1024;
	static constexpr int32 MaxCppDirectoryEntries = 8192;
	static constexpr int64 MaxCppFileBytes = 2 * 1024 * 1024;
	static constexpr int64 MaxCppTotalBytes = 16 * 1024 * 1024;

	static const TArray<FHyperAIContextSearchManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(const FString& QualifiedToolset, bool bAllowPendingForTests);
	static FString AdmissionStateToString(FHyperAIContextSearchManifestEntry::EAdmissionState State);

	static bool NormalizeProjection(
		const TArray<FString>& Requested,
		const TSet<FString>& Allowed,
		const TArray<FString>& Defaults,
		TArray<FString>& OutProjection,
		FString& OutErrorCode,
		FString& OutError);
	static bool NormalizeStringSet(
		const TArray<FString>& Requested,
		const TSet<FString>& Allowed,
		int32 HardMax,
		bool bAllowEmpty,
		TArray<FString>& OutValues,
		FString& OutErrorCode,
		FString& OutError);
	static FString HashTokens(const TArray<FString>& Tokens);
	static FString MakeCursor(
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		int32 Offset);
	static bool ParseCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		int32 TotalRecords,
		int32& OutOffset,
		FString& OutErrorCode,
		FString& OutError);
	static FString ComputeSnapshotFingerprint(const FHyperAIContextSearchValueSnapshot& Snapshot);
	/** Pure independent scene value-model validation; it does not read UObjects or share mutation code. */
	static TArray<FHyperAIReadDiagnostic> ValidateSceneSnapshot(
		const FHyperAIContextSearchValueSnapshot& Snapshot,
		int32 MaxIssues,
		bool& bOutTruncated);
	static bool ProjectPage(
		const FHyperAIContextSearchValueSnapshot& Snapshot,
		const TArray<FString>& Projection,
		const FString& RequestFingerprint,
		int32 PageSize,
		const FString& Cursor,
		int32 MaxOutputBytes,
		FHyperAIReadReport& OutReport);

	static bool IsProjectContainedSourcePath(
		const FString& ProjectRoot,
		const FString& CandidatePath,
		FString& OutRelativePath);
	static bool EnumerateProjectCppSourceFiles(
		const FString& ProjectRoot,
		int32 MaxDirectories,
		int32 MaxEntries,
		TArray<FString>& OutCandidatePaths,
		bool& bOutTruncated,
		FString& OutErrorCode);
	static bool ReadProjectContainedCppSource(
		const FString& ProjectRoot,
		const FString& CandidatePath,
		FHyperAIProjectSourceReadResult& OutRead,
		FString& OutErrorCode,
		int64 MaxBytes = MaxCppFileBytes);
	static bool TryCopyBoundedPreview(const FString& Source, FString& OutPreview);
	static bool TryCopyBoundedTextPreview(const FText& Source, FString& OutPreview);
	static TArray<FHyperAIContextSearchValueRecord> ExtractCppSymbols(
		const FString& ProjectRelativePath,
		const FString& SourceText,
		int32 MaxSymbols,
		bool& bOutTruncated);
	static FHyperAIReadReport SearchProjectIndex(
		const FHyperAIProjectIndexValueSnapshot& Snapshot,
		const FHyperAIProjectSearchRequest& Request);
	static FHyperAIReadReport AnalyzeBatch(
		const FHyperAIContextSearchValueSnapshot& EditorSnapshot,
		const FHyperAIProjectIndexValueSnapshot& ProjectSnapshot,
		const FHyperAIBatchQueryRequest& Request);
};

/** Thread-safe process-local store. UObject/AssetRegistry capture is completed on the game thread before the worker starts. */
class FHyperAIStudioProjectIndexStore final
{
public:
	static FHyperAIStudioProjectIndexStore& Get();
	FHyperAIProjectIndexStatus GetStatus() const;
	FHyperAIProjectIndexValueSnapshot GetSnapshot() const;
	/** Shared immutable view; repeated reads do not deep-copy the complete project index. */
	TSharedRef<const FHyperAIProjectIndexValueSnapshot, ESPMode::ThreadSafe> GetSnapshotShared() const;
	bool RequestRefresh(FString& OutDiagnostic);
	void Shutdown();

#if WITH_DEV_AUTOMATION_TESTS
	void SetSnapshotForTests(const FHyperAIProjectIndexValueSnapshot& Snapshot);
#endif

private:
	FHyperAIStudioProjectIndexStore() = default;
	void CommitWorkerResult(FHyperAIProjectIndexValueSnapshot&& Result);

	mutable FCriticalSection Mutex;
	FHyperAIProjectIndexValueSnapshot Snapshot;
	mutable TSharedPtr<const FHyperAIProjectIndexValueSnapshot, ESPMode::ThreadSafe> SnapshotView;
	bool bWorkerRunning = false;
	bool bShuttingDown = false;
	int64 NextGeneration = 1;
	TFuture<void> WorkerFuture;
};

/** Registration owner to be wired by the module after source-candidate review. */
class FHyperAIStudioContextSearchRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();
	void RefreshMcpToolsIfSafe() const;

	FDelegateHandle PostEngineInitHandle;
	TArray<UClass*> OwnedToolsets;
	bool bStarted = false;
};
