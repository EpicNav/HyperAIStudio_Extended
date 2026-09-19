// Games by Hyper 2026.

#include "HyperAIStudioTrustedExecution.h"

#include "HyperAIStudioTrustedExecutionInternal.h"

#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioTypedArtifactExecutionInternal.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PluginReferenceDescriptor.h"
#include "ProjectDescriptor.h"

namespace HyperAIStudio::TrustedExecution::Private
{
	struct FRegisteredAdapterRecord
	{
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		FHyperAIStudioDomainRegistrationHandle Handle;
		TWeakPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
		uint64 CatalogGeneration = 0;
		FString CatalogFingerprint;
		EHyperAIStudioCapabilityAdmissionState Admission =
			EHyperAIStudioCapabilityAdmissionState::Planned;
		FString AtomicCohortId;
		FString CohortSourceArtifactFingerprint;
		TArray<FString> RequiredNonBlockingRequirementGroupIds;
		int32 ActiveTrustedPins = 0;
	};

	struct FRegisteredProbeRecord
	{
		FHyperAIStudioDomainRegistrationHandle OwnerAdapter;
		FHyperAIStudioTrustedProbeResult CachedObservation;
		int64 ObservedMonotonicMs = 0;
		uint64 Generation = 0;
		bool bHasObservation = false;
	};

	bool IsSha256(const FString& Value)
	{
		if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool IsBoundedId(const FString& Value, const int32 MaxChars, const TCHAR* Prefix = nullptr)
	{
		if (Value.IsEmpty() || Value.Len() > MaxChars
			|| (Prefix && !Value.StartsWith(Prefix, ESearchCase::CaseSensitive)))
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(':');
			if (!bAllowed)
			{
				return false;
			}
		}
		return true;
	}

	EHyperAIStudioTrustedCatalogAdmission ToPublicAdmission(
		const EHyperAIStudioCapabilityAdmissionState State)
	{
		switch (State)
		{
		case EHyperAIStudioCapabilityAdmissionState::Admitted:
			return EHyperAIStudioTrustedCatalogAdmission::Admitted;
		case EHyperAIStudioCapabilityAdmissionState::SourceCandidate:
			return EHyperAIStudioTrustedCatalogAdmission::SourceCandidate;
		default:
			return EHyperAIStudioTrustedCatalogAdmission::Planned;
		}
	}

	void SetDiagnostic(
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		const FString& Code,
		const FString& Diagnostic,
		const bool bPrerequisite = false,
		const bool bAdmission = false)
	{
		OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
		OutStatus.StatusCode = Code.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		OutStatus.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		OutStatus.bPrerequisiteConflict = bPrerequisite;
		OutStatus.bAdmissionConflict = bAdmission;
	}

	bool SameTrustSeal(
		const FHyperAIStudioDomainTrustSeal& A,
		const FHyperAIStudioDomainTrustSeal& B)
	{
		return A.SchemaVersion == B.SchemaVersion
			&& A.CanonicalProjectId == B.CanonicalProjectId
			&& A.CatalogFingerprint == B.CatalogFingerprint
			&& A.ApprovedPlanFingerprint == B.ApprovedPlanFingerprint
			&& A.AdmissionMatrixFingerprint == B.AdmissionMatrixFingerprint
			&& A.SourceLedgerFingerprint == B.SourceLedgerFingerprint
			&& A.SourceArtifactFingerprint == B.SourceArtifactFingerprint
			&& A.AtomicCohortId == B.AtomicCohortId
			&& A.CohortSourceArtifactFingerprint == B.CohortSourceArtifactFingerprint
			&& A.CatalogGeneration == B.CatalogGeneration
			&& A.AdapterRegistryGeneration == B.AdapterRegistryGeneration
			&& A.ObservedMonotonicMs == B.ObservedMonotonicMs
			&& A.ExpiresMonotonicMs == B.ExpiresMonotonicMs
			&& A.bPreAdmissionEvidence == B.bPreAdmissionEvidence;
	}

	bool SameSemanticPrerequisites(
		const FHyperAIStudioDomainPrerequisiteSnapshot& A,
		const FHyperAIStudioDomainPrerequisiteSnapshot& B)
	{
		if (A.PackId != B.PackId || A.bPackEnabled != B.bPackEnabled
			|| A.Observations.Num() != B.Observations.Num())
		{
			return false;
		}
		TArray<FHyperAIStudioDomainPrerequisiteObservation> Left = A.Observations;
		TArray<FHyperAIStudioDomainPrerequisiteObservation> Right = B.Observations;
		auto Sort = [](TArray<FHyperAIStudioDomainPrerequisiteObservation>& Values)
		{
			Values.Sort([](const FHyperAIStudioDomainPrerequisiteObservation& X,
				const FHyperAIStudioDomainPrerequisiteObservation& Y)
			{
				return X.Id < Y.Id;
			});
		};
		Sort(Left);
		Sort(Right);
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (Left[Index].Id != Right[Index].Id || Left[Index].State != Right[Index].State)
			{
				return false;
			}
		}
		return true;
	}

	bool SameReceipt(
		const FHyperAIStudioTypedArtifactStageReceipt& A,
		const FHyperAIStudioTypedArtifactStageReceipt& B)
	{
		const FString Left = HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(A);
		const FString Right = HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(B);
		return !Left.IsEmpty() && Left == Right;
	}

	bool BuildTrustedAuthorizationRequest(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		const FString& OpaqueToken,
		FHyperAIStudioDomainAuthorizationRequest& OutRequest,
		FString& OutError)
	{
		OutRequest = FHyperAIStudioDomainAuthorizationRequest{};
		const FHyperAIStudioDomainVariantDescriptor* Variant = Descriptor.Variants.FindByPredicate(
			[&Receipt](const FHyperAIStudioDomainVariantDescriptor& Candidate)
			{
				return Candidate.ToolName == Receipt.ToolName
					&& Candidate.VariantId == Receipt.VariantId;
			});
		if (!Variant || (Variant->Safety != EHyperAIStudioDomainSafety::Destructive
				&& Variant->Safety != EHyperAIStudioDomainSafety::ExternalEffect))
		{
			OutError = TEXT("Core UI authorization is valid only for an exact risky catalog variant.");
			return false;
		}
		OutRequest.OpaqueToken = OpaqueToken;
		OutRequest.CanonicalProjectId = Receipt.CanonicalProjectId;
		OutRequest.OperationId = Receipt.OperationId;
		OutRequest.PlanHash = Receipt.PlanHash;
		OutRequest.PackId = Receipt.PackId;
		OutRequest.ToolName = Receipt.ToolName;
		OutRequest.VariantId = Receipt.VariantId;
		OutRequest.AdapterFingerprint = Receipt.AdapterFingerprint;
		OutRequest.AdmissionFingerprint = Receipt.AdmissionFingerprint;
		OutRequest.PrerequisiteFingerprint = Receipt.PrerequisiteFingerprint;
		OutRequest.Safety = Variant->Safety;
		OutRequest.AdapterGeneration = Receipt.AdapterGeneration;
		OutRequest.RegistryEpoch = Receipt.RegistryEpoch;
		return true;
	}

	class FGeneratedCatalogAuthority final : public IHyperAIStudioTrustedCatalogAuthority
	{
	public:
		virtual bool Snapshot(
			FHyperAIStudioTrustedCatalogAuthoritySnapshot& OutSnapshot,
			FString& OutError) const override
		{
			OutError.Reset();
			OutSnapshot = FHyperAIStudioTrustedCatalogAuthoritySnapshot{};
			OutSnapshot.Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
			OutSnapshot.Generation = 1;
			return true;
		}
	};

	class FEnginePrerequisiteEnvironment final
		: public IHyperAIStudioTrustedPrerequisiteEnvironment
	{
	public:
		virtual bool ObservePlugin(
			const FString& PluginName,
			FHyperAIStudioTrustedPluginObservation& OutObservation,
			FString& OutError) const override
		{
			OutObservation = FHyperAIStudioTrustedPluginObservation{};
			OutError.Reset();
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plugin.IsValid())
			{
				return true;
			}
			OutObservation.bInstalled = true;
			OutObservation.bEnabled = Plugin->IsEnabled();
			const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
			if (Project)
			{
				if (const FPluginReferenceDescriptor* Reference = Project->Plugins.FindByPredicate(
					[&PluginName](const FPluginReferenceDescriptor& Item)
					{
						return Item.Name == PluginName;
					}))
				{
					OutObservation.bRestartRequired =
						Reference->bEnabled != OutObservation.bEnabled;
				}
			}
			return true;
		}

		virtual bool ObserveModuleLoaded(
			const FString& ModuleName,
			bool& bOutLoaded,
			FString& OutError) const override
		{
			OutError.Reset();
			bOutLoaded = FModuleManager::Get().IsModuleLoaded(FName(*ModuleName));
			return true;
		}

		virtual bool AreSourceCandidateToolsEnabled() const override
		{
			return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
		}
	};

	class FSystemClock final : public IHyperAIStudioTypedArtifactClock
	{
	public:
		virtual int64 NowMonotonicMs() const override
		{
			return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
		}

		virtual int64 NowUtcMs() const override
		{
			const FDateTime Now = FDateTime::UtcNow();
			return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
		}
	};

	bool SameAuthorizationRequestExact(
		const FHyperAIStudioDomainAuthorizationRequest& A,
		const FHyperAIStudioDomainAuthorizationRequest& B,
		const bool bCompareOpaqueToken = true)
	{
		return (!bCompareOpaqueToken || A.OpaqueToken == B.OpaqueToken)
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

	struct FTrustedAuthorizationGrantEntry
	{
		FHyperAIStudioDomainAuthorizationReceipt Receipt;
		FString ExactStageFingerprint;
		int64 ExpiresMonotonicMs = 0;
	};

	/**
	 * Process-local server authority used by production trusted execution. Optional modules see only
	 * the consumer facade; issuance is reachable exclusively through the private core-UI hook below.
	 */
	class FTrustedServerGrantGate final : public IHyperAIStudioDomainAuthorizationGate
	{
	public:
		explicit FTrustedServerGrantGate(
			const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& InClock)
			: Clock(InClock)
		{
		}

		bool IssueExact(
			const FHyperAIStudioDomainAuthorizationRequest& TrustedBinding,
			const FString& ExactStageFingerprint,
			const bool bAdmitConsumedTerminalReplay,
			FString& OutToken,
			FString& OutError)
		{
			OutToken.Reset();
			OutError.Reset();
			if (!TrustedBinding.OpaqueToken.IsEmpty()
				|| !IsSha256(ExactStageFingerprint)
				|| !IsBoundedId(
					TrustedBinding.CanonicalProjectId,
					FHyperAIStudioDomainLimits::MaxCanonicalProjectIdChars)
				|| !FHyperAIStudioOperationJournal::IsValidOperationId(TrustedBinding.OperationId)
				|| !IsSha256(TrustedBinding.PlanHash)
				|| !IsBoundedId(TrustedBinding.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
				|| !IsBoundedId(TrustedBinding.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars)
				|| !IsBoundedId(TrustedBinding.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
				|| !IsSha256(TrustedBinding.AdapterFingerprint)
				|| !IsSha256(TrustedBinding.AdmissionFingerprint)
				|| !IsSha256(TrustedBinding.PrerequisiteFingerprint)
				|| (TrustedBinding.Safety != EHyperAIStudioDomainSafety::Destructive
					&& TrustedBinding.Safety != EHyperAIStudioDomainSafety::ExternalEffect)
				|| TrustedBinding.AdapterGeneration == 0 || TrustedBinding.RegistryEpoch == 0)
			{
				OutError = TEXT("Only an exact core-sealed destructive/external stage may receive a trusted grant.");
				return false;
			}
			// Clock callbacks are core-owned but remain outside the grant-store lock by contract.
			const int64 NowMonotonicMs = Clock->NowMonotonicMs();
			const int64 NowUtcMs = Clock->NowUtcMs();
			if (NowMonotonicMs < 0 || NowUtcMs < 0
				|| NowMonotonicMs > MAX_int64 - FHyperAIStudioTrustedExecutionLimits::AuthorizationGrantLifetimeMs
				|| NowUtcMs > MAX_int64 - FHyperAIStudioTrustedExecutionLimits::AuthorizationGrantLifetimeMs)
			{
				OutError = TEXT("Trusted authorization clock sample is outside the bounded envelope.");
				return false;
			}

			FScopeLock Lock(&Mutex);
			PruneExpiredLocked(NowMonotonicMs);
			if (bShuttingDown)
			{
				OutError = TEXT("Trusted authorization authority is quiescing.");
				return false;
			}
			if (const FString* ExistingToken = TokenByStageFingerprint.Find(ExactStageFingerprint))
			{
				const FTrustedAuthorizationGrantEntry* Existing = GrantsByToken.Find(*ExistingToken);
				if (!Existing
					|| !SameAuthorizationRequestExact(
						Existing->Receipt.BoundRequest, TrustedBinding, false))
				{
					OutError = TEXT("The exact stage is already bound to a different trusted authorization grant.");
					return false;
				}
				OutToken = *ExistingToken;
				return true;
			}
			if (GrantsByToken.Num() >= FHyperAIStudioTrustedExecutionLimits::MaxActiveAuthorizationGrants)
			{
				OutError = TEXT("The bounded trusted authorization table is full.");
				return false;
			}

			FString Token;
			for (int32 Attempt = 0; Attempt < 8 && Token.IsEmpty(); ++Attempt)
			{
				const FString Candidate = TEXT("trusted-auth-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
				if (!GrantsByToken.Contains(Candidate))
				{
					Token = Candidate;
				}
			}
			if (Token.IsEmpty())
			{
				OutError = TEXT("A unique bounded trusted authorization token could not be allocated.");
				return false;
			}
			FTrustedAuthorizationGrantEntry Entry;
			Entry.ExactStageFingerprint = ExactStageFingerprint;
			Entry.ExpiresMonotonicMs =
				NowMonotonicMs + FHyperAIStudioTrustedExecutionLimits::AuthorizationGrantLifetimeMs;
			Entry.Receipt.BoundRequest = TrustedBinding;
			Entry.Receipt.BoundRequest.OpaqueToken = Token;
			Entry.Receipt.Nonce = TEXT("trusted-grant-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			Entry.Receipt.IssuedUtcMs = NowUtcMs;
			Entry.Receipt.ExpiresUtcMs =
				NowUtcMs + FHyperAIStudioTrustedExecutionLimits::AuthorizationGrantLifetimeMs;
			Entry.Receipt.bConsumed = bAdmitConsumedTerminalReplay;
			GrantsByToken.Add(Token, MoveTemp(Entry));
			TokenByStageFingerprint.Add(ExactStageFingerprint, Token);
			OutToken = MoveTemp(Token);
			return true;
		}

		virtual bool Inspect(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			return InspectOrConsume(Request, NowUtcMs, false, OutReceipt, OutError);
		}

		virtual bool Consume(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			return InspectOrConsume(Request, NowUtcMs, true, OutReceipt, OutError);
		}

		void Shutdown()
		{
			FScopeLock Lock(&Mutex);
			bShuttingDown = true;
			GrantsByToken.Reset();
			TokenByStageFingerprint.Reset();
		}

	private:
		bool InspectOrConsume(
			const FHyperAIStudioDomainAuthorizationRequest& Request,
			const int64 NowUtcMs,
			const bool bConsume,
			FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
			FString& OutError)
		{
			OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
			OutError.Reset();
			if (NowUtcMs < 0 || Request.OpaqueToken.Len() < 16
				|| Request.OpaqueToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
			{
				OutError = TEXT("Trusted authorization request is outside the bounded envelope.");
				return false;
			}
			// Never invoke even a core-owned clock while holding the grant-store mutex.
			const int64 NowMonotonicMs = Clock->NowMonotonicMs();
			if (NowMonotonicMs < 0)
			{
				OutError = TEXT("Trusted authorization monotonic clock is invalid.");
				return false;
			}
			FScopeLock Lock(&Mutex);
			PruneExpiredLocked(NowMonotonicMs);
			if (bShuttingDown)
			{
				OutError = TEXT("Trusted authorization authority is quiescing.");
				return false;
			}
			FTrustedAuthorizationGrantEntry* Existing = GrantsByToken.Find(Request.OpaqueToken);
			if (!Existing || !SameAuthorizationRequestExact(Existing->Receipt.BoundRequest, Request)
				|| Existing->ExpiresMonotonicMs <= NowMonotonicMs
				|| Existing->Receipt.IssuedUtcMs > NowUtcMs
				|| Existing->Receipt.ExpiresUtcMs <= NowUtcMs)
			{
				OutError = TEXT("Trusted authorization binding is missing, expired, or mismatched.");
				return false;
			}
			if (bConsume && Existing->Receipt.bConsumed)
			{
				OutError = TEXT("Trusted authorization grant was already consumed.");
				return false;
			}
			if (bConsume)
			{
				Existing->Receipt.bConsumed = true;
			}
			OutReceipt = Existing->Receipt;
			return true;
		}

		void PruneExpiredLocked(const int64 NowMonotonicMs)
		{
			for (auto It = GrantsByToken.CreateIterator(); It; ++It)
			{
				if (It.Value().ExpiresMonotonicMs <= NowMonotonicMs)
				{
					if (const FString* IndexedToken =
						TokenByStageFingerprint.Find(It.Value().ExactStageFingerprint))
					{
						if (*IndexedToken == It.Key())
						{
							TokenByStageFingerprint.Remove(It.Value().ExactStageFingerprint);
						}
					}
					It.RemoveCurrent();
				}
			}
		}

		FCriticalSection Mutex;
		TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock;
		TMap<FString, FTrustedAuthorizationGrantEntry> GrantsByToken;
		TMap<FString, FString> TokenByStageFingerprint;
		bool bShuttingDown = false;
	};

	struct FTrustedAuthorizationBundle
	{
		TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> Gate;
		TSharedPtr<FTrustedServerGrantGate, ESPMode::ThreadSafe> CoreIssuer;
	};

	FTrustedAuthorizationBundle MakeTrustedAuthorizationBundle(
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock,
		const TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe>& InjectedGate)
	{
		FTrustedAuthorizationBundle Bundle;
		if (InjectedGate.IsValid())
		{
			Bundle.Gate = InjectedGate;
			return Bundle;
		}
		Bundle.CoreIssuer = MakeShared<FTrustedServerGrantGate, ESPMode::ThreadSafe>(Clock);
		Bundle.Gate = Bundle.CoreIssuer;
		return Bundle;
	}
}

namespace HyperAIStudio::TrustedExecution::Private
{
	class FTrustedAdapterModulePin;
	struct FTrustedPreparationRecord;
}

struct FHyperAIStudioTrustedPreparedArtifactState
{
	~FHyperAIStudioTrustedPreparedArtifactState();
	TWeakPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Host;
	uint64 HostGeneration = 0;
	FString PreparationId;
	FString ContractFingerprint;
	int64 ExpiresUtcMs = 0;
	int64 ExpiresMonotonicMs = 0;
};

namespace HyperAIStudio::TrustedExecution::Private
{
	class FTrustedStateGate;

	struct FTrustedPreparationRecord
	{
		uint64 HostGeneration = 0;
		FHyperAIStudioTrustedArtifactRequest Request;
		FHyperAIStudioPreparedTypedArtifact Prepared;
		// Pin is declared before optional-module objects; the explicit destructor repeats this rule.
		TSharedPtr<FTrustedAdapterModulePin, ESPMode::ThreadSafe> AdapterModulePin;
		TSharedPtr<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload;
		TSharedPtr<IHyperAIStudioTrustedFreshVerifier, ESPMode::ThreadSafe> FreshVerifier;
		EHyperAIStudioCapabilityAdmissionState Admission =
			EHyperAIStudioCapabilityAdmissionState::Planned;
		bool bPreAdmissionEvidenceExecution = false;
		FHyperAIStudioDomainAdapterDescriptor AdapterDescriptor;
		FString PreparationId;
		int64 ExpiresUtcMs = 0;
		int64 ExpiresMonotonicMs = 0;

		~FTrustedPreparationRecord();
	};

	struct FTrustedStageContext
	{
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		TSharedPtr<const FTrustedPreparationRecord, ESPMode::ThreadSafe> Prepared;
		TSharedPtr<FTrustedStateGate, ESPMode::ThreadSafe> StateGate;
		uint64 Sequence = 0;
		int64 ExpiresMonotonicMs = 0;
		bool bDurable = false;
	};
}

struct FHyperAIStudioTrustedExecutionHostState
{
	FHyperAIStudioTrustedExecutionHostState(
		const FString& InProjectRoot,
		const TSharedRef<IHyperAIStudioTrustedCatalogAuthority, ESPMode::ThreadSafe>& InCatalogAuthority,
		const TSharedRef<IHyperAIStudioTrustedPrerequisiteEnvironment, ESPMode::ThreadSafe>& InEnvironment,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& InClock,
		TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> InAuthorizationGate)
		: ProjectRoot(InProjectRoot)
		, CanonicalProjectId(FHyperAIStudioOperationJournal::MakeCanonicalProjectId(InProjectRoot))
		, CatalogAuthority(InCatalogAuthority)
		, Environment(InEnvironment)
		, Clock(InClock)
		, Authorization(HyperAIStudio::TrustedExecution::Private::MakeTrustedAuthorizationBundle(
			InClock, InAuthorizationGate))
		, Registry(MakeShared<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>(
			nullptr, Authorization.Gate))
		, Service(MakeUnique<FHyperAIStudioTypedArtifactExecutionService>(
			Registry, InProjectRoot, InClock))
	{
	}

	mutable FCriticalSection Mutex;
	FString ProjectRoot;
	FString CanonicalProjectId;
	TSharedRef<IHyperAIStudioTrustedCatalogAuthority, ESPMode::ThreadSafe> CatalogAuthority;
	TSharedRef<IHyperAIStudioTrustedPrerequisiteEnvironment, ESPMode::ThreadSafe> Environment;
	TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock;
	HyperAIStudio::TrustedExecution::Private::FTrustedAuthorizationBundle Authorization;
	TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> Registry;
	TUniquePtr<FHyperAIStudioTypedArtifactExecutionService> Service;
	TMap<FString, HyperAIStudio::TrustedExecution::Private::FRegisteredAdapterRecord> RegisteredAdapters;
	TSet<FString> AdapterReservations;
	TMap<FString, HyperAIStudio::TrustedExecution::Private::FRegisteredProbeRecord> RegisteredProbes;
	TSet<FString> DuplicateProbeIds;
	TMap<FString, HyperAIStudio::TrustedExecution::Private::FTrustedStageContext> StageContexts;
	TMap<FString, TSharedPtr<HyperAIStudio::TrustedExecution::Private::FTrustedPreparationRecord,
		ESPMode::ThreadSafe>> Preparations;
	TSet<FString> StageReservations;
	uint64 HostGeneration = 1;
	uint64 ProbeRegistryGeneration = 1;
	uint64 NextProbeGeneration = 1;
	uint64 NextStageSequence = 1;
	uint64 NextPreparationSequence = 1;
	int32 ActiveStageCalls = 0;
	bool bStarted = false;
	bool bShuttingDown = false;
};

FHyperAIStudioTrustedPreparedArtifactState::~FHyperAIStudioTrustedPreparedArtifactState()
{
	TSharedPtr<HyperAIStudio::TrustedExecution::Private::FTrustedPreparationRecord,
		ESPMode::ThreadSafe> Released;
	if (const TSharedPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Pinned = Host.Pin())
	{
		FScopeLock Lock(&Pinned->Mutex);
		if (TSharedPtr<HyperAIStudio::TrustedExecution::Private::FTrustedPreparationRecord,
			ESPMode::ThreadSafe>* Record = Pinned->Preparations.Find(PreparationId))
		{
			if (Record->IsValid() && (*Record)->Prepared.ContractFingerprint == ContractFingerprint
				&& (*Record)->HostGeneration == HostGeneration)
			{
				Released = MoveTemp(*Record);
				Pinned->Preparations.Remove(PreparationId);
			}
		}
	}
}

namespace HyperAIStudio::TrustedExecution::Private
{
	/** Strong optional-module pin paired with a host-side unregister veto. */
	class FTrustedAdapterModulePin final
	{
	public:
		FTrustedAdapterModulePin(
			const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& InState,
			const FString& InAdapterFingerprint,
			const uint64 InAdapterGeneration,
			const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& InAdapter)
			: State(InState)
			, AdapterFingerprint(InAdapterFingerprint)
			, AdapterGeneration(InAdapterGeneration)
			, Adapter(InAdapter)
		{
		}

		~FTrustedAdapterModulePin()
		{
			// Drop the last pin-owned optional vtable while unregister is still vetoed.
			Adapter.Reset();
			if (const TSharedPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Pinned =
				State.Pin())
			{
				FScopeLock Lock(&Pinned->Mutex);
				if (FRegisteredAdapterRecord* Record =
					Pinned->RegisteredAdapters.Find(AdapterFingerprint))
				{
					if (Record->Handle.AdapterGeneration == AdapterGeneration
						&& Record->ActiveTrustedPins > 0)
					{
						--Record->ActiveTrustedPins;
					}
				}
			}
		}

	private:
		TWeakPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> State;
		FString AdapterFingerprint;
		uint64 AdapterGeneration = 0;
		TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
	};

	FTrustedPreparationRecord::~FTrustedPreparationRecord()
	{
		FreshVerifier.Reset();
		Payload.Reset();
		AdapterModulePin.Reset();
	}

	/** Bounded read-only reconcile; service queries and optional-object destruction run unlocked. */
	void ReconcileTrustedOwnedState(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State)
	{
		struct FCandidate
		{
			FString StageId;
			FString OperationId;
			FString ReceiptFingerprint;
		};
		const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
		// The trusted stage context and the private typed store each retain their own
		// exact-generation pin. Counting also performs the store's bounded monotonic
		// expiry purge, so both pins are reconciled by the same host entry points.
		(void)State->Service->NumStaged();
		TArray<FCandidate> DurableCandidates;
		{
			FScopeLock Lock(&State->Mutex);
			DurableCandidates.Reserve(State->StageContexts.Num());
			for (const TPair<FString, FTrustedStageContext>& Pair : State->StageContexts)
			{
				if (Pair.Value.bDurable)
				{
					DurableCandidates.Add({
						Pair.Key,
						Pair.Value.Receipt.OperationId,
						HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(
							Pair.Value.Receipt)});
				}
			}
		}

		TSet<FString> TerminalStageIds;
		for (const FCandidate& Candidate : DurableCandidates)
		{
			FHyperAIStudioTypedArtifactOperationStatus Status;
			FString QueryError;
			if (!Candidate.ReceiptFingerprint.IsEmpty()
				&& State->Service->QueryStatus(Candidate.OperationId, Status, QueryError)
				&& Status.bFound && Status.bTerminal)
			{
				TerminalStageIds.Add(Candidate.StageId);
			}
		}

		TArray<FTrustedStageContext> ReleasedStages;
		TArray<TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>> ReleasedPreparations;
		{
			FScopeLock Lock(&State->Mutex);
			TArray<FString> PreparationIdsToRelease;
			for (const TPair<FString, TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>>& Pair
				: State->Preparations)
			{
				if (Pair.Value.IsValid()
					&& Pair.Value->ExpiresMonotonicMs <= NowMonotonicMs)
				{
					PreparationIdsToRelease.Add(Pair.Key);
				}
			}
			for (const FString& Id : PreparationIdsToRelease)
			{
				if (TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>* Record =
					State->Preparations.Find(Id))
				{
					ReleasedPreparations.Add(MoveTemp(*Record));
					State->Preparations.Remove(Id);
				}
			}

			TArray<FString> StageIdsToRelease;
			for (const TPair<FString, FTrustedStageContext>& Pair : State->StageContexts)
			{
				const bool bExpiredUnadmitted = !Pair.Value.bDurable
					&& Pair.Value.ExpiresMonotonicMs > 0
					&& Pair.Value.ExpiresMonotonicMs <= NowMonotonicMs;
				if (bExpiredUnadmitted || TerminalStageIds.Contains(Pair.Key))
				{
					StageIdsToRelease.Add(Pair.Key);
				}
			}
			for (const FString& Id : StageIdsToRelease)
			{
				if (FTrustedStageContext* Context = State->StageContexts.Find(Id))
				{
					if (Context->bDurable)
					{
						const FCandidate* Candidate = DurableCandidates.FindByPredicate(
							[&Id](const FCandidate& Item) { return Item.StageId == Id; });
						const FString CurrentFingerprint =
							HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(
								Context->Receipt);
						if (!Candidate || Candidate->ReceiptFingerprint != CurrentFingerprint
							|| !TerminalStageIds.Contains(Id))
						{
							continue;
						}
					}
					ReleasedStages.Add(MoveTemp(*Context));
					State->StageContexts.Remove(Id);
				}
			}
		}
	}

	struct FResolvedAuthority
	{
		FHyperAIStudioTrustedCatalogAuthoritySnapshot Catalog;
		FHyperAIStudioDomainBinding Binding;
		FHyperAIStudioDomainAdapterDescriptor AdapterDescriptor;
		EHyperAIStudioCapabilityAdmissionState Admission =
			EHyperAIStudioCapabilityAdmissionState::Planned;
		bool bPreAdmissionEvidenceExecution = false;
		bool bPrerequisitesReady = false;
		FHyperAIStudioTrustedExecutionDiagnostic Status;
	};

	struct FProbeSnapshot
	{
		TMap<FString, FRegisteredProbeRecord> Observations;
		TSet<FString> DuplicateIds;
		int64 SampledMonotonicMs = 0;
		uint64 Generation = 0;
		uint64 HostGeneration = 0;
	};

	bool ValidateCatalogSnapshot(
		const FHyperAIStudioTrustedCatalogAuthoritySnapshot& Snapshot,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError)
	{
		TArray<FString> Errors;
		if (Snapshot.Generation == 0
			|| Snapshot.Catalog.Schema != FHyperAIStudioCapabilityPackRegistry::CatalogSchema
			|| !FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(Snapshot.Catalog, Errors))
		{
			OutError = Errors.IsEmpty()
				? TEXT("Generated capability catalog authority is missing or unsupported.")
				: FString::Join(Errors, TEXT("; "));
			SetDiagnostic(OutStatus, TEXT("catalog_authority_invalid"), OutError, false, true);
			return false;
		}
		return true;
	}

	bool SnapshotCatalog(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		FHyperAIStudioTrustedCatalogAuthoritySnapshot& OutCatalog,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError)
	{
		uint64 HostGeneration = 0;
		{
			FScopeLock Lock(&State->Mutex);
			if (!State->bStarted || State->bShuttingDown)
			{
				OutError = TEXT("Trusted execution host is not accepting work.");
				SetDiagnostic(OutStatus, TEXT("trusted_host_quiescing"), OutError);
				return false;
			}
			HostGeneration = State->HostGeneration;
		}
		if (!State->CatalogAuthority->Snapshot(OutCatalog, OutError)
			|| !ValidateCatalogSnapshot(OutCatalog, OutStatus, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Generated capability catalog callback failed.");
				SetDiagnostic(OutStatus, TEXT("catalog_callback_error"), OutError, false, true);
			}
			return false;
		}
		{
			FScopeLock Lock(&State->Mutex);
			if (!State->bStarted || State->bShuttingDown
				|| State->HostGeneration != HostGeneration)
			{
				OutError = TEXT("Trusted host lifecycle changed while catalog authority was sampled.");
				SetDiagnostic(OutStatus, TEXT("trusted_host_lifecycle_drift"), OutError);
				return false;
			}
		}
		return true;
	}

	const FHyperAIStudioCapabilityPrerequisiteDefinition* FindPrerequisite(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FString& Id)
	{
		return Catalog.Prerequisites.FindByPredicate([&Id](const auto& Item)
		{
			return Item.Id == Id;
		});
	}

	const FHyperAIStudioCapabilityPackDefinition* FindPack(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FString& Id)
	{
		return Catalog.Packs.FindByPredicate([&Id](const auto& Item)
		{
			return Item.Id == Id;
		});
	}

	const FHyperAIStudioCapabilityToolDefinition* FindTool(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FString& Name)
	{
		return Catalog.Tools.FindByPredicate([&Name](const auto& Item)
		{
			return Item.Name == Name;
		});
	}

	bool IsGeneratedSafetyAllowed(
		const FHyperAIStudioCapabilityToolDefinition& Tool,
		const EHyperAIStudioDomainSafety Safety)
	{
		EHyperAIStudioCapabilitySafetyClass GeneratedSafety;
		switch (Safety)
		{
		case EHyperAIStudioDomainSafety::Read:
			GeneratedSafety = EHyperAIStudioCapabilitySafetyClass::Read;
			break;
		case EHyperAIStudioDomainSafety::Edit:
			GeneratedSafety = EHyperAIStudioCapabilitySafetyClass::Edit;
			break;
		case EHyperAIStudioDomainSafety::Destructive:
			GeneratedSafety = EHyperAIStudioCapabilitySafetyClass::Destructive;
			break;
		case EHyperAIStudioDomainSafety::ExternalEffect:
			GeneratedSafety = EHyperAIStudioCapabilitySafetyClass::ExternalEffect;
			break;
		default:
			return false;
		}
		return Tool.AllowedSafetyClasses.Contains(GeneratedSafety);
	}

	bool CaptureProbeSnapshot(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		const TSet<FString>& NeededIds,
		FProbeSnapshot& OutSnapshot,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError)
	{
		const int64 SampledMonotonicMs = State->Clock->NowMonotonicMs();
		if (SampledMonotonicMs < 0)
		{
			OutError = TEXT("Trusted prerequisite monotonic observation is invalid.");
			SetDiagnostic(OutStatus, TEXT("prerequisite_clock_invalid"), OutError, true);
			return false;
		}
		FScopeLock Lock(&State->Mutex);
		if (!State->bStarted || State->bShuttingDown)
		{
			OutError = TEXT("Trusted execution host quiesced before prerequisite observation.");
			SetDiagnostic(OutStatus, TEXT("trusted_host_quiescing"), OutError);
			return false;
		}
		OutSnapshot.Generation = State->ProbeRegistryGeneration;
		OutSnapshot.HostGeneration = State->HostGeneration;
		OutSnapshot.SampledMonotonicMs = SampledMonotonicMs;
		for (const FString& Id : NeededIds)
		{
			if (const FRegisteredProbeRecord* Record = State->RegisteredProbes.Find(Id))
			{
				OutSnapshot.Observations.Add(Id, *Record);
			}
			if (State->DuplicateProbeIds.Contains(Id))
			{
				OutSnapshot.DuplicateIds.Add(Id);
			}
		}
		return true;
	}

	EHyperAIStudioDomainPrerequisiteState SamplePrerequisite(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition,
		const FProbeSnapshot& Probes,
		FString& OutDiagnostic)
	{
		OutDiagnostic.Reset();
		FString Name = Definition.Id;
		if (Definition.Kind == EHyperAIStudioCapabilityPrerequisiteKind::Plugin)
		{
			Name.RightChopInline(7, EAllowShrinking::No);
			FHyperAIStudioTrustedPluginObservation Observation;
			FString CallbackError;
			if (!State->Environment->ObservePlugin(Name, Observation, CallbackError))
			{
				OutDiagnostic = CallbackError.IsEmpty() ? TEXT("plugin_observer_callback_error") : CallbackError;
				return EHyperAIStudioDomainPrerequisiteState::Missing;
			}
			if (!Observation.bInstalled)
			{
				OutDiagnostic = TEXT("plugin_missing");
				return EHyperAIStudioDomainPrerequisiteState::Missing;
			}
			if (Observation.bRestartRequired)
			{
				OutDiagnostic = TEXT("plugin_restart_required");
				return EHyperAIStudioDomainPrerequisiteState::RestartRequired;
			}
			if (!Observation.bEnabled)
			{
				OutDiagnostic = TEXT("plugin_disabled");
				return EHyperAIStudioDomainPrerequisiteState::Disabled;
			}
			return EHyperAIStudioDomainPrerequisiteState::Available;
		}
		if (Definition.Kind == EHyperAIStudioCapabilityPrerequisiteKind::Module)
		{
			Name.RightChopInline(7, EAllowShrinking::No);
			bool bLoaded = false;
			FString CallbackError;
			if (!State->Environment->ObserveModuleLoaded(Name, bLoaded, CallbackError))
			{
				OutDiagnostic = CallbackError.IsEmpty() ? TEXT("module_observer_callback_error") : CallbackError;
				return EHyperAIStudioDomainPrerequisiteState::Missing;
			}
			if (!bLoaded)
			{
				OutDiagnostic = TEXT("module_not_loaded");
				return EHyperAIStudioDomainPrerequisiteState::Missing;
			}
			return EHyperAIStudioDomainPrerequisiteState::Available;
		}

		if (Probes.DuplicateIds.Contains(Definition.Id))
		{
			OutDiagnostic = TEXT("probe_registration_duplicate");
			return EHyperAIStudioDomainPrerequisiteState::Missing;
		}
		const FRegisteredProbeRecord* Cached = Probes.Observations.Find(Definition.Id);
		if (!Cached || !Cached->bHasObservation)
		{
			OutDiagnostic = Cached ? TEXT("probe_observation_missing") : TEXT("probe_provider_missing");
			return EHyperAIStudioDomainPrerequisiteState::Missing;
		}
		if (Cached->ObservedMonotonicMs < 0
			|| Probes.SampledMonotonicMs < Cached->ObservedMonotonicMs
			|| Probes.SampledMonotonicMs - Cached->ObservedMonotonicMs
				>= FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs)
		{
			OutDiagnostic = TEXT("probe_observation_stale");
			return EHyperAIStudioDomainPrerequisiteState::Missing;
		}
		if (Cached->CachedObservation.StatusCode.Len()
				> FHyperAIStudioDomainLimits::MaxStatusCodeChars
			|| Cached->CachedObservation.Diagnostic.Len()
				> FHyperAIStudioTrustedExecutionLimits::MaxProbeDiagnosticChars)
		{
			OutDiagnostic = TEXT("probe_observation_invalid");
			return EHyperAIStudioDomainPrerequisiteState::Missing;
		}
		if (!Cached->CachedObservation.bReady)
		{
			OutDiagnostic = Cached->CachedObservation.StatusCode.IsEmpty()
				? TEXT("probe_not_ready") : Cached->CachedObservation.StatusCode;
			return EHyperAIStudioDomainPrerequisiteState::Missing;
		}
		return EHyperAIStudioDomainPrerequisiteState::Available;
	}

	bool SamplePackPrerequisites(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		const FHyperAIStudioTrustedCatalogAuthoritySnapshot& CatalogSnapshot,
		const FHyperAIStudioCapabilityPackDefinition& RootPack,
		const TArray<FString>& ApplicableNonBlockingRequirementGroupIds,
		FHyperAIStudioDomainPrerequisiteSnapshot& OutPrerequisites,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError)
	{
		TArray<const FHyperAIStudioCapabilityPackDefinition*> Packs;
		TSet<FString> Visited;
		TArray<FString> Queue{RootPack.Id};
		while (!Queue.IsEmpty())
		{
			const FString Id = Queue[0];
			Queue.RemoveAt(0, 1, EAllowShrinking::No);
			if (Visited.Contains(Id))
			{
				continue;
			}
			Visited.Add(Id);
			const FHyperAIStudioCapabilityPackDefinition* Pack = FindPack(CatalogSnapshot.Catalog, Id);
			if (!Pack)
			{
				OutError = TEXT("Catalog dependency pack is missing.");
				SetDiagnostic(OutStatus, TEXT("catalog_dependency_missing"), OutError, false, true);
				return false;
			}
			Packs.Add(Pack);
			Queue.Append(Pack->DependsOnPackIds);
		}

		TSet<FString> NeededIds;
		for (const FHyperAIStudioCapabilityPackDefinition* Pack : Packs)
		{
			for (const FHyperAIStudioCapabilityRequirementGroup& Group : Pack->Requirements)
			{
				const bool bApplicable = Group.bBlocking
					|| (Pack->Id == RootPack.Id
						&& ApplicableNonBlockingRequirementGroupIds.Contains(Group.Id));
				if (bApplicable)
				{
					NeededIds.Append(Group.PrerequisiteIds);
				}
			}
		}
		FProbeSnapshot ProbeSnapshot;
		if (!CaptureProbeSnapshot(State, NeededIds, ProbeSnapshot, OutStatus, OutError))
		{
			return false;
		}

		TMap<FString, EHyperAIStudioDomainPrerequisiteState> States;
		TMap<FString, FString> Diagnostics;
		for (const FString& Id : NeededIds)
		{
			const FHyperAIStudioCapabilityPrerequisiteDefinition* Definition =
				FindPrerequisite(CatalogSnapshot.Catalog, Id);
			if (!Definition)
			{
				OutError = TEXT("Catalog requirement references an unknown prerequisite.");
				SetDiagnostic(OutStatus, TEXT("catalog_prerequisite_unknown"), OutError, true);
				return false;
			}
			FString Diagnostic;
			States.Add(Id, SamplePrerequisite(State, *Definition, ProbeSnapshot, Diagnostic));
			Diagnostics.Add(Id, Diagnostic);
		}

		{
			FScopeLock Lock(&State->Mutex);
			if (!State->bStarted || State->bShuttingDown
				|| State->HostGeneration != ProbeSnapshot.HostGeneration
				|| State->ProbeRegistryGeneration != ProbeSnapshot.Generation)
			{
				OutError = TEXT("Cached prerequisite registry changed while its snapshot was sampled.");
				SetDiagnostic(OutStatus, TEXT("prerequisite_provider_drift"), OutError, true);
				return false;
			}
		}

		TMap<FString, EHyperAIStudioDomainPrerequisiteState> Selected;
		bool bAllReady = true;
		for (const FHyperAIStudioCapabilityPackDefinition* Pack : Packs)
		{
			for (const FHyperAIStudioCapabilityRequirementGroup& Group : Pack->Requirements)
			{
				const bool bApplicable = Group.bBlocking
					|| (Pack->Id == RootPack.Id
						&& ApplicableNonBlockingRequirementGroupIds.Contains(Group.Id));
				if (!bApplicable)
				{
					continue;
				}
				bool bGroupReady = Group.Mode == EHyperAIStudioCapabilityRequirementMode::AllOf;
				if (Group.Mode == EHyperAIStudioCapabilityRequirementMode::AllOf)
				{
					for (const FString& Id : Group.PrerequisiteIds)
					{
						Selected.Add(Id, States.FindChecked(Id));
						bGroupReady &= States.FindChecked(Id)
							== EHyperAIStudioDomainPrerequisiteState::Available;
					}
				}
				else
				{
					bGroupReady = false;
					for (const FString& Id : Group.PrerequisiteIds)
					{
						if (States.FindChecked(Id) == EHyperAIStudioDomainPrerequisiteState::Available)
						{
							Selected.Add(Id, EHyperAIStudioDomainPrerequisiteState::Available);
							bGroupReady = true;
							break;
						}
					}
					if (!bGroupReady)
					{
						for (const FString& Id : Group.PrerequisiteIds)
						{
							Selected.Add(Id, States.FindChecked(Id));
						}
					}
				}
				bAllReady &= bGroupReady;
				if (!bGroupReady)
				{
					OutStatus.BlockingPrerequisiteIds.Append(Group.PrerequisiteIds);
				}
			}
		}

		OutPrerequisites = FHyperAIStudioDomainPrerequisiteSnapshot{};
		OutPrerequisites.PackId = RootPack.Id;
		OutPrerequisites.bPackEnabled = bAllReady;
		OutPrerequisites.Revision = CatalogSnapshot.Generation;
		for (const TPair<FString, EHyperAIStudioDomainPrerequisiteState>& Pair : Selected)
		{
			OutPrerequisites.Observations.Add({Pair.Key, Pair.Value});
		}
		OutPrerequisites.Observations.Sort([](const auto& A, const auto& B)
		{
			return A.Id < B.Id;
		});
		if (!bAllReady)
		{
			TArray<FString> BlockingIds = MoveTemp(OutStatus.BlockingPrerequisiteIds);
			OutError = TEXT("One or more exact generated prerequisites are not live and current.");
			SetDiagnostic(OutStatus, TEXT("prerequisite_conflict"), OutError, true);
			OutStatus.BlockingPrerequisiteIds = MoveTemp(BlockingIds);
		}
		return true;
	}

	bool ResolveAuthority(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		const FHyperAIStudioTrustedArtifactRequest& Request,
		FResolvedAuthority& OutResolved,
		FString& OutError)
	{
		OutResolved = FResolvedAuthority{};
		OutError.Reset();
		if (!SnapshotCatalog(State, OutResolved.Catalog, OutResolved.Status, OutError))
		{
			return false;
		}
		const FHyperAIStudioCapabilityPackDefinition* Pack =
			FindPack(OutResolved.Catalog.Catalog, Request.PackId);
		const FHyperAIStudioCapabilityToolDefinition* Tool =
			FindTool(OutResolved.Catalog.Catalog, Request.ToolName);
		if (!Pack || !Tool || Tool->PackId != Request.PackId
			|| !Pack->ToolNames.Contains(Request.ToolName)
			|| !Pack->AtomicCohortIds.Contains(Tool->AtomicCohortId))
		{
			OutError = TEXT("Pack, tool and atomic cohort do not match the generated catalog exactly.");
			SetDiagnostic(OutResolved.Status, TEXT("catalog_exact_binding_missing"), OutError, false, true);
			return false;
		}
		if (Pack->AdmissionState != Tool->AdmissionState)
		{
			OutError = TEXT("Generated pack and tool admission rows disagree.");
			SetDiagnostic(OutResolved.Status, TEXT("catalog_admission_mismatch"), OutError, false, true);
			return false;
		}
		OutResolved.Admission = Tool->AdmissionState;
		OutResolved.bPreAdmissionEvidenceExecution =
			OutResolved.Admission == EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			&& State->Environment->AreSourceCandidateToolsEnabled();

		FRegisteredAdapterRecord Adapter;
		uint64 HostGeneration = 0;
		{
			FScopeLock Lock(&State->Mutex);
			if (!State->bStarted || State->bShuttingDown)
			{
				OutError = TEXT("Trusted execution host is quiescing.");
				SetDiagnostic(OutResolved.Status, TEXT("trusted_host_quiescing"), OutError);
				return false;
			}
			const FRegisteredAdapterRecord* Found = nullptr;
			for (const TPair<FString, FRegisteredAdapterRecord>& Pair : State->RegisteredAdapters)
			{
				if (Pair.Value.Descriptor.PackId != Request.PackId)
				{
					continue;
				}
				const bool bOwnsVariant = Pair.Value.Descriptor.Variants.ContainsByPredicate(
					[&Request](const FHyperAIStudioDomainVariantDescriptor& Item)
					{
						return Item.ToolName == Request.ToolName
							&& Item.VariantId == Request.VariantId;
					});
				if (bOwnsVariant)
				{
					if (Found)
					{
						OutError = TEXT("Multiple registered adapters ambiguously own the exact variant.");
						SetDiagnostic(OutResolved.Status, TEXT("trusted_adapter_ambiguous"), OutError, true);
						return false;
					}
					Found = &Pair.Value;
				}
			}
			if (!Found)
			{
				OutError = TEXT("No exact catalog-authorized adapter is registered for this pack.");
				SetDiagnostic(OutResolved.Status, TEXT("trusted_adapter_missing"), OutError, true);
				return false;
			}
			Adapter = *Found;
			HostGeneration = State->HostGeneration;
		}
		if (Adapter.CatalogGeneration != OutResolved.Catalog.Generation
			|| Adapter.CatalogFingerprint != OutResolved.Catalog.Catalog.GeneratedFingerprint
			|| Adapter.Admission != Tool->AdmissionState
			|| Adapter.AtomicCohortId != Tool->AtomicCohortId)
		{
			OutError = TEXT("Exact adapter registration is stale against the current generated catalog authority.");
			SetDiagnostic(OutResolved.Status, TEXT("trusted_adapter_catalog_drift"), OutError, false, true);
			return false;
		}
		const FHyperAIStudioDomainVariantDescriptor* Variant =
			Adapter.Descriptor.Variants.FindByPredicate([&Request](const auto& Item)
			{
				return Item.ToolName == Request.ToolName && Item.VariantId == Request.VariantId;
			});
		if (!Variant || Variant->Safety != Request.Safety)
		{
			OutError = TEXT("Requested safety is not the exact registered typed variant.");
			SetDiagnostic(OutResolved.Status, TEXT("exact_safety_variant_missing"), OutError, false, true);
			return false;
		}

			if (!SamplePackPrerequisites(State, OutResolved.Catalog, *Pack,
					Adapter.RequiredNonBlockingRequirementGroupIds,
				OutResolved.Binding.Prerequisites, OutResolved.Status, OutError))
		{
			return false;
		}
		OutResolved.bPrerequisitesReady = OutResolved.Binding.Prerequisites.bPackEnabled;

		const uint64 RegistryGeneration = State->Registry->GetRegistryEpoch();
		const int64 ObservedMs = State->Clock->NowMonotonicMs();
		if (RegistryGeneration == 0 || ObservedMs < 0
			|| ObservedMs > MAX_int64 - FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs)
		{
			OutError = TEXT("Trusted authority generation or monotonic observation is invalid.");
			SetDiagnostic(OutResolved.Status, TEXT("trusted_authority_sample_invalid"), OutError, true);
			return false;
		}
		FHyperAIStudioDomainTrustSeal TrustSeal;
		TrustSeal.SchemaVersion = 2;
		TrustSeal.CanonicalProjectId = State->CanonicalProjectId;
		TrustSeal.CatalogFingerprint = OutResolved.Catalog.Catalog.GeneratedFingerprint;
		TrustSeal.ApprovedPlanFingerprint = OutResolved.Catalog.Catalog.ApprovedPlanFingerprint;
		TrustSeal.AdmissionMatrixFingerprint = OutResolved.Catalog.Catalog.AdmissionMatrixFingerprint;
		TrustSeal.SourceLedgerFingerprint = OutResolved.Catalog.Catalog.SourceLedgerFingerprint;
		TrustSeal.SourceArtifactFingerprint = OutResolved.Catalog.Catalog.SourceArtifactFingerprint;
		TrustSeal.AtomicCohortId = Adapter.AtomicCohortId;
		TrustSeal.CohortSourceArtifactFingerprint = Adapter.CohortSourceArtifactFingerprint;
		TrustSeal.CatalogGeneration = OutResolved.Catalog.Generation;
		TrustSeal.AdapterRegistryGeneration = RegistryGeneration;
		TrustSeal.ObservedMonotonicMs = ObservedMs;
		TrustSeal.ExpiresMonotonicMs = ObservedMs
			+ FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs;
		TrustSeal.bPreAdmissionEvidence = OutResolved.bPreAdmissionEvidenceExecution;
		OutResolved.Binding.Prerequisites.TrustSeal = TrustSeal;
		OutResolved.Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				OutResolved.Binding.Prerequisites);

		FHyperAIStudioDomainAdmissionSnapshot Admission;
		Admission.PackId = Request.PackId;
		Admission.bPackAdmitted = OutResolved.Admission
			== EHyperAIStudioCapabilityAdmissionState::Admitted
			|| OutResolved.bPreAdmissionEvidenceExecution;
		if (Admission.bPackAdmitted)
		{
			switch (Request.Safety)
			{
			case EHyperAIStudioDomainSafety::Read: Admission.bReadAdmitted = true; break;
			case EHyperAIStudioDomainSafety::Edit: Admission.bEditAdmitted = true; break;
			case EHyperAIStudioDomainSafety::Destructive: Admission.bDestructiveAdmitted = true; break;
			case EHyperAIStudioDomainSafety::ExternalEffect: Admission.bExternalEffectAdmitted = true; break;
			default: break;
			}
		}
		Admission.Revision = OutResolved.Catalog.Generation;
		Admission.TrustSeal = TrustSeal;
		Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Admission);

		OutResolved.Binding.PackId = Request.PackId;
		OutResolved.Binding.ToolName = Request.ToolName;
		OutResolved.Binding.VariantId = Request.VariantId;
		OutResolved.Binding.ExpectedSafety = Request.Safety;
		OutResolved.Binding.CanonicalProjectId = State->CanonicalProjectId;
		OutResolved.Binding.ExpectedAdapterFingerprint = Adapter.Descriptor.AdapterFingerprint;
		OutResolved.Binding.ExpectedAdapterGeneration = Adapter.Handle.AdapterGeneration;
		OutResolved.Binding.ExpectedRegistryEpoch = RegistryGeneration;
		OutResolved.Binding.Admission = Admission;
		OutResolved.AdapterDescriptor = Adapter.Descriptor;
		{
			FScopeLock Lock(&State->Mutex);
			const FRegisteredAdapterRecord* Current =
				State->RegisteredAdapters.Find(Adapter.Descriptor.AdapterFingerprint);
			if (!State->bStarted || State->bShuttingDown || State->HostGeneration != HostGeneration
				|| !Current || Current->Handle.AdapterGeneration != Adapter.Handle.AdapterGeneration
				|| Current->Handle.AdapterId != Adapter.Handle.AdapterId
				|| Current->Descriptor.AdapterFingerprint != Adapter.Descriptor.AdapterFingerprint
				|| Current->AtomicCohortId != Adapter.AtomicCohortId
				|| Current->CohortSourceArtifactFingerprint
					!= Adapter.CohortSourceArtifactFingerprint
				|| State->Registry->GetRegistryEpoch() != RegistryGeneration)
			{
				OutError = TEXT("Adapter registry changed while the trusted binding was sealed.");
				SetDiagnostic(OutResolved.Status, TEXT("adapter_registry_drift"), OutError, true);
				return false;
			}
		}
		if (!OutResolved.bPrerequisitesReady)
		{
			return false;
		}
		const bool bAdmitted = OutResolved.Admission
			== EHyperAIStudioCapabilityAdmissionState::Admitted;
		SetDiagnostic(OutResolved.Status,
			bAdmitted ? TEXT("trusted_binding_ready")
				: (OutResolved.bPreAdmissionEvidenceExecution
					? TEXT("source_candidate_preview_ready") : TEXT("staged_backend_required")),
			bAdmitted
				? TEXT("Exact generated prerequisites and safety admission are sealed.")
				: (OutResolved.bPreAdmissionEvidenceExecution
					? TEXT("Exact SourceCandidate cohort is enabled by the Preview channel or legacy test override.")
					: TEXT("SourceCandidate is available for dry-run only; enable Preview or use an admitted backend for mutation.")),
			false, !bAdmitted && !OutResolved.bPreAdmissionEvidenceExecution);
		return true;
	}

	bool ValidateExpectedCurrent(
		const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& State,
		const FTrustedPreparationRecord& Expected,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError)
	{
		OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
		OutError.Reset();
		const int64 NowMs = State->Clock->NowMonotonicMs();
		const FHyperAIStudioDomainTrustSeal& ExpectedSeal =
			Expected.Prepared.Contract.Binding.Prerequisites.TrustSeal;
		if (ExpectedSeal.SchemaVersion != 2 || NowMs >= ExpectedSeal.ExpiresMonotonicMs)
		{
			OutError = TEXT("The core prerequisite seal is stale and must be prepared again.");
			SetDiagnostic(OutStatus, TEXT("prerequisite_snapshot_stale"), OutError, true);
			return false;
		}
		FResolvedAuthority Current;
		if (!ResolveAuthority(State, Expected.Request, Current, OutError))
		{
			OutStatus = Current.Status;
			return false;
		}
		const FHyperAIStudioDomainBinding& ExpectedBinding = Expected.Prepared.Contract.Binding;
		if (Current.Admission != Expected.Admission
			|| Current.bPreAdmissionEvidenceExecution != Expected.bPreAdmissionEvidenceExecution
			|| Current.Catalog.Generation != ExpectedSeal.CatalogGeneration
			|| Current.Catalog.Catalog.GeneratedFingerprint != ExpectedSeal.CatalogFingerprint
			|| Current.Catalog.Catalog.ApprovedPlanFingerprint != ExpectedSeal.ApprovedPlanFingerprint
			|| Current.Catalog.Catalog.AdmissionMatrixFingerprint
				!= ExpectedSeal.AdmissionMatrixFingerprint
			|| Current.Catalog.Catalog.SourceLedgerFingerprint != ExpectedSeal.SourceLedgerFingerprint
			|| Current.Catalog.Catalog.SourceArtifactFingerprint != ExpectedSeal.SourceArtifactFingerprint
			|| Current.Binding.Prerequisites.TrustSeal.AtomicCohortId
				!= ExpectedSeal.AtomicCohortId
			|| Current.Binding.Prerequisites.TrustSeal.CohortSourceArtifactFingerprint
				!= ExpectedSeal.CohortSourceArtifactFingerprint)
		{
			OutError = TEXT("Generated catalog, admission, or source authority changed after preparation.");
			SetDiagnostic(OutStatus, TEXT("catalog_admission_source_drift"), OutError, false, true);
			return false;
		}
		if (Current.Binding.CanonicalProjectId != ExpectedBinding.CanonicalProjectId
			|| Current.Binding.ExpectedAdapterFingerprint != ExpectedBinding.ExpectedAdapterFingerprint
			|| Current.Binding.ExpectedAdapterGeneration != ExpectedBinding.ExpectedAdapterGeneration)
		{
			OutError = TEXT("Canonical project or exact adapter generation changed after preparation.");
			SetDiagnostic(OutStatus, TEXT("project_or_adapter_binding_drift"), OutError, true);
			return false;
		}
		if (!SameSemanticPrerequisites(Current.Binding.Prerequisites, ExpectedBinding.Prerequisites))
		{
			OutError = TEXT("Live prerequisite observations changed after preparation.");
			SetDiagnostic(OutStatus, TEXT("prerequisite_observation_drift"), OutError, true);
			return false;
		}
		if ((Expected.Admission != EHyperAIStudioCapabilityAdmissionState::Admitted
				&& !Expected.bPreAdmissionEvidenceExecution)
			|| !ExpectedBinding.Admission.bPackAdmitted)
		{
			OutError = TEXT("Only an exact Admitted safety variant or explicit non-shipping evidence cohort can enter staging.");
			SetDiagnostic(OutStatus, TEXT("source_candidate_execution_denied"), OutError, false, true);
			return false;
		}
		SetDiagnostic(OutStatus, TEXT("trusted_revalidation_succeeded"),
			TEXT("Catalog, prerequisite, project and adapter authority remain exact."));
		return true;
	}

	class FTrustedStateGate final : public IHyperAIStudioTypedArtifactStateGate
	{
	public:
		FTrustedStateGate(
			const TSharedRef<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>& InHost,
			const TSharedRef<const FTrustedPreparationRecord, ESPMode::ThreadSafe>& InExpected)
			: Host(InHost), Expected(InExpected)
		{
		}

		virtual bool Revalidate(
			const FHyperAIStudioPreparedTypedArtifact& Prepared,
			const IHyperAIStudioTypedArtifactPayload& Payload,
			EHyperAIStudioDomainExecutionActionKind,
			FHyperAIStudioDomainPrerequisiteSnapshot& OutPrerequisites,
			FHyperAIStudioDomainAdmissionSnapshot& OutAdmission,
			FString& OutError) override
		{
			const TSharedPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Pinned = Host.Pin();
			if (!Pinned.IsValid() || Prepared.ContractFingerprint != Expected->Prepared.ContractFingerprint
				|| Payload.GetTypeId() != Expected->Prepared.Contract.ArtifactTypeId
				|| Payload.GetSchemaFingerprint()
					!= Expected->Prepared.Contract.ArtifactSchemaFingerprint
				|| Payload.GetSemanticFingerprint()
					!= Expected->Prepared.Contract.ArtifactSemanticFingerprint)
			{
				OutError = TEXT("Trusted state gate received a different prepared artifact or typed payload.");
				FHyperAIStudioTrustedExecutionDiagnostic Status;
				SetDiagnostic(Status, TEXT("trusted_gate_binding_mismatch"), OutError, true);
				SetLast(Status);
				return false;
			}
			FHyperAIStudioTrustedExecutionDiagnostic Status;
			if (!ValidateExpectedCurrent(Pinned.ToSharedRef(), *Expected, Status, OutError))
			{
				SetLast(Status);
				return false;
			}
			OutPrerequisites = Expected->Prepared.Contract.Binding.Prerequisites;
			OutAdmission = Expected->Prepared.Contract.Binding.Admission;
			SetLast(Status);
			return true;
		}

		virtual bool VerifyFresh(
			const FHyperAIStudioPreparedTypedArtifact& Prepared,
			const IHyperAIStudioTypedArtifactPayload& Payload,
			const IHyperAIStudioDomainResultPayload& AdapterResult,
			FString& OutPostconditionHash,
			FString& OutError) override
		{
			OutPostconditionHash.Reset();
			const TSharedPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Pinned = Host.Pin();
			FString DomainPostconditionHash;
			if (!Pinned.IsValid()
				|| Prepared.ContractFingerprint != Expected->Prepared.ContractFingerprint
				|| Payload.GetTypeId() != Expected->Prepared.Contract.ArtifactTypeId
				|| Payload.GetSchemaFingerprint()
					!= Expected->Prepared.Contract.ArtifactSchemaFingerprint
				|| Payload.GetSemanticFingerprint()
					!= Expected->Prepared.Contract.ArtifactSemanticFingerprint
				|| !Expected->FreshVerifier.IsValid()
				|| !Expected->FreshVerifier->VerifyFreshExact(
					Payload, AdapterResult, DomainPostconditionHash, OutError)
				|| !IsSha256(DomainPostconditionHash))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Domain fresh verification did not return an exact bounded SHA-256 receipt.");
				}
				FHyperAIStudioTrustedExecutionDiagnostic Status;
				SetDiagnostic(Status, TEXT("fresh_verification_failed"), OutError, true);
				SetLast(Status);
				return false;
			}
			FHyperAIStudioTrustedExecutionDiagnostic RevalidatedStatus;
			if (!ValidateExpectedCurrent(
					Pinned.ToSharedRef(), *Expected, RevalidatedStatus, OutError))
			{
				SetLast(RevalidatedStatus);
				return false;
			}
			const FHyperAIStudioDomainBinding& Binding = Expected->Prepared.Contract.Binding;
			const FHyperAIStudioDomainTrustSeal& Seal = Binding.Prerequisites.TrustSeal;
			OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
				TEXT("hyperai.trusted-fresh-receipt.v1\n")
				+ DomainPostconditionHash + TEXT("\n")
				+ Expected->Prepared.ContractFingerprint + TEXT("\n")
				+ Binding.ExpectedAdapterFingerprint + TEXT("\n")
				+ Seal.SourceArtifactFingerprint + TEXT("\n")
				+ Seal.AtomicCohortId + TEXT("\n")
				+ Seal.CohortSourceArtifactFingerprint + TEXT("\n")
				+ Seal.SourceLedgerFingerprint + TEXT("\n")
				+ Expected->Prepared.Contract.ArtifactSemanticFingerprint);
			if (!IsSha256(OutPostconditionHash))
			{
				OutError = TEXT("Core could not bind the fresh receipt to the exact admitted source artifact.");
				return false;
			}
			{
				FScopeLock Lock(&Pinned->Mutex);
				if (!Pinned->bStarted || Pinned->bShuttingDown
					|| Pinned->HostGeneration != Expected->HostGeneration)
				{
					OutError = TEXT("Trusted host lifecycle changed during fresh verification.");
					return false;
				}
			}
			return true;
		}

		FHyperAIStudioTrustedExecutionDiagnostic GetLast() const
		{
			FScopeLock Lock(&LastMutex);
			return LastStatus;
		}

	private:
		void SetLast(const FHyperAIStudioTrustedExecutionDiagnostic& Status)
		{
			FScopeLock Lock(&LastMutex);
			LastStatus = Status;
		}

		TWeakPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> Host;
		TSharedRef<const FTrustedPreparationRecord, ESPMode::ThreadSafe> Expected;
		mutable FCriticalSection LastMutex;
		FHyperAIStudioTrustedExecutionDiagnostic LastStatus;
	};

	bool ValidateAdapterAgainstCatalog(
		const FHyperAIStudioTrustedCatalogAuthoritySnapshot& CatalogSnapshot,
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		bool bAllowSourceCandidate,
		EHyperAIStudioCapabilityAdmissionState& OutAdmission,
		FString& OutAtomicCohortId,
		FString& OutCohortSourceArtifactFingerprint,
		TArray<FString>& OutRequiredNonBlockingRequirementGroupIds,
		FString& OutError)
	{
		const FHyperAIStudioCapabilityPackDefinition* Pack =
			FindPack(CatalogSnapshot.Catalog, Descriptor.PackId);
		if (!Pack || Pack->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Planned)
		{
			OutError = TEXT("Generated catalog does not authorize registration for this pack.");
			return false;
		}
		if (Pack->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			&& !bAllowSourceCandidate)
		{
			OutError = TEXT("SourceCandidate adapter registration requires the Preview channel or legacy test override.");
			return false;
		}
		TSet<FString> AdapterTools;
		TSet<FString> Cohorts;
		for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
		{
			const FHyperAIStudioCapabilityToolDefinition* Tool = FindTool(
				CatalogSnapshot.Catalog, Variant.ToolName);
			if (!Tool || Tool->PackId != Descriptor.PackId
				|| Tool->AdmissionState != Pack->AdmissionState
				|| !Pack->AtomicCohortIds.Contains(Tool->AtomicCohortId)
				|| !IsGeneratedSafetyAllowed(*Tool, Variant.Safety))
			{
				OutError = TEXT("Adapter variant safety is outside the exact generated tool-bound safety authority.");
				return false;
			}
			AdapterTools.Add(Variant.ToolName);
			Cohorts.Add(Tool->AtomicCohortId);
		}
		if (Cohorts.Num() != 1)
		{
			OutError = TEXT("One adapter registration must match exactly one generated atomic cohort.");
			return false;
		}
		const FString CohortId = Cohorts.Array()[0];
		TSet<FString> DeclaredLimitedGroups;
		for (const FString& GroupId : Descriptor.ApplicableNonBlockingRequirementGroupIds)
		{
			const FHyperAIStudioCapabilityRequirementGroup* Group = Pack->Requirements.FindByPredicate(
				[&GroupId](const FHyperAIStudioCapabilityRequirementGroup& Item)
				{
					return Item.Id == GroupId;
				});
			if (!Group || Group->bBlocking || DeclaredLimitedGroups.Contains(GroupId))
			{
				OutError = TEXT("Adapter limited requirement-group binding is not exact for its generated pack.");
				return false;
			}
			DeclaredLimitedGroups.Add(GroupId);
		}
		TSet<FString> CatalogTools;
		TSet<FString> GeneratedLimitedGroups;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : CatalogSnapshot.Catalog.Tools)
		{
			if (Tool.PackId == Descriptor.PackId && Tool.AtomicCohortId == CohortId)
			{
				CatalogTools.Add(Tool.Name);
				for (const FString& GroupId : Tool.RequiredNonBlockingRequirementGroupIds)
				{
					GeneratedLimitedGroups.Add(GroupId);
				}
			}
		}
		if (DeclaredLimitedGroups.Difference(GeneratedLimitedGroups).Num() > 0
			|| GeneratedLimitedGroups.Difference(DeclaredLimitedGroups).Num() > 0)
		{
			OutError = TEXT("Adapter limited requirement-group assertion differs from generated cohort authority.");
			return false;
		}
		if (AdapterTools.Num() != CatalogTools.Num())
		{
			OutError = TEXT("Adapter tool set does not cover its generated atomic cohort exactly.");
			return false;
		}
		for (const FString& Tool : CatalogTools)
		{
			if (!AdapterTools.Contains(Tool))
			{
				OutError = TEXT("Adapter tool set does not cover its generated atomic cohort exactly.");
				return false;
			}
		}
		TArray<FString> SortedTools = CatalogTools.Array();
		SortedTools.Sort();
		FString CohortSourceCanonical = TEXT("hyperai.cohort-source-artifacts.v2\n") + CohortId;
		TArray<FString> SortedLimitedGroups = GeneratedLimitedGroups.Array();
		SortedLimitedGroups.Sort();
		for (const FString& GroupId : SortedLimitedGroups)
		{
			CohortSourceCanonical += TEXT("\nrequirement-group\n") + GroupId;
		}
		for (const FString& ToolName : SortedTools)
		{
			const FHyperAIStudioCapabilityToolDefinition* Tool =
				FindTool(CatalogSnapshot.Catalog, ToolName);
			if (!Tool || !IsSha256(Tool->SourceArtifactFingerprint))
			{
				OutError = TEXT("Generated cohort tool lacks an exact source-artifact fingerprint.");
				return false;
			}
			CohortSourceCanonical += TEXT("\n") + ToolName + TEXT("\n")
				+ Tool->SourceArtifactFingerprint + TEXT("\n")
				+ FString::FromInt(Tool->SourceArtifactCount);
		}
		OutAtomicCohortId = CohortId;
		OutRequiredNonBlockingRequirementGroupIds = SortedLimitedGroups;
		OutCohortSourceArtifactFingerprint =
			FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(CohortSourceCanonical);
		if (!IsSha256(OutCohortSourceArtifactFingerprint))
		{
			OutError = TEXT("Exact cohort source-artifact seal could not be computed.");
			return false;
		}
		OutAdmission = Pack->AdmissionState;
		return true;
	}
}

FHyperAIStudioTrustedExecutionHost::FHyperAIStudioTrustedExecutionHost(
	const FString& TrustedProjectRoot,
	const TSharedRef<IHyperAIStudioTrustedCatalogAuthority, ESPMode::ThreadSafe>& CatalogAuthority,
	const TSharedRef<IHyperAIStudioTrustedPrerequisiteEnvironment, ESPMode::ThreadSafe>& Environment,
	const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock,
	TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> AuthorizationGate)
	: State(MakeShared<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe>(
		TrustedProjectRoot, CatalogAuthority, Environment, Clock, MoveTemp(AuthorizationGate)))
{
}

FHyperAIStudioTrustedExecutionHost::~FHyperAIStudioTrustedExecutionHost()
{
	Shutdown();
}

bool FHyperAIStudioTrustedExecutionHost::Startup(FString& OutError)
{
	OutError.Reset();
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bStarted && !State->bShuttingDown)
		{
			return true;
		}
		if (State->bShuttingDown || State->CanonicalProjectId.IsEmpty())
		{
			OutError = TEXT("Trusted execution host cannot start without a quiescent canonical project.");
			return false;
		}
	}
	FHyperAIStudioTrustedCatalogAuthoritySnapshot Catalog;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	if (!State->CatalogAuthority->Snapshot(Catalog, OutError)
		|| !HyperAIStudio::TrustedExecution::Private::ValidateCatalogSnapshot(Catalog, Status, OutError))
	{
		return false;
	}
	if (!State->Service->Startup(OutError))
	{
		return false;
	}
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShuttingDown)
		{
			OutError = TEXT("Trusted execution host quiesced during startup.");
		}
		else
		{
			State->bStarted = true;
			++State->HostGeneration;
			return true;
		}
	}
	State->Service->Shutdown();
	return false;
}

void FHyperAIStudioTrustedExecutionHost::Shutdown()
{
	bool bCallService = false;
	bool bStageCallsDrained = false;
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bShuttingDown)
		{
			State->bShuttingDown = true;
			++State->HostGeneration;
			bCallService = State->ActiveStageCalls == 0;
		}
		bStageCallsDrained = State->ActiveStageCalls == 0;
	}
	if (!bStageCallsDrained)
	{
		return;
	}
	if (bCallService || !State->Service->CanShutdownSafely())
	{
		State->Service->Shutdown();
	}
	if (!State->Service->CanShutdownSafely())
	{
		return;
	}
	if (State->Authorization.CoreIssuer.IsValid())
	{
		State->Authorization.CoreIssuer->Shutdown();
	}
	TMap<FString, HyperAIStudio::TrustedExecution::Private::FRegisteredProbeRecord> ReleasedProbes;
	TMap<FString, HyperAIStudio::TrustedExecution::Private::FTrustedStageContext> ReleasedStages;
	TMap<FString, TSharedPtr<HyperAIStudio::TrustedExecution::Private::FTrustedPreparationRecord,
		ESPMode::ThreadSafe>> ReleasedPreparations;
	{
		FScopeLock Lock(&State->Mutex);
		State->bStarted = false;
		ReleasedProbes = MoveTemp(State->RegisteredProbes);
		ReleasedStages = MoveTemp(State->StageContexts);
		ReleasedPreparations = MoveTemp(State->Preparations);
		State->DuplicateProbeIds.Reset();
		State->AdapterReservations.Reset();
		State->StageReservations.Reset();
	}
}

bool FHyperAIStudioTrustedExecutionHost::IsAvailable() const
{
	FScopeLock Lock(&State->Mutex);
	return State->bStarted && !State->bShuttingDown;
}

bool FHyperAIStudioTrustedExecutionHost::CanShutdownSafely() const
{
	HyperAIStudio::TrustedExecution::Private::ReconcileTrustedOwnedState(State.ToSharedRef());
	FScopeLock Lock(&State->Mutex);
	bool bAdapterPinsDrained = true;
	for (const TPair<FString, HyperAIStudio::TrustedExecution::Private::FRegisteredAdapterRecord>& Pair
		: State->RegisteredAdapters)
	{
		bAdapterPinsDrained &= Pair.Value.ActiveTrustedPins == 0;
	}
	return State->ActiveStageCalls == 0 && bAdapterPinsDrained
		&& State->Service->CanShutdownSafely();
}

bool FHyperAIStudioTrustedExecutionHost::RegisterAdapter(
	const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
	FHyperAIStudioDomainRegistrationHandle& OutHandle,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutHandle = FHyperAIStudioDomainRegistrationHandle{};
	OutError.Reset();
	ReconcileTrustedOwnedState(State.ToSharedRef());
	const FHyperAIStudioDomainAdapterDescriptor Descriptor = Adapter->GetDescriptor();
	FHyperAIStudioTrustedCatalogAuthoritySnapshot Catalog;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	if (!SnapshotCatalog(State.ToSharedRef(), Catalog, Status, OutError))
	{
		return false;
	}
	EHyperAIStudioCapabilityAdmissionState Admission;
	FString AtomicCohortId;
	FString CohortSourceArtifactFingerprint;
	TArray<FString> RequiredNonBlockingRequirementGroupIds;
	if (!ValidateAdapterAgainstCatalog(
			Catalog, Descriptor, State->Environment->AreSourceCandidateToolsEnabled(),
			Admission, AtomicCohortId, CohortSourceArtifactFingerprint,
			RequiredNonBlockingRequirementGroupIds, OutError))
	{
		return false;
	}
	if (!Descriptor.ReplacesAdapterFingerprint.IsEmpty())
	{
		OutError = TEXT("Trusted cohort replacement requires explicit unregister followed by registration.");
		return false;
	}
	const FString ReservationKey = Descriptor.PackId + TEXT("\n") + Descriptor.AdapterId;
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bStarted || State->bShuttingDown
			|| State->AdapterReservations.Contains(ReservationKey))
		{
			OutError = TEXT("Adapter identity already has an in-progress trusted reservation.");
			return false;
		}
		for (const TPair<FString, FRegisteredAdapterRecord>& Pair : State->RegisteredAdapters)
		{
			if ((Pair.Value.Descriptor.PackId == Descriptor.PackId
					&& Pair.Value.Descriptor.AdapterId == Descriptor.AdapterId)
				|| Pair.Value.Descriptor.AdapterFingerprint == Descriptor.AdapterFingerprint)
			{
				OutError = TEXT("Adapter identity already has an exact trusted registration.");
				return false;
			}
			for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
			{
				if (Pair.Value.Descriptor.Variants.ContainsByPredicate(
					[&Variant](const FHyperAIStudioDomainVariantDescriptor& ExistingVariant)
					{
						return ExistingVariant.ToolName == Variant.ToolName;
					}))
				{
					OutError = TEXT("Adapter tool ownership overlaps an existing trusted cohort.");
					return false;
				}
			}
		}
		State->AdapterReservations.Add(ReservationKey);
	}
	FHyperAIStudioDomainRegistrationHandle RegisteredHandle;
	const bool bRegistered = State->Registry->RegisterAdapter(Adapter, RegisteredHandle, OutError);
	FHyperAIStudioTrustedCatalogAuthoritySnapshot RecheckedCatalog;
	FString RecheckError;
	const bool bAuthorityUnchanged = bRegistered
		&& State->CatalogAuthority->Snapshot(RecheckedCatalog, RecheckError)
		&& RecheckedCatalog.Generation == Catalog.Generation
		&& RecheckedCatalog.Catalog.GeneratedFingerprint == Catalog.Catalog.GeneratedFingerprint
		&& RegisteredHandle.AdapterId == Descriptor.AdapterId
		&& RegisteredHandle.AdapterFingerprint == Descriptor.AdapterFingerprint;
	bool bKeep = false;
	{
		FScopeLock Lock(&State->Mutex);
		State->AdapterReservations.Remove(ReservationKey);
		bool bFinalCollision = false;
		for (const TPair<FString, FRegisteredAdapterRecord>& Pair : State->RegisteredAdapters)
		{
			bFinalCollision |= (Pair.Value.Descriptor.PackId == Descriptor.PackId
					&& Pair.Value.Descriptor.AdapterId == Descriptor.AdapterId)
				|| Pair.Value.Descriptor.AdapterFingerprint == Descriptor.AdapterFingerprint;
			for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
			{
				bFinalCollision |= Pair.Value.Descriptor.Variants.ContainsByPredicate(
					[&Variant](const FHyperAIStudioDomainVariantDescriptor& ExistingVariant)
					{
						return ExistingVariant.ToolName == Variant.ToolName;
					});
			}
		}
		if (bRegistered && bAuthorityUnchanged && State->bStarted && !State->bShuttingDown
			&& !bFinalCollision
			&& !State->RegisteredAdapters.Contains(Descriptor.AdapterFingerprint))
		{
			FRegisteredAdapterRecord Record;
			Record.Descriptor = Descriptor;
			Record.Handle = RegisteredHandle;
			Record.Adapter = Adapter;
			Record.CatalogGeneration = Catalog.Generation;
			Record.CatalogFingerprint = Catalog.Catalog.GeneratedFingerprint;
			Record.Admission = Admission;
			Record.AtomicCohortId = AtomicCohortId;
			Record.CohortSourceArtifactFingerprint = CohortSourceArtifactFingerprint;
			Record.RequiredNonBlockingRequirementGroupIds = RequiredNonBlockingRequirementGroupIds;
			State->RegisteredAdapters.Add(Descriptor.AdapterFingerprint, MoveTemp(Record));
			bKeep = true;
		}
	}
	if (!bKeep && bRegistered)
	{
		FString Ignore;
		State->Registry->UnregisterAdapter(RegisteredHandle, Ignore);
	}
	if (!bKeep)
	{
		if (OutError.IsEmpty())
		{
				OutError = bAuthorityUnchanged
					? TEXT("Trusted host lifecycle or exact adapter ownership changed during registration.")
				: TEXT("Catalog authority or adapter descriptor changed during registration.");
		}
		return false;
	}
	OutHandle = RegisteredHandle;
	return true;
}

EHyperAIStudioDomainUnregisterResult FHyperAIStudioTrustedExecutionHost::UnregisterAdapter(
	const FHyperAIStudioDomainRegistrationHandle& Handle,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutError.Reset();
	ReconcileTrustedOwnedState(State.ToSharedRef());
	const FString ReservationKey = Handle.PackId + TEXT("\n") + Handle.AdapterId;
	{
		FScopeLock Lock(&State->Mutex);
		const FRegisteredAdapterRecord* Existing =
			State->RegisteredAdapters.Find(Handle.AdapterFingerprint);
		if (!Existing)
		{
			OutError = TEXT("Trusted adapter registration was not found.");
			return EHyperAIStudioDomainUnregisterResult::NotFound;
		}
		if (Existing->Handle.AdapterGeneration != Handle.AdapterGeneration
			|| Existing->Handle.AdapterId != Handle.AdapterId
			|| Existing->Handle.AdapterFingerprint != Handle.AdapterFingerprint)
		{
			OutError = TEXT("Trusted adapter unregister handle is stale.");
			return EHyperAIStudioDomainUnregisterResult::StaleHandle;
		}
		if (State->AdapterReservations.Contains(ReservationKey))
		{
			OutError = TEXT("Trusted adapter registration is busy.");
			return EHyperAIStudioDomainUnregisterResult::Busy;
		}
		if (Existing->ActiveTrustedPins > 0)
		{
			OutError = TEXT("Prepared or staged trusted artifacts still pin this optional adapter module.");
			return EHyperAIStudioDomainUnregisterResult::Busy;
		}
		for (const TPair<FString, FRegisteredProbeRecord>& Pair : State->RegisteredProbes)
		{
			if (Pair.Value.OwnerAdapter.PackId == Handle.PackId
				&& Pair.Value.OwnerAdapter.AdapterId == Handle.AdapterId
				&& Pair.Value.OwnerAdapter.AdapterFingerprint == Handle.AdapterFingerprint
				&& Pair.Value.OwnerAdapter.AdapterGeneration == Handle.AdapterGeneration)
			{
				OutError = TEXT("Owned cached live probes must be unregistered before their adapter module.");
				return EHyperAIStudioDomainUnregisterResult::Busy;
			}
		}
		State->AdapterReservations.Add(ReservationKey);
	}
	const EHyperAIStudioDomainUnregisterResult Result =
		State->Registry->UnregisterAdapter(Handle, OutError);
	{
		FScopeLock Lock(&State->Mutex);
		State->AdapterReservations.Remove(ReservationKey);
		if (Result == EHyperAIStudioDomainUnregisterResult::Removed)
		{
			State->RegisteredAdapters.Remove(Handle.AdapterFingerprint);
		}
	}
	return Result;
}

bool FHyperAIStudioTrustedExecutionHost::RegisterLiveProbe(
	const FHyperAIStudioDomainRegistrationHandle& OwnerAdapter,
	const FString& ProbeId,
	FHyperAIStudioTrustedProbeRegistrationHandle& OutHandle,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutHandle = FHyperAIStudioTrustedProbeRegistrationHandle{};
	OutError.Reset();
	if (!IsBoundedId(ProbeId, FHyperAIStudioTrustedExecutionLimits::MaxProbeIdChars, TEXT("probe.")))
	{
		OutError = TEXT("Live probe id is outside the closed bounded namespace.");
		return false;
	}
	FHyperAIStudioTrustedCatalogAuthoritySnapshot Catalog;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	if (!SnapshotCatalog(State.ToSharedRef(), Catalog, Status, OutError))
	{
		return false;
	}
	const FHyperAIStudioCapabilityPrerequisiteDefinition* Definition =
		FindPrerequisite(Catalog.Catalog, ProbeId);
	if (!Definition || Definition->Kind != EHyperAIStudioCapabilityPrerequisiteKind::Probe)
	{
		OutError = TEXT("Live probe id is not an exact generated catalog prerequisite.");
		return false;
	}
	const FHyperAIStudioCapabilityPackDefinition* OwnerPack =
		FindPack(Catalog.Catalog, OwnerAdapter.PackId);
	if (!OwnerPack)
	{
		OutError = TEXT("Live probe owner pack is not present in the generated catalog.");
		return false;
	}
	FScopeLock Lock(&State->Mutex);
	if (!State->bStarted || State->bShuttingDown)
	{
		OutError = TEXT("Trusted host is not accepting probe registrations.");
		return false;
	}
	const FRegisteredAdapterRecord* OwnerRecord =
		State->RegisteredAdapters.Find(OwnerAdapter.AdapterFingerprint);
	if (!OwnerRecord || OwnerRecord->Handle.AdapterGeneration != OwnerAdapter.AdapterGeneration
		|| OwnerRecord->Handle.AdapterId != OwnerAdapter.AdapterId
		|| OwnerRecord->Handle.AdapterFingerprint != OwnerAdapter.AdapterFingerprint)
	{
		OutError = TEXT("Live probe owner is not the exact registered catalog adapter.");
		return false;
	}
	const bool bOwnedRequirement = OwnerPack->Requirements.ContainsByPredicate(
		[&ProbeId, &OwnerRecord](const FHyperAIStudioCapabilityRequirementGroup& Group)
		{
			return Group.PrerequisiteIds.Contains(ProbeId)
				&& (Group.bBlocking
					|| OwnerRecord->RequiredNonBlockingRequirementGroupIds.Contains(Group.Id));
		});
	if (!bOwnedRequirement)
	{
		OutError = TEXT("Live probe is not an exact applicable requirement of the owning adapter cohort.");
		return false;
	}
	if (State->RegisteredProbes.Contains(ProbeId))
	{
		State->DuplicateProbeIds.Add(ProbeId);
		++State->ProbeRegistryGeneration;
		OutError = TEXT("Duplicate live probe registration fails closed for this prerequisite.");
		return false;
	}
	if (State->RegisteredProbes.Num() >= FHyperAIStudioTrustedExecutionLimits::MaxRegisteredProbes)
	{
		OutError = TEXT("Bounded trusted probe registry is full.");
		return false;
	}
	FRegisteredProbeRecord Record;
	Record.OwnerAdapter = OwnerAdapter;
	Record.Generation = State->NextProbeGeneration++;
	State->RegisteredProbes.Add(ProbeId, Record);
	State->DuplicateProbeIds.Remove(ProbeId);
	++State->ProbeRegistryGeneration;
	OutHandle.ProbeId = ProbeId;
	OutHandle.OwnerPackId = OwnerAdapter.PackId;
	OutHandle.OwnerAdapterId = OwnerAdapter.AdapterId;
	OutHandle.OwnerAdapterFingerprint = OwnerAdapter.AdapterFingerprint;
	OutHandle.OwnerAdapterGeneration = OwnerAdapter.AdapterGeneration;
	OutHandle.Generation = Record.Generation;
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::PublishLiveProbeExact(
	const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
	const FHyperAIStudioTrustedProbeResult& Observation,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutError.Reset();
	if (!Handle.IsValid()
		|| Observation.StatusCode.Len() > FHyperAIStudioDomainLimits::MaxStatusCodeChars
		|| Observation.Diagnostic.Len()
			> FHyperAIStudioTrustedExecutionLimits::MaxProbeDiagnosticChars)
	{
		OutError = TEXT("Cached live-probe observation is outside the exact bounded contract.");
		return false;
	}
	const int64 ObservedMonotonicMs = State->Clock->NowMonotonicMs();
	if (ObservedMonotonicMs < 0)
	{
		OutError = TEXT("Cached live-probe observation has an invalid monotonic timestamp.");
		return false;
	}
	FScopeLock Lock(&State->Mutex);
	FRegisteredProbeRecord* Existing = State->RegisteredProbes.Find(Handle.ProbeId);
	if (!State->bStarted || State->bShuttingDown || !Existing
		|| Existing->Generation != Handle.Generation
		|| Existing->OwnerAdapter.PackId != Handle.OwnerPackId
		|| Existing->OwnerAdapter.AdapterId != Handle.OwnerAdapterId
		|| Existing->OwnerAdapter.AdapterFingerprint != Handle.OwnerAdapterFingerprint
		|| Existing->OwnerAdapter.AdapterGeneration != Handle.OwnerAdapterGeneration)
	{
		OutError = TEXT("Cached live-probe publication handle is missing, stale, or quiescing.");
		return false;
	}
	Existing->CachedObservation = Observation;
	Existing->ObservedMonotonicMs = ObservedMonotonicMs;
	Existing->bHasObservation = true;
	++State->ProbeRegistryGeneration;
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::UnregisterLiveProbe(
	const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutError.Reset();
	{
		FScopeLock Lock(&State->Mutex);
		FRegisteredProbeRecord* Existing = State->RegisteredProbes.Find(Handle.ProbeId);
		if (!Existing)
		{
			OutError = TEXT("Live probe registration was not found.");
			return false;
		}
		if (!Handle.IsValid() || Existing->Generation != Handle.Generation)
		{
			OutError = TEXT("Live probe unregister handle is stale.");
			return false;
		}
		if (Existing->OwnerAdapter.PackId != Handle.OwnerPackId
			|| Existing->OwnerAdapter.AdapterId != Handle.OwnerAdapterId
			|| Existing->OwnerAdapter.AdapterFingerprint != Handle.OwnerAdapterFingerprint
			|| Existing->OwnerAdapter.AdapterGeneration != Handle.OwnerAdapterGeneration)
		{
			OutError = TEXT("Live probe owner binding is stale.");
			return false;
		}
		State->RegisteredProbes.Remove(Handle.ProbeId);
		State->DuplicateProbeIds.Remove(Handle.ProbeId);
		++State->ProbeRegistryGeneration;
	}
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::PrepareDryRun(
	const FHyperAIStudioTrustedArtifactRequest& Request,
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
	const TSharedRef<IHyperAIStudioTrustedFreshVerifier, ESPMode::ThreadSafe>& FreshVerifier,
	FHyperAIStudioTrustedPreparedArtifact& OutPrepared,
	FHyperAIStudioTrustedPrepareReport& OutReport,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutPrepared.Reset();
	OutReport = FHyperAIStudioTrustedPrepareReport{};
	OutError.Reset();
	ReconcileTrustedOwnedState(State.ToSharedRef());
	if (!IsBoundedId(Request.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
		|| !IsBoundedId(Request.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars, TEXT("hyper_"))
		|| !IsBoundedId(Request.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
		|| Request.Safety == EHyperAIStudioDomainSafety::Read
		|| static_cast<uint8>(Request.Safety)
			> static_cast<uint8>(EHyperAIStudioDomainSafety::ExternalEffect)
		|| !IsSha256(Request.ArtifactSemanticFingerprint))
	{
		OutError = TEXT("Trusted artifact intent is invalid or is not a mutating typed variant.");
		SetDiagnostic(OutReport.Status, TEXT("trusted_request_invalid"), OutError);
		return false;
	}
	FResolvedAuthority Resolved;
	if (!ResolveAuthority(State.ToSharedRef(), Request, Resolved, OutError))
	{
		OutReport.Admission = ToPublicAdmission(Resolved.Admission);
		OutReport.CatalogFingerprint = Resolved.Catalog.Catalog.GeneratedFingerprint;
		OutReport.CatalogGeneration = Resolved.Catalog.Generation;
		OutReport.Status = Resolved.Status;
		return false;
	}
	const FHyperAIStudioDomainVariantDescriptor* Variant =
		Resolved.AdapterDescriptor.Variants.FindByPredicate([&Request](const auto& Item)
		{
			return Item.ToolName == Request.ToolName && Item.VariantId == Request.VariantId;
		});
	if (!Variant)
	{
		OutError = TEXT("Exact catalog-authorized adapter variant disappeared before payload inspection.");
		SetDiagnostic(OutReport.Status, TEXT("typed_variant_binding_missing"), OutError, false, true);
		return false;
	}
	TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> PinnedAdapter;
	{
		FScopeLock Lock(&State->Mutex);
		FRegisteredAdapterRecord* Record =
			State->RegisteredAdapters.Find(Resolved.AdapterDescriptor.AdapterFingerprint);
		if (!State->bStarted || State->bShuttingDown || !Record
			|| Record->Handle.AdapterGeneration != Resolved.Binding.ExpectedAdapterGeneration)
		{
			OutError = TEXT("Exact optional adapter changed before payload callbacks could be pinned.");
			SetDiagnostic(OutReport.Status, TEXT("adapter_registry_drift"), OutError, true);
			return false;
		}
		PinnedAdapter = Record->Adapter.Pin();
		if (!PinnedAdapter.IsValid())
		{
			OutError = TEXT("Exact optional adapter lifetime was unavailable for a trusted pin.");
			SetDiagnostic(OutReport.Status, TEXT("adapter_lifetime_unavailable"), OutError, true);
			return false;
		}
		++Record->ActiveTrustedPins;
	}
	const TSharedRef<FTrustedAdapterModulePin, ESPMode::ThreadSafe> AdapterModulePin =
		MakeShared<FTrustedAdapterModulePin, ESPMode::ThreadSafe>(
			State.ToSharedRef(), Resolved.AdapterDescriptor.AdapterFingerprint,
			Resolved.Binding.ExpectedAdapterGeneration, PinnedAdapter.ToSharedRef());
	PinnedAdapter.Reset();
	if (Resolved.Admission == EHyperAIStudioCapabilityAdmissionState::Admitted
		|| Resolved.bPreAdmissionEvidenceExecution)
	{
		const FHyperAIStudioDomainResolveResult Ready = State->Registry->ResolveStatus(Resolved.Binding);
		const bool bSameGenerationPinned = Ready.State == EHyperAIStudioDomainState::Busy
			&& Ready.DiagnosticCode == TEXT("adapter_busy")
			&& Ready.AdapterFingerprint == Resolved.Binding.ExpectedAdapterFingerprint
			&& Ready.AdapterGeneration == Resolved.Binding.ExpectedAdapterGeneration
			&& Ready.RegistryEpoch == Resolved.Binding.ExpectedRegistryEpoch;
		if (Ready.State != EHyperAIStudioDomainState::Ready && !bSameGenerationPinned)
		{
			OutError = TEXT("Exact admitted adapter was not ready at preparation.");
			SetDiagnostic(OutReport.Status,
				Ready.DiagnosticCode.IsEmpty() ? TEXT("trusted_adapter_not_ready") : Ready.DiagnosticCode,
				OutError, true);
			return false;
		}
	}
	const FString SourceType = Payload->GetTypeId();
	const FString SourceSchema = Payload->GetSchemaFingerprint();
	const FString SourceSemantic = Payload->GetSemanticFingerprint();
	const int32 SourceBytes = Payload->GetBoundedByteSize();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Immutable =
		Payload->CloneImmutable();
	if (&Immutable.Get() == &Payload.Get()
		|| SourceType != Immutable->GetTypeId()
		|| SourceSchema != Immutable->GetSchemaFingerprint()
		|| SourceSemantic != Immutable->GetSemanticFingerprint()
		|| SourceBytes != Immutable->GetBoundedByteSize()
		|| SourceSemantic != Request.ArtifactSemanticFingerprint
		|| SourceBytes < 0 || SourceBytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		OutError = TEXT("Typed payload did not produce an exact detached immutable semantic snapshot.");
		SetDiagnostic(OutReport.Status, TEXT("typed_payload_clone_invalid"), OutError);
		return false;
	}
	FString CanonicalEffectTarget;
	if (FreshVerifier->GetOwnerAdapterFingerprint()
			!= Resolved.AdapterDescriptor.AdapterFingerprint
		|| !FreshVerifier->ResolveCanonicalEffectTarget(
			*Immutable, CanonicalEffectTarget, OutError)
		|| CanonicalEffectTarget != Request.EffectTarget
		|| CanonicalEffectTarget.IsEmpty()
		|| CanonicalEffectTarget.Len() > FHyperAIStudioTypedArtifactLimits::MaxEffectTargetChars)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Caller effect target did not match the domain-owned canonical typed target.");
		}
		SetDiagnostic(OutReport.Status, TEXT("canonical_effect_target_mismatch"), OutError);
		return false;
	}

	if (!Variant || Variant->RequestTypeId != SourceType
		|| Variant->RequestSchemaFingerprint != SourceSchema)
	{
		OutError = TEXT("Immutable payload does not match the exact catalog-authorized adapter variant.");
		SetDiagnostic(OutReport.Status, TEXT("typed_payload_binding_mismatch"), OutError, false, true);
		return false;
	}

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Resolved.Binding;
	Contract.ArtifactTypeId = SourceType;
	Contract.ArtifactSchemaFingerprint = SourceSchema;
	Contract.ArtifactSemanticFingerprint = SourceSemantic;
	Contract.EffectTarget = CanonicalEffectTarget;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = Request.MaxNativeOperations;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = Request.MaxResultBytes;
	Contract.StageLifetimeMs = Request.StageLifetimeMs;
	Contract.bCompileOnce = Request.bCompileOnce;
	Contract.bSaveOnce = Request.bSaveOnce;
	Contract.bValidateOnce = Request.bValidateOnce;
	Contract.bVerifyFreshOnce = Request.bVerifyFreshOnce;
	FHyperAIStudioPreparedTypedArtifact CorePrepared;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, CorePrepared, OutError))
	{
		SetDiagnostic(OutReport.Status, TEXT("typed_contract_rejected"), OutError);
		return false;
	}
	const int64 NowUtcMs = State->Clock->NowUtcMs();
	const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
	const int64 PreparationLifetimeMs = FMath::Clamp<int64>(
		Request.StageLifetimeMs, 1, FHyperAIStudioTypedArtifactLimits::MaxStageLifetimeMs);
	if (NowUtcMs < 0 || NowMonotonicMs < 0
		|| NowUtcMs > MAX_int64 - PreparationLifetimeMs
		|| NowMonotonicMs > MAX_int64 - PreparationLifetimeMs)
	{
		OutError = TEXT("Trusted preparation clock sample is outside the bounded lifetime envelope.");
		SetDiagnostic(OutReport.Status, TEXT("trusted_preparation_clock_invalid"), OutError);
		return false;
	}
	const FString PreparationId = TEXT("prep.")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const TSharedRef<FTrustedPreparationRecord, ESPMode::ThreadSafe> PreparationRecord =
		MakeShared<FTrustedPreparationRecord, ESPMode::ThreadSafe>();
	PreparationRecord->Request = Request;
	PreparationRecord->Prepared = CorePrepared;
	PreparationRecord->AdapterModulePin = AdapterModulePin;
	PreparationRecord->Payload = Immutable;
	PreparationRecord->FreshVerifier = FreshVerifier;
	PreparationRecord->Admission = Resolved.Admission;
	PreparationRecord->bPreAdmissionEvidenceExecution = Resolved.bPreAdmissionEvidenceExecution;
	PreparationRecord->AdapterDescriptor = Resolved.AdapterDescriptor;
	PreparationRecord->PreparationId = PreparationId;
	PreparationRecord->ExpiresUtcMs = NowUtcMs + PreparationLifetimeMs;
	PreparationRecord->ExpiresMonotonicMs = NowMonotonicMs + PreparationLifetimeMs;
	const TSharedRef<FHyperAIStudioTrustedPreparedArtifactState, ESPMode::ThreadSafe> PreparedState =
		MakeShared<FHyperAIStudioTrustedPreparedArtifactState, ESPMode::ThreadSafe>();
	PreparedState->Host = State;
	PreparedState->PreparationId = PreparationId;
	PreparedState->ContractFingerprint = CorePrepared.ContractFingerprint;
	PreparedState->ExpiresUtcMs = PreparationRecord->ExpiresUtcMs;
	PreparedState->ExpiresMonotonicMs = PreparationRecord->ExpiresMonotonicMs;
	TArray<TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>> ReleasedExpiredPreparations;
	{
		FScopeLock Lock(&State->Mutex);
		TArray<FString> ExpiredPreparationIds;
		for (const TPair<FString, TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>>& Pair
			: State->Preparations)
		{
			if (Pair.Value.IsValid()
				&& Pair.Value->ExpiresMonotonicMs <= NowMonotonicMs)
			{
				ExpiredPreparationIds.Add(Pair.Key);
			}
		}
		for (const FString& ExpiredId : ExpiredPreparationIds)
		{
			if (TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>* Expired =
				State->Preparations.Find(ExpiredId))
			{
				ReleasedExpiredPreparations.Add(MoveTemp(*Expired));
				State->Preparations.Remove(ExpiredId);
			}
		}
		if (!State->bStarted || State->bShuttingDown)
		{
			OutError = TEXT("Trusted host quiesced before preparation could be returned.");
			SetDiagnostic(OutReport.Status, TEXT("trusted_host_quiescing"), OutError);
			return false;
		}
		if (State->Preparations.Num()
			>= FHyperAIStudioTrustedExecutionLimits::MaxTrustedPreparations)
		{
			OutError = TEXT("Bounded trusted preparation capacity is full.");
			SetDiagnostic(OutReport.Status, TEXT("trusted_preparation_capacity"), OutError);
			return false;
		}
		if (State->Preparations.Contains(PreparationId))
		{
			OutError = TEXT("Core preparation identity collided unexpectedly.");
			SetDiagnostic(OutReport.Status, TEXT("trusted_preparation_id_collision"), OutError);
			return false;
		}
		PreparationRecord->HostGeneration = State->HostGeneration;
		PreparedState->HostGeneration = State->HostGeneration;
		State->Preparations.Add(PreparationId, PreparationRecord);
		OutPrepared.State = PreparedState;
	}
	OutReport.bPrepared = true;
	OutReport.bMutationAdmitted = Resolved.Admission
		== EHyperAIStudioCapabilityAdmissionState::Admitted;
	OutReport.bPreAdmissionEvidenceExecution = Resolved.bPreAdmissionEvidenceExecution;
	OutReport.Admission = ToPublicAdmission(Resolved.Admission);
	OutReport.ContractFingerprint = CorePrepared.ContractFingerprint;
	OutReport.PlanHash = CorePrepared.PlanHash;
	OutReport.CatalogFingerprint = Resolved.Catalog.Catalog.GeneratedFingerprint;
	OutReport.CatalogGeneration = Resolved.Catalog.Generation;
	OutReport.Status = Resolved.Status;
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::StageExact(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
	FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
	OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
	OutError.Reset();
	ReconcileTrustedOwnedState(State.ToSharedRef());
	const TSharedPtr<const FHyperAIStudioTrustedPreparedArtifactState, ESPMode::ThreadSafe> Opaque =
		Prepared.State;
	if (!Opaque.IsValid() || Opaque->Host.Pin().Get() != State.Get()
		|| !IsBoundedId(Opaque->PreparationId, FHyperAIStudioDomainLimits::MaxOperationIdChars,
			TEXT("prep."))
		|| !IsSha256(Opaque->ContractFingerprint))
	{
		OutError = TEXT("Opaque preparation does not belong to this trusted core host.");
		SetDiagnostic(OutStatus, TEXT("trusted_preparation_invalid"), OutError);
		return false;
	}
	const int64 PreparationNowMonotonicMs = State->Clock->NowMonotonicMs();
	TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe> Exact;
	TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe> ReleasedExpiredPreparation;
	{
		FScopeLock Lock(&State->Mutex);
		TSharedPtr<FTrustedPreparationRecord, ESPMode::ThreadSafe>* Found =
			State->Preparations.Find(Opaque->PreparationId);
		if (Found && Found->IsValid()
			&& (*Found)->ExpiresMonotonicMs <= PreparationNowMonotonicMs)
		{
			ReleasedExpiredPreparation = MoveTemp(*Found);
			State->Preparations.Remove(Opaque->PreparationId);
			Found = nullptr;
		}
		if (Found && Found->IsValid()
			&& (*Found)->Prepared.ContractFingerprint == Opaque->ContractFingerprint
			&& (*Found)->HostGeneration == Opaque->HostGeneration)
		{
			Exact = *Found;
		}
	}
	if (!Exact.IsValid())
	{
		OutError = ReleasedExpiredPreparation.IsValid()
			? TEXT("Opaque trusted preparation expired before staging.")
			: TEXT("Opaque trusted preparation is missing or has a different exact binding.");
		SetDiagnostic(OutStatus, ReleasedExpiredPreparation.IsValid()
			? TEXT("trusted_preparation_expired") : TEXT("trusted_preparation_missing"), OutError, true);
		return false;
	}
	if (!IsBoundedId(OperationId, FHyperAIStudioDomainLimits::MaxOperationIdChars))
	{
		OutError = TEXT("operation_id is outside the trusted bounded identifier envelope.");
		SetDiagnostic(OutStatus, TEXT("trusted_operation_id_invalid"), OutError);
		return false;
	}
	if (Exact->Admission != EHyperAIStudioCapabilityAdmissionState::Admitted
		&& !Exact->bPreAdmissionEvidenceExecution)
	{
		OutError = TEXT("SourceCandidate and Planned artifacts are dry-run only.");
		SetDiagnostic(OutStatus, TEXT("source_candidate_execution_denied"), OutError, false, true);
		return false;
	}
	FHyperAIStudioTypedArtifactOperationStatus Existing;
	FString IgnoreQueryError;
	const bool bDurableReplay = State->Service->QueryStatus(OperationId, Existing, IgnoreQueryError)
		&& Existing.bFound && Existing.bDurable;
	if (!bDurableReplay && !ValidateExpectedCurrent(State.ToSharedRef(), *Exact, OutStatus, OutError))
	{
		return false;
	}

	// Clock sampling and optional-object destruction stay outside the host lock. Expired,
	// never-admitted contexts are the only contexts reclaimed for capacity.
	const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
	if (NowMonotonicMs < 0
		|| NowMonotonicMs > MAX_int64 - Exact->Request.StageLifetimeMs)
	{
		OutError = TEXT("Trusted stage clock sample is outside the bounded lifetime envelope.");
		SetDiagnostic(OutStatus, TEXT("trusted_stage_clock_invalid"), OutError);
		return false;
	}
	TArray<FTrustedStageContext> ReleasedExpired;
	bool bReservedExistingOperation = false;
	{
		FScopeLock Lock(&State->Mutex);
		TArray<FString> ExpiredIds;
		for (const TPair<FString, FTrustedStageContext>& Pair : State->StageContexts)
		{
			if (!Pair.Value.bDurable && Pair.Value.ExpiresMonotonicMs > 0
				&& Pair.Value.ExpiresMonotonicMs <= NowMonotonicMs)
			{
				ExpiredIds.Add(Pair.Key);
			}
		}
		for (const FString& ExpiredId : ExpiredIds)
		{
			if (FTrustedStageContext* Expired = State->StageContexts.Find(ExpiredId))
			{
				ReleasedExpired.Add(MoveTemp(*Expired));
				State->StageContexts.Remove(ExpiredId);
			}
		}
		if (!State->bStarted || State->bShuttingDown
			|| State->HostGeneration != Exact->HostGeneration)
		{
			OutError = TEXT("Trusted host lifecycle changed before stage admission.");
			SetDiagnostic(OutStatus, TEXT("trusted_host_lifecycle_drift"), OutError);
			return false;
		}
		if (State->StageReservations.Contains(OperationId))
		{
			OutError = TEXT("The exact operation already has an in-progress trusted stage admission.");
			SetDiagnostic(OutStatus, TEXT("trusted_stage_admission_busy"), OutError);
			return false;
		}
		for (const TPair<FString, FTrustedStageContext>& Pair : State->StageContexts)
		{
			if (Pair.Value.Receipt.OperationId != OperationId)
			{
				continue;
			}
			if (Pair.Value.Prepared->Prepared.ContractFingerprint
				!= Exact->Prepared.ContractFingerprint)
			{
				OutError = TEXT("operation_id is already bound to a different trusted preparation.");
				SetDiagnostic(OutStatus, TEXT("trusted_stage_operation_conflict"), OutError);
				return false;
			}
			bReservedExistingOperation = true;
			break;
		}
		if (!bReservedExistingOperation
			&& State->StageContexts.Num() + State->StageReservations.Num()
				>= FHyperAIStudioTrustedExecutionLimits::MaxTrustedStageContexts)
		{
			OutError = TEXT("Bounded trusted stage-context capacity is full; no live context was evicted.");
			SetDiagnostic(OutStatus, TEXT("trusted_stage_capacity"), OutError);
			return false;
		}
		State->StageReservations.Add(OperationId);
		++State->ActiveStageCalls;
	}

	const TSharedRef<FTrustedStateGate, ESPMode::ThreadSafe> Gate =
		MakeShared<FTrustedStateGate, ESPMode::ThreadSafe>(State.ToSharedRef(), Exact.ToSharedRef());
	const bool bServiceStaged = State->Service->StageExact(
		Exact->Prepared, OperationId, Exact->Payload.ToSharedRef(), OutReceipt, OutError);
	bool bKeepReceipt = false;
	bool bForceServiceShutdown = false;
	bool bLifecycleDrift = false;
	{
		FScopeLock Lock(&State->Mutex);
		State->StageReservations.Remove(OperationId);
		if (State->ActiveStageCalls > 0)
		{
			--State->ActiveStageCalls;
		}
		bLifecycleDrift = bServiceStaged && (!State->bStarted || State->bShuttingDown
			|| State->HostGeneration != Exact->HostGeneration);
		if (bServiceStaged && !bLifecycleDrift
			&& State->HostGeneration == Exact->HostGeneration)
		{
			if (FTrustedStageContext* ExistingContext = State->StageContexts.Find(OutReceipt.StageId))
			{
				bKeepReceipt = SameReceipt(ExistingContext->Receipt, OutReceipt)
					&& ExistingContext->Prepared->Prepared.ContractFingerprint
						== Exact->Prepared.ContractFingerprint;
			}
			else if (bReservedExistingOperation)
			{
				// The service must recover the same StageId for an exact operation replay.
				bKeepReceipt = false;
			}
			else
			{
				FTrustedStageContext Context;
				Context.Receipt = OutReceipt;
				Context.Prepared = Exact;
				Context.StateGate = Gate;
				Context.Sequence = State->NextStageSequence++;
				Context.ExpiresMonotonicMs = NowMonotonicMs
					+ Exact->Request.StageLifetimeMs;
				Context.bDurable = bDurableReplay;
				State->StageContexts.Add(OutReceipt.StageId, MoveTemp(Context));
				bKeepReceipt = true;
			}
		}
		bForceServiceShutdown = (State->bShuttingDown && State->ActiveStageCalls == 0)
			|| (bServiceStaged && !bKeepReceipt);
	}
	if (bForceServiceShutdown)
	{
		State->Service->Shutdown();
		Shutdown();
	}
	if (!bServiceStaged)
	{
		SetDiagnostic(OutStatus, TEXT("trusted_stage_rejected"), OutError,
			OutStatus.bPrerequisiteConflict, OutStatus.bAdmissionConflict);
		return false;
	}
	if (!bKeepReceipt)
	{
		OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
		OutError = bLifecycleDrift
			? TEXT("Trusted host lifecycle changed during stage admission; the service stage was quiesced.")
			: TEXT("Trusted stage admission could not atomically commit its exact context.");
		SetDiagnostic(OutStatus, bLifecycleDrift
			? TEXT("trusted_host_lifecycle_drift") : TEXT("trusted_stage_context_conflict"), OutError);
		return false;
	}
	SetDiagnostic(OutStatus, bDurableReplay ? TEXT("trusted_stage_replay") : TEXT("trusted_stage_ready"),
		bDurableReplay
			? TEXT("Exact durable operation stage receipt was recovered.")
			: TEXT("Exact admitted artifact is staged; no mutation has been dispatched."));
	if (!bDurableReplay && Exact->Request.Safety != EHyperAIStudioDomainSafety::Read)
	{
		// The receipt carries no target, so the activity log keeps it from here for the outcome entry.
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::Submitted;
		Entry.PackId = OutReceipt.PackId;
		Entry.ToolName = OutReceipt.ToolName;
		Entry.Target = Exact->Request.EffectTarget;
		Entry.OperationId = OutReceipt.OperationId;
		Entry.StatusCode = TEXT("staged");
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::SubmitExact(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	const FString& AuthorizationToken,
	FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
	FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutSubmission = FHyperAIStudioTypedArtifactSubmissionReceipt{};
	OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
	OutError.Reset();
	if (!HyperAIStudio::TypedArtifact::Private::ValidateStageReceiptBounds(Receipt, OutError)
		|| AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Authorization token exceeds the trusted bounded envelope.");
		}
		SetDiagnostic(OutStatus, TEXT("trusted_submission_invalid"), OutError);
		return false;
	}
	FTrustedStageContext Context;
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bStarted || State->bShuttingDown)
		{
			OutError = TEXT("Trusted host is quiescing and accepts no submission.");
			SetDiagnostic(OutStatus, TEXT("trusted_host_quiescing"), OutError);
			return false;
		}
		const FTrustedStageContext* Found = State->StageContexts.Find(Receipt.StageId);
		if (!Found || !SameReceipt(Found->Receipt, Receipt))
		{
			OutError = TEXT("Submission receipt has no exact core-owned trusted stage context.");
			SetDiagnostic(OutStatus, TEXT("trusted_stage_context_missing"), OutError);
			return false;
		}
		Context = *Found;
	}
	FHyperAIStudioTypedArtifactOperationStatus Existing;
	FString IgnoreQueryError;
	const bool bDurableReplay = State->Service->QueryStatus(
		Receipt.OperationId, Existing, IgnoreQueryError) && Existing.bFound && Existing.bDurable;
	if (!bDurableReplay
		&& !ValidateExpectedCurrent(State.ToSharedRef(), *Context.Prepared, OutStatus, OutError))
	{
		return false;
	}
	const bool bSubmitted = State->Service->SubmitExact(
		Receipt, AuthorizationToken, Context.StateGate.ToSharedRef(), OutSubmission, OutError);
	if (OutSubmission.Operation.bDurable)
	{
		FScopeLock Lock(&State->Mutex);
		if (FTrustedStageContext* Stored = State->StageContexts.Find(Receipt.StageId))
		{
			if (SameReceipt(Stored->Receipt, Receipt))
			{
				Stored->bDurable = true;
			}
		}
	}
	if (!bSubmitted)
	{
		const FHyperAIStudioTrustedExecutionDiagnostic GateStatus = Context.StateGate->GetLast();
		if (!GateStatus.StatusCode.IsEmpty())
		{
			OutStatus = GateStatus;
		}
		else
		{
			SetDiagnostic(OutStatus,
				OutSubmission.Operation.Status.IsEmpty()
					? TEXT("trusted_submission_rejected") : OutSubmission.Operation.Status,
				OutError);
		}
		return false;
	}
	SetDiagnostic(OutStatus,
		OutSubmission.Operation.bReplay ? TEXT("trusted_submission_replay") : TEXT("trusted_submission_accepted"),
		OutSubmission.Operation.Diagnostic);
	return true;
}

bool FHyperAIStudioTrustedExecutionHost::IssueAuthorizationFromCoreUiApproval(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	FString& OutAuthorizationToken,
	FString& OutError)
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutAuthorizationToken.Reset();
	OutError.Reset();
	if (!HyperAIStudio::TypedArtifact::Private::ValidateStageReceiptBounds(Receipt, OutError))
	{
		return false;
	}
	FTrustedStageContext Context;
	TSharedPtr<FTrustedServerGrantGate, ESPMode::ThreadSafe> Issuer;
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bStarted || State->bShuttingDown || !State->Authorization.CoreIssuer.IsValid())
		{
			OutError = TEXT("The production trusted authorization authority is unavailable or quiescing.");
			return false;
		}
		const FTrustedStageContext* Found = State->StageContexts.Find(Receipt.StageId);
		if (!Found || !SameReceipt(Found->Receipt, Receipt) || !Found->Prepared.IsValid())
		{
			OutError = TEXT("Core UI approval requires one exact live core-owned trusted stage.");
			return false;
		}
		Context = *Found;
		Issuer = State->Authorization.CoreIssuer;
	}

	FHyperAIStudioTrustedExecutionDiagnostic CurrentStatus;
	if (!ValidateExpectedCurrent(State.ToSharedRef(), *Context.Prepared, CurrentStatus, OutError))
	{
		return false;
	}
	// A successful durable terminal record can receive an already-consumed replay grant after an
	// editor restart. No new mutation may use this path, and every journal hash must match the stage.
	FHyperAIStudioTypedArtifactOperationStatus DurableStatus;
	FString QueryError;
	const bool bAdmitConsumedTerminalReplay = State->Service->QueryStatus(
			Receipt.OperationId, DurableStatus, QueryError)
		&& DurableStatus.bFound && DurableStatus.bAccepted && DurableStatus.bDurable
		&& DurableStatus.bTerminal && !DurableStatus.bOutcomeUnknown
		&& (DurableStatus.Status == TEXT("completed")
			|| DurableStatus.Status == TEXT("replay_completed"))
		&& DurableStatus.CanonicalProjectId == Receipt.CanonicalProjectId
		&& DurableStatus.PlanHash == Receipt.PlanHash
		&& DurableStatus.AuthorizationPlanHash == Receipt.AuthorizationPlanHash
		&& DurableStatus.CapabilityHash == Receipt.CapabilityHash
		&& DurableStatus.EffectFingerprint == Receipt.EffectFingerprint;

	FHyperAIStudioDomainAuthorizationRequest TrustedBinding;
	if (!BuildTrustedAuthorizationRequest(
			Receipt, Context.Prepared->AdapterDescriptor, FString(), TrustedBinding, OutError))
	{
		return false;
	}
	const FString StageFingerprint =
		HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(Receipt);
	return Issuer->IssueExact(
		TrustedBinding, StageFingerprint, bAdmitConsumedTerminalReplay,
		OutAuthorizationToken, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS
bool FHyperAIStudioTrustedExecutionHost::InspectCoreAuthorizationGrantForTests(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	const FString& AuthorizationToken,
	FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
	FString& OutError) const
{
	using namespace HyperAIStudio::TrustedExecution::Private;
	OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
	OutError.Reset();
	if (!HyperAIStudio::TypedArtifact::Private::ValidateStageReceiptBounds(Receipt, OutError)
		|| AuthorizationToken.Len() < 16
		|| AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The trusted authorization test inspection is outside the bounded envelope.");
		}
		return false;
	}
	FTrustedStageContext Context;
	TSharedPtr<FTrustedServerGrantGate, ESPMode::ThreadSafe> Issuer;
	{
		FScopeLock Lock(&State->Mutex);
		const FTrustedStageContext* Found = State->StageContexts.Find(Receipt.StageId);
		if (!Found || !SameReceipt(Found->Receipt, Receipt) || !Found->Prepared.IsValid()
			|| !State->Authorization.CoreIssuer.IsValid())
		{
			OutError = TEXT("The exact trusted stage or production grant authority is unavailable.");
			return false;
		}
		Context = *Found;
		Issuer = State->Authorization.CoreIssuer;
	}
	FHyperAIStudioDomainAuthorizationRequest Request;
	if (!BuildTrustedAuthorizationRequest(
			Receipt, Context.Prepared->AdapterDescriptor, AuthorizationToken, Request, OutError))
	{
		return false;
	}
	const int64 NowUtcMs = State->Clock->NowUtcMs();
	return Issuer->Inspect(Request, NowUtcMs, OutReceipt, OutError);
}
#endif

bool FHyperAIStudioTrustedExecutionHost::QueryStatus(
	const FString& OperationId,
	FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
	FString& OutError) const
{
	const bool bQueried = State->Service->QueryStatus(OperationId, OutStatus, OutError);
	TArray<HyperAIStudio::TrustedExecution::Private::FTrustedStageContext> ReleasedTerminal;
	if (bQueried && OutStatus.bFound && OutStatus.bTerminal)
	{
		FScopeLock Lock(&State->Mutex);
		TArray<FString> TerminalStageIds;
		for (const TPair<FString, HyperAIStudio::TrustedExecution::Private::FTrustedStageContext>& Pair
			: State->StageContexts)
		{
			if (Pair.Value.Receipt.OperationId == OperationId)
			{
				TerminalStageIds.Add(Pair.Key);
			}
		}
		for (const FString& StageId : TerminalStageIds)
		{
			if (HyperAIStudio::TrustedExecution::Private::FTrustedStageContext* Context =
				State->StageContexts.Find(StageId))
			{
				ReleasedTerminal.Add(MoveTemp(*Context));
				State->StageContexts.Remove(StageId);
			}
		}
	}
	return bQueried;
}

namespace HyperAIStudio::TrustedExecution::Private
{
	FCriticalSection CoreHostMutex;
	TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> CoreHost;
	bool bCoreHostStarting = false;

	TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> GetCoreHost()
	{
		FScopeLock Lock(&CoreHostMutex);
		return CoreHost;
	}

	bool StartupCore(const FString& TrustedProjectRoot, FString& OutError)
	{
		OutError.Reset();
		{
			FScopeLock Lock(&CoreHostMutex);
			if (CoreHost.IsValid() && CoreHost->IsAvailable())
			{
				return true;
			}
			if (bCoreHostStarting || CoreHost.IsValid())
			{
				OutError = TEXT("Trusted core host lifecycle is not quiescent.");
				return false;
			}
			bCoreHostStarting = true;
		}
		const TSharedRef<IHyperAIStudioTrustedCatalogAuthority, ESPMode::ThreadSafe> Catalog =
			MakeShared<FGeneratedCatalogAuthority, ESPMode::ThreadSafe>();
		const TSharedRef<IHyperAIStudioTrustedPrerequisiteEnvironment, ESPMode::ThreadSafe> Environment =
			MakeShared<FEnginePrerequisiteEnvironment, ESPMode::ThreadSafe>();
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FSystemClock, ESPMode::ThreadSafe>();
		const TSharedRef<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Candidate =
			MakeShared<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe>(
				TrustedProjectRoot, Catalog, Environment, Clock);
		const bool bStarted = Candidate->Startup(OutError);
		bool bStored = false;
		{
			FScopeLock Lock(&CoreHostMutex);
			bCoreHostStarting = false;
			if (bStarted && !CoreHost.IsValid())
			{
				CoreHost = Candidate;
				bStored = true;
			}
		}
		if (!bStored)
		{
			Candidate->Shutdown();
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Trusted core host startup lost its exact lifecycle reservation.");
			}
		}
		return bStored;
	}

	bool IssueCoreUiAuthorizationGrant(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FString& OutAuthorizationToken,
		FString& OutError)
	{
		const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = GetCoreHost();
		if (!Host.IsValid())
		{
			OutAuthorizationToken.Reset();
			OutError = TEXT("Trusted core execution host is unavailable.");
			return false;
		}
		return Host->IssueAuthorizationFromCoreUiApproval(
			Receipt, OutAuthorizationToken, OutError);
	}

	void BeginEnginePreExit()
	{
		const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = GetCoreHost();
		if (Host.IsValid())
		{
			Host->Shutdown();
		}
	}

	void ShutdownCore()
	{
		const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host = GetCoreHost();
		if (!Host.IsValid())
		{
			return;
		}
		Host->Shutdown();
		if (!Host->CanShutdownSafely())
		{
			return;
		}
		FScopeLock Lock(&CoreHostMutex);
		if (CoreHost.Get() == Host.Get())
		{
			CoreHost.Reset();
		}
	}
}

bool FHyperAIStudioTrustedExecutionFacade::IsAvailable()
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	return Host.IsValid() && Host->IsAvailable();
}

bool FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
	const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
	FHyperAIStudioDomainRegistrationHandle& OutHandle,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutHandle = FHyperAIStudioDomainRegistrationHandle{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->RegisterAdapter(Adapter, OutHandle, OutError);
}

EHyperAIStudioDomainUnregisterResult FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(
	const FHyperAIStudioDomainRegistrationHandle& Handle,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutError = TEXT("Trusted core execution host is unavailable.");
		return EHyperAIStudioDomainUnregisterResult::NotFound;
	}
	return Host->UnregisterAdapter(Handle, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
	const FHyperAIStudioDomainRegistrationHandle& OwnerAdapter,
	const FString& ProbeId,
	FHyperAIStudioTrustedProbeRegistrationHandle& OutHandle,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutHandle = FHyperAIStudioTrustedProbeRegistrationHandle{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->RegisterLiveProbe(OwnerAdapter, ProbeId, OutHandle, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
	const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
	const FHyperAIStudioTrustedProbeResult& Observation,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->PublishLiveProbeExact(Handle, Observation, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(
	const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->UnregisterLiveProbe(Handle, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(
	const FHyperAIStudioTrustedArtifactRequest& Request,
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
	const TSharedRef<IHyperAIStudioTrustedFreshVerifier, ESPMode::ThreadSafe>& FreshVerifier,
	FHyperAIStudioTrustedPreparedArtifact& OutPrepared,
	FHyperAIStudioTrustedPrepareReport& OutReport,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutPrepared.Reset();
		OutReport = FHyperAIStudioTrustedPrepareReport{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->PrepareDryRun(Request, Payload, FreshVerifier, OutPrepared, OutReport, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::StageExact(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
	FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
		OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->StageExact(Prepared, OperationId, OutReceipt, OutStatus, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::SubmitExact(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	const FString& AuthorizationToken,
	FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
	FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutSubmission = FHyperAIStudioTypedArtifactSubmissionReceipt{};
		OutStatus = FHyperAIStudioTrustedExecutionDiagnostic{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->SubmitExact(Receipt, AuthorizationToken, OutSubmission, OutStatus, OutError);
}

bool FHyperAIStudioTrustedExecutionFacade::QueryStatus(
	const FString& OperationId,
	FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
	FString& OutError)
{
	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutStatus = FHyperAIStudioTypedArtifactOperationStatus{};
		OutError = TEXT("Trusted core execution host is unavailable.");
		return false;
	}
	return Host->QueryStatus(OperationId, OutStatus, OutError);
}
