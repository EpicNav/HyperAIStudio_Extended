// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDiagnosticsCommon.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDiagnoseToolset.generated.h"

/** Closed, bounded synchronous diagnostic request. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsDiagnoseRequest
{
	GENERATED_BODY()

	FHyperAIStudioDiagnosticsDiagnoseRequest()
	{
		Checks = {
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
	}

	/** Allowlist: asset_context, asset_registry_readiness, current_map_state, dirty_loaded_packages, loaded_blueprint_health, plugin_readiness. */
	UPROPERTY()
	TArray<FString> Checks;

	/** Exact canonical /Game object paths used only by asset_context. Hard maximum: 64. */
	UPROPERTY()
	TArray<FString> AssetPaths;

	/** Maximum loaded Blueprints or packages inspected per relevant check. Hard maximum: 8192. */
	UPROPERTY()
	int32 MaxObjectsScanned = 2048;

	/** Maximum findings returned on this page. Hard maximum: 128. */
	UPROPERTY()
	int32 PageSize = 64;

	/** Opaque cursor from an identical request and unchanged diagnostic snapshot. */
	UPROPERTY()
	FString Cursor;

	/** Approximate serialized finding budget. Range: 8192..262144 bytes. */
	UPROPERTY()
	int32 MaxOutputBytes = 128 * 1024;
};

/** Bounded diagnostic report over loaded editor state and exact on-disk Asset Registry lookups. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsDiagnoseResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.diagnose.v1");

	/** complete, partial, invalid_request, or unavailable. */
	UPROPERTY()
	FString Status = TEXT("unavailable");

	UPROPERTY()
	FString ObservationScope = TEXT("loaded_editor_and_exact_asset_registry");

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString SnapshotFingerprint;

	UPROPERTY()
	bool bIncomplete = true;

	UPROPERTY()
	bool bCaptureWorkBoundReached = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	bool bOutputBudgetReached = false;

	UPROPERTY()
	int32 BlueprintsScanned = 0;

	UPROPERTY()
	int32 PackagesScanned = 0;

	UPROPERTY()
	int32 DirtyLoadedPackageCount = 0;

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

	/** Always not_performed; partial registry/read evidence never implies deletion safety. */
	UPROPERTY()
	FString DeletionAssessment = TEXT("not_performed");

	UPROPERTY()
	FString SafetyNotice = TEXT("No orphan or safe-to-delete conclusion is inferred from diagnostics or partial Asset Registry state.");

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsFinding> Findings;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

struct FHyperAIStudioDiagnosticsPluginSnapshot
{
	FString Name;
	bool bInstalled = false;
	bool bEnabled = false;
};

struct FHyperAIStudioDiagnosticsAssetSnapshot
{
	FString RequestedPath;
	FString ObjectPath;
	FString PackageName;
	FString AssetName;
	FString ClassPath;
	bool bPackageStateKnown = false;
	bool bPackageExists = false;
	bool bPackageAbsent = false;
	/** Exact already-loaded object identity; never interpreted as an on-disk registry record. */
	bool bFound = false;
};

struct FHyperAIStudioDiagnosticsBlueprintSnapshot
{
	FString ObjectPath;
	FString Status;
	bool bPackageDirty = false;
};

/** Immutable-by-convention value snapshot; no UObject or registry pointers survive capture. */
struct FHyperAIStudioDiagnosticsValueSnapshot
{
	FString RequestFingerprint;
	FString CapturedUtc;
	bool bIncomplete = false;
	bool bCaptureWorkBoundReached = false;
	bool bAssetRegistryAvailable = false;
	bool bAssetRegistryGathering = false;
	bool bAssetRegistrySearchAllObserved = false;
	bool bEditorWorldAvailable = false;
	FString CurrentMapPackage;
	bool bCurrentMapDirty = false;
	int32 BlueprintsScanned = 0;
	int32 PackagesScanned = 0;
	TArray<FHyperAIStudioDiagnosticsPluginSnapshot> Plugins;
	TArray<FHyperAIStudioDiagnosticsAssetSnapshot> Assets;
	TArray<FHyperAIStudioDiagnosticsBlueprintSnapshot> Blueprints;
	TArray<FString> DirtyLoadedPackages;
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

struct FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest
{
	TArray<FString> Checks;
	TArray<FString> AssetPaths;
	int32 MaxObjectsScanned = 0;
	int32 PageSize = 0;
	FString Cursor;
	int32 MaxOutputBytes = 0;
	FString RequestFingerprint;
};

/** Pure request/analyzer seams plus the game-thread value capture. */
class FHyperAIStudioDiagnoseContracts final
{
public:
	static constexpr int32 MaxChecks = 6;
	static constexpr int32 MaxAssetPaths = 64;
	static constexpr int32 MaxFindings = 1024;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MinObjectsScanned = 64;
	static constexpr int32 MaxObjectsScanned = 8192;
	static constexpr double MaxCaptureSeconds = 0.050;

	static bool NormalizeRequest(
		const FHyperAIStudioDiagnosticsDiagnoseRequest& Request,
		FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError);

	static FHyperAIStudioDiagnosticsValueSnapshot Capture(
		const FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest& Request);

	static FHyperAIStudioDiagnosticsDiagnoseResult Analyze(
		const FHyperAIStudioDiagnosticsValueSnapshot& Snapshot,
		const FHyperAIStudioDiagnosticsDiagnoseRequest& Request,
		const FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest* PreparedRequest = nullptr);
};

/** Source candidate; production registration remains admission-evidence controlled. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioDiagnoseToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Diagnose only allowlisted, bounded loaded/editor and exact on-disk asset state. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Diagnostics")
	static FHyperAIStudioDiagnosticsDiagnoseResult hyper_diagnose(
		const FHyperAIStudioDiagnosticsDiagnoseRequest& Request);
};
