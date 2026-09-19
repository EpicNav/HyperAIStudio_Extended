// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAnimBlueprintToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintVariable
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** bool, float, or other for types this toolset does not author. */
	UPROPERTY() FString Type;
	/** What sets it every frame: speed, is_falling or is_crouching; empty when nothing this toolset added does. */
	UPROPERTY() FString Driver;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintState
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** sequence, blend_space, empty, or custom for graphs this toolset did not build. */
	UPROPERTY() FString Player;
	UPROPERTY() FString Asset;
	UPROPERTY() FString XVariable;
	UPROPERTY() FString YVariable;
	UPROPERTY() bool bLooping = true;
	UPROPERTY() bool bEntry = false;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintTransition
{
	GENERATED_BODY()

	UPROPERTY() FString From;
	UPROPERTY() FString To;
	/** The rule as written in the plan; "(custom)" for rules built by hand, "(none)" when it never fires. */
	UPROPERTY() FString Rule;
	UPROPERTY() float BlendTime = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintMachine
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** True when its pose feeds the AnimGraph output. */
	UPROPERTY() bool bDrivesOutput = false;
	UPROPERTY() TArray<FHyperAIAnimBlueprintState> States;
	UPROPERTY() TArray<FHyperAIAnimBlueprintTransition> Transitions;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintInspectRequest
{
	GENERATED_BODY()

	/** /Game object path of an Animation Blueprint, e.g. /Game/Characters/ABP_Hero.ABP_Hero. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() bool bLoad = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString TargetPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	UPROPERTY() FString Skeleton;
	UPROPERTY() FString ParentClass;
	/** up_to_date, warnings, dirty or error. */
	UPROPERTY() FString CompileStatus;
	UPROPERTY() bool bOpenInEditor = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() TArray<FHyperAIAnimBlueprintVariable> Variables;
	UPROPERTY() TArray<FHyperAIAnimBlueprintMachine> Machines;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One closed Animation Blueprint edit. Names are identifiers (letters, digits, _; up to 64).
 *   add_variable: Name, Value = bool or float
 *   bind_variable: Name, Value = speed (float), is_falling or is_crouching (bool); added if missing, set every frame
 *   add_state_machine: Name; drives the output pose when nothing else does, or when Value = output
 *   add_state: Machine, Name, optional Asset (sequence or blend space) with XVariable/YVariable for blend spaces,
 *              Value = once to play a sequence once instead of looping
 *   set_state_asset: Machine, Name, Asset, XVariable, YVariable, Value as add_state
 *   set_entry_state: Machine, Name
 *   add_transition: Machine, Name (from), To, Rule, Value = blend seconds (default 0.2).
 *       Rule: conditions joined by &&, each Var, !Var, or Var > n (>, <, >=, <=); or alone, time_remaining < n
 *   remove_transition: Machine, Name (from), To
 *   remove_state: Machine, Name, with its transitions
 */
USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Machine;
	UPROPERTY() FString Name;
	UPROPERTY() FString To;
	UPROPERTY() FString Asset;
	UPROPERTY() FString Rule;
	UPROPERTY() FString Value;
	UPROPERTY() FString XVariable;
	UPROPERTY() FString YVariable;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	/** Create the Animation Blueprint for SkeletonPath (parent ParentClassPath, default AnimInstance). */
	UPROPERTY() bool bCreate = false;
	UPROPERTY() FString SkeletonPath;
	UPROPERTY() FString ParentClassPath;
	/** From hyper_anim_blueprint_inspect; empty when creating. */
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step, then compiled. */
	UPROPERTY() TArray<FHyperAIAnimBlueprintOp> Ops;
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintApplyPlanReport
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
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimBlueprintValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString CompileStatus;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIAnimBlueprintIssue> Issues;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Animation Blueprint graphs: variables and what drives them, state machines, states, transitions and their rules. */
UCLASS()
class HYPERAISTUDIOANIMBLUEPRINT_API UHyperAIStudioAnimBlueprintToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** An Animation Blueprint's skeleton, variables (and their drivers), state machines with states, players, transitions and rules, compile status, and revision. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|AnimBlueprint")
	static FHyperAIAnimBlueprintInspectReport hyper_anim_blueprint_inspect(const FHyperAIAnimBlueprintInspectRequest& Request);

	/**
	 * Closed graph edits, then a compile, as one undo step: create for a skeleton, variables and per-frame drivers,
	 * state machines, states playing sequences or blend spaces, transitions with rules. Read
	 * .hyperai/docs/HyperAIStudio-AnimationCookbook.md first. Dry-run, resubmit, poll, then validate.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|AnimBlueprint")
	static FHyperAIAnimBlueprintApplyPlanReport hyper_anim_blueprint_apply_plan(const FHyperAIAnimBlueprintApplyPlanRequest& Request);

	/** Compile status plus graph checks: output pose wired, entry states, unreachable or dead-end states, rules without a gap between thresholds, instant blends, skeleton mismatches. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|AnimBlueprint")
	static FHyperAIAnimBlueprintValidateReport hyper_anim_blueprint_validate(const FHyperAIAnimBlueprintValidateRequest& Request);
};

class FHyperAIStudioAnimBlueprintInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIAnimBlueprintInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimBlueprintValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIAnimBlueprintValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimBlueprintEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BaseRevision;
	bool bCreate = false;
	FString SkeletonPath;
	FString ParentClassPath;
	TArray<FHyperAIAnimBlueprintOp> Ops;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioAnimBlueprintInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIAnimBlueprintInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimBlueprintValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIAnimBlueprintValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimBlueprintMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimBlueprintDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioAnimBlueprintFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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

class FHyperAIStudioAnimBlueprintContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("anim_blueprint");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioanimblueprinttoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.anim_blueprint_graph");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_anim_blueprint_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_anim_blueprint_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_anim_blueprint_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("anim_graph_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("anim_graph_edit_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("anim_graph_checks.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.anim_blueprint.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.anim_blueprint.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.anim_blueprint.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.anim_blueprint.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.anim_blueprint.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.anim_blueprint.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 48;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIAnimBlueprintInspectReport Inspect(const FHyperAIAnimBlueprintInspectRequest& Request);
	static FHyperAIAnimBlueprintValidateReport Validate(const FHyperAIAnimBlueprintValidateRequest& Request);
	static FHyperAIAnimBlueprintApplyPlanReport BuildPlan(const FHyperAIAnimBlueprintApplyPlanRequest& Request);
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioAnimBlueprintEditOpsPayload& Payload);
};

class FHyperAIStudioAnimBlueprintRegistration final
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
	TSharedPtr<FHyperAIStudioAnimBlueprintDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
