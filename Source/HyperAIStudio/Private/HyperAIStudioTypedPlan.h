// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

enum class EHyperAIStudioPlanSafety : uint8
{
	Read,
	Edit,
	Destructive,
	ExternalEffect
};

enum class EHyperAIStudioPlanValueType : uint8
{
	String,
	Integer,
	Boolean,
	StringArray
};

enum class EHyperAIStudioPlanPreconditionKind : uint8
{
	ObjectExists,
	ObjectAbsent,
	RevisionEquals,
	PropertyEquals,
	PluginAvailable,
	EditorStateEquals
};

enum class EHyperAIStudioPlanEffectKind : uint8
{
	ObjectCreated,
	ObjectUpdated,
	ObjectDeleted,
	RuntimeExternalEffect
};

enum class EHyperAIStudioPlanActionKind : uint8
{
	Step,
	CompileOnce,
	ValidateOnce,
	SaveOnce,
	VerifyFreshOnce
};

enum class EHyperAIStudioPlanRollbackState : uint8
{
	NotNeeded,
	Complete,
	Partial,
	Unsupported,
	Unknown
};

/** Hard parser and execution-envelope ceilings. Callers may request smaller budgets only. */
struct FHyperAIStudioPlanLimits
{
	static constexpr int32 MaxPlanJsonBytes = 256 * 1024;
	static constexpr int32 MaxJsonDepth = 64;
	static constexpr int32 MaxJsonNodes = 32768;
	static constexpr int32 MaxSteps = 128;
	static constexpr int32 MaxDependenciesPerStep = 32;
	static constexpr int32 MaxTotalDependencies = 512;
	static constexpr int32 MaxArgumentsPerStep = 32;
	static constexpr int32 MaxPreconditionsPerStep = 32;
	static constexpr int32 MaxEffectsPerStep = 32;
	static constexpr int32 MaxTotalPreconditions = 512;
	static constexpr int32 MaxTotalEffects = 512;
	static constexpr int32 MaxDiagnostics = 64;
	static constexpr int32 MaxStepIdChars = 64;
	static constexpr int32 MaxTargetChars = 1024;
	static constexpr int32 MaxFieldChars = 128;
	static constexpr int32 MaxValidatorIdChars = 128;
	static constexpr int32 MaxArgumentStringChars = 4096;
	static constexpr int32 MaxArgumentArrayItems = 128;
	/** Largest integer that JSON's IEEE-754 number representation preserves exactly. */
	static constexpr int64 MaxExactJsonInteger = 9007199254740991LL;
	static constexpr int32 MaxDeadlineMs = 300000;
	static constexpr int32 MaxMutationSteps = 128;
	static constexpr int32 MaxNativeOperations = 4096;
	static constexpr int32 MaxGameThreadMs = 30000;
	static constexpr int32 MaxOutputBytes = 4 * 1024 * 1024;
	static constexpr int32 MaxAuthorizationTokenChars = 256;
	static constexpr int32 MaxAuthorizationNonceChars = 128;
	static constexpr int32 MaxValidatorReceiptTokenChars = 512;
	static constexpr int32 MaxActionNonceChars = 128;
	static constexpr int32 MaxCanonicalProjectIdChars = 128;
};

struct FHyperAIStudioPlanPreconditionTargetBinding
{
	EHyperAIStudioPlanPreconditionKind Kind = EHyperAIStudioPlanPreconditionKind::ObjectExists;
	FString ArgumentName;
	FString FixedTarget;
};

struct FHyperAIStudioPlanEffectTargetBinding
{
	EHyperAIStudioPlanEffectKind Kind = EHyperAIStudioPlanEffectKind::ObjectUpdated;
	FString ArgumentName;
	FString FixedTarget;
};

struct FHyperAIStudioPlanArgumentSpec
{
	FString Name;
	EHyperAIStudioPlanValueType Type = EHyperAIStudioPlanValueType::String;
	bool bRequired = false;
	int32 MaxStringChars = 0;
	int32 MaxArrayItems = 0;
	int64 MinInteger = 0;
	int64 MaxInteger = 0;
	TArray<FString> AllowedStrings;
};

/**
 * Clean-room metadata for one closed native operation variant.
 * It contains no raw Epic tool name and no executable callback or script body.
 */
struct FHyperAIStudioTypedOperationMetadata
{
	FString TypeId;
	EHyperAIStudioPlanSafety Safety = EHyperAIStudioPlanSafety::Read;
	TArray<FHyperAIStudioPlanArgumentSpec> Arguments;
	TArray<EHyperAIStudioPlanPreconditionKind> AllowedPreconditions;
	TArray<EHyperAIStudioPlanPreconditionKind> RequiredPreconditions;
	TArray<EHyperAIStudioPlanEffectKind> AllowedEffects;
	TArray<EHyperAIStudioPlanEffectKind> RequiredEffects;
	TArray<FHyperAIStudioPlanPreconditionTargetBinding> PreconditionTargetBindings;
	TArray<FHyperAIStudioPlanEffectTargetBinding> EffectTargetBindings;
	TArray<FString> Prerequisites;
	FString PostconditionValidatorId;
	int32 EstimatedNativeOperations = 1;
	int32 EstimatedGameThreadMs = 1;
	bool bCompileOnce = false;
	bool bSaveOnce = false;
	bool bValidateOnce = false;
	bool bSupportsDryRun = true;
	bool bVerifyFreshOnce = false;
};

/** Explicit allowlist used by a domain tool to admit typed operation variants. */
class FHyperAIStudioTypedOperationRegistry
{
public:
	bool Add(const FHyperAIStudioTypedOperationMetadata& Metadata, FString& OutError);
	const FHyperAIStudioTypedOperationMetadata* Find(const FString& TypeId) const;
	const TArray<FHyperAIStudioTypedOperationMetadata>& GetOperations() const { return Operations; }
	FString ComputeCapabilityHash() const;

	/** Small first-party fixture registry for the shared engine and its contract tests. */
	static FHyperAIStudioTypedOperationRegistry CreateFoundationRegistry();

private:
	TArray<FHyperAIStudioTypedOperationMetadata> Operations;
};

struct FHyperAIStudioPlanValue
{
	EHyperAIStudioPlanValueType Type = EHyperAIStudioPlanValueType::String;
	FString StringValue;
	int64 IntegerValue = 0;
	bool bBooleanValue = false;
	TArray<FString> StringArrayValue;
};

struct FHyperAIStudioPlanPrecondition
{
	EHyperAIStudioPlanPreconditionKind Kind = EHyperAIStudioPlanPreconditionKind::ObjectExists;
	FString Target;
	FString Field;
	FString Expected;
};

struct FHyperAIStudioPlanEffect
{
	EHyperAIStudioPlanEffectKind Kind = EHyperAIStudioPlanEffectKind::ObjectUpdated;
	FString Target;
	FString ValidatorId;
	FString Expected;
};

struct FHyperAIStudioPlanBudget
{
	int32 DeadlineMs = 0;
	int32 MaxSteps = 0;
	int32 MaxMutations = 0;
	int32 MaxNativeOperations = 0;
	int32 MaxGameThreadMs = 0;
	int32 MaxOutputBytes = 0;
};

struct FHyperAIStudioPlanStepBudget
{
	int32 MaxNativeOperations = 0;
	int32 MaxGameThreadMs = 0;
	int32 MaxOutputBytes = 0;
};

struct FHyperAIStudioPlanStep
{
	FString StepId;
	FString OperationType;
	EHyperAIStudioPlanSafety Safety = EHyperAIStudioPlanSafety::Read;
	TArray<FString> DependsOn;
	TMap<FString, FHyperAIStudioPlanValue> Arguments;
	TArray<FHyperAIStudioPlanPrecondition> Preconditions;
	TArray<FHyperAIStudioPlanEffect> Effects;
	FHyperAIStudioPlanStepBudget Budget;
	bool bCompileOnce = false;
	bool bSaveOnce = false;
	bool bValidateOnce = false;
	bool bVerifyFreshOnce = false;
};

/**
 * Game-thread budget each finalizer action gets per target. General typed plans keep these defaults; typed-artifact
 * mutations raise them to measured editor-asset costs (saving a package alone takes far longer than 10 ms).
 */
struct FHyperAIStudioPlanFinalizerBudgets
{
	int32 CompileGameThreadMs = 20;
	int32 ValidateGameThreadMs = 5;
	int32 SaveGameThreadMs = 10;
	int32 VerifyFreshGameThreadMs = 5;

	bool IsDefault() const
	{
		const FHyperAIStudioPlanFinalizerBudgets Default;
		return CompileGameThreadMs == Default.CompileGameThreadMs
			&& ValidateGameThreadMs == Default.ValidateGameThreadMs
			&& SaveGameThreadMs == Default.SaveGameThreadMs
			&& VerifyFreshGameThreadMs == Default.VerifyFreshGameThreadMs;
	}
};

struct FHyperAIStudioValidatedPlan
{
	static constexpr const TCHAR* SchemaVersion = TEXT("hyperai.plan.v1");

	bool bValidated = false;
	bool bDryRun = true;
	bool bHasMutation = false;
	bool bHasDestructive = false;
	bool bHasExternalEffect = false;
	FString OperationId;
	FString CapabilityHash;
	FString ExpectedPlanHash;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString EffectFingerprint;
	FString AuthorizationToken;
	EHyperAIStudioPlanSafety MaximumSafety = EHyperAIStudioPlanSafety::Read;
	FHyperAIStudioPlanBudget Budget;
	FHyperAIStudioPlanFinalizerBudgets FinalizerBudgets;
	TArray<FHyperAIStudioPlanStep> Steps;
	TArray<int32> OrderedStepIndices;
};

struct FHyperAIStudioPlanDiagnostic
{
	FString Code;
	FString Path;
	FString Message;
};

struct FHyperAIStudioPlanScheduledAction
{
	EHyperAIStudioPlanActionKind Kind = EHyperAIStudioPlanActionKind::Step;
	FString StepId;
	FString OperationType;
	/** Generated by the coordinator at dispatch time; never accepted from plan JSON. */
	FString ActionNonce;
	EHyperAIStudioPlanSafety Safety = EHyperAIStudioPlanSafety::Read;
	FHyperAIStudioPlanStepBudget Budget;
};

/** Pure result returned by the future hyper_plan_validate tool. */
struct FHyperAIStudioPlanDryRunResult
{
	static constexpr const TCHAR* SchemaVersion = TEXT("hyperai.plan-validation.v1");

	bool bValid = false;
	bool bWouldMutate = false;
	bool bRequiresOperationId = false;
	bool bRequiresDestructiveAuthorization = false;
	bool bRequiresExternalEffectAuthorization = false;
	FString OperationId;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString EffectFingerprint;
	FString CapabilityHash;
	EHyperAIStudioPlanSafety MaximumSafety = EHyperAIStudioPlanSafety::Read;
	int32 StepCount = 0;
	int32 MutationStepCount = 0;
	int32 PlannedNativeOperationBudget = 0;
	int32 PlannedGameThreadBudgetMs = 0;
	int32 PlannedOutputBudgetBytes = 0;
	TArray<FString> OrderedStepIds;
	TArray<FHyperAIStudioPlanScheduledAction> Schedule;
	TArray<FHyperAIStudioPlanDiagnostic> Diagnostics;
};

class FHyperAIStudioTypedPlanValidator
{
public:
	/** Strictly parses and validates one bounded v1 plan. Unknown fields are rejected. */
	static bool ValidateJson(
		const FString& Json,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		FHyperAIStudioValidatedPlan& OutPlan,
		FHyperAIStudioPlanDryRunResult& OutResult);

	static bool BuildDryRun(
		const FHyperAIStudioValidatedPlan& Plan,
		FHyperAIStudioPlanDryRunResult& OutResult,
		FString& OutError);

	static bool SerializeDryRunJson(
		const FHyperAIStudioPlanDryRunResult& Result,
		FString& OutJson,
		FString& OutError);

	/** Execution-invariant semantic hash; excludes dry_run, bearer token, operation_id and expected_plan_hash. */
	static FString ComputePlanHash(const FHyperAIStudioValidatedPlan& Plan);
	/** Semantic hash used by a server-issued authorization grant; excludes mode, bearer token and operation_id. */
	static FString ComputeAuthorizationPlanHash(const FHyperAIStudioValidatedPlan& Plan);
	/** Stable digest of the declared effect contract, independent of execution envelope fields. */
	static FString ComputeEffectFingerprint(const FHyperAIStudioValidatedPlan& Plan);
	static const TCHAR* FreshValidatorId();
	static const TCHAR* FreshValidatorFingerprint();
	static const TCHAR* RollbackValidatorId();
	static const TCHAR* RollbackValidatorFingerprint();
	static FString SafetyToString(EHyperAIStudioPlanSafety Safety);
	static FString ActionKindToString(EHyperAIStudioPlanActionKind Kind);
};

enum class EHyperAIStudioPlanJournalBegin : uint8
{
	ProceedNew,
	ReplayCompleted,
	AlreadyInProgress,
	ExistingTerminal,
	OutcomeUnknown,
	Conflict,
	Unavailable
};

enum class EHyperAIStudioPlanJournalTransition : uint8
{
	Running,
	CommitStarted,
	Completed,
	FailedPreCommit,
	RolledBack,
	Partial,
	OutcomeUnknown
};

/**
 * Claims from a short-lived, server-issued validator receipt. The opaque token is verified by
 * a trusted server seam and is never persisted; only the bound claims/fingerprint reach the journal.
 */
struct FHyperAIStudioPlanValidatorReceipt
{
	FString ServerReceiptToken;
	FString ReceiptFingerprint;
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString ActionNonce;
	FString ValidatorId;
	FString ApprovedValidatorFingerprint;
	FString PostconditionHash;
	int64 IssuedUtcMs = 0;
	int64 ExpiresUtcMs = 0;

	bool IsPresent() const
	{
		return !ServerReceiptToken.IsEmpty() || !ReceiptFingerprint.IsEmpty()
			|| !CanonicalProjectId.IsEmpty() || !OperationId.IsEmpty() || !PlanHash.IsEmpty()
			|| !CapabilityHash.IsEmpty() || !EffectFingerprint.IsEmpty() || !ActionNonce.IsEmpty()
			|| !ValidatorId.IsEmpty() || !ApprovedValidatorFingerprint.IsEmpty()
			|| !PostconditionHash.IsEmpty() || IssuedUtcMs != 0 || ExpiresUtcMs != 0;
	}
};

struct FHyperAIStudioPlanOutcomeEvidence
{
	EHyperAIStudioPlanRollbackState RollbackState = EHyperAIStudioPlanRollbackState::NotNeeded;
	FHyperAIStudioPlanValidatorReceipt ValidatorReceipt;
};

/** Verifies the opaque server receipt; clients cannot self-assert validator claims. */
class IHyperAIStudioPlanValidatorReceiptGate
{
public:
	virtual ~IHyperAIStudioPlanValidatorReceiptGate() = default;
	virtual bool Verify(
		const FHyperAIStudioPlanValidatorReceipt& Receipt,
		int64 NowUtcMs,
		FString& OutError) = 0;
};

/** Narrow persistence seam; the coordinator never owns or bypasses the project mutation lease. */
class IHyperAIStudioPlanJournalGate
{
public:
	virtual ~IHyperAIStudioPlanJournalGate() = default;
	virtual EHyperAIStudioPlanJournalBegin Begin(
		const FString& OperationId,
		const FString& PlanHash,
		const FString& CapabilityHash,
		FString& OutError) = 0;
	virtual bool Transition(
		const FString& OperationId,
		EHyperAIStudioPlanJournalTransition Transition,
		const FHyperAIStudioPlanOutcomeEvidence& Evidence,
		FString& OutError) = 0;
	/** Core journal-owned proof for a synchronous dispatcher false/no-callback/no-effect return. */
	virtual bool CertifyNoEffect(
		const FString& OperationId,
		const FString& EffectFingerprint,
		const FString& ActionNonce,
		FString& OutError)
	{
		OutError = TEXT("The journal gate does not support certified no-effect closure.");
		return false;
	}
};

enum class EHyperAIStudioPlanAuthorizationState : uint8
{
	Available,
	AlreadyConsumed
};

struct FHyperAIStudioPlanAuthorizationRequest
{
	FString Token;
	FString CanonicalProjectId;
	FString OperationId;
	FString AuthorizationPlanHash;
	EHyperAIStudioPlanSafety Safety = EHyperAIStudioPlanSafety::Read;
};

struct FHyperAIStudioPlanAuthorizationReceipt
{
	FHyperAIStudioPlanAuthorizationRequest BoundRequest;
	FString Nonce;
	int64 ExpiresUtcMs = 0;
	EHyperAIStudioPlanAuthorizationState State = EHyperAIStudioPlanAuthorizationState::Available;
};

/** Trusted server seam. The plan carries only an opaque bearer token; clients never supply its claims. */
class IHyperAIStudioPlanAuthorizationGate
{
public:
	virtual ~IHyperAIStudioPlanAuthorizationGate() = default;
	virtual bool Inspect(
		const FHyperAIStudioPlanAuthorizationRequest& Request,
		int64 NowUtcMs,
		FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
		FString& OutError) = 0;
	virtual bool Consume(
		const FHyperAIStudioPlanAuthorizationRequest& Request,
		int64 NowUtcMs,
		FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
		FString& OutError) = 0;
};

class IHyperAIStudioPlanClock
{
public:
	virtual ~IHyperAIStudioPlanClock() = default;
	virtual int64 NowMonotonicMs() const = 0;
	virtual int64 NowUtcMs() const = 0;
};

class FHyperAIStudioOperationJournal;

/** Adapter that translates the narrow plan seam to the existing durable operation journal. */
class FHyperAIStudioOperationJournalPlanAdapter final : public IHyperAIStudioPlanJournalGate
{
public:
	explicit FHyperAIStudioOperationJournalPlanAdapter(FHyperAIStudioOperationJournal& InJournal)
		: Journal(InJournal)
	{
	}

	virtual EHyperAIStudioPlanJournalBegin Begin(
		const FString& OperationId,
		const FString& PlanHash,
		const FString& CapabilityHash,
		FString& OutError) override;
	virtual bool Transition(
		const FString& OperationId,
		EHyperAIStudioPlanJournalTransition Transition,
		const FHyperAIStudioPlanOutcomeEvidence& Evidence,
		FString& OutError) override;
	virtual bool CertifyNoEffect(
		const FString& OperationId,
		const FString& EffectFingerprint,
		const FString& ActionNonce,
		FString& OutError) override;

private:
	FHyperAIStudioOperationJournal& Journal;
};

enum class EHyperAIStudioPlanCoordinatorState : uint8
{
	Idle,
	Ready,
	AwaitingActionResult,
	Completed,
	Failed,
	Partial,
	RolledBack,
	OutcomeUnknown,
	ReplayCompleted
};

enum class EHyperAIStudioPlanActionOutcome : uint8
{
	Succeeded,
	FailedBeforeEffect,
	FailedAfterKnownEffect,
	RolledBackWithEvidence,
	OutcomeUnknown
};

struct FHyperAIStudioPlanActionUsage
{
	int32 NativeOperations = 0;
	int32 GameThreadMs = 0;
	int32 OutputBytes = 0;
};

struct FHyperAIStudioPlanActionResult
{
	EHyperAIStudioPlanActionOutcome Outcome = EHyperAIStudioPlanActionOutcome::Succeeded;
	FHyperAIStudioPlanActionUsage Usage;
	FHyperAIStudioPlanOutcomeEvidence Evidence;
};

/**
 * Serial, pull-based state machine. It returns typed actions one at a time but deliberately
 * contains no UObject mutation, script runner, raw tool-name dispatch, or backend fallback.
 */
class FHyperAIStudioSerialPlanCoordinator
{
public:
	bool Start(
		const FHyperAIStudioValidatedPlan& Plan,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		const FString& CanonicalProjectId,
		IHyperAIStudioPlanJournalGate* JournalGate,
		IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
		IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
		IHyperAIStudioPlanClock* Clock,
		FString& OutError);

	bool AcquireNextAction(FHyperAIStudioPlanScheduledAction& OutAction, FString& OutError);
	bool CompleteCurrentAction(const FHyperAIStudioPlanActionResult& Result, FString& OutError);
	/** Core-only closure after exact acquisition certifies that no dispatch, effect, or callback occurred. */
	bool CertifyCurrentActionNoEffect(const FString& Reason, FString& OutError);
	/** Closes a journal-admitted operation before commit/dispatch; never synthesizes rollback. */
	bool AbortBeforeFirstDispatch(const FString& Reason, FString& OutError);
	/** Admission-only secret hygiene; the opaque bearer is never needed after Start returns. */
	void ForgetAuthorizationToken() { ActivePlan.AuthorizationToken.Reset(); }

	EHyperAIStudioPlanCoordinatorState GetState() const { return State; }
	const TArray<FHyperAIStudioPlanScheduledAction>& GetSchedule() const { return Schedule; }
	int32 GetCompletedActionCount() const { return NextActionIndex; }
	bool HasMutationCommitStarted() const { return bCommitStarted; }
	bool HasKnownCommittedEffect() const { return bKnownCommittedEffect; }
	int64 GetUsedNativeOperations() const { return UsedNativeOperations; }
	int64 GetUsedGameThreadMs() const { return UsedGameThreadMs; }
	int64 GetUsedOutputBytes() const { return UsedOutputBytes; }
	const FString& GetActiveCanonicalProjectId() const { return ActiveCanonicalProjectId; }
	const FString& GetActiveOperationId() const { return ActivePlan.OperationId; }
	const FString& GetActivePlanHash() const { return ActivePlan.PlanHash; }
	const FString& GetActiveCapabilityHash() const { return ActivePlan.CapabilityHash; }
	const FString& GetActiveEffectFingerprint() const { return ActivePlan.EffectFingerprint; }
	const FString& GetActiveActionNonce() const { return ActiveActionNonce; }
	int64 GetCurrentUtcMs() const { return ActiveClock ? ActiveClock->NowUtcMs() : 0; }

private:
	bool TransitionJournal(
		EHyperAIStudioPlanJournalTransition Transition,
		const FHyperAIStudioPlanOutcomeEvidence& Evidence,
		FString& OutError);
	bool ClosePreCommitAfterFailure(FString& OutError);
	bool ValidateExecutionArtifact(const FHyperAIStudioTypedOperationRegistry& Registry, FString& OutError) const;
	bool ValidateAuthorizationReceipt(
		const FHyperAIStudioPlanAuthorizationRequest& Request,
		const FHyperAIStudioPlanAuthorizationReceipt& Receipt,
		int64 NowUtcMs,
		FString& OutError) const;
	bool ValidateValidatorReceipt(
		const FHyperAIStudioPlanValidatorReceipt& Receipt,
		const TCHAR* ExpectedValidatorId,
		const TCHAR* ExpectedValidatorFingerprint,
		int64 NowUtcMs,
		FString& OutError) const;
	bool CheckBeforeDispatch(const FHyperAIStudioPlanScheduledAction& Action, FString& OutError);
	bool AccountUsage(const FHyperAIStudioPlanActionResult& Result, FString& OutError);
	bool FailForRuntimeLimit(const FString& Reason, FString& OutError);
	bool FinishSuccessfully(FString& OutError);
	void Reset();

	EHyperAIStudioPlanCoordinatorState State = EHyperAIStudioPlanCoordinatorState::Idle;
	FHyperAIStudioValidatedPlan ActivePlan;
	TArray<FHyperAIStudioPlanScheduledAction> Schedule;
	IHyperAIStudioPlanJournalGate* ActiveJournalGate = nullptr;
	IHyperAIStudioPlanAuthorizationGate* ActiveAuthorizationGate = nullptr;
	IHyperAIStudioPlanValidatorReceiptGate* ActiveValidatorReceiptGate = nullptr;
	IHyperAIStudioPlanClock* ActiveClock = nullptr;
	FString ActiveCanonicalProjectId;
	FString ActiveActionNonce;
	int32 NextActionIndex = 0;
	int64 DeadlineMonotonicMs = 0;
	int64 UsedNativeOperations = 0;
	int64 UsedGameThreadMs = 0;
	int64 UsedOutputBytes = 0;
	bool bCommitStarted = false;
	bool bKnownCommittedEffect = false;
	bool bFreshVerificationComplete = false;
	FHyperAIStudioPlanOutcomeEvidence FreshVerificationEvidence;
};
