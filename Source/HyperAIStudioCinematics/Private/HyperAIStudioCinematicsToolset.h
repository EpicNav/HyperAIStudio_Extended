// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioCinematicsToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAICinematicsIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsDelegationAuthority
{
	GENERATED_BODY()

	UPROPERTY() FString AuthorityKind;
	UPROPERTY() FString SourceGroup;
	UPROPERTY() int32 CallableCount = 0;
	UPROPERTY() FString ReviewedFingerprint;
	UPROPERTY() FString SourceCoordinate;
	UPROPERTY() FString Disposition = TEXT("epic_delegate");
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("cinematics");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() bool bRenderSubmissionImplemented = false;
	UPROPERTY() bool bMovieRenderPipelineHardLinked = true;
	UPROPERTY() FString State = TEXT("source_candidate_async_continuation_host_required");
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() int32 DelegationAuthorityCount = 0;
	UPROPERTY() int32 DelegatedEpicCallableCount = 0;
	UPROPERTY() FString DelegationAuthorityFingerprint;
	/** Kept empty in bounded reports; GetDelegationAuthority() owns the exact frozen matrix. */
	UPROPERTY() TArray<FHyperAICinematicsDelegationAuthority> DelegationAuthority;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsFrameRange
{
	GENERATED_BODY()

	UPROPERTY() bool bHasLowerBound = false;
	UPROPERTY() bool bLowerInclusive = false;
	UPROPERTY() int32 LowerFrame = 0;
	UPROPERTY() bool bHasUpperBound = false;
	UPROPERTY() bool bUpperInclusive = false;
	UPROPERTY() int32 UpperFrame = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsSequenceRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString ClassName;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() bool bPersistedRevisionComplete = false;
	UPROPERTY() FString VolatileRevision;
	UPROPERTY() bool bVolatileRevisionComplete = false;
	UPROPERTY() int32 TickResolutionNumerator = 0;
	UPROPERTY() int32 TickResolutionDenominator = 1;
	UPROPERTY() int32 DisplayRateNumerator = 0;
	UPROPERTY() int32 DisplayRateDenominator = 1;
	UPROPERTY() FHyperAICinematicsFrameRange PlaybackRange;
	UPROPERTY() int32 BindingCount = 0;
	UPROPERTY() int32 TrackCount = 0;
	UPROPERTY() int32 SectionCount = 0;
	UPROPERTY() int32 ChannelCount = 0;
	UPROPERTY() int32 KeyCount = 0;
	UPROPERTY() int32 CameraCutCount = 0;
	UPROPERTY() int32 EventCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsBindingRecord
{
	GENERATED_BODY()

	UPROPERTY() FString BindingId;
	UPROPERTY() FString BindingGuid;
	UPROPERTY() FString Name;
	UPROPERTY() FString Kind;
	UPROPERTY() bool bDetailProjectionComplete = false;
	UPROPERTY() FString DetailFingerprint;
	UPROPERTY() int32 TrackCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsTrackRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TrackId;
	UPROPERTY() FString BindingGuid;
	UPROPERTY() FString ClassName;
	UPROPERTY() bool bRootTrack = false;
	UPROPERTY() bool bCameraCutTrack = false;
	UPROPERTY() bool bEventTrack = false;
	UPROPERTY() bool bDetailProjectionComplete = false;
	UPROPERTY() FString DetailFingerprint;
	UPROPERTY() int32 SectionCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsSectionRecord
{
	GENERATED_BODY()

	UPROPERTY() FString SectionId;
	UPROPERTY() FString TrackId;
	UPROPERTY() FString ClassName;
	UPROPERTY() FHyperAICinematicsFrameRange Range;
	UPROPERTY() int32 RowIndex = 0;
	UPROPERTY() int32 OverlapPriority = 0;
	UPROPERTY() int32 PreRollFrames = 0;
	UPROPERTY() int32 PostRollFrames = 0;
	UPROPERTY() bool bActive = false;
	UPROPERTY() bool bLocked = false;
	UPROPERTY() bool bDetailProjectionComplete = false;
	UPROPERTY() FString DetailFingerprint;
	UPROPERTY() bool bChannelProjectionSupported = false;
	UPROPERTY() FString ChannelFingerprint;
	UPROPERTY() int32 ChannelCount = 0;
	UPROPERTY() int32 KeyCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsKeyRecord
{
	GENERATED_BODY()

	UPROPERTY() FString KeyId;
	UPROPERTY() FString SectionId;
	UPROPERTY() FString ChannelType;
	UPROPERTY() int32 ChannelIndex = 0;
	UPROPERTY() int32 KeyIndex = 0;
	UPROPERTY() int32 Frame = 0;
	UPROPERTY() FString ValueFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsCameraRecord
{
	GENERATED_BODY()

	UPROPERTY() FString CameraCutId;
	UPROPERTY() FString SectionId;
	UPROPERTY() FString CameraBindingId;
	UPROPERTY() int32 CameraBindingSequenceId = 0;
	UPROPERTY() int32 CameraBindingResolveParentIndex = 0;
	UPROPERTY() bool bLockPreviousCamera = false;
	UPROPERTY() FString BindingFingerprint;
	UPROPERTY() FHyperAICinematicsFrameRange Range;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsEventRecord
{
	GENERATED_BODY()

	UPROPERTY() FString EventId;
	UPROPERTY() FString SectionId;
	UPROPERTY() FString EventKind;
	UPROPERTY() int32 Frame = 0;
	UPROPERTY() FString CompiledFunctionName;
	UPROPERTY() FString BoundObjectClassName;
	UPROPERTY() int32 PayloadVariableCount = 0;
	UPROPERTY() bool bDetailProjectionComplete = false;
	UPROPERTY() FString DetailFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsMRQSettingRecord
{
	GENERATED_BODY()

	UPROPERTY() FString SettingId;
	UPROPERTY() FString ClassName;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bDetailProjectionComplete = false;
	UPROPERTY() FString DetailFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsMRQConfigRecord
{
	GENERATED_BODY()

	UPROPERTY() bool bPresent = false;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString PersistedRevision;
	UPROPERTY() bool bPersistedRevisionComplete = false;
	UPROPERTY() FString VolatileRevision;
	UPROPERTY() bool bVolatileRevisionComplete = false;
	UPROPERTY() int32 SettingsSerialNumber = -1;
	UPROPERTY() int32 SettingCount = 0;
	UPROPERTY() int32 ShotOverrideCount = 0;
	UPROPERTY() bool bOutputSettingPresent = false;
	UPROPERTY() FString OutputSettingFingerprint;
	UPROPERTY() FString OutputDirectory;
	UPROPERTY() FString FileNameFormat;
	UPROPERTY() int32 OutputWidth = 0;
	UPROPERTY() int32 OutputHeight = 0;
	UPROPERTY() int32 OutputRateNumerator = 0;
	UPROPERTY() int32 OutputRateDenominator = 1;
	UPROPERTY() bool bUseCustomFrameRate = false;
	UPROPERTY() bool bUseCustomPlaybackRange = false;
	UPROPERTY() int32 CustomStartFrame = 0;
	UPROPERTY() int32 CustomEndFrame = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsInspectRequest
{
	GENERATED_BODY()

	/** Exact canonical already-loaded top-level /Game LevelSequence object path. */
	UPROPERTY() FString SequencePath;
	/** Optional exact canonical already-loaded top-level /Game MoviePipelinePrimaryConfig path. */
	UPROPERTY() FString RenderConfigPath;
	UPROPERTY() int32 PageSize = 64;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 262144;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() bool bCursorEligible = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString SnapshotFingerprint;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAICinematicsSequenceRecord Sequence;
	UPROPERTY() FHyperAICinematicsMRQConfigRecord RenderConfig;
	UPROPERTY() TArray<FHyperAICinematicsBindingRecord> Bindings;
	UPROPERTY() TArray<FHyperAICinematicsTrackRecord> Tracks;
	UPROPERTY() TArray<FHyperAICinematicsSectionRecord> Sections;
	UPROPERTY() TArray<FHyperAICinematicsKeyRecord> Keys;
	UPROPERTY() TArray<FHyperAICinematicsCameraRecord> Cameras;
	UPROPERTY() TArray<FHyperAICinematicsEventRecord> Events;
	UPROPERTY() TArray<FHyperAICinematicsMRQSettingRecord> RenderSettings;
	UPROPERTY() TArray<FHyperAICinematicsIssue> Issues;
	UPROPERTY() TArray<FHyperAICinematicsCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString SequencePath;
	UPROPERTY() FString RenderConfigPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FString ExpectedVolatileRevision;
	UPROPERTY() FString ExpectedRenderConfigPersistedRevision;
	UPROPERTY() FString ExpectedRenderConfigVolatileRevision;
	/** structural or render_ready. */
	UPROPERTY() FString Policy = TEXT("structural");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 262144;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsValidateReport
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
	UPROPERTY() FString VolatileRevision;
	UPROPERTY() FString RenderConfigPersistedRevision;
	UPROPERTY() FString RenderConfigVolatileRevision;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAICinematicsIssue> Issues;
	UPROPERTY() TArray<FHyperAICinematicsCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsRenderIntent
{
	GENERATED_BODY()

	UPROPERTY() FString Lifecycle = TEXT("render_validated_sequence");
	UPROPERTY() FString ExpectedTerminalState = TEXT("render_outputs_verified");
	UPROPERTY() bool bRequireCameraCut = true;
	UPROPERTY() bool bRequireOutputSetting = true;
	UPROPERTY() bool bVerifyOutputFrames = true;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString SequencePath;
	UPROPERTY() FString RenderConfigPath;
	UPROPERTY() FString ExpectedPersistedRevision;
	UPROPERTY() FString ExpectedVolatileRevision;
	UPROPERTY() FString ExpectedRenderConfigPersistedRevision;
	UPROPERTY() FString ExpectedRenderConfigVolatileRevision;
	UPROPERTY() FHyperAICinematicsRenderIntent Intent;
	UPROPERTY() int32 DeadlineMs = 300000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 262144;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 BeginCallCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bWouldCompileOnce = false;
	UPROPERTY() bool bWouldSubmitRenderOnce = false;
	UPROPERTY() bool bWouldPollWithoutBlocking = false;
	UPROPERTY() bool bWouldValidateFreshOnce = false;
	UPROPERTY() bool bWouldVerifyOutputFrames = false;
	UPROPERTY() bool bWouldObserveTerminalState = false;
};

USTRUCT(BlueprintType)
struct FHyperAICinematicsApplyPlanReport
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
	UPROPERTY() FString BaseVolatileRevision;
	UPROPERTY() FString BaseRenderConfigPersistedRevision;
	UPROPERTY() FString BaseRenderConfigVolatileRevision;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAICinematicsPlanEffects Effects;
	UPROPERTY() TArray<FHyperAICinematicsIssue> Issues;
	UPROPERTY() TArray<FHyperAICinematicsCapabilityStatus> Capabilities;
};

/** Exactly three functions form the cinematics atomic cohort. */
UCLASS()
class HYPERAISTUDIOCINEMATICS_API UHyperAIStudioCinematicsToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAICinematicsInspectReport hyper_cinematics_inspect(
		const FHyperAICinematicsInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAICinematicsApplyPlanReport hyper_cinematics_apply_plan(
		const FHyperAICinematicsApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAICinematicsValidateReport hyper_cinematics_validate(
		const FHyperAICinematicsValidateRequest& Request);
};

/** Detached value-only input to the independent validator. */
struct FHyperAIStudioCinematicsValueSnapshot
{
	FHyperAICinematicsSequenceRecord Sequence;
	FHyperAICinematicsMRQConfigRecord RenderConfig;
	TArray<FHyperAICinematicsBindingRecord> Bindings;
	TArray<FHyperAICinematicsTrackRecord> Tracks;
	TArray<FHyperAICinematicsSectionRecord> Sections;
	TArray<FHyperAICinematicsKeyRecord> Keys;
	TArray<FHyperAICinematicsCameraRecord> Cameras;
	TArray<FHyperAICinematicsEventRecord> Events;
	TArray<FHyperAICinematicsMRQSettingRecord> RenderSettings;
	TArray<FHyperAICinematicsIssue> CaptureIssues;
	FString SnapshotFingerprint;
	bool bComplete = false;
};

class FHyperAIStudioCinematicsInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAICinematicsInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCinematicsValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAICinematicsValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCinematicsRenderPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString SequencePath;
	FString RenderConfigPath;
	FString BasePersistedRevision;
	FString BaseVolatileRevision;
	FString BaseRenderConfigPersistedRevision;
	FString BaseRenderConfigVolatileRevision;
	FString OutputSettingFingerprint;
	FString Lifecycle;
	FString ExpectedTerminalState;
	int32 AsyncDeadlineMs = 0;
	bool bRequireCameraCut = true;
	bool bRequireOutputSetting = true;
	bool bVerifyOutputFrames = true;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioCinematicsInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAICinematicsInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCinematicsValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAICinematicsValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCinematicsDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioCinematicsDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioCinematicsManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioCinematicsContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("cinematics");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiocinematicstoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("cinematics_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("cinematics_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.sequencer");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_sequence_render_snapshot.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("render_validated_sequence_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_value_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.cinematics.inspect.v1");
	static constexpr const TCHAR* RenderPayloadTypeId = TEXT("hyperai.payload.cinematics.render_preflight.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.cinematics.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.cinematics.inspect.v1");
	static constexpr const TCHAR* RenderResultTypeId = TEXT("hyperai.result.cinematics.render_preflight.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.cinematics.validate.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("async_continuation_host_required");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxStringCharacters = 1024;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxBindings = 64;
	static constexpr int32 MaxTracks = 256;
	static constexpr int32 MaxSections = 1024;
	static constexpr int32 MaxChannels = 2048;
	static constexpr int32 MaxKeys = 4096;
	static constexpr int32 MaxCameraCuts = 512;
	static constexpr int32 MaxEvents = 512;
	static constexpr int32 MaxEventPayloadVariables = 64;
	static constexpr int32 MaxMRQSettings = 64;
	static constexpr int32 MaxSnapshotItems = MaxBindings + MaxTracks + MaxSections
		+ MaxKeys + MaxCameraCuts + MaxEvents + MaxMRQSettings;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxAsyncDeadlineMs = 30 * 60 * 1000;
	static constexpr int32 MinAsyncDeadlineMs = 5000;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioCinematicsManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FHyperAICinematicsCapabilityStatus> GetCapabilityMatrix();
	static const TArray<FHyperAICinematicsDelegationAuthority>& GetDelegationAuthority();
	static bool IsCanonicalPrimaryAssetPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static bool AdmitCountsBeforeProjection(
		int32 Bindings, int32 Tracks, int32 Sections, int32 Channels,
		int32 Keys, int32 Cameras, int32 Events, int32 MRQSettings,
		FString& OutError);
	static FString InspectPayloadSchemaFingerprint();
	static FString RenderPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString RenderResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FString BuildCursor(
		const FString& SequencePath, const FString& RenderConfigPath,
		const FString& SnapshotFingerprint, int32 PageSize, int32 Offset);
	static bool ParseCursor(
		const FString& Cursor, const FString& SequencePath, const FString& RenderConfigPath,
		const FString& SnapshotFingerprint, int32 PageSize, int32& OutOffset);
	static FString ComputeRenderSemanticFingerprint(
		const FHyperAIStudioCinematicsRenderPayload& Payload);
	static FString ComputeSnapshotFingerprint(
		const FHyperAIStudioCinematicsValueSnapshot& Snapshot);
	/** Pure value seal helpers used by the detached validator and automation fixtures. */
	static FString ComputePersistedRevisionForEvidence(
		const FString& PackageName, const FString& DiskExistence,
		const FString& PackageSavedHash, int64 DiskSize);
	static FString ComputeSequenceVolatileRevisionForValues(
		const FHyperAIStudioCinematicsValueSnapshot& Snapshot);
	static FString ComputeRenderConfigVolatileRevisionForValues(
		const FHyperAIStudioCinematicsValueSnapshot& Snapshot);
	static bool CaptureExact(
		const FString& SequencePath, const FString& RenderConfigPath,
		int32 MaxWorkMs, FHyperAIStudioCinematicsValueSnapshot& OutSnapshot,
		FString& OutStatus, FString& OutDiagnostic);
	static FHyperAICinematicsValidateReport ValidateSnapshot(
		const FHyperAIStudioCinematicsValueSnapshot& Snapshot,
		const FHyperAICinematicsValidateRequest& Request);
	static FHyperAICinematicsInspectReport Inspect(const FHyperAICinematicsInspectRequest& Request);
	static FHyperAICinematicsValidateReport Validate(const FHyperAICinematicsValidateRequest& Request);
	static FHyperAICinematicsApplyPlanReport BuildPlan(
		const FHyperAICinematicsApplyPlanRequest& Request);
};

class FHyperAIStudioCinematicsRegistration final
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
	TSharedPtr<FHyperAIStudioCinematicsDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
