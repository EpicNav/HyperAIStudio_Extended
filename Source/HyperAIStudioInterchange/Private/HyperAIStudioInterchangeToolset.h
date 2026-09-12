// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioInterchangeToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIInterchangeIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> DelegatedPythonCallables;
	UPROPERTY() TArray<FString> CapabilityRequirementCoordinates;
	UPROPERTY() TArray<FString> AuthorityFingerprints;
	UPROPERTY() TArray<FString> ClosedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeSourceRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 SourceIndex = INDEX_NONE;
	UPROPERTY() FString PathClassification;
	/** Present only for a path proven contained by the canonical project directory. */
	UPROPERTY() FString ProjectRelativePath;
	UPROPERTY() FString PathFingerprint;
	UPROPERTY() FString Extension;
	UPROPERTY() int64 ImportedTimestampUtcTicks = 0;
	UPROPERTY() FString ImportedFileHash;
	UPROPERTY() FString DisplayLabel;
	UPROPERTY() FString Fingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString AssetKind;
	UPROPERTY() FString ImportDataClassPath;
	UPROPERTY() FString ImportDataObjectPath;
	UPROPERTY() FString NodeUniqueId;
	UPROPERTY() FString SceneImportAssetPath;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() FString VolatileObservationFingerprint;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bImportDataWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bAsyncCompilationActive = false;
	UPROPERTY() bool bPipelineObjectsMaterialized = false;
	UPROPERTY() FString PipelineProjectionStatus;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() int32 SourceFileCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical already-loaded top-level /Game static or skeletal mesh object path. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 PageSize = 16;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() bool bCursorEligible = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAIInterchangeAssetRecord Asset;
	UPROPERTY() TArray<FHyperAIInterchangeSourceRecord> Sources;
	UPROPERTY() TArray<FHyperAIInterchangeIssue> Issues;
	UPROPERTY() TArray<FHyperAIInterchangeCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	/** provenance or reimport_ready. */
	UPROPERTY() FString Policy = TEXT("provenance");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeValidateReport
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
	UPROPERTY() TArray<FHyperAIInterchangeIssue> Issues;
	UPROPERTY() TArray<FHyperAIInterchangeCapabilityStatus> Capabilities;
};

/** Closed project-contained reimport intent; no arbitrary import path or pipeline class is accepted. */
USTRUCT(BlueprintType)
struct FHyperAIInterchangeReimportPatch
{
	GENERATED_BODY()

	UPROPERTY() int32 SourceIndex = 0;
	UPROPERTY() FString ExpectedSourceFingerprint;
	/** Optional path relative to the canonical project directory. Empty preserves the recorded source. */
	UPROPERTY() FString ProjectRelativeSourcePath;
	/** Must remain true; pipeline object materialization/configuration is outside this source cohort. */
	UPROPERTY() bool bPreserveExistingPipelines = true;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FHyperAIInterchangeReimportPatch Patch;
	UPROPERTY() int32 DeadlineMs = 1500;
	UPROPERTY() int32 MaxGameThreadMs = 200;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangePlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 SourceCount = 0;
	UPROPERTY() bool bSourceProjectContained = false;
	UPROPERTY() bool bDestinationProjectContained = false;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bDetachedImmutableClone = false;
	UPROPERTY() bool bWouldImportOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIInterchangeApplyPlanReport
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
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FString ResolvedProjectRelativeSourcePath;
	UPROPERTY() FHyperAIInterchangePlanEffects Effects;
	UPROPERTY() TArray<FHyperAIInterchangeIssue> Issues;
	UPROPERTY() TArray<FHyperAIInterchangeCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Interchange atomic cohort. */
UCLASS()
class HYPERAISTUDIOINTERCHANGE_API UHyperAIStudioInterchangeToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Interchange")
	static FHyperAIInterchangeInspectReport hyper_interchange_inspect(
		const FHyperAIInterchangeInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Interchange")
	static FHyperAIInterchangeApplyPlanReport hyper_interchange_apply_plan(
		const FHyperAIInterchangeApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Interchange")
	static FHyperAIInterchangeValidateReport hyper_interchange_validate(
		const FHyperAIInterchangeValidateRequest& Request);
};

/** Detached, UObject-free input to the independent validator. */
struct FHyperAIStudioInterchangeValueSnapshot
{
	FHyperAIInterchangeAssetRecord Asset;
	TArray<FHyperAIInterchangeSourceRecord> Sources;
	TArray<FHyperAIInterchangeIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioInterchangeInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIInterchangeInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioInterchangeValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIInterchangeValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioInterchangePatchPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedRevision;
	FHyperAIInterchangeReimportPatch Patch;
	FString ResolvedProjectRelativeSourcePath;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioInterchangeInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIInterchangeInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioInterchangeValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIInterchangeValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioInterchangeDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioInterchangeDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioInterchangeManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

struct FHyperAIStudioInterchangeAuthorityRecord
{
	FString SourceCoordinate;
	FString Disposition;
	FString Route;
	FString Access;
	FString SourceHash;
};

class FHyperAIStudioInterchangeContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("geometry_interchange");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiointerchangetoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("geometry_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("geometry_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.interchange_manager");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_interchange_provenance_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("project_contained_reimport_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_import_provenance_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.interchange.inspect.v1");
	static constexpr const TCHAR* PatchPayloadTypeId = TEXT("hyperai.payload.interchange.reimport.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.interchange.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.interchange.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.interchange.mutation-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.interchange.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_import_continuation_required");
	static constexpr const TCHAR* NativeAccessReviewId =
		TEXT("epic-ue5.8-native-aicallable-access-2026-08-15");
	static constexpr const TCHAR* NativeAccessReviewRecordsSha256 =
		TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr const TCHAR* CapabilityRequirementsSha256 =
		TEXT("sha256:0f93e07865697a34efd4c7ace1264fc2721f2d9435e71c025ace0fc5325407e5");
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxObjectPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxSourceFiles = 32;
	static constexpr int32 MaxPageSize = 32;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxPrepareDeadlineMs = 2000;
	static constexpr int32 MinPrepareDeadlineMs = 100;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioInterchangeManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FString>& GetEpicPythonDelegates();
	static const TArray<FHyperAIStudioInterchangeAuthorityRecord>& GetCapabilityRequirements();
	static TArray<FHyperAIInterchangeCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static bool ClassifySourcePath(const FString& RawPath, FString& OutClassification,
		FString& OutProjectRelativePath, FString& OutExtension,
		FString& OutPathFingerprint);
	static bool ResolveProjectRelativeSourcePath(const FString& RelativePath,
		FString& OutNormalizedRelativePath, FString& OutExtension);
	static FString BuildCursor(const FString& TargetPath, const FString& Revision,
		int32 PageSize, int32 Offset);
	static bool ParseCursor(const FString& Cursor, const FString& TargetPath,
		const FString& Revision, int32 PageSize, int32& OutOffset);
	static FString InspectPayloadSchemaFingerprint();
	static FString PatchPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool CaptureExact(const FString& TargetPath, int32 MaxWorkMs,
		FHyperAIStudioInterchangeValueSnapshot& OutSnapshot, FString& OutStatus,
		FString& OutDiagnostic);
	static bool ValidateDetached(const FHyperAIStudioInterchangeValueSnapshot& Snapshot,
		const FString& Policy, int32 MaxIssues, int32 OutputByteLimit,
		FHyperAIInterchangeValidateReport& OutReport);
	static FHyperAIInterchangeInspectReport Inspect(
		const FHyperAIInterchangeInspectRequest& Request);
	static FHyperAIInterchangeValidateReport Validate(
		const FHyperAIInterchangeValidateRequest& Request);
	static FHyperAIInterchangeApplyPlanReport BuildPlan(
		const FHyperAIInterchangeApplyPlanRequest& Request);
	static FString ComputePatchSemanticFingerprint(
		const FHyperAIStudioInterchangePatchPayload& Payload);
};

class FHyperAIStudioInterchangeRegistration final
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
	TSharedPtr<FHyperAIStudioInterchangeDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
