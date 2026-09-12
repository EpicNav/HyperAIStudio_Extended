// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioGeometryToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIGeometryIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> DelegatedNativeCallables;
	UPROPERTY() TArray<FString> DelegatedPythonCallables;
	UPROPERTY() TArray<FString> CapabilityRequirementCoordinates;
	UPROPERTY() TArray<FString> AuthorityFingerprints;
	UPROPERTY() TArray<FString> ClosedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryMaterialRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 SlotIndex = INDEX_NONE;
	UPROPERTY() FString SlotName;
	UPROPERTY() FString ImportedSlotName;
	UPROPERTY() FString MaterialPath;
	UPROPERTY() FString Fingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryLodRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 LodIndex = INDEX_NONE;
	UPROPERTY() bool bBuildSettingsProjected = false;
	UPROPERTY() bool bRecomputeNormals = false;
	UPROPERTY() bool bRecomputeTangents = false;
	UPROPERTY() bool bUseMikkTSpace = false;
	UPROPERTY() bool bGenerateLightmapUVs = false;
	UPROPERTY() bool bBuildReversedIndexBuffer = false;
	UPROPERTY() bool bUseHighPrecisionTangentBasis = false;
	UPROPERTY() bool bUseFullPrecisionUVs = false;
	UPROPERTY() int32 MinLightmapResolution = 0;
	UPROPERTY() int32 SrcLightmapIndex = INDEX_NONE;
	UPROPERTY() int32 DstLightmapIndex = INDEX_NONE;
	UPROPERTY() FVector BuildScale3D = FVector::OneVector;
	UPROPERTY() double DistanceFieldResolutionScale = 0.0;
	UPROPERTY() FString Fingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString AssetKind;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() FString VolatileObservationFingerprint;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bAsyncCompilationActive = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() int32 SourceModelCount = 0;
	UPROPERTY() int32 MaterialSlotCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical already-loaded top-level /Game static or skeletal mesh object path. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 PageSize = 32;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() bool bCursorEligible = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAIGeometryAssetRecord Asset;
	UPROPERTY() TArray<FHyperAIGeometryLodRecord> Lods;
	UPROPERTY() TArray<FHyperAIGeometryMaterialRecord> Materials;
	UPROPERTY() TArray<FHyperAIGeometryIssue> Issues;
	UPROPERTY() TArray<FHyperAIGeometryCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	/** structural or static_mesh_build_ready. */
	UPROPERTY() FString Policy = TEXT("structural");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryValidateReport
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
	UPROPERTY() TArray<FHyperAIGeometryIssue> Issues;
	UPROPERTY() TArray<FHyperAIGeometryCapabilityStatus> Capabilities;
};

/** Closed missing-gap static-mesh build-policy patch. Epic-equivalent material/Nanite/LOD CRUD is excluded. */
USTRUCT(BlueprintType)
struct FHyperAIGeometryBuildPolicyPatch
{
	GENERATED_BODY()

	UPROPERTY() int32 LodIndex = INDEX_NONE;
	UPROPERTY() FString ExpectedLodFingerprint;
	UPROPERTY() bool bSetRecomputeNormals = false;
	UPROPERTY() bool bRecomputeNormals = false;
	UPROPERTY() bool bSetRecomputeTangents = false;
	UPROPERTY() bool bRecomputeTangents = false;
	UPROPERTY() bool bSetUseMikkTSpace = false;
	UPROPERTY() bool bUseMikkTSpace = false;
	UPROPERTY() bool bSetGenerateLightmapUVs = false;
	UPROPERTY() bool bGenerateLightmapUVs = false;
	UPROPERTY() bool bSetLightmapLayout = false;
	UPROPERTY() int32 MinLightmapResolution = 64;
	UPROPERTY() int32 SrcLightmapIndex = 0;
	UPROPERTY() int32 DstLightmapIndex = 1;
	UPROPERTY() bool bSetBuildScale = false;
	UPROPERTY() FVector BuildScale3D = FVector::OneVector;
	UPROPERTY() bool bSetDistanceFieldResolutionScale = false;
	UPROPERTY() double DistanceFieldResolutionScale = 1.0;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FHyperAIGeometryBuildPolicyPatch Patch;
	UPROPERTY() int32 DeadlineMs = 1500;
	UPROPERTY() int32 MaxGameThreadMs = 200;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 PatchFieldCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bDetachedImmutableClone = false;
	UPROPERTY() bool bWouldTransactionOnce = false;
	UPROPERTY() bool bWouldBuildOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGeometryApplyPlanReport
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
	UPROPERTY() FHyperAIGeometryPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIGeometryIssue> Issues;
	UPROPERTY() TArray<FHyperAIGeometryCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Geometry atomic cohort. */
UCLASS()
class HYPERAISTUDIOGEOMETRY_API UHyperAIStudioGeometryToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Geometry")
	static FHyperAIGeometryInspectReport hyper_geometry_inspect(
		const FHyperAIGeometryInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Geometry")
	static FHyperAIGeometryApplyPlanReport hyper_geometry_apply_plan(
		const FHyperAIGeometryApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Geometry")
	static FHyperAIGeometryValidateReport hyper_geometry_validate(
		const FHyperAIGeometryValidateRequest& Request);
};

/** Detached, UObject-free input to the independent validator. */
struct FHyperAIStudioGeometryValueSnapshot
{
	FHyperAIGeometryAssetRecord Asset;
	TArray<FHyperAIGeometryLodRecord> Lods;
	TArray<FHyperAIGeometryMaterialRecord> Materials;
	TArray<FHyperAIGeometryIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioGeometryInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIGeometryInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioGeometryValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIGeometryValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioGeometryPatchPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedRevision;
	FHyperAIGeometryBuildPolicyPatch Patch;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioGeometryInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIGeometryInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioGeometryValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIGeometryValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioGeometryDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioGeometryDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioGeometryManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

struct FHyperAIStudioGeometryAuthorityRecord
{
	FString SourceCoordinate;
	FString Disposition;
	FString Route;
	FString Access;
	FString SourceHash;
};

class FHyperAIStudioGeometryContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("geometry_interchange");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiogeometrytoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("geometry_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("geometry_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.geometry_script");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_mesh_build_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("static_mesh_build_policy_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_mesh_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.geometry.inspect.v1");
	static constexpr const TCHAR* PatchPayloadTypeId = TEXT("hyperai.payload.geometry.build-policy.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.geometry.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.geometry.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.geometry.mutation-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.geometry.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_mesh_build_save_continuation_required");
	static constexpr const TCHAR* NativeAccessReviewId =
		TEXT("epic-ue5.8-native-aicallable-access-2026-08-15");
	static constexpr const TCHAR* NativeAccessReviewRecordsSha256 =
		TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr const TCHAR* CapabilityRequirementsSha256 =
		TEXT("sha256:d51a121e238a5ca14a86b639f6fa8002a5bc4294b04e56d73d2c89c78e03d825");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxSourceModels = 128;
	static constexpr int32 MaxMaterialSlots = 256;
	static constexpr int32 MaxPageSize = 64;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 32 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxPrepareDeadlineMs = 2000;
	static constexpr int32 MinPrepareDeadlineMs = 100;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioGeometryManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FString>& GetEpicNativeDelegates();
	static const TArray<FString>& GetEpicPythonDelegates();
	static const TArray<FHyperAIStudioGeometryAuthorityRecord>& GetCapabilityRequirements();
	static TArray<FHyperAIGeometryCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
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
		FHyperAIStudioGeometryValueSnapshot& OutSnapshot, FString& OutStatus,
		FString& OutDiagnostic);
	static bool ValidateDetached(const FHyperAIStudioGeometryValueSnapshot& Snapshot,
		const FString& Policy, int32 MaxIssues, int32 OutputByteLimit,
		FHyperAIGeometryValidateReport& OutReport);
	static FHyperAIGeometryInspectReport Inspect(const FHyperAIGeometryInspectRequest& Request);
	static FHyperAIGeometryValidateReport Validate(const FHyperAIGeometryValidateRequest& Request);
	static FHyperAIGeometryApplyPlanReport BuildPlan(
		const FHyperAIGeometryApplyPlanRequest& Request);
	static FString ComputePatchSemanticFingerprint(
		const FHyperAIStudioGeometryPatchPayload& Payload);
};

class FHyperAIStudioGeometryRegistration final
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
	TSharedPtr<FHyperAIStudioGeometryDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
