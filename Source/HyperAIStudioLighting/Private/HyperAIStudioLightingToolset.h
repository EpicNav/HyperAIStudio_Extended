// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLightingToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAILightingProperty
{
	GENERATED_BODY()

	UPROPERTY() FString Key;
	UPROPERTY() FString Value;
};

/** One lighting actor in the level and the settings the apply ops can change. */
USTRUCT(BlueprintType)
struct FHyperAILightingActorRecord
{
	GENERATED_BODY()

	/** sun, sky_light, fog, sky_atmosphere or post_process. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Label;
	UPROPERTY() FString Path;
	UPROPERTY() TArray<FHyperAILightingProperty> Properties;
};

USTRUCT(BlueprintType)
struct FHyperAILightingInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILightingInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** The level open in the editor; apply_plan edits only this level. */
	UPROPERTY() FString LevelPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	/** False means auto exposure is on, so screenshots cannot be compared fairly; lock it first. */
	UPROPERTY() bool bExposureLocked = false;
	UPROPERTY() TArray<FHyperAILightingActorRecord> Actors;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Measurements of one image, in display-referred terms both a photo and a viewport capture share. */
USTRUCT(BlueprintType)
struct FHyperAILightingImageMetrics
{
	GENERATED_BODY()

	UPROPERTY() bool bValid = false;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	/** Linear luminance percentiles, 0-1. */
	UPROPERTY() float LuminanceP5 = 0.f;
	UPROPERTY() float LuminanceP50 = 0.f;
	UPROPERTY() float LuminanceP95 = 0.f;
	/** Brightest over darkest tones (p95 / p5): higher is harder, more contrasty light. */
	UPROPERTY() float ContrastRatio = 0.f;
	/** Correlated colour temperature of the average colour, in kelvin: lower is warmer. */
	UPROPERTY() float ColorTemperatureK = 0.f;
	UPROPERTY() float MeanSaturation = 0.f;
	/** Share of near-black pixels, and of clipped highlights. */
	UPROPERTY() float ShadowFraction = 0.f;
	UPROPERTY() float HighlightFraction = 0.f;
	/** 16 bins of perceptual brightness, summing to 1. */
	UPROPERTY() TArray<float> Histogram;
};

USTRUCT(BlueprintType)
struct FHyperAILightingCompareRequest
{
	GENERATED_BODY()

	/** PNG or JPG inside the project folder: the look to match. */
	UPROPERTY() FString ReferenceImagePath;
	/** Optional image to measure instead of the active level viewport. */
	UPROPERTY() FString CurrentImagePath;
	/** Brightness-distribution distance (0-1) at or below which the look counts as matched. */
	UPROPERTY() float MatchThreshold = 0.05f;
};

USTRUCT(BlueprintType)
struct FHyperAILightingCompareReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FHyperAILightingImageMetrics Reference;
	UPROPERTY() FHyperAILightingImageMetrics Current;
	/** Earth mover's distance between the two brightness histograms, 0 (same) to 1. */
	UPROPERTY() float HistogramDistance = 0.f;
	/** Exposure change that would bring the current median brightness to the reference's, in stops. */
	UPROPERTY() float ExposureDeltaEV = 0.f;
	/** Reference minus current colour temperature: negative means the reference is warmer. */
	UPROPERTY() float ColorTemperatureDeltaK = 0.f;
	UPROPERTY() float ContrastRatioDelta = 0.f;
	UPROPERTY() float SaturationDelta = 0.f;
	UPROPERTY() bool bMatched = false;
	/** False when auto exposure is on; lock_exposure first so comparisons measure the lighting, not the eye. */
	UPROPERTY() bool bExposureLocked = false;
	/** What to change next, strongest difference first. */
	UPROPERTY() TArray<FString> Suggestions;
	/** A PNG of what was measured, to view beside the reference. */
	UPROPERTY() FString CurrentImageWrittenPath;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One closed lighting edit. Kind and Property:
 *   set_sun: intensity (lux), temperature (K), color (r,g,b), pitch, yaw (degrees), source_angle
 *   set_sky_light: intensity, color
 *   set_fog: density, height_falloff, start_distance, inscattering_color
 *   set_sky_atmosphere: rayleigh_scale, mie_scale
 *   set_post_process: exposure_bias (EV), white_temp (K), saturation, contrast, gamma
 *   lock_exposure: value is the exposure bias in EV (default 0); unlock_exposure: no value
 * ActorLabel picks an actor; empty uses the first of its kind, adding one if the level has none.
 */
USTRUCT(BlueprintType)
struct FHyperAILightingOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString ActorLabel;
	UPROPERTY() FString Property;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAILightingApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	/** The revision hyper_lighting_inspect reported. */
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAILightingOp> Ops;
	/** Saves the changed lighting actors. Off by default so iterations stay unsaved until the look is right. */
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILightingApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Match a level's lighting to a reference image: inspect, lock exposure, compare, adjust, compare again. */
UCLASS()
class HYPERAISTUDIOLIGHTING_API UHyperAIStudioLightingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Sun, sky light, fog, sky atmosphere and post-process settings in the open level, whether exposure is locked, and the revision apply_plan needs. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingInspectReport hyper_lighting_inspect(const FHyperAILightingInspectRequest& Request);

	/**
	 * Closed lighting edits as one undo step: sun, sky light, fog, sky atmosphere, post process, and exposure lock.
	 * Dry-run, resubmit with operation_id and expected_plan_hash = plan_hash, poll hyper_operation_status, then
	 * hyper_lighting_compare. Unsaved by default; set bSave once the look matches.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingApplyPlanReport hyper_lighting_apply_plan(const FHyperAILightingApplyPlanRequest& Request);

	/**
	 * Measures the active level viewport (or current_image_path) against a reference image: brightness percentiles,
	 * contrast, colour temperature, saturation, histogram distance, and suggested next changes. Frame the view to
	 * match the reference first, and lock exposure, or brightness comparisons measure auto exposure, not lighting.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingCompareReport hyper_lighting_compare(const FHyperAILightingCompareRequest& Request);
};

class FHyperAIStudioLightingInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILightingInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingComparePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILightingCompareRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString LevelPath;
	FString BaseRevision;
	TArray<FHyperAILightingOp> Ops;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioLightingInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILightingInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingCompareResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILightingCompareReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioLightingFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};

class FHyperAIStudioLightingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("lighting_lookdev");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiolightingtoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.lighting_editor_world");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_lighting_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_lighting_apply_plan");
	static constexpr const TCHAR* CompareToolName = TEXT("hyper_lighting_compare");
	static constexpr const TCHAR* InspectVariantId = TEXT("lighting_actor_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("lighting_edit_ops.v1");
	static constexpr const TCHAR* CompareVariantId = TEXT("image_metrics_compare.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.lighting.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.lighting.edit_ops.v1");
	static constexpr const TCHAR* ComparePayloadTypeId = TEXT("hyperai.payload.lighting.compare.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.lighting.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.lighting.edit_ops.v1");
	static constexpr const TCHAR* CompareResultTypeId = TEXT("hyperai.result.lighting.compare.v1");
	static constexpr int32 MaxOpsPerPlan = 32;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ComparePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString CompareResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAILightingInspectReport Inspect(const FHyperAILightingInspectRequest& Request);
	static FHyperAILightingCompareReport Compare(const FHyperAILightingCompareRequest& Request);
	static FHyperAILightingApplyPlanReport BuildPlan(const FHyperAILightingApplyPlanRequest& Request);
	/** Order-sensitive: the same ops in a different order are a different plan. */
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioLightingEditOpsPayload& Payload);
};

class FHyperAIStudioLightingRegistration final
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
	TSharedPtr<FHyperAIStudioLightingDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
