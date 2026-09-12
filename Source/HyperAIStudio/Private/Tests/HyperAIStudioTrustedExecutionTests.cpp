// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioTrustedExecutionInternal.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::TrustedExecution::Tests
{
	constexpr const TCHAR* PackId = TEXT("trusted_test_pack");
	constexpr const TCHAR* BaseTool = TEXT("hyper_trusted_base_apply");
	constexpr const TCHAR* LimitedTool = TEXT("hyper_trusted_limited_apply");
	constexpr const TCHAR* BaseCohort = TEXT("cohort.test.trusted.base.v1");
	constexpr const TCHAR* LimitedCohort = TEXT("cohort.test.trusted.limited.v1");
	constexpr const TCHAR* RequestType = TEXT("hyperai.payload.test.trusted_request");
	constexpr const TCHAR* ResultType = TEXT("hyperai.result.test.trusted_result");
	constexpr const TCHAR* RequestSchema = TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");
	constexpr const TCHAR* ResultSchema = TEXT("sha256:2222222222222222222222222222222222222222222222222222222222222222");
	constexpr const TCHAR* Semantic = TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333");
	constexpr const TCHAR* Postcondition = TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444");
	constexpr const TCHAR* EmptySource = TEXT("sha256:4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945");

	class FTestClock final : public IHyperAIStudioTypedArtifactClock
	{
	public:
		FTestClock()
		{
			const FDateTime Now = FDateTime::UtcNow();
			UtcMs = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
		}
		virtual int64 NowMonotonicMs() const override { return MonotonicMs; }
		virtual int64 NowUtcMs() const override { return UtcMs; }
		void AdvanceMonotonic(const int64 Delta) { MonotonicMs += Delta; }
		void AdjustUtc(const int64 Delta) { UtcMs += Delta; }
		int64 MonotonicMs = 1000;
		int64 UtcMs = 0;
	};

	class FTestEnvironment final : public IHyperAIStudioTrustedPrerequisiteEnvironment
	{
	public:
		virtual bool ObservePlugin(
			const FString&, FHyperAIStudioTrustedPluginObservation& Out, FString& OutError) const override
		{
			OutError.Reset();
			Out.bInstalled = bPluginInstalled;
			Out.bEnabled = bPluginEnabled;
			Out.bRestartRequired = bRestartRequired;
			return bCallbacksSucceed;
		}
		virtual bool ObserveModuleLoaded(
			const FString&, bool& bOutLoaded, FString& OutError) const override
		{
			OutError.Reset();
			bOutLoaded = bModuleLoaded;
			return bCallbacksSucceed;
		}
		virtual bool AreSourceCandidateToolsEnabled() const override { return bSourceCandidateToolsEnabled; }

		bool bPluginInstalled = true;
		bool bPluginEnabled = true;
		bool bRestartRequired = false;
		bool bModuleLoaded = true;
		bool bCallbacksSucceed = true;
		bool bSourceCandidateToolsEnabled = false;
	};

	class FTestCatalogAuthority final : public IHyperAIStudioTrustedCatalogAuthority
	{
	public:
		explicit FTestCatalogAuthority(FHyperAIStudioCapabilityCatalog InCatalog)
			: Catalog(MoveTemp(InCatalog)) {}

		virtual bool Snapshot(
			FHyperAIStudioTrustedCatalogAuthoritySnapshot& Out, FString& OutError) const override
		{
			FScopeLock Lock(&Mutex);
			OutError.Reset();
			Out.Catalog = Catalog;
			Out.Generation = Generation;
			return bSucceed;
		}

		void SetAdmission(const EHyperAIStudioCapabilityAdmissionState Admission)
		{
			FScopeLock Lock(&Mutex);
			for (FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
			{
				Pack.AdmissionState = Admission;
			}
			for (FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
			{
				Tool.AdmissionState = Admission;
			}
			++Generation;
			Catalog.GeneratedFingerprint =
				FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(Catalog);
		}

		void SetAllowedSafety(
			const FString& ToolName,
			TArray<EHyperAIStudioCapabilitySafetyClass> AllowedSafetyClasses)
		{
			FScopeLock Lock(&Mutex);
			FHyperAIStudioCapabilityToolDefinition* Tool = Catalog.Tools.FindByPredicate(
				[&ToolName](const FHyperAIStudioCapabilityToolDefinition& Item)
				{
					return Item.Name == ToolName;
				});
			if (!Tool)
			{
				return;
			}
			Tool->AllowedSafetyClasses = MoveTemp(AllowedSafetyClasses);
			Tool->bMayCauseExternalEffects = Tool->AllowedSafetyClasses.Contains(
				EHyperAIStudioCapabilitySafetyClass::ExternalEffect);
			for (FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
			{
				Pack.bContainsExternalEffects = Catalog.Tools.ContainsByPredicate(
					[&Pack](const FHyperAIStudioCapabilityToolDefinition& Item)
					{
						return Item.PackId == Pack.Id && Item.bMayCauseExternalEffects;
					});
			}
			++Generation;
			Catalog.GeneratedFingerprint =
				FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(Catalog);
		}

		mutable FCriticalSection Mutex;
		FHyperAIStudioCapabilityCatalog Catalog;
		uint64 Generation = 1;
		bool bSucceed = true;
	};

	FHyperAIStudioCapabilityToolDefinition MakeTool(
		const FString& Name,
		const FString& Cohort,
		const EHyperAIStudioCapabilityAdmissionState Admission)
	{
		FHyperAIStudioCapabilityToolDefinition Tool;
		Tool.Name = Name;
		Tool.PackId = PackId;
		Tool.AtomicCohortId = Cohort;
		Tool.AllowedSafetyClasses.Add(EHyperAIStudioCapabilitySafetyClass::Edit);
		Tool.AdmissionState = Admission;
		Tool.SourceArtifactFingerprint = EmptySource;
		Tool.SourceLedgerFingerprint = EmptySource;
		Tool.SourceLedgerCoverage = EHyperAIStudioCapabilityLedgerCoverage::ApprovedPlanExtension;
		return Tool;
	}

	FHyperAIStudioCapabilityCatalog MakeCatalog(
		const EHyperAIStudioCapabilityAdmissionState Admission,
		const bool bUseProbeRequirements)
	{
		FHyperAIStudioCapabilityCatalog Catalog;
		Catalog.Schema = FHyperAIStudioCapabilityPackRegistry::CatalogSchema;
		Catalog.ApprovedPlanFingerprint = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Catalog.AdmissionMatrixFingerprint = TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
		Catalog.SourceLedgerFingerprint = TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
		Catalog.SourceArtifactFingerprint = EmptySource;
		Catalog.Prerequisites.Add({TEXT("module.TestModule"), EHyperAIStudioCapabilityPrerequisiteKind::Module});
		if (bUseProbeRequirements)
		{
			Catalog.Prerequisites.Add({TEXT("probe.base"), EHyperAIStudioCapabilityPrerequisiteKind::Probe});
			Catalog.Prerequisites.Add({TEXT("probe.limited"), EHyperAIStudioCapabilityPrerequisiteKind::Probe});
		}

		FHyperAIStudioCapabilityPackDefinition Pack;
		Pack.Id = PackId;
		Pack.Tier = EHyperAIStudioCapabilityPackTier::Optional;
		Pack.AdmissionState = Admission;
		Pack.AtomicCohortIds = {BaseCohort, LimitedCohort};
		Pack.ToolNames = {BaseTool, LimitedTool};
		FHyperAIStudioCapabilityRequirementGroup BaseRequirement;
		BaseRequirement.Id = TEXT("base_runtime");
		BaseRequirement.Mode = EHyperAIStudioCapabilityRequirementMode::AllOf;
		BaseRequirement.bBlocking = true;
		BaseRequirement.PrerequisiteIds.Add(TEXT("module.TestModule"));
		if (bUseProbeRequirements)
		{
			BaseRequirement.PrerequisiteIds.Add(TEXT("probe.base"));
		}
		Pack.Requirements.Add(MoveTemp(BaseRequirement));
		if (bUseProbeRequirements)
		{
			FHyperAIStudioCapabilityRequirementGroup LimitedRequirement;
			LimitedRequirement.Id = TEXT("limited_runtime");
			LimitedRequirement.Mode = EHyperAIStudioCapabilityRequirementMode::AllOf;
			LimitedRequirement.bBlocking = false;
			LimitedRequirement.PrerequisiteIds.Add(TEXT("probe.limited"));
			Pack.Requirements.Add(MoveTemp(LimitedRequirement));
		}
		Catalog.Packs.Add(MoveTemp(Pack));
		Catalog.Tools.Add(MakeTool(BaseTool, BaseCohort, Admission));
		FHyperAIStudioCapabilityToolDefinition Limited = MakeTool(LimitedTool, LimitedCohort, Admission);
		if (bUseProbeRequirements)
		{
			Limited.RequiredNonBlockingRequirementGroupIds.Add(TEXT("limited_runtime"));
		}
		Catalog.Tools.Add(MoveTemp(Limited));
		Catalog.GeneratedFingerprint =
			FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(Catalog);
		return Catalog;
	}

	FHyperAIStudioDomainAdapterDescriptor MakeDescriptor(
		const FString& AdapterId,
		const FString& ToolName,
		const bool bLimited,
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit)
	{
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		Descriptor.PackId = PackId;
		Descriptor.AdapterId = AdapterId;
		Descriptor.SemanticVersion = TEXT("1.0.0");
		Descriptor.AdapterVersion = 1;
		if (bLimited)
		{
			Descriptor.ApplicableNonBlockingRequirementGroupIds.Add(TEXT("limited_runtime"));
		}
		Descriptor.Variants.Add({
			ToolName, TEXT("apply_v1"), RequestType, RequestSchema,
			ResultType, ResultSchema, Safety});
		Descriptor.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		Descriptor.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		return Descriptor;
	}

	class FTestResult final : public IHyperAIStudioDomainResultPayload
	{
	public:
		virtual FString GetTypeId() const override { return ResultType; }
		virtual FString GetSchemaFingerprint() const override { return ResultSchema; }
		virtual int32 GetBoundedByteSize() const override { return 32; }
	};

	class FTwoPartyBarrier final
	{
	public:
		FTwoPartyBarrier()
			: Released(FPlatformProcess::GetSynchEventFromPool(true))
		{
		}
		~FTwoPartyBarrier()
		{
			FPlatformProcess::ReturnSynchEventToPool(Released);
		}
		void Arrive()
		{
			bool bRelease = false;
			{
				FScopeLock Lock(&Mutex);
				bRelease = ++ArrivalCount >= 2;
			}
			if (bRelease)
			{
				Released->Trigger();
			}
			Released->Wait(2000);
		}

	private:
		FCriticalSection Mutex;
		FEvent* Released = nullptr;
		int32 ArrivalCount = 0;
	};

	class FTestAdapter final : public IHyperAIStudioDomainAdapter
	{
	public:
		explicit FTestAdapter(FHyperAIStudioDomainAdapterDescriptor InDescriptor)
			: Descriptor(MoveTemp(InDescriptor)) {}
		virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override
		{
			TSharedPtr<FTwoPartyBarrier, ESPMode::ThreadSafe> Barrier;
			{
				FScopeLock Lock(&DescriptorMutex);
				if (!bDescriptorBarrierUsed && DescriptorBarrier.IsValid())
				{
					bDescriptorBarrierUsed = true;
					Barrier = DescriptorBarrier;
				}
			}
			if (Barrier.IsValid())
			{
				Barrier->Arrive();
			}
			return Descriptor;
		}
		virtual FHyperAIStudioDomainAdapterResult Execute(
			const FHyperAIStudioDomainDispatchContext& Context,
			const IHyperAIStudioDomainRequestPayload&) override
		{
			++ExecuteCount;
			if (Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Apply)
			{
				++ApplyCount;
			}
			if (OnExecute)
			{
				OnExecute(Context, ExecuteCount);
			}
			FHyperAIStudioDomainAdapterResult Result;
			Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
			Result.StatusCode = TEXT("trusted_test_succeeded");
			Result.Payload = MakeShared<FTestResult, ESPMode::ThreadSafe>();
			return Result;
		}
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		int32 ExecuteCount = 0;
		int32 ApplyCount = 0;
		TFunction<void(const FHyperAIStudioDomainDispatchContext&, int32)> OnExecute;
		TSharedPtr<FTwoPartyBarrier, ESPMode::ThreadSafe> DescriptorBarrier;
		mutable FCriticalSection DescriptorMutex;
		mutable bool bDescriptorBarrierUsed = false;
	};

	class FTestPayload final : public IHyperAIStudioTypedArtifactPayload
	{
	public:
		FTestPayload() = default;
		FTestPayload(const bool bInClone, TFunction<void()> InCloneDestroyed)
			: bClone(bInClone), CloneDestroyed(MoveTemp(InCloneDestroyed)) {}
		~FTestPayload()
		{
			if (bClone && CloneDestroyed)
			{
				CloneDestroyed();
			}
		}
		virtual FString GetTypeId() const override { return RequestType; }
		virtual FString GetSchemaFingerprint() const override { return RequestSchema; }
		virtual int32 GetBoundedByteSize() const override { return 64; }
		virtual FString GetSemanticFingerprint() const override { return Semantic; }
		virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override
		{
			if (CloneEntered)
			{
				CloneEntered->Trigger();
			}
			if (CloneRelease)
			{
				CloneRelease->Wait(2000);
			}
			return MakeShared<FTestPayload, ESPMode::ThreadSafe>(true, CloneDestroyed);
		}
		FEvent* CloneEntered = nullptr;
		FEvent* CloneRelease = nullptr;
		TFunction<void()> CloneDestroyed;
		bool bClone = false;
	};

	class FTestFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
	{
	public:
		explicit FTestFreshVerifier(FString InOwner) : Owner(MoveTemp(InOwner)) {}
		~FTestFreshVerifier()
		{
			if (Destroyed)
			{
				Destroyed();
			}
		}
		virtual FString GetOwnerAdapterFingerprint() const override { return Owner; }
		virtual bool ResolveCanonicalEffectTarget(
			const IHyperAIStudioTypedArtifactPayload&, FString& OutTarget, FString& OutError) override
		{
			OutError.Reset();
			OutTarget = TEXT("test:/trusted-target");
			return true;
		}
		virtual bool VerifyFreshExact(
			const IHyperAIStudioTypedArtifactPayload&,
			const IHyperAIStudioDomainResultPayload&,
			FString& OutHash,
			FString& OutError) override
		{
			OutError.Reset();
			OutHash = Postcondition;
			return true;
		}
		FString Owner;
		TFunction<void()> Destroyed;
	};

	FHyperAIStudioTrustedArtifactRequest MakeRequest(
		const FString& ToolName,
		const int64 StageLifetimeMs = 15000,
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit)
	{
		FHyperAIStudioTrustedArtifactRequest Request;
		Request.PackId = PackId;
		Request.ToolName = ToolName;
		Request.VariantId = TEXT("apply_v1");
		Request.Safety = Safety;
		Request.ArtifactSemanticFingerprint = Semantic;
		Request.EffectTarget = TEXT("test:/trusted-target");
		Request.StageLifetimeMs = StageLifetimeMs;
		return Request;
	}

	FString MakeProjectRoot()
	{
		const FString Root = FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("HyperAIStudioTrustedExecutionTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Root, true);
		return Root;
	}

	bool RegisterReadyProbe(
		FHyperAIStudioTrustedExecutionHost& Host,
		const FHyperAIStudioDomainRegistrationHandle& Owner,
		const FString& ProbeId,
		FHyperAIStudioTrustedProbeRegistrationHandle& OutHandle,
		FString& OutError)
	{
		return Host.RegisterLiveProbe(Owner, ProbeId, OutHandle, OutError)
			&& Host.PublishLiveProbeExact(
				OutHandle, {true, TEXT("ready"), FString()}, OutError);
	}

	bool Prepare(
		FHyperAIStudioTrustedExecutionHost& Host,
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		const FString& ToolName,
		FHyperAIStudioTrustedPreparedArtifact& OutPrepared,
		FHyperAIStudioTrustedPrepareReport& OutReport,
		FString& OutError,
		const int64 StageLifetimeMs = 15000,
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit)
	{
		return Host.PrepareDryRun(
			MakeRequest(ToolName, StageLifetimeMs, Safety),
			MakeShared<FTestPayload, ESPMode::ThreadSafe>(),
			MakeShared<FTestFreshVerifier, ESPMode::ThreadSafe>(Descriptor.AdapterFingerprint),
			OutPrepared, OutReport, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedMultiAdapterPrerequisiteTest,
	"HyperAIStudio.NativeTools.TrustedExecution.MultiAdapterPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedMultiAdapterPrerequisiteTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, true));
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString Error;
	TestTrue(TEXT("trusted host starts"), Host.Startup(Error));

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> BaseAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.base"), BaseTool, false));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> LimitedAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.limited"), LimitedTool, true));
	FHyperAIStudioDomainRegistrationHandle BaseHandle;
	FHyperAIStudioDomainRegistrationHandle LimitedHandle;
	Catalog->SetAllowedSafety(BaseTool, {
		EHyperAIStudioCapabilitySafetyClass::Destructive});
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> UnderclassifiedAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.underclassified"), BaseTool, false,
				EHyperAIStudioDomainSafety::Edit));
	FHyperAIStudioDomainRegistrationHandle RejectedHandle;
	TestFalse(TEXT("edit cannot underclassify a generated destructive-only tool"),
		Host.RegisterAdapter(UnderclassifiedAdapter, RejectedHandle, Error));
	Catalog->SetAllowedSafety(BaseTool, {EHyperAIStudioCapabilitySafetyClass::Edit});
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> ExtraSafetyAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.extra-safety"), BaseTool, false,
				EHyperAIStudioDomainSafety::ExternalEffect));
	TestFalse(TEXT("adapter cannot add an ungenerated external-effect safety variant"),
		Host.RegisterAdapter(ExtraSafetyAdapter, RejectedHandle, Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> OmittedLimitedAuthority =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.omitted-limited"), LimitedTool, false));
	TestFalse(TEXT("adapter cannot omit generated nonblocking authority"),
		Host.RegisterAdapter(OmittedLimitedAuthority, RejectedHandle, Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> ExtraLimitedAuthority =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.extra-limited"), BaseTool, true));
	TestFalse(TEXT("adapter cannot add generated nonblocking authority"),
		Host.RegisterAdapter(ExtraLimitedAuthority, RejectedHandle, Error));
	TestTrue(TEXT("base cohort registers"), Host.RegisterAdapter(BaseAdapter, BaseHandle, Error));
	TestTrue(TEXT("second disjoint cohort in one pack registers"),
		Host.RegisterAdapter(LimitedAdapter, LimitedHandle, Error));

	FHyperAIStudioTrustedProbeRegistrationHandle BaseProbe;
	TestTrue(TEXT("base cached probe publishes"),
		RegisterReadyProbe(Host, BaseHandle, TEXT("probe.base"), BaseProbe, Error));
	FHyperAIStudioTrustedPreparedArtifact BasePrepared;
	FHyperAIStudioTrustedPrepareReport Report;
	TestTrue(TEXT("nonblocking limited prerequisite does not block base cohort"),
		Prepare(Host, BaseAdapter->Descriptor, BaseTool, BasePrepared, Report, Error));

	FHyperAIStudioTrustedPreparedArtifact LimitedPrepared;
	TestFalse(TEXT("limited cohort fails closed without its exact cached probe"),
		Prepare(Host, LimitedAdapter->Descriptor, LimitedTool, LimitedPrepared, Report, Error));
	FHyperAIStudioTrustedProbeRegistrationHandle LimitedProbe;
	TestTrue(TEXT("limited cached probe publishes only for its owning cohort"),
		RegisterReadyProbe(Host, LimitedHandle, TEXT("probe.limited"), LimitedProbe, Error));
	TestTrue(TEXT("limited cohort becomes ready after exact publication"),
		Prepare(Host, LimitedAdapter->Descriptor, LimitedTool, LimitedPrepared, Report, Error));

	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Overlap =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.overlap"), BaseTool, false));
	FHyperAIStudioDomainRegistrationHandle OverlapHandle;
	TestFalse(TEXT("overlapping tool ownership is rejected atomically"),
		Host.RegisterAdapter(Overlap, OverlapHandle, Error));

	Clock->AdvanceMonotonic(FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs + 1);
	FHyperAIStudioTrustedPreparedArtifact StalePrepared;
	TestFalse(TEXT("stale cached probes never claim readiness"),
		Prepare(Host, BaseAdapter->Descriptor, BaseTool, StalePrepared, Report, Error));
	TestTrue(TEXT("base probe refresh is value-only"), Host.PublishLiveProbeExact(
		BaseProbe, {true, TEXT("ready"), FString()}, Error));
	TestTrue(TEXT("limited probe refresh is value-only"), Host.PublishLiveProbeExact(
		LimitedProbe, {true, TEXT("ready"), FString()}, Error));
	Environment->bModuleLoaded = false;
	TestFalse(TEXT("missing loaded module fails closed"),
		Prepare(Host, BaseAdapter->Descriptor, BaseTool, StalePrepared, Report, Error));
	Environment->bModuleLoaded = true;

	BasePrepared.Reset();
	LimitedPrepared.Reset();
	TestTrue(TEXT("base probe unregisters"), Host.UnregisterLiveProbe(BaseProbe, Error));
	FHyperAIStudioTrustedProbeRegistrationHandle TransferredBaseProbe;
	TestTrue(TEXT("shared blocking probe can transfer to remaining cohort"),
		RegisterReadyProbe(Host, LimitedHandle, TEXT("probe.base"), TransferredBaseProbe, Error));
	TestEqual(TEXT("unregistering one cohort leaves the other intact"),
		Host.UnregisterAdapter(BaseHandle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	FHyperAIStudioTrustedPreparedArtifact RemainingPrepared;
	TestTrue(TEXT("remaining same-pack cohort still resolves"),
		Prepare(Host, LimitedAdapter->Descriptor, LimitedTool, RemainingPrepared, Report, Error));
	RemainingPrepared.Reset();
	Host.UnregisterLiveProbe(TransferredBaseProbe, Error);
	Host.UnregisterLiveProbe(LimitedProbe, Error);
	TestEqual(TEXT("remaining cohort unregisters independently"),
		Host.UnregisterAdapter(LimitedHandle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedAdapterRegistrationRaceTest,
	"HyperAIStudio.NativeTools.TrustedExecution.AdapterRegistrationRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedAdapterRegistrationRaceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, false));
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString StartupError;
	TestTrue(TEXT("race host starts"), Host.Startup(StartupError));

	auto RunRace = [this, &Host](
		const TSharedRef<FTestAdapter, ESPMode::ThreadSafe>& A,
		const TSharedRef<FTestAdapter, ESPMode::ThreadSafe>& B,
		const TCHAR* Label)
	{
		const TSharedRef<FTwoPartyBarrier, ESPMode::ThreadSafe> Barrier =
			MakeShared<FTwoPartyBarrier, ESPMode::ThreadSafe>();
		A->DescriptorBarrier = Barrier;
		B->DescriptorBarrier = Barrier;
		FEvent* Start = FPlatformProcess::GetSynchEventFromPool(true);
		FEvent* DoneA = FPlatformProcess::GetSynchEventFromPool(true);
		FEvent* DoneB = FPlatformProcess::GetSynchEventFromPool(true);
		FHyperAIStudioDomainRegistrationHandle HandleA;
		FHyperAIStudioDomainRegistrationHandle HandleB;
		FString ErrorA;
		FString ErrorB;
		bool bA = false;
		bool bB = false;
		Async(EAsyncExecution::ThreadPool, [&]()
		{
			Start->Wait(2000);
			bA = Host.RegisterAdapter(A, HandleA, ErrorA);
			DoneA->Trigger();
		});
		Async(EAsyncExecution::ThreadPool, [&]()
		{
			Start->Wait(2000);
			bB = Host.RegisterAdapter(B, HandleB, ErrorB);
			DoneB->Trigger();
		});
		Start->Trigger();
		const bool bCompletedA = DoneA->Wait(5000);
		const bool bCompletedB = DoneB->Wait(5000);
		TestTrue(FString::Printf(TEXT("%s concurrent callback A completes"), Label), bCompletedA);
		TestTrue(FString::Printf(TEXT("%s concurrent callback B completes"), Label), bCompletedB);
		TestEqual(FString::Printf(TEXT("%s commits exactly one winner"), Label),
			static_cast<int32>(bA) + static_cast<int32>(bB), 1);
		if (bA)
		{
			FString Ignore;
			Host.UnregisterAdapter(HandleA, Ignore);
		}
		if (bB)
		{
			FString Ignore;
			Host.UnregisterAdapter(HandleB, Ignore);
		}
		FPlatformProcess::ReturnSynchEventToPool(Start);
		FPlatformProcess::ReturnSynchEventToPool(DoneA);
		FPlatformProcess::ReturnSynchEventToPool(DoneB);
	};

	RunRace(
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.same-id"), BaseTool, false)),
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.same-id"), LimitedTool, false)),
		TEXT("same adapter id/different fingerprint"));
	RunRace(
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.overlap-a"), BaseTool, false)),
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.overlap-b"), BaseTool, false)),
		TEXT("different adapter ids/overlapping tool"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedAdmissionProjectDriftTest,
	"HyperAIStudio.NativeTools.TrustedExecution.AdmissionProjectDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedAdmissionProjectDriftTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString RootA = MakeProjectRoot();
	const FString RootB = MakeProjectRoot();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*RootA, false, true);
		IFileManager::Get().DeleteDirectory(*RootB, false, true);
	};
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	Environment->bSourceCandidateToolsEnabled = true;
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::SourceCandidate, true));
	FHyperAIStudioTrustedExecutionHost HostA(RootA, Catalog, Environment, Clock);
	FHyperAIStudioTrustedExecutionHost HostB(RootB, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { HostA.Shutdown(); HostB.Shutdown(); };
	FString Error;
	TestTrue(TEXT("first host starts"), HostA.Startup(Error));
	TestTrue(TEXT("second project host starts"), HostB.Startup(Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.source"), BaseTool, false));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("source candidate registers when its activation channel is enabled"),
		HostA.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioTrustedProbeRegistrationHandle Probe;
	TestTrue(TEXT("source candidate publishes cached prerequisite"),
		RegisterReadyProbe(HostA, Handle, TEXT("probe.base"), Probe, Error));

	Environment->bSourceCandidateToolsEnabled = false;
	FHyperAIStudioTrustedPreparedArtifact DryRun;
	FHyperAIStudioTrustedPrepareReport Report;
	TestTrue(TEXT("registered source candidate remains available for pure dry-run"),
		Prepare(HostA, Adapter->Descriptor, BaseTool, DryRun, Report, Error));
	TestFalse(TEXT("normal source candidate cannot stage"), Report.bMutationAdmitted);
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	TestFalse(TEXT("Stable-only source candidate execution is denied"),
		HostA.StageExact(DryRun, TEXT("trusted-source-denied"), Receipt, Status, Error));

	Environment->bSourceCandidateToolsEnabled = true;
	FHyperAIStudioTrustedPreparedArtifact Evidence;
	TestTrue(TEXT("Preview source-candidate preparation succeeds"),
		Prepare(HostA, Adapter->Descriptor, BaseTool, Evidence, Report, Error));
	TestTrue(TEXT("legacy report bit marks the exact Preview route"), Report.bPreAdmissionEvidenceExecution);
	TestFalse(TEXT("opaque preparation cannot cross canonical projects"),
		HostB.StageExact(Evidence, TEXT("trusted-cross-project"), Receipt, Status, Error));
	TestTrue(TEXT("evidence route stages without mutation"),
		HostA.StageExact(Evidence, TEXT("trusted-evidence-stage"), Receipt, Status, Error));
	const FHyperAIStudioTypedArtifactStageReceipt FirstReceipt = Receipt;
	TestTrue(TEXT("response-loss retry recovers exact stage receipt"),
		HostA.StageExact(Evidence, TEXT("trusted-evidence-stage"), Receipt, Status, Error));
	TestEqual(TEXT("response-loss receipt keeps one stage id"), Receipt.StageId, FirstReceipt.StageId);
	TestEqual(TEXT("stage alone never dispatches adapter code"), Adapter->ExecuteCount, 0);
	FHyperAIStudioTypedArtifactSubmissionReceipt EvidenceSubmission;
	Environment->bSourceCandidateToolsEnabled = false;
	TestFalse(TEXT("source-candidate submission rechecks the activation channel"),
		HostA.SubmitExact(FirstReceipt, FString(), EvidenceSubmission, Status, Error));
	TestEqual(TEXT("disabled Preview route remains zero-effect"), Adapter->ExecuteCount, 0);
	Environment->bSourceCandidateToolsEnabled = true;
	TestTrue(TEXT("source-candidate edit evidence submits without a client bearer"),
		HostA.SubmitExact(FirstReceipt, FString(), EvidenceSubmission, Status, Error));
	TestEqual(TEXT("evidence submission returns before adapter mutation"), Adapter->ExecuteCount, 0);
	for (int32 Tick = 0; Tick < 12; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.0f);
	}
	TestEqual(TEXT("core-sealed evidence dispatches the exact edit once"), Adapter->ApplyCount, 1);

	FHyperAIStudioTrustedPreparedArtifact Drifted;
	TestTrue(TEXT("second evidence preparation seals old catalog"),
		Prepare(HostA, Adapter->Descriptor, BaseTool, Drifted, Report, Error));
	Catalog->SetAdmission(EHyperAIStudioCapabilityAdmissionState::Admitted);
	TestFalse(TEXT("catalog/admission generation drift is rejected before effect"),
		HostA.StageExact(Drifted, TEXT("trusted-catalog-drift"), Receipt, Status, Error));
	TestEqual(TEXT("drift rejection adds no mutation"), Adapter->ApplyCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedEffectPhaseRecheckTest,
	"HyperAIStudio.NativeTools.TrustedExecution.EffectPhaseRecheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedEffectPhaseRecheckTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, false));
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString Error;
	TestTrue(TEXT("phase-recheck host starts"), Host.Startup(Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.phase-drift"), BaseTool, false));
	Adapter->OnExecute = [Catalog](const FHyperAIStudioDomainDispatchContext&, const int32 Count)
	{
		if (Count == 1)
		{
			Catalog->SetAdmission(EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
		}
	};
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("phase-recheck adapter registers"), Host.RegisterAdapter(Adapter, Handle, Error));
	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Report;
	TestTrue(TEXT("phase-recheck artifact prepares"),
		Prepare(Host, Adapter->Descriptor, BaseTool, Prepared, Report, Error));
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic Diagnostic;
	TestTrue(TEXT("phase-recheck artifact stages"), Host.StageExact(
		Prepared, TEXT("trusted-phase-drift"), Receipt, Diagnostic, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	TestTrue(TEXT("phase-recheck artifact is durably admitted"), Host.SubmitExact(
		Receipt, FString(), Submission, Diagnostic, Error));
	TestEqual(TEXT("admission response precedes first effect"), Adapter->ExecuteCount, 0);
	for (int32 Tick = 0; Tick < 12; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.0f);
	}
	TestEqual(TEXT("catalog drift blocks every later effectful phase"), Adapter->ExecuteCount, 1);
	FHyperAIStudioTypedArtifactOperationStatus Status;
	TestTrue(TEXT("phase drift remains queryable"),
		Host.QueryStatus(Receipt.OperationId, Status, Error));
	TestTrue(TEXT("phase drift reaches a durable terminal"), Status.bDurable && Status.bTerminal);
	TestFalse(TEXT("phase drift never authorizes fallback"), Status.bFallbackPermitted);
	Prepared.Reset();
	TestEqual(TEXT("phase-drift terminal releases its adapter pin"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedLifetimeMonotonicTest,
	"HyperAIStudio.NativeTools.TrustedExecution.LifetimeMonotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedLifetimeMonotonicTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, false));
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString Error;
	TestTrue(TEXT("host starts"), Host.Startup(Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.lifetime"), BaseTool, false));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Host.RegisterAdapter(Adapter, Handle, Error));

	EHyperAIStudioDomainUnregisterResult FreshDestructorResult = EHyperAIStudioDomainUnregisterResult::NotFound;
	EHyperAIStudioDomainUnregisterResult PayloadDestructorResult = EHyperAIStudioDomainUnregisterResult::NotFound;
	TSharedPtr<FTestPayload, ESPMode::ThreadSafe> TrackedPayload =
		MakeShared<FTestPayload, ESPMode::ThreadSafe>();
	TrackedPayload->CloneDestroyed = [&]()
	{
		FString LocalError;
		PayloadDestructorResult = Host.UnregisterAdapter(Handle, LocalError);
	};
	TSharedPtr<FTestFreshVerifier, ESPMode::ThreadSafe> TrackedFresh =
		MakeShared<FTestFreshVerifier, ESPMode::ThreadSafe>(Adapter->Descriptor.AdapterFingerprint);
	TrackedFresh->Destroyed = [&]()
	{
		FString LocalError;
		FreshDestructorResult = Host.UnregisterAdapter(Handle, LocalError);
	};
	FHyperAIStudioTrustedPreparedArtifact TrackedPreparation;
	FHyperAIStudioTrustedPrepareReport Report;
	TestTrue(TEXT("tracked preparation succeeds"), Host.PrepareDryRun(
		MakeRequest(BaseTool), TrackedPayload.ToSharedRef(), TrackedFresh.ToSharedRef(),
		TrackedPreparation, Report, Error));
	TrackedPayload.Reset();
	TrackedFresh.Reset();
	TestEqual(TEXT("prepared artifact vetoes adapter unload"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Busy);
	TrackedPreparation.Reset();
	TestEqual(TEXT("fresh verifier is destroyed while module pin remains held"),
		FreshDestructorResult, EHyperAIStudioDomainUnregisterResult::Busy);
	TestEqual(TEXT("immutable payload is destroyed while module pin remains held"),
		PayloadDestructorResult, EHyperAIStudioDomainUnregisterResult::Busy);
	TestEqual(TEXT("adapter unload succeeds after ordered cleanup"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);

	TestTrue(TEXT("adapter re-registers"), Host.RegisterAdapter(Adapter, Handle, Error));
	FEvent* CloneEntered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* CloneRelease = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* UnregisterDone = FPlatformProcess::GetSynchEventFromPool(true);
	ON_SCOPE_EXIT
	{
		FPlatformProcess::ReturnSynchEventToPool(CloneEntered);
		FPlatformProcess::ReturnSynchEventToPool(CloneRelease);
		FPlatformProcess::ReturnSynchEventToPool(UnregisterDone);
	};
	const TSharedRef<FTestPayload, ESPMode::ThreadSafe> BlockingPayload =
		MakeShared<FTestPayload, ESPMode::ThreadSafe>();
	BlockingPayload->CloneEntered = CloneEntered;
	BlockingPayload->CloneRelease = CloneRelease;
	EHyperAIStudioDomainUnregisterResult ConcurrentResult = EHyperAIStudioDomainUnregisterResult::NotFound;
	Async(EAsyncExecution::ThreadPool, [&]()
	{
		CloneEntered->Wait(2000);
		FString LocalError;
		ConcurrentResult = Host.UnregisterAdapter(Handle, LocalError);
		CloneRelease->Trigger();
		UnregisterDone->Trigger();
	});
	FHyperAIStudioTrustedPreparedArtifact MonotonicPreparation;
	TestTrue(TEXT("blocking clone completes under an exact adapter pin"), Host.PrepareDryRun(
		MakeRequest(BaseTool, 100), BlockingPayload,
		MakeShared<FTestFreshVerifier, ESPMode::ThreadSafe>(Adapter->Descriptor.AdapterFingerprint),
		MonotonicPreparation, Report, Error));
	UnregisterDone->Wait(2000);
	TestEqual(TEXT("concurrent unregister is vetoed throughout payload callback"),
		ConcurrentResult, EHyperAIStudioDomainUnregisterResult::Busy);

	Clock->AdjustUtc(24 * 60 * 60 * 1000LL);
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	TestTrue(TEXT("wall-clock forward jump does not expire monotonic preparation"),
		Host.StageExact(MonotonicPreparation, TEXT("trusted-monotonic-stage"), Receipt, Status, Error));
	MonotonicPreparation.Reset();
	Clock->AdjustUtc(-48 * 60 * 60 * 1000LL);
	Clock->AdvanceMonotonic(101);
	TestEqual(TEXT("wall-clock rollback cannot retain expired stage/module pins"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedProductionGrantTest,
	"HyperAIStudio.NativeTools.TrustedExecution.ProductionCoreUiGrant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedProductionGrantTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, false));
	Catalog->SetAllowedSafety(BaseTool, {EHyperAIStudioCapabilitySafetyClass::Destructive});
	// No test authorization injection: this is the same core-owned gate shape used by StartupCore.
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString Error;
	TestTrue(TEXT("production-shape host starts"), Host.Startup(Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(
			TEXT("adapter.test.risky"), BaseTool, false,
			EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("risky admitted adapter registers"), Host.RegisterAdapter(Adapter, Handle, Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> EditAdapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(MakeDescriptor(
			TEXT("adapter.test.non-risky"), LimitedTool, false));
	FHyperAIStudioDomainRegistrationHandle EditHandle;
	TestTrue(TEXT("non-risky cohort registers independently"),
		Host.RegisterAdapter(EditAdapter, EditHandle, Error));
	FHyperAIStudioTrustedPreparedArtifact EditPrepared;
	FHyperAIStudioTrustedPrepareReport Report;
	TestTrue(TEXT("edit artifact prepares"),
		Prepare(Host, EditAdapter->Descriptor, LimitedTool, EditPrepared, Report, Error));
	FHyperAIStudioTypedArtifactStageReceipt EditReceipt;
	FHyperAIStudioTrustedExecutionDiagnostic Diagnostic;
	TestTrue(TEXT("edit artifact stages tokenlessly"), Host.StageExact(
		EditPrepared, TEXT("trusted-edit-no-grant"), EditReceipt, Diagnostic, Error));
	FString NonRiskyGrant;
	TestFalse(TEXT("core issuer rejects a non-risky edit stage"),
		Host.IssueAuthorizationFromCoreUiApproval(EditReceipt, NonRiskyGrant, Error));
	TestTrue(TEXT("rejected edit grant returns no bearer material"), NonRiskyGrant.IsEmpty());
	EditPrepared.Reset();

	FHyperAIStudioTrustedPreparedArtifact Ungranted;
	TestTrue(TEXT("risky artifact prepares"), Prepare(
		Host, Adapter->Descriptor, BaseTool, Ungranted, Report, Error, 15000,
		EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioTypedArtifactStageReceipt UngrantedReceipt;
	TestTrue(TEXT("risky artifact stages without effect"), Host.StageExact(
		Ungranted, TEXT("trusted-risky-no-grant"), UngrantedReceipt, Diagnostic, Error));
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	TestFalse(TEXT("risky mutation defaults deny without a server grant"), Host.SubmitExact(
		UngrantedReceipt, FString(), Submission, Diagnostic, Error));
	TestEqual(TEXT("default deny happens before adapter effect"), Adapter->ExecuteCount, 0);
	Ungranted.Reset();

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	TestTrue(TEXT("second risky artifact prepares"), Prepare(
		Host, Adapter->Descriptor, BaseTool, Prepared, Report, Error, 15000,
		EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TestTrue(TEXT("second risky artifact stages"), Host.StageExact(
		Prepared, TEXT("trusted-risky-approved"), Receipt, Diagnostic, Error));
	FString Grant;
	TestTrue(TEXT("private core-UI boundary issues an exact grant"),
		Host.IssueAuthorizationFromCoreUiApproval(Receipt, Grant, Error));
	TestTrue(TEXT("server grant is opaque and bounded"),
		Grant.Len() >= 16 && Grant.Len() <= FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars);
	FString RecoveredGrant;
	TestTrue(TEXT("lost grant response recovers the exact token"),
		Host.IssueAuthorizationFromCoreUiApproval(Receipt, RecoveredGrant, Error));
	TestEqual(TEXT("grant issuance is idempotent per exact stage"), RecoveredGrant, Grant);
	FHyperAIStudioTrustedPreparedArtifact OtherPrepared;
	TestTrue(TEXT("same variant can prepare a second exact operation"), Prepare(
		Host, Adapter->Descriptor, BaseTool, OtherPrepared, Report, Error, 15000,
		EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioTypedArtifactStageReceipt OtherReceipt;
	TestTrue(TEXT("same variant can stage a second exact operation"), Host.StageExact(
		OtherPrepared, TEXT("trusted-risky-other-stage"), OtherReceipt, Diagnostic, Error));
	TestFalse(TEXT("grant from stage A cannot authorize stage B"), Host.SubmitExact(
		OtherReceipt, Grant, Submission, Diagnostic, Error));
	TestEqual(TEXT("cross-stage grant rejection is before effect"), Adapter->ExecuteCount, 0);
	OtherPrepared.Reset();
	TestTrue(TEXT("exact grant admits the risky mutation"), Host.SubmitExact(
		Receipt, Grant, Submission, Diagnostic, Error));
	TestEqual(TEXT("submission receipt returns before mutation"), Adapter->ExecuteCount, 0);
	FHyperAIStudioDomainAuthorizationReceipt ConsumedGrant;
	TestTrue(TEXT("accepted risky submission leaves exact replay evidence inspectable"),
		Host.InspectCoreAuthorizationGrantForTests(
			Receipt, Grant, ConsumedGrant, Error));
	TestTrue(TEXT("server grant is consumed exactly after durable admission"),
		ConsumedGrant.bConsumed);
	TestFalse(TEXT("bogus credential cannot replay an active risky operation"), Host.SubmitExact(
		Receipt, TEXT("trusted-auth-bogus-bounded"), Submission, Diagnostic, Error));
	TestTrue(TEXT("exact consumed credential replays without a second consume"), Host.SubmitExact(
		Receipt, Grant, Submission, Diagnostic, Error));
	for (int32 Tick = 0; Tick < 12; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.0f);
	}
	TestTrue(TEXT("approved risky operation eventually dispatches"), Adapter->ExecuteCount >= 1);

	FHyperAIStudioTrustedPreparedArtifact ExpiringPrepared;
	TestTrue(TEXT("expiry fixture prepares"), Prepare(
		Host, Adapter->Descriptor, BaseTool, ExpiringPrepared, Report, Error, 15000,
		EHyperAIStudioDomainSafety::Destructive));
	FHyperAIStudioTypedArtifactStageReceipt ExpiringReceipt;
	TestTrue(TEXT("expiry fixture stages"), Host.StageExact(
		ExpiringPrepared, TEXT("trusted-risky-expiring"), ExpiringReceipt, Diagnostic, Error));
	FString ExpiringGrant;
	TestTrue(TEXT("expiry fixture receives a trusted grant"),
		Host.IssueAuthorizationFromCoreUiApproval(ExpiringReceipt, ExpiringGrant, Error));
	Clock->AdjustUtc(20000);
	Clock->AdvanceMonotonic(
		FHyperAIStudioTrustedExecutionLimits::AuthorizationGrantLifetimeMs + 1);
	Clock->AdjustUtc(-10000);
	FHyperAIStudioDomainAuthorizationReceipt Inspected;
	TestFalse(TEXT("monotonic expiry rejects after UTC forward-and-rollback inside UTC lifetime"),
		Host.InspectCoreAuthorizationGrantForTests(
			ExpiringReceipt, ExpiringGrant, Inspected, Error));
	TestFalse(TEXT("expired grant never reports consumed"), Inspected.bConsumed);
	ExpiringPrepared.Reset();
	Prepared.Reset();
	TestEqual(TEXT("terminal risky context auto-reconciles for unload"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	TestEqual(TEXT("non-risky cohort unloads after expired stage cleanup"),
		Host.UnregisterAdapter(EditHandle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTrustedNoPumpTerminalReconcileTest,
	"HyperAIStudio.NativeTools.TrustedExecution.NoPumpTerminalReconcile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTrustedNoPumpTerminalReconcileTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TrustedExecution::Tests;
	const FString Root = MakeProjectRoot();
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const TSharedRef<FTestClock, ESPMode::ThreadSafe> Clock = MakeShared<FTestClock, ESPMode::ThreadSafe>();
	const TSharedRef<FTestEnvironment, ESPMode::ThreadSafe> Environment =
		MakeShared<FTestEnvironment, ESPMode::ThreadSafe>();
	const TSharedRef<FTestCatalogAuthority, ESPMode::ThreadSafe> Catalog =
		MakeShared<FTestCatalogAuthority, ESPMode::ThreadSafe>(
			MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Admitted, false));
	FHyperAIStudioTrustedExecutionHost Host(Root, Catalog, Environment, Clock);
	ON_SCOPE_EXIT { Host.Shutdown(); };
	FString Error;
	TestTrue(TEXT("host starts"), Host.Startup(Error));
	const TSharedRef<FTestAdapter, ESPMode::ThreadSafe> Adapter =
		MakeShared<FTestAdapter, ESPMode::ThreadSafe>(
			MakeDescriptor(TEXT("adapter.test.serial"), BaseTool, false));
	FHyperAIStudioDomainRegistrationHandle Handle;
	TestTrue(TEXT("adapter registers"), Host.RegisterAdapter(Adapter, Handle, Error));

	for (int32 Index = 0; Index < 33; ++Index)
	{
		FHyperAIStudioTrustedPreparedArtifact Prepared;
		FHyperAIStudioTrustedPrepareReport Report;
		TestTrue(FString::Printf(TEXT("preparation %d succeeds after automatic reconcile"), Index),
			Prepare(Host, Adapter->Descriptor, BaseTool, Prepared, Report, Error));
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		FHyperAIStudioTrustedExecutionDiagnostic Status;
		const FString OperationId = FString::Printf(TEXT("trusted-terminal-%02d"), Index);
		TestTrue(FString::Printf(TEXT("stage %d succeeds"), Index),
			Host.StageExact(Prepared, OperationId, Receipt, Status, Error));
		const int32 BeforeSubmitEffects = Adapter->ExecuteCount;
		FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
		TestTrue(FString::Printf(TEXT("submit %d is accepted"), Index),
			Host.SubmitExact(Receipt, FString(), Submission, Status, Error));
		TestEqual(FString::Printf(TEXT("submit %d returns before any effect"), Index),
			Adapter->ExecuteCount, BeforeSubmitEffects);
		for (int32 Tick = 0; Tick < 12; ++Tick)
		{
			FTSTicker::GetCoreTicker().Tick(0.0f);
		}
		Prepared.Reset();
	}
	TestTrue(TEXT("all serial operations eventually dispatched"), Adapter->ExecuteCount >= 33);
	TestEqual(TEXT("terminal contexts reconcile without any client QueryStatus polling"),
		Host.UnregisterAdapter(Handle, Error), EHyperAIStudioDomainUnregisterResult::Removed);
	return true;
}

#endif
