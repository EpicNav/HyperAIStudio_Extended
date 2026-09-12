// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioTypedArtifactExecution.h"
#include "HyperAIStudioTypedArtifactExecutionService.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioTypedPlan.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::TypedArtifact::Tests
{
	constexpr const TCHAR* RequestType = TEXT("hyperai.payload.test.typed_artifact");
	constexpr const TCHAR* ResultType = TEXT("hyperai.result.test.typed_result");
	constexpr const TCHAR* RequestSchema = TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");
	constexpr const TCHAR* ResultSchema = TEXT("sha256:2222222222222222222222222222222222222222222222222222222222222222");
	constexpr const TCHAR* SemanticFingerprint = TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333");
	constexpr const TCHAR* PostconditionHash = TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444");
	constexpr const TCHAR* AuthorizationToken = TEXT("server-issued-artifact-token-0001");

	class FTestClock : public IHyperAIStudioTypedArtifactClock
	{
	public:
		FTestClock()
		{
			const FDateTime Now = FDateTime::UtcNow();
			UtcMs = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
		}
		virtual int64 NowMonotonicMs() const override { return MonotonicMs; }
		virtual int64 NowUtcMs() const override { return UtcMs; }
		void Advance(const int64 Milliseconds)
		{
			MonotonicMs += Milliseconds;
			UtcMs += Milliseconds;
		}
		int64 MonotonicMs = 1000;
		int64 UtcMs = 0;
	};

	class FReentrantTestClock final : public FTestClock
	{
	public:
		virtual int64 NowMonotonicMs() const override
		{
			if (Callback && !bInsideCallback)
			{
				TGuardValue<bool> Guard(bInsideCallback, true);
				Callback();
			}
			return MonotonicMs;
		}
		mutable TFunction<void()> Callback;
		mutable bool bInsideCallback = false;
	};

	class FTestArtifactPayload final : public IHyperAIStudioTypedArtifactPayload
	{
	public:
		explicit FTestArtifactPayload(FString InSemantic = SemanticFingerprint)
			: Semantic(MoveTemp(InSemantic)) {}
		virtual FString GetTypeId() const override { return RequestType; }
		virtual FString GetSchemaFingerprint() const override { return RequestSchema; }
		virtual int32 GetBoundedByteSize() const override { return 64; }
		virtual FString GetSemanticFingerprint() const override { return Semantic; }
		virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override
		{
			return MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>(Semantic);
		}
		void SetSemantic(FString InSemantic) { Semantic = MoveTemp(InSemantic); }
	private:
		FString Semantic;
	};

	class FTestResultPayload final : public IHyperAIStudioDomainResultPayload
	{
	public:
		virtual FString GetTypeId() const override { return ResultType; }
		virtual FString GetSchemaFingerprint() const override { return ResultSchema; }
		virtual int32 GetBoundedByteSize() const override { return 32; }
	};

	class FTestAuthorizationGate final : public IHyperAIStudioDomainAuthorizationGate
	{
	public:
		virtual bool Inspect(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++InspectCount;
			if (Request.OpaqueToken != AuthorizationToken
				|| (bBound && !SameRequest(Request, BoundRequest)))
			{
				OutError = TEXT("test_server_token_rejected");
				return false;
			}
			if (!bBound)
			{
				bBound = true;
				BoundRequest = Request;
				IssuedUtcMs = NowUtcMs;
				ExpiresUtcMs = NowUtcMs + 60000;
			}
			LastRequest = Request;
			OutReceipt.BoundRequest = Request;
			OutReceipt.Nonce = TEXT("server-artifact-nonce-0001");
			OutReceipt.IssuedUtcMs = IssuedUtcMs;
			OutReceipt.ExpiresUtcMs = ExpiresUtcMs;
			OutReceipt.bConsumed = bConsumed;
			return true;
		}
		virtual bool Consume(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			if (!Inspect(Request, NowUtcMs, OutReceipt, OutError) || bConsumed)
			{
				OutError = TEXT("test_server_token_already_consumed");
				return false;
			}
			++CallCount;
			bConsumed = true;
			OutReceipt.bConsumed = true;
			return true;
		}
		FHyperAIStudioDomainAuthorizationRequest LastRequest;
		int32 InspectCount = 0;
		int32 CallCount = 0;
		bool bConsumed = false;

	private:
		static bool SameRequest(
			const FHyperAIStudioDomainAuthorizationRequest& A,
			const FHyperAIStudioDomainAuthorizationRequest& B)
		{
			return A.OpaqueToken == B.OpaqueToken
				&& A.CanonicalProjectId == B.CanonicalProjectId
				&& A.OperationId == B.OperationId
				&& A.PlanHash == B.PlanHash
				&& A.PackId == B.PackId
				&& A.ToolName == B.ToolName
				&& A.VariantId == B.VariantId
				&& A.AdapterFingerprint == B.AdapterFingerprint
				&& A.AdmissionFingerprint == B.AdmissionFingerprint
				&& A.PrerequisiteFingerprint == B.PrerequisiteFingerprint
				&& A.Safety == B.Safety
				&& A.AdapterGeneration == B.AdapterGeneration
				&& A.RegistryEpoch == B.RegistryEpoch;
		}
		FHyperAIStudioDomainAuthorizationRequest BoundRequest;
		int64 IssuedUtcMs = 0;
		int64 ExpiresUtcMs = 0;
		bool bBound = false;
	};

	class FTestAdapter final : public IHyperAIStudioDomainAdapter
	{
	public:
		explicit FTestAdapter(FHyperAIStudioDomainAdapterDescriptor InDescriptor)
			: Descriptor(MoveTemp(InDescriptor)) {}
		virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override
		{
			return Descriptor;
		}
		virtual FHyperAIStudioDomainAdapterResult Execute(
			const FHyperAIStudioDomainDispatchContext& Context,
			const IHyperAIStudioDomainRequestPayload& Payload) override
		{
			++ActiveCallCount;
			PeakActiveCallCount = FMath::Max(PeakActiveCallCount, ActiveCallCount);
			ON_SCOPE_EXIT { --ActiveCallCount; };
			Contexts.Add(Context);
			FHyperAIStudioDomainAdapterResult Result;
			Result.StatusCode = TEXT("test_action_complete");
			Result.Outcome = bOutcomeUnknownOnApply
				&& Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Apply
				? EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown
				: EHyperAIStudioDomainDispatchOutcome::Succeeded;
			if (Result.Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				Result.Payload = MakeShared<FTestResultPayload, ESPMode::ThreadSafe>();
			}
			if (ClockToAdvance && Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Apply)
			{
				ClockToAdvance->Advance(AdvanceAfterApplyMs);
			}
			if (ExecuteCallback)
			{
				ExecuteCallback();
			}
			return Result;
		}
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		TArray<FHyperAIStudioDomainDispatchContext> Contexts;
		FTestClock* ClockToAdvance = nullptr;
		int64 AdvanceAfterApplyMs = 0;
		bool bOutcomeUnknownOnApply = false;
		TFunction<void()> ExecuteCallback;
		int32 ActiveCallCount = 0;
		int32 PeakActiveCallCount = 0;
	};

	class FTestStateGate final : public IHyperAIStudioTypedArtifactStateGate
	{
	public:
		virtual bool Revalidate(
			const FHyperAIStudioPreparedTypedArtifact& Prepared,
			const IHyperAIStudioTypedArtifactPayload& Payload,
			const EHyperAIStudioDomainExecutionActionKind ActionKind,
			FHyperAIStudioDomainPrerequisiteSnapshot& OutPrerequisites,
			FHyperAIStudioDomainAdmissionSnapshot& OutAdmission,
			FString& OutError) override
		{
			++RevalidateCount;
			if (RevalidateCallback && !bInsideRevalidateCallback)
			{
				TGuardValue<bool> Guard(bInsideRevalidateCallback, true);
				RevalidateCallback();
			}
			if (!bAllow)
			{
				OutError = TEXT("test_state_drift");
				return false;
			}
			OutPrerequisites = Prepared.Contract.Binding.Prerequisites;
			OutAdmission = Prepared.Contract.Binding.Admission;
			return true;
		}
		virtual bool VerifyFresh(
			const FHyperAIStudioPreparedTypedArtifact& Prepared,
			const IHyperAIStudioTypedArtifactPayload& Payload,
			const IHyperAIStudioDomainResultPayload& AdapterResult,
			FString& OutPostconditionHash,
			FString& OutError) override
		{
			++FreshVerifyCount;
			if (!bFreshValid)
			{
				OutError = TEXT("test_fresh_verify_failed");
				return false;
			}
			OutPostconditionHash = PostconditionHash;
			return true;
		}
		int32 RevalidateCount = 0;
		int32 FreshVerifyCount = 0;
		bool bAllow = true;
		bool bFreshValid = true;
		TFunction<void()> RevalidateCallback;
		bool bInsideRevalidateCallback = false;
	};

	FHyperAIStudioDomainAdapterDescriptor MakeDescriptor(
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit)
	{
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		Descriptor.PackId = TEXT("enhanced_input");
		Descriptor.AdapterId = TEXT("adapter.enhanced_input.test");
		Descriptor.SemanticVersion = TEXT("1.0.0");
		Descriptor.AdapterVersion = 1;
		Descriptor.Variants.Add({
			TEXT("hyper_input_apply_plan"), TEXT("typed_artifact_v1"),
			RequestType, RequestSchema, ResultType, ResultSchema,
			Safety});
		Descriptor.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		Descriptor.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		return Descriptor;
	}

	FHyperAIStudioDomainBinding MakeBinding(
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		const FHyperAIStudioDomainRegistrationHandle& Handle,
		const FString& CanonicalProjectId)
	{
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = Descriptor.PackId;
		Binding.ToolName = Descriptor.Variants[0].ToolName;
		Binding.VariantId = Descriptor.Variants[0].VariantId;
		Binding.ExpectedSafety = Descriptor.Variants[0].Safety;
		Binding.CanonicalProjectId = CanonicalProjectId;
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = Handle.AdapterGeneration;
		Binding.ExpectedRegistryEpoch = Handle.RegistryEpoch;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 7;
		Binding.Prerequisites.Observations.Add({
			TEXT("module.enhanced_input"), EHyperAIStudioDomainPrerequisiteState::Available});
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.bPackAdmitted = true;
		Binding.Admission.bEditAdmitted = true;
		Binding.Admission.bDestructiveAdmitted = true;
		Binding.Admission.bExternalEffectAdmitted = true;
		Binding.Admission.Revision = 11;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
		return Binding;
	}

	FHyperAIStudioTypedArtifactContract MakeContract(const FHyperAIStudioDomainBinding& Binding)
	{
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = RequestType;
		Contract.ArtifactSchemaFingerprint = RequestSchema;
		Contract.ArtifactSemanticFingerprint = SemanticFingerprint;
		Contract.EffectTarget = TEXT("input:/Game/Input/IMC_Test");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 4096;
		Contract.MaxResultBytes = 128;
		Contract.StageLifetimeMs = 10000;
		Contract.bCompileOnce = false;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return Contract;
	}

	bool SameStageReceipt(
		const FHyperAIStudioTypedArtifactStageReceipt& A,
		const FHyperAIStudioTypedArtifactStageReceipt& B)
	{
		return A.StageId == B.StageId && A.CanonicalProjectId == B.CanonicalProjectId
			&& A.OperationId == B.OperationId && A.PackId == B.PackId
			&& A.ToolName == B.ToolName && A.VariantId == B.VariantId
			&& A.ArtifactTypeId == B.ArtifactTypeId
			&& A.ArtifactSchemaFingerprint == B.ArtifactSchemaFingerprint
			&& A.ArtifactSemanticFingerprint == B.ArtifactSemanticFingerprint
			&& A.AdapterFingerprint == B.AdapterFingerprint
			&& A.AdmissionFingerprint == B.AdmissionFingerprint
			&& A.PrerequisiteFingerprint == B.PrerequisiteFingerprint
			&& A.ContractFingerprint == B.ContractFingerprint && A.PlanHash == B.PlanHash
			&& A.AuthorizationPlanHash == B.AuthorizationPlanHash
			&& A.CapabilityHash == B.CapabilityHash && A.EffectFingerprint == B.EffectFingerprint
			&& A.AdapterGeneration == B.AdapterGeneration && A.RegistryEpoch == B.RegistryEpoch
			&& A.ExpiresUtcMs == B.ExpiresUtcMs;
	}

	FString MakeProjectRoot()
	{
		const FString Root = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("HyperAIStudioTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactClockReentrancyTest,
	"HyperAIStudio.NativeTools.TypedArtifact.ClockReentrancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactClockReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FReentrantTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FReentrantTestClock, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	bool bReentered = false;
	Clock->Callback = [&]()
	{
		bReentered = true;
		TestEqual(TEXT("reentrant observation sees the empty bounded store"), Store.NumStaged(), 0);
	};
	TestEqual(TEXT("outer observation remains stable"), Store.NumStaged(), 0);
	TestTrue(TEXT("clock callback can re-enter because it runs outside the store mutex"), bReentered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactClockLifetimeTest,
	"HyperAIStudio.NativeTools.TypedArtifact.ClockLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactClockLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));

	FHyperAIStudioTypedArtifactClaim Claim;
	TWeakPtr<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> ClockWeak;
	{
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FTestClock, ESPMode::ThreadSafe>();
		ClockWeak = Clock;
		FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		TestTrue(TEXT("artifact stages"), Store.StageExact(
			Prepared, TEXT("typed-artifact-op-clock-lifetime"), Payload, Receipt, Error));
		TestTrue(TEXT("artifact claims"), Store.ClaimExact(Receipt, Claim, Error));
	}
	TestTrue(TEXT("escaped claim keeps its monotonic clock alive after store destruction"), ClockWeak.IsValid());
	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	TestTrue(TEXT("escaped claim executes without a dangling clock"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, FString(), StateGate, Result, Error));
	TestEqual(TEXT("escaped claim completes"), Result.State,
		EHyperAIStudioTypedArtifactExecutionState::Completed);
	TestFalse(TEXT("clock is released after terminal claim consumption"), ClockWeak.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactLifecycleTest,
	"HyperAIStudio.NativeTools.TypedArtifact.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));

	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("closed contract prepares without staging"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(
			MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	TestTrue(TEXT("plan hash is sealed"), Prepared.PlanHash.StartsWith(TEXT("sha256:")));
	TestTrue(TEXT("effect hash is sealed"), Prepared.EffectFingerprint.StartsWith(TEXT("sha256:")));

	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	// Two racing stage requests for one project/operation can produce only one committed claim.
	TArray<FHyperAIStudioTypedArtifactStageReceipt> RacingReceipts;
	RacingReceipts.SetNum(2);
	TArray<FString> RacingErrors;
	RacingErrors.SetNum(2);
	FThreadSafeCounter RacingSuccesses;
	const TSharedRef<FTestArtifactPayload, ESPMode::ThreadSafe> MutablePayload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload = MutablePayload;
	ParallelFor(2, [&](const int32 Index)
	{
		if (Store.StageExact(
			Prepared, TEXT("typed-artifact-op-race"), Payload,
			RacingReceipts[Index], RacingErrors[Index]))
		{
			RacingSuccesses.Increment();
		}
	});
	TestTrue(TEXT("racing stages expose at least one recoverable receipt"),
		RacingSuccesses.GetValue() >= 1 && RacingSuccesses.GetValue() <= 2);
	for (int32 Index = 0; Index < RacingReceipts.Num(); ++Index)
	{
		if (RacingReceipts[Index].StageId.IsEmpty())
		{
			TestEqual(TEXT("non-winning concurrent caller receives bounded retry status"),
				RacingErrors[Index], FString(TEXT("stage_in_progress_retry")));
		}
	}
	TestEqual(TEXT("identical racing stages do not duplicate storage"), Store.NumStaged(), 1);
	FHyperAIStudioTypedArtifactStageReceipt RacingRetryReceipt;
	Error.Reset();
	TestTrue(TEXT("concurrent response retry recovers the committed receipt"),
		Store.StageExact(
			Prepared, TEXT("typed-artifact-op-race"), Payload, RacingRetryReceipt, Error));
	for (const FHyperAIStudioTypedArtifactStageReceipt& RacingReceipt : RacingReceipts)
	{
		if (!RacingReceipt.StageId.IsEmpty())
		{
			TestEqual(TEXT("every successful race/retry observes one server stage id"),
				RacingReceipt.StageId, RacingRetryReceipt.StageId);
		}
	}
	FHyperAIStudioTypedArtifactClaim RacingClaim;
	TestTrue(TEXT("recovered racing receipt claims"),
		Store.ClaimExact(RacingRetryReceipt, RacingClaim, Error));
	RacingClaim.Reset();

	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> WrongPayload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>(
			TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	TestFalse(TEXT("semantic payload drift cannot stage"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-0001"), WrongPayload, Receipt, Error));

	Error.Reset();
	TestTrue(TEXT("exact typed payload stages"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-0001"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactStageReceipt LostResponseRetryReceipt;
	TestTrue(TEXT("lost stage response retries idempotently"),
		Store.StageExact(
			Prepared, TEXT("typed-artifact-op-0001"), Payload, LostResponseRetryReceipt, Error));
	TestEqual(TEXT("lost-response retry recovers the exact original receipt"),
		LostResponseRetryReceipt.StageId, Receipt.StageId);
	TestEqual(TEXT("lost-response retry does not extend server expiry"),
		LostResponseRetryReceipt.ExpiresUtcMs, Receipt.ExpiresUtcMs);
	TestEqual(TEXT("lost-response retry keeps one staged entry"), Store.NumStaged(), 1);
	MutablePayload->SetSemantic(
		TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	TestEqual(TEXT("stage store owns one artifact"), Store.NumStaged(), 1);
	TestTrue(TEXT("adapter generation is exact"), Receipt.AdapterGeneration == Handle.AdapterGeneration);
	Error.Reset();
	TestEqual(TEXT("stage pin vetoes optional module unload"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Busy);

	FHyperAIStudioTypedArtifactStageReceipt WrongReceipt = Receipt;
	WrongReceipt.PlanHash = TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	FHyperAIStudioTypedArtifactClaim Claim;
	Error.Reset();
	TestFalse(TEXT("partial claim binding is rejected without consumption"),
		Store.ClaimExact(WrongReceipt, Claim, Error));
	TestEqual(TEXT("mismatch leaves stage available"), Store.NumStaged(), 1);
	Error.Reset();
	TestTrue(TEXT("exact claim succeeds once"), Store.ClaimExact(Receipt, Claim, Error));
	TestEqual(TEXT("claim consumes stage"), Store.NumStaged(), 0);
	FHyperAIStudioTypedArtifactClaim DuplicateClaim;
	TestFalse(TEXT("stage cannot be claimed twice"), Store.ClaimExact(Receipt, DuplicateClaim, Error));

	FHyperAIStudioDomainAdapterDescriptor DisjointDescriptor = MakeDescriptor();
	DisjointDescriptor.AdapterId = TEXT("adapter.enhanced_input.disjoint");
	DisjointDescriptor.Variants[0].ToolName = TEXT("hyper_input_validate_disjoint");
	DisjointDescriptor.ContractFingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(DisjointDescriptor);
	DisjointDescriptor.AdapterFingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(DisjointDescriptor);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> DisjointAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(DisjointDescriptor);
	FHyperAIStudioDomainRegistrationHandle DisjointHandle;
	TestEqual(TEXT("same A adapter still cannot unregister while its exact claim is pinned"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Busy);
	bool bDisjointChurnRan = false;
	bool bDisjointRegistered = false;
	EHyperAIStudioDomainUnregisterResult DisjointUnregister =
		EHyperAIStudioDomainUnregisterResult::NotFound;
	FString DisjointError;
	Adapter->ExecuteCallback = [&]()
	{
		if (!bDisjointChurnRan)
		{
			bDisjointChurnRan = true;
			bDisjointRegistered = Registry.RegisterAdapter(
				DisjointAdapter, DisjointHandle, DisjointError);
			if (bDisjointRegistered)
			{
				DisjointUnregister = Registry.UnregisterAdapter(DisjointHandle, DisjointError);
			}
		}
	};

	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	Error.Reset();
	TestTrue(TEXT("claim executes through journal and fresh verification"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, FString(), StateGate, Result, Error));
	Adapter->ExecuteCallback = nullptr;
	TestEqual(TEXT("execution completed"), Result.State, EHyperAIStudioTypedArtifactExecutionState::Completed);
	TestTrue(TEXT("disjoint same-pack adapter registers after A apply"),
		bDisjointChurnRan && bDisjointRegistered);
	TestEqual(TEXT("disjoint same-pack adapter unregisters before A compile/save/verify"),
		DisjointUnregister, EHyperAIStudioDomainUnregisterResult::Removed);
	TestTrue(TEXT("disjoint registry epoch churn did not invalidate A phases"),
		Adapter->Contexts.Num() > 1);
	TestEqual(TEXT("apply/validate/save/fresh are serial"), Adapter->Contexts.Num(), 4);
	for (int32 Index = 0; Index < Adapter->Contexts.Num(); ++Index)
	{
		TestTrue(TEXT("each action nonce is unique"),
			Adapter->Contexts.FindLastByPredicate([&](const FHyperAIStudioDomainDispatchContext& Other)
			{
				return Other.ActionNonce == Adapter->Contexts[Index].ActionNonce;
			}) == Index);
		TestTrue(TEXT("journal-issued edit nonce reaches every phase"),
			Adapter->Contexts[Index].VerifiedAuthorizationNonce.StartsWith(TEXT("journal-edit-")));
		TestEqual(TEXT("one journal nonce binds the whole serial execution"),
			Adapter->Contexts[Index].VerifiedAuthorizationNonce,
			Adapter->Contexts[0].VerifiedAuthorizationNonce);
	}
	TestEqual(TEXT("ordinary edit consumes no risky bearer grant"), Authorization->CallCount, 0);
	TestEqual(TEXT("fresh state gate called once"), StateGate.FreshVerifyCount, 1);
	TestFalse(TEXT("completed route never permits fallback"), Result.bFallbackPermitted);
	MutablePayload->SetSemantic(SemanticFingerprint);

	// A fresh stage/claim for the same exact operation replays the durable completed record.
	FHyperAIStudioTypedArtifactStageReceipt ReplayReceipt;
	Error.Reset();
	TestTrue(TEXT("completed identity may be restaged for replay"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-0001"), Payload, ReplayReceipt, Error));
	FHyperAIStudioTypedArtifactClaim ReplayClaim;
	TestTrue(TEXT("replay artifact claims"), Store.ClaimExact(ReplayReceipt, ReplayClaim, Error));
	FHyperAIStudioTypedArtifactExecutionResult ReplayResult;
	TestTrue(TEXT("completed operation replays without redispatch or reauthorization"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(ReplayClaim), ProjectRoot, FString(), StateGate, ReplayResult, Error));
	TestEqual(TEXT("replay state is explicit"), ReplayResult.State,
		EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted);
	TestEqual(TEXT("replay did not call adapter"), Adapter->Contexts.Num(), 4);
	TestEqual(TEXT("replay did not create a risky grant"), Authorization->CallCount, 0);

	// UTC rollback cannot extend the internally monotonic stage capability.
	FHyperAIStudioTypedArtifactStageReceipt ExpiringReceipt;
	TestTrue(TEXT("expiry fixture stages"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-expiry"), Payload, ExpiringReceipt, Error));
	Clock->UtcMs -= 24LL * 60 * 60 * 1000;
	Clock->MonotonicMs += Prepared.Contract.StageLifetimeMs + 1;
	TestEqual(TEXT("monotonic expiry survives UTC rollback"), Store.NumStaged(), 0);
	FHyperAIStudioTypedArtifactClaim ExpiredClaim;
	TestFalse(TEXT("expired capability cannot claim after UTC rollback"),
		Store.ClaimExact(ExpiringReceipt, ExpiredClaim, Error));

	Error.Reset();
	TestEqual(TEXT("module can unload after terminal claim release"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactAmbiguityTest,
	"HyperAIStudio.NativeTools.TypedArtifact.Ambiguity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactAmbiguityTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	Adapter->bOutcomeUnknownOnApply = true;
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("artifact stages"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-unknown"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactClaim Claim;
	TestTrue(TEXT("artifact claims"), Store.ClaimExact(Receipt, Claim, Error));
	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	TestFalse(TEXT("unknown adapter result never claims success"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, FString(), StateGate, Result, Error));
	TestEqual(TEXT("unknown result is terminal and explicit"), Result.State,
		EHyperAIStudioTypedArtifactExecutionState::OutcomeUnknown);
	TestTrue(TEXT("unknown flag is set"), Result.bOutcomeUnknown);
	TestFalse(TEXT("unknown result cannot fallback"), Result.bFallbackPermitted);
	Error.Reset();
	TestEqual(TEXT("outcome_unknown permanently vetoes adapter unload"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::OutcomeUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactAuthorizationOrderingTest,
	"HyperAIStudio.NativeTools.TypedArtifact.AuthorizationOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactAuthorizationOrderingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	const FString OperationId = TEXT("typed-artifact-op-auth-fail");
	TestTrue(TEXT("artifact stages"), Store.StageExact(Prepared, OperationId, Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactClaim Claim;
	TestTrue(TEXT("artifact claims"), Store.ClaimExact(Receipt, Claim, Error));
	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	TestFalse(TEXT("edit rejects every client bearer before journal admission"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, TEXT("not-a-server-token-0001"),
			StateGate, Result, Error));
	TestEqual(TEXT("adapter was never dispatched"), Adapter->Contexts.Num(), 0);
	TestEqual(TEXT("risky server gate was never reached for edit"), Authorization->CallCount, 0);

	// Client-token rejection happens before Begin; the operation identity remains unused.
	FHyperAIStudioOperationJournal Journal(ProjectRoot);
	FString JournalError;
	TestTrue(TEXT("journal reopens"), Journal.Load(JournalError));
	const TOptional<FHyperAIStudioOperationRecord> Record = Journal.Find(OperationId);
	TestFalse(TEXT("rejected edit token did not consume journal identity"), Record.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactTimeoutTest,
	"HyperAIStudio.NativeTools.TypedArtifact.Timeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	Adapter->ClockToAdvance = &Clock.Get();
	// Cross the 100 ms operation deadline without exceeding the derived 180 ms apply budget.
	Adapter->AdvanceAfterApplyMs = 101;
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioTypedArtifactContract Contract =
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId));
	Contract.DeadlineMs = 100;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("bounded timeout contract prepares"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, Error));
	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("artifact stages"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-timeout"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactClaim Claim;
	TestTrue(TEXT("artifact claims"), Store.ClaimExact(Receipt, Claim, Error));
	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	TestFalse(TEXT("deadline after known apply cannot claim completion"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, FString(), StateGate, Result, Error));
	TestEqual(TEXT("known applied effect times out as partial"),
		Result.State, EHyperAIStudioTypedArtifactExecutionState::Partial);
	TestEqual(TEXT("no finalizer dispatched after deadline"), Adapter->Contexts.Num(), 1);
	TestFalse(TEXT("partial timeout never permits fallback"), Result.bFallbackPermitted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactRiskyGrantTest,
	"HyperAIStudio.NativeTools.TypedArtifact.RiskyGrant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactRiskyGrantTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("destructive adapter registers"), Registry.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("destructive contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	FHyperAIStudioTypedArtifactStore Store(Registry, Clock);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("destructive artifact stages without storing bearer"),
		Store.StageExact(Prepared, TEXT("typed-artifact-op-risky"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactClaim Claim;
	TestTrue(TEXT("destructive artifact claims"), Store.ClaimExact(Receipt, Claim, Error));
	FTestStateGate StateGate;
	FHyperAIStudioTypedArtifactExecutionResult Result;
	TestTrue(TEXT("exact server-issued grant executes destructive plan"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(Claim), ProjectRoot, AuthorizationToken, StateGate, Result, Error));
	TestEqual(TEXT("risky grant consumed exactly once"), Authorization->CallCount, 1);
	TestTrue(TEXT("destructive plan dispatched at least one phase"), !Adapter->Contexts.IsEmpty());
	if (!Adapter->Contexts.IsEmpty())
	{
		TestEqual(TEXT("server nonce reaches destructive phases"),
			Adapter->Contexts[0].VerifiedAuthorizationNonce, FString(TEXT("server-artifact-nonce-0001")));
	}

	FHyperAIStudioTypedArtifactStageReceipt BogusReplayReceipt;
	TestTrue(TEXT("completed risky identity can be restaged for exact replay inspection"), Store.StageExact(
		Prepared, TEXT("typed-artifact-op-risky"), Payload, BogusReplayReceipt, Error));
	FHyperAIStudioTypedArtifactClaim BogusReplayClaim;
	TestTrue(TEXT("bogus-token replay candidate claims"),
		Store.ClaimExact(BogusReplayReceipt, BogusReplayClaim, Error));
	FHyperAIStudioTypedArtifactExecutionResult BogusReplayResult;
	TestFalse(TEXT("completed risky identity rejects a different opaque token before journal replay"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(BogusReplayClaim), ProjectRoot, TEXT("bogus-token-risky-replay-0001"),
			StateGate, BogusReplayResult, Error));
	TestEqual(TEXT("bogus replay did not consume the exact grant again"), Authorization->CallCount, 1);
	TestEqual(TEXT("bogus replay did not redispatch"), Adapter->Contexts.Num(), 4);

	// Journal replay wins before the domain grant is consumed, even though the original grant is
	// now single-use/consumed. The non-consuming exact server inspection recognizes only that grant.
	FHyperAIStudioTypedArtifactStageReceipt RiskyReplayReceipt;
	TestTrue(TEXT("completed risky identity may be restaged"), Store.StageExact(
		Prepared, TEXT("typed-artifact-op-risky"), Payload, RiskyReplayReceipt, Error));
	FHyperAIStudioTypedArtifactClaim RiskyReplayClaim;
	TestTrue(TEXT("completed risky identity claims"),
		Store.ClaimExact(RiskyReplayReceipt, RiskyReplayClaim, Error));
	FHyperAIStudioTypedArtifactExecutionResult RiskyReplayResult;
	TestTrue(TEXT("completed risky identity replays without consuming the token again"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(RiskyReplayClaim), ProjectRoot, AuthorizationToken,
			StateGate, RiskyReplayResult, Error));
	TestEqual(TEXT("risky replay state is explicit"), RiskyReplayResult.State,
		EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted);
	TestEqual(TEXT("risky replay did not consume the server grant twice"), Authorization->CallCount, 1);
	TestEqual(TEXT("risky replay did not redispatch"), Adapter->Contexts.Num(), 4);

	// A new risky operation cannot be auto-authorized by the executor.
	FHyperAIStudioTypedArtifactStageReceipt MissingGrantReceipt;
	TestTrue(TEXT("second risky artifact stages"), Store.StageExact(
		Prepared, TEXT("typed-artifact-op-no-grant"), Payload, MissingGrantReceipt, Error));
	FHyperAIStudioTypedArtifactClaim MissingGrantClaim;
	TestTrue(TEXT("second risky artifact claims"),
		Store.ClaimExact(MissingGrantReceipt, MissingGrantClaim, Error));
	FHyperAIStudioTypedArtifactExecutionResult MissingGrantResult;
	TestFalse(TEXT("risky operation never receives an implicit grant"),
		FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
			MoveTemp(MissingGrantClaim), ProjectRoot, FString(), StateGate,
			MissingGrantResult, Error));
	TestEqual(TEXT("missing grant did not dispatch"), Adapter->Contexts.Num(), 4);
	TestEqual(TEXT("missing grant did not consume another server token"), Authorization->CallCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactAsyncHostLifecycleTest,
	"HyperAIStudio.NativeTools.TypedArtifact.AsyncHost.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactAsyncHostLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("async host adapter registers"), Registry->RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("async host contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> StateGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService Service(Registry, ProjectRoot, Clock);
	TestTrue(TEXT("async host starts"), Service.Startup(Error));

	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("exact artifact stages"), Service.StageExact(
		Prepared, TEXT("async-host-lifecycle-0001"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactStageReceipt LostStageResponseRetry;
	TestTrue(TEXT("lost stage response recovers exact receipt"), Service.StageExact(
		Prepared, TEXT("async-host-lifecycle-0001"), Payload, LostStageResponseRetry, Error));
	TestTrue(TEXT("stage retry returns the exact immutable receipt"),
		SameStageReceipt(Receipt, LostStageResponseRetry));
	FHyperAIStudioTypedArtifactStageReceipt TamperedReceipt = Receipt;
	const FString AlternatePlanHash =
		TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	TamperedReceipt.PlanHash = AlternatePlanHash;
	if (Receipt.PlanHash == AlternatePlanHash)
	{
		TamperedReceipt.PlanHash =
			TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
	}
	FHyperAIStudioTypedArtifactSubmissionReceipt TamperedSubmission;
	TestFalse(TEXT("bounded tampered receipt cannot claim the real live stage"), Service.SubmitExact(
		TamperedReceipt, FString(), StateGate, TamperedSubmission, Error));
	TestEqual(TEXT("bounded tampered receipt failure is explicit"),
		TamperedSubmission.Operation.Status, FString(TEXT("stage_claim_rejected")));
	FHyperAIStudioTypedArtifactSubmissionReceipt TamperedRetry;
	TestFalse(TEXT("bounded tampered receipt has exact response-loss recovery"), Service.SubmitExact(
		TamperedReceipt, FString(), StateGate, TamperedRetry, Error));
	TestEqual(TEXT("bounded tampered retry preserves its own failure"),
		TamperedRetry.Operation.Status, TamperedSubmission.Operation.Status);
	TestEqual(TEXT("tampered receipt never consumes or poisons the real stage"),
		Service.NumStaged(), 1);

	FHyperAIStudioTypedArtifactStageReceipt OversizedIdReceipt = Receipt;
	OversizedIdReceipt.StageId = FString::ChrN(65, TEXT('s'));
	FHyperAIStudioTypedArtifactSubmissionReceipt OversizedIdSubmission;
	TestFalse(TEXT("oversized receipt id is rejected before host admission"), Service.SubmitExact(
		OversizedIdReceipt, FString(), StateGate, OversizedIdSubmission, Error));
	TestTrue(TEXT("invalid receipt is never copied into the submission response"),
		OversizedIdSubmission.Stage.StageId.IsEmpty()
		&& OversizedIdSubmission.Operation.OperationId.IsEmpty());

	FHyperAIStudioTypedArtifactStageReceipt OversizedStringReceipt = Receipt;
	OversizedStringReceipt.ToolName = FString::ChrN(
		FHyperAIStudioDomainLimits::MaxToolNameChars + 1, TEXT('t'));
	FHyperAIStudioTypedArtifactSubmissionReceipt OversizedStringSubmission;
	TestFalse(TEXT("oversized receipt binding is rejected before host admission"), Service.SubmitExact(
		OversizedStringReceipt, FString(), StateGate, OversizedStringSubmission, Error));
	TestTrue(TEXT("oversized receipt binding leaves output empty"),
		OversizedStringSubmission.Stage.StageId.IsEmpty());

	FHyperAIStudioTypedArtifactStageReceipt OversizedHashReceipt = Receipt;
	OversizedHashReceipt.PlanHash = TEXT("sha256:") + FString::ChrN(65, TEXT('a'));
	FHyperAIStudioTypedArtifactSubmissionReceipt OversizedHashSubmission;
	TestFalse(TEXT("oversized receipt hash is rejected before host admission"), Service.SubmitExact(
		OversizedHashReceipt, FString(), StateGate, OversizedHashSubmission, Error));
	TestTrue(TEXT("oversized receipt hash leaves output empty"),
		OversizedHashSubmission.Stage.StageId.IsEmpty());

	const FString OversizedAuthorization = FString::ChrN(
		FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars + 1, TEXT('g'));
	FHyperAIStudioTypedArtifactSubmissionReceipt OversizedAuthorizationSubmission;
	TestFalse(TEXT("oversized authorization is rejected before receipt copy or claim"),
		Service.SubmitExact(
			Receipt, OversizedAuthorization, StateGate, OversizedAuthorizationSubmission, Error));
	TestTrue(TEXT("oversized authorization leaves output empty"),
		OversizedAuthorizationSubmission.Stage.StageId.IsEmpty());
	TestEqual(TEXT("invalid receipts and bearer did not reserve or consume the staged artifact"),
		Service.NumStaged(), 1);
	FHyperAIStudioTypedArtifactSubmissionReceipt OversizedStageAndSubmit;
	TestFalse(TEXT("stage-and-submit rejects oversized authorization before staging"),
		Service.StageAndSubmitExact(
			Prepared, TEXT("async-host-oversized-token-0001"), Payload,
			OversizedAuthorization, StateGate, OversizedStageAndSubmit, Error));
	TestEqual(TEXT("oversized stage-and-submit bearer creates no extra stage"),
		Service.NumStaged(), 1);

	bool bReentrantSubmitObserved = false;
	FHyperAIStudioTypedArtifactSubmissionReceipt ReentrantSubmission;
	StateGate->RevalidateCallback = [&]()
	{
		FString ReentrantError;
		const bool bReentrantAccepted = Service.SubmitExact(
			Receipt, FString(), StateGate, ReentrantSubmission, ReentrantError);
		bReentrantSubmitObserved = true;
		TestFalse(TEXT("reentrant submit cannot pass the reserved lane"), bReentrantAccepted);
		TestEqual(TEXT("reentrant exact receipt sees admission reservation"),
			ReentrantSubmission.Operation.Status, FString(TEXT("admission_in_progress")));
	};
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	TestTrue(TEXT("submit durably admits without pumping"), Service.SubmitExact(
		Receipt, FString(), StateGate, Submission, Error));
	StateGate->RevalidateCallback = nullptr;
	TestTrue(TEXT("injected state-gate reentry was exercised"), bReentrantSubmitObserved);
	TestTrue(TEXT("immediate response is accepted and durable"),
		Submission.bOk && Submission.Operation.bAccepted && Submission.Operation.bDurable);
	TestEqual(TEXT("no adapter action runs before submit returns"), Adapter->Contexts.Num(), 0);
	FHyperAIStudioTypedArtifactSubmissionReceipt LostSubmitResponseRetry;
	TestTrue(TEXT("lost submit response returns the same active operation"), Service.SubmitExact(
		Receipt, FString(), StateGate, LostSubmitResponseRetry, Error));
	TestEqual(TEXT("duplicate submit still did not dispatch"), Adapter->Contexts.Num(), 0);
	TestTrue(TEXT("duplicate submit preserves the exact stage receipt"),
		SameStageReceipt(LostSubmitResponseRetry.Stage, Receipt));
	TestEqual(TEXT("duplicate status preserves operation identity"),
		LostSubmitResponseRetry.Operation.OperationId, Receipt.OperationId);

	FHyperAIStudioTypedArtifactStageReceipt ActiveStageRetry;
	TestTrue(TEXT("stage-and-submit caller can recover the original active receipt"), Service.StageExact(
		Prepared, TEXT("async-host-lifecycle-0001"), Payload, ActiveStageRetry, Error));
	TestEqual(TEXT("active stage recovery is exact"), ActiveStageRetry.StageId, Receipt.StageId);
	FString UnregisterError;
	TestEqual(TEXT("active execution lease vetoes adapter/module unload"),
		Registry->UnregisterAdapter(Handle, UnregisterError), EHyperAIStudioDomainUnregisterResult::Busy);

	FHyperAIStudioTypedArtifactStageReceipt SecondReceipt;
	TestTrue(TEXT("bounded pre-admission store accepts another artifact"), Service.StageExact(
		Prepared, TEXT("async-host-lifecycle-0002"), Payload, SecondReceipt, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt BusySubmission;
	TestFalse(TEXT("second mutation cannot overlap the canonical project lane"), Service.SubmitExact(
		SecondReceipt, FString(), StateGate, BusySubmission, Error));
	TestEqual(TEXT("busy status is explicit"), BusySubmission.Operation.Status,
		FString(TEXT("project_execution_busy")));
	TestEqual(TEXT("busy submission leaves the staged artifact recoverable"), Service.NumStaged(), 1);

	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestEqual(TEXT("first ticker callback is a strict no-pump admission barrier"),
		Adapter->Contexts.Num(), 0);
	for (int32 Guard = 0; Guard < 8; ++Guard)
	{
		FTSTicker::GetCoreTicker().Tick(0.0f);
		FHyperAIStudioTypedArtifactOperationStatus Polled;
		if (Service.QueryStatus(Receipt.OperationId, Polled, Error) && Polled.bTerminal)
		{
			break;
		}
	}
	FHyperAIStudioTypedArtifactOperationStatus Completed;
	TestTrue(TEXT("terminal operation remains queryable"),
		Service.QueryStatus(Receipt.OperationId, Completed, Error));
	TestTrue(TEXT("serial lifecycle completed with fresh proof"),
		Completed.bTerminal && Completed.Status == TEXT("completed"));
	TestEqual(TEXT("adapter actions never overlap"), Adapter->PeakActiveCallCount, 1);
	TestEqual(TEXT("apply plus validate/save/fresh execute once each"), Adapter->Contexts.Num(), 4);
	TestEqual(TEXT("fresh validator ran exactly once"), StateGate->FreshVerifyCount, 1);

	FHyperAIStudioOperationJournal Journal(ProjectRoot);
	TestTrue(TEXT("completed host journal reopens"), Journal.Load(Error));
	const TOptional<FHyperAIStudioOperationRecord> Durable = Journal.Find(Receipt.OperationId);
	TestTrue(TEXT("completed operation is durable"), Durable.IsSet());
	if (Durable.IsSet())
	{
		TestEqual(TEXT("durable terminal state is completed"), Durable->State,
			EHyperAIStudioOperationState::Completed);
		TestEqual(TEXT("fresh receipt uses the current evidence schema"),
			Durable->TerminalEvidence.Version, FHyperAIStudioOperationEvidence::CurrentVersion);
		TestEqual(TEXT("fresh receipt binds exact canonical project"),
			Durable->TerminalEvidence.CanonicalProjectId, Receipt.CanonicalProjectId);
		TestEqual(TEXT("fresh receipt binds exact operation"),
			Durable->TerminalEvidence.OperationId, Receipt.OperationId);
		TestEqual(TEXT("fresh receipt binds exact plan"),
			Durable->TerminalEvidence.PlanHash, Receipt.PlanHash);
		TestEqual(TEXT("fresh receipt binds exact capability inventory"),
			Durable->TerminalEvidence.CapabilityHash, Receipt.CapabilityHash);
		TestEqual(TEXT("fresh receipt binds exact effect"),
			Durable->TerminalEvidence.EffectFingerprint, Receipt.EffectFingerprint);
		TestEqual(TEXT("fresh receipt binds approved validator identity"),
			Durable->TerminalEvidence.ValidatorId,
			FHyperAIStudioTypedPlanValidator::FreshValidatorId());
		TestEqual(TEXT("fresh receipt binds approved validator implementation"),
			Durable->TerminalEvidence.ApprovedValidatorFingerprint,
			FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint());
		TestTrue(TEXT("fresh receipt binds an action nonce and postcondition"),
			!Durable->TerminalEvidence.ActionNonce.IsEmpty()
			&& !Durable->TerminalEvidence.PostconditionHash.IsEmpty());
		TestTrue(TEXT("fresh receipt fingerprint is present"),
			!Durable->TerminalEvidence.ReceiptFingerprint.IsEmpty());
	}
	Service.Shutdown();
	TestTrue(TEXT("shutdown is quiescent after synchronous adapter completion"),
		Service.CanShutdownSafely());
	TestEqual(TEXT("shutdown releases the leftover staged lease"),
		Registry->UnregisterAdapter(Handle, UnregisterError), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactAsyncHostAuthorizationReplayTest,
	"HyperAIStudio.NativeTools.TypedArtifact.AsyncHost.AuthorizationReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactAsyncHostAuthorizationReplayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString ProjectRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true); };
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> Authorization =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>(nullptr, Authorization);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioDomainRegistrationHandle Handle;
	FString Error;
	TestTrue(TEXT("risky host adapter registers"), Registry->RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("risky host contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId)), Prepared, Error));
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> StateGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();

	FHyperAIStudioTypedArtifactExecutionService FirstService(Registry, ProjectRoot, Clock);
	TestTrue(TEXT("first risky host starts"), FirstService.Startup(Error));
	FHyperAIStudioTypedArtifactStageReceipt MissingGrantReceipt;
	TestTrue(TEXT("missing-grant artifact stages"), FirstService.StageExact(
		Prepared, TEXT("async-host-risky-no-grant"), Payload, MissingGrantReceipt, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt MissingGrant;
	TestFalse(TEXT("new risky mutation defaults deny without server grant"), FirstService.SubmitExact(
		MissingGrantReceipt, FString(), StateGate, MissingGrant, Error));
	TestEqual(TEXT("missing grant status is explicit"), MissingGrant.Operation.Status,
		FString(TEXT("authorization_required")));
	TestEqual(TEXT("missing grant consumed nothing"), Authorization->CallCount, 0);
	FHyperAIStudioTypedArtifactOperationStatus MissingGrantQuery;
	TestTrue(TEXT("claim-to-start rejection remains queryable"), FirstService.QueryStatus(
		MissingGrantReceipt.OperationId, MissingGrantQuery, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt MissingGrantRetry;
	TestFalse(TEXT("exact rejected admission retry returns the original bounded response"),
		FirstService.SubmitExact(
			MissingGrantReceipt, FString(), StateGate, MissingGrantRetry, Error));
	TestEqual(TEXT("exact rejected admission retry preserves status"),
		MissingGrantRetry.Operation.Status, MissingGrant.Operation.Status);
	TestEqual(TEXT("exact rejected admission retry consumes nothing"), Authorization->CallCount, 0);

	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("risky exact artifact stages"), FirstService.StageExact(
		Prepared, TEXT("async-host-risky-0001"), Payload, Receipt, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	TestTrue(TEXT("server grant durably admits risky mutation"), FirstService.SubmitExact(
		Receipt, AuthorizationToken, StateGate, Submission, Error));
	TestEqual(TEXT("grant is consumed exactly once during durable admission"), Authorization->CallCount, 1);
	TestEqual(TEXT("durable risky admission still has not mutated"), Adapter->Contexts.Num(), 0);
	FHyperAIStudioTypedArtifactSubmissionReceipt BogusActiveDuplicate;
	TestFalse(TEXT("active risky replay rejects replacement bearer material"), FirstService.SubmitExact(
		Receipt, TEXT("bogus-duplicate-token-0001"), StateGate, BogusActiveDuplicate, Error));
	TestEqual(TEXT("bogus active replay status is explicit"),
		BogusActiveDuplicate.Operation.Status, FString(TEXT("authorization_replay_mismatch")));
	FHyperAIStudioTypedArtifactSubmissionReceipt ExactActiveDuplicate;
	TestTrue(TEXT("active risky replay accepts the exact consumed grant without consuming again"),
		FirstService.SubmitExact(
			Receipt, AuthorizationToken, StateGate, ExactActiveDuplicate, Error));
	TestEqual(TEXT("active duplicate did not consume another grant"), Authorization->CallCount, 1);
	for (int32 Guard = 0; Guard < 8; ++Guard)
	{
		FTSTicker::GetCoreTicker().Tick(0.0f);
	}
	FHyperAIStudioTypedArtifactOperationStatus Completed;
	TestTrue(TEXT("risky operation completes"), FirstService.QueryStatus(
		Receipt.OperationId, Completed, Error));
	TestTrue(TEXT("risky terminal result is complete"),
		Completed.bTerminal && Completed.Status == TEXT("completed"));
	const int32 DispatchCount = Adapter->Contexts.Num();
	FHyperAIStudioTypedArtifactSubmissionReceipt BogusArchivedDuplicate;
	TestFalse(TEXT("archived risky replay rejects a bogus bearer"), FirstService.SubmitExact(
		Receipt, TEXT("bogus-archived-token-0001"), StateGate, BogusArchivedDuplicate, Error));
	TestEqual(TEXT("bogus archived replay status is explicit"),
		BogusArchivedDuplicate.Operation.Status,
		FString(TEXT("authorization_replay_mismatch")));
	TestEqual(TEXT("bogus archived replay did not consume grant"), Authorization->CallCount, 1);
	FHyperAIStudioTypedArtifactSubmissionReceipt ExactArchivedDuplicate;
	TestTrue(TEXT("archived risky replay accepts exact original grant without Consume"),
		FirstService.SubmitExact(
			Receipt, AuthorizationToken, StateGate, ExactArchivedDuplicate, Error));
	TestEqual(TEXT("exact archived replay did not consume grant"), Authorization->CallCount, 1);
	FirstService.Shutdown();

	// A new host instance has no process-local archive. Journal-v3 terminal evidence still cannot
	// bypass the server grant: risky replay requires the exact already-consumed credential.
	FHyperAIStudioTypedArtifactExecutionService RestartedService(Registry, ProjectRoot, Clock);
	TestTrue(TEXT("restarted replay host starts"), RestartedService.Startup(Error));
	FHyperAIStudioTypedArtifactContract WrongContract =
		MakeContract(MakeBinding(Adapter->Descriptor, Handle, CanonicalProjectId));
	WrongContract.EffectTarget = TEXT("input:/Game/Input/IMC_WrongPlan");
	FHyperAIStudioPreparedTypedArtifact WrongPrepared;
	TestTrue(TEXT("conflicting replay contract prepares"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(WrongContract, WrongPrepared, Error));
	TestNotEqual(TEXT("conflicting replay has a distinct sealed plan"),
		WrongPrepared.PlanHash, Prepared.PlanHash);
	FHyperAIStudioTypedArtifactStageReceipt WrongPlanReceipt;
	TestTrue(TEXT("conflicting artifact can stage before journal identity comparison"),
		RestartedService.StageExact(
			WrongPrepared, Receipt.OperationId, Payload, WrongPlanReceipt, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt WrongPlanReplay;
	TestFalse(TEXT("durable identity rejects a different exact plan"),
		RestartedService.SubmitExact(
			WrongPlanReceipt, AuthorizationToken, StateGate, WrongPlanReplay, Error));
	TestEqual(TEXT("wrong-plan restart replay status is explicit"),
		WrongPlanReplay.Operation.Status, FString(TEXT("operation_id_conflict")));
	TestEqual(TEXT("wrong-plan rejection consumes no grant"), Authorization->CallCount, 1);
	TestEqual(TEXT("wrong-plan rejection does not redispatch"), Adapter->Contexts.Num(), DispatchCount);
	FHyperAIStudioTypedArtifactSubmissionReceipt WrongPlanFailureRetry;
	TestFalse(TEXT("wrong-plan response loss is recovered by its exact stage receipt"),
		RestartedService.SubmitExact(
			WrongPlanReceipt, AuthorizationToken, StateGate, WrongPlanFailureRetry, Error));
	TestEqual(TEXT("wrong-plan response retry preserves the exact failure"),
		WrongPlanFailureRetry.Operation.Status, WrongPlanReplay.Operation.Status);
	FHyperAIStudioTypedArtifactOperationStatus DurableAfterWrongPlan;
	TestTrue(TEXT("wrong-plan receipt does not poison operation-level durable query"),
		RestartedService.QueryStatus(
			Receipt.OperationId, DurableAfterWrongPlan, Error));
	TestTrue(TEXT("durable completed identity survives a wrong-plan receipt"),
		DurableAfterWrongPlan.bAccepted && DurableAfterWrongPlan.bDurable
		&& DurableAfterWrongPlan.bTerminal && DurableAfterWrongPlan.bReplay);

	FHyperAIStudioTypedArtifactStageReceipt EmptyReplayReceipt;
	TestTrue(TEXT("completed identity stages for empty-token restart replay"),
		RestartedService.StageExact(
			Prepared, Receipt.OperationId, Payload, EmptyReplayReceipt, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt EmptyRestartReplay;
	TestFalse(TEXT("durable risky replay rejects an empty grant after restart"),
		RestartedService.SubmitExact(
			EmptyReplayReceipt, FString(), StateGate, EmptyRestartReplay, Error));
	TestEqual(TEXT("empty restart replay status is explicit"),
		EmptyRestartReplay.Operation.Status, FString(TEXT("authorization_replay_mismatch")));
	TestEqual(TEXT("empty restart replay consumed nothing"), Authorization->CallCount, 1);
	TestEqual(TEXT("empty restart replay did not redispatch"), Adapter->Contexts.Num(), DispatchCount);
	FHyperAIStudioTypedArtifactSubmissionReceipt EmptyFailureRetry;
	TestFalse(TEXT("lost rejected-submit response is recovered by exact stage receipt"),
		RestartedService.SubmitExact(
			EmptyReplayReceipt, FString(), StateGate, EmptyFailureRetry, Error));
	TestEqual(TEXT("rejected-submit response retry preserves its exact status"),
		EmptyFailureRetry.Operation.Status, EmptyRestartReplay.Operation.Status);
	FHyperAIStudioTypedArtifactOperationStatus DurableAfterEmptyAttempt;
	TestTrue(TEXT("rejected credential does not hide durable completed status"),
		RestartedService.QueryStatus(
			Receipt.OperationId, DurableAfterEmptyAttempt, Error));
	TestTrue(TEXT("durable completed status survives the rejected attempt"),
		DurableAfterEmptyAttempt.bTerminal && DurableAfterEmptyAttempt.bReplay);

	FHyperAIStudioTypedArtifactStageReceipt BogusReplayReceipt;
	TestTrue(TEXT("completed identity stages for bogus-token restart replay"),
		RestartedService.StageExact(
			Prepared, Receipt.OperationId, Payload, BogusReplayReceipt, Error));
	TestNotEqual(TEXT("a rejected credential attempt receives a fresh exact stage lifecycle"),
		BogusReplayReceipt.StageId, EmptyReplayReceipt.StageId);
	FHyperAIStudioTypedArtifactSubmissionReceipt BogusRestartReplay;
	TestFalse(TEXT("durable risky replay rejects a replacement grant after restart"),
		RestartedService.SubmitExact(
			BogusReplayReceipt, TEXT("bogus-restart-token-0001"), StateGate,
			BogusRestartReplay, Error));
	TestEqual(TEXT("bogus restart replay status is explicit"),
		BogusRestartReplay.Operation.Status, FString(TEXT("authorization_replay_mismatch")));
	TestEqual(TEXT("bogus restart replay consumed nothing"), Authorization->CallCount, 1);
	TestEqual(TEXT("bogus restart replay did not redispatch"), Adapter->Contexts.Num(), DispatchCount);

	// Overflow the bounded response-loss table, then retry an evicted receipt. Its consumed stage
	// must be archived only by stage id: operation-level status still belongs to the durable journal.
	FHyperAIStudioTypedArtifactStageReceipt EvictedReceipt;
	FString EvictedAuthorizationToken;
	for (int32 Index = 0;
		Index < FHyperAIStudioTypedArtifactHostLimits::MaxRejectedSubmissionReceipts + 3;
		++Index)
	{
		FHyperAIStudioTypedArtifactStageReceipt RejectedReceipt;
		TestTrue(TEXT("bounded rejected receipt candidate stages"), RestartedService.StageExact(
			Prepared, Receipt.OperationId, Payload, RejectedReceipt, Error));
		const FString RejectedToken = FString::Printf(
			TEXT("bogus-eviction-token-%04d"), Index);
		if (Index == 0)
		{
			EvictedReceipt = RejectedReceipt;
			EvictedAuthorizationToken = RejectedToken;
		}
		FHyperAIStudioTypedArtifactSubmissionReceipt RejectedReplay;
		TestFalse(TEXT("bounded replacement credential is rejected"),
			RestartedService.SubmitExact(
				RejectedReceipt, RejectedToken, StateGate, RejectedReplay, Error));
		TestEqual(TEXT("bounded replacement credential status is exact"),
			RejectedReplay.Operation.Status,
			FString(TEXT("authorization_replay_mismatch")));
	}
	FHyperAIStudioTypedArtifactSubmissionReceipt EvictedRetry;
	TestFalse(TEXT("evicted consumed stage cannot be reclaimed"), RestartedService.SubmitExact(
		EvictedReceipt, EvictedAuthorizationToken, StateGate, EvictedRetry, Error));
	TestEqual(TEXT("evicted consumed stage reports claim rejection"),
		EvictedRetry.Operation.Status, FString(TEXT("stage_claim_rejected")));
	FHyperAIStudioTypedArtifactOperationStatus DurableAfterEvictedRetry;
	TestTrue(TEXT("evicted stage retry preserves durable operation query"),
		RestartedService.QueryStatus(
			Receipt.OperationId, DurableAfterEvictedRetry, Error));
	TestTrue(TEXT("evicted stage retry cannot poison completed durable identity"),
		DurableAfterEvictedRetry.bAccepted && DurableAfterEvictedRetry.bDurable
		&& DurableAfterEvictedRetry.bTerminal && DurableAfterEvictedRetry.bReplay
		&& DurableAfterEvictedRetry.Status == TEXT("completed"));
	TestEqual(TEXT("rejected receipt overflow consumed no additional grant"),
		Authorization->CallCount, 1);
	TestEqual(TEXT("rejected receipt overflow did not redispatch"),
		Adapter->Contexts.Num(), DispatchCount);

	FHyperAIStudioTypedArtifactStageReceipt RestartReceipt;
	TestTrue(TEXT("completed identity can be staged after restart"), RestartedService.StageExact(
		Prepared, Receipt.OperationId, Payload, RestartReceipt, Error));
	TestNotEqual(TEXT("the exact grant retries through another fresh stage lifecycle"),
		RestartReceipt.StageId, BogusReplayReceipt.StageId);
	FHyperAIStudioTypedArtifactSubmissionReceipt RestartReplay;
	TestTrue(TEXT("durable completed replay inspects the exact consumed grant after restart"),
		RestartedService.SubmitExact(
			RestartReceipt, AuthorizationToken, StateGate, RestartReplay, Error));
	TestTrue(TEXT("restart replay is explicit"),
		RestartReplay.Operation.bAccepted && RestartReplay.Operation.bDurable
		&& RestartReplay.Operation.bReplay && RestartReplay.Operation.bTerminal);
	TestEqual(TEXT("restart replay did not consume grant twice"), Authorization->CallCount, 1);
	TestEqual(TEXT("restart replay did not redispatch"), Adapter->Contexts.Num(), DispatchCount);
	RestartedService.Shutdown();
	FString UnregisterError;
	TestEqual(TEXT("completed replay releases optional adapter lease"),
		Registry->UnregisterAdapter(Handle, UnregisterError), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedArtifactAsyncHostFailurePolicyTest,
	"HyperAIStudio.NativeTools.TypedArtifact.AsyncHost.FailurePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedArtifactAsyncHostFailurePolicyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedArtifact::Tests;
	const FString DriftRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*DriftRoot, false, true); };
	const FString DriftProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(DriftRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> DriftClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> DriftRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> DriftAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle DriftHandle;
	FString Error;
	TestTrue(TEXT("drift adapter registers"), DriftRegistry->RegisterAdapter(DriftAdapter, DriftHandle, Error));
	FHyperAIStudioPreparedTypedArtifact DriftPrepared;
	TestTrue(TEXT("drift contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(DriftAdapter->Descriptor, DriftHandle, DriftProjectId)),
		DriftPrepared, Error));
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FTestArtifactPayload, ESPMode::ThreadSafe>();
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> DriftGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService DriftService(DriftRegistry, DriftRoot, DriftClock);
	TestTrue(TEXT("drift host starts"), DriftService.Startup(Error));
	FHyperAIStudioTypedArtifactStageReceipt ExpiredReceipt;
	TestTrue(TEXT("expiry/restage candidate stages"), DriftService.StageExact(
		DriftPrepared, TEXT("async-host-expiry-restage-0001"), Payload, ExpiredReceipt, Error));
	DriftClock->Advance(DriftPrepared.Contract.StageLifetimeMs + 1);
	FHyperAIStudioTypedArtifactSubmissionReceipt ExpiredSubmission;
	TestFalse(TEXT("expired exact stage fails claim without durable identity"), DriftService.SubmitExact(
		ExpiredReceipt, FString(), DriftGate, ExpiredSubmission, Error));
	TestEqual(TEXT("expired receipt failure is explicit"),
		ExpiredSubmission.Operation.Status, FString(TEXT("stage_claim_rejected")));
	FHyperAIStudioTypedArtifactStageReceipt RestagedReceipt;
	TestTrue(TEXT("claim failure remains stage-scoped so the same operation can restage"),
		DriftService.StageExact(
			DriftPrepared, ExpiredReceipt.OperationId, Payload, RestagedReceipt, Error));
	TestNotEqual(TEXT("restaged operation receives a fresh stage identity"),
		RestagedReceipt.StageId, ExpiredReceipt.StageId);
	FHyperAIStudioTypedArtifactSubmissionReceipt ExpiredRetry;
	TestFalse(TEXT("old expired receipt retains its exact bounded failure"), DriftService.SubmitExact(
		ExpiredReceipt, FString(), DriftGate, ExpiredRetry, Error));
	TestEqual(TEXT("old expired receipt retry preserves stage-scoped status"),
		ExpiredRetry.Operation.Status, ExpiredSubmission.Operation.Status);
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> AdmissionDeniedGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	AdmissionDeniedGate->bAllow = false;
	FHyperAIStudioTypedArtifactSubmissionReceipt AdmissionDenied;
	TestFalse(TEXT("admission-time state gate denial rejects submission"),
		DriftService.StageAndSubmitExact(
			DriftPrepared, TEXT("async-host-admission-denied-0001"), Payload, FString(),
			AdmissionDeniedGate, AdmissionDenied, Error));
	TestTrue(TEXT("admission-time denial is an explicit non-durable terminal response"),
		AdmissionDenied.Operation.bTerminal && !AdmissionDenied.Operation.bAccepted
		&& !AdmissionDenied.Operation.bDurable
		&& AdmissionDenied.Operation.Status == TEXT("execution_rejected"));
	FHyperAIStudioTypedArtifactOperationStatus AdmissionDeniedQuery;
	TestTrue(TEXT("admission-time denial remains queryable after failed runtime start"),
		DriftService.QueryStatus(
			AdmissionDenied.Stage.OperationId, AdmissionDeniedQuery, Error));
	TestTrue(TEXT("queried admission-time denial never remains prepared/nonterminal"),
		AdmissionDeniedQuery.bTerminal
		&& AdmissionDeniedQuery.Status == TEXT("execution_rejected"));
	TestEqual(TEXT("admission-time denial never reached the adapter"),
		DriftAdapter->Contexts.Num(), 0);
	FHyperAIStudioTypedArtifactSubmissionReceipt DriftSubmission;
	TestTrue(TEXT("drift candidate admits"), DriftService.StageAndSubmitExact(
		DriftPrepared, TEXT("async-host-drift-0001"), Payload, FString(), DriftGate,
		DriftSubmission, Error));
	DriftGate->bAllow = false;
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FHyperAIStudioTypedArtifactOperationStatus DriftStatus;
	TestTrue(TEXT("CAS drift result remains queryable"), DriftService.QueryStatus(
		DriftSubmission.Stage.OperationId, DriftStatus, Error));
	TestTrue(TEXT("pre-effect CAS drift fails terminal without fallback"),
		DriftStatus.bTerminal && !DriftStatus.bFallbackPermitted && !DriftStatus.bOutcomeUnknown);
	TestEqual(TEXT("CAS drift prevented adapter mutation"), DriftAdapter->Contexts.Num(), 0);
	FHyperAIStudioOperationJournal DriftJournal(DriftRoot);
	TestTrue(TEXT("pre-effect CAS drift journal reopens"), DriftJournal.Load(Error));
	const TOptional<FHyperAIStudioOperationRecord> DriftRecord =
		DriftJournal.Find(DriftSubmission.Stage.OperationId);
	TestTrue(TEXT("pre-effect CAS drift has durable retry-safe failed evidence"),
		DriftRecord.IsSet() && DriftRecord->State == EHyperAIStudioOperationState::Failed
		&& DriftRecord->bRetrySafe && !DriftRecord->bPartialCommit);
	DriftService.Shutdown();
	FString UnregisterError;
	TestEqual(TEXT("pre-effect drift releases adapter"),
		DriftRegistry->UnregisterAdapter(DriftHandle, UnregisterError),
		EHyperAIStudioDomainUnregisterResult::Removed);

	const FString CancelRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*CancelRoot, false, true); };
	const FString CancelProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(CancelRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> CancelClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> CancelRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> CancelAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle CancelHandle;
	TestTrue(TEXT("pre-pump cancel adapter registers"),
		CancelRegistry->RegisterAdapter(CancelAdapter, CancelHandle, Error));
	FHyperAIStudioPreparedTypedArtifact CancelPrepared;
	TestTrue(TEXT("pre-pump cancel contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(CancelAdapter->Descriptor, CancelHandle, CancelProjectId)),
		CancelPrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> CancelGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService CancelService(CancelRegistry, CancelRoot, CancelClock);
	TestTrue(TEXT("pre-pump cancel host starts"), CancelService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt CancelSubmission;
	TestTrue(TEXT("pre-pump cancel candidate durably admits"), CancelService.StageAndSubmitExact(
		CancelPrepared, TEXT("async-host-cancel-before-pump-0001"), Payload, FString(), CancelGate,
		CancelSubmission, Error));
	FHyperAIStudioTypedArtifactOperationStatus CancelStatus;
	TestTrue(TEXT("cancel immediately after submit closes before first pump"), CancelService.Cancel(
		CancelSubmission.Stage.OperationId, TEXT("cancel before pump"), CancelStatus, Error));
	TestTrue(TEXT("pre-pump cancel is durable retry-safe failed, not unknown"),
		CancelStatus.bTerminal && CancelStatus.bDurable && !CancelStatus.bOutcomeUnknown
		&& !CancelStatus.bFallbackPermitted);
	TestEqual(TEXT("pre-pump cancel never reached the adapter"), CancelAdapter->Contexts.Num(), 0);
	FHyperAIStudioOperationJournal CancelJournal(CancelRoot);
	TestTrue(TEXT("pre-pump cancel journal reopens"), CancelJournal.Load(Error));
	const TOptional<FHyperAIStudioOperationRecord> CancelRecord =
		CancelJournal.Find(CancelSubmission.Stage.OperationId);
	TestTrue(TEXT("pre-pump cancel persisted failed_precommit without marker"),
		CancelRecord.IsSet() && CancelRecord->State == EHyperAIStudioOperationState::Failed
		&& CancelRecord->bRetrySafe && !CancelRecord->bPartialCommit
		&& CancelRecord->StatusCode == TEXT("failed_precommit"));
	CancelService.Shutdown();
	TestEqual(TEXT("pre-pump cancel releases adapter"),
		CancelRegistry->UnregisterAdapter(CancelHandle, UnregisterError),
		EHyperAIStudioDomainUnregisterResult::Removed);

	const FString TimeoutRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*TimeoutRoot, false, true); };
	const FString TimeoutProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(TimeoutRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> TimeoutClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> TimeoutRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> TimeoutAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle TimeoutHandle;
	TestTrue(TEXT("timeout adapter registers"), TimeoutRegistry->RegisterAdapter(
		TimeoutAdapter, TimeoutHandle, Error));
	FHyperAIStudioTypedArtifactContract TimeoutContract = MakeContract(
		MakeBinding(TimeoutAdapter->Descriptor, TimeoutHandle, TimeoutProjectId));
	TimeoutContract.DeadlineMs = 100;
	FHyperAIStudioPreparedTypedArtifact TimeoutPrepared;
	TestTrue(TEXT("timeout contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		TimeoutContract, TimeoutPrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> TimeoutGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService TimeoutService(
		TimeoutRegistry, TimeoutRoot, TimeoutClock);
	TestTrue(TEXT("timeout host starts"), TimeoutService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt TimeoutSubmission;
	TestTrue(TEXT("timeout candidate durably admits"), TimeoutService.StageAndSubmitExact(
		TimeoutPrepared, TEXT("async-host-timeout-0001"), Payload, FString(), TimeoutGate,
		TimeoutSubmission, Error));
	TimeoutClock->Advance(101);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FHyperAIStudioTypedArtifactOperationStatus TimeoutStatus;
	TestTrue(TEXT("deadline result remains queryable"), TimeoutService.QueryStatus(
		TimeoutSubmission.Stage.OperationId, TimeoutStatus, Error));
	TestTrue(TEXT("deadline before first dispatch is durable retry-safe failure"),
		TimeoutStatus.bTerminal && !TimeoutStatus.bOutcomeUnknown
		&& TimeoutStatus.bDurable && !TimeoutStatus.bFallbackPermitted);
	TestEqual(TEXT("expired operation never reached adapter"), TimeoutAdapter->Contexts.Num(), 0);
	FHyperAIStudioOperationJournal TimeoutJournal(TimeoutRoot);
	TestTrue(TEXT("timeout journal reopens"), TimeoutJournal.Load(Error));
	const TOptional<FHyperAIStudioOperationRecord> TimeoutRecord =
		TimeoutJournal.Find(TimeoutSubmission.Stage.OperationId);
	TestTrue(TEXT("timeout record exists"), TimeoutRecord.IsSet());
	if (TimeoutRecord.IsSet())
	{
		TestEqual(TEXT("timeout journal state is failed before commit"), TimeoutRecord->State,
			EHyperAIStudioOperationState::Failed);
		TestTrue(TEXT("timeout before dispatch is retry-safe and has no partial commit"),
			TimeoutRecord->bRetrySafe && !TimeoutRecord->bPartialCommit
			&& TimeoutRecord->StatusCode == TEXT("failed_precommit"));
	}
	TimeoutService.Shutdown();
	TestTrue(TEXT("timeout host has no outstanding callback"), TimeoutService.CanShutdownSafely());
	TestEqual(TEXT("retry-safe timeout releases the adapter lease"),
		TimeoutRegistry->UnregisterAdapter(TimeoutHandle, UnregisterError),
		EHyperAIStudioDomainUnregisterResult::Removed);

	const FString ShutdownRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ShutdownRoot, false, true); };
	const FString ShutdownProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ShutdownRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> ShutdownClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> ShutdownRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> ShutdownAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle ShutdownHandle;
	TestTrue(TEXT("shutdown adapter registers"), ShutdownRegistry->RegisterAdapter(
		ShutdownAdapter, ShutdownHandle, Error));
	FHyperAIStudioPreparedTypedArtifact ShutdownPrepared;
	TestTrue(TEXT("shutdown contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(ShutdownAdapter->Descriptor, ShutdownHandle, ShutdownProjectId)),
		ShutdownPrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> ShutdownGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService ShutdownService(
		ShutdownRegistry, ShutdownRoot, ShutdownClock);
	TestTrue(TEXT("shutdown host starts"), ShutdownService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt ShutdownSubmission;
	TestTrue(TEXT("shutdown candidate durably admits"), ShutdownService.StageAndSubmitExact(
		ShutdownPrepared, TEXT("async-host-shutdown-0001"), Payload, FString(), ShutdownGate,
		ShutdownSubmission, Error));
	ShutdownService.Shutdown();
	FHyperAIStudioTypedArtifactOperationStatus ShutdownStatus;
	TestTrue(TEXT("shutdown result is archived/queryable"), ShutdownService.QueryStatus(
		ShutdownSubmission.Stage.OperationId, ShutdownStatus, Error));
	TestTrue(TEXT("shutdown before dispatch is durable fail-closed terminal"),
		ShutdownStatus.bTerminal && ShutdownStatus.bDurable
		&& !ShutdownStatus.bOutcomeUnknown && !ShutdownStatus.bFallbackPermitted);
	TestEqual(TEXT("shutdown never dispatched adapter action"), ShutdownAdapter->Contexts.Num(), 0);
	FHyperAIStudioOperationJournal ShutdownJournal(ShutdownRoot);
	TestTrue(TEXT("shutdown journal reopens"), ShutdownJournal.Load(Error));
	const TOptional<FHyperAIStudioOperationRecord> ShutdownRecord =
		ShutdownJournal.Find(ShutdownSubmission.Stage.OperationId);
	TestTrue(TEXT("shutdown before dispatch is durably failed before commit"),
		ShutdownRecord.IsSet() && ShutdownRecord->State == EHyperAIStudioOperationState::Failed
		&& ShutdownRecord->bRetrySafe && !ShutdownRecord->bPartialCommit
		&& ShutdownRecord->StatusCode == TEXT("failed_precommit"));
	TestTrue(TEXT("shutdown quiesces ticker and callbacks"), ShutdownService.CanShutdownSafely());

	const FString RaceRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*RaceRoot, false, true); };
	const FString RaceProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(RaceRoot);
	const TSharedRef<FReentrantTestClock, ESPMode::ThreadSafe> RaceClock =
		MakeShared<FReentrantTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> RaceRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> RaceAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle RaceHandle;
	TestTrue(TEXT("shutdown-race adapter registers"), RaceRegistry->RegisterAdapter(
		RaceAdapter, RaceHandle, Error));
	FHyperAIStudioPreparedTypedArtifact RacePrepared;
	TestTrue(TEXT("shutdown-race contract prepares"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(
			MakeContract(MakeBinding(RaceAdapter->Descriptor, RaceHandle, RaceProjectId)),
			RacePrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> RaceGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService RaceService(
		RaceRegistry, RaceRoot, RaceClock);
	TestTrue(TEXT("shutdown-race host starts"), RaceService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt RaceSubmission;
	TestTrue(TEXT("shutdown-race candidate durably admits"), RaceService.StageAndSubmitExact(
		RacePrepared, TEXT("async-host-shutdown-race-0001"), Payload, FString(), RaceGate,
		RaceSubmission, Error));
	FTSTicker::GetCoreTicker().Tick(0.0f); // Consume the no-pump admission barrier.
	FEventRef BeginShutdown(EEventMode::ManualReset);
	FEventRef ShutdownFinished(EEventMode::ManualReset);
	bool bShutdownRaceTriggered = false;
	bool bShutdownWorkerFinished = false;
	TFuture<void> ShutdownTask = Async(EAsyncExecution::Thread,
		[BeginEvent = BeginShutdown.Get(), FinishedEvent = ShutdownFinished.Get(), &RaceService]()
		{
			BeginEvent->Wait();
			RaceService.Shutdown();
			FinishedEvent->Trigger();
		});
	TestTrue(TEXT("shutdown-race worker starts"), ShutdownTask.IsValid());
	RaceClock->Callback = [&]()
	{
		if (!bShutdownRaceTriggered)
		{
			bShutdownRaceTriggered = true;
			BeginShutdown->Trigger();
			bShutdownWorkerFinished = ShutdownFinished->Wait(5000);
			TestTrue(TEXT("off-thread shutdown synchronizes inside ticker deadline sampling"),
				bShutdownWorkerFinished);
		}
	};
	FTSTicker::GetCoreTicker().Tick(0.0f);
	RaceClock->Callback = nullptr;
	TestTrue(TEXT("shutdown race reached the post-snapshot pre-pump window"),
		bShutdownRaceTriggered && bShutdownWorkerFinished);
	TestTrue(TEXT("shutdown race quiesces host before pump"), RaceService.IsQuiescing());
	TestEqual(TEXT("post-snapshot shutdown recheck prevents adapter dispatch"),
		RaceAdapter->Contexts.Num(), 0);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FHyperAIStudioTypedArtifactOperationStatus RaceStatus;
	TestTrue(TEXT("shutdown-race result remains durably queryable"), RaceService.QueryStatus(
		RaceSubmission.Stage.OperationId, RaceStatus, Error));
	TestTrue(TEXT("shutdown-race result is terminal without fallback"),
		RaceStatus.bTerminal && RaceStatus.bDurable && !RaceStatus.bFallbackPermitted);
	TestEqual(TEXT("shutdown-race drain still never dispatches adapter"),
		RaceAdapter->Contexts.Num(), 0);
	TestTrue(TEXT("shutdown-race drain releases ticker and session"),
		RaceService.CanShutdownSafely());

	const FString GateRaceRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*GateRaceRoot, false, true); };
	const FString GateRaceProjectId =
		FHyperAIStudioOperationJournal::MakeCanonicalProjectId(GateRaceRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> GateRaceClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> GateRaceRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> GateRaceAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle GateRaceHandle;
	TestTrue(TEXT("state-gate shutdown-race adapter registers"), GateRaceRegistry->RegisterAdapter(
		GateRaceAdapter, GateRaceHandle, Error));
	FHyperAIStudioPreparedTypedArtifact GateRacePrepared;
	TestTrue(TEXT("state-gate shutdown-race contract prepares"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(
			MakeContract(MakeBinding(
				GateRaceAdapter->Descriptor, GateRaceHandle, GateRaceProjectId)),
			GateRacePrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> GateRaceGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService GateRaceService(
		GateRaceRegistry, GateRaceRoot, GateRaceClock);
	TestTrue(TEXT("state-gate shutdown-race host starts"), GateRaceService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt GateRaceSubmission;
	TestTrue(TEXT("state-gate shutdown-race candidate durably admits"),
		GateRaceService.StageAndSubmitExact(
			GateRacePrepared, TEXT("async-host-state-gate-shutdown-race-0001"), Payload,
			FString(), GateRaceGate, GateRaceSubmission, Error));
	FTSTicker::GetCoreTicker().Tick(0.0f); // Consume the no-pump admission barrier.
	FEventRef BeginGateShutdown(EEventMode::ManualReset);
	FEventRef GateShutdownFinished(EEventMode::ManualReset);
	bool bGateShutdownTriggered = false;
	bool bGateShutdownWorkerFinished = false;
	TFuture<void> GateShutdownTask = Async(EAsyncExecution::Thread,
		[BeginEvent = BeginGateShutdown.Get(), FinishedEvent = GateShutdownFinished.Get(),
			&GateRaceService]()
		{
			BeginEvent->Wait();
			GateRaceService.Shutdown();
			FinishedEvent->Trigger();
		});
	TestTrue(TEXT("state-gate shutdown-race worker starts"), GateShutdownTask.IsValid());
	GateRaceGate->RevalidateCallback = [&]()
	{
		if (!bGateShutdownTriggered)
		{
			bGateShutdownTriggered = true;
			BeginGateShutdown->Trigger();
			bGateShutdownWorkerFinished = GateShutdownFinished->Wait(5000);
			TestTrue(TEXT("off-thread shutdown completes inside trusted state-gate callback"),
				bGateShutdownWorkerFinished);
		}
	};
	FTSTicker::GetCoreTicker().Tick(0.0f);
	GateRaceGate->RevalidateCallback = nullptr;
	TestTrue(TEXT("trusted state-gate shutdown race is exercised"),
		bGateShutdownTriggered && bGateShutdownWorkerFinished);
	TestTrue(TEXT("trusted state-gate shutdown quiesces before commit"),
		GateRaceService.IsQuiescing());
	TestEqual(TEXT("quiesce permit prevents adapter dispatch after state-gate callback"),
		GateRaceAdapter->Contexts.Num(), 0);
	FHyperAIStudioTypedArtifactOperationStatus GateRaceStatus;
	TestTrue(TEXT("state-gate shutdown result remains durably queryable"),
		GateRaceService.QueryStatus(
			GateRaceSubmission.Stage.OperationId, GateRaceStatus, Error));
	TestTrue(TEXT("shutdown proven before commit is terminal and retry-safe, not unknown"),
		GateRaceStatus.bTerminal && GateRaceStatus.bDurable && !GateRaceStatus.bOutcomeUnknown
		&& !GateRaceStatus.bFallbackPermitted);
	{
		FHyperAIStudioOperationJournal GateRaceJournal(GateRaceRoot);
		TestTrue(TEXT("state-gate shutdown journal reopens"), GateRaceJournal.Load(Error));
		const TOptional<FHyperAIStudioOperationRecord> GateRaceRecord =
			GateRaceJournal.Find(GateRaceSubmission.Stage.OperationId);
		TestTrue(TEXT("state-gate shutdown before commit remains retry-safe failed"),
			GateRaceRecord.IsSet() && GateRaceRecord->State == EHyperAIStudioOperationState::Failed
			&& GateRaceRecord->bRetrySafe && !GateRaceRecord->bPartialCommit);
	}
	TestTrue(TEXT("state-gate shutdown drains ticker, callbacks, and permits"),
		GateRaceService.CanShutdownSafely());
	TestTrue(TEXT("state-gate host restarts after the off-thread shutdown drain"),
		GateRaceService.Startup(Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt ReentrantGateShutdownSubmission;
	TestTrue(TEXT("game-thread state-gate shutdown candidate durably admits"),
		GateRaceService.StageAndSubmitExact(
			GateRacePrepared, TEXT("async-host-state-gate-reentrant-shutdown-0001"), Payload,
			FString(), GateRaceGate, ReentrantGateShutdownSubmission, Error));
	FTSTicker::GetCoreTicker().Tick(0.0f); // Consume the no-pump admission barrier.
	bool bGameThreadGateShutdownTriggered = false;
	GateRaceGate->RevalidateCallback = [&]()
	{
		if (!bGameThreadGateShutdownTriggered)
		{
			bGameThreadGateShutdownTriggered = true;
			GateRaceService.Shutdown();
		}
	};
	FTSTicker::GetCoreTicker().Tick(0.0f);
	GateRaceGate->RevalidateCallback = nullptr;
	TestTrue(TEXT("game-thread shutdown reentry from trusted state gate is exercised"),
		bGameThreadGateShutdownTriggered);
	TestEqual(TEXT("game-thread shutdown reentry never reaches the adapter"),
		GateRaceAdapter->Contexts.Num(), 0);
	FHyperAIStudioTypedArtifactOperationStatus ReentrantGateShutdownStatus;
	TestTrue(TEXT("game-thread state-gate shutdown remains queryable"),
		GateRaceService.QueryStatus(
			ReentrantGateShutdownSubmission.Stage.OperationId,
			ReentrantGateShutdownStatus,
			Error));
	TestTrue(TEXT("game-thread shutdown before permit is retry-safe, not unknown"),
		ReentrantGateShutdownStatus.bTerminal && ReentrantGateShutdownStatus.bDurable
		&& !ReentrantGateShutdownStatus.bOutcomeUnknown
		&& !ReentrantGateShutdownStatus.bFallbackPermitted);
	TestTrue(TEXT("game-thread state-gate shutdown drains without runtime reentry"),
		GateRaceService.CanShutdownSafely());

	const FString LateRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*LateRoot, false, true); };
	const FString LateProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(LateRoot);
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> LateClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> LateRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> LateAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor());
	FHyperAIStudioDomainRegistrationHandle LateHandle;
	TestTrue(TEXT("late-callback adapter registers"), LateRegistry->RegisterAdapter(
		LateAdapter, LateHandle, Error));
	FHyperAIStudioPreparedTypedArtifact LatePrepared;
	TestTrue(TEXT("late-callback contract prepares"), FHyperAIStudioTypedArtifactExecutor::Prepare(
		MakeContract(MakeBinding(LateAdapter->Descriptor, LateHandle, LateProjectId)),
		LatePrepared, Error));
	const TSharedRef<FTestStateGate, ESPMode::ThreadSafe> LateGate =
		MakeShared<FTestStateGate, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService LateService(LateRegistry, LateRoot, LateClock);
	TestTrue(TEXT("late-callback host starts"), LateService.Startup(Error));
	bool bShutdownReentered = false;
	LateAdapter->ExecuteCallback = [&]()
	{
		if (!bShutdownReentered)
		{
			bShutdownReentered = true;
			LateService.Shutdown();
		}
	};
	FHyperAIStudioTypedArtifactSubmissionReceipt LateSubmission;
	TestTrue(TEXT("late-callback candidate durably admits"), LateService.StageAndSubmitExact(
		LatePrepared, TEXT("async-host-late-0001"), Payload, FString(), LateGate,
		LateSubmission, Error));
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	FTSTicker::GetCoreTicker().Tick(0.0f); // The outer drain tick closes remaining work.
	LateAdapter->ExecuteCallback = nullptr;
	TestTrue(TEXT("adapter reentered shutdown after its commit permit was acquired"), bShutdownReentered);
	TestEqual(TEXT("the already-admitted adapter action drains exactly once"),
		LateAdapter->Contexts.Num(), 1);
	FHyperAIStudioTypedArtifactOperationStatus LateStatus;
	TestTrue(TEXT("admitted shutdown drain remains queryable"), LateService.QueryStatus(
		LateSubmission.Stage.OperationId, LateStatus, Error));
	TestTrue(TEXT("known apply followed by pre-dispatch shutdown closes durable partial"),
		LateStatus.bTerminal && !LateStatus.bOutcomeUnknown && LateStatus.bDurable
		&& !LateStatus.bFallbackPermitted && LateStatus.Status == TEXT("partial"));
	TestEqual(TEXT("the admitted result is accepted rather than treated as late"),
		LateStatus.LateResultCount, 0);
	TestTrue(TEXT("admitted callback drain releases ticker ownership and permit"),
		LateService.CanShutdownSafely());
	TestEqual(TEXT("known partial releases the drained adapter lease"),
		LateRegistry->UnregisterAdapter(LateHandle, UnregisterError),
		EHyperAIStudioDomainUnregisterResult::Removed);

	const FString ReconcileRoot = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ReconcileRoot, false, true); };
	const FString ReconcileOperationId = TEXT("async-host-restart-reconcile-0001");
	const FString ReconcilePlanHash =
		TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const FString ReconcileCapabilityHash =
		TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	{
		FHyperAIStudioOperationJournal AbandonedJournal(ReconcileRoot);
		TestTrue(TEXT("abandoned journal loads"), AbandonedJournal.Load(Error));
		FHyperAIStudioOperationRecord Record;
		TestEqual(TEXT("abandoned operation begins"), AbandonedJournal.BeginOperation(
			ReconcileOperationId, ReconcilePlanHash, ReconcileCapabilityHash, Record, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("abandoned operation reaches durable running"), AbandonedJournal.Transition(
			ReconcileOperationId,
			EHyperAIStudioOperationState::Queued,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			TEXT("running"),
			Record,
			Error));
	}
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> ReconcileRegistry =
		MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>();
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> ReconcileClock =
		MakeShared<FTestClock, ESPMode::ThreadSafe>();
	FHyperAIStudioTypedArtifactExecutionService ReconcileService(
		ReconcileRegistry, ReconcileRoot, ReconcileClock);
	TestTrue(TEXT("reconcile host starts"), ReconcileService.Startup(Error));
	FHyperAIStudioTypedArtifactOperationStatus Reconciled;
	TestTrue(TEXT("restart query reconciles abandoned running state"), ReconcileService.QueryStatus(
		ReconcileOperationId, Reconciled, Error));
	TestTrue(TEXT("reconciled precommit running record closes retry-safe failed"),
		Reconciled.bAccepted && Reconciled.bDurable && Reconciled.bReconciled
		&& Reconciled.bTerminal && !Reconciled.bOutcomeUnknown
		&& Reconciled.Status == TEXT("failed"));
	TestTrue(TEXT("journal-only status leaves unavailable authorization hash empty"),
		Reconciled.AuthorizationPlanHash.IsEmpty());
	TestTrue(TEXT("journal-only status leaves unavailable stage id empty"),
		Reconciled.StageId.IsEmpty());
	ReconcileService.Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
