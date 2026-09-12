// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLiveProductionToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAILiveProductionIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Detail;
};

/** Frozen review/input row. This is authority evidence, never a dispatch table. */
USTRUCT(BlueprintType)
struct FHyperAILiveProductionAuthorityRow
{
	GENERATED_BODY()

	UPROPERTY() FString AuthorityKind;
	UPROPERTY() FString Source;
	UPROPERTY() FString SourceId;
	UPROPERTY() FString Lifecycle;
	UPROPERTY() FString Access;
	UPROPERTY() FString Disposition;
	UPROPERTY() FString HyperAIContract;
	UPROPERTY() FString RequiredPlugins;
	UPROPERTY() int32 ReviewedCallableCount = 0;
	UPROPERTY() int32 MatchingCallableCount = 0;
	UPROPERTY() FString ReviewedFingerprint;
	UPROPERTY() FString SourceCoordinate;
	UPROPERTY() FString FrozenCoverage;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("live_production");
	UPROPERTY() FString State = TEXT("source_candidate_specialized_adapter_required");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bOptionalFamilyHardLinked = false;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() bool bExternalEffectExecutionImplemented = false;
	UPROPERTY() int32 ReviewedEpicNativeCallableCount = 278;
	UPROPERTY() int32 ReviewedEpicPythonCallableCount = 598;
	UPROPERTY() int32 MatchingEpicCallableCount = 0;
	UPROPERTY() int32 NonEpicRowCount = 20;
	UPROPERTY() int32 CapabilityGatedRowCount = 19;
	UPROPERTY() int32 DroppedRowCount = 1;
	UPROPERTY() FString AuthorityFingerprint;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

/** Exact already-loaded top-level UObject/package identity; no family internals are reflected. */
USTRUCT(BlueprintType)
struct FHyperAILiveProductionObjectRecord
{
	GENERATED_BODY()

	UPROPERTY() FString FamilyId;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bFamilyClassMatched = false;
	UPROPERTY() bool bPersistedIdentityComplete = false;
	/** False in this base adapter: object identity is not complete family semantics. */
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionDetachedSnapshot
{
	GENERATED_BODY()

	UPROPERTY() FString FamilyId;
	UPROPERTY() FString RequestFingerprint;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
	UPROPERTY() bool bIdentityProjectionComplete = false;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() int32 TotalRecords = 0;
	UPROPERTY() TArray<FHyperAILiveProductionObjectRecord> Records;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionInspectRequest
{
	GENERATED_BODY()

	/** livelink, media, remote_control, dmx, avalanche, scene_state, ndisplay, text3d, or svg. */
	UPROPERTY() FString FamilyId;
	/** Exact canonical /Game primary-object paths. Empty returns only the frozen capability status. */
	UPROPERTY() TArray<FString> TargetPaths;
	UPROPERTY() int32 PageSize = 16;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 131072;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("loaded_exact_identity_only");
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAILiveProductionCapabilityStatus Capability;
	UPROPERTY() FHyperAILiveProductionDetachedSnapshot Snapshot;
	UPROPERTY() TArray<FHyperAILiveProductionIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionNDisplayNode
{
	GENERATED_BODY()

	UPROPERTY() FString NodeId;
	UPROPERTY() FString HostAddress;
	UPROPERTY() bool bPrimary = false;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionNDisplayViewport
{
	GENERATED_BODY()

	UPROPERTY() FString NodeId;
	UPROPERTY() FString ViewportId;
	UPROPERTY() int32 X = 0;
	UPROPERTY() int32 Y = 0;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
};

/** Closed value model. It never claims to have been projected from a live nDisplay UObject. */
USTRUCT(BlueprintType)
struct FHyperAILiveProductionNDisplayTopology
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAILiveProductionNDisplayNode> Nodes;
	UPROPERTY() TArray<FHyperAILiveProductionNDisplayViewport> Viewports;
	UPROPERTY() FString Fingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	/** Empty for dry-run; a non-dry request must echo a canonical prior plan hash. */
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedFingerprint;
	UPROPERTY() FHyperAILiveProductionNDisplayTopology BaseTopology;
	UPROPERTY() FHyperAILiveProductionNDisplayTopology DesiredTopology;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxOutputBytes = 131072;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 NodeCount = 0;
	UPROPERTY() int32 ViewportCount = 0;
	UPROPERTY() bool bIntentValueModelValid = false;
	UPROPERTY() bool bLoadedSemanticProjectionComplete = false;
	UPROPERTY() bool bWouldRequireExternalConfirmation = true;
	UPROPERTY() bool bWouldRequireOneBeginCall = true;
	UPROPERTY() bool bWouldRequireAsyncTerminalObservation = true;
	UPROPERTY() bool bWouldRequireFreshVerification = true;
	UPROPERTY() int32 PhysicalEffectCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bEffectStarted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString SafetyClass = TEXT("external_effect");
	UPROPERTY() FString VariantId;
	UPROPERTY() FString BasePersistedFingerprint;
	UPROPERTY() FString BaseTopologyFingerprint;
	UPROPERTY() FString DesiredTopologyFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAILiveProductionPlanEffects Effects;
	UPROPERTY() TArray<FHyperAILiveProductionIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionValidateRequest
{
	GENERATED_BODY()

	/** identity_snapshot or ndisplay_topology. */
	UPROPERTY() FString Policy = TEXT("identity_snapshot");
	UPROPERTY() FHyperAILiveProductionDetachedSnapshot Snapshot;
	UPROPERTY() FHyperAILiveProductionNDisplayTopology Topology;
	UPROPERTY() int32 MaxIssues = 64;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILiveProductionValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() FString RecomputedPersistedFingerprint;
	UPROPERTY() FString RecomputedVolatileFingerprint;
	UPROPERTY() FString RecomputedTopologyFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAILiveProductionIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOLIVEPRODUCTION_API UHyperAIStudioLiveProductionToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Live Production")
	static FHyperAILiveProductionInspectReport hyper_live_production_inspect(
		const FHyperAILiveProductionInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Live Production")
	static FHyperAILiveProductionApplyPlanReport hyper_live_production_apply_plan(
		const FHyperAILiveProductionApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Live Production")
	static FHyperAILiveProductionValidateReport hyper_live_production_validate(
		const FHyperAILiveProductionValidateRequest& Request);
};

class FHyperAIStudioLiveProductionPlanPayload final
	: public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedFingerprint;
	FString BaseTopologyFingerprint;
	FString DesiredTopologyFingerprint;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

struct FHyperAIStudioLiveProductionManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioLiveProductionContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("live_production");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioliveproductiontoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("live_production_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("live_production_backend");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_identity_snapshot.v1");
	static constexpr const TCHAR* ApplyVariantId = TEXT("ndisplay_cluster_intent_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("detached_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.live-production.inspect.v1");
	static constexpr const TCHAR* ApplyPayloadTypeId = TEXT("hyperai.payload.live-production.ndisplay-intent.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.live-production.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.live-production.inspect.v1");
	static constexpr const TCHAR* ApplyResultTypeId = TEXT("hyperai.result.live-production.apply-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.live-production.validate.v1");
	static constexpr const TCHAR* SemanticProjectionBlocker =
		TEXT("semantic_projection_backend_required");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("external_effect_continuation_backend_required");
	static constexpr const TCHAR* NativeAccessReviewId =
		TEXT("epic-ue5.8-native-aicallable-access-2026-08-15");
	static constexpr const TCHAR* NativeAccessReviewRecordsSha256 =
		TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 96;
	static constexpr int32 MaxHostCharacters = 255;
	static constexpr int32 MaxTargetPaths = 32;
	static constexpr int32 MaxNodes = 32;
	static constexpr int32 MaxViewports = 128;
	static constexpr int32 MaxPageSize = 32;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxContainerAllocatedBytes = 512 * 1024;
	static constexpr int32 MaxCanonicalCharacters = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioLiveProductionManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FHyperAILiveProductionAuthorityRow>& GetEpicReviewAuthority();
	static const TArray<FHyperAILiveProductionAuthorityRow>& GetPluginDescriptorAuthority();
	static const TArray<FHyperAILiveProductionAuthorityRow>& GetCapabilityRequirements();
	static FHyperAILiveProductionCapabilityStatus GetCapabilityStatus();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static bool IsKnownFamilyId(const FString& Value);
	static bool DoesClassMatchFamily(const FString& ClassPath, const FString& FamilyId);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString ComputeObjectPersistedFingerprint(
		const FHyperAILiveProductionObjectRecord& Record);
	static FString ComputeObjectVolatileFingerprint(
		const FHyperAILiveProductionObjectRecord& Record);
	static FString ComputeSnapshotPersistedFingerprint(
		const TArray<FHyperAILiveProductionObjectRecord>& Records);
	static FString ComputeSnapshotVolatileFingerprint(
		const TArray<FHyperAILiveProductionObjectRecord>& Records);
	static bool ValidateTopologyValueModel(
		const FHyperAILiveProductionNDisplayTopology& Topology,
		FString& OutFingerprint, TArray<FHyperAILiveProductionIssue>& OutIssues);
	static FString BuildCursor(int32 Offset, const FString& RequestFingerprint,
		const FString& PersistedFingerprint, const FString& VolatileFingerprint);
	static bool ParseCursor(const FString& Cursor, const FString& RequestFingerprint,
		const FString& PersistedFingerprint, const FString& VolatileFingerprint,
		int32& OutOffset);
	static FString InspectPayloadSchemaFingerprint();
	static FString ApplyPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ApplyResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAILiveProductionInspectReport Inspect(
		const FHyperAILiveProductionInspectRequest& Request);
	static FHyperAILiveProductionApplyPlanReport BuildPlan(
		const FHyperAILiveProductionApplyPlanRequest& Request);
	static FHyperAILiveProductionValidateReport Validate(
		const FHyperAILiveProductionValidateRequest& Request);
	static FString ComputePlanSemanticFingerprint(
		const FHyperAIStudioLiveProductionPlanPayload& Payload);
};

class FHyperAIStudioLiveProductionRegistration final
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
