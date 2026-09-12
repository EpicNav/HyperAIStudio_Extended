// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPaper2DToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIPaper2DIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("paper2d_metadata");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() FString State = TEXT("source_candidate_backend_required");
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> ClosedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DReferenceRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Role;
	UPROPERTY() FString ObjectPath;
	UPROPERTY() FString ClassPath;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DKeyFrameRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 FrameRun = 0;
	UPROPERTY() FString SpritePath;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DLayerRecord
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() FString LayerPath;
	UPROPERTY() FString LayerName;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	UPROPERTY() bool bCollides = false;
	UPROPERTY() bool bVisibleInEditor = false;
	UPROPERTY() bool bVisibleInGame = false;
};

/** Detached common projection for an exact loaded Paper2D top-level asset. */
USTRUCT(BlueprintType)
struct FHyperAIPaper2DAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString PackageName;
	/** sprite, flipbook, tile_set, or tile_map. */
	UPROPERTY() FString AssetKind;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() FString VolatileObservationFingerprint;
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bProjectionComplete = false;

	UPROPERTY() double PixelsPerUnrealUnit = 0.0;
	UPROPERTY() int32 CollisionMode = 0;
	UPROPERTY() double CollisionThickness = 0.0;
	UPROPERTY() FString DefaultMaterialPath;
	UPROPERTY() FString AlternateMaterialPath;
	UPROPERTY() FString BodySetupPath;

	UPROPERTY() double FramesPerSecond = 0.0;
	UPROPERTY() int32 NumFrames = 0;
	UPROPERTY() int32 NumKeyFrames = 0;

	UPROPERTY() FString TileSheetPath;
	UPROPERTY() int32 TileSheetWidth = 0;
	UPROPERTY() int32 TileSheetHeight = 0;
	UPROPERTY() int32 TileWidth = 0;
	UPROPERTY() int32 TileHeight = 0;
	UPROPERTY() int32 TileCountX = 0;
	UPROPERTY() int32 TileCountY = 0;
	UPROPERTY() int32 TileCount = 0;
	UPROPERTY() int32 MarginLeft = 0;
	UPROPERTY() int32 MarginTop = 0;
	UPROPERTY() int32 MarginRight = 0;
	UPROPERTY() int32 MarginBottom = 0;
	UPROPERTY() int32 SpacingX = 0;
	UPROPERTY() int32 SpacingY = 0;
	UPROPERTY() int32 DrawingOffsetX = 0;
	UPROPERTY() int32 DrawingOffsetY = 0;

	UPROPERTY() int32 MapWidth = 0;
	UPROPERTY() int32 MapHeight = 0;
	UPROPERTY() int32 ProjectionMode = 0;
	UPROPERTY() int32 LayerCount = 0;
	UPROPERTY() int32 CollidingLayerCount = 0;

	UPROPERTY() double SourceWidth = 0.0;
	UPROPERTY() double SourceHeight = 0.0;
	UPROPERTY() int32 RenderVertexCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical loaded top-level /Game Paper2D object path. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FHyperAIPaper2DAssetRecord Asset;
	UPROPERTY() TArray<FHyperAIPaper2DReferenceRecord> References;
	UPROPERTY() TArray<FHyperAIPaper2DKeyFrameRecord> KeyFrames;
	UPROPERTY() TArray<FHyperAIPaper2DLayerRecord> Layers;
	UPROPERTY() TArray<FHyperAIPaper2DIssue> Issues;
	UPROPERTY() TArray<FHyperAIPaper2DCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	/** structural or authoring_ready. */
	UPROPERTY() FString Policy = TEXT("structural");
	UPROPERTY() int32 MaxIssues = 64;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DValidateReport
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
	UPROPERTY() TArray<FHyperAIPaper2DIssue> Issues;
	UPROPERTY() TArray<FHyperAIPaper2DCapabilityStatus> Capabilities;
};

/** Closed edit-only metadata patch; creation, resize, deletion and cell edits are excluded. */
USTRUCT(BlueprintType)
struct FHyperAIPaper2DMetadataPatch
{
	GENERATED_BODY()

	UPROPERTY() FString ExpectedKind;
	UPROPERTY() bool bSetPixelsPerUnrealUnit = false;
	UPROPERTY() double PixelsPerUnrealUnit = 0.0;
	UPROPERTY() bool bSetCollisionMode = false;
	/** Exact Paper2D collision enum integer in [0,2]. */
	UPROPERTY() int32 CollisionMode = 0;
	UPROPERTY() bool bSetCollisionThickness = false;
	UPROPERTY() double CollisionThickness = 0.0;
	UPROPERTY() bool bSetFramesPerSecond = false;
	UPROPERTY() double FramesPerSecond = 0.0;
	UPROPERTY() bool bSetTileSize = false;
	UPROPERTY() int32 TileWidth = 0;
	UPROPERTY() int32 TileHeight = 0;
	UPROPERTY() bool bSetTileSpacing = false;
	UPROPERTY() int32 SpacingX = 0;
	UPROPERTY() int32 SpacingY = 0;
	UPROPERTY() bool bSetTileDrawingOffset = false;
	UPROPERTY() int32 DrawingOffsetX = 0;
	UPROPERTY() int32 DrawingOffsetY = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FHyperAIPaper2DMetadataPatch Patch;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 PatchFieldCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bDetachedImmutableClone = false;
	UPROPERTY() bool bWouldTransactionOnce = false;
	UPROPERTY() bool bWouldRebuildOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPaper2DApplyPlanReport
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
	UPROPERTY() FHyperAIPaper2DPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIPaper2DIssue> Issues;
	UPROPERTY() TArray<FHyperAIPaper2DCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Paper2D atomic cohort. */
UCLASS()
class HYPERAISTUDIOPAPER2D_API UHyperAIStudioPaper2DToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Paper2D")
	static FHyperAIPaper2DInspectReport hyper_paper2d_inspect(
		const FHyperAIPaper2DInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Paper2D")
	static FHyperAIPaper2DApplyPlanReport hyper_paper2d_apply_plan(
		const FHyperAIPaper2DApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Paper2D")
	static FHyperAIPaper2DValidateReport hyper_paper2d_validate(
		const FHyperAIPaper2DValidateRequest& Request);
};

/** Detached, UObject-free input to the independent validator. */
struct FHyperAIStudioPaper2DValueSnapshot
{
	FHyperAIPaper2DAssetRecord Asset;
	TArray<FHyperAIPaper2DReferenceRecord> References;
	TArray<FHyperAIPaper2DKeyFrameRecord> KeyFrames;
	TArray<FHyperAIPaper2DLayerRecord> Layers;
	TArray<FHyperAIPaper2DIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioPaper2DInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPaper2DInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPaper2DValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPaper2DValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPaper2DPatchPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedRevision;
	FHyperAIPaper2DMetadataPatch Patch;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioPaper2DInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPaper2DInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPaper2DValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPaper2DValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPaper2DDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioPaper2DDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioPaper2DManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioPaper2DContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("paper2d");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiopaper2dtoolset.v1");
	static constexpr const TCHAR* BlockingRequirementGroupId = TEXT("paper2d");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.paper2d");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_paper2d_metadata_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("guarded_paper2d_metadata_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_paper2d_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.paper2d.inspect.v1");
	static constexpr const TCHAR* PatchPayloadTypeId = TEXT("hyperai.payload.paper2d.metadata_patch.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.paper2d.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.paper2d.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.paper2d.mutation-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.paper2d.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_rebuild_save_backend_required");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3bcefe82b4acab88");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxKeyFrames = 256;
	static constexpr int32 MaxLayers = 128;
	static constexpr int32 MaxReferences = MaxKeyFrames + MaxLayers + 8;
	static constexpr int32 MaxRenderVertices = 1024 * 1024;
	static constexpr int32 MaxTiles = 1024 * 1024;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 200;
	static constexpr int32 MinPrepareDeadlineMs = 100;
	static constexpr int32 MaxPrepareDeadlineMs = 2000;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioPaper2DManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FString>& GetEpicDelegates();
	static TArray<FHyperAIPaper2DCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString InspectPayloadSchemaFingerprint();
	static FString PatchPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool CaptureExact(const FString& TargetPath, int32 MaxWorkMs,
		FHyperAIStudioPaper2DValueSnapshot& OutSnapshot, FString& OutStatus,
		FString& OutDiagnostic);
	static bool ValidateDetached(const FHyperAIStudioPaper2DValueSnapshot& Snapshot,
		const FString& Policy, int32 MaxIssueCount, int32 OutputByteLimit,
		FHyperAIPaper2DValidateReport& OutReport);
	static FHyperAIPaper2DInspectReport Inspect(const FHyperAIPaper2DInspectRequest& Request);
	static FHyperAIPaper2DValidateReport Validate(const FHyperAIPaper2DValidateRequest& Request);
	static FHyperAIPaper2DApplyPlanReport BuildPlan(
		const FHyperAIPaper2DApplyPlanRequest& Request);
	/** Public-API support matrix for the default fast reversible edit path. */
	static bool CanExecuteFastPatch(const FHyperAIPaper2DMetadataPatch& Patch,
		FString& OutReason);
	static FString ComputePatchSemanticFingerprint(
		const FHyperAIStudioPaper2DPatchPayload& Payload);
};

class FHyperAIStudioPaper2DRegistration final
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
	TSharedPtr<FHyperAIStudioPaper2DDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
