// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioGameplayAIToolset.generated.h"

/** One bounded, stable Gameplay AI finding. */
USTRUCT(BlueprintType)
struct FHyperAIGameplayAIIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Exact prerequisite/implementation state for one closed variant. */
USTRUCT(BlueprintType)
struct FHyperAIGameplayAIVariantStatus
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	TArray<FString> RequiredPlugins;

	UPROPERTY()
	TArray<FString> RequiredModules;

	UPROPERTY()
	bool bPluginsInstalled = false;

	UPROPERTY()
	bool bPluginsEnabled = false;

	UPROPERTY()
	bool bModulesLoaded = false;

	UPROPERTY()
	bool bInspectImplemented = false;

	UPROPERTY()
	bool bApplyImplemented = false;

	/** False in this source-candidate slice: apply can only dry-run and stage. */
	UPROPERTY()
	bool bApplyBackendExecutable = false;

	/** Exact closed operations/evidence exposed by this variant. */
	UPROPERTY()
	TArray<FString> SupportedCases;

	/** Explicitly unsupported cases; consumers must not infer a reflection fallback. */
	UPROPERTY()
	TArray<FString> UnsupportedCases;

	/** ready, staged_backend_required, inspect_ready_apply_unavailable, missing_plugin,
	 * plugin_disabled, enabled_not_loaded, or adapter_unavailable. */
	UPROPERTY()
	FString State;

	UPROPERTY()
	FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAINodeView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	/** root, composite, composite_decorator, task, decorator, service, comment, or unknown. */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString GraphNodeClassPath;

	UPROPERTY()
	FString RuntimeNodeClassPath;

	UPROPERTY()
	FString Error;

	UPROPERTY()
	bool bInjected = false;

	UPROPERTY()
	bool bBreakpointPresent = false;

	UPROPERTY()
	bool bBreakpointEnabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIKeyView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString KeyType;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString Category;

	UPROPERTY()
	bool bInstanceSynced = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAISenseView
{
	GENERATED_BODY()

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ConfigClassPath;

	UPROPERTY()
	FString SenseClassPath;

	UPROPERTY()
	FString SenseName;

	UPROPERTY()
	double MaxAgeSeconds = 0.0;

	UPROPERTY()
	bool bStartsEnabled = false;

	UPROPERTY()
	bool bDominant = false;
};

/** One immutable, loaded-only projection. */
USTRUCT(BlueprintType)
struct FHyperAIGameplayAIAssetRecord
{
	GENERATED_BODY()

	/** behavior_tree, blackboard, or ai_controller_perception. */
	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	bool bPackageDirty = false;

	/** Output-only signal: nested details were omitted to keep a page bounded; Revision still seals the full capture. */
	UPROPERTY()
	bool bOutputDetailsTruncated = false;

	UPROPERTY()
	FString BlueprintStatus;

	UPROPERTY()
	FString GeneratedClassPath;

	UPROPERTY()
	FString ParentClassPath;

	/** Behavior Tree blackboard or AI Controller default blackboard. */
	UPROPERTY()
	FString BlackboardPath;

	/** Behavior Tree editor-root blackboard; compared independently with the runtime asset link. */
	UPROPERTY()
	FString BehaviorTreeGraphBlackboardPath;

	/** Blackboard parent. */
	UPROPERTY()
	FString ParentAssetPath;

	UPROPERTY()
	FString DominantSenseClassPath;

	UPROPERTY()
	int32 RootDecoratorCount = 0;

	UPROPERTY()
	TArray<FHyperAIGameplayAINodeView> Nodes;

	UPROPERTY()
	TArray<FHyperAIGameplayAIKeyView> Keys;

	UPROPERTY()
	TArray<FHyperAIGameplayAISenseView> Senses;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIInspectRequest
{
	GENERATED_BODY()

	/** Exact /Game object paths. Empty enumerates a bounded set of already-loaded objects. */
	UPROPERTY()
	TArray<FString> AssetPaths;

	/** all plus one exact variant from the prerequisite matrix. */
	UPROPERTY()
	FString Variant = TEXT("all");

	UPROPERTY()
	bool bIncludeDetails = true;

	UPROPERTY()
	bool bIncludePrerequisites = true;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIInspectReport
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
	int32 BehaviorTreeCount = 0;

	UPROPERTY()
	int32 BlackboardCount = 0;

	UPROPERTY()
	int32 AIControllerCount = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIGameplayAIAssetRecord> Records;

	UPROPERTY()
	TArray<FHyperAIGameplayAIIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGameplayAIVariantStatus> Variants;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FString> AssetPaths;

	UPROPERTY()
	FString Variant = TEXT("all");

	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	bool bRequirePackagesClean = false;

	UPROPERTY()
	bool bIncludePrerequisites = true;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIValidateReport
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
	TArray<FHyperAIGameplayAIIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGameplayAIVariantStatus> Variants;
};

/** One strict discriminated editor operation. Unused fields must retain defaults. */
USTRUCT(BlueprintType)
struct FHyperAIGameplayAIPlanOperation
{
	GENERATED_BODY()

	/** set_behavior_tree_blackboard, add_blackboard_key, remove_blackboard_key,
	 * rename_blackboard_key, or set_blackboard_parent. Optional variants are recognized
	 * only to return their exact prerequisite/adapter-unavailable state. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString TargetPath;

	/** Exact per-target CAS from inspect/validate. */
	UPROPERTY()
	FString ExpectedRevision;

	/** Key name, or old key name for rename. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString NewName;

	/** bool, int, float, name, string, vector, or rotator. */
	UPROPERTY()
	FString KeyType;

	/** Loaded Blackboard path; empty clears a BehaviorTree blackboard or Blackboard parent. */
	UPROPERTY()
	FString ReferencePath;

	UPROPERTY()
	bool bInstanceSynced = false;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Required only for staging. Edit plans intentionally have no authorization-token field. */
	UPROPERTY()
	FString OperationId;

	/** Echo the exact dry-run plan hash before idempotent staging. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	int32 DeadlineMs = 2000;

	UPROPERTY()
	int32 MaxGameThreadMs = 250;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;

	UPROPERTY()
	TArray<FHyperAIGameplayAIPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIGameplayAIPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 TargetCount = 0;

	UPROPERTY()
	int32 BehaviorTreesUpdated = 0;

	UPROPERTY()
	int32 BlackboardsUpdated = 0;

	UPROPERTY()
	int32 KeysAdded = 0;

	UPROPERTY()
	int32 KeysRemoved = 0;

	UPROPERTY()
	int32 KeysRenamed = 0;

	UPROPERTY()
	bool bTransactionOnce = false;

	/** Behavior Tree link/update phase; Blackboard-only plans leave this false. */
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
struct FHyperAIGameplayAIApplyPlanReport
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
	FHyperAIGameplayAIPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIGameplayAIIssue> Issues;

	UPROPERTY()
	TArray<FHyperAIGameplayAIVariantStatus> Variants;
};

/** Exactly three names form the Gameplay AI pack's atomic cohort. */
UCLASS()
class HYPERAISTUDIOGAMEPLAYAI_API UHyperAIStudioGameplayAIToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIGameplayAIInspectReport hyper_gameplay_ai_inspect(
		const FHyperAIGameplayAIInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIGameplayAIApplyPlanReport hyper_gameplay_ai_apply_plan(
		const FHyperAIGameplayAIApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIGameplayAIValidateReport hyper_gameplay_ai_validate(
		const FHyperAIGameplayAIValidateRequest& Request);
};

enum class EHyperAIStudioGameplayAIOperationKind : uint8
{
	SetBehaviorTreeBlackboard,
	AddBlackboardKey,
	RemoveBlackboardKey,
	RenameBlackboardKey,
	SetBlackboardParent
};

struct FHyperAIStudioGameplayAIBackendOperation
{
	EHyperAIStudioGameplayAIOperationKind Kind =
		EHyperAIStudioGameplayAIOperationKind::SetBehaviorTreeBlackboard;
	FString TargetPath;
	FString ExpectedRevision;
	/** Remains an owning string until the admitted backend, avoiding untrusted FName allocation. */
	FString Name;
	FString NewName;
	FString KeyType;
	FString ReferencePath;
	bool bInstanceSynced = false;
};

struct FHyperAIStudioGameplayAIValueSnapshot
{
	FString ScopeFingerprint;
	FString Revision;
	bool bComplete = true;
	int32 LoadedObjectsScanned = 0;
	TArray<FHyperAIGameplayAIAssetRecord> Records;
	TArray<FHyperAIGameplayAIIssue> CaptureIssues;
};

/** Immutable, closed typed payload consumed by the shared executor once async hosting exists. */
class FHyperAIStudioGameplayAITypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FHyperAIStudioGameplayAIBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioGameplayAIResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Base adapter descriptor/pin. Execute fails before effect until async PlanExecutionService owns it. */
class FHyperAIStudioGameplayAIDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioGameplayAIDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioGameplayAIVariantDescriptor
{
	FString Variant;
	TArray<FString> RequiredPlugins;
	TArray<FString> RequiredModules;
	bool bInspectImplemented = false;
	bool bApplyImplemented = false;
	bool bApplyBackendExecutable = false;
	TArray<FString> SupportedCases;
	TArray<FString> UnsupportedCases;
};

/** Clean-room facade. Optional families attach as exact typed variants, never reflection fallback. */
class FHyperAIStudioGameplayAIFacade final
{
public:
	static const TArray<FHyperAIStudioGameplayAIVariantDescriptor>& GetVariantDescriptors();
	static TArray<FHyperAIGameplayAIVariantStatus> ResolveVariantStatuses();
	static bool CaptureLoaded(
		const TArray<FString>& AssetPaths,
		const FString& Variant,
		bool bIncludeDetails,
		FHyperAIStudioGameplayAIValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
};

struct FHyperAIStudioGameplayAIStagedArtifact
{
	FHyperAIStudioPreparedTypedArtifact Prepared;
	/** Concrete closed DTO: bespoke staging never accepts a caller-defined payload subtype. */
	TSharedPtr<const FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe> Payload;
	FString CanonicalProjectId;
	FString OperationId;
	FString StageId;
	int64 ExpiresMonotonicMs = 0;
};

/** Bounded, side-effect-free staging only. It deliberately exports no execution route. */
class FHyperAIStudioGameplayAIStagingService final
{
public:
	static bool Stage(
		const FHyperAIStudioGameplayAIStagedArtifact& Artifact,
		bool& bOutReplay,
		FString& OutError);
	static int32 NumStaged();
	static void Reset();
};

struct FHyperAIStudioGameplayAIManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioGameplayAIContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("gameplay_ai");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiogameplayaitoolset.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("base_editor_assets.v1");
	static constexpr const TCHAR* PayloadTypeId =
		TEXT("hyperai.payload.gameplay_ai.base_editor_assets.v1");
	static constexpr const TCHAR* ResultTypeId =
		TEXT("hyperai.result.gameplay_ai.base_editor_assets.v1");
	static constexpr int32 MaxAssetPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxNodesPerTree = 2048;
	static constexpr int32 MaxKeysPerBlackboard = 512;
	static constexpr int32 MaxSensesPerController = 32;
	static constexpr int32 MaxLoadedObjectsScanned = 8192;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioGameplayAIManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString PayloadSchemaFingerprint();
	static FString ResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetBaseAdapterDescriptor();
	static bool ValidateOperationShape(
		const FHyperAIGameplayAIPlanOperation& Operation,
		FHyperAIStudioGameplayAIBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIStudioGameplayAIValueSnapshot& Snapshot);
	static TArray<FHyperAIGameplayAIIssue> ValidateValueSnapshot(
		const FHyperAIStudioGameplayAIValueSnapshot& Snapshot,
		bool bRequirePackagesClean,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static FString ComputePayloadSemanticFingerprint(
		const TArray<FHyperAIStudioGameplayAIBackendOperation>& Operations,
		const FString& BaseRevision);
	static FString ComputeStageId(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		const FString& EffectFingerprint);
	static FHyperAIGameplayAIApplyPlanReport BuildPlan(
		const FHyperAIGameplayAIApplyPlanRequest& Request);
};

class FHyperAIStudioGameplayAIRegistration final
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
