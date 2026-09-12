// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioActorModifierToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIActorModifierIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierAuthorityRow
{
	GENERATED_BODY()

	UPROPERTY() FString Source;
	UPROPERTY() FString SourceId;
	UPROPERTY() FString Lifecycle;
	UPROPERTY() FString Disposition;
	UPROPERTY() FString HyperAIContract;
	UPROPERTY() FString RequiredPlugin;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("actor_modifier");
	UPROPERTY() FString State = TEXT("source_candidate_loaded_semantic_preflight");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bUsesEpicPublicSubsystem = true;
	UPROPERTY() bool bAllEpicMcpMatchesDelegated = true;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() int32 ReviewedEpicNativeCallableCount = 278;
	UPROPERTY() int32 ReviewedEpicPythonCallableCount = 598;
	UPROPERTY() int32 MatchingEpicCallableCount = 0;
	UPROPERTY() FString AuthorityFingerprint;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierEntry
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = -1;
	UPROPERTY() FString ModifierPath;
	UPROPERTY() FString ModifierName;
	UPROPERTY() FString ClassPath;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bApplied = false;
	UPROPERTY() bool bIdle = false;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierSnapshot
{
	GENERATED_BODY()

	UPROPERTY() FString ActorPath;
	UPROPERTY() FString ActorClassPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() bool bActorLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bHasStack = false;
	UPROPERTY() bool bStackFrozen = false;
	UPROPERTY() bool bPersistedProjectionComplete = false;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
	UPROPERTY() TArray<FHyperAIActorModifierEntry> Modifiers;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierInspectRequest
{
	GENERATED_BODY()

	/** Exact already-loaded actor object path. Empty returns capability evidence only. */
	UPROPERTY() FString ActorPath;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("one_exact_loaded_actor_direct_stack");
	UPROPERTY() FHyperAIActorModifierCapabilityStatus Capability;
	UPROPERTY() FHyperAIActorModifierSnapshot Snapshot;
	UPROPERTY() TArray<FHyperAIActorModifierIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierPlanOperation
{
	GENERATED_BODY()

	/** add, remove, move_before, move_after, or set_enabled. */
	UPROPERTY() FString Action;
	/** Existing exact modifier path for every action except add. */
	UPROPERTY() FString ModifierPath;
	/** Registered modifier name for add; empty otherwise. */
	UPROPERTY() FString ModifierName;
	/** Existing anchor path for move_before/move_after; empty otherwise. */
	UPROPERTY() FString AnchorPath;
	/** Desired state for add/set_enabled. */
	UPROPERTY() bool bEnabled = true;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString ActorPath;
	UPROPERTY() FString ExpectedPersistedFingerprint;
	UPROPERTY() TArray<FHyperAIActorModifierPlanOperation> Operations;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OperationCount = 0;
	UPROPERTY() bool bLoadedSemanticCasComplete = false;
	UPROPERTY() bool bWouldUseOneTransaction = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() int32 PhysicalEffectCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bEffectStarted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString SafetyClass = TEXT("edit");
	UPROPERTY() FString VariantId;
	UPROPERTY() FString BasePersistedFingerprint;
	UPROPERTY() FString OperationFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAIActorModifierPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIActorModifierIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FHyperAIActorModifierSnapshot Snapshot;
	UPROPERTY() int32 MaxIssues = 32;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIActorModifierValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() FString RecomputedPersistedFingerprint;
	UPROPERTY() FString RecomputedVolatileFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() TArray<FHyperAIActorModifierIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOACTORMODIFIER_API UHyperAIStudioActorModifierToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Actor Modifier")
	static FHyperAIActorModifierInspectReport hyper_actor_modifier_inspect(
		const FHyperAIActorModifierInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Actor Modifier")
	static FHyperAIActorModifierApplyPlanReport hyper_actor_modifier_apply_plan(
		const FHyperAIActorModifierApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Actor Modifier")
	static FHyperAIActorModifierValidateReport hyper_actor_modifier_validate(
		const FHyperAIActorModifierValidateRequest& Request);
};

class FHyperAIStudioActorModifierInspectPayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIActorModifierInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioActorModifierValidatePayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIActorModifierValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioActorModifierPlanPayload final
	: public IHyperAIStudioTypedArtifactPayload
{
public:
	FString ActorPath;
	FString BasePersistedFingerprint;
	TArray<FHyperAIActorModifierPlanOperation> Operations;
	FString OperationFingerprint;
	FString SemanticFingerprint;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioActorModifierInspectResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIActorModifierInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioActorModifierValidateResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIActorModifierValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioActorModifierDomainAdapter final
	: public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioActorModifierDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioActorModifierManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioActorModifierContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("actor_modifier_property_animation");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioactormodifiertoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("modifier_plugin");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_actor_modifier_stack.v1");
	static constexpr const TCHAR* ApplyVariantId = TEXT("actor_modifier_operation_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("detached_actor_modifier_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.actor-modifier.inspect.v1");
	static constexpr const TCHAR* ApplyPayloadTypeId = TEXT("hyperai.payload.actor-modifier.plan.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.actor-modifier.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.actor-modifier.inspect.v1");
	static constexpr const TCHAR* ApplyResultTypeId = TEXT("hyperai.result.actor-modifier.apply-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.actor-modifier.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_transaction_save_backend_required");
	static constexpr int32 MaxModifiers = 64;
	static constexpr int32 MaxOperations = 32;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 96;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MinOutputBytes = 8192;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxContainerBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioActorModifierManifestEntry>& GetManifest();
	static const TArray<FHyperAIActorModifierAuthorityRow>& GetAuthorityRows();
	static FHyperAIActorModifierCapabilityStatus GetCapabilityStatus();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsSafeActorPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString ComputePersistedFingerprint(const FHyperAIActorModifierSnapshot& Snapshot);
	static FString ComputeVolatileFingerprint(const FHyperAIActorModifierSnapshot& Snapshot);
	static bool ValidateOperations(const TArray<FHyperAIActorModifierPlanOperation>& Operations,
		FString& OutFingerprint, FString& OutError);
	static FString InspectPayloadSchemaFingerprint();
	static FString ApplyPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ApplyResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIActorModifierInspectReport Inspect(
		const FHyperAIActorModifierInspectRequest& Request);
	static FHyperAIActorModifierApplyPlanReport BuildPlan(
		const FHyperAIActorModifierApplyPlanRequest& Request);
	static FHyperAIActorModifierValidateReport Validate(
		const FHyperAIActorModifierValidateRequest& Request);
	static FString ComputePlanSemanticFingerprint(
		const FHyperAIStudioActorModifierPlanPayload& Payload);
};

class FHyperAIStudioActorModifierRegistration final
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
	TSharedPtr<FHyperAIStudioActorModifierDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
