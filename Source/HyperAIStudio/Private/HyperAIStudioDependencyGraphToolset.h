// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDependencyGraphToolset.generated.h"

/**
 * Bounded request for a dependency graph rooted at exactly one project asset.
 *
 * Categories are an allowlist containing one or more of: package, manage,
 * searchable_name. Properties are an optional any-match allowlist containing:
 * hard, soft, game, editor_only, build, not_build, direct, indirect,
 * cook_rule, chunk_only, or none. An empty Properties array accepts every
 * property combination in the selected categories.
 */
USTRUCT(BlueprintType)
struct FHyperAIStudioDependencyGraphRequest
{
	GENERATED_BODY()

	FHyperAIStudioDependencyGraphRequest()
	{
		Categories.Add(TEXT("package"));
	}

	/** Canonical /Game long package path or canonical /Game object path. */
	UPROPERTY()
	FString AssetPath;

	/** dependencies, referencers, or both. */
	UPROPERTY()
	FString Direction = TEXT("both");

	/** Edge-category allowlist. */
	UPROPERTY()
	TArray<FString> Categories;

	/** Optional edge-property any-match allowlist. Empty means all properties. */
	UPROPERTY()
	TArray<FString> Properties;

	/** Root is depth 0. Hard maximum: 16. */
	UPROPERTY()
	int32 MaxDepth = 2;

	/** Maximum unique graph identifiers, including the root. Hard maximum: 4096. */
	UPROPERTY()
	int32 MaxNodes = 512;

	/** Maximum records returned on this page. Hard maximum: 256. */
	UPROPERTY()
	int32 PageSize = 128;

	/** Opaque cursor from a previous identical request and unchanged graph snapshot. */
	UPROPERTY()
	FString Cursor;
};

/** Stable machine-readable diagnostic carried inside every result envelope. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDependencyGraphDiagnostic
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

/**
 * One deterministic page record. Kind is node, edge, or scc_member. Fields not
 * applicable to the selected Kind remain empty/default.
 */
USTRUCT(BlueprintType)
struct FHyperAIStudioDependencyGraphRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString RecordId;

	/** Node or SCC-member identifier. */
	UPROPERTY()
	FString Identifier;

	/** Minimum traversal depth for a node; otherwise -1. */
	UPROPERTY()
	int32 Depth = -1;

	UPROPERTY()
	bool bRoot = false;

	/** Canonical source of a source_depends_on_target edge. */
	UPROPERTY()
	FString SourceIdentifier;

	/** Canonical target of a source_depends_on_target edge. */
	UPROPERTY()
	FString TargetIdentifier;

	UPROPERTY()
	FString Relation;

	UPROPERTY()
	FString Category;

	UPROPERTY()
	TArray<FString> Properties;

	/** Asset Registry category/property explanation when available. */
	UPROPERTY()
	FString Reason;

	UPROPERTY()
	bool bReasonAvailable = false;

	/** Stable hash-derived identifier shared by all members of one SCC. */
	UPROPERTY()
	FString ComponentId;

	UPROPERTY()
	int32 ComponentSize = 0;

	UPROPERTY()
	bool bCycle = false;
};

/** Stable reflected result for the HyperAI dependency graph candidate. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDependencyGraphResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.asset-dependency-graph.v2");

	/** complete_on_disk, partial, not_found, invalid_request, invalid_snapshot, or unavailable. */
	UPROPERTY()
	FString Status = TEXT("unavailable");

	UPROPERTY()
	FString InputAssetPath;

	UPROPERTY()
	FString RootPackage;

	UPROPERTY()
	FString Direction;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString SnapshotFingerprint;

	/** ready, indexing, incomplete, or unavailable. */
	UPROPERTY()
	FString RegistryStatus = TEXT("unavailable");

	/** Asset Registry dependency/referencer APIs report their indexed on-disk relation state. */
	UPROPERTY()
	FString ObservationScope = TEXT("asset_registry_on_disk");

	UPROPERTY()
	bool bIncludesUnsavedChanges = false;

	/** UE 5.8 relation APIs allocate their raw result array before HyperAI can bound processing. */
	UPROPERTY()
	bool bUpstreamRelationAllocationBounded = false;

	UPROPERTY()
	bool bRegistryLoading = false;

	UPROPERTY()
	bool bRegistryGathering = false;

	UPROPERTY()
	bool bSearchAllAssetsObserved = false;

	UPROPERTY()
	bool bIncomplete = true;

	UPROPERTY()
	bool bGraphTruncated = false;

	UPROPERTY()
	bool bDepthLimitReached = false;

	UPROPERTY()
	bool bNodeLimitReached = false;

	UPROPERTY()
	bool bEdgeLimitReached = false;

	UPROPERTY()
	bool bCaptureWorkBudgetReached = false;

	UPROPERTY()
	int64 RawRelationRowsObserved = 0;

	UPROPERTY()
	int64 RawRelationRowsProcessed = 0;

	UPROPERTY()
	int32 DirtyLoadedPackageCount = 0;

	UPROPERTY()
	bool bDirtyLoadedPackageListTruncated = false;

	UPROPERTY()
	TArray<FString> DirtyLoadedPackages;

	UPROPERTY()
	int32 NodeCount = 0;

	UPROPERTY()
	int32 EdgeCount = 0;

	UPROPERTY()
	int32 StronglyConnectedComponentCount = 0;

	UPROPERTY()
	int32 CycleComponentCount = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 PageSize = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bHasMore = false;

	/** True when the graph was bounded or this response is one page of a larger result. */
	UPROPERTY()
	bool bTruncated = false;

	/** none, accepted, or rejected. */
	UPROPERTY()
	FString CursorStatus = TEXT("none");

	UPROPERTY()
	FString CursorDiagnosticCode;

	UPROPERTY()
	FString NextCursor;

	/** Always not_performed: referencer absence is never an orphan/delete verdict. */
	UPROPERTY()
	FString DeletionAssessment = TEXT("not_performed");

	UPROPERTY()
	FString SafetyNotice = TEXT("Referencer absence is not proof that an asset is orphaned or safe to delete.");

	UPROPERTY()
	TArray<FHyperAIStudioDependencyGraphRecord> Records;

	UPROPERTY()
	TArray<FHyperAIStudioDependencyGraphDiagnostic> Diagnostics;
};

/** Canonical, validated value-only request used by capture and analysis. */
struct FHyperAIStudioNormalizedDependencyGraphRequest
{
	FString InputAssetPath;
	FString RootPackage;
	FString Direction;
	TArray<FString> Categories;
	TArray<FString> Properties;
	int32 MaxDepth = 0;
	int32 MaxNodes = 0;
	int32 PageSize = 0;
	FString Cursor;
	FString RequestFingerprint;
};

/** Value-only edge copied from Asset Registry state before graph analysis. */
struct FHyperAIStudioDependencyGraphSnapshotEdge
{
	FString SourceIdentifier;
	FString TargetIdentifier;
	FString Category;
	TArray<FString> Properties;
	FString Reason;
	bool bReasonAvailable = false;
};

/**
 * Immutable-by-convention Asset Registry snapshot. It contains no UObject,
 * registry, module, or future handles and is safe for pure analysis/tests.
 */
struct FHyperAIStudioDependencyGraphSnapshot
{
	FString RootPackage;
	bool bRootFound = false;
	bool bRegistryAvailable = true;
	bool bRegistryGatheringAtStart = false;
	bool bRegistryGatheringAtEnd = false;
	bool bSearchAllAssetsAtStart = false;
	bool bSearchAllAssetsAtEnd = false;
	/** True only for a core-owned bounded immutable relation cache, never for UE's raw TArray APIs. */
	bool bUpstreamRelationAllocationBounded = false;
	bool bQueryIncomplete = false;
	bool bCaptureDepthLimitReached = false;
	bool bCaptureNodeLimitReached = false;
	bool bCaptureEdgeLimitReached = false;
	bool bCaptureWorkBudgetReached = false;
	int64 RawRelationRowsObserved = 0;
	int64 RawRelationRowsProcessed = 0;
	TArray<FString> DirtyLoadedPackages;
	bool bDirtyLoadedPackageListTruncated = false;
	TArray<FHyperAIStudioDependencyGraphSnapshotEdge> Edges;
	TArray<FHyperAIStudioDependencyGraphDiagnostic> Diagnostics;
};

/** Pure, deterministic graph analyzer and cursor codec. */
class FHyperAIStudioDependencyGraphAnalyzer
{
public:
	static constexpr int32 HardMaxDepth = 16;
	static constexpr int32 HardMaxNodes = 4096;
	static constexpr int32 HardMaxPageSize = 256;
	static constexpr int32 HardMaxEdges = 16384;
	static constexpr int32 HardMaxRawRowsProcessed = 32768;
	static constexpr int32 HardMaxDiagnostics = 32;
	static constexpr int32 HardMaxDirtyLoadedPackages = 64;
	static constexpr int32 HardMaxPathChars = 2048;
	static constexpr int32 HardMaxCursorChars = 256;

	static bool NormalizeRequest(
		const FHyperAIStudioDependencyGraphRequest& Request,
		FHyperAIStudioNormalizedDependencyGraphRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static FHyperAIStudioDependencyGraphResult Analyze(
		const FHyperAIStudioDependencyGraphSnapshot& Snapshot,
		const FHyperAIStudioDependencyGraphRequest& Request,
		const FHyperAIStudioNormalizedDependencyGraphRequest* PreparedRequest = nullptr);
};

/** Game-thread nonblocking package-state reader; relation edges require a bounded immutable cache. */
class FHyperAIStudioDependencyGraphSnapshotBuilder
{
public:
	static FHyperAIStudioDependencyGraphSnapshot Capture(
		const FHyperAIStudioNormalizedDependencyGraphRequest& Request);

	/** Pure testable selector used to prove bounded, order-independent high-fanout selection. */
	static TArray<FHyperAIStudioDependencyGraphSnapshotEdge> SelectDeterministicBoundedEdges(
		const TArray<FHyperAIStudioDependencyGraphSnapshotEdge>& Source,
		int32 MaxEdges,
		bool& bOutTruncated);
};

/**
 * Registered source candidate. HyperAIStudio's central registration owner
 * registers and unregisters it alongside the core native read toolset; the
 * packaged/live admission matrix remains authoritative for product claims.
 */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioDependencyGraphToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/** Returns a bounded immutable dependency view; live UE relation arrays are refused until a bounded async cache is attached. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Diagnostics")
	static FHyperAIStudioDependencyGraphResult hyper_asset_dependency_graph(
		const FHyperAIStudioDependencyGraphRequest& Request);
};
