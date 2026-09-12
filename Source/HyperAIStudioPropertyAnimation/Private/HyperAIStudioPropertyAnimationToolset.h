// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPropertyAnimationToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationIssue
{
	GENERATED_BODY()
	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationAuthorityRow
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
struct FHyperAIPropertyAnimationCapabilityStatus
{
	GENERATED_BODY()
	UPROPERTY() FString Family = TEXT("property_animation");
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
struct FHyperAIPropertyAnimationContextEntry
{
	GENERATED_BODY()
	UPROPERTY() FString LocatorPath;
	UPROPERTY() FString DisplayName;
	UPROPERTY() bool bResolved = false;
	UPROPERTY() bool bAnimated = false;
	UPROPERTY() double Magnitude = 1.0;
	UPROPERTY() double TimeOffset = 0.0;
	UPROPERTY() FString Mode = TEXT("absolute");
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationAnimatorEntry
{
	GENERATED_BODY()
	UPROPERTY() int32 Index = -1;
	UPROPERTY() FString AnimatorPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString DisplayName;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bOverrideTimeSource = false;
	UPROPERTY() FString TimeSourceName;
	UPROPERTY() TArray<FHyperAIPropertyAnimationContextEntry> Contexts;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationSnapshot
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
	UPROPERTY() bool bPersistedProjectionComplete = false;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
	UPROPERTY() TArray<FHyperAIPropertyAnimationAnimatorEntry> Animators;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationInspectRequest
{
	GENERATED_BODY()
	/** Exact already-loaded actor object path. Empty returns capability evidence only. */
	UPROPERTY() FString ActorPath;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationInspectReport
{
	GENERATED_BODY()
	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("one_exact_loaded_actor_property_animators");
	UPROPERTY() FHyperAIPropertyAnimationCapabilityStatus Capability;
	UPROPERTY() FHyperAIPropertyAnimationSnapshot Snapshot;
	UPROPERTY() TArray<FHyperAIPropertyAnimationIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationPlanOperation
{
	GENERATED_BODY()
	/** create, remove, link, unlink, set_animator_enabled, or set_context. */
	UPROPERTY() FString Action;
	/** Existing exact animator path except for create. */
	UPROPERTY() FString AnimatorPath;
	/** Registered animator class path for create; empty otherwise. */
	UPROPERTY() FString AnimatorClassPath;
	/** Closed property locator for link/unlink/set_context; empty otherwise. */
	UPROPERTY() FString PropertyLocator;
	UPROPERTY() bool bEnabled = true;
	UPROPERTY() double Magnitude = 1.0;
	UPROPERTY() double TimeOffset = 0.0;
	/** absolute or additive for set_context. */
	UPROPERTY() FString Mode = TEXT("absolute");
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationApplyPlanRequest
{
	GENERATED_BODY()
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString ActorPath;
	UPROPERTY() FString ExpectedPersistedFingerprint;
	UPROPERTY() TArray<FHyperAIPropertyAnimationPlanOperation> Operations;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationPlanEffects
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
struct FHyperAIPropertyAnimationApplyPlanReport
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
	UPROPERTY() FHyperAIPropertyAnimationPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIPropertyAnimationIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationValidateRequest
{
	GENERATED_BODY()
	UPROPERTY() FHyperAIPropertyAnimationSnapshot Snapshot;
	UPROPERTY() int32 MaxIssues = 32;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIPropertyAnimationValidateReport
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
	UPROPERTY() TArray<FHyperAIPropertyAnimationIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOPROPERTYANIMATION_API UHyperAIStudioPropertyAnimationToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()
public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Property Animation")
	static FHyperAIPropertyAnimationInspectReport hyper_property_animation_inspect(
		const FHyperAIPropertyAnimationInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Property Animation")
	static FHyperAIPropertyAnimationApplyPlanReport hyper_property_animation_apply_plan(
		const FHyperAIPropertyAnimationApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Property Animation")
	static FHyperAIPropertyAnimationValidateReport hyper_property_animation_validate(
		const FHyperAIPropertyAnimationValidateRequest& Request);
};

class FHyperAIStudioPropertyAnimationInspectPayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPropertyAnimationInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPropertyAnimationValidatePayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIPropertyAnimationValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPropertyAnimationPlanPayload final
	: public IHyperAIStudioTypedArtifactPayload
{
public:
	FString ActorPath;
	FString BasePersistedFingerprint;
	TArray<FHyperAIPropertyAnimationPlanOperation> Operations;
	FString OperationFingerprint;
	FString SemanticFingerprint;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioPropertyAnimationInspectResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPropertyAnimationInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPropertyAnimationValidateResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIPropertyAnimationValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioPropertyAnimationDomainAdapter final
	: public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioPropertyAnimationDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioPropertyAnimationManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioPropertyAnimationContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("actor_modifier_property_animation");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiopropertyanimationtoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("modifier_plugin");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_property_animator_state.v1");
	static constexpr const TCHAR* ApplyVariantId = TEXT("property_animator_operation_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("detached_property_animator_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.property-animation.inspect.v1");
	static constexpr const TCHAR* ApplyPayloadTypeId = TEXT("hyperai.payload.property-animation.plan.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.property-animation.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.property-animation.inspect.v1");
	static constexpr const TCHAR* ApplyResultTypeId = TEXT("hyperai.result.property-animation.apply-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.property-animation.validate.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_transaction_save_backend_required");
	static constexpr int32 MaxAnimators = 32;
	static constexpr int32 MaxContextsPerAnimator = 32;
	static constexpr int32 MaxTotalContexts = 64;
	static constexpr int32 MaxOperations = 32;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MinOutputBytes = 8192;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxContainerBytes = 256 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioPropertyAnimationManifestEntry>& GetManifest();
	static const TArray<FHyperAIPropertyAnimationAuthorityRow>& GetAuthorityRows();
	static FHyperAIPropertyAnimationCapabilityStatus GetCapabilityStatus();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsSafeActorPath(const FString& Path);
	static bool IsSafeLocator(const FString& Locator);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString ComputePersistedFingerprint(const FHyperAIPropertyAnimationSnapshot& Snapshot);
	static FString ComputeVolatileFingerprint(const FHyperAIPropertyAnimationSnapshot& Snapshot);
	static bool ValidateOperations(const TArray<FHyperAIPropertyAnimationPlanOperation>& Operations,
		FString& OutFingerprint, FString& OutError);
	static FString InspectPayloadSchemaFingerprint();
	static FString ApplyPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ApplyResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIPropertyAnimationInspectReport Inspect(
		const FHyperAIPropertyAnimationInspectRequest& Request);
	static FHyperAIPropertyAnimationApplyPlanReport BuildPlan(
		const FHyperAIPropertyAnimationApplyPlanRequest& Request);
	static FHyperAIPropertyAnimationValidateReport Validate(
		const FHyperAIPropertyAnimationValidateRequest& Request);
	static FString ComputePlanSemanticFingerprint(
		const FHyperAIStudioPropertyAnimationPlanPayload& Payload);
};

class FHyperAIStudioPropertyAnimationRegistration final
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
	TSharedPtr<FHyperAIStudioPropertyAnimationDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
