// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioGASToolset.generated.h"

/** One bounded machine-readable GAS finding. */
USTRUCT(BlueprintType)
struct FHyperAIGASIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Explicit route for a GAS capability already owned by Epic's UE 5.8 toolsets. */
USTRUCT(BlueprintType)
struct FHyperAIGASEpicDelegate
{
	GENERATED_BODY()

	UPROPERTY()
	FString Capability;

	UPROPERTY()
	FString Toolset;

	UPROPERTY()
	FString Tool;

	/** read, edit, or external_effect. */
	UPROPERTY()
	FString Safety;

	UPROPERTY()
	FString Reason;
};

USTRUCT(BlueprintType)
struct FHyperAIGASModifierView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString AttributeOwnerClassPath;

	UPROPERTY()
	FString AttributeName;

	UPROPERTY()
	FString Operation;

	UPROPERTY()
	FString MagnitudeKind;

	UPROPERTY()
	bool bHasStaticMagnitude = false;

	UPROPERTY()
	double StaticMagnitude = 0.0;

	UPROPERTY()
	TArray<FString> SourceRequiredTags;

	UPROPERTY()
	TArray<FString> SourceBlockedTags;

	UPROPERTY()
	TArray<FString> TargetRequiredTags;

	UPROPERTY()
	TArray<FString> TargetBlockedTags;
};

USTRUCT(BlueprintType)
struct FHyperAIGASCueView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	TArray<FString> CueTags;

	UPROPERTY()
	double MinLevel = 0.0;

	UPROPERTY()
	double MaxLevel = 0.0;

	UPROPERTY()
	FString MagnitudeAttributeOwnerClassPath;

	UPROPERTY()
	FString MagnitudeAttributeName;
};

USTRUCT(BlueprintType)
struct FHyperAIGASAttributeView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString OwnerClassPath;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString PropertyType;

	UPROPERTY()
	bool bGameplayAttributeData = false;

	UPROPERTY()
	bool bHasDefaultValue = false;

	UPROPERTY()
	double DefaultValue = 0.0;
};

/** One immutable loaded-state projection. */
USTRUCT(BlueprintType)
struct FHyperAIGASAssetRecord
{
	GENERATED_BODY()

	/** gameplay_ability_blueprint, gameplay_effect_blueprint, or attribute_set[_blueprint]. */
	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString GeneratedClassPath;

	UPROPERTY()
	FString ParentClassPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	FString BlueprintStatus;

	UPROPERTY()
	bool bBlueprint = false;

	UPROPERTY()
	bool bPackageDirty = false;

	UPROPERTY()
	bool bRevisionComplete = false;

	/** True only when this projected revision includes every field used by supported edit CAS. */
	UPROPERTY()
	bool bMutationCasEligible = false;

	/** Ability-only values. */
	UPROPERTY()
	FString InstancingPolicy;

	UPROPERTY()
	FString ReplicationPolicy;

	UPROPERTY()
	FString NetExecutionPolicy;

	UPROPERTY()
	FString NetSecurityPolicy;

	UPROPERTY()
	FString CostEffectClassPath;

	UPROPERTY()
	FString CooldownEffectClassPath;

	/** GameplayEffect-only values. */
	UPROPERTY()
	FString DurationPolicy;

	UPROPERTY()
	bool bHasStaticDuration = false;

	UPROPERTY()
	double DurationSeconds = 0.0;

	UPROPERTY()
	bool bHasStaticMaxDuration = false;

	UPROPERTY()
	double MaxDurationSeconds = 0.0;

	UPROPERTY()
	double PeriodSeconds = 0.0;

	UPROPERTY()
	bool bExecutePeriodicEffectOnApplication = false;

	UPROPERTY()
	bool bRequireModifierSuccessToTriggerCues = false;

	UPROPERTY()
	bool bSuppressStackingCues = false;

	UPROPERTY()
	int32 ExecutionCount = 0;

	UPROPERTY()
	int32 ComponentCount = 0;

	UPROPERTY()
	TArray<FString> AssetTags;

	UPROPERTY()
	TArray<FString> GrantedTags;

	UPROPERTY()
	TArray<FString> BlockedAbilityTags;

	UPROPERTY()
	TArray<FHyperAIGASModifierView> Modifiers;

	UPROPERTY()
	TArray<FHyperAIGASCueView> Cues;

	/** AttributeSet-only values. */
	UPROPERTY()
	TArray<FHyperAIGASAttributeView> Attributes;
};

USTRUCT(BlueprintType)
struct FHyperAIGASInspectRequest
{
	GENERATED_BODY()

	/** Exact /Game Blueprint object paths. Empty means a bounded already-loaded enumeration. */
	UPROPERTY()
	TArray<FString> AssetPaths;

	/** all, gameplay_ability, gameplay_effect, or attribute_set. */
	UPROPERTY()
	FString Variant = TEXT("all");

	UPROPERTY()
	bool bIncludeTags = true;

	UPROPERTY()
	bool bIncludeModifiers = true;

	UPROPERTY()
	bool bIncludeCues = true;

	UPROPERTY()
	bool bIncludeAttributes = true;

	/** Include already-loaded native AttributeSet classes when AssetPaths is empty. */
	UPROPERTY()
	bool bIncludeNativeAttributeSets = false;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	/** Hard synchronous game-thread budget; exhausted capture returns partial evidence. */
	UPROPERTY()
	int32 MaxGameThreadMs = 100;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGASInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 AbilityCount = 0;

	UPROPERTY()
	int32 EffectCount = 0;

	UPROPERTY()
	int32 AttributeSetCount = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIGASAssetRecord> Records;

	UPROPERTY()
	TArray<FHyperAIGASIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGASEpicDelegate> EpicDelegates;
};

USTRUCT(BlueprintType)
struct FHyperAIGASValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> AssetPaths;

	UPROPERTY()
	FString ExpectedRevision;

	/** Must remain true: validation always performs a new loaded-state capture and never a disk load. */
	UPROPERTY()
	bool bRequireFreshCapture = true;

	UPROPERTY()
	bool bRequirePackagesClean = false;

	UPROPERTY()
	int32 MaxIssues = 128;

	/** Hard synchronous fresh-capture budget. */
	UPROPERTY()
	int32 MaxGameThreadMs = 100;
};

USTRUCT(BlueprintType)
struct FHyperAIGASValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	bool bFreshCapture = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("fresh_loaded_state");

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
	TArray<FHyperAIGASIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGASEpicDelegate> EpicDelegates;
};

/** Closed constant GameplayEffect modifier. No arbitrary class, calculation, or property write. */
USTRUCT(BlueprintType)
struct FHyperAIGASModifierSpec
{
	GENERATED_BODY()

	/** Exact loaded UAttributeSet class path. */
	UPROPERTY()
	FString AttributeOwnerClassPath;

	UPROPERTY()
	FString AttributeName;

	/** additive, multiplicative, division, or override. */
	UPROPERTY()
	FString Operation;

	UPROPERTY()
	double Magnitude = 0.0;
};

/** Closed cue reference. It can only reference already-registered GameplayCue tags. */
USTRUCT(BlueprintType)
struct FHyperAIGASCueSpec
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> CueTags;

	UPROPERTY()
	double MinLevel = 0.0;

	UPROPERTY()
	double MaxLevel = 0.0;

	/** Both empty means cue magnitude uses effect level. */
	UPROPERTY()
	FString MagnitudeAttributeOwnerClassPath;

	UPROPERTY()
	FString MagnitudeAttributeName;
};

/** One strict discriminated editor operation. Unused fields must retain defaults. */
USTRUCT(BlueprintType)
struct FHyperAIGASPlanOperation
{
	GENERATED_BODY()

	/**
	 * Typed-backend slice: set_effect_duration, replace_effect_modifiers,
	 * replace_effect_cues, set_effect_cue_flags, set_ability_asset_tags.
	 * Recognized but fail-closed until inheritance-safe component or creator/variable backends exist:
	 * replace_effect_asset_tags, replace_effect_granted_tags,
	 * create_gameplay_ability_blueprint, create_gameplay_effect_blueprint,
	 * create_attribute_set_blueprint, edit_attribute_set_blueprint.
	 */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString TargetPath;

	/** Required exact per-asset CAS for every currently supported operation. */
	UPROPERTY()
	FString ExpectedRevision;

	/** instant, infinite, or duration. */
	UPROPERTY()
	FString DurationPolicy;

	UPROPERTY()
	double DurationSeconds = -1.0;

	UPROPERTY()
	double MaxDurationSeconds = -1.0;

	UPROPERTY()
	double PeriodSeconds = -1.0;

	UPROPERTY()
	bool bSetExecutePeriodicEffectOnApplication = false;

	UPROPERTY()
	bool bExecutePeriodicEffectOnApplication = false;

	UPROPERTY()
	bool bSetRequireModifierSuccessToTriggerCues = false;

	UPROPERTY()
	bool bRequireModifierSuccessToTriggerCues = false;

	UPROPERTY()
	bool bSetSuppressStackingCues = false;

	UPROPERTY()
	bool bSuppressStackingCues = false;

	UPROPERTY()
	TArray<FString> Tags;

	UPROPERTY()
	TArray<FHyperAIGASModifierSpec> Modifiers;

	UPROPERTY()
	TArray<FHyperAIGASCueSpec> Cues;

	/** Used only by recognized creation variants; arbitrary class paths are never dispatched. */
	UPROPERTY()
	FString ParentClassPath;
};

USTRUCT(BlueprintType)
struct FHyperAIGASApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	int32 MaxGameThreadMs = 200;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;

	UPROPERTY()
	TArray<FHyperAIGASPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIGASPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 TargetCount = 0;

	UPROPERTY()
	int32 EffectsUpdated = 0;

	UPROPERTY()
	int32 AbilitiesUpdated = 0;

	UPROPERTY()
	int32 ModifiersReplaced = 0;

	UPROPERTY()
	int32 TagSetsReplaced = 0;

	UPROPERTY()
	int32 CueSetsReplaced = 0;

	UPROPERTY()
	bool bTransactionOnce = false;

	UPROPERTY()
	bool bCompileOnce = false;

	UPROPERTY()
	bool bSaveOnce = false;

	UPROPERTY()
	bool bValidateOnce = false;

	UPROPERTY()
	bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGASApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bStaged = false;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bFallbackPermitted = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

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
	FString StageId;

	UPROPERTY()
	int32 NativeOperationCount = 0;

	UPROPERTY()
	int32 GameThreadMs = 0;

	UPROPERTY()
	FHyperAIGASPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIGASIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGASEpicDelegate> EpicDelegates;
};

/** Exactly three names form the GAS pack's atomic cohort. */
UCLASS()
class HYPERAISTUDIOGAS_API UHyperAIStudioGASToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Bounded loaded-only editor projection; it never loads an asset or inspects a runtime ASC. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GAS")
	static FHyperAIGASInspectReport hyper_gas_inspect(const FHyperAIGASInspectRequest& Request);

	/** Strict dry-run planner. Execution remains closed until bounded compile/save/CAS exists. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GAS")
	static FHyperAIGASApplyPlanReport hyper_gas_apply_plan(const FHyperAIGASApplyPlanRequest& Request);

	/** Independent fresh loaded-state validator; runtime activation/effect application is excluded. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GAS")
	static FHyperAIGASValidateReport hyper_gas_validate(const FHyperAIGASValidateRequest& Request);
};

enum class EHyperAIStudioGASOperationKind : uint8
{
	SetEffectDuration,
	ReplaceEffectModifiers,
	ReplaceEffectAssetTags,
	ReplaceEffectGrantedTags,
	ReplaceEffectCues,
	SetEffectCueFlags,
	SetAbilityAssetTags,
	CreateGameplayAbilityBlueprint,
	CreateGameplayEffectBlueprint,
	CreateAttributeSetBlueprint,
	EditAttributeSetBlueprint
};

struct FHyperAIStudioGASBackendOperation
{
	EHyperAIStudioGASOperationKind Kind = EHyperAIStudioGASOperationKind::SetEffectDuration;
	FString TargetPath;
	FString ExpectedRevision;
	FString DurationPolicy;
	double DurationSeconds = -1.0;
	double MaxDurationSeconds = -1.0;
	double PeriodSeconds = -1.0;
	bool bSetExecutePeriodicEffectOnApplication = false;
	bool bExecutePeriodicEffectOnApplication = false;
	bool bSetRequireModifierSuccessToTriggerCues = false;
	bool bRequireModifierSuccessToTriggerCues = false;
	bool bSetSuppressStackingCues = false;
	bool bSuppressStackingCues = false;
	TArray<FString> Tags;
	TArray<FHyperAIGASModifierSpec> Modifiers;
	TArray<FHyperAIGASCueSpec> Cues;
	FString ParentClassPath;
	bool bBackendSupported = false;
};

struct FHyperAIStudioGASValueSnapshot
{
	FString Revision;
	bool bComplete = true;
	int32 LoadedObjectsScanned = 0;
	TArray<FHyperAIGASAssetRecord> Records;
	TArray<FHyperAIGASIssue> CaptureIssues;
};

/** Immutable, closed typed payload consumed by the shared executor. */
class FHyperAIStudioGASTypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FHyperAIStudioGASBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioGASResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/**
 * Sealed adapter descriptor for planning evidence. Execute is deliberately zero-effect until UE
 * exposes a bounded non-loading compile/save backend with exact runtime-side-effect CAS.
 */
class FHyperAIStudioGASDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioGASDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioGASManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioGASContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("gas");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiogastoolset.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("editor_defaults_patch.v1");
	static constexpr const TCHAR* ExecutionBlocker =
		TEXT("bounded_compile_or_runtime_cas_backend_required");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.gas.editor_defaults_patch.v1");
	static constexpr const TCHAR* PayloadSchemaCanonical = TEXT("hyperai.payload.gas.editor_defaults_patch.v1|base_revision:sha256|operations<=64:[kind,target_path,expected_revision,duration_policy,duration_seconds,max_duration_seconds,period_seconds,set_periodic,periodic,set_require_modifier_success,require_modifier_success,set_suppress_stacking,suppress_stacking,tags<=128,modifiers<=64:[attribute_owner,attribute_name,operation,magnitude],cues<=32:[tags<=16,min,max,magnitude_attribute_owner,magnitude_attribute_name],parent_class_path,backend_supported]|semantic_fingerprint:sha256");
	static constexpr const TCHAR* PayloadSchemaFingerprint = TEXT("sha256:080d1b570316a5595db19d089af6fde905b9ac0b2a0849b3f821f1a10cb3b987");
	static constexpr const TCHAR* ResultTypeId = TEXT("hyperai.result.gas.editor_defaults_patch.v1");
	static constexpr const TCHAR* ResultSchemaCanonical = TEXT("hyperai.result.gas.editor_defaults_patch.v1|phase:{apply,compile,validate,save,verify_fresh}|revision:sha256|valid:bool|error_count:int32|warning_count:int32");
	static constexpr const TCHAR* ResultSchemaFingerprint = TEXT("sha256:da065a3800a8bca9d67b9d5b07865aab5b15301ba722e78f83153e56222bb869");
	static constexpr int32 MaxAssetPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxTagCharacters = 256;
	static constexpr int32 MaxTagsPerSet = 128;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxModifiers = 64;
	static constexpr int32 MaxCues = 32;
	static constexpr int32 MaxCueTags = 16;
	static constexpr int32 MaxAttributes = 512;
	static constexpr int32 MaxRecords = 512;
	static constexpr int32 MaxLoadedObjectsScanned = 8192;
	static constexpr int32 MaxEffectComponentsScanned = 256;
	static constexpr int32 MaxReadGameThreadMs = 250;
	static constexpr int32 MaxProjectionElements = 16384;
	static constexpr int32 MaxProjectionBytes = 2 * 1024 * 1024;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioGASManifestEntry>& GetManifest();
	static const TArray<FHyperAIGASEpicDelegate>& GetEpicDelegates();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool ValidateOperationShape(
		const FHyperAIGASPlanOperation& Operation,
		FHyperAIStudioGASBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIStudioGASValueSnapshot& Snapshot);
	/** Internal loaded-only capture used by the trusted adapter and independent automation tests. */
	static FHyperAIStudioGASValueSnapshot CaptureLoadedSnapshot(
		const FHyperAIGASInspectRequest& Request);
	/** Exact CAS base over sorted target paths and their independent record revisions. */
	static FString ComputeBaseRevision(const TArray<FHyperAIGASAssetRecord>& Records);
	static TArray<FHyperAIGASIssue> ValidateValueSnapshot(
		const FHyperAIStudioGASValueSnapshot& Snapshot,
		bool bRequirePackagesClean,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static FHyperAIGASApplyPlanReport BuildPlan(const FHyperAIGASApplyPlanRequest& Request);
	static FString ComputePayloadSemanticFingerprint(
		const TArray<FHyperAIStudioGASBackendOperation>& Operations,
		const FString& BaseRevision);
};

class FHyperAIStudioGASRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
