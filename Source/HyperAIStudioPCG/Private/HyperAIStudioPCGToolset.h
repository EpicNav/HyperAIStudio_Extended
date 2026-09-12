// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPCGToolset.generated.h"

class UPCGComponent;
class UPCGGraph;

USTRUCT(BlueprintType)
struct FHyperAIPCGIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("pcg");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() FString State = TEXT("source_candidate_async_continuation_required");
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> CapabilityRequirementCoordinates;
	UPROPERTY() TArray<FString> UniqueCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGPinRecord
{
	GENERATED_BODY()

	UPROPERTY() FString PinId;
	UPROPERTY() FString Direction;
	UPROPERTY() FString Label;
	UPROPERTY() int32 Usage = 0;
	UPROPERTY() int32 Status = 0;
	UPROPERTY() bool bAllowsMultipleConnections = false;
	UPROPERTY() bool bAllowsMultipleData = false;
	UPROPERTY() bool bInvisible = false;
	UPROPERTY() int32 EdgeCount = 0;
	UPROPERTY() FString AllowedTypeFingerprint;
	UPROPERTY() FString CurrentTypeFingerprint;
	/** Exact bounded projection of the persisted UE pin-property struct. */
	UPROPERTY() FString PropertiesFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGNodeRecord
{
	GENERATED_BODY()

	UPROPERTY() FString NodeId;
	UPROPERTY() FString Role;
	UPROPERTY() FString AuthoredTitle;
	UPROPERTY() FString Comment;
	UPROPERTY() int32 PositionX = 0;
	UPROPERTY() int32 PositionY = 0;
	UPROPERTY() bool bHidden = false;
	UPROPERTY() bool bSettingsProjectionComplete = false;
	UPROPERTY() FString SettingsInterfaceClass;
	UPROPERTY() FString SettingsClass;
	UPROPERTY() FString SettingsFingerprint;
	/** Exact bounded projection of persisted node fields outside settings and pins. */
	UPROPERTY() FString NodePropertiesFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() TArray<FHyperAIPCGPinRecord> InputPins;
	UPROPERTY() TArray<FHyperAIPCGPinRecord> OutputPins;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGEdgeRecord
{
	GENERATED_BODY()

	UPROPERTY() FString EdgeId;
	UPROPERTY() FString FromNodeId;
	UPROPERTY() FString FromPinId;
	UPROPERTY() FString ToNodeId;
	UPROPERTY() FString ToPinId;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGGraphRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString GraphPropertiesFingerprint;
	UPROPERTY() FString UserParametersFingerprint;
	UPROPERTY() int32 NodeCount = 0;
	UPROPERTY() int32 EdgeCount = 0;
	UPROPERTY() int32 UserParameterCount = 0;
	UPROPERTY() int32 EmbeddedSubgraphCount = 0;
	UPROPERTY() int32 CommentCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGComponentRecord
{
	GENERATED_BODY()

	UPROPERTY() bool bPresent = false;
	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ComponentClassPath;
	UPROPERTY() FString OwnerPath;
	UPROPERTY() FString GraphPath;
	UPROPERTY() FString GraphInstancePath;
	/** Direct graph-interface reference; may differ from the resolved graph for graph-instance chains. */
	UPROPERTY() FString GraphInterfacePath;
	UPROPERTY() FString PackageName;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString ComponentPropertiesFingerprint;
	UPROPERTY() FString GraphInstancePropertiesFingerprint;
	UPROPERTY() FString InstanceParametersFingerprint;
	UPROPERTY() int32 InstanceParameterCount = 0;
	UPROPERTY() FString InstanceOverrideMaskFingerprint;
	UPROPERTY() int32 InstanceOverrideCount = 0;
	UPROPERTY() FString SchedulingPolicyFingerprint;
	UPROPERTY() FString OwnerTransformFingerprint;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() bool bPersistedProjectionComplete = false;
	/** Separate observation fingerprint. It is never part of PersistedRevision. */
	UPROPERTY() FString VolatileObservationFingerprint;
	UPROPERTY() bool bVolatileStateStable = true;
	UPROPERTY() bool bActivated = false;
	UPROPERTY() bool bGenerated = false;
	UPROPERTY() bool bDirtyGenerated = false;
	UPROPERTY() bool bGenerating = false;
	UPROPERTY() bool bCleaningUp = false;
	UPROPERTY() bool bRefreshInProgress = false;
	UPROPERTY() FString GenerationTaskId;
	UPROPERTY() FString CleanupTaskId;
	UPROPERTY() int32 Seed = 0;
	UPROPERTY() int32 GenerationTrigger = 0;
	UPROPERTY() bool bPartitioned = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical loaded top-level /Game PCG graph object path. */
	UPROPERTY() FString GraphPath;
	/** Optional exact loaded PCG component object path; never a search expression. */
	UPROPERTY() FString ComponentPath;
	UPROPERTY() int32 PageSize = 24;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() bool bCursorEligible = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAIPCGGraphRecord Graph;
	UPROPERTY() FHyperAIPCGComponentRecord Component;
	UPROPERTY() TArray<FHyperAIPCGNodeRecord> Nodes;
	UPROPERTY() TArray<FHyperAIPCGEdgeRecord> Edges;
	UPROPERTY() TArray<FHyperAIPCGIssue> Issues;
	UPROPERTY() TArray<FHyperAIPCGCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString GraphPath;
	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FString ExpectedComponentFingerprint;
	/** structural or generation_ready. */
	UPROPERTY() FString Policy = TEXT("structural");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Policy;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIPCGIssue> Issues;
	UPROPERTY() TArray<FHyperAIPCGCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGLifecycleIntent
{
	GENERATED_BODY()

	/** The only v1 compound intent; all graph authoring remains delegated to Epic. */
	UPROPERTY() FString Lifecycle = TEXT("generate_validate_cleanup");
	UPROPERTY() FString ValidationPolicy = TEXT("generation_ready");
	UPROPERTY() FString ExpectedTerminalState = TEXT("clean");
	UPROPERTY() bool bForceGenerate = false;
	UPROPERTY() bool bRemoveGeneratedComponentsOnCleanup = true;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString GraphPath;
	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FString ExpectedComponentFingerprint;
	UPROPERTY() FHyperAIPCGLifecycleIntent Intent;
	UPROPERTY() int32 DeadlineMs = 60000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 BeginCallCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bWouldGenerateOnce = false;
	UPROPERTY() bool bWouldPollWithoutBlocking = false;
	UPROPERTY() bool bWouldValidateFreshOnce = false;
	UPROPERTY() bool bWouldCleanupOnce = false;
	UPROPERTY() bool bWouldObserveTerminalState = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BasePersistedRevision;
	UPROPERTY() FString BaseComponentFingerprint;
	UPROPERTY() FString BaseVolatileObservationFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAIPCGPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIPCGIssue> Issues;
	UPROPERTY() TArray<FHyperAIPCGCapabilityStatus> Capabilities;
};

/** Exactly three functions form the PCG atomic cohort. */
UCLASS()
class HYPERAISTUDIOPCG_API UHyperAIStudioPCGToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGInspectReport hyper_pcg_inspect(const FHyperAIPCGInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGApplyPlanReport hyper_pcg_apply_plan(const FHyperAIPCGApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGValidateReport hyper_pcg_validate(const FHyperAIPCGValidateRequest& Request);
};

struct FHyperAIStudioPCGEdgeState
{
	FString EdgeId;
	FString FromNodeId;
	FString FromPinId;
	FString ToNodeId;
	FString ToPinId;
};

/** Detached value-only input to the independent validator. */
struct FHyperAIStudioPCGValueSnapshot
{
	FHyperAIPCGGraphRecord Graph;
	FHyperAIPCGComponentRecord Component;
	TArray<FHyperAIPCGNodeRecord> Nodes;
	TArray<FHyperAIStudioPCGEdgeState> Edges;
	TArray<FHyperAIPCGIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioPCGInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPCGInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPCGValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPCGValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPCGLifecyclePayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString GraphPath;
	FString ComponentPath;
	FString BasePersistedRevision;
	FString BaseComponentFingerprint;
	FString BaseVolatileObservationFingerprint;
	FString Lifecycle;
	FString ValidationPolicy;
	FString ExpectedTerminalState;
	int32 AsyncDeadlineMs = 0;
	bool bForceGenerate = false;
	bool bRemoveGeneratedComponentsOnCleanup = true;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioPCGInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPCGInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPCGValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPCGValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPCGDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioPCGDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioPCGManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioPCGContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("pcg");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiopcgtoolset.v1");
	static constexpr const TCHAR* RequirementGroupId = TEXT("pcg");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.pcg");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_graph_component_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("generate_validate_cleanup_intent.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.pcg.inspect.v1");
	static constexpr const TCHAR* LifecyclePayloadTypeId = TEXT("hyperai.payload.pcg.generate_validate_cleanup.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.pcg.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.pcg.inspect.v1");
	static constexpr const TCHAR* LifecycleResultTypeId = TEXT("hyperai.result.pcg.lifecycle.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.pcg.validate.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("async_continuation_host_required");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxStringCharacters = 2048;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxPageSize = 48;
	static constexpr int32 MaxNodes = 1024;
	static constexpr int32 MaxPinsPerNode = 128;
	static constexpr int32 MaxEdges = 8192;
	static constexpr int32 MaxSnapshotItems = MaxNodes + MaxEdges;
	static constexpr int32 MaxEmbeddedSubgraphs = 64;
	static constexpr int32 MaxComments = 512;
	static constexpr int32 MaxProperties = 4096;
	static constexpr int32 MaxContainerElements = 512;
	static constexpr int32 MaxProjectionDepth = 12;
	static constexpr int32 MaxProjectionWorkUnits = 32768;
	static constexpr int32 MaxProjectionBytes = 256 * 1024;
	static constexpr int32 MaxValueProjectionBytes = 8 * 1024;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxAsyncDeadlineMs = 120000;
	static constexpr int32 MinAsyncDeadlineMs = 5000;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioPCGManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FHyperAIPCGCapabilityStatus> GetCapabilityMatrix();
	static const TArray<FString>& GetEpicDelegates();
	static const TArray<FString>& GetCapabilityRequirementCoordinates();
	static bool IsCanonicalGraphPath(const FString& Path);
	static bool IsCanonicalComponentPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static bool AdmitContainerBeforeProjection(
		int32 Count, int32 CurrentWork, int32 CurrentBytes,
		int32 MinimumBytesPerElement, FString& OutError);
	static bool AdmitSparseContainerBeforeProjection(
		int32 Count, int32 SlotCount, int32 CurrentWork, int32 CurrentBytes,
		int32 MinimumBytesPerElement, FString& OutError);
	static bool IsPagingStable(const FHyperAIPCGComponentRecord& Component);
	static FString BuildCursor(
		const FString& GraphPath, const FString& ComponentPath,
		const FString& PersistedRevision, const FString& ComponentFingerprint,
		int32 PageSize, int32 Offset);
	static bool ParseCursor(
		const FString& Cursor, const FString& GraphPath, const FString& ComponentPath,
		const FString& PersistedRevision, const FString& ComponentFingerprint,
		int32 PageSize, int32& OutOffset);
	static FString InspectPayloadSchemaFingerprint();
	static FString LifecyclePayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString LifecycleResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static const UPCGGraph* ResolveAlreadyLoadedGraph(const FString& GraphPath);
	static const UPCGComponent* ResolveAlreadyLoadedComponent(const FString& ComponentPath);
	static FString ComputeNodeFingerprint(const FHyperAIPCGNodeRecord& Node);
	static FString ComputeEdgeFingerprint(const FHyperAIStudioPCGEdgeState& Edge);
	static FString ComputePersistedRevision(const FHyperAIStudioPCGValueSnapshot& Snapshot);
	static FString ComputeComponentPersistedFingerprint(
		const FHyperAIPCGComponentRecord& Component);
	static FString ComputeComponentVolatileObservationFingerprint(
		const FHyperAIPCGComponentRecord& Component);
	static bool ValidateValueSnapshot(
		const FHyperAIStudioPCGValueSnapshot& Snapshot,
		const FString& Policy,
		int32 MaxIssues,
		TArray<FHyperAIPCGIssue>& OutIssues,
		FString& OutValidatorFingerprint);
	static bool CaptureExact(
		const FString& GraphPath,
		const FString& ComponentPath,
		int32 MaxWorkMs,
		FHyperAIStudioPCGValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FHyperAIPCGInspectReport Inspect(const FHyperAIPCGInspectRequest& Request);
	static FHyperAIPCGValidateReport Validate(const FHyperAIPCGValidateRequest& Request);
	static FHyperAIPCGApplyPlanReport BuildPlan(const FHyperAIPCGApplyPlanRequest& Request);
	static FString ComputeLifecycleSemanticFingerprint(const FHyperAIStudioPCGLifecyclePayload& Payload);
};

class FHyperAIStudioPCGRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioPCGDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
