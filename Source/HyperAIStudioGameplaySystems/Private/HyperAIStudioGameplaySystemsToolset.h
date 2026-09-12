// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioGameplaySystemsToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsTarget
{
	GENERATED_BODY()

	/** game_feature_data, mass_entity_config, or world_condition_schema. */
	UPROPERTY()
	FString Role;

	/** Exact primary object path, or an exact /Script class path for the schema role. */
	UPROPERTY()
	FString ObjectPath;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Role;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString Message;
};

/** Detached record; persisted and volatile observations are sealed separately. */
USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Role;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString ExpectedClassPath;

	UPROPERTY()
	FString ActualClassPath;

	UPROPERTY()
	FString PluginName;

	/** exists, does_not_exist, unknown, or not_applicable. */
	UPROPERTY()
	FString PackageState;

	/** Hash of bounded action, trait, or schema-context identities. */
	UPROPERTY()
	FString ContentFingerprint;

	/** Persisted/authored identity only. */
	UPROPERTY()
	FString ElementFingerprint;

	UPROPERTY()
	int32 EntryCount = 0;

	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bWasLoaded = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bClassCompatible = false;

	UPROPERTY()
	bool bPluginFound = false;

	UPROPERTY()
	bool bPluginEnabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsDetachedSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	FString FeaturePluginName;

	UPROPERTY()
	FString FeatureMountPoint;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	bool bFeaturePluginFound = false;

	UPROPERTY()
	bool bFeaturePluginEnabled = false;

	UPROPERTY()
	bool bFeaturePluginProjectOwned = false;

	UPROPERTY()
	bool bFeaturePluginContainsContent = false;

	UPROPERTY()
	bool bFeaturePluginExplicitlyLoaded = false;

	UPROPERTY()
	bool bSnapshotComplete = false;

	/** Exactly three records in canonical role order. */
	UPROPERTY()
	TArray<FHyperAIGameplaySystemsRecord> Records;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> DelegatedEpicCallables;

	UPROPERTY()
	TArray<FString> ClosedCases;

	UPROPERTY()
	TArray<FString> UnsupportedCases;

	UPROPERTY()
	FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsInspectRequest
{
	GENERATED_BODY()

	/** Exact project-owned Game Feature plugin name. */
	UPROPERTY()
	FString FeaturePluginName;

	/** Exactly one target for each of the three closed roles. */
	UPROPERTY()
	TArray<FHyperAIGameplaySystemsTarget> Targets;

	UPROPERTY()
	int32 DeadlineMs = 500;

	UPROPERTY()
	int32 MaxGameThreadMs = 100;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only_exact_preflight");

	UPROPERTY()
	FHyperAIGameplaySystemsDetachedSnapshot Snapshot;

	UPROPERTY()
	TArray<FHyperAIGameplaySystemsIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGameplaySystemsCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FHyperAIGameplaySystemsDetachedSnapshot Snapshot;

	UPROPERTY()
	int32 MaxIssues = 64;

	UPROPERTY()
	int32 DeadlineMs = 500;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsValidateReport
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
	FString RecomputedRequestFingerprint;

	UPROPERTY()
	FString RecomputedPersistedFingerprint;

	UPROPERTY()
	FString RecomputedVolatileObservationFingerprint;

	UPROPERTY()
	TArray<FHyperAIGameplaySystemsIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	/** Exact plan hash returned by a fresh dry-run. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString FeaturePluginName;

	UPROPERTY()
	TArray<FHyperAIGameplaySystemsTarget> Targets;

	/** Fresh complete preflight CAS. */
	UPROPERTY()
	FString ExpectedPersistedFingerprint;

	/** The only admitted operation is preflight.mass_game_feature_world_condition. */
	UPROPERTY()
	FString Operation = TEXT("preflight.mass_game_feature_world_condition");

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	int32 MaxGameThreadMs = 150;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 TargetCount = 0;

	UPROPERTY()
	bool bWouldPersistJournalReceiptOnce = false;

	UPROPERTY()
	bool bWouldValidateOnce = false;

	UPROPERTY()
	bool bWouldVerifyFreshOnce = false;

	UPROPERTY()
	bool bProjectAssetMutationAttempted = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplaySystemsApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bTypedPrepared = false;

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
	FString BasePersistedFingerprint;

	UPROPERTY()
	FString SemanticFingerprint;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FHyperAIGameplaySystemsPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIGameplaySystemsIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOGAMEPLAYSYSTEMS_API UHyperAIStudioGameplaySystemsToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Gameplay Systems")
	static FHyperAIGameplaySystemsInspectReport hyper_gameplay_systems_inspect(
		const FHyperAIGameplaySystemsInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Gameplay Systems")
	static FHyperAIGameplaySystemsApplyPlanReport hyper_gameplay_systems_apply_plan(
		const FHyperAIGameplaySystemsApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Gameplay Systems")
	static FHyperAIGameplaySystemsValidateReport hyper_gameplay_systems_validate(
		const FHyperAIGameplaySystemsValidateRequest& Request);
};

class FHyperAIStudioGameplaySystemsPlanPayload final
	: public IHyperAIStudioTypedArtifactPayload
{
public:
	FString FeaturePluginName;
	FString FeatureMountPoint;
	FString BasePersistedFingerprint;
	FString SemanticFingerprint;
	TArray<FString> CanonicalTargets;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

struct FHyperAIStudioGameplaySystemsManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioGameplaySystemsContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("gameplay_systems");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiogameplaysystemstoolset.v1");
	static constexpr const TCHAR* InspectVariantId =
		TEXT("gameplay_systems.loaded_preflight.inspect.v1");
	static constexpr const TCHAR* ApplyVariantId =
		TEXT("gameplay_systems.loaded_preflight.prepare.v1");
	static constexpr const TCHAR* ValidateVariantId =
		TEXT("gameplay_systems.detached_preflight.validate.v1");
	static constexpr const TCHAR* InspectPayloadTypeId =
		TEXT("hyperai.payload.gameplay-systems.inspect.v1");
	static constexpr const TCHAR* ApplyPayloadTypeId =
		TEXT("hyperai.payload.gameplay-systems.preflight-plan.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId =
		TEXT("hyperai.payload.gameplay-systems.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId =
		TEXT("hyperai.result.gameplay-systems.inspect.v1");
	static constexpr const TCHAR* ApplyResultTypeId =
		TEXT("hyperai.result.gameplay-systems.preflight-plan.v1");
	static constexpr const TCHAR* ValidateResultTypeId =
		TEXT("hyperai.result.gameplay-systems.validate.v1");
	static constexpr const TCHAR* GameFeatureDataClassPath =
		TEXT("/Script/GameFeatures.GameFeatureData");
	static constexpr const TCHAR* MassEntityConfigClassPath =
		TEXT("/Script/MassSpawner.MassEntityConfigAsset");
	static constexpr const TCHAR* WorldConditionSchemaClassPath =
		TEXT("/Script/WorldConditions.WorldConditionSchema");
	static constexpr const TCHAR* ClosedOperation =
		TEXT("preflight.mass_game_feature_world_condition");
	static constexpr const TCHAR* NonDryCallableState = TEXT("staged_backend_required");
	static constexpr int32 MaxTargets = 3;
	static constexpr int32 MaxEntriesPerTarget = 64;
	static constexpr int32 MaxFeaturePluginNameChars = 64;
	static constexpr int32 MaxPathChars = 512;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxGameThreadMs = 250;
	static constexpr int32 MaxCanonicalChars = 128 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioGameplaySystemsManifestEntry>& GetManifest();
	static const TArray<FString>& GetEpicDelegates();
	static TArray<FHyperAIGameplaySystemsCapabilityStatus> GetCapabilityMatrix();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsSafeFeaturePluginName(const FString& Value);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsCanonicalTargetPath(
		const FString& Role,
		const FString& Path,
		const FString& FeaturePluginName);
	static FString ClassifyPackageExistence(int32 StateValue);
	static FString ComputeRequestFingerprint(
		const FString& FeaturePluginName,
		const TArray<FHyperAIGameplaySystemsRecord>& Records);
	static FString ComputeElementFingerprint(const FHyperAIGameplaySystemsRecord& Record);
	static FString ComputePersistedFingerprint(
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot);
	static FString ComputeVolatileFingerprint(
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot);
	static FHyperAIGameplaySystemsValidateReport ValidateDetached(
		const FHyperAIGameplaySystemsValidateRequest& Request);
	static FString ApplyPayloadSchemaFingerprint();
	static FString ComputePayloadSemanticFingerprint(
		const FHyperAIStudioGameplaySystemsPlanPayload& Payload);
	static const FHyperAIStudioDomainAdapterDescriptor& GetPreparationDescriptor();
	static FHyperAIGameplaySystemsInspectReport Inspect(
		const FHyperAIGameplaySystemsInspectRequest& Request);
	static FHyperAIGameplaySystemsApplyPlanReport BuildPlan(
		const FHyperAIGameplaySystemsApplyPlanRequest& Request);
	/** Pure seam for detached automation; never reads UObject, plugin, or registry state. */
	static FHyperAIGameplaySystemsApplyPlanReport PrepareFromSnapshotForTest(
		const FHyperAIGameplaySystemsApplyPlanRequest& Request,
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot);
};

class FHyperAIStudioGameplaySystemsRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const { return bOwnsToolset; }

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
