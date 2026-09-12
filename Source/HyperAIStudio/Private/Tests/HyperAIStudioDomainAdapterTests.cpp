// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDomainAdapter.h"

#include "Misc/AutomationTest.h"

namespace HyperAIStudio::DomainAdapter::Tests
{
	constexpr const TCHAR* TestRequestSchema = TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");
	constexpr const TCHAR* TestResultSchema = TEXT("sha256:2222222222222222222222222222222222222222222222222222222222222222");

	class FTestRequestPayload final : public IHyperAIStudioDomainRequestPayload
	{
	public:
		FTestRequestPayload(FString InTypeId, const int32 InBytes, FString InSchema = TestRequestSchema)
			: TypeId(MoveTemp(InTypeId)), Schema(MoveTemp(InSchema)), Bytes(InBytes)
		{
		}

		virtual FString GetTypeId() const override { return TypeId; }
		virtual FString GetSchemaFingerprint() const override { return Schema; }
		virtual int32 GetBoundedByteSize() const override { return Bytes; }

	private:
		FString TypeId;
		FString Schema;
		int32 Bytes = 0;
	};

	class FTestResultPayload final : public IHyperAIStudioDomainResultPayload
	{
	public:
		FTestResultPayload(FString InTypeId, const int32 InBytes)
			: TypeId(MoveTemp(InTypeId)), Bytes(InBytes)
		{
		}

		virtual FString GetTypeId() const override { return TypeId; }
		virtual FString GetSchemaFingerprint() const override { return TestResultSchema; }
		virtual int32 GetBoundedByteSize() const override { return Bytes; }
		int32 GetConcreteMarker() const { return 73; }

	private:
		FString TypeId;
		int32 Bytes = 0;
	};

	FHyperAIStudioDomainAdapterDescriptor MakeDescriptor(
		const FString& PackId,
		const FString& ToolName,
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read,
		const uint64 Version = 1,
		const FString& SemanticVersion = TEXT("1.0.0"),
		const FString& AdapterId = FString())
	{
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		Descriptor.PackId = PackId;
		Descriptor.AdapterId = AdapterId.IsEmpty() ? FString(TEXT("adapter.")) + PackId : AdapterId;
		Descriptor.SemanticVersion = SemanticVersion;
		Descriptor.AdapterVersion = Version;
		Descriptor.Variants.Add({
			ToolName,
			TEXT("default"),
			TEXT("hyperai.payload.test.request"),
			TestRequestSchema,
			TEXT("hyperai.result.test.result"),
			TestResultSchema,
			Safety});
		Descriptor.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		Descriptor.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		return Descriptor;
	}

	class FTestAdapter final : public IHyperAIStudioDomainAdapter
	{
	public:
		explicit FTestAdapter(FHyperAIStudioDomainAdapterDescriptor InDescriptor)
			: Descriptor(MoveTemp(InDescriptor))
		{
		}

		virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override
		{
			return Descriptor;
		}

		virtual FHyperAIStudioDomainAdapterResult Execute(
			const FHyperAIStudioDomainDispatchContext& Context,
			const IHyperAIStudioDomainRequestPayload& Payload) override
		{
			++ExecuteCount;
			LastContext = Context;
			LastRequestType = Payload.GetTypeId();
			FHyperAIStudioDomainAdapterResult Result;
			Result.Outcome = NextOutcome;
			Result.StatusCode = TEXT("test_result");
			if (NextOutcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				Result.Payload = MakeShared<FTestResultPayload, ESPMode::ThreadSafe>(
					Descriptor.Variants[0].ResultTypeId, ResultBytes);
			}
			return Result;
		}

		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		EHyperAIStudioDomainDispatchOutcome NextOutcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		int32 ResultBytes = 16;
		int32 ExecuteCount = 0;
		FString LastRequestType;
		FHyperAIStudioDomainDispatchContext LastContext;
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
			if (!bAllow || Request.OpaqueToken != TEXT("server-token-123456"))
			{
				OutError = TEXT("test_authorization_denied");
				return false;
			}
			OutReceipt.BoundRequest = Request;
			OutReceipt.Nonce = TEXT("server-nonce-1");
			OutReceipt.IssuedUtcMs = NowUtcMs;
			OutReceipt.ExpiresUtcMs = NowUtcMs + 60000;
			OutReceipt.bConsumed = bConsumed;
			return true;
		}
		virtual bool Consume(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++CallCount;
			if (bConsumed || !Inspect(Request, NowUtcMs, OutReceipt, OutError))
			{
				return false;
			}
			bConsumed = true;
			OutReceipt.bConsumed = true;
			return true;
		}

		bool bAllow = true;
		bool bConsumed = false;
		int32 InspectCount = 0;
		int32 CallCount = 0;
	};

	class FTestLoader final : public IHyperAIStudioDomainAdapterLoader
	{
	public:
		virtual EHyperAIStudioDomainAdapterLoadResult LoadExact(
			FHyperAIStudioDomainAdapterRegistry& Registry,
			const FHyperAIStudioDomainLoadRequest& Request,
			FString& OutError) override
		{
			++CallCount;
			LastRequest = Request;
			if (BindingToObserve.IsSet())
			{
				ObservedState = Registry.ResolveStatus(BindingToObserve.GetValue()).State;
			}
			if (Result != EHyperAIStudioDomainAdapterLoadResult::Loaded)
			{
				OutError = TEXT("test_loader_failed");
				return Result;
			}
			if (!Adapter.IsValid())
			{
				OutError = TEXT("test_loader_missing_adapter");
				return EHyperAIStudioDomainAdapterLoadResult::Failed;
			}
			FString RegisterError;
			if (!Registry.RegisterAdapter(Adapter.ToSharedRef(), Handle, RegisterError))
			{
				OutError = RegisterError;
				return EHyperAIStudioDomainAdapterLoadResult::Failed;
			}
			return EHyperAIStudioDomainAdapterLoadResult::Loaded;
		}

		EHyperAIStudioDomainAdapterLoadResult Result = EHyperAIStudioDomainAdapterLoadResult::Failed;
		TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
		FHyperAIStudioDomainRegistrationHandle Handle;
		FHyperAIStudioDomainLoadRequest LastRequest;
		TOptional<FHyperAIStudioDomainBinding> BindingToObserve;
		EHyperAIStudioDomainState ObservedState = EHyperAIStudioDomainState::MissingPrerequisite;
		int32 CallCount = 0;
	};

	FHyperAIStudioDomainBinding MakeBinding(
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read)
	{
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = Descriptor.PackId;
		Binding.ToolName = Descriptor.Variants[0].ToolName;
		Binding.VariantId = Descriptor.Variants[0].VariantId;
		Binding.ExpectedSafety = Safety;
		Binding.CanonicalProjectId = TEXT("canonical-project-test");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.Prerequisites.PackId = Descriptor.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations.Add({
			TEXT("probe.test"), EHyperAIStudioDomainPrerequisiteState::Available});
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Descriptor.PackId;
		Binding.Admission.bPackAdmitted = true;
		Binding.Admission.bReadAdmitted = true;
		Binding.Admission.bEditAdmitted = true;
		Binding.Admission.bDestructiveAdmitted = true;
		Binding.Admission.bExternalEffectAdmitted = true;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
		return Binding;
	}

	FHyperAIStudioDomainRequestEnvelope MakeRequest(
		const FHyperAIStudioDomainBinding& Binding,
		const int32 PayloadBytes = 16)
	{
		FHyperAIStudioDomainRequestEnvelope Request;
		Request.Binding = Binding;
		Request.MaxResultBytes = 1024;
		Request.Payload = MakeShared<FTestRequestPayload, ESPMode::ThreadSafe>(
			TEXT("hyperai.payload.test.request"), PayloadBytes);
		return Request;
	}

	bool Register(
		FHyperAIStudioDomainAdapterRegistry& Registry,
		const TSharedRef<FTestAdapter, ESPMode::ThreadSafe>& Adapter,
		FHyperAIStudioDomainRegistrationHandle& OutHandle)
	{
		FString Error;
		return Registry.RegisterAdapter(Adapter, OutHandle, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterTypeNamespaceTest,
	"HyperAIStudio.NativeTools.DomainAdapter.TypeNamespaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterTypeNamespaceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;

	FString Error;
	FHyperAIStudioDomainRegistrationHandle Handle;
	{
		FHyperAIStudioDomainAdapterRegistry Registry;
		FHyperAIStudioDomainAdapterDescriptor Descriptor =
			MakeDescriptor(TEXT("request_namespace"), TEXT("hyper_request_namespace_inspect"));
		Descriptor.Variants[0].RequestTypeId = TEXT("hyperai.result.test.request");
		Descriptor.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		Descriptor.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
			MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MoveTemp(Descriptor));
		TestFalse(TEXT("result namespace cannot be used for a request"),
			Registry.RegisterAdapter(Adapter, Handle, Error));
		TestEqual(TEXT("request namespace mismatch is explicit"), Error,
			FString(TEXT("adapter_variant_invalid")));
	}

	Error.Reset();
	{
		FHyperAIStudioDomainAdapterRegistry Registry;
		FHyperAIStudioDomainAdapterDescriptor Descriptor =
			MakeDescriptor(TEXT("result_namespace"), TEXT("hyper_result_namespace_inspect"));
		Descriptor.Variants[0].ResultTypeId = TEXT("hyperai.payload.test.result");
		Descriptor.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		Descriptor.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
			MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MoveTemp(Descriptor));
		TestFalse(TEXT("payload namespace cannot be used for a result"),
			Registry.RegisterAdapter(Adapter, Handle, Error));
		TestEqual(TEXT("result namespace mismatch is explicit"), Error,
			FString(TEXT("adapter_variant_invalid")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterCollisionTest,
	"HyperAIStudio.NativeTools.DomainAdapter.Collision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterCollisionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> First =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("pack_collision"), TEXT("hyper_collision_inspect")));
	FHyperAIStudioDomainRegistrationHandle FirstHandle;
	TestTrue(TEXT("first adapter registers"), Register(Registry, First, FirstHandle));
	FHyperAIStudioDomainBinding FirstBinding = MakeBinding(First->Descriptor);
	FirstBinding.ExpectedAdapterGeneration = FirstHandle.AdapterGeneration;
	FirstBinding.ExpectedRegistryEpoch = FirstHandle.RegistryEpoch;

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Duplicate =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(
			TEXT("pack_collision"), TEXT("hyper_collision_inspect"), EHyperAIStudioDomainSafety::Read,
			2, TEXT("2.0.0"), TEXT("adapter.pack_collision")));
	FHyperAIStudioDomainRegistrationHandle DuplicateHandle;
	FString Error;
	TestFalse(TEXT("same-pack duplicate is rejected"), Registry.RegisterAdapter(Duplicate, DuplicateHandle, Error));
	TestEqual(TEXT("same identity collision is explicit"), Error, FString(TEXT("adapter_identity_collision")));

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> DisjointSamePack =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("pack_collision"), TEXT("hyper_disjoint_inspect"),
				EHyperAIStudioDomainSafety::Read, 1, TEXT("1.0.0"),
				TEXT("adapter.pack_collision.disjoint")));
	FHyperAIStudioDomainRegistrationHandle DisjointHandle;
	TestTrue(TEXT("disjoint adapter in the same generated pack registers"),
		Registry.RegisterAdapter(DisjointSamePack, DisjointHandle, Error));
	TestEqual(TEXT("disjoint registration epoch churn preserves exact first generation"),
		Registry.ResolveStatus(FirstBinding).State, EHyperAIStudioDomainState::Ready);
	TestEqual(TEXT("disjoint same-pack adapter unregisters independently"),
		Registry.UnregisterAdapter(DisjointHandle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	TestEqual(TEXT("disjoint unregister epoch churn still preserves exact first generation"),
		Registry.ResolveStatus(FirstBinding).State, EHyperAIStudioDomainState::Ready);

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> ToolCollision =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("other_pack"), TEXT("hyper_collision_inspect")));
	Error.Reset();
	TestFalse(TEXT("cross-pack tool collision is rejected"), Registry.RegisterAdapter(ToolCollision, DuplicateHandle, Error));
	TestEqual(TEXT("tool collision is explicit"), Error, FString(TEXT("adapter_tool_collision")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterFingerprintTest,
	"HyperAIStudio.NativeTools.DomainAdapter.Fingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterFingerprintTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDomainAdmissionSnapshot Snapshot;
	Snapshot.PackId = TEXT("pack");
	Snapshot.bPackAdmitted = true;
	Snapshot.bReadAdmitted = true;
	Snapshot.Revision = 7;
	TestEqual(
		TEXT("local Win64-safe SHA-256 matches the canonical UTF-8 vector"),
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Snapshot),
		FString(TEXT("sha256:9a9a80a05be0da93ca241a7b2bf73c629310206b8e1f374536a1d94229f5b94c")));

	FString MalformedPackId;
	MalformedPackId.AppendChar(static_cast<TCHAR>(0xd800));
	Snapshot.PackId = MalformedPackId;
	TestTrue(
		TEXT("malformed UTF-16 is rejected before canonical hashing"),
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Snapshot).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterFastValidationTest,
	"HyperAIStudio.NativeTools.DomainAdapter.FastValidationPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterFastValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> ReadAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("fast_validation"), TEXT("hyper_fast_validation_inspect")));
	const FHyperAIStudioDomainBinding ReadBinding = MakeBinding(ReadAdapter->Descriptor);

	FHyperAIStudioDomainRegistrationHandle FastHandle;
	FHyperAIStudioDomainAdapterRegistry FastRegistry(
		nullptr,
		nullptr,
		TOptional<EHyperAIStudioNativeExecutionMode>(EHyperAIStudioNativeExecutionMode::Fast));
	TestTrue(TEXT("fast validation adapter registers"),
		Register(FastRegistry, ReadAdapter, FastHandle));
	TestEqual(TEXT("First Fast read validates and caches the exact sealed authority"),
		FastRegistry.ResolveStatus(ReadBinding).State, EHyperAIStudioDomainState::Ready);
	TestEqual(TEXT("Repeated Fast read reuses the exact validated authority"),
		FastRegistry.ResolveStatus(ReadBinding).State, EHyperAIStudioDomainState::Ready);
	FHyperAIStudioDomainBinding TamperedRead = ReadBinding;
	TamperedRead.Prerequisites.Observations[0].Id = TEXT("probe.tampered");
	const FHyperAIStudioDomainResolveResult TamperedFast = FastRegistry.ResolveStatus(TamperedRead);
	TestEqual(TEXT("A cache-key collision cannot bypass exact field comparison"),
		TamperedFast.State, EHyperAIStudioDomainState::Disabled);
	TestEqual(TEXT("Fast cache miss retains the complete tamper diagnostic"),
		TamperedFast.DiagnosticCode, FString(TEXT("trusted_snapshot_invalid")));

	FHyperAIStudioDomainRegistrationHandle StrictHandle;
	FHyperAIStudioDomainAdapterRegistry StrictRegistry(
		nullptr,
		nullptr,
		TOptional<EHyperAIStudioNativeExecutionMode>(EHyperAIStudioNativeExecutionMode::StrictSafety));
	TestTrue(TEXT("strict validation adapter registers"),
		Register(StrictRegistry, ReadAdapter, StrictHandle));
	const FHyperAIStudioDomainResolveResult StrictRead = StrictRegistry.ResolveStatus(TamperedRead);
	TestEqual(TEXT("Strict Safety recomputes and rejects the same tamper"),
		StrictRead.State, EHyperAIStudioDomainState::Disabled);
	TestEqual(TEXT("Strict tamper remains explicit"), StrictRead.DiagnosticCode,
		FString(TEXT("trusted_snapshot_invalid")));

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> DestructiveAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(
			TEXT("fast_risky_validation"), TEXT("hyper_fast_risky_apply_plan"),
			EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioDomainRegistrationHandle DestructiveHandle;
	TestTrue(TEXT("destructive validation adapter registers"),
		Register(FastRegistry, DestructiveAdapter, DestructiveHandle));
	FHyperAIStudioDomainBinding DestructiveBinding = MakeBinding(
		DestructiveAdapter->Descriptor, EHyperAIStudioDomainSafety::Destructive);
	DestructiveBinding.Prerequisites.Observations[0].Id = TEXT("probe.tampered");
	TestEqual(TEXT("Fast mode still deeply validates destructive bindings"),
		FastRegistry.ResolveStatus(DestructiveBinding).State, EHyperAIStudioDomainState::Disabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterExactBindingTest,
	"HyperAIStudio.NativeTools.DomainAdapter.ExactBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterExactBindingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("exact_pack"), TEXT("hyper_exact_inspect")));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Register(Registry, Adapter, Handle));

	FHyperAIStudioDomainBinding WrongTool = MakeBinding(Adapter->Descriptor);
	WrongTool.ToolName = TEXT("hyper_spoofed_inspect");
	const FHyperAIStudioDomainDispatchResult WrongToolResult = Registry.DispatchExact(MakeRequest(WrongTool));
	TestEqual(TEXT("spoofed tool is disabled"), WrongToolResult.State, EHyperAIStudioDomainState::Disabled);
	FHyperAIStudioDomainRequestEnvelope WrongSchema = MakeRequest(MakeBinding(Adapter->Descriptor));
	WrongSchema.Payload = MakeShared<FTestRequestPayload, ESPMode::ThreadSafe>(
		TEXT("hyperai.payload.test.request"), 16,
		TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333"));
	TestEqual(TEXT("spoofed schema is disabled"),
		Registry.DispatchExact(WrongSchema).State, EHyperAIStudioDomainState::Disabled);

	FHyperAIStudioDomainBinding WrongPack = MakeBinding(Adapter->Descriptor);
	WrongPack.PackId = TEXT("spoofed_pack");
	WrongPack.Prerequisites.PackId = WrongPack.PackId;
	WrongPack.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(WrongPack.Prerequisites);
	WrongPack.Admission.PackId = WrongPack.PackId;
	WrongPack.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(WrongPack.Admission);
	const FHyperAIStudioDomainDispatchResult WrongPackResult = Registry.DispatchExact(MakeRequest(WrongPack));
	TestEqual(TEXT("fingerprint cannot move to another pack"), WrongPackResult.State, EHyperAIStudioDomainState::Disabled);
	TestEqual(TEXT("spoofed calls never reach adapter"), Adapter->ExecuteCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterDisabledPrerequisiteTest,
	"HyperAIStudio.NativeTools.DomainAdapter.DisabledPrerequisite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterDisabledPrerequisiteTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FTestLoader Loader;
	FHyperAIStudioDomainAdapterRegistry Registry(&Loader);
	const FHyperAIStudioDomainAdapterDescriptor Descriptor = MakeDescriptor(TEXT("disabled_pack"), TEXT("hyper_disabled_inspect"));
	FHyperAIStudioDomainBinding Binding = MakeBinding(Descriptor);
	Binding.Prerequisites.Observations[0].State = EHyperAIStudioDomainPrerequisiteState::Disabled;
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);

	const FHyperAIStudioDomainResolveResult Status = Registry.ResolveStatus(Binding);
	TestEqual(TEXT("disabled prerequisite is explicit"), Status.State, EHyperAIStudioDomainState::Disabled);
	Registry.WarmExact(Binding);
	TestEqual(TEXT("disabled pack never invokes loader"), Loader.CallCount, 0);
	Binding.Prerequisites.Observations[0].State = EHyperAIStudioDomainPrerequisiteState::RestartRequired;
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	TestEqual(TEXT("restart prerequisite is explicit"),
		Registry.ResolveStatus(Binding).State, EHyperAIStudioDomainState::RestartRequired);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterLoadFailureTest,
	"HyperAIStudio.NativeTools.DomainAdapter.GenericLoaderDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterLoadFailureTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FTestLoader Loader;
	Loader.Result = EHyperAIStudioDomainAdapterLoadResult::Loaded;
	FHyperAIStudioDomainAdapterRegistry Registry(&Loader);
	const FHyperAIStudioDomainAdapterDescriptor Descriptor = MakeDescriptor(TEXT("load_pack"), TEXT("hyper_load_inspect"));
	const FHyperAIStudioDomainBinding Binding = MakeBinding(Descriptor);
	Loader.Adapter = MakeShared<FTestAdapter, ESPMode::ThreadSafe>(Descriptor);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Existing =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("load_pack"), TEXT("hyper_existing_inspect")));
	FHyperAIStudioDomainRegistrationHandle ExistingHandle;
	TestTrue(TEXT("pre-existing disjoint adapter registers"), Register(Registry, Existing, ExistingHandle));

	TestEqual(TEXT("missing exact adapter requires explicit registration"),
		Registry.ResolveStatus(Binding).State, EHyperAIStudioDomainState::MissingPrerequisite);
	const FHyperAIStudioDomainResolveResult Warm = Registry.WarmExact(Binding);
	TestEqual(TEXT("warm remains fail-closed"), Warm.State, EHyperAIStudioDomainState::MissingPrerequisite);
	TestEqual(TEXT("generic loader is never invoked"), Loader.CallCount, 0);
	TestEqual(TEXT("wrong or partial loader cannot register or poison state"),
		Registry.ResolveStatus(Binding).State, EHyperAIStudioDomainState::MissingPrerequisite);
	TestEqual(TEXT("pre-existing disjoint registration remains intact"),
		Registry.ResolveStatus(MakeBinding(Existing->Descriptor)).State, EHyperAIStudioDomainState::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterEpochReloadTest,
	"HyperAIStudio.NativeTools.DomainAdapter.EpochReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterEpochReloadTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> V1 =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("epoch_pack"), TEXT("hyper_epoch_inspect")));
	FHyperAIStudioDomainRegistrationHandle V1Handle;
	TestTrue(TEXT("v1 registers"), Register(Registry, V1, V1Handle));

	FHyperAIStudioDomainAdapterDescriptor V2Descriptor = MakeDescriptor(
		TEXT("epoch_pack"), TEXT("hyper_epoch_inspect"), EHyperAIStudioDomainSafety::Read,
		2, TEXT("2.0.0"), V1->Descriptor.AdapterId);
	V2Descriptor.ReplacesAdapterFingerprint = V1->Descriptor.AdapterFingerprint;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> V2 =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(V2Descriptor);
	FHyperAIStudioDomainRegistrationHandle V2Handle;
	TestTrue(TEXT("exact versioned replacement registers"), Register(Registry, V2, V2Handle));
	TestTrue(TEXT("replacement advances generation"), V2Handle.AdapterGeneration > V1Handle.AdapterGeneration);
	TestTrue(TEXT("replacement advances epoch"), V2Handle.RegistryEpoch > V1Handle.RegistryEpoch);

	FHyperAIStudioDomainBinding Stale = MakeBinding(V2->Descriptor);
	Stale.ExpectedAdapterGeneration = V1Handle.AdapterGeneration;
	TestEqual(TEXT("stale generation is rejected"), Registry.ResolveStatus(Stale).State, EHyperAIStudioDomainState::Disabled);
	Stale.ExpectedAdapterGeneration = V2Handle.AdapterGeneration;
	TestEqual(TEXT("exact replacement generation is ready"), Registry.ResolveStatus(Stale).State, EHyperAIStudioDomainState::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterConcurrentLeaseTest,
	"HyperAIStudio.NativeTools.DomainAdapter.ConcurrentLease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterConcurrentLeaseTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("lease_pack"), TEXT("hyper_lease_inspect")));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Register(Registry, Adapter, Handle));
	const FHyperAIStudioDomainBinding Binding = MakeBinding(Adapter->Descriptor);

	FHyperAIStudioDomainAdapterLease First;
	FHyperAIStudioDomainAdapterLease Second;
	TestEqual(TEXT("first lease is ready"), Registry.AcquireReadyLease(Binding, First).State, EHyperAIStudioDomainState::Ready);
	TestTrue(TEXT("first lease is valid"), First.IsValid());
	TestEqual(TEXT("overlapping lease is busy"), Registry.AcquireReadyLease(Binding, Second).State, EHyperAIStudioDomainState::Busy);
	TestFalse(TEXT("second lease is not granted"), Second.IsValid());
	First.Reset();
	TestEqual(TEXT("lease release restores ready"), Registry.ResolveStatus(Binding).State, EHyperAIStudioDomainState::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterMutationAuthorizationTest,
	"HyperAIStudio.NativeTools.DomainAdapter.MutationAuthorization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterMutationAuthorizationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	const TSharedRef<FTestAuthorizationGate, ESPMode::ThreadSafe> AuthorizationGate =
		MakeShared<FTestAuthorizationGate, ESPMode::ThreadSafe>();
	FHyperAIStudioDomainAdapterRegistry Registry(nullptr, AuthorizationGate);
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(
			TEXT("mutation_pack"), TEXT("hyper_mutation_apply_plan"), EHyperAIStudioDomainSafety::Edit));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("mutation adapter registers"), Register(Registry, Adapter, Handle));
	const FHyperAIStudioDomainBinding Binding = MakeBinding(Adapter->Descriptor, EHyperAIStudioDomainSafety::Edit);
	FHyperAIStudioDomainBinding WrongAdmission = Binding;
	WrongAdmission.Admission.bEditAdmitted = false;
	WrongAdmission.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(WrongAdmission.Admission);
	TestEqual(TEXT("edit does not inherit another safety admission"),
		Registry.DispatchExact(MakeRequest(WrongAdmission)).State, EHyperAIStudioDomainState::Disabled);

	FHyperAIStudioDomainRequestEnvelope Request = MakeRequest(Binding);
	const FHyperAIStudioDomainDispatchResult NoIdentity = Registry.DispatchExact(Request);
	TestEqual(TEXT("direct mutation bypass is rejected"),
		NoIdentity.Outcome, EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("direct mutation bypass has a stable status"),
		NoIdentity.StatusCode, FString(TEXT("mutation_requires_typed_executor")));
	TestEqual(TEXT("direct mutation without identity never executes"), Adapter->ExecuteCount, 0);

	Request.OperationId = TEXT("operation-001");
	Request.PlanHash = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Request.AuthorizationToken = TEXT("server-token-123456");
	const FHyperAIStudioDomainDispatchResult Result = Registry.DispatchExact(Request);
	TestEqual(TEXT("even a token cannot bypass the typed journal executor"),
		Result.StatusCode, FString(TEXT("mutation_requires_typed_executor")));
	TestEqual(TEXT("bypass never consumes the authorization gate"), AuthorizationGate->CallCount, 0);
	TestEqual(TEXT("bypass never executes the adapter"), Adapter->ExecuteCount, 0);
	TestEqual(TEXT("rejected bypass leaves the admitted pack ready"),
		Registry.ResolveStatus(Binding).State, EHyperAIStudioDomainState::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterNoFallbackTest,
	"HyperAIStudio.NativeTools.DomainAdapter.NoFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterNoFallbackTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Exact =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("exact_only"), TEXT("hyper_exact_only_inspect")));
	Exact->NextOutcome = EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Other =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("other_only"), TEXT("hyper_other_only_inspect")));
	FHyperAIStudioDomainRegistrationHandle ExactHandle;
	FHyperAIStudioDomainRegistrationHandle OtherHandle;
	TestTrue(TEXT("exact adapter registers"), Register(Registry, Exact, ExactHandle));
	TestTrue(TEXT("other adapter registers"), Register(Registry, Other, OtherHandle));

	const FHyperAIStudioDomainDispatchResult Result = Registry.DispatchExact(MakeRequest(MakeBinding(Exact->Descriptor)));
	TestEqual(TEXT("exact failure is returned"), Result.Outcome, EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect);
	TestFalse(TEXT("runtime never permits fallback"), Result.bFallbackPermitted);
	TestEqual(TEXT("exact adapter executes once"), Exact->ExecuteCount, 1);
	TestEqual(TEXT("other adapter is never tried"), Other->ExecuteCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterBoundedPayloadTest,
	"HyperAIStudio.NativeTools.DomainAdapter.BoundedPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterBoundedPayloadTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("bounds_pack"), TEXT("hyper_bounds_inspect")));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Register(Registry, Adapter, Handle));
	const FHyperAIStudioDomainBinding Binding = MakeBinding(Adapter->Descriptor);

	const FHyperAIStudioDomainDispatchResult OversizedRequest = Registry.DispatchExact(
		MakeRequest(Binding, FHyperAIStudioDomainLimits::MaxRequestBytes + 1));
	TestEqual(TEXT("oversized request rejected before effect"),
		OversizedRequest.Outcome, EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("oversized request never executes"), Adapter->ExecuteCount, 0);

	Adapter->ResultBytes = 2048;
	FHyperAIStudioDomainRequestEnvelope Request = MakeRequest(Binding);
	Request.MaxResultBytes = 1024;
	const FHyperAIStudioDomainDispatchResult OversizedResult = Registry.DispatchExact(Request);
	TestEqual(TEXT("oversized read result is rejected"),
		OversizedResult.Outcome, EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestFalse(TEXT("oversized result is not returned"), OversizedResult.Payload.IsValid());
	TestFalse(TEXT("bounds failure never permits fallback"), OversizedResult.bFallbackPermitted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDomainAdapterUnloadWhileActiveTest,
	"HyperAIStudio.NativeTools.DomainAdapter.UnloadWhileActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDomainAdapterUnloadWhileActiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DomainAdapter::Tests;
	FHyperAIStudioDomainAdapterRegistry Registry;
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(TEXT("unload_pack"), TEXT("hyper_unload_inspect")));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Register(Registry, Adapter, Handle));

	FHyperAIStudioDomainAdapterLease Lease;
	TestEqual(TEXT("active call pins adapter"),
		Registry.AcquireReadyLease(MakeBinding(Adapter->Descriptor), Lease).State,
		EHyperAIStudioDomainState::Ready);
	FString Error;
	TestEqual(TEXT("unload is blocked while active"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Busy);
	Lease.Reset();
	{
		const FHyperAIStudioDomainDispatchResult ReadResult =
			Registry.DispatchExact(MakeRequest(MakeBinding(Adapter->Descriptor)));
		TestTrue(TEXT("successful read returns a bounded payload"), ReadResult.Payload.IsValid());
		if (ReadResult.Payload.IsValid())
		{
			const FTestResultPayload* ConcretePayload =
				static_cast<const FTestResultPayload*>(ReadResult.Payload.Get());
			TestEqual(TEXT("lifetime pin preserves the adapter's concrete result payload identity"),
				ConcretePayload->GetConcreteMarker(), 73);
		}
		Error.Reset();
		TestEqual(TEXT("returned optional-module payload keeps its adapter pinned"),
			Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Busy);
	}
	Error.Reset();
	TestEqual(TEXT("unload succeeds after lease and returned payload release"),
		Registry.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
