// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioWorldbuildingToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIWorldIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Bounded projection shared by loaded-object and on-disk Asset Registry captures. */
USTRUCT(BlueprintType)
struct FHyperAIWorldRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString PackageName;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	FString WorldPath;

	UPROPERTY()
	FString ParentPath;

	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	int32 PrimaryCount = 0;

	UPROPERTY()
	int32 SecondaryCount = 0;

	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	bool bHasBounds = false;

	UPROPERTY()
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY()
	FVector BoundsMax = FVector::ZeroVector;

	UPROPERTY()
	TArray<FString> Details;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldInspectRequest
{
	GENERATED_BODY()

	/** loaded_only, on_disk_index, or loaded_and_on_disk. No object is synchronously loaded. */
	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	/** Empty means every allowlisted variant. */
	UPROPERTY()
	TArray<FString> Variants;

	/** Exact object paths only. On-disk class inventory remains incomplete until an async cache exists. */
	UPROPERTY()
	TArray<FString> ObjectPaths;

	/** Allowlist: identity, package, world, counts, bounds, details. */
	UPROPERTY()
	TArray<FString> Projection;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 SourceRecordsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIWorldRecord> Records;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	TArray<FString> Variants;

	UPROPERTY()
	TArray<FString> ObjectPaths;

	UPROPERTY()
	int32 MaxIssues = 128;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

/**
 * Closed worldbuilding operation. Asset fields accept canonical /Game object paths only.
 * There is deliberately no filesystem path, script, console command, class name, or tool name.
 */
USTRUCT(BlueprintType)
struct FHyperAIWorldPlanOperation
{
	GENERATED_BODY()

	/**
	 * landscape.create|sculpt_delta|paint_layer|import_height_asset|export_height_asset,
	 * foliage.add_type|paint_instances|erase_instances|remove_type,
	 * world_partition.load_region|unload_region, data_layer.set_state, hlod.build,
	 * rvt.bind_volume, water.create_body|set_zone|set_wave_profile,
	 * spline.create|add_point|set_point|remove_point|set_closed,
	 * level_instance.create|update, optimize.merge|proxy|instance,
	 * environment.set_lighting|set_fog|set_sky|set_post_process.
	 */
	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString TargetPath;

	/** Required CAS for an existing target; empty only for create operations. */
	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FString ReferencePath;

	/** Project-contained input/output asset, never a raw file path. */
	UPROPERTY()
	FString SourceAssetPath;

	UPROPERTY()
	FString DestinationAssetPath;

	/** Exact loaded source actor/object paths for merge/proxy/instance operations. */
	UPROPERTY()
	TArray<FString> SourcePaths;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString LayerName;

	UPROPERTY()
	FString State;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Extent = FVector::ZeroVector;

	UPROPERTY()
	TArray<FVector> Points;

	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	double Radius = 0.0;

	UPROPERTY()
	double Strength = 0.0;

	UPROPERTY()
	double Value = 0.0;

	UPROPERTY()
	bool bSetEnabled = false;

	UPROPERTY()
	bool bEnabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Required only for non-dry-run submission to the shared journal route. */
	UPROPERTY()
	FString OperationId;

	/** Echo of the exact dry-run plan hash. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	TArray<FHyperAIWorldPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 EditCount = 0;

	UPROPERTY()
	int32 DestructiveCount = 0;

	UPROPERTY()
	int32 ExternalEffectCount = 0;

	UPROPERTY()
	int32 CreateCount = 0;

	UPROPERTY()
	int32 RemoveCount = 0;

	UPROPERTY()
	bool bTransactionOnce = false;

	UPROPERTY()
	bool bSaveOnce = false;

	UPROPERTY()
	bool bValidateOnce = true;

	UPROPERTY()
	bool bFreshVerifyOnce = true;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldApplyPlanReport
{
	GENERATED_BODY()

	/** True only for a valid dry-run or completed admitted execution. */
	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bRequiresTrustedAuthorization = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass;

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
	FHyperAIWorldPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	int32 PrimaryCount = 0;

	UPROPERTY()
	int32 SecondaryCount = 0;

	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	bool bHasBounds = false;

	UPROPERTY()
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY()
	FVector BoundsMax = FVector::ZeroVector;

	UPROPERTY()
	TArray<FString> Details;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationInspectRequest
{
	GENERATED_BODY()

	/** loaded_only or on_disk_index; the latter is disclosure-only until an async cache exists. */
	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	/** Empty means nav_system, navmesh, nav_link, and nav_area. */
	UPROPERTY()
	TArray<FString> Variants;

	UPROPERTY()
	TArray<FString> ObjectPaths;

	/** Allowlist: identity, counts, bounds, details. */
	UPROPERTY()
	TArray<FString> Projection;

	UPROPERTY()
	/** Requested path proof; currently returns async_path_query_backend_required without executing UE's synchronous helper. */
	bool bQueryPath = false;

	UPROPERTY()
	FVector PathStart = FVector::ZeroVector;

	UPROPERTY()
	FVector PathEnd = FVector::ZeroVector;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 SourceRecordsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	bool bPathQueryAttempted = false;

	UPROPERTY()
	bool bPathValid = false;

	UPROPERTY()
	double PathLength = 0.0;

	UPROPERTY()
	TArray<FVector> PathPoints;

	UPROPERTY()
	TArray<FHyperAINavigationRecord> Records;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	TArray<FString> Variants;

	UPROPERTY()
	TArray<FString> ObjectPaths;

	UPROPERTY()
	bool bQueryPath = false;

	UPROPERTY()
	FVector PathStart = FVector::ZeroVector;

	UPROPERTY()
	FVector PathEnd = FVector::ZeroVector;

	UPROPERTY()
	int32 MaxIssues = 128;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationPlanOperation
{
	GENERATED_BODY()

	/** navmesh.configure|build, nav_link.create|update|remove, nav_area.create|update|remove. */
	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString TargetPath;

	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FString ReferencePath;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Extent = FVector::ZeroVector;

	UPROPERTY()
	FVector LinkStart = FVector::ZeroVector;

	UPROPERTY()
	FVector LinkEnd = FVector::ZeroVector;

	UPROPERTY()
	double AgentRadius = 0.0;

	UPROPERTY()
	double AgentHeight = 0.0;

	UPROPERTY()
	double Cost = 0.0;

	UPROPERTY()
	bool bSetEnabled = false;

	UPROPERTY()
	bool bEnabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	TArray<FHyperAINavigationPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 EditCount = 0;

	UPROPERTY()
	int32 DestructiveCount = 0;

	UPROPERTY()
	int32 ExternalEffectCount = 0;

	UPROPERTY()
	bool bTransactionOnce = false;

	UPROPERTY()
	bool bSaveOnce = false;

	UPROPERTY()
	bool bValidateOnce = true;

	UPROPERTY()
	bool bFreshVerifyOnce = true;
};

USTRUCT(BlueprintType)
struct FHyperAINavigationApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bRequiresTrustedAuthorization = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass;

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
	FHyperAINavigationPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIWorldIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOWORLDBUILDING_API UHyperAIStudioWorldbuildingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Worldbuilding")
	static FHyperAIWorldInspectReport hyper_worldbuilding_inspect(const FHyperAIWorldInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Worldbuilding")
	static FHyperAIWorldApplyPlanReport hyper_worldbuilding_apply_plan(const FHyperAIWorldApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Worldbuilding")
	static FHyperAIWorldValidateReport hyper_worldbuilding_validate(const FHyperAIWorldValidateRequest& Request);
};

UCLASS()
class HYPERAISTUDIOWORLDBUILDING_API UHyperAIStudioNavigationToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Navigation")
	static FHyperAINavigationInspectReport hyper_navigation_inspect(const FHyperAINavigationInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Navigation")
	static FHyperAINavigationApplyPlanReport hyper_navigation_apply_plan(const FHyperAINavigationApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Navigation")
	static FHyperAINavigationValidateReport hyper_navigation_validate(const FHyperAINavigationValidateRequest& Request);
};

enum class EHyperAIWorldSafety : uint8
{
	Edit,
	Destructive,
	ExternalEffect
};

struct FHyperAIWorldBackendOperation
{
	FString Variant;
	EHyperAIWorldSafety Safety = EHyperAIWorldSafety::Edit;
	FString TargetPath;
	FString ExpectedRevision;
	FString ReferencePath;
	FString SourceAssetPath;
	FString DestinationAssetPath;
	TArray<FString> SourcePaths;
	FString Name;
	FString LayerName;
	FString State;
	FVector Location = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	TArray<FVector> Points;
	int32 Index = -1;
	int32 Count = 0;
	double Radius = 0.0;
	double Strength = 0.0;
	double Value = 0.0;
	bool bSetEnabled = false;
	bool bEnabled = false;
};

struct FHyperAINavigationBackendOperation
{
	FString Variant;
	EHyperAIWorldSafety Safety = EHyperAIWorldSafety::Edit;
	FString TargetPath;
	FString ExpectedRevision;
	FString ReferencePath;
	FVector Location = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	FVector LinkStart = FVector::ZeroVector;
	FVector LinkEnd = FVector::ZeroVector;
	double AgentRadius = 0.0;
	double AgentHeight = 0.0;
	double Cost = 0.0;
	bool bSetEnabled = false;
	bool bEnabled = false;
};

/** Detached typed payload used only for shared dry-run sealing and later exact executor staging. */
class FHyperAIWorldTypedArtifactPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TypeId;
	FString SchemaFingerprint;
	FString PackId;
	FString SafetyClass;
	FString BaseRevision;
	TArray<FString> CanonicalOperations;

	virtual FString GetTypeId() const override { return TypeId; }
	virtual FString GetSchemaFingerprint() const override { return SchemaFingerprint; }
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIWorldValueSnapshot
{
	FString Scope;
	FString Revision;
	bool bComplete = true;
	int32 Scanned = 0;
	TArray<FHyperAIWorldRecord> Records;
	TArray<FHyperAIWorldIssue> Issues;
};

struct FHyperAINavigationValueSnapshot
{
	FString Scope;
	FString Revision;
	bool bComplete = true;
	int32 Scanned = 0;
	bool bPathAttempted = false;
	bool bPathValid = false;
	double PathLength = 0.0;
	TArray<FVector> PathPoints;
	TArray<FHyperAINavigationRecord> Records;
	TArray<FHyperAIWorldIssue> Issues;
};

struct FHyperAIWorldManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioWorldbuildingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("worldbuilding_navigation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioworldbuildingtoolset.v1");
	static constexpr int32 MaxPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxVariants = 32;
	static constexpr int32 MaxProjectionFields = 8;
	static constexpr int32 MaxOperations = 128;
	static constexpr int32 MaxPointsPerOperation = 4096;
	static constexpr int32 MaxTotalPoints = 8192;
	static constexpr int32 MaxTotalSourcePaths = 256;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxScannedObjects = 8192;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIWorldManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsWorldVariant(const FString& Variant);
	/** Inspection-only identity/transform digest; deliberately not accepted as mutation CAS. */
	static FString ComputeLoadedTargetRevision(const FString& ExactObjectPath);
	static bool ValidateOperationShape(const FHyperAIWorldPlanOperation& In, FHyperAIWorldBackendOperation& Out, FString& OutError);
	static bool Capture(const FHyperAIWorldInspectRequest& Request, FHyperAIWorldValueSnapshot& Out, FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIWorldValueSnapshot& Snapshot);
	static TArray<FHyperAIWorldIssue> ValidateSnapshot(const FHyperAIWorldValueSnapshot& Snapshot, int32 MaxIssueCount, bool& bOutTruncated);
	static FHyperAIWorldApplyPlanReport BuildPlan(const FHyperAIWorldApplyPlanRequest& Request);
	static const TCHAR* SafetyToString(EHyperAIWorldSafety Safety);
};

class FHyperAIStudioNavigationContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("worldbuilding_navigation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioworldbuildingtoolset.v1");
	static constexpr int32 MaxPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxVariants = 8;
	static constexpr int32 MaxProjectionFields = 8;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxRecords = 1024;
	static constexpr int32 MaxScannedObjects = 4096;
	static constexpr int32 MaxPathPoints = 2048;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIWorldManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsNavigationVariant(const FString& Variant);
	static bool ValidateOperationShape(const FHyperAINavigationPlanOperation& In, FHyperAINavigationBackendOperation& Out, FString& OutError);
	static bool Capture(const FHyperAINavigationInspectRequest& Request, FHyperAINavigationValueSnapshot& Out, FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAINavigationValueSnapshot& Snapshot);
	static TArray<FHyperAIWorldIssue> ValidateSnapshot(const FHyperAINavigationValueSnapshot& Snapshot, int32 MaxIssueCount, bool& bOutTruncated);
	static FHyperAINavigationApplyPlanReport BuildPlan(const FHyperAINavigationApplyPlanRequest& Request);
};

class FHyperAIStudioWorldbuildingRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsWorldbuildingRegistered() const;
	bool IsNavigationRegistered() const;

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsWorldbuildingRegistration = false;
	bool bOwnsNavigationRegistration = false;
};
