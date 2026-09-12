// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDiagnosticsCommon.h"
#include "Misc/OutputDevice.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLogTailToolset.generated.h"

/** Closed filter over HyperAIStudio's bounded current-process log ring. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsLogTailRequest
{
	GENERATED_BODY()

	/** Optional exact category filters. Empty means every category retained by the bounded ring. Hard maximum: 16. */
	UPROPERTY()
	TArray<FString> Categories;

	/** fatal, error, warning, display, log, verbose, or very_verbose. */
	UPROPERTY()
	FString MaxVerbosity = TEXT("log");

	/** Optional case-insensitive bounded substring filter. */
	UPROPERTY()
	FString Contains;

	/** Maximum returned entries. Hard maximum: 128. */
	UPROPERTY()
	int32 PageSize = 64;

	/** Opaque polling cursor. Empty returns the newest matching page; non-empty continues forward. */
	UPROPERTY()
	FString Cursor;

	/** Approximate serialized entry budget. Range: 8192..262144 bytes. */
	UPROPERTY()
	int32 MaxOutputBytes = 128 * 1024;
};

USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsLogEntry
{
	GENERATED_BODY()

	UPROPERTY()
	int64 Sequence = 0;

	UPROPERTY()
	double TimeSeconds = 0.0;

	UPROPERTY()
	FString Category;

	UPROPERTY()
	FString Verbosity;

	UPROPERTY()
	FString Message;
};

/** Bounded log page; no log-file path or arbitrary filesystem read is accepted. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsLogTailResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.log-tail.v1");

	/** complete, partial, invalid_request, or unavailable. */
	UPROPERTY()
	FString Status = TEXT("unavailable");

	UPROPERTY()
	FString ObservationScope = TEXT("current_process_bounded_ring");

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString BufferEpoch;

	UPROPERTY()
	bool bAttached = false;

	UPROPERTY()
	bool bIncomplete = true;

	UPROPERTY()
	bool bHistoryGap = false;

	UPROPERTY()
	bool bInitialTailOmittedOlderMatches = false;

	UPROPERTY()
	bool bOutputBudgetReached = false;

	UPROPERTY()
	bool bHasMore = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int64 EarliestRetainedSequence = 0;

	UPROPERTY()
	int64 LatestRetainedSequence = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	FString CursorStatus = TEXT("none");

	UPROPERTY()
	FString CursorDiagnosticCode;

	/** Poll with this cursor to continue or receive only newer retained entries. */
	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsLogEntry> Entries;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

struct FHyperAIStudioDiagnosticsNormalizedLogTailRequest
{
	TArray<FString> Categories;
	int32 MaxVerbosityValue = 0;
	FString MaxVerbosity;
	FString ContainsLower;
	int32 PageSize = 0;
	FString Cursor;
	int32 MaxOutputBytes = 0;
	FString RequestFingerprint;
};

struct FHyperAIStudioDiagnosticsLogSnapshot
{
	FString Epoch;
	bool bAttached = false;
	int64 EarliestSequence = 0;
	int64 LatestSequence = 0;
	TArray<FHyperAIStudioDiagnosticsLogEntry> Entries;
};

/** Pure normalization, cursor, and projection helpers. */
class FHyperAIStudioLogTailContracts final
{
public:
	static constexpr int32 MaxCategories = 16;
	static constexpr int32 MaxCategoryCharacters = 64;
	static constexpr int32 MaxContainsCharacters = 128;
	static constexpr int32 MaxMessageCharacters = 2048;
	static constexpr int32 MaxRingEntries = 2048;
	static constexpr int32 MaxRingCharacters = 1024 * 1024;

	static bool NormalizeRequest(
		const FHyperAIStudioDiagnosticsLogTailRequest& Request,
		FHyperAIStudioDiagnosticsNormalizedLogTailRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError);

	static FHyperAIStudioDiagnosticsLogTailResult Analyze(
		const FHyperAIStudioDiagnosticsLogSnapshot& Snapshot,
		const FHyperAIStudioDiagnosticsLogTailRequest& Request);
};

/** Lifecycle-managed thread-safe output device with fixed entry and character bounds. */
class FHyperAIStudioDiagnosticsLogBuffer final : public FOutputDevice
{
public:
	static FHyperAIStudioDiagnosticsLogBuffer& Get();

	void Startup();
	void Shutdown();
	FHyperAIStudioDiagnosticsLogSnapshot Snapshot() const;

	virtual void Serialize(
		const TCHAR* Value,
		ELogVerbosity::Type Verbosity,
		const FName& Category) override;
	virtual void Serialize(
		const TCHAR* Value,
		ELogVerbosity::Type Verbosity,
		const FName& Category,
		double Time) override;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests(const FString& Epoch = TEXT("0123456789abcdef0123456789abcdef"));
	void AppendForTests(const FHyperAIStudioDiagnosticsLogEntry& Entry);
#endif

private:
	void AppendLocked(FHyperAIStudioDiagnosticsLogEntry&& Entry);
	void RemoveOldestLocked();

	mutable FCriticalSection Mutex;
	TArray<FHyperAIStudioDiagnosticsLogEntry> Slots;
	int32 Count = 0;
	int32 NextSlot = 0;
	int32 TotalCharacters = 0;
	int64 NextSequence = 1;
	FString Epoch;
	bool bAttached = false;
};

/** Source candidate; production registration remains admission-evidence controlled. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioLogTailToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Return a bounded filtered page from the lifecycle-managed current-process ring; never reads arbitrary log files. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Diagnostics")
	static FHyperAIStudioDiagnosticsLogTailResult hyper_log_tail(
		const FHyperAIStudioDiagnosticsLogTailRequest& Request);
};
