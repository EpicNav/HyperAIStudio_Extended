// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPhysicsToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIPhysicsIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("physics_asset");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() FString State = TEXT("source_candidate_backend_required");
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> DelegatedPythonCallables;
	UPROPERTY() TArray<FString> ClosedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsSolverRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 PositionIterations = 0;
	UPROPERTY() int32 VelocityIterations = 0;
	UPROPERTY() int32 ProjectionIterations = 0;
	UPROPERTY() double CullDistance = 0.0;
	UPROPERTY() double MaxDepenetrationVelocity = 0.0;
	UPROPERTY() double FixedTimeStep = 0.0;
	UPROPERTY() bool bUseLinearJointSolver = false;
	UPROPERTY() bool bUseManifolds = false;
	UPROPERTY() FString Fingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsBodyRecord
{
	GENERATED_BODY()

	UPROPERTY() FString StableId;
	UPROPERTY() FString BoneName;
	UPROPERTY() FString BodyObjectPath;
	UPROPERTY() FString BodyFingerprint;
	UPROPERTY() FString DefaultInstanceFingerprint;
	UPROPERTY() FString BodySetupGuid;
	UPROPERTY() FString PhysicalMaterialPath;
	UPROPERTY() FString CollisionProfileName;
	UPROPERTY() int32 PhysicsType = 0;
	UPROPERTY() int32 CollisionTraceFlag = 0;
	UPROPERTY() int32 BodyCollisionResponse = 0;
	UPROPERTY() int32 CollisionEnabled = 0;
	UPROPERTY() double MassScale = 0.0;
	UPROPERTY() bool bSimulatePhysics = false;
	UPROPERTY() bool bConsiderForBounds = false;
	UPROPERTY() bool bShapeProjectionComplete = false;
	UPROPERTY() bool bShapeGeometryValid = false;
	UPROPERTY() int32 SphereCount = 0;
	UPROPERTY() int32 BoxCount = 0;
	UPROPERTY() int32 CapsuleCount = 0;
	UPROPERTY() int32 ConvexCount = 0;
	UPROPERTY() int32 TaperedCapsuleCount = 0;
	UPROPERTY() int32 UnsupportedShapeCount = 0;
	UPROPERTY() int32 TotalShapeCount = 0;
	UPROPERTY() int32 InvalidShapeCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsConstraintRecord
{
	GENERATED_BODY()

	UPROPERTY() FString StableId;
	UPROPERTY() FString TemplateObjectPath;
	UPROPERTY() FString ConstraintFingerprint;
	UPROPERTY() FString ChildBoneName;
	UPROPERTY() FString ParentBoneName;
	UPROPERTY() int32 LinearXMotion = 0;
	UPROPERTY() int32 LinearYMotion = 0;
	UPROPERTY() int32 LinearZMotion = 0;
	UPROPERTY() int32 Swing1Motion = 0;
	UPROPERTY() int32 Swing2Motion = 0;
	UPROPERTY() int32 TwistMotion = 0;
	UPROPERTY() double LinearLimit = 0.0;
	UPROPERTY() double Swing1LimitDegrees = 0.0;
	UPROPERTY() double Swing2LimitDegrees = 0.0;
	UPROPERTY() double TwistLimitDegrees = 0.0;
	UPROPERTY() bool bDisableCollision = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsCollisionPairRecord
{
	GENERATED_BODY()

	UPROPERTY() FString StableId;
	UPROPERTY() int32 BodyIndexA = INDEX_NONE;
	UPROPERTY() int32 BodyIndexB = INDEX_NONE;
	UPROPERTY() FString BodyNameA;
	UPROPERTY() FString BodyNameB;
	UPROPERTY() bool bDisabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() FString VolatileObservationFingerprint;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() int32 BodyCount = 0;
	UPROPERTY() int32 ConstraintCount = 0;
	UPROPERTY() int32 DisabledCollisionPairCount = 0;
	UPROPERTY() FHyperAIPhysicsSolverRecord Solver;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical loaded top-level /Game UPhysicsAsset object path. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 PageSize = 32;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() bool bCursorEligible = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAIPhysicsAssetRecord Asset;
	UPROPERTY() TArray<FHyperAIPhysicsBodyRecord> Bodies;
	UPROPERTY() TArray<FHyperAIPhysicsConstraintRecord> Constraints;
	UPROPERTY() TArray<FHyperAIPhysicsCollisionPairRecord> DisabledCollisionPairs;
	UPROPERTY() TArray<FHyperAIPhysicsIssue> Issues;
	UPROPERTY() TArray<FHyperAIPhysicsCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	/** structural or simulation_ready. */
	UPROPERTY() FString Policy = TEXT("structural");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsValidateReport
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
	UPROPERTY() TArray<FHyperAIPhysicsIssue> Issues;
	UPROPERTY() TArray<FHyperAIPhysicsCapabilityStatus> Capabilities;
};

/** Closed missing-gap patch. Epic-equivalent mass/body-mode/shape/constraint-limit edits are excluded. */
USTRUCT(BlueprintType)
struct FHyperAIPhysicsSettingsPatch
{
	GENERATED_BODY()

	UPROPERTY() FString BodyName;
	UPROPERTY() FString ExpectedBodyFingerprint;
	UPROPERTY() bool bSetCollisionProfile = false;
	UPROPERTY() FString CollisionProfileName;
	UPROPERTY() bool bSetCollisionEnabled = false;
	/** Exact ECollisionEnabled::Type integer in [0,5]. */
	UPROPERTY() int32 CollisionEnabled = 0;
	UPROPERTY() bool bSetSimulatePhysics = false;
	UPROPERTY() bool bSimulatePhysics = false;
	UPROPERTY() bool bSetSolverSettings = false;
	UPROPERTY() FString ExpectedSolverFingerprint;
	UPROPERTY() int32 PositionIterations = 0;
	UPROPERTY() int32 VelocityIterations = 0;
	UPROPERTY() int32 ProjectionIterations = 0;
	UPROPERTY() double CullDistance = 0.0;
	UPROPERTY() double MaxDepenetrationVelocity = 0.0;
	UPROPERTY() double FixedTimeStep = 0.0;
	UPROPERTY() bool bUseLinearJointSolver = false;
	UPROPERTY() bool bUseManifolds = false;
	UPROPERTY() bool bSetConstraintCollisionDisabled = false;
	UPROPERTY() FString ConstraintStableId;
	UPROPERTY() FString ExpectedConstraintFingerprint;
	UPROPERTY() bool bConstraintCollisionDisabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FHyperAIPhysicsSettingsPatch Patch;
	UPROPERTY() int32 DeadlineMs = 1500;
	UPROPERTY() int32 MaxGameThreadMs = 200;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 PatchFieldCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bDetachedImmutableClone = false;
	UPROPERTY() bool bWouldTransactionOnce = false;
	UPROPERTY() bool bWouldRebuildPhysicsStateOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPhysicsApplyPlanReport
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
	UPROPERTY() FHyperAIPhysicsPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIPhysicsIssue> Issues;
	UPROPERTY() TArray<FHyperAIPhysicsCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Physics/Chaos atomic cohort. */
UCLASS()
class HYPERAISTUDIOPHYSICS_API UHyperAIStudioPhysicsToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Physics")
	static FHyperAIPhysicsInspectReport hyper_physics_inspect(
		const FHyperAIPhysicsInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Physics")
	static FHyperAIPhysicsApplyPlanReport hyper_physics_apply_plan(
		const FHyperAIPhysicsApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Physics")
	static FHyperAIPhysicsValidateReport hyper_physics_validate(
		const FHyperAIPhysicsValidateRequest& Request);
};

/** Detached, UObject-free input to the independent validator. */
struct FHyperAIStudioPhysicsValueSnapshot
{
	FHyperAIPhysicsAssetRecord Asset;
	TArray<FHyperAIPhysicsBodyRecord> Bodies;
	TArray<FHyperAIPhysicsConstraintRecord> Constraints;
	TArray<FHyperAIPhysicsCollisionPairRecord> DisabledCollisionPairs;
	TArray<FHyperAIPhysicsIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioPhysicsInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPhysicsInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPhysicsValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPhysicsValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPhysicsPatchPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedRevision;
	FHyperAIPhysicsSettingsPatch Patch;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioPhysicsInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPhysicsInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPhysicsValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPhysicsValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPhysicsDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioPhysicsDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioPhysicsManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioPhysicsContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("physics_chaos");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiophysicstoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("physics_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("physics_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.physics_asset");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_physics_asset_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("guarded_settings_patch_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_physics_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.physics.inspect.v1");
	static constexpr const TCHAR* PatchPayloadTypeId = TEXT("hyperai.payload.physics.settings_patch.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.physics.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.physics.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.physics.mutation-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.physics.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_compile_or_simulation_backend_required");
	static constexpr const TCHAR* NativeAccessReviewId =
		TEXT("epic-ue5.8-native-aicallable-access-2026-08-15");
	static constexpr const TCHAR* NativeAccessReviewRecordsSha256 =
		TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxBodies = 256;
	static constexpr int32 MaxConstraints = 512;
	static constexpr int32 MaxDisabledCollisionPairs = 4096;
	static constexpr int32 MaxShapesPerBody = 256;
	static constexpr int32 MaxConvexVerticesPerShape = 4096;
	static constexpr int32 MaxConvexIndicesPerShape = 24576;
	static constexpr int32 MaxProjectionDepth = 12;
	static constexpr int32 MaxProjectionProperties = 4096;
	static constexpr int32 MaxProjectionContainerElements = 8192;
	static constexpr int32 MaxProjectionCharacters = 768 * 1024;
	static constexpr int32 MaxPageSize = 64;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 24 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxPrepareDeadlineMs = 2000;
	static constexpr int32 MinPrepareDeadlineMs = 100;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioPhysicsManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FString>& GetEpicDelegates();
	static const TArray<FString>& GetPythonDelegates();
	static TArray<FHyperAIPhysicsCapabilityStatus> GetCapabilityMatrix();
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
		FHyperAIStudioPhysicsValueSnapshot& OutSnapshot, FString& OutStatus,
		FString& OutDiagnostic);
	static bool ValidateDetached(const FHyperAIStudioPhysicsValueSnapshot& Snapshot,
		const FString& Policy, int32 MaxIssues, int32 OutputByteLimit,
		FHyperAIPhysicsValidateReport& OutReport);
	static FHyperAIPhysicsInspectReport Inspect(const FHyperAIPhysicsInspectRequest& Request);
	static FHyperAIPhysicsValidateReport Validate(const FHyperAIPhysicsValidateRequest& Request);
	static FHyperAIPhysicsApplyPlanReport BuildPlan(
		const FHyperAIPhysicsApplyPlanRequest& Request);
	static FString ComputePatchSemanticFingerprint(
		const FHyperAIStudioPhysicsPatchPayload& Payload);
};

class FHyperAIStudioPhysicsRegistration final
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
	TSharedPtr<FHyperAIStudioPhysicsDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
