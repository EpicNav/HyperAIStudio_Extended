// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

#include "HyperAIStudioDiagnosticsCommon.generated.h"

/** One bounded machine-readable diagnostic emitted by a diagnostics tool. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsDiagnostic
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

/** A projected scalar field; diagnostics tools never return arbitrary UObject serialization. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsField
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Value;
};

/** Stable, sortable finding shared by the synchronous diagnose and asynchronous audit contracts. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsFinding
{
	GENERATED_BODY()

	UPROPERTY()
	FString RecordId;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Subject;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsField> Fields;
};

/** Value-only page result used by tool-specific result envelopes. */
struct FHyperAIStudioDiagnosticsProjectedPage
{
	FString CursorStatus = TEXT("none");
	FString CursorDiagnosticCode;
	FString NextCursor;
	int32 TotalRecords = 0;
	int32 PageOffset = 0;
	int32 ReturnedRecords = 0;
	bool bHasMore = false;
	bool bOutputBudgetReached = false;
	TArray<FHyperAIStudioDiagnosticsFinding> Findings;
};

/** Pure shared bounds, canonical hashing, diagnostics, and cursor/page helpers. */
class FHyperAIStudioDiagnosticsCommon final
{
public:
	static constexpr int32 MaxDiagnostics = 32;
	static constexpr int32 MaxDiagnosticCharacters = 512;
	static constexpr int32 MaxFindingFields = 12;
	static constexpr int32 MaxFieldNameCharacters = 64;
	static constexpr int32 MaxFieldValueCharacters = 1024;
	static constexpr int32 MaxFindingMessageCharacters = 768;
	static constexpr int32 MaxSubjectCharacters = 1024;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MinOutputBytes = 8 * 1024;
	static constexpr int32 MaxOutputBytes = 256 * 1024;

	static bool ContainsEmbeddedNull(const FString& Value);
	static bool HasWellFormedUtf16(const FString& Value);
	static bool IsSafeIdentifier(const FString& Value, int32 MinCharacters, int32 MaxCharacters);
	static FString Clip(const FString& Value, int32 MaxCharacters);
	static FString HashTokens(const TArray<FString>& Tokens);
	static FString MakeRecordId(const FString& Kind, const FString& Subject, const FString& Code);
	static int32 Utf8Bytes(const FString& Value);
	static int32 EstimateFindingBytes(const FHyperAIStudioDiagnosticsFinding& Finding);

	static void AddDiagnostic(
		TArray<FHyperAIStudioDiagnosticsDiagnostic>& Diagnostics,
		const FString& Code,
		const FString& Severity,
		const FString& Field,
		const FString& Message);

	static void AddDiagnosticOnce(
		TArray<FHyperAIStudioDiagnosticsDiagnostic>& Diagnostics,
		const FString& Code,
		const FString& Severity,
		const FString& Field,
		const FString& Message);

	static void AddFindingField(
		FHyperAIStudioDiagnosticsFinding& Finding,
		const FString& Name,
		const FString& Value);

	static bool NormalizeAllowlist(
		const TArray<FString>& Requested,
		const TSet<FString>& Allowed,
		const TArray<FString>& Defaults,
		int32 HardMax,
		TArray<FString>& OutValues,
		FString& OutErrorCode,
		FString& OutError);

	static bool ValidatePageBounds(
		int32 PageSize,
		const FString& Cursor,
		int32 MaxOutputBytes,
		FString& OutErrorCode,
		FString& OutError);

	static FString ComputeFindingsFingerprint(
		const TArray<FHyperAIStudioDiagnosticsFinding>& Findings,
		const TArray<FString>& StateTokens);

	static bool ProjectFindings(
		const TArray<FHyperAIStudioDiagnosticsFinding>& SortedFindings,
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		int32 PageSize,
		const FString& Cursor,
		int32 MaxOutputBytes,
		FHyperAIStudioDiagnosticsProjectedPage& OutPage,
		FString& OutErrorCode,
		FString& OutError);
};
