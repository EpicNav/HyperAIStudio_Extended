// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioUIToolset.h"

/** Detached snapshot: no UObject pointer crosses into validation, paging, or shadow replay. */
struct FHyperAIStudioUIValueSnapshot
{
	TArray<FHyperAIUIRecord> Records;
	TArray<FHyperAIUIIssue> CaptureIssues;
	FString PersistedFingerprint;
	FString VolatileObservationFingerprint;
	bool bComplete = true;
	bool bContainsVolatile = false;
	bool bPackageEvidenceComplete = true;
	bool bAllTargetsLoadedFromDisk = true;
	bool bAllTargetPackagesClean = true;
	bool bAllTargetClassesExact = true;
	int32 LoadedObjectsScanned = 0;
};

struct FHyperAIStudioUIValidationOptions
{
	bool bCheckLayout = true;
	bool bCheckAccessibility = true;
	bool bCheckAnimations = true;
	bool bCheckBindings = true;
	bool bCheckMVVM = true;
	int32 MaxIssues = 128;
};

/** Pure validation is intentionally separate from UObject capture and result pagination. */
class FHyperAIStudioUIValueValidator final
{
public:
	static TArray<FHyperAIUIIssue> Validate(
		const FHyperAIStudioUIValueSnapshot& Snapshot,
		const FHyperAIStudioUIValidationOptions& Options,
		bool& bOutTruncated);
};

class FHyperAIStudioUIValueContracts final
{
public:
	static bool ComputeFingerprints(FHyperAIStudioUIValueSnapshot& Snapshot, FString& OutError);
	static FString ComputeRecordFingerprint(const FHyperAIUIRecord& Record);
	static int32 EstimateRecordBytes(const FHyperAIUIRecord& Record);
	static int32 EstimateIssueBytes(const FHyperAIUIIssue& Issue);
	static bool ReplayShadowPlan(
		const FHyperAIStudioUIValueSnapshot& Base,
		const TArray<FHyperAIStudioUIBackendOperation>& Operations,
		FHyperAIStudioUIValueSnapshot& OutDesired,
		FHyperAIUIPlanEffects& OutEffects,
		TArray<FHyperAIUIIssue>& OutIssues,
		FString& OutError);
};

/** Game-thread loaded-only capture. It never loads an asset or requests/creates an MVVM view. */
class FHyperAIStudioUICapture final
{
public:
	static bool Capture(
		const TArray<FString>& TargetPaths,
		bool bIncludeTree,
		bool bIncludeLayout,
		bool bIncludeAnimations,
		bool bIncludeLegacyBindings,
		bool bIncludeMVVM,
		bool bIncludeVolatile,
		int32 DeadlineMs,
		FHyperAIStudioUIValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
};
