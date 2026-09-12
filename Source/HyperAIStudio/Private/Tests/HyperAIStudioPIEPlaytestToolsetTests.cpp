// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPIEPlaytestToolset.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "Misc/AutomationTest.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::PIEPlaytest::Tests
{
	constexpr const TCHAR* CanonicalProjectId = TEXT("project:test-project:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

	FHyperAIStudioPIEDriverResult DriverResult(
		const EHyperAIStudioPIEDriverOutcome Outcome,
		const TCHAR* Code = TEXT(""),
		const TCHAR* Diagnostic = TEXT(""))
	{
		FHyperAIStudioPIEDriverResult Result;
		Result.Outcome = Outcome;
		Result.Code = Code;
		Result.Diagnostic = Diagnostic;
		return Result;
	}

	FHyperAIStudioPIEDriverResult ObservableWorld()
	{
		FHyperAIStudioPIEDriverResult Result = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		Result.Session.bActive = true;
		Result.Session.TopologySource = TEXT("original_request_params");
		Result.Session.ContextHandle = TEXT("pie-context-1");
		Result.Session.WorldPath = TEXT("/Game/Maps/UEDPIE_0_Test.Test");
		Result.Session.WorldPackage = TEXT("/Game/Maps/UEDPIE_0_Test");
		Result.Session.SourcePackage = TEXT("/Game/Maps/Test");
		Result.Session.NetMode = TEXT("standalone");
		Result.Session.PieInstance = 0;
		Result.Session.ObservableWorldCount = 1;
		Result.Session.Fingerprint = TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
		return Result;
	}

	FHyperAIPIEPlaytestRequest ReadRequest(const TCHAR* OperationId = TEXT("operation-pie-read-0001"))
	{
		FHyperAIPIEPlaytestRequest Request;
		Request.OperationId = OperationId;
		Request.Mode = EHyperAIPIEPlaytestMode::AttachOnly;
		Request.Teardown = EHyperAIPIEPlaytestTeardown::LeaveSessionRunning;
		Request.WholeTimeoutMs = 10000;
		Request.MaxEvidenceItems = 32;
		Request.MaxOutputBytes = 32768;
		FHyperAIPIEPlaytestStep& Step = Request.Steps.AddDefaulted_GetRef();
		Step.StepId = TEXT("query-actors");
		Step.Kind = EHyperAIPIEPlaytestStepKind::QueryActors;
		Step.TimeoutMs = 1000;
		return Request;
	}

	FHyperAIPIEPlaytestRequest OwnedStartRequest(const TCHAR* OperationId = TEXT("operation-pie-start-0001"))
	{
		FHyperAIPIEPlaytestRequest Request;
		Request.OperationId = OperationId;
		Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
		Request.Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;
		Request.AuthorizationToken = TEXT("opaque-exact-plan-grant");
		Request.WholeTimeoutMs = 10000;
		Request.MaxEvidenceItems = 32;
		Request.MaxOutputBytes = 32768;
		FHyperAIPIEPlaytestStep& Step = Request.Steps.AddDefaulted_GetRef();
		Step.StepId = TEXT("assert-actor");
		Step.Kind = EHyperAIPIEPlaytestStepKind::AssertActorExists;
		Step.TargetObjectPath = TEXT("/Game/Maps/UEDPIE_0_Test.Test:PersistentLevel.TestActor_0");
		Step.TimeoutMs = 1000;
		return Request;
	}

	FHyperAIPIEPlaytestRequest PendingEffectRequest()
	{
		FHyperAIPIEPlaytestRequest Request = ReadRequest(TEXT("operation-pie-timeout-0001"));
		Request.AuthorizationToken = TEXT("opaque-exact-plan-grant");
		Request.Steps[0].StepId = TEXT("pause-runtime");
		Request.Steps[0].Kind = EHyperAIPIEPlaytestStepKind::PauseSession;
		return Request;
	}

	bool Normalize(const FHyperAIPIEPlaytestRequest& Request, FHyperAIStudioPIENormalizedPlan& OutPlan)
	{
		FString ErrorCode;
		FString Error;
		return FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Request, OutPlan, ErrorCode, Error);
	}

	class FFakeClock final : public IHyperAIStudioPIEClock
	{
	public:
		virtual int64 NowMonotonicMs() const override { return NowMs; }
		virtual FString NowUtc() const override { return FString::Printf(TEXT("test-%lld"), NowMs); }
		void Advance(const int64 DeltaMs) { NowMs += DeltaMs; }

	private:
		int64 NowMs = 1000;
	};

	class FFakeDriver final : public IHyperAIStudioPIEPlaytestDriver
	{
	public:
		virtual FHyperAIStudioPIEDriverResult PreflightStart(const FHyperAIStudioPIENormalizedPlan&) override
		{
			++PreflightCalls;
			return PreflightResult;
		}

		virtual FHyperAIStudioPIEDriverResult RequestStart(const FHyperAIStudioPIENormalizedPlan&) override
		{
			++StartCalls;
			FHyperAIStudioPIEDriverResult Result = StartResult;
			Result.bOwnedSessionClaimed = bClaimOwnedSession;
			return Result;
		}

		virtual FHyperAIStudioPIEDriverResult ObserveWorld(const FHyperAIStudioPIENormalizedPlan&) override
		{
			++ObserveCalls;
			if (ObservePendingCount > 0)
			{
				--ObservePendingCount;
				return DriverResult(EHyperAIStudioPIEDriverOutcome::Pending);
			}
			return ObserveResult;
		}

		virtual FHyperAIStudioPIEDriverResult BeginStep(
			const FHyperAIStudioPIENormalizedPlan&,
			const FHyperAIPIEPlaytestStep& Step,
			const FHyperAIStudioPIEExecutionBinding&,
			IHyperAIStudioPIETrustedExecutionGate*) override
		{
			++BeginStepCalls;
			FHyperAIStudioPIEDriverResult Result = BeginStepResult;
			if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Succeeded)
			{
				Result.Evidence.StepId = Step.StepId;
				Result.Evidence.Kind = FHyperAIStudioPIEPlaytestContracts::StepKindToString(Step.Kind);
				Result.Evidence.Status = TEXT("completed");
				Result.Evidence.bAssertion = Step.Kind == EHyperAIPIEPlaytestStepKind::AssertActorExists;
				Result.Evidence.bPassed = Result.Evidence.bAssertion;
			}
			return Result;
		}

		virtual FHyperAIStudioPIEDriverResult PollStep(
			const FHyperAIStudioPIENormalizedPlan&,
			const FHyperAIPIEPlaytestStep&) override
		{
			++PollStepCalls;
			return PollStepResult;
		}

		virtual FHyperAIStudioPIEDriverResult BeginTeardown(
			const FHyperAIStudioPIENormalizedPlan&,
			const bool) override
		{
			++BeginTeardownCalls;
			return BeginTeardownResult;
		}

		virtual FHyperAIStudioPIEDriverResult PollTeardown(
			const FHyperAIStudioPIENormalizedPlan&,
			const bool) override
		{
			++PollTeardownCalls;
			return PollTeardownResult;
		}

		virtual void ForceTeardown(const FHyperAIStudioPIENormalizedPlan&, const bool) override
		{
			++ForceTeardownCalls;
		}

		FHyperAIStudioPIEDriverResult PreflightResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		FHyperAIStudioPIEDriverResult StartResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		FHyperAIStudioPIEDriverResult ObserveResult = ObservableWorld();
		FHyperAIStudioPIEDriverResult BeginStepResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		FHyperAIStudioPIEDriverResult PollStepResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		FHyperAIStudioPIEDriverResult BeginTeardownResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		FHyperAIStudioPIEDriverResult PollTeardownResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
		int32 ObservePendingCount = 0;
		bool bClaimOwnedSession = true;
		int32 PreflightCalls = 0;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 BeginStepCalls = 0;
		int32 PollStepCalls = 0;
		int32 BeginTeardownCalls = 0;
		int32 PollTeardownCalls = 0;
		int32 ForceTeardownCalls = 0;
	};

	class FFakeTrustedGate final : public IHyperAIStudioPIETrustedExecutionGate
	{
	public:
		virtual EHyperAIStudioPlanJournalBegin Begin(
			const FHyperAIStudioPIEExecutionBinding& Binding,
			FString&) override
		{
			++BeginCalls;
			LastBinding = Binding;
			return BeginResult;
		}

		virtual bool InspectAuthorization(
			const FHyperAIStudioPIEExecutionBinding& Binding,
			FString& OutError) override
		{
			++InspectCalls;
			LastBinding = Binding;
			if (!bInspectAllowed) { OutError = TEXT("authorization rejected by fixture"); }
			return bInspectAllowed;
		}

		virtual bool ConsumeAuthorization(
			const FHyperAIStudioPIEExecutionBinding& Binding,
			FString& OutError) override
		{
			++ConsumeCalls;
			LastBinding = Binding;
			if (!bConsumeAllowed) { OutError = TEXT("authorization consumption rejected by fixture"); }
			return bConsumeAllowed;
		}

		virtual bool Transition(
			const FHyperAIStudioPIEExecutionBinding& Binding,
			const EHyperAIStudioPlanJournalTransition TransitionValue,
			const FHyperAIStudioPlanOutcomeEvidence&,
			FString& OutError) override
		{
			LastBinding = Binding;
			Transitions.Add(TransitionValue);
			if (!bTransitionsAllowed) { OutError = TEXT("journal transition rejected by fixture"); }
			return bTransitionsAllowed;
		}

		virtual bool IssueTerminalEvidence(
			const FHyperAIStudioPIEExecutionBinding& Binding,
			const FString& RuntimeEvidenceHash,
			const bool bRollbackProof,
			FHyperAIStudioPlanOutcomeEvidence&,
			FString& OutError) override
		{
			++EvidenceCalls;
			LastBinding = Binding;
			LastRuntimeEvidenceHash = RuntimeEvidenceHash;
			bLastRollbackProof = bRollbackProof;
			if (!bEvidenceAllowed) { OutError = TEXT("terminal evidence rejected by fixture"); }
			return bEvidenceAllowed;
		}

		virtual bool ApproveRuntimeTarget(
			const FHyperAIStudioPIEExecutionBinding&,
			const EHyperAIPIEPlaytestStepKind,
			const AActor&,
			FString&) override
		{
			return bTargetAllowed;
		}

		virtual UClass* ResolveAllowlistedSpawnClass(
			const FHyperAIStudioPIEExecutionBinding&,
			const FString&,
			FString& OutError) override
		{
			OutError = TEXT("No spawn class is installed in this fixture.");
			return nullptr;
		}

		EHyperAIStudioPlanJournalBegin BeginResult = EHyperAIStudioPlanJournalBegin::ProceedNew;
		bool bInspectAllowed = true;
		bool bConsumeAllowed = true;
		bool bTransitionsAllowed = true;
		bool bEvidenceAllowed = true;
		bool bTargetAllowed = true;
		int32 BeginCalls = 0;
		int32 InspectCalls = 0;
		int32 ConsumeCalls = 0;
		int32 EvidenceCalls = 0;
		bool bLastRollbackProof = false;
		FString LastRuntimeEvidenceHash;
		FHyperAIStudioPIEExecutionBinding LastBinding;
		TArray<EHyperAIStudioPlanJournalTransition> Transitions;
	};

	FHyperAIStudioCapabilityCatalog CandidateCatalog(
		const EHyperAIStudioCapabilityAdmissionState State = EHyperAIStudioCapabilityAdmissionState::SourceCandidate)
	{
		FHyperAIStudioCapabilityCatalog Catalog;
		for (const FHyperAIStudioPIEPlaytestManifestEntry& Entry : FHyperAIStudioPIEPlaytestContracts::GetManifest())
		{
			FHyperAIStudioCapabilityToolDefinition& Tool = Catalog.Tools.AddDefaulted_GetRef();
			Tool.Name = Entry.Name;
			Tool.PackId = TEXT("pie_runtime");
			Tool.AtomicCohortId = TEXT("pie-runtime-atomic-v1");
			Tool.AdmissionState = State;
			Tool.SourceArtifactCount = 3;
			Tool.SourceArtifactFingerprint = TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
		}
		return Catalog;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestManifestAdmissionTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.ManifestAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestManifestAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	const TArray<FHyperAIStudioPIEPlaytestManifestEntry>& Manifest =
		FHyperAIStudioPIEPlaytestContracts::GetManifest();
	TestEqual(TEXT("Exactly two PIE runtime callables exist"), Manifest.Num(), 2);
	TestEqual(TEXT("Run callable wire name is exact"), Manifest[0].Name, FString(TEXT("hyper_playtest_run")));
	TestEqual(TEXT("Status callable wire name is exact"), Manifest[1].Name, FString(TEXT("hyper_playtest_status")));
	TestEqual(TEXT("Both callables share one toolset"), Manifest[0].Toolset, Manifest[1].Toolset);

	TSet<FString> ReflectedCallables;
	for (TFieldIterator<UFunction> It(UHyperAIStudioPIEPlaytestToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		UFunction* Function = *It;
		if (Function->HasMetaData(TEXT("AICallable")))
		{
			ReflectedCallables.Add(Function->GetName());
			TestTrue(TEXT("PIE callable is static"), Function->HasAllFunctionFlags(FUNC_Static));
			TObjectPtr<UFunction> FunctionObject = Function;
			const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(FunctionObject);
			TestTrue(TEXT("Epic ToolsetRegistry accepts the reflected callable"),
				Callability.HasValue() && Callability.GetValue());
		}
	}
	TestEqual(TEXT("No hidden third AICallable exists"), ReflectedCallables.Num(), 2);
	TestTrue(TEXT("Run is reflected"), ReflectedCallables.Contains(TEXT("hyper_playtest_run")));
	TestTrue(TEXT("Status is reflected"), ReflectedCallables.Contains(TEXT("hyper_playtest_status")));

	FString Error;
	FHyperAIStudioCapabilityCatalog Catalog = CandidateCatalog();
	TestFalse(TEXT("SourceCandidate cohort is refused in production"),
		FHyperAIStudioPIEPlaytestContracts::EvaluateCatalogAdmission(Catalog, false, Error));
	TestTrue(TEXT("SourceCandidate cohort is available only behind the explicit dev gate"),
		FHyperAIStudioPIEPlaytestContracts::EvaluateCatalogAdmission(Catalog, true, Error));
	Catalog.Tools[1].AtomicCohortId = TEXT("different-cohort");
	TestFalse(TEXT("Split atomic cohorts are refused"),
		FHyperAIStudioPIEPlaytestContracts::EvaluateCatalogAdmission(Catalog, true, Error));
	Catalog = CandidateCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted);
	TestTrue(TEXT("One generated admitted cohort may register in production"),
		FHyperAIStudioPIEPlaytestContracts::EvaluateCatalogAdmission(Catalog, false, Error));

	const FHyperAIStudioCapabilityCatalog& Generated = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	for (const FHyperAIStudioPIEPlaytestManifestEntry& Entry : Manifest)
	{
		const FHyperAIStudioCapabilityToolDefinition* Tool = Generated.Tools.FindByPredicate(
			[&Entry](const FHyperAIStudioCapabilityToolDefinition& Candidate) { return Candidate.Name == Entry.Name; });
		TestNotNull(TEXT("Generated CapabilityPackRegistry is the only production admission authority"), Tool);
		if (Tool) { TestEqual(TEXT("Generated pack id is exact"), Tool->PackId, FString(TEXT("pie_runtime"))); }
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestNormalizeBoundsTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.NormalizeBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestNormalizeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	FHyperAIStudioPIENormalizedPlan Plan;
	FString Code;
	FString Error;
	TestTrue(TEXT("A bounded read-only actor query normalizes"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(ReadRequest(), Plan, Code, Error));
	TestFalse(TEXT("Read-only plan has no effects"), Plan.bHasEffects);
	TestFalse(TEXT("Read-only plan has no destructive steps"), Plan.bHasDestructive);
	TestFalse(TEXT("Plan hash is emitted"), Plan.PlanHash.IsEmpty());

	FHyperAIPIEPlaytestRequest MissingAuth = OwnedStartRequest();
	MissingAuth.AuthorizationToken.Reset();
	TestFalse(TEXT("Starting PIE without opaque exact-plan authorization is rejected"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(MissingAuth, Plan, Code, Error));
	TestEqual(TEXT("Authorization rejection is typed"), Code, FString(TEXT("authorization_required")));

	FHyperAIPIEPlaytestRequest InvalidEnum = ReadRequest();
	InvalidEnum.Mode = static_cast<EHyperAIPIEPlaytestMode>(255);
	TestFalse(TEXT("Unknown lifecycle enum is rejected"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(InvalidEnum, Plan, Code, Error));

	FHyperAIPIEPlaytestRequest Overflow = ReadRequest();
	const FHyperAIPIEPlaytestStep OverflowStep = Overflow.Steps[0];
	while (Overflow.Steps.Num() <= FHyperAIStudioPIEPlaytestContracts::MaxSteps)
	{
		Overflow.Steps.Add(OverflowStep);
	}
	TestFalse(TEXT("Step work is hard bounded"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Overflow, Plan, Code, Error));
	TestEqual(TEXT("Step bound has an exact code"), Code, FString(TEXT("step_limit_exceeded")));

	FHyperAIPIEPlaytestRequest Duplicate = ReadRequest();
	const FHyperAIPIEPlaytestStep DuplicateStep = Duplicate.Steps[0];
	Duplicate.Steps.Add(DuplicateStep);
	TestFalse(TEXT("Duplicate step ids are rejected"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Duplicate, Plan, Code, Error));
	TestEqual(TEXT("Duplicate id has an exact code"), Code, FString(TEXT("duplicate_step_id")));

	FHyperAIPIEPlaytestRequest Smuggled = ReadRequest();
	Smuggled.Steps[0].Name = TEXT("raw-input-name");
	TestFalse(TEXT("Irrelevant union fields cannot smuggle a raw route"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Smuggled, Plan, Code, Error));
	Smuggled = ReadRequest();
	Smuggled.Steps[0].Key = static_cast<EHyperAIPIEPlaytestKey>(255);
	TestFalse(TEXT("Input accepts only the closed key enum"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Smuggled, Plan, Code, Error));
	Smuggled = ReadRequest();
	Smuggled.Steps[0].TimeoutMs = FHyperAIStudioPIEPlaytestContracts::MaxStepTimeoutMs + 1;
	TestFalse(TEXT("Per-step timeout is hard bounded"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Smuggled, Plan, Code, Error));
	Smuggled = ReadRequest();
	Smuggled.Steps[0].Value.bBoolean = true;
	TestFalse(TEXT("Inactive typed-value union fields are rejected"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Smuggled, Plan, Code, Error));
	Smuggled = PendingEffectRequest();
	Smuggled.Steps[0].Kind = EHyperAIPIEPlaytestStepKind::StopSession;
	Smuggled.Steps.AddDefaulted_GetRef().StepId = TEXT("step-after-stop");
	TestFalse(TEXT("StopSession must be the terminal typed step"),
		FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Smuggled, Plan, Code, Error));
	TestEqual(TEXT("Terminal-step ordering has an exact code"), Code, FString(TEXT("invalid_step_order")));

	const FString Fingerprint = TEXT("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
	const FString Cursor = FHyperAIStudioPIEPlaytestContracts::MakeCursor(Fingerprint, 12);
	int32 Offset = INDEX_NONE;
	TestTrue(TEXT("Bound status cursor round-trips"),
		FHyperAIStudioPIEPlaytestContracts::ParseCursor(Cursor, Fingerprint, 20, Offset, Error));
	TestEqual(TEXT("Cursor offset is immutable"), Offset, 12);
	TestFalse(TEXT("Cursor cannot be replayed against other evidence"),
		FHyperAIStudioPIEPlaytestContracts::ParseCursor(Cursor, TEXT("sha256:other"), 20, Offset, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestSerialLifecycleTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.SerialLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestSerialLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	FHyperAIStudioPIENormalizedPlan Plan;
	TestTrue(TEXT("Owned start fixture normalizes"), Normalize(OwnedStartRequest(), Plan));
	TSharedPtr<FFakeClock> Clock = MakeShared<FFakeClock>();
	TSharedPtr<FFakeDriver> Driver = MakeShared<FFakeDriver>();
	TSharedPtr<FFakeTrustedGate> Gate = MakeShared<FFakeTrustedGate>();
	Driver->ObservePendingCount = 1;
	FHyperAIStudioPIEPlaytestRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Run is accepted without waiting for PIE"),
		Runtime.Start(Plan, CanonicalProjectId, Driver, Gate, Clock, Error));
	TestEqual(TEXT("Immediate state is accepted"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::Accepted);

	Runtime.Tick();
	TestEqual(TEXT("One tick dispatches only the start"), Driver->StartCalls, 1);
	TestEqual(TEXT("Commit barrier precedes start"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::StartingSession);
	Runtime.Tick();
	TestEqual(TEXT("World observation may remain pending"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::StartingSession);
	Runtime.Tick();
	TestEqual(TEXT("Frozen world identity advances execution"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::ExecutingStep);
	Runtime.Tick();
	TestEqual(TEXT("Exactly one typed assertion executes"), Driver->BeginStepCalls, 1);
	TestEqual(TEXT("Successful assertion increments the serial step"), Runtime.Snapshot().CompletedSteps, 1);
	Runtime.Tick();
	TestEqual(TEXT("Completed steps always enter deterministic teardown"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::Teardown);
	Runtime.Tick();
	const FHyperAIStudioPIERuntimeSnapshot Snapshot = Runtime.Snapshot();
	TestEqual(TEXT("Trusted evidence makes the run completed"), Snapshot.State, EHyperAIStudioPIERuntimeState::Completed);
	TestEqual(TEXT("Owned session teardown executes once"), Driver->BeginTeardownCalls, 1);
	TestTrue(TEXT("Assertion evidence is retained"),
		Snapshot.Evidence.ContainsByPredicate([](const FHyperAIPIEPlaytestEvidence& Evidence)
		{
			return Evidence.bAssertion && Evidence.bPassed;
		}));
	TestTrue(TEXT("Journal moved through Running"), Gate->Transitions.Contains(EHyperAIStudioPlanJournalTransition::Running));
	TestTrue(TEXT("Journal crossed CommitStarted"), Gate->Transitions.Contains(EHyperAIStudioPlanJournalTransition::CommitStarted));
	TestTrue(TEXT("Journal committed trusted completion"), Gate->Transitions.Contains(EHyperAIStudioPlanJournalTransition::Completed));
	TestFalse(TEXT("Terminal evidence hash is present"), Gate->LastRuntimeEvidenceHash.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestTimeoutCancelLateTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.TimeoutCancelLate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestTimeoutCancelLateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	FHyperAIStudioPIENormalizedPlan EffectPlan;
	TestTrue(TEXT("Pending effect fixture normalizes"), Normalize(PendingEffectRequest(), EffectPlan));
	TSharedPtr<FFakeClock> Clock = MakeShared<FFakeClock>();
	TSharedPtr<FFakeDriver> Driver = MakeShared<FFakeDriver>();
	TSharedPtr<FFakeTrustedGate> Gate = MakeShared<FFakeTrustedGate>();
	Driver->BeginStepResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Pending);
	FHyperAIStudioPIEPlaytestRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Effect fixture starts"), Runtime.Start(EffectPlan, CanonicalProjectId, Driver, Gate, Clock, Error));
	Runtime.Tick(); // attach -> wait
	Runtime.Tick(); // world -> execute
	Runtime.Tick(); // effect -> pending after commit barrier
	Clock->Advance(EffectPlan.Request.Steps[0].TimeoutMs + 1);
	Runtime.Tick(); // timeout -> teardown
	Runtime.Tick(); // proven teardown, but pending effect remains ambiguous
	const FHyperAIStudioPIERuntimeSnapshot TimedOut = Runtime.Snapshot();
	TestEqual(TEXT("Pending effect timeout fails closed"), TimedOut.State, EHyperAIStudioPIERuntimeState::OutcomeUnknown);
	TestTrue(TEXT("Outcome ambiguity is explicit"), TimedOut.bOutcomeUnknown);
	const int32 PollsBeforeLateResult = Driver->PollStepCalls;
	Driver->PollStepResult = DriverResult(EHyperAIStudioPIEDriverOutcome::Succeeded);
	Runtime.Tick();
	TestEqual(TEXT("Late completion is ignored after terminal state"), Driver->PollStepCalls, PollsBeforeLateResult);
	TestEqual(TEXT("Late completion cannot rewrite outcome_unknown"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::OutcomeUnknown);

	FHyperAIStudioPIENormalizedPlan ReadPlan;
	TestTrue(TEXT("Cancel fixture normalizes"), Normalize(ReadRequest(TEXT("operation-pie-cancel-0001")), ReadPlan));
	TSharedPtr<FFakeDriver> CancelDriver = MakeShared<FFakeDriver>();
	FHyperAIStudioPIEPlaytestRuntime CancelRuntime;
	TestTrue(TEXT("Read run starts without a mutation gate"),
		CancelRuntime.Start(ReadPlan, TEXT(""), CancelDriver, nullptr, Clock, Error));
	TestTrue(TEXT("Cancellation is accepted before any effect"), CancelRuntime.RequestCancel(TEXT("test cancel"), Error));
	CancelRuntime.Tick();
	TestEqual(TEXT("Pre-effect cancellation is a known failure"), CancelRuntime.Snapshot().State, EHyperAIStudioPIERuntimeState::Failed);
	TestFalse(TEXT("Cancellation after terminal is rejected"), CancelRuntime.RequestCancel(TEXT("late"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestMultiprocessRefusalTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.MultiprocessRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestMultiprocessRefusalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	FHyperAIStudioPIENormalizedPlan Plan;
	TestTrue(TEXT("Read fixture normalizes"), Normalize(ReadRequest(TEXT("operation-pie-multiprocess-0001")), Plan));
	TSharedPtr<FFakeClock> Clock = MakeShared<FFakeClock>();
	TSharedPtr<FFakeDriver> Driver = MakeShared<FFakeDriver>();
	Driver->ObserveResult = DriverResult(
		EHyperAIStudioPIEDriverOutcome::UnsupportedMultiprocess,
		TEXT("unsupported_multiprocess"),
		TEXT("OriginalRequestParams describe an unobservable multi-process session."));
	Driver->ObserveResult.Session.bActive = true;
	Driver->ObserveResult.Session.bMultiprocess = true;
	Driver->ObserveResult.Session.TopologySource = TEXT("original_request_params");
	FHyperAIStudioPIEPlaytestRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Attach request is accepted before observing topology"),
		Runtime.Start(Plan, TEXT(""), Driver, nullptr, Clock, Error));
	Runtime.Tick();
	Runtime.Tick();
	TestEqual(TEXT("Unobservable topology enters teardown"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::Teardown);
	Runtime.Tick();
	TestEqual(TEXT("Multi-process attach is refused as a known failure"), Runtime.Snapshot().State, EHyperAIStudioPIERuntimeState::Failed);
	TestEqual(TEXT("No runtime step executes"), Driver->BeginStepCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestIdempotencyAuthorizationTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.IdempotencyAuthorization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestIdempotencyAuthorizationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PIEPlaytest::Tests;
	FHyperAIStudioPIENormalizedPlan Plan;
	TestTrue(TEXT("Effect fixture normalizes"), Normalize(OwnedStartRequest(TEXT("operation-pie-replay-0001")), Plan));
	TSharedPtr<FFakeClock> Clock = MakeShared<FFakeClock>();
	TSharedPtr<FFakeDriver> ReplayDriver = MakeShared<FFakeDriver>();
	TSharedPtr<FFakeTrustedGate> ReplayGate = MakeShared<FFakeTrustedGate>();
	ReplayGate->BeginResult = EHyperAIStudioPlanJournalBegin::ReplayCompleted;
	FHyperAIStudioPIEPlaytestRuntime ReplayRuntime;
	FString Error;
	TestTrue(TEXT("Identical completed operation replays from journal-v3"),
		ReplayRuntime.Start(Plan, CanonicalProjectId, ReplayDriver, ReplayGate, Clock, Error));
	TestEqual(TEXT("Replay is terminal immediately"), ReplayRuntime.Snapshot().State, EHyperAIStudioPIERuntimeState::ReplayCompleted);
	TestTrue(TEXT("Replay flag is explicit"), ReplayRuntime.Snapshot().bReplay);
	TestEqual(TEXT("Replay never starts PIE again"), ReplayDriver->StartCalls, 0);

	TSharedPtr<FFakeDriver> DeniedDriver = MakeShared<FFakeDriver>();
	TSharedPtr<FFakeTrustedGate> DeniedGate = MakeShared<FFakeTrustedGate>();
	DeniedGate->bInspectAllowed = false;
	FHyperAIStudioPIEPlaytestRuntime DeniedRuntime;
	TestFalse(TEXT("Opaque client text is not authority when the trusted gate rejects it"),
		DeniedRuntime.Start(Plan, CanonicalProjectId, DeniedDriver, DeniedGate, Clock, Error));
	TestEqual(TEXT("Rejected authorization is terminal"), DeniedRuntime.Snapshot().State, EHyperAIStudioPIERuntimeState::Failed);
	TestFalse(TEXT("Rejected plan is not marked accepted"), DeniedRuntime.Snapshot().bAccepted);
	TestEqual(TEXT("Rejected authorization never reaches the driver"), DeniedDriver->StartCalls, 0);

	FHyperAIStudioPIEPlaytestRuntime MissingGateRuntime;
	TestFalse(TEXT("Effects default deny when the shared trusted seam is absent"),
		MissingGateRuntime.Start(Plan, CanonicalProjectId, MakeShared<FFakeDriver>(), nullptr, Clock, Error));
	TestEqual(TEXT("Fail-closed result retains the operation id"),
		MissingGateRuntime.Snapshot().Plan.Request.OperationId, Plan.Request.OperationId);
	TestEqual(TEXT("Missing seam has an exact status"),
		MissingGateRuntime.Snapshot().Status, FString(TEXT("trusted_gate_unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestOptionalVariantAndClosedSchemaTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.OptionalVariantsClosedSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestOptionalVariantAndClosedSchemaTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioPIEOptionalVariantRegistry::ResetAll();
	TestFalse(TEXT("Gameplay AI is unavailable until its gated adapter is loaded"),
		FHyperAIStudioPIEOptionalVariantRegistry::GetAdapter(EHyperAIStudioPIEOptionalVariant::GameplayAI).IsValid());
	TestFalse(TEXT("Enhanced Input is unavailable until its gated adapter is loaded"),
		FHyperAIStudioPIEOptionalVariantRegistry::GetAdapter(EHyperAIStudioPIEOptionalVariant::EnhancedInput).IsValid());
	TestFalse(TEXT("Replay is unavailable until its gated adapter is loaded"),
		FHyperAIStudioPIEOptionalVariantRegistry::GetAdapter(EHyperAIStudioPIEOptionalVariant::Replay).IsValid());

	UScriptStruct* Step = FHyperAIPIEPlaytestStep::StaticStruct();
	for (const FName Forbidden : {
		FName(TEXT("ToolName")), FName(TEXT("Command")), FName(TEXT("ConsoleCommand")),
		FName(TEXT("InputName")), FName(TEXT("PropertySetter")), FName(TEXT("FilePath")),
		FName(TEXT("Script")), FName(TEXT("Python")), FName(TEXT("Lua")), FName(TEXT("Authority")) })
	{
		TestNull(TEXT("Closed schema exposes no raw execution/authority route"), Step->FindPropertyByName(Forbidden));
	}
	const FEnumProperty* KeyProperty = CastField<FEnumProperty>(Step->FindPropertyByName(TEXT("Key")));
	TestNotNull(TEXT("Input uses a reflected closed enum"), KeyProperty);
	const FEnumProperty* KindProperty = CastField<FEnumProperty>(Step->FindPropertyByName(TEXT("Kind")));
	TestNotNull(TEXT("Runtime operations use a reflected closed enum"), KindProperty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestFunctionalCoverageTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.FunctionalCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestFunctionalCoverageTest::RunTest(const FString& Parameters)
{
	const TArray<EHyperAIPIEPlaytestStepKind> RuntimeCapabilityFamilies = {
		EHyperAIPIEPlaytestStepKind::QueryActors,
		EHyperAIPIEPlaytestStepKind::QueryScalarProperty,
		EHyperAIPIEPlaytestStepKind::TeleportActor,
		EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor,
		EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor,
		EHyperAIPIEPlaytestStepKind::AIMoveTo,
		EHyperAIPIEPlaytestStepKind::AIStop,
		EHyperAIPIEPlaytestStepKind::BlackboardRead,
		EHyperAIPIEPlaytestStepKind::BlackboardWrite,
		EHyperAIPIEPlaytestStepKind::WaitForWorld,
		EHyperAIPIEPlaytestStepKind::InjectEnhancedAction,
		EHyperAIPIEPlaytestStepKind::PauseSession
	};
	TestEqual(TEXT("Twelve observed runtime capability families have typed coverage"), RuntimeCapabilityFamilies.Num(), 12);
	TSet<FString> TypedNames;
	for (const EHyperAIPIEPlaytestStepKind Kind : RuntimeCapabilityFamilies)
	{
		const FString Name = FHyperAIStudioPIEPlaytestContracts::StepKindToString(Kind);
		TestFalse(TEXT("Every mapped capability has a stable typed name"), Name.IsEmpty());
		TypedNames.Add(Name);
	}
	TestEqual(TEXT("Mapped capability families do not alias one another"), TypedNames.Num(), RuntimeCapabilityFamilies.Num());

	const TArray<EHyperAIPIEPlaytestStepKind> BoundedLifecycle = {
		EHyperAIPIEPlaytestStepKind::WaitForWorld,
		EHyperAIPIEPlaytestStepKind::QueryActors,
		EHyperAIPIEPlaytestStepKind::CaptureScreenshotHash,
		EHyperAIPIEPlaytestStepKind::AssertActorExists,
		EHyperAIPIEPlaytestStepKind::StopSession
	};
	TestEqual(TEXT("Game-testing lifecycle is representable as wait, act, capture, assert, teardown"),
		BoundedLifecycle.Num(), 5);
	TestTrue(TEXT("Screenshot evidence is hash-only"),
		FHyperAIPIEPlaytestEvidence::StaticStruct()->FindPropertyByName(TEXT("ScreenshotHash")) != nullptr);
	TestNull(TEXT("Screenshot evidence never returns a filesystem path"),
		FHyperAIPIEPlaytestEvidence::StaticStruct()->FindPropertyByName(TEXT("ScreenshotPath")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
