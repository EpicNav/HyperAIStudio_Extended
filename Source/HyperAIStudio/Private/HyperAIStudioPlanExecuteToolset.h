// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioBlueprintPatch.h"
#include "HyperAIStudioTypedPlan.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPlanExecuteToolset.generated.h"

/** Bounded execution counters retained for benchmark and failure analysis. */
USTRUCT(BlueprintType)
struct FHyperAIPlanExecuteTelemetry
{
	GENERATED_BODY()

	UPROPERTY()
	int32 DispatchedActionCount = 0;

	UPROPERTY()
	int32 CompletedActionCount = 0;

	UPROPERTY()
	int32 NativeOperationCount = 0;

	UPROPERTY()
	int32 GameThreadMs = 0;

	UPROPERTY()
	int32 OutputBytes = 0;

	UPROPERTY()
	int32 EpicDelegateCount = 0;

	UPROPERTY()
	int32 EpicDelegateLatencyMs = 0;

	UPROPERTY()
	int32 BlueprintPatchCount = 0;

	UPROPERTY()
	int32 CompileCount = 0;

	UPROPERTY()
	int32 ValidateCount = 0;

	UPROPERTY()
	int32 SaveCount = 0;

	UPROPERTY()
	int32 FreshVerifyCount = 0;

	UPROPERTY()
	int32 LateResultCount = 0;

	UPROPERTY()
	int32 PeakInFlightActionCount = 0;
};

/** Immediate, bounded acknowledgement. Execution continues serially after this call returns. */
USTRUCT(BlueprintType)
struct FHyperAIPlanExecuteResponse
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bAccepted = false;

	UPROPERTY()
	bool bTerminal = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bOutcomeUnknown = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ProjectIdentityHash;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString ActiveActionKind;

	UPROPERTY()
	FString ActiveStepId;

	UPROPERTY()
	int32 CompletedActionCount = 0;

	UPROPERTY()
	int32 ScheduledActionCount = 0;

	/** This is the executor's closed backend surface, not every TypedPlan metadata fixture. */
	UPROPERTY()
	TArray<FString> SupportedOperationTypes;

	UPROPERTY()
	FHyperAIPlanExecuteTelemetry Telemetry;
};

/** One-function Epic ToolsetRegistry class. It accepts work and never waits on an async future. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioPlanExecuteToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Execute one strict, closed typed plan. Poll hyper_operation_status after acceptance. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Workflow")
	static FHyperAIPlanExecuteResponse hyper_plan_execute(const FString& PlanJson);
};

/** Exact server-owned binding for a patch prepared by a native Blueprint workflow tool. */
struct FHyperAIStudioStagedBlueprintPatchArtifact
{
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString PatchId;
	FString AssetPath;
	int64 ExpiresUtcMs = 0;
	FHyperAIBlueprintPatch Patch;
};

struct FHyperAIStudioPlanBackendTelemetry
{
	int32 NativeOperations = 0;
	int32 GameThreadMs = 0;
	int32 OutputBytes = 0;
	int32 EpicDelegateCount = 0;
	int32 EpicDelegateLatencyMs = 0;
	int32 BlueprintPatchCount = 0;
	int32 CompileCount = 0;
	int32 ValidateCount = 0;
	int32 SaveCount = 0;
	int32 FreshVerifyCount = 0;
};

/**
 * Narrow asynchronous seam. It receives only a prevalidated typed action; there is no client
	 * toolset/tool/script/raw-dispatch parameter. A synchronous false return is a core-certified
	 * guarantee that no effect occurred and the completion callback was not retained or invoked.
 */
class IHyperAIStudioPlanAsyncDispatcher
{
public:
	using FCompletion = TFunction<void(FHyperAIStudioPlanActionResult, FHyperAIStudioPlanBackendTelemetry)>;

	virtual ~IHyperAIStudioPlanAsyncDispatcher() = default;
	virtual bool Prepare(
		const FHyperAIStudioValidatedPlan& Plan,
		const FString& CanonicalProjectId,
		FString& OutError) = 0;
	/** Trusted final CAS immediately before the coordinator persists its first commit marker. */
	virtual bool PreflightBeforeCommit(
		const FHyperAIStudioValidatedPlan&,
		const FHyperAIStudioPlanScheduledAction&,
		FString&)
	{
		return true;
	}
	virtual bool Dispatch(
		const FHyperAIStudioValidatedPlan& Plan,
		const FHyperAIStudioPlanScheduledAction& Action,
		FCompletion Completion,
		FString& OutError) = 0;
	virtual void Finish(EHyperAIStudioPlanCoordinatorState FinalState) = 0;
};

struct FHyperAIStudioPlanExecutionStatus
{
	FString Status;
	FString Diagnostic;
	FString OperationId;
	FString CanonicalProjectId;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString ActiveActionKind;
	FString ActiveStepId;
	EHyperAIStudioPlanCoordinatorState CoordinatorState = EHyperAIStudioPlanCoordinatorState::Idle;
	int32 CompletedActionCount = 0;
	int32 ScheduledActionCount = 0;
	bool bAccepted = false;
	bool bTerminal = false;
	bool bReplay = false;
	bool bOutcomeUnknown = false;
	FHyperAIPlanExecuteTelemetry Telemetry;
};

/**
 * Pure serial orchestration layer. Start never dispatches; Pump admits at most one action, and
 * asynchronous completion must return through Complete. Timeout/cancel never claim rollback.
 */
class FHyperAIStudioPlanExecutionRuntime final
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
		IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
		FString& OutError);

	/**
	 * Core-only entry for one already-admitted closed typed-artifact operation. It deliberately
	 * bypasses the production MCP operation-name allowlist, while the coordinator still revalidates
	 * the exact typed registry, hashes, budgets, journal, authorization and finalizer schedule.
	 */
	bool StartTypedArtifact(
		const FHyperAIStudioValidatedPlan& Plan,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		const FString& CanonicalProjectId,
		IHyperAIStudioPlanJournalGate* JournalGate,
		IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
		IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
		IHyperAIStudioPlanClock* Clock,
		IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
		FString& OutError);

	/** Dispatches at most one action and never waits. */
	bool Pump(FString& OutError);
	/** Checks a pending action deadline; safe to call from an editor ticker. */
	void Tick();
	/** Cancellation is fail-closed and can never synthesize rollback evidence. */
	bool RequestCancel(const FString& Reason, FString& OutError);
	/** Closes a started journal operation before any action/commit marker is acquired. */
	bool AbortBeforeDispatch(const FString& Reason, FString& OutError);
	/** Erases admission-only bearer material from both internal plan copies. */
	void ForgetAuthorizationToken();
	FHyperAIStudioPlanExecutionStatus GetStatus() const;

	bool IsTerminal() const;
	bool IsOutcomeUnknown() const;
	bool HasInFlightAction() const { return bActionInFlight; }
	bool HasOutstandingDispatch() const { return OutstandingDispatchCount > 0; }

	static TArray<FString> GetSupportedOperationTypes();
	static bool IsOperationTypeSupported(const FString& OperationType);
	static bool ValidateExecutablePlanShape(
		const FHyperAIStudioValidatedPlan& Plan,
		FString& OutError);

private:
	bool StartInternal(
		const FHyperAIStudioValidatedPlan& Plan,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		const FString& CanonicalProjectId,
		IHyperAIStudioPlanJournalGate* JournalGate,
		IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
		IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
		IHyperAIStudioPlanClock* Clock,
		IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
		bool bRequireProductionShape,
		FString& OutError);
	void Complete(
		uint64 DispatchGeneration,
		FHyperAIStudioPlanActionResult Result,
		const FHyperAIStudioPlanBackendTelemetry& BackendTelemetry);
	bool FailReadyActionBeforeDispatch(const FString& Reason, FString& OutError);
	void ForceUnknownOrPreEffectFailure(const FString& Reason);
	void RefreshTerminalState(const FString& Diagnostic = FString());
	EHyperAIStudioPlanCoordinatorState GetEffectiveState() const;
	static FString StateToStatus(EHyperAIStudioPlanCoordinatorState State);

	FHyperAIStudioSerialPlanCoordinator Coordinator;
	FHyperAIStudioValidatedPlan ActivePlan;
	IHyperAIStudioPlanAsyncDispatcher* ActiveDispatcher = nullptr;
	IHyperAIStudioPlanClock* ActiveClock = nullptr;
	FString CanonicalProjectId;
	FString LastDiagnostic;
	FHyperAIStudioPlanScheduledAction ActiveAction;
	FHyperAIPlanExecuteTelemetry Telemetry;
	int64 DeadlineMonotonicMs = 0;
	uint64 DispatchGeneration = 0;
	int32 OutstandingDispatchCount = 0;
	bool bStarted = false;
	bool bActionInFlight = false;
	bool bDispatcherFinished = false;
	bool bAmbiguousOutcome = false;
};

/** Server-owned integration surface used by domain callables and the one-tool wrapper. */
class FHyperAIStudioPlanExecutionService final
{
public:
	/** Reopens the process-local service after a proven quiescent registration lifecycle. */
	static void Startup();

	static FHyperAIPlanExecuteResponse Submit(const FString& PlanJson);
	static bool QueryStatus(const FString& OperationId, FHyperAIStudioPlanExecutionStatus& OutStatus);
	static bool Cancel(const FString& OperationId, const FString& Reason, FString& OutError);

	/** Stages an already-typed patch; this is deliberately not AICallable. */
	static bool StageBlueprintPatch(
		const FHyperAIStudioStagedBlueprintPatchArtifact& Artifact,
		FString& OutError);

	/** Issues an opaque, exact, single-use destructive/external grant; this is not AICallable. */
	static bool IssueAuthorization(
		const FHyperAIStudioValidatedPlan& Plan,
		const FString& CanonicalProjectId,
		FString& OutOpaqueToken,
		FString& OutError);

	/** False while an uncancellable async engine/tool callback can still enter this module. */
	static bool CanShutdownSafely();
	static void Shutdown();
};

/** Checked-in admission stays SourceCandidate and is callable only in explicit dev automation. */
class FHyperAIStudioPlanExecuteContracts final
{
public:
	static constexpr int32 MaxHashInputBytes = 1024 * 1024;

	static FString GetQualifiedToolsetName();
	static bool IsPendingNativeToolsTestEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	/** Win64-safe, bounded SHA-256 over exact UTF-8 bytes; malformed UTF-16 fails closed. */
	static FString ComputeBoundedSha256(const FString& Value);
	/** Raw-byte entrypoint backed by the same incremental SHA-256 used for package receipts. */
	static FString ComputeBoundedSha256Bytes(const TArray<uint8>& Bytes);
	/** A save attempt is ambiguous unless both the API result and in-memory dirty state prove success. */
	static EHyperAIStudioPlanActionOutcome ClassifySaveAttempt(
		bool bSaveReturnedSuccess,
		bool bPackageIsDirty);
	static FString ClipText(const FString& Value, int32 MaxCharacters);
};

class FHyperAIStudioPlanExecuteToolRegistration final
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
