// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioTypedPlan.h"
#include "HyperAIStudioOperationJournal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::TypedPlan::Tests
{
	constexpr const TCHAR* TestCanonicalProjectId = TEXT("canonical-project-test");

	int64 CurrentUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}
	FString ReadStep(const FString& StepId = TEXT("inspect"), const FString& DependsOn = TEXT(""))
	{
		const FString Dependencies = DependsOn.IsEmpty()
			? TEXT("[]")
			: FString::Printf(TEXT("[\"%s\"]"), *DependsOn);
		return FString::Printf(TEXT(R"json(
{
  "id":"%s",
  "operation":"foundation.inspect",
  "depends_on":%s,
  "arguments":{"subject":"/Game/Test","fields":["name","class"]},
  "preconditions":[{"kind":"object_exists","target":"/Game/Test"}],
  "effects":[],
  "budget":{"max_native_operations":1,"max_game_thread_ms":2,"max_output_bytes":1024}
})json"), *StepId, *Dependencies);
	}

	FString BlueprintStep(const FString& StepId, const FString& DependsOn = TEXT(""), const bool bReverseObjectFields = false)
	{
		const FString Dependencies = DependsOn.IsEmpty()
			? TEXT("[]")
			: FString::Printf(TEXT("[\"%s\"]"), *DependsOn);
		const FString Arguments = bReverseObjectFields
			? TEXT("{\"patch_id\":\"patch-01\",\"asset_path\":\"/Game/BP_Test\"}")
			: TEXT("{\"asset_path\":\"/Game/BP_Test\",\"patch_id\":\"patch-01\"}");
		return FString::Printf(TEXT(R"json(
{
  "id":"%s",
  "operation":"blueprint.apply_patch",
  "depends_on":%s,
  "arguments":%s,
	  "preconditions":[{"kind":"object_exists","target":"/Game/BP_Test"},{"kind":"revision_equals","target":"/Game/BP_Test","expected":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],
  "effects":[{"kind":"object_updated","target":"/Game/BP_Test","validator_id":"blueprint.compile_validate","expected":"compiled"}],
  "budget":{"max_native_operations":4,"max_game_thread_ms":20,"max_output_bytes":1024}
})json"), *StepId, *Dependencies, *Arguments);
	}

	FString DeleteStep()
	{
		return TEXT(R"json(
{
  "id":"delete_asset",
  "operation":"asset.delete",
  "depends_on":[],
  "arguments":{"asset_path":"/Game/Old"},
  "preconditions":[{"kind":"object_exists","target":"/Game/Old"}],
  "effects":[{"kind":"object_deleted","target":"/Game/Old","validator_id":"asset.absence"}],
  "budget":{"max_native_operations":2,"max_game_thread_ms":10,"max_output_bytes":1024}
	})json");
	}

	FString BlueprintDeleteStep(const FString& StepId = TEXT("delete_blueprint_nodes"))
	{
		return FString::Printf(TEXT(R"json(
{
  "id":"%s",
  "operation":"blueprint.delete_patch",
  "depends_on":[],
  "arguments":{"asset_path":"/Game/BP_Test","patch_id":"delete-patch-01"},
  "preconditions":[{"kind":"object_exists","target":"/Game/BP_Test"},{"kind":"revision_equals","target":"/Game/BP_Test","expected":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],
  "effects":[{"kind":"object_updated","target":"/Game/BP_Test","validator_id":"blueprint.delete_compile_validate","expected":"compiled"},{"kind":"object_deleted","target":"/Game/BP_Test","validator_id":"blueprint.delete_compile_validate","expected":"requested_nodes_absent"}],
  "budget":{"max_native_operations":4,"max_game_thread_ms":20,"max_output_bytes":2048}
})json"), *StepId);
	}

	FString PlaytestStep()
	{
		return TEXT(R"json(
{
  "id":"run_playtest",
  "operation":"playtest.run",
  "depends_on":[],
  "arguments":{"scenario_id":"movement-smoke","max_seconds":30},
  "preconditions":[{"kind":"editor_state_equals","target":"pie","expected":"stopped"}],
  "effects":[{"kind":"runtime_external_effect","target":"pie","validator_id":"playtest.assertions"}],
  "budget":{"max_native_operations":3,"max_game_thread_ms":10,"max_output_bytes":2048}
})json");
	}

	FString MakePlan(
		const FString& CapabilityHash,
		const FString& Steps,
		const bool bDryRun,
		const FString& OperationId = FString(),
		const FString& AuthorizationToken = FString(),
		const int32 MaxSteps = 16,
		const int32 MaxMutations = 8,
		const int32 MaxNativeOperations = 64,
		const int32 MaxGameThreadMs = 500,
		const int32 MaxOutputBytes = 65536)
	{
		const FString OperationIdField = OperationId.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\"operation_id\":\"%s\""), *OperationId);
		const FString AuthorizationField = AuthorizationToken.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\"authorization_token\":\"%s\""), *AuthorizationToken);
		return FString::Printf(TEXT(R"json(
{
  "schema":"hyperai.plan.v1",
  "dry_run":%s,
  "capability_hash":"%s"%s%s,
  "budget":{
    "deadline_ms":10000,
    "max_steps":%d,
    "max_mutations":%d,
    "max_native_operations":%d,
    "max_game_thread_ms":%d,
    "max_output_bytes":%d
  },
  "steps":[%s]
})json"),
			bDryRun ? TEXT("true") : TEXT("false"),
			*CapabilityHash,
			*OperationIdField,
			*AuthorizationField,
			MaxSteps,
			MaxMutations,
			MaxNativeOperations,
			MaxGameThreadMs,
			MaxOutputBytes,
			*Steps);
	}

	FString FirstCode(const FHyperAIStudioPlanDryRunResult& Result)
	{
		return Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code;
	}

	bool Validate(
		const FString& Json,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		FHyperAIStudioValidatedPlan& OutPlan,
		FHyperAIStudioPlanDryRunResult& OutResult)
	{
		return FHyperAIStudioTypedPlanValidator::ValidateJson(Json, Registry, OutPlan, OutResult);
	}

	class FFakeJournalGate final : public IHyperAIStudioPlanJournalGate
	{
	public:
		EHyperAIStudioPlanJournalBegin BeginResult = EHyperAIStudioPlanJournalBegin::ProceedNew;
		bool bFailTransitions = false;
		int32 BeginCalls = 0;
		int32 TransitionCalls = 0;
		FString BoundOperationId;
		FString BoundPlanHash;
		FString BoundCapabilityHash;
		TArray<EHyperAIStudioPlanJournalTransition> Transitions;

		virtual EHyperAIStudioPlanJournalBegin Begin(
			const FString& OperationId,
			const FString& PlanHash,
			const FString& CapabilityHash,
			FString& OutError) override
		{
			++BeginCalls;
			BoundOperationId = OperationId;
			BoundPlanHash = PlanHash;
			BoundCapabilityHash = CapabilityHash;
			if (BeginResult != EHyperAIStudioPlanJournalBegin::ProceedNew
				&& BeginResult != EHyperAIStudioPlanJournalBegin::ReplayCompleted)
			{
				OutError = TEXT("fake begin rejection");
			}
			return BeginResult;
		}

		virtual bool Transition(
			const FString& OperationId,
			const EHyperAIStudioPlanJournalTransition Transition,
			const FHyperAIStudioPlanOutcomeEvidence& Evidence,
			FString& OutError) override
		{
			++TransitionCalls;
			if (OperationId != BoundOperationId)
			{
				OutError = TEXT("unexpected operation id");
				return false;
			}
			if (bFailTransitions)
			{
				OutError = TEXT("fake transition persistence failure");
				return false;
			}
			Transitions.Add(Transition);
			return true;
		}
	};

	class FFakeClock final : public IHyperAIStudioPlanClock
	{
	public:
		int64 MonotonicMs = 1000;
		int64 UtcMs = CurrentUtcMs();
		virtual int64 NowMonotonicMs() const override { return MonotonicMs; }
		virtual int64 NowUtcMs() const override { return UtcMs; }
	};

	class FFakeAuthorizationGate final : public IHyperAIStudioPlanAuthorizationGate
	{
	public:
		FHyperAIStudioPlanAuthorizationRequest BoundRequest;
		FString Nonce = TEXT("server-nonce-001");
		FString ConsumeNonceOverride;
		int64 ExpiresUtcMs = CurrentUtcMs() + 60000;
		bool bConsumed = false;
		int32 InspectCalls = 0;
		int32 ConsumeCalls = 0;

		virtual bool Inspect(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++InspectCalls;
			if (Request.Token != BoundRequest.Token || Request.CanonicalProjectId != BoundRequest.CanonicalProjectId
				|| Request.OperationId != BoundRequest.OperationId
				|| Request.AuthorizationPlanHash != BoundRequest.AuthorizationPlanHash || Request.Safety != BoundRequest.Safety
				|| NowUtcMs >= ExpiresUtcMs)
			{
				OutError = TEXT("fake authorization binding mismatch");
				return false;
			}
			OutReceipt = {BoundRequest, Nonce, ExpiresUtcMs,
				bConsumed ? EHyperAIStudioPlanAuthorizationState::AlreadyConsumed : EHyperAIStudioPlanAuthorizationState::Available};
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
				OutError = TEXT("fake authorization token already consumed");
				return false;
			}
			bConsumed = true;
			if (!ConsumeNonceOverride.IsEmpty())
			{
				OutReceipt.Nonce = ConsumeNonceOverride;
			}
			OutReceipt.State = EHyperAIStudioPlanAuthorizationState::AlreadyConsumed;
			return true;
		}
	};

	class FFakeValidatorReceiptGate final : public IHyperAIStudioPlanValidatorReceiptGate
	{
	public:
		bool bReject = false;
		int32 VerifyCalls = 0;

		virtual bool Verify(
			const FHyperAIStudioPlanValidatorReceipt& Receipt,
			const int64 NowUtcMs,
			FString& OutError) override
		{
			++VerifyCalls;
			if (bReject || Receipt.ServerReceiptToken != TEXT("server-validator-receipt-v1")
				|| NowUtcMs < Receipt.IssuedUtcMs || NowUtcMs >= Receipt.ExpiresUtcMs)
			{
				OutError = TEXT("fake validator receipt rejected");
				return false;
			}
			return true;
		}
	};

	FFakeValidatorReceiptGate& ValidatorGate()
	{
		static FFakeValidatorReceiptGate Gate;
		return Gate;
	}

	FHyperAIStudioPlanActionResult ActionResult(const EHyperAIStudioPlanActionOutcome Outcome)
	{
		FHyperAIStudioPlanActionResult Result;
		Result.Outcome = Outcome;
		if (Outcome == EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect)
		{
			Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
		}
		else if (Outcome == EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence)
		{
			Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Complete;
		}
		return Result;
	}

	FHyperAIStudioPlanActionResult BoundActionResult(
		FHyperAIStudioSerialPlanCoordinator& Coordinator,
		const EHyperAIStudioPlanActionOutcome Outcome)
	{
		FHyperAIStudioPlanActionResult Result = ActionResult(Outcome);
		const bool bNeedsReceipt = Outcome == EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence
			|| (Outcome == EHyperAIStudioPlanActionOutcome::Succeeded
				&& Coordinator.GetSchedule().IsValidIndex(Coordinator.GetCompletedActionCount())
				&& Coordinator.GetSchedule()[Coordinator.GetCompletedActionCount()].Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce);
		if (bNeedsReceipt)
		{
			FHyperAIStudioPlanValidatorReceipt& Receipt = Result.Evidence.ValidatorReceipt;
			Receipt.ServerReceiptToken = TEXT("server-validator-receipt-v1");
			Receipt.ReceiptFingerprint = TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
			Receipt.CanonicalProjectId = Coordinator.GetActiveCanonicalProjectId();
			Receipt.OperationId = Coordinator.GetActiveOperationId();
			Receipt.PlanHash = Coordinator.GetActivePlanHash();
			Receipt.CapabilityHash = Coordinator.GetActiveCapabilityHash();
			Receipt.EffectFingerprint = Coordinator.GetActiveEffectFingerprint();
			Receipt.ActionNonce = Coordinator.GetActiveActionNonce();
			Receipt.ValidatorId = Outcome == EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence
				? FHyperAIStudioTypedPlanValidator::RollbackValidatorId()
				: FHyperAIStudioTypedPlanValidator::FreshValidatorId();
			Receipt.ApprovedValidatorFingerprint = Outcome == EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence
				? FHyperAIStudioTypedPlanValidator::RollbackValidatorFingerprint()
				: FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint();
			Receipt.PostconditionHash = TEXT("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
			Receipt.IssuedUtcMs = Coordinator.GetCurrentUtcMs() - 1000;
			Receipt.ExpiresUtcMs = Receipt.IssuedUtcMs + 300000;
		}
		return Result;
	}

	bool Complete(
		FHyperAIStudioSerialPlanCoordinator& Coordinator,
		const EHyperAIStudioPlanActionOutcome Outcome,
		FString& OutError)
	{
		return Coordinator.CompleteCurrentAction(BoundActionResult(Coordinator, Outcome), OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanValidationTest,
	"HyperAIStudio.NativeTools.TypedPlan.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	TestTrue(TEXT("Foundation registry has a lowercase SHA-256 capability fingerprint"),
		CapabilityHash.StartsWith(TEXT("sha256:")) && CapabilityHash.Len() == 71);
	TestEqual(TEXT("Foundation registry exposes six closed operation variants"), Registry.GetOperations().Num(), 6);

	FString MetadataError;
	FHyperAIStudioTypedOperationRegistry BadRegistry;
	FHyperAIStudioTypedOperationMetadata BadRead;
	BadRead.TypeId = TEXT("unsafe.read");
	BadRead.Safety = EHyperAIStudioPlanSafety::Read;
	BadRead.AllowedEffects = {EHyperAIStudioPlanEffectKind::ObjectUpdated};
	TestFalse(TEXT("Registry rejects read metadata that claims effects"), BadRegistry.Add(BadRead, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadMutation;
	BadMutation.TypeId = TEXT("unsafe.mutate");
	BadMutation.Safety = EHyperAIStudioPlanSafety::Edit;
	TestFalse(TEXT("Registry rejects mutations without preconditions, effects, and validator"), BadRegistry.Add(BadMutation, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadReadFinalizer;
	BadReadFinalizer.TypeId = TEXT("unsafe.read_finalizer");
	BadReadFinalizer.Safety = EHyperAIStudioPlanSafety::Read;
	BadReadFinalizer.bSaveOnce = true;
	TestFalse(TEXT("Registry rejects mutating finalizers on read operations"), BadRegistry.Add(BadReadFinalizer, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadEnum;
	BadEnum.TypeId = TEXT("unsafe.enum");
	BadEnum.Safety = static_cast<EHyperAIStudioPlanSafety>(255);
	TestFalse(TEXT("Registry rejects unknown safety enum values"), BadRegistry.Add(BadEnum, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadValueEnum;
	BadValueEnum.TypeId = TEXT("unsafe.value_enum");
	BadValueEnum.Arguments = {{TEXT("value"), static_cast<EHyperAIStudioPlanValueType>(255), true}};
	TestFalse(TEXT("Registry rejects unknown value-type enum values"), BadRegistry.Add(BadValueEnum, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadPreconditionEnum;
	BadPreconditionEnum.TypeId = TEXT("unsafe.precondition_enum");
	BadPreconditionEnum.AllowedPreconditions = {static_cast<EHyperAIStudioPlanPreconditionKind>(255)};
	TestFalse(TEXT("Registry rejects unknown precondition enum values"),
		BadRegistry.Add(BadPreconditionEnum, MetadataError));
	FHyperAIStudioTypedOperationMetadata BadEffectEnum;
	BadEffectEnum.TypeId = TEXT("unsafe.effect_enum");
	BadEffectEnum.AllowedEffects = {static_cast<EHyperAIStudioPlanEffectKind>(255)};
	TestFalse(TEXT("Registry rejects unknown effect enum values"), BadRegistry.Add(BadEffectEnum, MetadataError));
	FHyperAIStudioTypedOperationMetadata UnderclassifiedEffect;
	UnderclassifiedEffect.TypeId = TEXT("unsafe.effect_risk");
	UnderclassifiedEffect.Safety = EHyperAIStudioPlanSafety::Edit;
	UnderclassifiedEffect.AllowedEffects = {EHyperAIStudioPlanEffectKind::ObjectDeleted};
	TestFalse(TEXT("Registry rejects an effect riskier than its operation safety"),
		BadRegistry.Add(UnderclassifiedEffect, MetadataError));

	FHyperAIStudioValidatedPlan Plan;
	FHyperAIStudioPlanDryRunResult Result;
	const FString ReadJson = MakePlan(CapabilityHash, ReadStep(), true, FString(), FString(), 4, 0);
	TestTrue(TEXT("A fully bounded read plan validates without operation_id"), Validate(ReadJson, Registry, Plan, Result));
	TestTrue(TEXT("Validated flag is set only after every gate passes"), Plan.bValidated);
	TestFalse(TEXT("Read plan is not classified as mutation"), Plan.bHasMutation);
	TestEqual(TEXT("Read safety is registry-derived"), Plan.MaximumSafety, EHyperAIStudioPlanSafety::Read);
	TestEqual(TEXT("Read plan contains one scheduled action"), Result.Schedule.Num(), 1);

	auto ExpectInvalid = [this, &Registry](const FString& Label, const FString& Json, const FString& ExpectedCode)
	{
		FHyperAIStudioValidatedPlan InvalidPlan;
		FHyperAIStudioPlanDryRunResult InvalidResult;
		TestFalse(Label, Validate(Json, Registry, InvalidPlan, InvalidResult));
		TestEqual(Label + TEXT(" diagnostic"), FirstCode(InvalidResult), ExpectedCode);
	};

	ExpectInvalid(TEXT("Malformed JSON is rejected"), TEXT("{"), TEXT("invalid_json"));
	ExpectInvalid(TEXT("Duplicate top-level key is rejected before Unreal JSON collapse"),
		ReadJson.Replace(TEXT("\"schema\":\"hyperai.plan.v1\""),
			TEXT("\"schema\":\"hyperai.plan.v1\",\"schema\":\"hyperai.plan.v1\"")),
		TEXT("duplicate_json_key"));
	ExpectInvalid(TEXT("Unicode-equivalent duplicate top-level key is rejected"),
		ReadJson.Replace(TEXT("\"schema\":\"hyperai.plan.v1\""),
			TEXT("\"schema\":\"hyperai.plan.v1\",\"sch\\u0065ma\":\"hyperai.plan.v1\"")),
		TEXT("duplicate_json_key"));
	ExpectInvalid(TEXT("Duplicate step key is rejected before semantic validation"),
		ReadJson.Replace(TEXT("\"id\":\"inspect\""), TEXT("\"id\":\"inspect\",\"id\":\"inspect\"")),
		TEXT("duplicate_json_key"));
	ExpectInvalid(TEXT("Duplicate argument key is rejected before typed parsing"),
		ReadJson.Replace(TEXT("\"subject\":\"/Game/Test\""),
			TEXT("\"subject\":\"/Game/Test\",\"subject\":\"/Game/Test\"")),
		TEXT("duplicate_json_key"));
	ExpectInvalid(TEXT("Unknown top-level field is rejected"),
		ReadJson.Replace(TEXT("\"steps\""), TEXT("\"extra\":1,\"steps\"")), TEXT("unknown_field"));
	ExpectInvalid(TEXT("Legacy client-supplied confirmation claims are rejected"),
		ReadJson.Replace(TEXT("\"steps\""), TEXT("\"confirmations\":{\"destructive\":true},\"steps\"")),
		TEXT("unknown_field"));
	ExpectInvalid(TEXT("Wrong schema is rejected"),
		ReadJson.Replace(TEXT("hyperai.plan.v1"), TEXT("hyperai.plan.v2")), TEXT("unsupported_schema"));
	ExpectInvalid(TEXT("Wrong capability fingerprint is rejected"),
		ReadJson.Replace(*CapabilityHash, TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")),
		TEXT("capability_mismatch"));
	ExpectInvalid(TEXT("Unknown operation dispatch is rejected"),
		ReadJson.Replace(TEXT("foundation.inspect"), TEXT("arbitrary.call_tool")), TEXT("operation_not_allowlisted"));
	ExpectInvalid(TEXT("Unknown typed argument is rejected"),
		ReadJson.Replace(TEXT("\"subject\":\"/Game/Test\""), TEXT("\"subject\":\"/Game/Test\",\"script\":\"anything\"")),
		TEXT("unknown_argument"));
	ExpectInvalid(TEXT("Missing required argument is rejected"),
		ReadJson.Replace(TEXT("\"subject\":\"/Game/Test\","), TEXT("")), TEXT("missing_argument"));
	ExpectInvalid(TEXT("Wrong argument type is rejected"),
		ReadJson.Replace(TEXT("\"fields\":[\"name\",\"class\"]"), TEXT("\"fields\":42")), TEXT("invalid_argument"));
	ExpectInvalid(TEXT("Duplicate string-array entries are rejected"),
		ReadJson.Replace(TEXT("[\"name\",\"class\"]"), TEXT("[\"name\",\"name\"]")), TEXT("invalid_argument"));
	const FString EditDryRun = MakePlan(CapabilityHash, BlueprintStep(TEXT("patch")), true, TEXT("operation-edit-001"));
	ExpectInvalid(TEXT("Metadata-required mutation precondition cannot be omitted"),
		EditDryRun.Replace(
			TEXT("[{\"kind\":\"object_exists\",\"target\":\"/Game/BP_Test\"},{\"kind\":\"revision_equals\",\"target\":\"/Game/BP_Test\",\"expected\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]"),
			TEXT("[]")),
		TEXT("missing_precondition"));
	ExpectInvalid(TEXT("Effect validator cannot be client-substituted"),
		EditDryRun.Replace(TEXT("blueprint.compile_validate"), TEXT("blueprint.untrusted")),
		TEXT("validator_mismatch"));
	ExpectInvalid(TEXT("Precondition target must be derived from its typed target argument"),
		EditDryRun.Replace(TEXT("\"target\":\"/Game/BP_Test\""), TEXT("\"target\":\"/Game/Other\""),
			ESearchCase::CaseSensitive),
		TEXT("precondition_target_mismatch"));
	ExpectInvalid(TEXT("Effect target must be derived from its typed target argument"),
		EditDryRun.Replace(TEXT("\"effects\":[{\"kind\":\"object_updated\",\"target\":\"/Game/BP_Test\""),
			TEXT("\"effects\":[{\"kind\":\"object_updated\",\"target\":\"/Game/Other\"")),
		TEXT("effect_target_mismatch"));
	ExpectInvalid(TEXT("Step budget below metadata estimate is rejected"),
		ReadJson.Replace(TEXT("\"max_game_thread_ms\":2"), TEXT("\"max_game_thread_ms\":1")), TEXT("step_budget_too_small"));
	ExpectInvalid(TEXT("Fractional-looking budget values are never rounded into integers"),
		ReadJson.Replace(TEXT("\"deadline_ms\":10000"), TEXT("\"deadline_ms\":1.0000000000000000001")),
		TEXT("bounded_integer"));
	ExpectInvalid(TEXT("Fractional-looking typed integer arguments are never rounded"),
		MakePlan(CapabilityHash,
			PlaytestStep().Replace(TEXT("\"max_seconds\":30"), TEXT("\"max_seconds\":30.0000000000000000001")),
			true, TEXT("operation-pie-integer")),
		TEXT("invalid_argument"));
	ExpectInvalid(TEXT("Aggregate output budget is enforced"),
		ReadJson.Replace(TEXT("\"max_output_bytes\":65536"), TEXT("\"max_output_bytes\":512")), TEXT("aggregate_budget"));
	ExpectInvalid(TEXT("max_steps is enforced independently of the hard ceiling"),
		MakePlan(CapabilityHash, ReadStep(TEXT("one")) + TEXT(",") + ReadStep(TEXT("two")), true,
			FString(), FString(), 1, 0), TEXT("step_limit"));

	const FString MissingDependency = MakePlan(CapabilityHash, ReadStep(TEXT("two"), TEXT("missing")), true, FString(), FString(), 4, 0);
	ExpectInvalid(TEXT("Missing dependency is rejected"), MissingDependency, TEXT("missing_dependency"));
	const FString CycleSteps = ReadStep(TEXT("one"), TEXT("two")) + TEXT(",") + ReadStep(TEXT("two"), TEXT("one"));
	ExpectInvalid(TEXT("Dependency cycles are rejected"), MakePlan(CapabilityHash, CycleSteps, true, FString(), FString(), 4, 0),
		TEXT("dependency_cycle"));
	const FString DuplicateSteps = ReadStep(TEXT("same")) + TEXT(",") + ReadStep(TEXT("same"));
	ExpectInvalid(TEXT("Duplicate step ids are rejected"), MakePlan(CapabilityHash, DuplicateSteps, true, FString(), FString(), 4, 0),
		TEXT("duplicate_step_id"));

	FString Oversized = ReadJson;
	Oversized += FString::ChrN(FHyperAIStudioPlanLimits::MaxPlanJsonBytes + 1, TEXT(' '));
	ExpectInvalid(TEXT("UTF-8 payload hard bound is enforced before parsing"), Oversized, TEXT("plan_size"));

	FString DeepValue = TEXT("\"name\"");
	for (int32 Depth = 0; Depth < FHyperAIStudioPlanLimits::MaxJsonDepth + 1; ++Depth)
	{
		DeepValue = TEXT("[") + DeepValue + TEXT("]");
	}
	ExpectInvalid(TEXT("JSON depth is bounded before DOM construction"),
		ReadJson.Replace(TEXT("[\"name\",\"class\"]"), *DeepValue), TEXT("json_depth_limit"));

	FString ManyNodes = TEXT("[0");
	for (int32 Index = 0; Index < FHyperAIStudioPlanLimits::MaxJsonNodes + 1; ++Index)
	{
		ManyNodes += TEXT(",0");
	}
	ManyNodes += TEXT("]");
	ExpectInvalid(TEXT("JSON node count is bounded before DOM construction"),
		ReadJson.Replace(TEXT("[\"name\",\"class\"]"), *ManyNodes), TEXT("json_node_limit"));

	FString InvalidUtf16 = ReadJson;
	FString LoneHighSurrogate;
	LoneHighSurrogate.AppendChar(static_cast<TCHAR>(0xd800));
	const FString InvalidTarget = FString(TEXT("/Game/")) + LoneHighSurrogate;
	InvalidUtf16.ReplaceInline(TEXT("/Game/Test"), *InvalidTarget);
	ExpectInvalid(TEXT("Malformed raw UTF-16 is rejected before UTF-8 conversion"), InvalidUtf16, TEXT("invalid_json"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanSafetyHashTest,
	"HyperAIStudio.NativeTools.TypedPlan.SafetyAndHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanSafetyHashTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	FHyperAIStudioValidatedPlan PlanA;
	FHyperAIStudioPlanDryRunResult ResultA;
	const FString EditJsonA = MakePlan(CapabilityHash, BlueprintStep(TEXT("patch"), FString(), false), false, TEXT("operation-edit-001"));
	TestTrue(TEXT("Execute-mode edit plan validates with operation_id"), Validate(EditJsonA, Registry, PlanA, ResultA));
	TestTrue(TEXT("Edit is classified as mutation from registry metadata"), PlanA.bHasMutation);
	TestEqual(TEXT("Edit maximum safety"), PlanA.MaximumSafety, EHyperAIStudioPlanSafety::Edit);
	TestTrue(TEXT("Dry-run result requires idempotency for mutation"), ResultA.bRequiresOperationId);
	TestEqual(TEXT("One edit step is counted"), ResultA.MutationStepCount, 1);

	FHyperAIStudioValidatedPlan PlanB;
	FHyperAIStudioPlanDryRunResult ResultB;
	const FString EditJsonB = MakePlan(CapabilityHash, BlueprintStep(TEXT("patch"), FString(), true), false, TEXT("operation-edit-999"));
	TestTrue(TEXT("Equivalent plan with reordered object keys validates"), Validate(EditJsonB, Registry, PlanB, ResultB));
	TestEqual(TEXT("Canonical hash ignores JSON object order and operation_id"), PlanA.PlanHash, PlanB.PlanHash);
	TestTrue(TEXT("Canonical plan hash is a journal-compatible SHA-256 token"), PlanA.PlanHash.Len() == 71 && PlanA.PlanHash.StartsWith(TEXT("sha256:")));
	FHyperAIStudioValidatedPlan StepOrderA;
	FHyperAIStudioValidatedPlan StepOrderB;
	FHyperAIStudioPlanDryRunResult StepOrderResultA;
	FHyperAIStudioPlanDryRunResult StepOrderResultB;
	const FString IndependentOne = BlueprintStep(TEXT("one"));
	const FString IndependentTwo = BlueprintStep(TEXT("two"));
	TestTrue(TEXT("First independent-step ordering validates"), Validate(
		MakePlan(CapabilityHash, IndependentOne + TEXT(",") + IndependentTwo, false, TEXT("operation-order-a")),
		Registry, StepOrderA, StepOrderResultA));
	TestTrue(TEXT("Second independent-step ordering validates"), Validate(
		MakePlan(CapabilityHash, IndependentTwo + TEXT(",") + IndependentOne, false, TEXT("operation-order-b")),
		Registry, StepOrderB, StepOrderResultB));
	TestEqual(TEXT("Canonical plan hash ignores non-semantic step-array order"), StepOrderA.PlanHash, StepOrderB.PlanHash);

	FHyperAIStudioValidatedPlan ExpectedHashPlan;
	FHyperAIStudioPlanDryRunResult ExpectedHashResult;
	const FString WithExpectedHash = EditJsonA.Replace(TEXT("\"steps\""),
		*FString::Printf(TEXT("\"expected_plan_hash\":\"%s\",\"steps\""), *PlanA.PlanHash));
	TestTrue(TEXT("Matching expected_plan_hash is admitted"), Validate(WithExpectedHash, Registry, ExpectedHashPlan, ExpectedHashResult));
	FHyperAIStudioValidatedPlan MismatchPlan;
	FHyperAIStudioPlanDryRunResult MismatchResult;
	const FString BadExpected = WithExpectedHash.Replace(*PlanA.PlanHash,
		TEXT("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
	TestFalse(TEXT("Mismatched expected_plan_hash fails closed"), Validate(BadExpected, Registry, MismatchPlan, MismatchResult));
	TestEqual(TEXT("Hash mismatch diagnostic"), FirstCode(MismatchResult), TEXT("plan_hash_mismatch"));

	FHyperAIStudioValidatedPlan MissingIdPlan;
	FHyperAIStudioPlanDryRunResult MissingIdResult;
	TestFalse(TEXT("Mutation without operation_id is rejected"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("patch")), true), Registry, MissingIdPlan, MissingIdResult));
	TestEqual(TEXT("Missing id diagnostic"), FirstCode(MissingIdResult), TEXT("operation_id_required"));

	FHyperAIStudioValidatedPlan DeletePlan;
	FHyperAIStudioPlanDryRunResult DeleteResult;
	TestFalse(TEXT("Execute-mode destructive plan requires server authorization"),
		Validate(MakePlan(CapabilityHash, DeleteStep(), false, TEXT("operation-delete-001")), Registry, DeletePlan, DeleteResult));
	TestEqual(TEXT("Destructive authorization diagnostic"), FirstCode(DeleteResult), TEXT("authorization_required"));
	TestTrue(TEXT("Opaque server-issued authorization token admits destructive validation"),
		Validate(MakePlan(CapabilityHash, DeleteStep(), false, TEXT("operation-delete-001"), TEXT("server-token-delete-001")),
			Registry, DeletePlan, DeleteResult));
	TestEqual(TEXT("Destructive safety is registry-derived"), DeletePlan.MaximumSafety, EHyperAIStudioPlanSafety::Destructive);
	FHyperAIStudioValidatedPlan DeleteDryPlan;
	FHyperAIStudioPlanDryRunResult DeleteDryResult;
	TestTrue(TEXT("Destructive dry-run needs no bearer token"),
		Validate(MakePlan(CapabilityHash, DeleteStep(), true, TEXT("operation-delete-preview")),
			Registry, DeleteDryPlan, DeleteDryResult));
	TestEqual(TEXT("Dry-run and token-bearing execution bind the same canonical destructive plan"),
		DeleteDryPlan.PlanHash, DeletePlan.PlanHash);
	TestEqual(TEXT("Authorization plan hash is bearer-token and envelope independent"),
		DeleteDryPlan.AuthorizationPlanHash, DeletePlan.AuthorizationPlanHash);
	TestEqual(TEXT("Declared-effect fingerprint is execution-envelope independent"),
		DeleteDryPlan.EffectFingerprint, DeletePlan.EffectFingerprint);
	TestTrue(TEXT("Declared-effect fingerprint is a journal-compatible SHA-256 token"),
		DeletePlan.EffectFingerprint.StartsWith(TEXT("sha256:")) && DeletePlan.EffectFingerprint.Len() == 71);
	FHyperAIStudioValidatedPlan ExecuteFromDryHashPlan;
	FHyperAIStudioPlanDryRunResult ExecuteFromDryHashResult;
	const FString ExecuteFromDryHashJson = MakePlan(
		CapabilityHash, DeleteStep(), false, TEXT("operation-delete-approved"), TEXT("server-token-delete-002"))
		.Replace(TEXT("\"steps\""),
			*FString::Printf(TEXT("\"expected_plan_hash\":\"%s\",\"steps\""), *DeleteDryPlan.PlanHash));
	TestTrue(TEXT("Dry-run canonical hash can be approved and reused by exact execute request"),
		Validate(ExecuteFromDryHashJson, Registry, ExecuteFromDryHashPlan, ExecuteFromDryHashResult));
	FHyperAIStudioValidatedPlan ApprovedTamperPlan;
	FHyperAIStudioPlanDryRunResult ApprovedTamperResult;
	TestFalse(TEXT("Mutation target tamper after dry-run approval is rejected"), Validate(
		ExecuteFromDryHashJson.Replace(TEXT("/Game/Old"), TEXT("/Game/Other")),
		Registry, ApprovedTamperPlan, ApprovedTamperResult));
	TestEqual(TEXT("Approved mutation tamper fails the canonical hash gate"),
		FirstCode(ApprovedTamperResult), TEXT("plan_hash_mismatch"));

	FHyperAIStudioValidatedPlan ExternalPlan;
	FHyperAIStudioPlanDryRunResult ExternalResult;
	TestFalse(TEXT("Execute-mode external plan requires server authorization"),
		Validate(MakePlan(CapabilityHash, PlaytestStep(), false, TEXT("operation-pie-001")), Registry, ExternalPlan, ExternalResult));
	TestEqual(TEXT("External authorization diagnostic"), FirstCode(ExternalResult), TEXT("authorization_required"));
	TestTrue(TEXT("Dry-run may inspect external plan without authorizing the effect"),
		Validate(MakePlan(CapabilityHash, PlaytestStep(), true, TEXT("operation-pie-001")), Registry, ExternalPlan, ExternalResult));
	TestTrue(TEXT("Dry-run clearly reports future external authorization"), ExternalResult.bRequiresExternalEffectAuthorization);

	FHyperAIStudioValidatedPlan DryEditPlan;
	FHyperAIStudioPlanDryRunResult DryEditResult;
	TestTrue(TEXT("Equivalent dry-run edit validates"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("patch")), true, TEXT("operation-edit-dry")),
			Registry, DryEditPlan, DryEditResult));
	TestEqual(TEXT("Authorization hash is independent of execute/dry-run mode and operation id"),
		PlanA.AuthorizationPlanHash, DryEditPlan.AuthorizationPlanHash);
	TestEqual(TEXT("Canonical plan hash is reusable from dry-run approval for exact execution"),
		PlanA.PlanHash, DryEditPlan.PlanHash);

	FString Serialized;
	FString Error;
	TestTrue(TEXT("Bounded dry-run result serializes"),
		FHyperAIStudioTypedPlanValidator::SerializeDryRunJson(ResultA, Serialized, Error));
	TestTrue(TEXT("Serialized result names the stable schema"), Serialized.Contains(TEXT("hyperai.plan-validation.v1")));
	TestTrue(TEXT("Serialized result contains canonical hash"), Serialized.Contains(PlanA.PlanHash));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanCoordinatorTest,
	"HyperAIStudio.NativeTools.TypedPlan.SerialCoordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanCoordinatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	const FString Steps = BlueprintStep(TEXT("second"), TEXT("first")) + TEXT(",") + BlueprintStep(TEXT("first"));
	FHyperAIStudioValidatedPlan Plan;
	FHyperAIStudioPlanDryRunResult Result;
	const bool bPlanValid = Validate(
		MakePlan(CapabilityHash, Steps, false, TEXT("operation-batch-001")), Registry, Plan, Result);
	TestTrue(TEXT("Two-step mutation plan validates"), bPlanValid);
	if (!bPlanValid || Result.OrderedStepIds.Num() < 2)
	{
		AddError(TEXT("Coordinator fixture did not produce the required ordered steps."));
		return false;
	}
	TestEqual(TEXT("Dependency order moves prerequisite first"), Result.OrderedStepIds[0], TEXT("first"));
	TestEqual(TEXT("Dependent remains second"), Result.OrderedStepIds[1], TEXT("second"));

	int32 CompileCount = 0;
	int32 SaveCount = 0;
	int32 ValidateCount = 0;
	int32 VerifyFreshCount = 0;
	int32 CompileIndex = INDEX_NONE;
	int32 ValidateIndex = INDEX_NONE;
	int32 SaveIndex = INDEX_NONE;
	int32 VerifyFreshIndex = INDEX_NONE;
	for (int32 ScheduleIndex = 0; ScheduleIndex < Result.Schedule.Num(); ++ScheduleIndex)
	{
		const FHyperAIStudioPlanScheduledAction& Action = Result.Schedule[ScheduleIndex];
		CompileCount += Action.Kind == EHyperAIStudioPlanActionKind::CompileOnce ? 1 : 0;
		SaveCount += Action.Kind == EHyperAIStudioPlanActionKind::SaveOnce ? 1 : 0;
		ValidateCount += Action.Kind == EHyperAIStudioPlanActionKind::ValidateOnce ? 1 : 0;
		VerifyFreshCount += Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce ? 1 : 0;
		CompileIndex = Action.Kind == EHyperAIStudioPlanActionKind::CompileOnce ? ScheduleIndex : CompileIndex;
		ValidateIndex = Action.Kind == EHyperAIStudioPlanActionKind::ValidateOnce ? ScheduleIndex : ValidateIndex;
		SaveIndex = Action.Kind == EHyperAIStudioPlanActionKind::SaveOnce ? ScheduleIndex : SaveIndex;
		VerifyFreshIndex = Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce ? ScheduleIndex : VerifyFreshIndex;
	}
	TestEqual(TEXT("Compile is scheduled exactly once across both edits"), CompileCount, 1);
	TestEqual(TEXT("Save is scheduled exactly once across both edits"), SaveCount, 1);
	TestEqual(TEXT("Final validation is scheduled exactly once across both edits"), ValidateCount, 1);
	TestEqual(TEXT("Fresh persisted verification is scheduled exactly once"), VerifyFreshCount, 1);
	TestEqual(TEXT("Two typed steps plus four deduplicated finalizers"), Result.Schedule.Num(), 6);
	TestTrue(TEXT("Finalizers have an exact compile, validate, save, fresh-verify order"),
		CompileIndex >= 0 && CompileIndex < ValidateIndex && ValidateIndex < SaveIndex && SaveIndex < VerifyFreshIndex);
	TestTrue(TEXT("Finalizer envelopes are included in aggregate native-operation accounting"),
		Result.PlannedNativeOperationBudget > 8);

	FFakeJournalGate Journal;
	FFakeClock Clock;
	FHyperAIStudioSerialPlanCoordinator Coordinator;
	FString Error;
	TestTrue(TEXT("Coordinator begins through idempotency gate"),
		Coordinator.Start(Plan, Registry, TestCanonicalProjectId, &Journal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("One begin call"), Journal.BeginCalls, 1);
	TestEqual(TEXT("Journal binds canonical plan hash"), Journal.BoundPlanHash, Plan.PlanHash);
	TestEqual(TEXT("Journal binds capability fingerprint"), Journal.BoundCapabilityHash, CapabilityHash);
	if (Journal.Transitions.IsEmpty())
	{
		AddError(TEXT("Coordinator did not persist its running transition."));
		return false;
	}
	TestEqual(TEXT("New plan enters running before exposing work"), Journal.Transitions[0], EHyperAIStudioPlanJournalTransition::Running);

	FHyperAIStudioPlanScheduledAction Action;
	TestTrue(TEXT("First action is acquired"), Coordinator.AcquireNextAction(Action, Error));
	TestEqual(TEXT("First action respects dependency order"), Action.StepId, TEXT("first"));
	TestTrue(TEXT("Commit is marked before first mutating action is exposed"), Coordinator.HasMutationCommitStarted());
	TestFalse(TEXT("A second action cannot overlap the in-flight action"), Coordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("First action completes"), Complete(Coordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	while (Coordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
	{
		TestTrue(TEXT("Next serial action acquired"), Coordinator.AcquireNextAction(Action, Error));
		TestTrue(TEXT("Serial action completed"), Complete(Coordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	}
	TestEqual(TEXT("Coordinator reaches completed only after all finalizers"), Coordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Completed);
	TestEqual(TEXT("All six actions completed"), Coordinator.GetCompletedActionCount(), 6);
	TestTrue(TEXT("A successful mutation is recorded as a known committed effect"), Coordinator.HasKnownCommittedEffect());
	TestEqual(TEXT("Exactly running, commit-started, completed journal transitions"), Journal.Transitions.Num(), 3);
	if (Journal.Transitions.Num() >= 3)
	{
		TestEqual(TEXT("Second transition is commit start"), Journal.Transitions[1], EHyperAIStudioPlanJournalTransition::CommitStarted);
		TestEqual(TEXT("Last transition is completed"), Journal.Transitions[2], EHyperAIStudioPlanJournalTransition::Completed);
	}

	FFakeJournalGate PersistenceFailureJournal;
	PersistenceFailureJournal.bFailTransitions = true;
	FHyperAIStudioSerialPlanCoordinator PersistenceFailureCoordinator;
	TestFalse(TEXT("A running-transition persistence failure exposes no work"),
		PersistenceFailureCoordinator.Start(
			Plan, Registry, TestCanonicalProjectId, &PersistenceFailureJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Coordinator attempts a terminal precommit closure after the running transition fails"),
		PersistenceFailureJournal.TransitionCalls, 2);
	TestEqual(TEXT("Transition failure is terminal before commit"),
		PersistenceFailureCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Failed);

	FFakeJournalGate ReplayJournal;
	ReplayJournal.BeginResult = EHyperAIStudioPlanJournalBegin::ReplayCompleted;
	FHyperAIStudioSerialPlanCoordinator ReplayCoordinator;
	TestTrue(TEXT("Completed idempotent replay is accepted without work"),
		ReplayCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &ReplayJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Replay state is explicit"), ReplayCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::ReplayCompleted);
	TestTrue(TEXT("Replay schedules no duplicate mutation"), ReplayCoordinator.GetSchedule().IsEmpty());

	FFakeJournalGate ConflictJournal;
	ConflictJournal.BeginResult = EHyperAIStudioPlanJournalBegin::Conflict;
	FHyperAIStudioSerialPlanCoordinator ConflictCoordinator;
	TestFalse(TEXT("Same operation_id with different hash is rejected"),
		ConflictCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &ConflictJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Conflict is a terminal coordinator failure"), ConflictCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Failed);

	FFakeJournalGate ExistingUnknownJournal;
	ExistingUnknownJournal.BeginResult = EHyperAIStudioPlanJournalBegin::OutcomeUnknown;
	FHyperAIStudioSerialPlanCoordinator ExistingUnknownCoordinator;
	TestFalse(TEXT("Existing outcome_unknown never retries"),
		ExistingUnknownCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &ExistingUnknownJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Existing unknown remains fail-closed"), ExistingUnknownCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);

	FFakeJournalGate UnknownJournal;
	FHyperAIStudioSerialPlanCoordinator UnknownCoordinator;
	TestTrue(TEXT("Fresh unknown-outcome fixture starts"),
		UnknownCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &UnknownJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Mutation action acquired before simulated response loss"), UnknownCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Unknown action outcome is recorded"),
		Complete(UnknownCoordinator, EHyperAIStudioPlanActionOutcome::OutcomeUnknown, Error));
	TestEqual(TEXT("Unknown outcome stops the coordinator"), UnknownCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);
	TestFalse(TEXT("No fallback or later action is exposed after unknown outcome"), UnknownCoordinator.AcquireNextAction(Action, Error));
	TestEqual(TEXT("Unknown path records running, commit, outcome_unknown"), UnknownJournal.Transitions.Num(), 3);
	if (!UnknownJournal.Transitions.IsEmpty())
	{
		TestEqual(TEXT("Unknown terminal journal transition"), UnknownJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::OutcomeUnknown);
	}

	FFakeJournalGate PreEffectFailureJournal;
	FHyperAIStudioSerialPlanCoordinator PreEffectFailureCoordinator;
	TestTrue(TEXT("Pre-effect failure fixture starts"),
		PreEffectFailureCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &PreEffectFailureJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Pre-effect failure fixture acquires action"), PreEffectFailureCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Explicit whole-transaction rollback evidence is handled"),
		Complete(PreEffectFailureCoordinator, EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence, Error));
	TestEqual(TEXT("Evidence-backed rollback has its own explicit terminal state"),
		PreEffectFailureCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::RolledBack);
	if (!PreEffectFailureJournal.Transitions.IsEmpty())
	{
		TestEqual(TEXT("Commit marker is closed only by explicit rollback evidence"),
			PreEffectFailureJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::RolledBack);
	}

	FFakeJournalGate SecondStepFailureJournal;
	FHyperAIStudioSerialPlanCoordinator SecondStepFailureCoordinator;
	TestTrue(TEXT("Second-step failure fixture starts"),
		SecondStepFailureCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &SecondStepFailureJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("First mutation is acquired"), SecondStepFailureCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("First mutation succeeds"), Complete(SecondStepFailureCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	TestTrue(TEXT("Second mutation is acquired"), SecondStepFailureCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Second step fails before its own effect"),
		Complete(SecondStepFailureCoordinator, EHyperAIStudioPlanActionOutcome::FailedBeforeEffect, Error));
	TestEqual(TEXT("Earlier committed edit makes later no-effect failure partial"),
		SecondStepFailureCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Later failure records partial instead of false rollback"),
		SecondStepFailureJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::Partial);

	FFakeJournalGate FinalizerFailureJournal;
	FHyperAIStudioSerialPlanCoordinator FinalizerFailureCoordinator;
	TestTrue(TEXT("Finalizer failure fixture starts"),
		FinalizerFailureCoordinator.Start(Plan, Registry, TestCanonicalProjectId, &FinalizerFailureJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("First edit acquired"), FinalizerFailureCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("First edit succeeds"), Complete(FinalizerFailureCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	TestTrue(TEXT("Second edit acquired"), FinalizerFailureCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Second edit succeeds"), Complete(FinalizerFailureCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	TestTrue(TEXT("Compile finalizer acquired"), FinalizerFailureCoordinator.AcquireNextAction(Action, Error));
	TestEqual(TEXT("Fixture fails at compile-once"), Action.Kind, EHyperAIStudioPlanActionKind::CompileOnce);
	TestTrue(TEXT("Finalizer reports failure before its own additional effect"),
		Complete(FinalizerFailureCoordinator, EHyperAIStudioPlanActionOutcome::FailedBeforeEffect, Error));
	TestEqual(TEXT("Finalizer failure after edits is partial"), FinalizerFailureCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Finalizer failure never claims rollback"),
		FinalizerFailureJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::Partial);

	const FString ReadThenMutationSteps = ReadStep(TEXT("read_first")) + TEXT(",")
		+ BlueprintStep(TEXT("mutate_second"), TEXT("read_first"));
	FHyperAIStudioValidatedPlan ReadThenMutationPlan;
	FHyperAIStudioPlanDryRunResult ReadThenMutationResult;
	TestTrue(TEXT("Read-before-mutation fixture validates"), Validate(
		MakePlan(CapabilityHash, ReadThenMutationSteps, false, TEXT("operation-precommit-001")),
		Registry, ReadThenMutationPlan, ReadThenMutationResult));
	FFakeJournalGate PreCommitUnknownJournal;
	FHyperAIStudioSerialPlanCoordinator PreCommitUnknownCoordinator;
	TestTrue(TEXT("Read-before-mutation fixture starts"),
		PreCommitUnknownCoordinator.Start(ReadThenMutationPlan, Registry, TestCanonicalProjectId, &PreCommitUnknownJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Precommit read action acquired"), PreCommitUnknownCoordinator.AcquireNextAction(Action, Error));
	TestFalse(TEXT("Commit marker is not started for a preceding read"), PreCommitUnknownCoordinator.HasMutationCommitStarted());
	TestTrue(TEXT("Precommit ambiguous read is terminally handled"),
		Complete(PreCommitUnknownCoordinator, EHyperAIStudioPlanActionOutcome::OutcomeUnknown, Error));
	TestEqual(TEXT("Precommit ambiguity closes as failed, not outcome_unknown"),
		PreCommitUnknownCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Failed);
	TestEqual(TEXT("Precommit ambiguity leaves no running journal record"),
		PreCommitUnknownJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::FailedPreCommit);

	FHyperAIStudioValidatedPlan ReadPlan;
	FHyperAIStudioPlanDryRunResult ReadResult;
	const FString ExecuteRead = MakePlan(CapabilityHash, ReadStep(), false, FString(), FString(), 4, 0);
	TestTrue(TEXT("Execute-mode read plan validates"), Validate(ExecuteRead, Registry, ReadPlan, ReadResult));
	FHyperAIStudioSerialPlanCoordinator ReadCoordinator;
	TestTrue(TEXT("Read plan needs no mutation journal"),
		ReadCoordinator.Start(ReadPlan, Registry, FString(), nullptr, nullptr, nullptr, &Clock, Error));
	TestTrue(TEXT("Read action acquired"), ReadCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Read action completed"), Complete(ReadCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	TestEqual(TEXT("Read coordinator completes"), ReadCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Completed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanAuthorizationAndSealingTest,
	"HyperAIStudio.NativeTools.TypedPlan.AuthorizationAndSealing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanAuthorizationAndSealingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	const FString Token = TEXT("server-token-delete-authorization-001");
	const FString ProjectId = TEXT("canonical-project-001");
	FHyperAIStudioValidatedPlan Plan;
	FHyperAIStudioPlanDryRunResult Result;
	TestTrue(TEXT("Risky execute fixture validates only as an opaque-token-bearing artifact"),
		Validate(MakePlan(CapabilityHash, DeleteStep(), false, TEXT("operation-auth-001"), Token), Registry, Plan, Result));

	FFakeClock Clock;
	FFakeAuthorizationGate Authorization;
	Authorization.BoundRequest = {
		Token,
		ProjectId,
		Plan.OperationId,
		Plan.AuthorizationPlanHash,
		Plan.MaximumSafety};
	FFakeJournalGate Journal;
	FHyperAIStudioSerialPlanCoordinator Coordinator;
	FString Error;
	TestTrue(TEXT("A correctly bound available server token starts the operation"),
		Coordinator.Start(Plan, Registry, ProjectId, &Journal, &Authorization, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("A new risky operation consumes its grant exactly once"), Authorization.bConsumed);
	TestEqual(TEXT("Authorization gate receives one consume request"), Authorization.ConsumeCalls, 1);

	FFakeJournalGate ReplayJournal;
	ReplayJournal.BeginResult = EHyperAIStudioPlanJournalBegin::ReplayCompleted;
	FHyperAIStudioSerialPlanCoordinator ReplayCoordinator;
	TestTrue(TEXT("Completed replay accepts the already-consumed grant without another mutation"),
		ReplayCoordinator.Start(Plan, Registry, ProjectId, &ReplayJournal, &Authorization, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Replay does not consume the grant a second time"), Authorization.ConsumeCalls, 1);
	TestEqual(TEXT("Replay is explicit"), ReplayCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::ReplayCompleted);

	FFakeJournalGate ReuseJournal;
	FHyperAIStudioSerialPlanCoordinator ReuseCoordinator;
	TestFalse(TEXT("Consumed token cannot authorize a fresh journal operation"),
		ReuseCoordinator.Start(Plan, Registry, ProjectId, &ReuseJournal, &Authorization, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Rejected token reuse is durably closed before commit"),
		ReuseJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::FailedPreCommit);

	FFakeAuthorizationGate WrongProjectAuthorization;
	WrongProjectAuthorization.BoundRequest = Authorization.BoundRequest;
	FFakeJournalGate WrongProjectJournal;
	FHyperAIStudioSerialPlanCoordinator WrongProjectCoordinator;
	TestFalse(TEXT("Authorization is bound to one canonical project identity"),
		WrongProjectCoordinator.Start(Plan, Registry, TEXT("canonical-project-002"),
			&WrongProjectJournal, &WrongProjectAuthorization, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Authorization mismatch is rejected before journal begin"), WrongProjectJournal.BeginCalls, 0);

	FFakeAuthorizationGate ExpiredAuthorization;
	ExpiredAuthorization.BoundRequest = Authorization.BoundRequest;
	ExpiredAuthorization.ExpiresUtcMs = Clock.UtcMs;
	FFakeJournalGate ExpiredJournal;
	FHyperAIStudioSerialPlanCoordinator ExpiredCoordinator;
	TestFalse(TEXT("Expired authorization is rejected before journal begin"),
		ExpiredCoordinator.Start(Plan, Registry, ProjectId, &ExpiredJournal, &ExpiredAuthorization, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Expired token never creates journal work"), ExpiredJournal.BeginCalls, 0);

	FFakeAuthorizationGate NonceSwapAuthorization;
	NonceSwapAuthorization.BoundRequest = Authorization.BoundRequest;
	NonceSwapAuthorization.ConsumeNonceOverride = TEXT("server-nonce-swapped");
	FFakeJournalGate NonceSwapJournal;
	FHyperAIStudioSerialPlanCoordinator NonceSwapCoordinator;
	TestFalse(TEXT("Inspect/consume receipts must retain the same server nonce"),
		NonceSwapCoordinator.Start(
			Plan, Registry, ProjectId, &NonceSwapJournal, &NonceSwapAuthorization, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Nonce mismatch is closed before commit"),
		NonceSwapJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::FailedPreCommit);

	FFakeJournalGate MissingGateJournal;
	FHyperAIStudioSerialPlanCoordinator MissingGateCoordinator;
	TestFalse(TEXT("A bearer token cannot self-authorize without a trusted server gate"),
		MissingGateCoordinator.Start(Plan, Registry, ProjectId, &MissingGateJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Missing trusted authorization is rejected before journal begin"), MissingGateJournal.BeginCalls, 0);

	FHyperAIStudioValidatedPlan EditPlan;
	FHyperAIStudioPlanDryRunResult EditResult;
	TestTrue(TEXT("Sealing fixture validates"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("sealed")), false, TEXT("operation-sealed-001")),
			Registry, EditPlan, EditResult));
	FHyperAIStudioValidatedPlan TamperedPlan = EditPlan;
	TamperedPlan.Steps[0].Arguments.FindChecked(TEXT("asset_path")).StringValue = TEXT("/Game/Tampered");
	FFakeJournalGate TamperJournal;
	FHyperAIStudioSerialPlanCoordinator TamperCoordinator;
	TestFalse(TEXT("Coordinator revalidates and rejects a mutated validated artifact"),
		TamperCoordinator.Start(TamperedPlan, Registry, TestCanonicalProjectId, &TamperJournal, nullptr, &ValidatorGate(), &Clock, Error));
	TestEqual(TEXT("Tampered artifacts are rejected before durable begin"), TamperJournal.BeginCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanRuntimeBudgetTest,
	"HyperAIStudio.NativeTools.TypedPlan.RuntimeBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanRuntimeBudgetTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	FString Error;

	FHyperAIStudioValidatedPlan ReadPlan;
	FHyperAIStudioPlanDryRunResult ReadResult;
	TestTrue(TEXT("Deadline fixture validates"),
		Validate(MakePlan(CapabilityHash, ReadStep(), false, FString(), FString(), 4, 0), Registry, ReadPlan, ReadResult));
	FFakeClock DeadlineClock;
	FHyperAIStudioSerialPlanCoordinator DeadlineCoordinator;
	TestTrue(TEXT("Deadline fixture starts"),
		DeadlineCoordinator.Start(ReadPlan, Registry, FString(), nullptr, nullptr, nullptr, &DeadlineClock, Error));
	DeadlineClock.MonotonicMs += ReadPlan.Budget.DeadlineMs;
	FHyperAIStudioPlanScheduledAction Action;
	TestFalse(TEXT("Deadline is enforced before every dispatch"), DeadlineCoordinator.AcquireNextAction(Action, Error));
	TestEqual(TEXT("Pre-dispatch read deadline fails without exposing work"),
		DeadlineCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Failed);

	FHyperAIStudioValidatedPlan EditPlan;
	FHyperAIStudioPlanDryRunResult EditResult;
	TestTrue(TEXT("Runtime usage fixture validates"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("bounded")), false, TEXT("operation-budget-001")),
			Registry, EditPlan, EditResult));
	FFakeClock MissingReceiptClock;
	FFakeJournalGate MissingReceiptJournal;
	FHyperAIStudioSerialPlanCoordinator MissingReceiptCoordinator;
	TestFalse(TEXT("A mutating plan cannot start without a trusted validator-receipt gate"),
		MissingReceiptCoordinator.Start(EditPlan, Registry, TestCanonicalProjectId,
			&MissingReceiptJournal, nullptr, nullptr, &MissingReceiptClock, Error));
	TestEqual(TEXT("Missing receipt authority is rejected before durable begin"), MissingReceiptJournal.BeginCalls, 0);
	for (const FHyperAIStudioPlanScheduledAction& Scheduled : EditResult.Schedule)
	{
		TestTrue(TEXT("Every step and finalizer carries a positive native-operation budget"),
			Scheduled.Budget.MaxNativeOperations > 0);
		TestTrue(TEXT("Every step and finalizer carries a positive game-thread budget"),
			Scheduled.Budget.MaxGameThreadMs > 0);
		TestTrue(TEXT("Every step and finalizer carries a positive output budget"),
			Scheduled.Budget.MaxOutputBytes > 0);
	}
	FFakeClock UsageClock;
	FFakeJournalGate UsageJournal;
	FHyperAIStudioSerialPlanCoordinator UsageCoordinator;
	TestTrue(TEXT("Runtime usage fixture starts"),
		UsageCoordinator.Start(EditPlan, Registry, TestCanonicalProjectId, &UsageJournal, nullptr, &ValidatorGate(), &UsageClock, Error));
	TestTrue(TEXT("Mutation action is acquired"), UsageCoordinator.AcquireNextAction(Action, Error));
	FHyperAIStudioPlanActionResult OverBudget = ActionResult(EHyperAIStudioPlanActionOutcome::Succeeded);
	OverBudget.Usage.NativeOperations = Action.Budget.MaxNativeOperations + 1;
	TestFalse(TEXT("Actual usage above the per-action envelope fails closed"),
		UsageCoordinator.CompleteCurrentAction(OverBudget, Error));
	TestEqual(TEXT("Over-budget result after an admitted mutation is partial"),
		UsageCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Partial);
	TestEqual(TEXT("Runtime limit closes the durable journal as partial"),
		UsageJournal.Transitions.Last(), EHyperAIStudioPlanJournalTransition::Partial);

	FHyperAIStudioValidatedPlan InFlightPlan;
	FHyperAIStudioPlanDryRunResult InFlightResult;
	TestTrue(TEXT("In-flight deadline fixture validates"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("deadline")), false, TEXT("operation-deadline-001")),
			Registry, InFlightPlan, InFlightResult));
	FFakeClock InFlightClock;
	FFakeJournalGate InFlightJournal;
	FHyperAIStudioSerialPlanCoordinator InFlightCoordinator;
	TestTrue(TEXT("In-flight deadline fixture starts"),
		InFlightCoordinator.Start(InFlightPlan, Registry, TestCanonicalProjectId, &InFlightJournal, nullptr, &ValidatorGate(), &InFlightClock, Error));
	TestTrue(TEXT("In-flight mutation is acquired"), InFlightCoordinator.AcquireNextAction(Action, Error));
	InFlightClock.MonotonicMs += InFlightPlan.Budget.DeadlineMs;
	TestFalse(TEXT("Deadline is rechecked when every action/finalizer returns"),
		Complete(InFlightCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	TestEqual(TEXT("Post-effect deadline overrun is durably partial"),
		InFlightCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Partial);

	FHyperAIStudioValidatedPlan VerifyPlan;
	FHyperAIStudioPlanDryRunResult VerifyResult;
	TestTrue(TEXT("Fresh verification evidence fixture validates"),
		Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("verify")), false, TEXT("operation-verify-001")),
			Registry, VerifyPlan, VerifyResult));
	auto ReachFreshVerifier = [this, &Registry, &VerifyPlan, &Error](
		FHyperAIStudioSerialPlanCoordinator& Coordinator,
		FFakeJournalGate& Journal,
		FFakeValidatorReceiptGate& ReceiptGate,
		FFakeClock& Clock)
	{
		if (!TestTrue(TEXT("Receipt-binding fixture starts"), Coordinator.Start(
			VerifyPlan, Registry, TestCanonicalProjectId, &Journal, nullptr, &ReceiptGate, &Clock, Error)))
		{
			return false;
		}
		FHyperAIStudioPlanScheduledAction Candidate;
		while (Coordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
		{
			if (!TestTrue(TEXT("Receipt-binding fixture acquires action"), Coordinator.AcquireNextAction(Candidate, Error)))
			{
				return false;
			}
			if (Candidate.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce)
			{
				return true;
			}
			if (!TestTrue(TEXT("Receipt-binding pre-verification action completes"),
				Complete(Coordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error)))
			{
				return false;
			}
		}
		return false;
	};

	auto RejectReceiptTamper = [this, &ReachFreshVerifier, &Error](const FString& Label, const auto& Tamper)
	{
		FFakeClock ReceiptClock;
		FFakeJournalGate ReceiptJournal;
		FFakeValidatorReceiptGate ReceiptGate;
		FHyperAIStudioSerialPlanCoordinator ReceiptCoordinator;
		if (!ReachFreshVerifier(ReceiptCoordinator, ReceiptJournal, ReceiptGate, ReceiptClock))
		{
			return;
		}
		FHyperAIStudioPlanActionResult Result = BoundActionResult(
			ReceiptCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded);
		Tamper(Result.Evidence.ValidatorReceipt);
		TestFalse(Label, ReceiptCoordinator.CompleteCurrentAction(Result, Error));
		TestEqual(Label + TEXT(" is rejected before trusted-gate verification"), ReceiptGate.VerifyCalls, 0);
		TestEqual(Label + TEXT(" closes the committed operation as outcome_unknown"),
			ReceiptCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);
	};
	RejectReceiptTamper(TEXT("Receipt project binding tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.CanonicalProjectId = TEXT("canonical-other-project");
	});
	RejectReceiptTamper(TEXT("Receipt operation binding tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.OperationId = TEXT("operation-other-001");
	});
	RejectReceiptTamper(TEXT("Receipt plan binding tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.PlanHash = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	});
	RejectReceiptTamper(TEXT("Receipt capability binding tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.CapabilityHash = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	});
	RejectReceiptTamper(TEXT("Receipt effect binding tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.EffectFingerprint = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	});
	RejectReceiptTamper(TEXT("Receipt action nonce tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.ActionNonce = TEXT("action-different-nonce");
	});
	RejectReceiptTamper(TEXT("Receipt validator id tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.ValidatorId = TEXT("hyperai.verify_rollback");
	});
	RejectReceiptTamper(TEXT("Receipt approved-validator fingerprint tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.ApprovedValidatorFingerprint = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	});
	RejectReceiptTamper(TEXT("Receipt postcondition fingerprint tamper is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.PostconditionHash = TEXT("not-a-sha256");
	});
	RejectReceiptTamper(TEXT("Expired validator receipt is rejected"), [](FHyperAIStudioPlanValidatorReceipt& Receipt)
	{
		Receipt.ExpiresUtcMs = Receipt.IssuedUtcMs;
	});

	FFakeClock UntrustedReceiptClock;
	FFakeJournalGate UntrustedReceiptJournal;
	FFakeValidatorReceiptGate UntrustedReceiptGate;
	UntrustedReceiptGate.bReject = true;
	FHyperAIStudioSerialPlanCoordinator UntrustedReceiptCoordinator;
	if (ReachFreshVerifier(UntrustedReceiptCoordinator, UntrustedReceiptJournal, UntrustedReceiptGate, UntrustedReceiptClock))
	{
		TestFalse(TEXT("Matching client claims cannot self-authorize without a server-accepted receipt"),
			UntrustedReceiptCoordinator.CompleteCurrentAction(
				BoundActionResult(UntrustedReceiptCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded), Error));
		TestEqual(TEXT("Authenticity is checked by the trusted gate exactly once"), UntrustedReceiptGate.VerifyCalls, 1);
	}
	FFakeClock VerifyClock;
	FFakeJournalGate VerifyJournal;
	FHyperAIStudioSerialPlanCoordinator VerifyCoordinator;
	TestTrue(TEXT("Fresh verification evidence fixture starts"),
		VerifyCoordinator.Start(VerifyPlan, Registry, TestCanonicalProjectId, &VerifyJournal, nullptr, &ValidatorGate(), &VerifyClock, Error));
	while (VerifyCoordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
	{
		TestTrue(TEXT("Fresh verification fixture acquires action"), VerifyCoordinator.AcquireNextAction(Action, Error));
		if (Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce)
		{
			break;
		}
		TestTrue(TEXT("Pre-verification action completes"),
			Complete(VerifyCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	}
	TestEqual(TEXT("Last completion gate is fresh verification"), Action.Kind, EHyperAIStudioPlanActionKind::VerifyFreshOnce);
	TestFalse(TEXT("Fresh verification cannot succeed without trusted postcondition evidence"),
		VerifyCoordinator.CompleteCurrentAction(ActionResult(EHyperAIStudioPlanActionOutcome::Succeeded), Error));
	TestEqual(TEXT("Missing postcondition evidence prevents completed journal state"),
		VerifyCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::OutcomeUnknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTypedPlanJournalAdapterTest,
	"HyperAIStudio.NativeTools.TypedPlan.RealJournalAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTypedPlanJournalAdapterTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("HyperAIStudioTypedPlanTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TestTrue(TEXT("Real-journal fixture directory is created"), IFileManager::Get().MakeDirectory(*TestRoot, true));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};

	FHyperAIStudioOperationJournal Journal(TestRoot, 16);
	FString Error;
	if (!TestTrue(TEXT("Real durable journal loads"), Journal.Load(Error)))
	{
		AddError(Error);
		return false;
	}
	FHyperAIStudioOperationJournalPlanAdapter Adapter(Journal);
	FFakeClock Clock;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();

	auto ValidateEdit = [this, &Registry, &CapabilityHash](
		const FString& OperationId,
		FHyperAIStudioValidatedPlan& OutPlan)
	{
		FHyperAIStudioPlanDryRunResult OutResult;
		return TestTrue(TEXT("Real-adapter edit fixture validates"),
			Validate(MakePlan(CapabilityHash, BlueprintStep(TEXT("edit")), false, OperationId),
				Registry, OutPlan, OutResult));
	};

	FHyperAIStudioValidatedPlan CompletionPlan;
	if (!ValidateEdit(TEXT("operation-real-complete-001"), CompletionPlan))
	{
		return false;
	}
	FHyperAIStudioSerialPlanCoordinator CompletionCoordinator;
	TestTrue(TEXT("Real adapter begins completion fixture"),
		CompletionCoordinator.Start(CompletionPlan, Registry, Journal.GetCanonicalProjectId(), &Adapter, nullptr, &ValidatorGate(), &Clock, Error));
	FHyperAIStudioPlanScheduledAction Action;
	while (CompletionCoordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
	{
		TestTrue(TEXT("Real completion fixture acquires its next serial action"),
			CompletionCoordinator.AcquireNextAction(Action, Error));
		TestTrue(TEXT("Real completion fixture supplies required final verification evidence"),
			Complete(CompletionCoordinator, EHyperAIStudioPlanActionOutcome::Succeeded, Error));
	}
	TestEqual(TEXT("Real completion requires and reaches fresh-verified completed state"),
		CompletionCoordinator.GetState(), EHyperAIStudioPlanCoordinatorState::Completed);
	const TOptional<FHyperAIStudioOperationRecord> CompletedRecord = Journal.Find(CompletionPlan.OperationId);
	TestTrue(TEXT("Completed record exists"), CompletedRecord.IsSet());
	if (CompletedRecord.IsSet())
	{
		TestEqual(TEXT("Real journal records completion only after fresh verification"),
			CompletedRecord->State, EHyperAIStudioOperationState::Completed);
		TestEqual(TEXT("Completion atomically persists current bound validator evidence"),
			CompletedRecord->TerminalEvidence.Version, FHyperAIStudioOperationEvidence::CurrentVersion);
		TestEqual(TEXT("Completion evidence is bound to the sealed effect contract"),
			CompletedRecord->TerminalEvidence.EffectFingerprint, CompletionPlan.EffectFingerprint);
		TestEqual(TEXT("Completion evidence is bound to the exact operation"),
			CompletedRecord->TerminalEvidence.OperationId, CompletionPlan.OperationId);
	}

	FHyperAIStudioValidatedPlan RollbackPlan;
	if (!ValidateEdit(TEXT("operation-real-rollback-001"), RollbackPlan))
	{
		return false;
	}
	FHyperAIStudioSerialPlanCoordinator RollbackCoordinator;
	TestTrue(TEXT("Real adapter begins rollback fixture"),
		RollbackCoordinator.Start(RollbackPlan, Registry, Journal.GetCanonicalProjectId(), &Adapter, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Real adapter persists commit barrier before mutation"), RollbackCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Trusted whole-plan rollback evidence closes the real journal"),
		Complete(RollbackCoordinator, EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence, Error));
	const TOptional<FHyperAIStudioOperationRecord> RolledBackRecord = Journal.Find(RollbackPlan.OperationId);
	TestTrue(TEXT("Rollback record exists"), RolledBackRecord.IsSet());
	if (RolledBackRecord.IsSet())
	{
		TestEqual(TEXT("Real journal records rolled_back"), RolledBackRecord->State, EHyperAIStudioOperationState::RolledBack);
		TestEqual(TEXT("Real journal records complete rollback evidence"),
			RolledBackRecord->RollbackState, EHyperAIStudioRollbackState::Complete);
		TestTrue(TEXT("Evidence-backed rollback is retry-safe"), RolledBackRecord->bRetrySafe);
		TestEqual(TEXT("Rollback atomically persists the allowlisted validator identity"),
			RolledBackRecord->TerminalEvidence.ValidatorId,
			FString(FHyperAIStudioTypedPlanValidator::RollbackValidatorId()));
		TestEqual(TEXT("Rollback evidence is bound to the sealed effect contract"),
			RolledBackRecord->TerminalEvidence.EffectFingerprint, RollbackPlan.EffectFingerprint);
	}

	FHyperAIStudioValidatedPlan PartialPlan;
	if (!ValidateEdit(TEXT("operation-real-partial-001"), PartialPlan))
	{
		return false;
	}
	FHyperAIStudioSerialPlanCoordinator PartialCoordinator;
	TestTrue(TEXT("Real adapter begins partial fixture"),
		PartialCoordinator.Start(PartialPlan, Registry, Journal.GetCanonicalProjectId(), &Adapter, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Partial fixture acquires mutation"), PartialCoordinator.AcquireNextAction(Action, Error));
	TestTrue(TEXT("Known-effect failure with exact rollback state closes as partial"),
		Complete(PartialCoordinator, EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect, Error));
	const TOptional<FHyperAIStudioOperationRecord> PartialRecord = Journal.Find(PartialPlan.OperationId);
	TestTrue(TEXT("Partial record exists"), PartialRecord.IsSet());
	if (PartialRecord.IsSet())
	{
		TestEqual(TEXT("Real journal records partial"), PartialRecord->State, EHyperAIStudioOperationState::Partial);
		TestEqual(TEXT("Unknown rollback evidence is not promoted to complete"),
			PartialRecord->RollbackState, EHyperAIStudioRollbackState::Unknown);
		TestFalse(TEXT("Partial operations are not retry-safe"), PartialRecord->bRetrySafe);
	}
	FHyperAIStudioPlanOutcomeEvidence NoEvidence;
	TestFalse(TEXT("Terminal partial journal record rejects an illegal completed transition"),
		Adapter.Transition(PartialPlan.OperationId, EHyperAIStudioPlanJournalTransition::Completed, NoEvidence, Error));

	const FString ReadThenMutationSteps = ReadStep(TEXT("read_first")) + TEXT(",")
		+ BlueprintStep(TEXT("mutate_second"), TEXT("read_first"));
	FHyperAIStudioValidatedPlan ContractPlan;
	FHyperAIStudioPlanDryRunResult ContractResult;
	TestTrue(TEXT("Read-contract fixture validates"),
		Validate(MakePlan(CapabilityHash, ReadThenMutationSteps, false, TEXT("operation-real-contract-001")),
			Registry, ContractPlan, ContractResult));
	FHyperAIStudioSerialPlanCoordinator ContractCoordinator;
	TestTrue(TEXT("Read-contract fixture starts"),
		ContractCoordinator.Start(ContractPlan, Registry, Journal.GetCanonicalProjectId(), &Adapter, nullptr, &ValidatorGate(), &Clock, Error));
	TestTrue(TEXT("Precommit read is acquired"), ContractCoordinator.AcquireNextAction(Action, Error));
	TestFalse(TEXT("Read action claiming an effect is a contract violation"),
		Complete(ContractCoordinator, EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect, Error));
	const TOptional<FHyperAIStudioOperationRecord> UnknownRecord = Journal.Find(ContractPlan.OperationId);
	TestTrue(TEXT("Contract-violation record exists"), UnknownRecord.IsSet());
	if (UnknownRecord.IsSet())
	{
		TestEqual(TEXT("Adapter inserts a commit barrier and records outcome_unknown"),
			UnknownRecord->State, EHyperAIStudioOperationState::OutcomeUnknown);
		TestEqual(TEXT("Unknown outcome carries unknown rollback state"),
			UnknownRecord->RollbackState, EHyperAIStudioRollbackState::Unknown);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintDeleteRouteContractTest,
	"HyperAIStudio.NativeTools.TypedPlan.BlueprintDeleteRouteContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintDeleteRouteContractTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TypedPlan::Tests;
	const FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	const FString CapabilityHash = Registry.ComputeCapabilityHash();
	const FHyperAIStudioTypedOperationMetadata* Edit = Registry.Find(TEXT("blueprint.apply_patch"));
	const FHyperAIStudioTypedOperationMetadata* Delete = Registry.Find(TEXT("blueprint.delete_patch"));
	TestNotNull(TEXT("Blueprint edit metadata is registered"), Edit);
	TestNotNull(TEXT("Blueprint destructive metadata is registered separately"), Delete);
	if (!Edit || !Delete)
	{
		return false;
	}

	TestEqual(TEXT("Blueprint edit safety stays non-destructive"), Edit->Safety, EHyperAIStudioPlanSafety::Edit);
	TestFalse(TEXT("Blueprint edit categorically disallows object_deleted"),
		Edit->AllowedEffects.Contains(EHyperAIStudioPlanEffectKind::ObjectDeleted));
	TestEqual(TEXT("Blueprint edit permits only object_updated"), Edit->AllowedEffects.Num(), 1);
	TestTrue(TEXT("Blueprint edit requires exact revision binding"),
		Edit->RequiredPreconditions.Contains(EHyperAIStudioPlanPreconditionKind::RevisionEquals));
	TestEqual(TEXT("Blueprint delete route is destructive"), Delete->Safety, EHyperAIStudioPlanSafety::Destructive);
	TestTrue(TEXT("Blueprint delete route requires object deletion evidence"),
		Delete->RequiredEffects.Contains(EHyperAIStudioPlanEffectKind::ObjectDeleted));
	TestTrue(TEXT("Blueprint delete route also requires the containing Blueprint update"),
		Delete->RequiredEffects.Contains(EHyperAIStudioPlanEffectKind::ObjectUpdated));
	TestTrue(TEXT("Blueprint delete route requires exact revision binding"),
		Delete->RequiredPreconditions.Contains(EHyperAIStudioPlanPreconditionKind::RevisionEquals));
	TestTrue(TEXT("Blueprint delete route always schedules fresh verification"), Delete->bVerifyFreshOnce);

	FHyperAIStudioValidatedPlan EditPlan;
	FHyperAIStudioPlanDryRunResult EditResult;
	TestTrue(TEXT("Delete-free Blueprint edit validates"), Validate(
		MakePlan(CapabilityHash, BlueprintStep(TEXT("edit_route")), true, TEXT("operation-blueprint-edit")),
		Registry, EditPlan, EditResult));
	FHyperAIStudioValidatedPlan SmuggledDeletePlan;
	FHyperAIStudioPlanDryRunResult SmuggledDeleteResult;
	TestFalse(TEXT("object_deleted cannot be smuggled through blueprint.apply_patch"), Validate(
		MakePlan(CapabilityHash,
			BlueprintStep(TEXT("edit_route")).Replace(TEXT("object_updated"), TEXT("object_deleted")),
			true, TEXT("operation-blueprint-edit-delete")),
		Registry, SmuggledDeletePlan, SmuggledDeleteResult));
	TestEqual(TEXT("Edit-route delete rejection is categorical"), FirstCode(SmuggledDeleteResult), TEXT("effect_not_allowed"));

	const FString DeleteStepJson = BlueprintDeleteStep();
	FHyperAIStudioValidatedPlan DeleteDryPlan;
	FHyperAIStudioPlanDryRunResult DeleteDryResult;
	TestTrue(TEXT("Blueprint delete dry-run validates without consuming a grant"), Validate(
		MakePlan(CapabilityHash, DeleteStepJson, true, TEXT("operation-blueprint-delete-preview")),
		Registry, DeleteDryPlan, DeleteDryResult));
	TestEqual(TEXT("Blueprint delete plan derives destructive safety"),
		DeleteDryPlan.MaximumSafety, EHyperAIStudioPlanSafety::Destructive);
	TestTrue(TEXT("Blueprint delete dry-run reports server authorization requirement"),
		DeleteDryResult.bRequiresDestructiveAuthorization);
	TestTrue(TEXT("Blueprint delete scheduled step retains fresh verification"),
		DeleteDryPlan.Steps.Num() == 1 && DeleteDryPlan.Steps[0].bVerifyFreshOnce);

	FHyperAIStudioValidatedPlan UnauthorizedPlan;
	FHyperAIStudioPlanDryRunResult UnauthorizedResult;
	TestFalse(TEXT("Execute-mode Blueprint delete requires a server-issued grant"), Validate(
		MakePlan(CapabilityHash, DeleteStepJson, false, TEXT("operation-blueprint-delete-execute")),
		Registry, UnauthorizedPlan, UnauthorizedResult));
	TestEqual(TEXT("Missing Blueprint delete grant fails before execution"),
		FirstCode(UnauthorizedResult), TEXT("authorization_required"));

	FHyperAIStudioValidatedPlan AuthorizedPlan;
	FHyperAIStudioPlanDryRunResult AuthorizedResult;
	TestTrue(TEXT("Opaque token-bearing Blueprint delete artifact validates for server gating"), Validate(
		MakePlan(CapabilityHash, DeleteStepJson, false, TEXT("operation-blueprint-delete-execute"),
			TEXT("server-token-blueprint-delete-001")),
		Registry, AuthorizedPlan, AuthorizedResult));
	TestEqual(TEXT("Dry-run and authorized execute bind the same delete plan hash"),
		DeleteDryPlan.PlanHash, AuthorizedPlan.PlanHash);
	TestEqual(TEXT("Bearer envelope does not alter Blueprint delete authorization hash"),
		DeleteDryPlan.AuthorizationPlanHash, AuthorizedPlan.AuthorizationPlanHash);
	TestNotEqual(TEXT("Edit and destructive Blueprint routes have distinct semantic hashes"),
		EditPlan.PlanHash, DeleteDryPlan.PlanHash);
	TestTrue(TEXT("Blueprint delete hash is canonical SHA-256"),
		DeleteDryPlan.PlanHash.StartsWith(TEXT("sha256:")) && DeleteDryPlan.PlanHash.Len() == 71);

	FHyperAIStudioValidatedPlan WrongRevisionTargetPlan;
	FHyperAIStudioPlanDryRunResult WrongRevisionTargetResult;
	TestFalse(TEXT("Blueprint delete revision target cannot differ from asset_path"), Validate(
		MakePlan(CapabilityHash,
			DeleteStepJson.Replace(
				TEXT("\"revision_equals\",\"target\":\"/Game/BP_Test\""),
				TEXT("\"revision_equals\",\"target\":\"/Game/Other\"")),
			true, TEXT("operation-blueprint-delete-wrong-revision")),
		Registry, WrongRevisionTargetPlan, WrongRevisionTargetResult));
	TestEqual(TEXT("Revision binding mismatch fails closed"),
		FirstCode(WrongRevisionTargetResult), TEXT("precondition_target_mismatch"));
	return true;
}

#endif
