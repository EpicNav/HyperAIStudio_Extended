// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioNiagaraToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAINiagaraIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString RuleClass;
	UPROPERTY() FString SourcePath;
	UPROPERTY() FString Summary;
	UPROPERTY() FString Description;
	/** Fix ids hyper_niagara_apply_plan can run through an apply_stack_issue_fix op. */
	UPROPERTY() TArray<FString> FixIds;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("niagara_system");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValidationImplemented = false;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State = TEXT("source_candidate_async_host_required");
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraUserParameterIdentity
{
	GENERATED_BODY()

	UPROPERTY() FString VariableGuid;
	UPROPERTY() FString Name;
	UPROPERTY() FString TypeId;
	UPROPERTY() FString TypeFingerprint;
	UPROPERTY() FString DefaultFingerprint;
	UPROPERTY() FString MetadataChangeId;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraHealthRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() bool bExistsOnDisk = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString AssetGuid;
	UPROPERTY() bool bCompileStateKnown = false;
	UPROPERTY() bool bCompileActive = false;
	UPROPERTY() bool bCompileStale = false;
	UPROPERTY() bool bCompileHasErrors = false;
	UPROPERTY() bool bCompileHasWarnings = false;
	UPROPERTY() bool bSystemValid = false;
	UPROPERTY() bool bReadyToRun = false;
	UPROPERTY() bool bExistingSystemViewModel = false;
	UPROPERTY() int32 EmitterCount = 0;
	UPROPERTY() int32 ParameterCount = 0;
	UPROPERTY() int32 ScriptCount = 0;
	UPROPERTY() int32 GraphCount = 0;
	UPROPERTY() int32 GraphNodeCount = 0;
	/** The System's effect type (scalability and budgets), or empty when it has none. */
	UPROPERTY() FString EffectTypePath;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraKeyValue
{
	GENERATED_BODY()

	UPROPERTY() FString Key;
	UPROPERTY() FString Value;
};

/** An effect type, data channel or sim cache, as inspect reports it when target_path is not a System. */
USTRUCT(BlueprintType)
struct FHyperAINiagaraAssetRecord
{
	GENERATED_BODY()

	/** effect_type, data_channel or sim_cache. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Path;
	UPROPERTY() TArray<FHyperAINiagaraKeyValue> Details;
};

/** A simulation capture and, when a golden cache was given, how it compares. */
USTRUCT(BlueprintType)
struct FHyperAINiagaraSimCacheReport
{
	GENERATED_BODY()

	UPROPERTY() FString CaptureId;
	/** capturing, complete or failed. */
	UPROPERTY() FString State;
	UPROPERTY() int32 FramesRequested = 0;
	UPROPERTY() int32 FramesCaptured = 0;
	/** Captures of a non-deterministic System can differ run to run. */
	UPROPERTY() bool bDeterministic = false;
	UPROPERTY() FString Error;
	UPROPERTY() TArray<FString> EmitterNames;
	/** Particles alive per emitter on the last captured frame, in EmitterNames order. */
	UPROPERTY() TArray<int32> LastFrameParticleCounts;
	UPROPERTY() bool bCompared = false;
	UPROPERTY() bool bMatch = false;
	UPROPERTY() FString GoldenPath;
	UPROPERTY() float Tolerance = 0.f;
	/** First differences the comparison found, one per line. */
	UPROPERTY() TArray<FString> Differences;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraModuleTopology
{
	GENERATED_BODY()

	/** Use as module_name in an edit op. */
	UPROPERTY() FString ModuleName;
	UPROPERTY() bool bEnabled = true;
	UPROPERTY() bool bIsSetParametersModule = false;
	UPROPERTY() FString ScriptAssetPath;
	/** Top-level input names, usable as the first input_name_stack entry of a set_input_value op. */
	UPROPERTY() TArray<FString> InputNames;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraScriptStackTopology
{
	GENERATED_BODY()

	/** Use as script_name in an edit op. */
	UPROPERTY() FString ScriptName;
	UPROPERTY() TArray<FHyperAINiagaraModuleTopology> Modules;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraRendererTopology
{
	GENERATED_BODY()

	UPROPERTY() int32 RendererIndex = INDEX_NONE;
	UPROPERTY() FString RendererClassPath;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraEmitterTopology
{
	GENERATED_BODY()

	/** Use as emitter_name in an edit op. */
	UPROPERTY() FString EmitterName;
	UPROPERTY() bool bEnabled = true;
	/** CPUSim or GPUComputeSim. */
	UPROPERTY() FString SimTarget;
	UPROPERTY() TArray<FHyperAINiagaraScriptStackTopology> ScriptStacks;
	UPROPERTY() TArray<FHyperAINiagaraRendererTopology> Renderers;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraInspectRequest
{
	GENERATED_BODY()

	/** One exact canonical loaded /Game object path; never a folder, wildcard, or load request. */
	UPROPERTY() FString TargetPath;
	/** Also return emitters, script stacks, modules, inputs and renderers: the addresses edit ops use. */
	UPROPERTY() bool bIncludeTopology = false;
	UPROPERTY() int32 PageSize = 32;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAINiagaraHealthRecord Health;
	UPROPERTY() TArray<FHyperAINiagaraUserParameterIdentity> UserParameters;
	/** Present only when bIncludeTopology was requested. */
	UPROPERTY() TArray<FHyperAINiagaraEmitterTopology> Emitters;
	/** Filled instead of Health when target_path is an effect type, data channel or sim cache. */
	UPROPERTY() FHyperAINiagaraAssetRecord Asset;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	/** Optional exact revision assertion from inspect. */
	UPROPERTY() FString ExpectedRevision;
	/**
	 * authoring fails on errors; runtime_ready also fails on warnings or an incomplete compile.
	 * sim_cache_capture records the System's simulation frame by frame: call once to start (returns capture_id),
	 * then again with capture_id until complete; with golden_sim_cache_path it compares against that cache.
	 */
	UPROPERTY() FString Policy = TEXT("authoring");
	/** sim_cache_capture: the id a previous call returned. */
	UPROPERTY() FString CaptureId;
	UPROPERTY() int32 CaptureFrames = 60;
	UPROPERTY() float CaptureDeltaSeconds = 1.f / 60.f;
	/** sim_cache_capture: a baked cache to compare the finished capture against. */
	UPROPERTY() FString GoldenSimCachePath;
	UPROPERTY() float FloatTolerance = 0.01f;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraValidateReport
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
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() int32 InfoCount = 0;
	UPROPERTY() bool bCompileStateKnown = false;
	UPROPERTY() bool bCompiling = false;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
	/** policy sim_cache_capture only. */
	UPROPERTY() FHyperAINiagaraSimCacheReport SimCache;
};

/** One closed edit. Addresses come from hyper_niagara_inspect with bIncludeTopology; unused fields stay empty. */
USTRUCT(BlueprintType)
struct FHyperAINiagaraEditOp
{
	GENERATED_BODY()

	/**
	 * System edits: set_module_enabled | add_module | add_renderer | add_emitter | set_input_value | apply_stack_issue_fix.
	 * Assets saved with the System: set_effect_type (asset_path, empty clears) | create_effect_type (asset_path) |
	 * set_effect_type_setting (asset_path, name, value) | create_data_channel (asset_path, value_type
	 * global|islands|gameplay_burst, value "Name:type,...") | bake_sim_cache (name = capture_id, asset_path).
	 */
	UPROPERTY() FString Kind;
	/** Required for emitter scripts and renderers; empty for System* scripts. */
	UPROPERTY() FString EmitterName;
	/** SystemSpawnScript | SystemUpdateScript | EmitterSpawnScript | EmitterUpdateScript | ParticleSpawnScript | ParticleUpdateScript */
	UPROPERTY() FString ScriptName;
	UPROPERTY() FString ModuleName;
	/** set_input_value: the module input, then any nested dynamic-input names. */
	UPROPERTY() TArray<FString> InputNameStack;
	/** add_module: module script object path. add_emitter: emitter template path. add_renderer: renderer class path. */
	UPROPERTY() FString AssetPath;
	/** add_emitter: the new emitter's name. */
	UPROPERTY() FString Name;
	/** set_input_value: float | int32 | bool | vector | color */
	UPROPERTY() FString ValueType;
	/** set_input_value: 1.5 | 3 | true | 1,2,3 | 1,0.5,0,1 */
	UPROPERTY() FString Value;
	/** set_module_enabled */
	UPROPERTY() bool bEnabled = true;
	/** apply_stack_issue_fix: ids from hyper_niagara_validate. */
	UPROPERTY() FString IssueId;
	UPROPERTY() FString FixId;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	/** Non-dry only: a fresh durable operation id, later passed to hyper_operation_status. */
	UPROPERTY() FString OperationId;
	/** Non-dry only: the plan_hash the dry run returned, so the reviewed plan is the one that runs. */
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAINiagaraEditOp> Ops;
	UPROPERTY() bool bCompile = true;
	/** Compiling requires saving: the shared executor has no compile-only finalizer. */
	UPROPERTY() bool bSave = true;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 OpCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bWouldTransactionOnce = false;
	UPROPERTY() bool bWouldCompileOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAINiagaraPlanEffects Effects;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Niagara atomic cohort. */
UCLASS()
class HYPERAISTUDIONIAGARA_API UHyperAIStudioNiagaraToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("2.0.0"); }

	/** Health, revision and user parameters of a loaded Niagara System. Set bIncludeTopology for the emitter, script, module and input names that hyper_niagara_apply_plan ops address. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraInspectReport hyper_niagara_inspect(
		const FHyperAINiagaraInspectRequest& Request);

	/** Batched Niagara edits as one undo step with revision check, compile, save and fresh verify. Dry-run, resubmit with operation_id and plan_hash, poll hyper_operation_status, then hyper_niagara_validate. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraApplyPlanReport hyper_niagara_apply_plan(
		const FHyperAINiagaraApplyPlanRequest& Request);

	/** Compile state and Niagara stack issues for a loaded System, including fix ids usable by apply_stack_issue_fix ops. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraValidateReport hyper_niagara_validate(
		const FHyperAINiagaraValidateRequest& Request);
};

struct FHyperAIStudioNiagaraValueSnapshot
{
	FHyperAINiagaraHealthRecord Health;
	TArray<FHyperAINiagaraUserParameterIdentity> Parameters;
	TArray<FHyperAINiagaraIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioNiagaraInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAINiagaraInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAINiagaraValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BaseRevision;
	TArray<FHyperAINiagaraEditOp> Ops;
	bool bCompile = true;
	bool bSave = true;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioNiagaraInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAINiagaraInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAINiagaraValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioNiagaraDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

class FHyperAIStudioNiagaraFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	explicit FHyperAIStudioNiagaraFreshVerifier(FString InOwnerAdapterFingerprint);
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

private:
	FString OwnerAdapterFingerprint;
};

struct FHyperAIStudioNiagaraManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioNiagaraContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("niagara_vfx");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioniagaratoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("niagara_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("niagara_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.niagara_editor");
	static constexpr const TCHAR* InspectVariantId = TEXT("exact_health_cas.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("external_edit_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("external_compile_and_stack_issues.v1");
	static constexpr const TCHAR* InspectPayloadTypeId =
		TEXT("hyperai.payload.niagara.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId =
		TEXT("hyperai.payload.niagara.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId =
		TEXT("hyperai.payload.niagara.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId =
		TEXT("hyperai.result.niagara.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId =
		TEXT("hyperai.result.niagara.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId =
		TEXT("hyperai.result.niagara.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 16;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxPageSize = 64;
	static constexpr int32 MaxEmitters = 128;
	static constexpr int32 MaxParameters = 256;
	static constexpr int32 MaxScripts = 512;
	static constexpr int32 MaxGraphs = 256;
	static constexpr int32 MaxGraphNodes = 4096;
	static constexpr int32 MaxGraphPins = 16384;
	static constexpr int32 MaxGraphMetadata = 8192;
	static constexpr int32 MaxEmitterVersions = 256;
	static constexpr int32 MaxRenderers = 512;
	static constexpr int32 MaxSimulationStages = 512;
	static constexpr int32 MaxStackEntries = 2048;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioNiagaraManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FHyperAINiagaraCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsUserParameterName(const FString& Name);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool CaptureExact(
		const FString& TargetPath,
		int32 MaxWorkMs,
		FHyperAIStudioNiagaraValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FHyperAINiagaraInspectReport Inspect(const FHyperAINiagaraInspectRequest& Request);
	static FHyperAINiagaraValidateReport Validate(const FHyperAINiagaraValidateRequest& Request);
	static FHyperAINiagaraApplyPlanReport BuildPlan(const FHyperAINiagaraApplyPlanRequest& Request);
	/** Order-sensitive: the same ops in a different order are a different plan. */
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioNiagaraEditOpsPayload& Payload);
};

class FHyperAIStudioNiagaraRegistration final
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
	TSharedPtr<FHyperAIStudioNiagaraDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	/** One publication at startup goes stale after ProbeFreshnessMs and would block every mutation. */
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
