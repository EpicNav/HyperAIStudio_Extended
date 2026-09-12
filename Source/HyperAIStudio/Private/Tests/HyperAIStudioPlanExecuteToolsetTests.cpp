// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPlanExecuteToolset.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"

namespace HyperAIStudio::PlanExecute::Tests
{
	const FString TestProjectId =
		TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");

	int64 UtcNowMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}

	FString ReadStep(const int32 OutputBudget = 1024)
	{
		return FString::Printf(TEXT(R"json(
{
  "id":"inspect",
  "operation":"foundation.inspect",
  "depends_on":[],
  "arguments":{"subject":"editor.current_level","fields":["path"]},
  "preconditions":[],
  "effects":[],
  "budget":{"max_native_operations":1,"max_game_thread_ms":2,"max_output_bytes":%d}
	})json"), OutputBudget);
	}

	FString DependentReadStep(const int32 OutputBudget = 1024)
	{
		FString Step = ReadStep(OutputBudget);
		Step.ReplaceInline(TEXT("\"id\":\"inspect\""), TEXT("\"id\":\"inspect_next\""));
		Step.ReplaceInline(TEXT("\"depends_on\":[]"), TEXT("\"depends_on\":[\"inspect\"]"));
		return Step;
	}

	FString BlueprintStep(const bool bDelete)
	{
		if (!bDelete)
		{
			return TEXT(R"json(
{
  "id":"patch",
  "operation":"blueprint.apply_patch",
  "depends_on":[],
  "arguments":{"asset_path":"/Game/BP_Test","patch_id":"patch-01"},
  "preconditions":[{"kind":"object_exists","target":"/Game/BP_Test"},{"kind":"revision_equals","target":"/Game/BP_Test","expected":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],
  "effects":[{"kind":"object_updated","target":"/Game/BP_Test","validator_id":"blueprint.compile_validate","expected":"compiled"}],
  "budget":{"max_native_operations":16,"max_game_thread_ms":100,"max_output_bytes":4096}
		})json");
	}

		return TEXT(R"json(
{
  "id":"delete_patch",
  "operation":"blueprint.delete_patch",
  "depends_on":[],
  "arguments":{"asset_path":"/Game/BP_Test","patch_id":"delete-patch-01"},
  "preconditions":[{"kind":"object_exists","target":"/Game/BP_Test"},{"kind":"revision_equals","target":"/Game/BP_Test","expected":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],
  "effects":[{"kind":"object_updated","target":"/Game/BP_Test","validator_id":"blueprint.delete_compile_validate","expected":"compiled"},{"kind":"object_deleted","target":"/Game/BP_Test","validator_id":"blueprint.delete_compile_validate","expected":"requested_nodes_absent"}],
  "budget":{"max_native_operations":16,"max_game_thread_ms":100,"max_output_bytes":4096}
})json");
	}

	FString DependentBlueprintStep()
	{
		FString Step = BlueprintStep(false);
		Step.ReplaceInline(TEXT("\"depends_on\":[]"), TEXT("\"depends_on\":[\"inspect\"]"));
		return Step;
	}

	FString AssetDeleteStep()
	{
		return TEXT(R"json(
{
  "id":"delete_asset",
  "operation":"asset.delete",
  "depends_on":[],
  "arguments":{"asset_path":"/Game/Old"},
  "preconditions":[{"kind":"object_exists","target":"/Game/Old"}],
  "effects":[{"kind":"object_deleted","target":"/Game/Old","validator_id":"asset.absence"}],
  "budget":{"max_native_operations":4,"max_game_thread_ms":100,"max_output_bytes":4096}
})json");
	}

	FString MakePlan(
		const FString& CapabilityHash,
		const FString& Step,
		const FString& OperationId,
		const FString& AuthorizationToken = FString(),
		const int32 DeadlineMs = 10000,
		const int32 MaxOutputBytes = 65536)
	{
		const FString Authorization = AuthorizationToken.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\"authorization_token\":\"%s\""), *AuthorizationToken);
		return FString::Printf(TEXT(R"json(
{
  "schema":"hyperai.plan.v1",
  "dry_run":false,
  "operation_id":"%s",
  "capability_hash":"%s"%s,
  "budget":{"deadline_ms":%d,"max_steps":16,"max_mutations":8,"max_native_operations":64,"max_game_thread_ms":1000,"max_output_bytes":%d},
  "steps":[%s]
})json"), *OperationId, *CapabilityHash, *Authorization, DeadlineMs, MaxOutputBytes, *Step);
	}

	bool Validate(
		const FString& Json,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		FHyperAIStudioValidatedPlan& OutPlan)
	{
		FHyperAIStudioPlanDryRunResult DryRun;
		return FHyperAIStudioTypedPlanValidator::ValidateJson(Json, Registry, OutPlan, DryRun);
	}

	class FFakeClock final : public IHyperAIStudioPlanClock
	{
	public:
		int64 MonotonicMs = 1000;
		int64 CurrentUtcMs = UtcNowMs();
		virtual int64 NowMonotonicMs() const override { return MonotonicMs; }
		virtual int64 NowUtcMs() const override { return CurrentUtcMs; }
	};

	class FFakeJournal final : public IHyperAIStudioPlanJournalGate
	{
	public:
		EHyperAIStudioPlanJournalBegin BeginResult = EHyperAIStudioPlanJournalBegin::ProceedNew;
		int32 BeginCalls = 0;
		FString OperationId;
		FString PlanHash;
		FString CapabilityHash;
		TArray<EHyperAIStudioPlanJournalTransition> Transitions;
		int32 CertifiedNoEffectCalls = 0;

		virtual EHyperAIStudioPlanJournalBegin Begin(
			const FString& InOperationId,
			const FString& InPlanHash,
			const FString& InCapabilityHash,
			FString& OutError) override
		{
			++BeginCalls;
			OperationId = InOperationId;
			PlanHash = InPlanHash;
			CapabilityHash = InCapabilityHash;
			if (BeginResult != EHyperAIStudioPlanJournalBegin::ProceedNew
				&& BeginResult != EHyperAIStudioPlanJournalBegin::ReplayCompleted)
			{
				OutError = TEXT("fake journal begin rejected");
			}
			return BeginResult;
		}

		virtual bool Transition(
			const FString& InOperationId,
			const EHyperAIStudioPlanJournalTransition Transition,
			const FHyperAIStudioPlanOutcomeEvidence& Evidence,
			FString& OutError) override
		{
			if (InOperationId != OperationId)
			{
				OutError = TEXT("fake journal operation binding changed");
				return false;
			}
			Transitions.Add(Transition);
			return true;
		}

		virtual bool CertifyNoEffect(
			const FString& InOperationId,
			const FString& EffectFingerprint,
			const FString& ActionNonce,
			FString& OutError) override
		{
			if (InOperationId != OperationId || EffectFingerprint.IsEmpty()
				|| !ActionNonce.StartsWith(TEXT("action-")))
			{
				OutError = TEXT("fake certified-no-effect binding changed");
				return false;
			}
			++CertifiedNoEffectCalls;
			return true;
		}
	};

	class FFakeAuthorization final : public IHyperAIStudioPlanAuthorizationGate
	{
	public:
		bool bAccept = true;
		bool bConsumed = false;
		FHyperAIStudioPlanAuthorizationRequest Expected;
		int32 InspectCalls = 0;
		int32 ConsumeCalls = 0;

		virtual bool Inspect(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++InspectCalls;
			if (!bAccept || Request.Token != Expected.Token
				|| Request.CanonicalProjectId != Expected.CanonicalProjectId
				|| Request.OperationId != Expected.OperationId
				|| Request.AuthorizationPlanHash != Expected.AuthorizationPlanHash
				|| Request.Safety != Expected.Safety)
			{
				OutError = TEXT("fake server authorization rejected");
				return false;
			}
			OutReceipt.BoundRequest = Expected;
			OutReceipt.Nonce = TEXT("server-grant-nonce");
			OutReceipt.ExpiresUtcMs = NowUtcMs + 60000;
			OutReceipt.State = bConsumed
				? EHyperAIStudioPlanAuthorizationState::AlreadyConsumed
				: EHyperAIStudioPlanAuthorizationState::Available;
			return true;
		}

		virtual bool Consume(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++ConsumeCalls;
			if (bConsumed || !Inspect(Request, NowUtcMs, OutReceipt, OutError))
			{
				return false;
			}
			bConsumed = true;
			OutReceipt.State = EHyperAIStudioPlanAuthorizationState::AlreadyConsumed;
			return true;
		}
	};

	class FFakeReceiptGate final : public IHyperAIStudioPlanValidatorReceiptGate
	{
	public:
		bool bAccept = true;
		int32 VerifyCalls = 0;
		virtual bool Verify(
			const FHyperAIStudioPlanValidatorReceipt& Receipt,
			const int64 NowUtcMs,
			FString& OutError) override
		{
			++VerifyCalls;
			if (!bAccept || Receipt.ServerReceiptToken != TEXT("fake-fresh-receipt")
				|| Receipt.IssuedUtcMs > NowUtcMs || Receipt.ExpiresUtcMs <= NowUtcMs)
			{
				OutError = TEXT("fake fresh receipt rejected");
				return false;
			}
			return true;
		}
	};

	class FFakeDispatcher final : public IHyperAIStudioPlanAsyncDispatcher
	{
	public:
		bool bRejectPrepare = false;
		bool bRejectDispatch = false;
		bool bPending = false;
		int32 DispatchCalls = 0;
		int32 FinishCalls = 0;
		int32 PeakPending = 0;
		FHyperAIStudioValidatedPlan Plan;
		FString ProjectId;
		FHyperAIStudioPlanScheduledAction PendingAction;
		FCompletion PendingCompletion;
		TArray<EHyperAIStudioPlanActionKind> DispatchedKinds;

		virtual bool Prepare(
			const FHyperAIStudioValidatedPlan& InPlan,
			const FString& CanonicalProjectId,
			FString& OutError) override
		{
			if (bRejectPrepare)
			{
				OutError = TEXT("fake prepare rejected");
				return false;
			}
			for (const FHyperAIStudioPlanStep& Step : InPlan.Steps)
			{
				if (!FHyperAIStudioPlanExecutionRuntime::IsOperationTypeSupported(Step.OperationType)
					|| Step.OperationType.StartsWith(TEXT("hyper_")))
				{
					OutError = TEXT("fake closed backend rejected recursive/unallowlisted operation");
					return false;
				}
			}
			Plan = InPlan;
			ProjectId = CanonicalProjectId;
			return true;
		}

		virtual bool Dispatch(
			const FHyperAIStudioValidatedPlan& InPlan,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError) override
		{
			if (bRejectDispatch)
			{
				OutError = TEXT("fake dispatch failed before effect");
				return false;
			}
			if (bPending)
			{
				OutError = TEXT("fake overlap");
				return false;
			}
			++DispatchCalls;
			bPending = true;
			PeakPending = FMath::Max(PeakPending, 1);
			PendingAction = Action;
			PendingCompletion = MoveTemp(Completion);
			DispatchedKinds.Add(Action.Kind);
			return true;
		}

		virtual void Finish(const EHyperAIStudioPlanCoordinatorState FinalState) override
		{
			++FinishCalls;
		}

		bool CompletePending(
			const EHyperAIStudioPlanActionOutcome Outcome,
			const FFakeClock& Clock,
			const int32 OutputBytesOverride = INDEX_NONE)
		{
			if (!bPending || !PendingCompletion)
			{
				return false;
			}
			FHyperAIStudioPlanActionResult Result;
			Result.Outcome = Outcome;
			if (Outcome == EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect)
			{
				Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
			}
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			Telemetry.NativeOperations = 1;
			Telemetry.GameThreadMs = 1;
			Telemetry.OutputBytes = OutputBytesOverride == INDEX_NONE ? 64 : OutputBytesOverride;
			if (PendingAction.Kind == EHyperAIStudioPlanActionKind::Step
				&& PendingAction.OperationType.StartsWith(TEXT("blueprint.")))
			{
				Telemetry.BlueprintPatchCount = Outcome == EHyperAIStudioPlanActionOutcome::Succeeded ? 1 : 0;
				Telemetry.CompileCount = Outcome == EHyperAIStudioPlanActionOutcome::Succeeded ? 1 : 0;
			}
			else if (PendingAction.Kind == EHyperAIStudioPlanActionKind::ValidateOnce)
			{
				Telemetry.ValidateCount = 1;
			}
			else if (PendingAction.Kind == EHyperAIStudioPlanActionKind::SaveOnce)
			{
				Telemetry.SaveCount = 1;
			}
			else if (PendingAction.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce)
			{
				Telemetry.FreshVerifyCount = 1;
				if (Outcome == EHyperAIStudioPlanActionOutcome::Succeeded)
				{
					FHyperAIStudioPlanValidatorReceipt& Receipt = Result.Evidence.ValidatorReceipt;
					Receipt.ServerReceiptToken = TEXT("fake-fresh-receipt");
					Receipt.ReceiptFingerprint = TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
					Receipt.CanonicalProjectId = ProjectId;
					Receipt.OperationId = Plan.OperationId;
					Receipt.PlanHash = Plan.PlanHash;
					Receipt.CapabilityHash = Plan.CapabilityHash;
					Receipt.EffectFingerprint = Plan.EffectFingerprint;
					Receipt.ActionNonce = PendingAction.ActionNonce;
					Receipt.ValidatorId = FHyperAIStudioTypedPlanValidator::FreshValidatorId();
					Receipt.ApprovedValidatorFingerprint = FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint();
					Receipt.PostconditionHash = TEXT("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
					Receipt.IssuedUtcMs = Clock.CurrentUtcMs - 1;
					Receipt.ExpiresUtcMs = Clock.CurrentUtcMs + 60000;
				}
			}
			FCompletion Completion = MoveTemp(PendingCompletion);
			bPending = false;
			PendingAction = FHyperAIStudioPlanScheduledAction{};
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}
	};

	bool CompleteSuccess(
		FHyperAIStudioPlanExecutionRuntime& Runtime,
		FFakeDispatcher& Dispatcher,
		const FFakeClock& Clock,
		FAutomationTestBase& Test)
	{
		FString Error;
		int32 Guard = 0;
		while (!Runtime.IsTerminal() && Guard++ < 32)
		{
			if (!Runtime.HasInFlightAction())
			{
				if (!Test.TestTrue(TEXT("Next serial action dispatches"), Runtime.Pump(Error)))
				{
					return false;
				}
			}
			if (!Test.TestTrue(TEXT("A pending serial action exists before fake completion"),
				Dispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::Succeeded, Clock)))
			{
				return false;
			}
		}
		return Test.TestTrue(TEXT("Runtime reaches a terminal state within its bounded schedule"), Runtime.IsTerminal());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanExecuteAsyncSuccessTest,
	"HyperAIStudio.NativeTools.PlanExecute.AsyncSuccessAndFreshReceipt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanExecuteAsyncSuccessTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanExecute::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry =
		FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	FHyperAIStudioValidatedPlan Plan;
	if (!TestTrue(TEXT("Blueprint execute fixture validates"), Validate(
		MakePlan(Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-execute-success")),
		Registry, Plan)))
	{
		return false;
	}
	FFakeClock Clock;
	FFakeJournal Journal;
	FFakeReceiptGate Receipts;
	FFakeDispatcher Dispatcher;
	FHyperAIStudioPlanExecutionRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Runtime accepts without dispatching"), Runtime.Start(
		Plan, Registry, TestProjectId, &Journal, nullptr, &Receipts, &Clock, &Dispatcher, Error));
	TestEqual(TEXT("Acceptance is immediate and no backend action ran"), Dispatcher.DispatchCalls, 0);
	TestEqual(TEXT("Journal begins and records running before response"), Journal.Transitions.Num(), 1);
	TestEqual(TEXT("Journal binds the exact client operation_id"), Journal.OperationId, Plan.OperationId);
	TestEqual(TEXT("Journal binds the exact canonical plan hash"), Journal.PlanHash, Plan.PlanHash);
	TestEqual(TEXT("Journal binds the exact capability hash"), Journal.CapabilityHash, Plan.CapabilityHash);
	TestTrue(TEXT("First action dispatches"), Runtime.Pump(Error));
	TestFalse(TEXT("A second action cannot overlap the pending action"), Runtime.Pump(Error));
	if (!TestTrue(TEXT("The first pending action can be completed"),
		Dispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::Succeeded, Clock)))
	{
		return false;
	}
	if (!CompleteSuccess(Runtime, Dispatcher, Clock, *this))
	{
		return false;
	}
	const FHyperAIStudioPlanExecutionStatus Status = Runtime.GetStatus();
	TestEqual(TEXT("Fresh-verified workflow completes"), Status.CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Completed);
	TestEqual(TEXT("Step plus four lifecycle gates execute"), Dispatcher.DispatchCalls, 5);
	TestEqual(TEXT("Compile barrier appears exactly once"),
		Dispatcher.DispatchedKinds.FilterByPredicate([](const EHyperAIStudioPlanActionKind Kind)
		{
			return Kind == EHyperAIStudioPlanActionKind::CompileOnce;
		}).Num(), 1);
	TestEqual(TEXT("Save appears exactly once"), Status.Telemetry.SaveCount, 1);
	TestEqual(TEXT("Integrated compile appears exactly once"), Status.Telemetry.CompileCount, 1);
	TestEqual(TEXT("Fresh verification appears exactly once"), Status.Telemetry.FreshVerifyCount, 1);
	TestEqual(TEXT("Trusted receipt gate is consulted exactly once"), Receipts.VerifyCalls, 1);
	TestEqual(TEXT("No action overlap is observed"), Status.Telemetry.PeakInFlightActionCount, 1);
	TestTrue(TEXT("Journal reaches completed only after a commit marker"),
		Journal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::CommitStarted)
		&& Journal.Transitions.Last() == EHyperAIStudioPlanJournalTransition::Completed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanExecuteFailureTimeoutTest,
	"HyperAIStudio.NativeTools.PlanExecute.FailuresTimeoutLateAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanExecuteFailureTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanExecute::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry =
		FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	FHyperAIStudioValidatedPlan ReadPlan;
	TestTrue(TEXT("Read fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep(), TEXT("operation-read-failure")), Registry, ReadPlan));
	FFakeClock FailureClock;
	FFakeDispatcher FailureDispatcher;
	FHyperAIStudioPlanExecutionRuntime FailureRuntime;
	FString Error;
	TestTrue(TEXT("Read failure fixture starts"), FailureRuntime.Start(
		ReadPlan, Registry, TestProjectId, nullptr, nullptr, nullptr,
		&FailureClock, &FailureDispatcher, Error));
	TestTrue(TEXT("Read action dispatches"), FailureRuntime.Pump(Error));
	if (!TestTrue(TEXT("The pending read failure action can be completed"),
		FailureDispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::FailedBeforeEffect, FailureClock)))
	{
		return false;
	}
	TestEqual(TEXT("Pre-effect read failure is terminal failed"), FailureRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);

	FHyperAIStudioValidatedPlan TimeoutPlan;
	if (!TestTrue(TEXT("Timeout fixture validates at the minimum accepted deadline"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep(), TEXT("operation-read-timeout"), FString(), 100),
		Registry, TimeoutPlan)))
	{
		return false;
	}
	FFakeClock TimeoutClock;
	FFakeDispatcher TimeoutDispatcher;
	FHyperAIStudioPlanExecutionRuntime TimeoutRuntime;
	if (!TestTrue(TEXT("Timeout fixture starts"), TimeoutRuntime.Start(
		TimeoutPlan, Registry, TestProjectId, nullptr, nullptr, nullptr,
		&TimeoutClock, &TimeoutDispatcher, Error)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Never-resolving fake dispatch starts"), TimeoutRuntime.Pump(Error)))
	{
		return false;
	}
	TimeoutClock.MonotonicMs += 101;
	TimeoutRuntime.Tick();
	TestEqual(TEXT("Ambiguous transport timeout remains durable outcome_unknown"),
		TimeoutRuntime.GetStatus().CoordinatorState, EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);
	if (!TestTrue(TEXT("The timed-out fake dispatch remains available as a late completion"),
		TimeoutDispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::Succeeded, TimeoutClock)))
	{
		return false;
	}
	TestEqual(TEXT("Late result is counted and ignored"), TimeoutRuntime.GetStatus().Telemetry.LateResultCount, 1);

	FHyperAIStudioValidatedPlan BudgetPlan;
	TestTrue(TEXT("Budget fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep(256), TEXT("operation-read-budget"), FString(), 10000, 256),
		Registry, BudgetPlan));
	FFakeClock BudgetClock;
	FFakeDispatcher BudgetDispatcher;
	FHyperAIStudioPlanExecutionRuntime BudgetRuntime;
	TestTrue(TEXT("Budget fixture starts"), BudgetRuntime.Start(
		BudgetPlan, Registry, TestProjectId, nullptr, nullptr, nullptr,
		&BudgetClock, &BudgetDispatcher, Error));
	TestTrue(TEXT("Budget action dispatches"), BudgetRuntime.Pump(Error));
	if (!TestTrue(TEXT("The pending budget action can be completed"),
		BudgetDispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::Succeeded, BudgetClock, 257)))
	{
		return false;
	}
	TestEqual(TEXT("One-byte per-action overrun fails closed"), BudgetRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanExecutePartialCancelTest,
	"HyperAIStudio.NativeTools.PlanExecute.PartialAndCancellationUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanExecutePartialCancelTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanExecute::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry =
		FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	FHyperAIStudioValidatedPlan Plan;
	TestTrue(TEXT("Mutation fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-partial")), Registry, Plan));
	FFakeClock PartialClock;
	FFakeJournal PartialJournal;
	FFakeReceiptGate PartialReceipts;
	FFakeDispatcher PartialDispatcher;
	FHyperAIStudioPlanExecutionRuntime PartialRuntime;
	FString Error;
	TestTrue(TEXT("Partial fixture starts"), PartialRuntime.Start(
		Plan, Registry, TestProjectId, &PartialJournal, nullptr, &PartialReceipts,
		&PartialClock, &PartialDispatcher, Error));
	TestTrue(TEXT("Mutation action dispatches after commit barrier"), PartialRuntime.Pump(Error));
	if (!TestTrue(TEXT("The pending mutation action can report its known effect"),
		PartialDispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect, PartialClock)))
	{
		return false;
	}
	TestEqual(TEXT("Known effect failure is durable partial"), PartialRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Partial journal closure is explicit"), PartialJournal.Transitions.Last(),
		EHyperAIStudioPlanJournalTransition::Partial);

	FHyperAIStudioValidatedPlan CancelPlan;
	TestTrue(TEXT("Cancellation fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-cancel")), Registry, CancelPlan));
	FFakeClock CancelClock;
	FFakeJournal CancelJournal;
	FFakeReceiptGate CancelReceipts;
	FFakeDispatcher CancelDispatcher;
	FHyperAIStudioPlanExecutionRuntime CancelRuntime;
	TestTrue(TEXT("Cancellation fixture starts"), CancelRuntime.Start(
		CancelPlan, Registry, TestProjectId, &CancelJournal, nullptr, &CancelReceipts,
		&CancelClock, &CancelDispatcher, Error));
	TestTrue(TEXT("Mutation enters its one in-flight action"), CancelRuntime.Pump(Error));
	TestTrue(TEXT("Cancellation request is accepted fail-closed"),
		CancelRuntime.RequestCancel(TEXT("test cancellation"), Error));
	TestEqual(TEXT("Cancellation after commit is outcome_unknown, never rolled_back"),
		CancelRuntime.GetStatus().CoordinatorState, EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);
	TestFalse(TEXT("Cancellation never records a rollback transition"),
		CancelJournal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::RolledBack));
	if (!TestTrue(TEXT("The cancelled fake dispatch remains available as a late completion"),
		CancelDispatcher.CompletePending(EHyperAIStudioPlanActionOutcome::Succeeded, CancelClock)))
	{
		return false;
	}
	TestEqual(TEXT("Cancelled late result is ignored"), CancelRuntime.GetStatus().Telemetry.LateResultCount, 1);

	FFakeJournal PreDispatchCancelJournal;
	FFakeDispatcher PreDispatchCancelDispatcher;
	FHyperAIStudioPlanExecutionRuntime PreDispatchCancelRuntime;
	TestTrue(TEXT("Pre-dispatch cancellation fixture starts"), PreDispatchCancelRuntime.Start(
		CancelPlan, Registry, TestProjectId, &PreDispatchCancelJournal, nullptr, &CancelReceipts,
		&CancelClock, &PreDispatchCancelDispatcher, Error));
	TestTrue(TEXT("Cancellation before first pump closes retry-safe precommit"),
		PreDispatchCancelRuntime.RequestCancel(TEXT("cancel before pump"), Error));
	TestEqual(TEXT("Pre-dispatch cancellation is failed, not unknown"),
		PreDispatchCancelRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Pre-dispatch cancellation performs no dispatch"),
		PreDispatchCancelDispatcher.DispatchCalls, 0);
	TestFalse(TEXT("Pre-dispatch cancellation writes no commit marker"),
		PreDispatchCancelJournal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::CommitStarted));

	FFakeClock PreDispatchTimeoutClock;
	FFakeJournal PreDispatchTimeoutJournal;
	FFakeDispatcher PreDispatchTimeoutDispatcher;
	FHyperAIStudioPlanExecutionRuntime PreDispatchTimeoutRuntime;
	FHyperAIStudioValidatedPlan PreDispatchTimeoutPlan;
	TestTrue(TEXT("Pre-dispatch timeout plan validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-timeout-before-dispatch"),
		FString(), 100), Registry, PreDispatchTimeoutPlan));
	TestTrue(TEXT("Pre-dispatch timeout fixture starts"), PreDispatchTimeoutRuntime.Start(
		PreDispatchTimeoutPlan, Registry, TestProjectId, &PreDispatchTimeoutJournal, nullptr,
		&CancelReceipts, &PreDispatchTimeoutClock, &PreDispatchTimeoutDispatcher, Error));
	PreDispatchTimeoutClock.MonotonicMs += 101;
	PreDispatchTimeoutRuntime.Tick();
	TestEqual(TEXT("Deadline before first dispatch closes failed_precommit"),
		PreDispatchTimeoutRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Deadline before first dispatch performs no dispatch"),
		PreDispatchTimeoutDispatcher.DispatchCalls, 0);
	TestFalse(TEXT("Deadline before first dispatch writes no commit marker"),
		PreDispatchTimeoutJournal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::CommitStarted));

	FFakeJournal BetweenActionsCancelJournal;
	FFakeDispatcher BetweenActionsCancelDispatcher;
	FHyperAIStudioPlanExecutionRuntime BetweenActionsCancelRuntime;
	FHyperAIStudioValidatedPlan BetweenActionsCancelPlan;
	TestTrue(TEXT("Between-actions cancel plan validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-cancel-between-actions")),
		Registry, BetweenActionsCancelPlan));
	TestTrue(TEXT("Between-actions cancel fixture starts"), BetweenActionsCancelRuntime.Start(
		BetweenActionsCancelPlan, Registry, TestProjectId, &BetweenActionsCancelJournal, nullptr,
		&CancelReceipts, &CancelClock, &BetweenActionsCancelDispatcher, Error));
	TestTrue(TEXT("Apply dispatches before the between-actions cancel"),
		BetweenActionsCancelRuntime.Pump(Error));
	TestTrue(TEXT("Apply completes with a known committed effect"),
		BetweenActionsCancelDispatcher.CompletePending(
			EHyperAIStudioPlanActionOutcome::Succeeded, CancelClock));
	TestTrue(TEXT("Cancellation between apply and compile is accepted"),
		BetweenActionsCancelRuntime.RequestCancel(TEXT("cancel before compile"), Error));
	TestEqual(TEXT("Known apply plus undispatched compile closes partial"),
		BetweenActionsCancelRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Cancellation dispatches no compile action"),
		BetweenActionsCancelDispatcher.DispatchCalls, 1);
	TestEqual(TEXT("Between-actions cancellation is durably partial"),
		BetweenActionsCancelJournal.Transitions.Last(),
		EHyperAIStudioPlanJournalTransition::Partial);

	FFakeClock BetweenActionsTimeoutClock;
	FFakeJournal BetweenActionsTimeoutJournal;
	FFakeDispatcher BetweenActionsTimeoutDispatcher;
	FHyperAIStudioPlanExecutionRuntime BetweenActionsTimeoutRuntime;
	FHyperAIStudioValidatedPlan BetweenActionsTimeoutPlan;
	TestTrue(TEXT("Between-actions timeout plan validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-timeout-between-actions"),
		FString(), 100), Registry, BetweenActionsTimeoutPlan));
	TestTrue(TEXT("Between-actions timeout fixture starts"), BetweenActionsTimeoutRuntime.Start(
		BetweenActionsTimeoutPlan, Registry, TestProjectId, &BetweenActionsTimeoutJournal, nullptr,
		&CancelReceipts, &BetweenActionsTimeoutClock, &BetweenActionsTimeoutDispatcher, Error));
	TestTrue(TEXT("Apply dispatches before the between-actions deadline"),
		BetweenActionsTimeoutRuntime.Pump(Error));
	TestTrue(TEXT("Apply completes before the deadline"),
		BetweenActionsTimeoutDispatcher.CompletePending(
			EHyperAIStudioPlanActionOutcome::Succeeded, BetweenActionsTimeoutClock));
	BetweenActionsTimeoutClock.MonotonicMs += 101;
	BetweenActionsTimeoutRuntime.Tick();
	TestEqual(TEXT("Deadline between apply and compile closes partial"),
		BetweenActionsTimeoutRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Deadline dispatches no compile action"),
		BetweenActionsTimeoutDispatcher.DispatchCalls, 1);

	FHyperAIStudioValidatedPlan ReadCancelPlan;
	TestTrue(TEXT("Two-read cancellation fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep() + TEXT(",") + DependentReadStep(),
		TEXT("operation-read-cancel-between-actions")), Registry, ReadCancelPlan));
	FFakeJournal ReadCancelJournal;
	FFakeDispatcher ReadCancelDispatcher;
	FHyperAIStudioPlanExecutionRuntime ReadCancelRuntime;
	TestTrue(TEXT("Two-read cancellation fixture starts"), ReadCancelRuntime.Start(
		ReadCancelPlan, Registry, TestProjectId, &ReadCancelJournal, nullptr,
		&CancelReceipts, &CancelClock, &ReadCancelDispatcher, Error));
	TestTrue(TEXT("First read dispatches"), ReadCancelRuntime.Pump(Error));
	TestTrue(TEXT("First read completes"), ReadCancelDispatcher.CompletePending(
		EHyperAIStudioPlanActionOutcome::Succeeded, CancelClock));
	TestTrue(TEXT("Cancellation before the second read is accepted"),
		ReadCancelRuntime.RequestCancel(TEXT("cancel before second read"), Error));
	TestEqual(TEXT("Undispatched second read closes failed, never unknown"),
		ReadCancelRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Cancellation dispatches no second read"), ReadCancelDispatcher.DispatchCalls, 1);

	FHyperAIStudioValidatedPlan ReadThenEditCancelPlan;
	TestTrue(TEXT("Read-then-edit cancellation fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep() + TEXT(",") + DependentBlueprintStep(),
		TEXT("operation-read-edit-cancel-between-actions")), Registry, ReadThenEditCancelPlan));
	FFakeJournal ReadThenEditCancelJournal;
	FFakeDispatcher ReadThenEditCancelDispatcher;
	FHyperAIStudioPlanExecutionRuntime ReadThenEditCancelRuntime;
	TestTrue(TEXT("Read-then-edit cancellation fixture starts"), ReadThenEditCancelRuntime.Start(
		ReadThenEditCancelPlan, Registry, TestProjectId, &ReadThenEditCancelJournal, nullptr,
		&CancelReceipts, &CancelClock, &ReadThenEditCancelDispatcher, Error));
	TestTrue(TEXT("Read before edit dispatches"), ReadThenEditCancelRuntime.Pump(Error));
	TestTrue(TEXT("Read before edit completes"), ReadThenEditCancelDispatcher.CompletePending(
		EHyperAIStudioPlanActionOutcome::Succeeded, CancelClock));
	TestTrue(TEXT("Cancellation before the first edit is accepted"),
		ReadThenEditCancelRuntime.RequestCancel(TEXT("cancel before first edit"), Error));
	TestEqual(TEXT("Read plus undispatched first edit closes known failed, never unknown"),
		ReadThenEditCancelRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Cancellation dispatches no edit"), ReadThenEditCancelDispatcher.DispatchCalls, 1);
	TestEqual(TEXT("Undispatched first edit emits one durable no-effect certificate"),
		ReadThenEditCancelJournal.CertifiedNoEffectCalls, 1);
	TestFalse(TEXT("Undispatched first edit never persists outcome_unknown"),
		ReadThenEditCancelJournal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::OutcomeUnknown));

	FFakeClock ReadThenEditTimeoutClock;
	FHyperAIStudioValidatedPlan ReadThenEditTimeoutPlan;
	TestTrue(TEXT("Read-then-edit timeout fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), ReadStep() + TEXT(",") + DependentBlueprintStep(),
		TEXT("operation-read-edit-timeout-between-actions"), FString(), 100),
		Registry, ReadThenEditTimeoutPlan));
	FFakeJournal ReadThenEditTimeoutJournal;
	FFakeDispatcher ReadThenEditTimeoutDispatcher;
	FHyperAIStudioPlanExecutionRuntime ReadThenEditTimeoutRuntime;
	TestTrue(TEXT("Read-then-edit timeout fixture starts"), ReadThenEditTimeoutRuntime.Start(
		ReadThenEditTimeoutPlan, Registry, TestProjectId, &ReadThenEditTimeoutJournal, nullptr,
		&CancelReceipts, &ReadThenEditTimeoutClock, &ReadThenEditTimeoutDispatcher, Error));
	TestTrue(TEXT("Read before timed edit dispatches"), ReadThenEditTimeoutRuntime.Pump(Error));
	TestTrue(TEXT("Read before timed edit completes"), ReadThenEditTimeoutDispatcher.CompletePending(
		EHyperAIStudioPlanActionOutcome::Succeeded, ReadThenEditTimeoutClock));
	ReadThenEditTimeoutClock.MonotonicMs += 101;
	ReadThenEditTimeoutRuntime.Tick();
	TestEqual(TEXT("Deadline before the first edit closes known failed, never unknown"),
		ReadThenEditTimeoutRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Deadline dispatches no edit"), ReadThenEditTimeoutDispatcher.DispatchCalls, 1);
	TestFalse(TEXT("Deadline before the first edit never persists outcome_unknown"),
		ReadThenEditTimeoutJournal.Transitions.Contains(EHyperAIStudioPlanJournalTransition::OutcomeUnknown));

	FFakeJournal RejectedDispatchJournal;
	FFakeDispatcher RejectedDispatcher;
	RejectedDispatcher.bRejectDispatch = true;
	FHyperAIStudioPlanExecutionRuntime RejectedDispatchRuntime;
	FHyperAIStudioValidatedPlan RejectedDispatchPlan;
	TestTrue(TEXT("Rejected dispatcher plan validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-dispatch-false")),
		Registry, RejectedDispatchPlan));
	TestTrue(TEXT("Rejected dispatcher fixture starts"), RejectedDispatchRuntime.Start(
		RejectedDispatchPlan, Registry, TestProjectId, &RejectedDispatchJournal, nullptr,
		&CancelReceipts, &CancelClock, &RejectedDispatcher, Error));
	TestFalse(TEXT("Synchronous dispatcher false is returned without callback"),
		RejectedDispatchRuntime.Pump(Error));
	TestEqual(TEXT("Dispatcher false closes as certified failed, not unknown"),
		RejectedDispatchRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Dispatcher false invoked no backend adapter call"), RejectedDispatcher.DispatchCalls, 0);
	TestEqual(TEXT("Dispatcher false emits one durable no-effect certificate"),
		RejectedDispatchJournal.CertifiedNoEffectCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanExecuteAuthorizationReplayTest,
	"HyperAIStudio.NativeTools.PlanExecute.AuthorizationReplayAndBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanExecuteAuthorizationReplayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanExecute::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry =
		FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	FHyperAIStudioValidatedPlan DeletePlan;
	TestTrue(TEXT("Opaque-token destructive fixture validates syntactically"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(true), TEXT("operation-destructive"),
		TEXT("opaque-server-token")), Registry, DeletePlan));
	FFakeClock Clock;
	FFakeJournal Journal;
	FFakeReceiptGate Receipts;
	FFakeDispatcher Dispatcher;
	FFakeAuthorization RejectingAuthorization;
	RejectingAuthorization.Expected = {
		TEXT("opaque-server-token"), TestProjectId, DeletePlan.OperationId,
		DeletePlan.AuthorizationPlanHash, DeletePlan.MaximumSafety};
	RejectingAuthorization.bAccept = false;
	FHyperAIStudioPlanExecutionRuntime UnauthorizedRuntime;
	FString Error;
	TestFalse(TEXT("Client token text cannot bypass the server authorization gate"), UnauthorizedRuntime.Start(
		DeletePlan, Registry, TestProjectId, &Journal, &RejectingAuthorization, &Receipts,
		&Clock, &Dispatcher, Error));
	TestEqual(TEXT("Unauthorized destructive plan never dispatches"), Dispatcher.DispatchCalls, 0);
	TestEqual(TEXT("Unauthorized destructive plan never begins the journal"), Journal.BeginCalls, 0);

	FFakeJournal ReplayJournal;
	ReplayJournal.BeginResult = EHyperAIStudioPlanJournalBegin::ReplayCompleted;
	FFakeDispatcher ReplayDispatcher;
	FFakeReceiptGate ReplayReceipts;
	FHyperAIStudioValidatedPlan ReplayPlan;
	TestTrue(TEXT("Replay fixture validates"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), BlueprintStep(false), TEXT("operation-replay")), Registry, ReplayPlan));
	FHyperAIStudioPlanExecutionRuntime ReplayRuntime;
	TestTrue(TEXT("Exact completed binding enters replay"), ReplayRuntime.Start(
		ReplayPlan, Registry, TestProjectId, &ReplayJournal, nullptr, &ReplayReceipts,
		&Clock, &ReplayDispatcher, Error));
	TestEqual(TEXT("Replay is terminal without backend dispatch"), ReplayRuntime.GetStatus().CoordinatorState,
		EHyperAIStudioPlanCoordinatorState::ReplayCompleted);
	TestEqual(TEXT("Replay never dispatches an action"), ReplayDispatcher.DispatchCalls, 0);
	TestFalse(TEXT("One runtime instance cannot be started twice"), ReplayRuntime.Start(
		ReplayPlan, Registry, TestProjectId, &ReplayJournal, nullptr, &ReplayReceipts,
		&Clock, &ReplayDispatcher, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanExecuteClosedSurfaceTest,
	"HyperAIStudio.NativeTools.PlanExecute.ClosedSurfaceAndAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanExecuteClosedSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanExecute::Tests;
	const TArray<uint8> NistAbcBytes = {0x61u, 0x62u, 0x63u};
	TestEqual(TEXT("Streaming raw-byte SHA-256 matches the NIST abc vector"),
		FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256Bytes(NistAbcBytes),
		FString(TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("UTF-8 SHA-256 uses the same standard implementation"),
		FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(TEXT("abc")),
		FString(TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	FString MalformedUtf16;
	MalformedUtf16.AppendChar(static_cast<TCHAR>(0xd800));
	TestTrue(TEXT("Malformed UTF-16 fails closed before UTF-8 conversion"),
		FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(MalformedUtf16).IsEmpty());
	TestEqual(TEXT("A proven clean successful save is the only successful classification"),
		FHyperAIStudioPlanExecuteContracts::ClassifySaveAttempt(true, false),
		EHyperAIStudioPlanActionOutcome::Succeeded);
	TestEqual(TEXT("A failed save attempt has an ambiguous disk outcome"),
		FHyperAIStudioPlanExecuteContracts::ClassifySaveAttempt(false, true),
		EHyperAIStudioPlanActionOutcome::OutcomeUnknown);
	TestEqual(TEXT("A dirty package after a nominal save remains ambiguous"),
		FHyperAIStudioPlanExecuteContracts::ClassifySaveAttempt(true, true),
		EHyperAIStudioPlanActionOutcome::OutcomeUnknown);
	const TArray<FString> Supported = FHyperAIStudioPlanExecutionRuntime::GetSupportedOperationTypes();
	TestEqual(TEXT("Executor advertises only three concrete operation backends"), Supported.Num(), 3);
	TestTrue(TEXT("Exact Epic read delegate operation is supported"), Supported.Contains(TEXT("foundation.inspect")));
	TestTrue(TEXT("Blueprint edit backend is supported"), Supported.Contains(TEXT("blueprint.apply_patch")));
	TestTrue(TEXT("Blueprint destructive backend is supported"), Supported.Contains(TEXT("blueprint.delete_patch")));
	TestFalse(TEXT("Placeholder asset update is not advertised"), Supported.Contains(TEXT("asset.apply_update")));
	TestFalse(TEXT("Placeholder asset delete is not advertised"), Supported.Contains(TEXT("asset.delete")));
	TestFalse(TEXT("Placeholder playtest is not advertised"), Supported.Contains(TEXT("playtest.run")));
	TestFalse(TEXT("Recursive HyperAI tool operation is never supported"),
		FHyperAIStudioPlanExecutionRuntime::IsOperationTypeSupported(TEXT("hyper_plan_execute")));
	FHyperAIStudioValidatedPlan UnallowlistedReadPlan;
	const FHyperAIStudioTypedOperationRegistry Registry =
		FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	TestTrue(TEXT("Generic metadata can represent a read outside the executor allowlist"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(),
		ReadStep().Replace(TEXT("editor.current_level"), TEXT("unallowlisted.subject")),
		TEXT("operation-unallowlisted-read")), Registry, UnallowlistedReadPlan));
	FString UnallowlistedError;
	TestFalse(TEXT("Unallowlisted Epic delegate variant is rejected before dispatch"),
		FHyperAIStudioPlanExecutionRuntime::ValidateExecutablePlanShape(
			UnallowlistedReadPlan, UnallowlistedError));

	FHyperAIStudioValidatedPlan AssetDeletePlan;
	TestTrue(TEXT("Metadata-only asset delete fixture still validates with an opaque token"), Validate(MakePlan(
		Registry.ComputeCapabilityHash(), AssetDeleteStep(), TEXT("operation-unavailable-asset-delete"),
		TEXT("opaque-token-asset-delete-001")), Registry, AssetDeletePlan));
	FString ShapeError;
	TestFalse(TEXT("Metadata-only placeholder cannot enter production execution"),
		FHyperAIStudioPlanExecutionRuntime::ValidateExecutablePlanShape(AssetDeletePlan, ShapeError));

	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	const FHyperAIStudioCapabilityToolDefinition* Tool = Catalog.Tools.FindByPredicate(
		[](const FHyperAIStudioCapabilityToolDefinition& Candidate)
		{
			return Candidate.Name == TEXT("hyper_plan_execute");
		});
	TestNotNull(TEXT("Generated catalog owns hyper_plan_execute admission"), Tool);
	if (Tool)
	{
		TestEqual(TEXT("Generated admission belongs to exact shared_foundation pack"),
			Tool->PackId, FString(TEXT("shared_foundation")));
		TestEqual(TEXT("Release registration is owned only by generated admitted state"),
			FHyperAIStudioPlanExecuteContracts::IsRegistrationAllowed(false),
			Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted);
	}
	if (Tool && Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate)
	{
		TestTrue(TEXT("Generated SourceCandidate can be enabled only for explicit dev automation"),
			FHyperAIStudioPlanExecuteContracts::IsRegistrationAllowed(true));
	}
	return true;
}

#endif
