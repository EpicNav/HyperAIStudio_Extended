// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioEnhancedInputToolset.generated.h"

/** Closed, allowlisted trigger/modifier configuration. Kind is a token, never a class name. */
USTRUCT(BlueprintType)
struct FHyperAIInputComponentSpec
{
	GENERATED_BODY()

	/**
	 * trigger.down|pressed|released|hold|hold_and_release|tap|repeated_tap|pulse, or
	 * modifier.dead_zone|scalar|negate|swizzle|smooth|smooth_delta|response_exponential|
	 * scale_by_delta_time|fov_scaling|to_world_space.
	 */
	UPROPERTY()
	FString Kind;

	/** Required for trigger variants; range [0, 1]. */
	UPROPERTY()
	double ActuationThreshold = -1.0;

	/** Hold/tap/pulse duration, depending on Kind. Unused fields must retain -1. */
	UPROPERTY()
	double DurationSeconds = -1.0;

	/** Repeat delay for trigger.repeated_tap. */
	UPROPERTY()
	double SecondarySeconds = -1.0;

	/** Tap count or pulse limit, depending on Kind. */
	UPROPERTY()
	int32 Count = -1;

	/** Dead-zone bounds. */
	UPROPERTY()
	double LowerThreshold = -1.0;

	UPROPERTY()
	double UpperThreshold = -1.0;

	/** Scalar/exponential vector. bHasVector distinguishes an explicit zero vector. */
	UPROPERTY()
	bool bHasVector = false;

	UPROPERTY()
	FVector Vector = FVector::ZeroVector;

	/** Negate axes. bHasAxes distinguishes an explicit all-false choice. */
	UPROPERTY()
	bool bHasAxes = false;

	UPROPERTY()
	bool bX = false;

	UPROPERTY()
	bool bY = false;

	UPROPERTY()
	bool bZ = false;

	/** One-shot/trigger-on-start. bHasOption distinguishes false from unused. */
	UPROPERTY()
	bool bHasOption = false;

	UPROPERTY()
	bool bOption = false;

	/** Closed per-kind enum (for example radial, yxz, or interp_to). */
	UPROPERTY()
	FString Mode;

	UPROPERTY()
	double Speed = -1.0;

	UPROPERTY()
	double Exponent = -1.0;

	UPROPERTY()
	double Scale = -1.0;
};

/** Read-only representation. Unsupported custom classes are reported but can never be dispatched. */
USTRUCT(BlueprintType)
struct FHyperAIInputComponentView
{
	GENERATED_BODY()

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	bool bSupported = false;

	UPROPERTY()
	FHyperAIInputComponentSpec Config;
};

USTRUCT(BlueprintType)
struct FHyperAIInputIssue
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
	FString Message;
};

/** One bounded typed record from an immutable loaded-only capture. */
USTRUCT(BlueprintType)
struct FHyperAIInputRecord
{
	GENERATED_BODY()

	/** action, mapping_context, mapping, profile_mapping, or runtime_binding. */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString ParentPath;

	UPROPERTY()
	FString ActionPath;

	UPROPERTY()
	FString Key;

	UPROPERTY()
	FString ValueType;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString ProfileId;

	UPROPERTY()
	FString MappingFingerprint;

	UPROPERTY()
	FString MappingName;

	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	FString DisplayCategory;

	UPROPERTY()
	FString MetadataPath;

	UPROPERTY()
	TArray<FString> SupportedProfiles;

	UPROPERTY()
	FString RegistrationTrackingMode;

	UPROPERTY()
	FString InputModeFilter;

	UPROPERTY()
	FString InputModeQueryDescription;

	UPROPERTY()
	int32 MappingIndex = -1;

	UPROPERTY()
	int32 MappingCount = 0;

	UPROPERTY()
	int32 ProfileCount = 0;

	UPROPERTY()
	int32 ReferenceCount = 0;

	UPROPERTY()
	int32 Priority = 0;

	UPROPERTY()
	int32 RegistrationCount = 0;

	UPROPERTY()
	bool bPlayerMappable = false;

	UPROPERTY()
	bool bPackageDirty = false;

	UPROPERTY()
	bool bTriggerWhenPaused = false;

	UPROPERTY()
	bool bConsumeInput = true;

	UPROPERTY()
	bool bReserveAllMappings = false;

	UPROPERTY()
	bool bConsumesLegacyMappings = false;

	UPROPERTY()
	int32 LegacyConsumeEvents = 0;

	UPROPERTY()
	FString AccumulationBehavior;

	UPROPERTY()
	TArray<FHyperAIInputComponentView> Triggers;

	UPROPERTY()
	TArray<FHyperAIInputComponentView> Modifiers;
};

USTRUCT(BlueprintType)
struct FHyperAIInputInspectRequest
{
	GENERATED_BODY()

	/** Exact object paths. Empty enumerates a bounded prefix of already-loaded Enhanced Input objects. */
	UPROPERTY()
	TArray<FString> AssetPaths;

	UPROPERTY()
	bool bIncludeMappings = true;

	UPROPERTY()
	bool bIncludeProfiles = true;

	UPROPERTY()
	bool bIncludeComponents = true;

	UPROPERTY()
	bool bIncludeReferenceEvidence = true;

	UPROPERTY()
	bool bIncludeRuntimeBindings = true;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIInputInspectReport
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
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 ActionCount = 0;

	UPROPERTY()
	int32 ContextCount = 0;

	UPROPERTY()
	int32 MappingCount = 0;

	UPROPERTY()
	int32 ProfileMappingCount = 0;

	UPROPERTY()
	int32 RuntimeBindingCount = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIInputRecord> Records;

	UPROPERTY()
	TArray<FHyperAIInputIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIInputValidateRequest
{
	GENERATED_BODY()

	/** Exact paths, or empty for a bounded loaded-only project view. */
	UPROPERTY()
	TArray<FString> AssetPaths;

	UPROPERTY()
	bool bIncludeProfiles = true;

	UPROPERTY()
	bool bIncludeRuntimeBindings = true;

	UPROPERTY()
	int32 MaxIssues = 128;
};

USTRUCT(BlueprintType)
struct FHyperAIInputValidateReport
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
	FString Scope = TEXT("loaded_only");

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
	TArray<FHyperAIInputIssue> Issues;
};

/** Closed discriminated operation. Fields not owned by Type must retain their defaults. */
USTRUCT(BlueprintType)
struct FHyperAIInputPlanOperation
{
	GENERATED_BODY()

	/**
	 * create_action, create_context, set_action_config, add_action_trigger, add_action_modifier,
	 * add_mapping, append_mapping_trigger, append_mapping_modifier, replace_mapping_key,
	 * remove_mapping, delete_asset, or set_runtime_priority.
	 */
	UPROPERTY()
	FString Type;

	/** Exact target object path; creation is restricted to canonical /Game object paths. */
	UPROPERTY()
	FString TargetPath;

	/** Required CAS for an existing target; empty only when the target is created in this plan. */
	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FString ActionPath;

	UPROPERTY()
	FString Key;

	/** default mappings only. Profile override editing is not public in UE 5.8 and is rejected. */
	UPROPERTY()
	FString ProfileId;

	UPROPERTY()
	int32 MappingIndex = -1;

	UPROPERTY()
	FString ExpectedMappingFingerprint;

	/** boolean, axis1d, axis2d, axis3d, or empty for unchanged. */
	UPROPERTY()
	FString ValueType;

	UPROPERTY()
	bool bSetTriggerWhenPaused = false;

	UPROPERTY()
	bool bTriggerWhenPaused = false;

	UPROPERTY()
	bool bSetConsumeInput = false;

	UPROPERTY()
	bool bConsumeInput = true;

	UPROPERTY()
	bool bSetReserveAllMappings = false;

	UPROPERTY()
	bool bReserveAllMappings = false;

	UPROPERTY()
	bool bSetAccumulationBehavior = false;

	/** highest_absolute or cumulative. */
	UPROPERTY()
	FString AccumulationBehavior;

	/** Used only by set_runtime_priority; exact already-loaded UEnhancedPlayerInput path. */
	UPROPERTY()
	FString RuntimeOwnerPath;

	UPROPERTY()
	int32 Priority = 0;

	UPROPERTY()
	TArray<FHyperAIInputComponentSpec> Triggers;

	UPROPERTY()
	TArray<FHyperAIInputComponentSpec> Modifiers;
};

USTRUCT(BlueprintType)
struct FHyperAIInputApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Required for non-dry-run staging; excluded from semantic hashes. */
	UPROPERTY()
	FString OperationId;

	/** Echo the exact dry-run plan hash before staging. */
	UPROPERTY()
	FString ExpectedPlanHash;

	/** Opaque server-issued grant. Required only for destructive/external cohorts. */
	UPROPERTY()
	FString AuthorizationToken;

	UPROPERTY()
	TArray<FHyperAIInputPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIInputPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 ActionsCreated = 0;

	UPROPERTY()
	int32 ContextsCreated = 0;

	UPROPERTY()
	int32 ActionsUpdated = 0;

	UPROPERTY()
	int32 MappingsAdded = 0;

	UPROPERTY()
	int32 MappingsUpdated = 0;

	UPROPERTY()
	int32 MappingsRemoved = 0;

	UPROPERTY()
	int32 AssetsDeleted = 0;

	UPROPERTY()
	int32 RuntimePrioritiesChanged = 0;

	UPROPERTY()
	bool bTransactionOnce = false;

	UPROPERTY()
	bool bSaveOnce = false;

	UPROPERTY()
	bool bValidateOnce = false;

	UPROPERTY()
	bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIInputApplyPlanReport
{
	GENERATED_BODY()

	/** True only for a valid dry-run or completed admitted execution; staging alone remains false. */
	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bStaged = false;

	/** False until the central PlanExecutionService admits this typed backend operation. */
	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bRequiresJournalExecution = true;

	UPROPERTY()
	bool bRequiresTrustedAuthorization = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass;

	UPROPERTY()
	FString BaseRevision;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString TypedOperationType;

	UPROPERTY()
	FHyperAIInputPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIInputIssue> Issues;
};

/** All three names form one exact atomic admission cohort. */
UCLASS()
class HYPERAISTUDIOENHANCEDINPUT_API UHyperAIStudioEnhancedInputToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|EnhancedInput")
	static FHyperAIInputInspectReport hyper_input_inspect(const FHyperAIInputInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|EnhancedInput")
	static FHyperAIInputApplyPlanReport hyper_input_apply_plan(const FHyperAIInputApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|EnhancedInput")
	static FHyperAIInputValidateReport hyper_input_validate(const FHyperAIInputValidateRequest& Request);
};

enum class EHyperAIStudioEnhancedInputSafety : uint8
{
	Edit,
	Destructive,
	ExternalEffect
};

enum class EHyperAIStudioEnhancedInputOperationKind : uint8
{
	CreateAction,
	CreateContext,
	SetActionConfig,
	AddActionTrigger,
	AddActionModifier,
	AddMapping,
	AppendMappingTrigger,
	AppendMappingModifier,
	ReplaceMappingKey,
	RemoveMapping,
	DeleteAsset,
	SetRuntimePriority
};

enum class EHyperAIStudioEnhancedInputComponentKind : uint8
{
	TriggerDown,
	TriggerPressed,
	TriggerReleased,
	TriggerHold,
	TriggerHoldAndRelease,
	TriggerTap,
	TriggerRepeatedTap,
	TriggerPulse,
	ModifierDeadZone,
	ModifierScalar,
	ModifierNegate,
	ModifierSwizzle,
	ModifierSmooth,
	ModifierSmoothDelta,
	ModifierResponseExponential,
	ModifierScaleByDeltaTime,
	ModifierFovScaling,
	ModifierToWorldSpace
};

enum class EHyperAIStudioEnhancedInputValueType : uint8
{
	Boolean,
	Axis1D,
	Axis2D,
	Axis3D
};

struct FHyperAIStudioEnhancedInputBackendComponent
{
	EHyperAIStudioEnhancedInputComponentKind Kind = EHyperAIStudioEnhancedInputComponentKind::TriggerDown;
	FHyperAIInputComponentSpec Config;
};

/** Exact typed operation consumed only by a future central executor backend, never directly from MCP JSON. */
struct FHyperAIStudioEnhancedInputBackendOperation
{
	EHyperAIStudioEnhancedInputOperationKind Kind = EHyperAIStudioEnhancedInputOperationKind::CreateAction;
	EHyperAIStudioEnhancedInputSafety Safety = EHyperAIStudioEnhancedInputSafety::Edit;
	FString TargetPath;
	FString ExpectedRevision;
	FString ActionPath;
	FName Key;
	int32 MappingIndex = -1;
	FString ExpectedMappingFingerprint;
	EHyperAIStudioEnhancedInputValueType ValueType = EHyperAIStudioEnhancedInputValueType::Boolean;
	bool bHasValueType = false;
	bool bSetTriggerWhenPaused = false;
	bool bTriggerWhenPaused = false;
	bool bSetConsumeInput = false;
	bool bConsumeInput = true;
	bool bSetReserveAllMappings = false;
	bool bReserveAllMappings = false;
	bool bSetAccumulationBehavior = false;
	bool bCumulative = false;
	FString RuntimeOwnerPath;
	int32 Priority = 0;
	TArray<FHyperAIStudioEnhancedInputBackendComponent> Triggers;
	TArray<FHyperAIStudioEnhancedInputBackendComponent> Modifiers;
};

/** Server-owned, short-lived artifact. It intentionally contains no bearer token or executable code. */
struct FHyperAIStudioStagedEnhancedInputPlanArtifact
{
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString EffectFingerprint;
	FString BaseRevision;
	FString TypedOperationType;
	EHyperAIStudioEnhancedInputSafety Safety = EHyperAIStudioEnhancedInputSafety::Edit;
	int64 ExpiresUtcMs = 0;
	bool bRequiresSaveOnce = false;
	bool bRequiresValidateOnce = true;
	bool bRequiresFreshVerifyOnce = true;
	TArray<FHyperAIStudioEnhancedInputBackendOperation> Operations;
};

/**
 * Explicit seam for PlanExecutionService. Stage is side-effect free and idempotent; ClaimExact is
 * reserved for a journal/auth/transaction/save/verify backend and removes the exact artifact.
 * The current shared PlanExecutionService exposes only Blueprint-specific typed staging, so this
 * source candidate never claims execution until core exports a generic domain-artifact submit seam.
 */
class FHyperAIStudioEnhancedInputPlanStagingService final
{
public:
	static bool Stage(
		const FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact,
		bool& bOutReplay,
		FString& OutError);
	static bool ClaimExact(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		FHyperAIStudioStagedEnhancedInputPlanArtifact& OutArtifact,
		FString& OutError);
	static void Shutdown();
};

struct FHyperAIStudioEnhancedInputManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

/** Internal immutable values make validation deterministic and synthetic-testable. */
struct FHyperAIStudioEnhancedInputMappingValue
{
	FString ContextPath;
	FString ProfileId;
	int32 MappingIndex = -1;
	FString ActionPath;
	FString Key;
	FString ActionValueType;
	FString MappingFingerprint;
	bool bKeyValid = false;
	bool bRevisionComplete = true;
	TArray<FHyperAIInputComponentSpec> Triggers;
	TArray<FHyperAIInputComponentSpec> Modifiers;
};

struct FHyperAIStudioEnhancedInputValueSnapshot
{
	FString Revision;
	bool bComplete = true;
	int32 LoadedObjectsScanned = 0;
	TSet<FString> ActionPaths;
	TSet<FString> ContextPaths;
	TMap<FString, FString> ActionValueTypes;
	TArray<FHyperAIStudioEnhancedInputMappingValue> Mappings;
	TArray<FHyperAIInputRecord> Records;
	TArray<FHyperAIInputIssue> CaptureIssues;
};

class FHyperAIStudioEnhancedInputContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("enhanced_input");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1");
	static constexpr const TCHAR* EditOperationType = TEXT("enhanced_input.apply_edit_plan");
	static constexpr const TCHAR* DestructiveOperationType = TEXT("enhanced_input.apply_destructive_plan");
	static constexpr const TCHAR* ExternalOperationType = TEXT("enhanced_input.apply_external_plan");
	static constexpr int32 MaxAssetPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxProfileCharacters = 64;
	static constexpr int32 MaxOperations = 128;
	static constexpr int32 MaxComponentsPerOwner = 16;
	static constexpr int32 MaxMappings = 2048;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxLoadedObjectsScanned = 8192;
	static constexpr int32 MaxRuntimeOwnersScanned = 256;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxAuthorizationTokenCharacters = 256;
	static constexpr int64 ArtifactLifetimeMs = 4ll * 60ll * 1000ll;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioEnhancedInputManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool ValidateComponentSpec(
		const FHyperAIInputComponentSpec& Spec,
		bool bExpectTrigger,
		FHyperAIStudioEnhancedInputBackendComponent& OutComponent,
		FString& OutError);
	static bool ValidateOperationShape(
		const FHyperAIInputPlanOperation& Operation,
		FHyperAIStudioEnhancedInputBackendOperation& OutOperation,
		FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIStudioEnhancedInputValueSnapshot& Snapshot);
	static TArray<FHyperAIInputIssue> ValidateValueSnapshot(
		const FHyperAIStudioEnhancedInputValueSnapshot& Snapshot,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static FHyperAIInputApplyPlanReport BuildPlan(const FHyperAIInputApplyPlanRequest& Request);
	/** Pure envelope gate used by BuildPlan and automation tests; a token is opaque, never a client boolean. */
	static bool ValidateAuthorizationEnvelope(
		EHyperAIStudioEnhancedInputSafety Safety,
		bool bDryRun,
		const FString& AuthorizationToken,
		FString& OutError);
	/** Test/executor helper which binds the exact typed artifact to deterministic plan/effect hashes. */
	static bool FinalizeArtifactFingerprints(
		FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact,
		FString& OutError);
	static const TCHAR* SafetyToString(EHyperAIStudioEnhancedInputSafety Safety);
	static const TCHAR* TypedOperationType(EHyperAIStudioEnhancedInputSafety Safety);
};

class FHyperAIStudioEnhancedInputRegistration final
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
