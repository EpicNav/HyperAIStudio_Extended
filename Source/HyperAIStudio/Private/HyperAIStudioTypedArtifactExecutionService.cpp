// Games by Hyper 2026.

#include "HyperAIStudioTypedArtifactExecutionService.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioTypedArtifactExecutionInternal.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::TypedArtifactHost::Private
{
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

	struct FArchivedStatus
	{
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		FHyperAIStudioTypedArtifactOperationStatus Status;
		FString ReplayCredentialFingerprint;
		EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read;
	};

	bool SameReceipt(
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

	bool PreparedPayloadMatchesReceipt(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const IHyperAIStudioTypedArtifactPayload& Payload,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt)
	{
		FHyperAIStudioPreparedTypedArtifact Rebuilt;
		FString Ignore;
		if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Prepared.Contract, Rebuilt, Ignore))
		{
			return false;
		}
		return Prepared.ContractFingerprint == Rebuilt.ContractFingerprint
			&& Prepared.BindingFingerprint == Rebuilt.BindingFingerprint
			&& Prepared.PlanHash == Rebuilt.PlanHash
			&& Prepared.AuthorizationPlanHash == Rebuilt.AuthorizationPlanHash
			&& Prepared.CapabilityHash == Rebuilt.CapabilityHash
			&& Prepared.EffectFingerprint == Rebuilt.EffectFingerprint
			&& Rebuilt.Contract.Binding.CanonicalProjectId == Receipt.CanonicalProjectId
			&& Rebuilt.Contract.Binding.PackId == Receipt.PackId
			&& Rebuilt.Contract.Binding.ToolName == Receipt.ToolName
			&& Rebuilt.Contract.Binding.VariantId == Receipt.VariantId
			&& Rebuilt.Contract.Binding.ExpectedAdapterFingerprint == Receipt.AdapterFingerprint
			&& Rebuilt.Contract.Binding.ExpectedAdapterGeneration == Receipt.AdapterGeneration
			&& Rebuilt.Contract.Binding.ExpectedRegistryEpoch == Receipt.RegistryEpoch
			&& Rebuilt.Contract.Binding.Admission.Fingerprint == Receipt.AdmissionFingerprint
			&& Rebuilt.Contract.Binding.Prerequisites.Fingerprint == Receipt.PrerequisiteFingerprint
			&& Rebuilt.Contract.ArtifactTypeId == Receipt.ArtifactTypeId
			&& Rebuilt.Contract.ArtifactSchemaFingerprint == Receipt.ArtifactSchemaFingerprint
			&& Rebuilt.Contract.ArtifactSemanticFingerprint == Receipt.ArtifactSemanticFingerprint
			&& Rebuilt.ContractFingerprint == Receipt.ContractFingerprint
			&& Rebuilt.PlanHash == Receipt.PlanHash
			&& Rebuilt.AuthorizationPlanHash == Receipt.AuthorizationPlanHash
			&& Rebuilt.CapabilityHash == Receipt.CapabilityHash
			&& Rebuilt.EffectFingerprint == Receipt.EffectFingerprint
			&& Payload.GetTypeId() == Receipt.ArtifactTypeId
			&& Payload.GetSchemaFingerprint() == Receipt.ArtifactSchemaFingerprint
			&& Payload.GetSemanticFingerprint() == Receipt.ArtifactSemanticFingerprint
			&& Payload.GetBoundedByteSize() >= 0
			&& Payload.GetBoundedByteSize() <= FHyperAIStudioDomainLimits::MaxRequestBytes;
	}

	FHyperAIStudioTypedArtifactOperationStatus ToStatus(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FHyperAIStudioTypedArtifactAsyncSnapshot& Snapshot)
	{
		FHyperAIStudioTypedArtifactOperationStatus Status;
		Status.bFound = true;
		Status.bAccepted = Snapshot.bAccepted;
		Status.bTerminal = Snapshot.bTerminal;
		Status.bReplay = Snapshot.Result.bReplay;
		Status.bOutcomeUnknown = Snapshot.Result.bOutcomeUnknown;
		Status.bDurable = Snapshot.bDurable;
		Status.bFallbackPermitted = false;
		Status.Status = Snapshot.Result.Status;
		Status.Diagnostic = Snapshot.Result.Diagnostic.Left(
			FHyperAIStudioTypedArtifactHostLimits::MaxStatusDiagnosticChars);
		if (Status.bAccepted && !Status.bTerminal && Status.Status == TEXT("ready"))
		{
			Status.Status = TEXT("accepted");
			if (Status.Diagnostic.IsEmpty())
			{
				Status.Diagnostic = TEXT("Durable admission succeeded; serial execution is pending a ticker turn.");
			}
		}
		Status.StageId = Receipt.StageId;
		Status.OperationId = Receipt.OperationId;
		Status.CanonicalProjectId = Receipt.CanonicalProjectId;
		Status.PlanHash = Receipt.PlanHash;
		Status.AuthorizationPlanHash = Receipt.AuthorizationPlanHash;
		Status.CapabilityHash = Receipt.CapabilityHash;
		Status.EffectFingerprint = Receipt.EffectFingerprint;
		Status.ActiveActionKind = Snapshot.ActiveActionKind;
		Status.ActiveStepId = Snapshot.ActiveStepId;
		Status.CompletedActionCount = Snapshot.Result.CompletedActionCount;
		Status.ScheduledActionCount = Snapshot.Result.ScheduledActionCount;
		Status.NativeOperationCount = Snapshot.Result.NativeOperationCount;
		Status.GameThreadMs = Snapshot.Result.GameThreadMs;
		Status.OutputBytes = Snapshot.Result.OutputBytes;
		Status.LateResultCount = Snapshot.LateResultCount;
		return Status;
	}

	FHyperAIStudioTypedArtifactOperationStatus RejectedStatus(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& Code,
		const FString& Diagnostic)
	{
		FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot;
		Snapshot.bTerminal = true;
		Snapshot.Result.Status = Code;
		Snapshot.Result.Diagnostic = Diagnostic;
		Snapshot.Result.OperationId = Receipt.OperationId;
		Snapshot.Result.CanonicalProjectId = Receipt.CanonicalProjectId;
		Snapshot.Result.PlanHash = Receipt.PlanHash;
		Snapshot.Result.AuthorizationPlanHash = Receipt.AuthorizationPlanHash;
		Snapshot.Result.CapabilityHash = Receipt.CapabilityHash;
		Snapshot.Result.EffectFingerprint = Receipt.EffectFingerprint;
		return ToStatus(Receipt, Snapshot);
	}

	FHyperAIStudioTypedArtifactOperationStatus AdmissionStatus(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt)
	{
		FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot;
		Snapshot.Result.Status = TEXT("admission_in_progress");
		Snapshot.Result.Diagnostic =
			TEXT("The exact receipt owns the bounded admission reservation; retry without changing it.");
		return ToStatus(Receipt, Snapshot);
	}

	bool VerifyReplayCredential(
		const EHyperAIStudioDomainSafety Safety,
		const FString& ExpectedFingerprint,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& AuthorizationToken)
	{
		if (Safety == EHyperAIStudioDomainSafety::Edit)
		{
			return AuthorizationToken.IsEmpty();
		}
		if (Safety != EHyperAIStudioDomainSafety::Destructive
			&& Safety != EHyperAIStudioDomainSafety::ExternalEffect)
		{
			return true;
		}
		return !ExpectedFingerprint.IsEmpty()
			&& HyperAIStudio::TypedArtifact::Private::BuildReplayCredentialFingerprint(
				AuthorizationToken, Receipt) == ExpectedFingerprint;
	}

	void FillDurableStatus(
		const FHyperAIStudioOperationRecord& Record,
		const int32 ReconciledCount,
		FHyperAIStudioTypedArtifactOperationStatus& OutStatus)
	{
		OutStatus = FHyperAIStudioTypedArtifactOperationStatus{};
		OutStatus.bFound = true;
		OutStatus.bAccepted = Record.State != EHyperAIStudioOperationState::Queued;
		OutStatus.bTerminal = Record.IsTerminal();
		OutStatus.bReplay = Record.State == EHyperAIStudioOperationState::Completed;
		OutStatus.bOutcomeUnknown = Record.State == EHyperAIStudioOperationState::OutcomeUnknown;
		OutStatus.bDurable = true;
		OutStatus.bReconciled = ReconciledCount > 0;
		OutStatus.bFallbackPermitted = false;
		OutStatus.Status = FHyperAIStudioOperationJournal::LexToString(Record.State);
		OutStatus.Diagnostic = Record.StatusCode.Left(
			FHyperAIStudioTypedArtifactHostLimits::MaxStatusDiagnosticChars);
		OutStatus.OperationId = Record.OperationId;
		OutStatus.CanonicalProjectId = Record.CanonicalProjectId;
		OutStatus.PlanHash = Record.PlanHash;
		OutStatus.CapabilityHash = Record.CapabilityHash;
		if (Record.TerminalEvidence.IsPresent())
		{
			OutStatus.EffectFingerprint = Record.TerminalEvidence.EffectFingerprint;
		}
	}
}

struct FHyperAIStudioTypedArtifactExecutionServiceState
{
	FHyperAIStudioTypedArtifactExecutionServiceState(
		const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>& InRegistry,
		FString InProjectRoot,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& InClock)
		: Registry(InRegistry)
		, TrustedProjectRoot(MoveTemp(InProjectRoot))
		, CanonicalProjectId(FHyperAIStudioOperationJournal::MakeCanonicalProjectId(TrustedProjectRoot))
		, Clock(InClock)
		, Store(MakeUnique<FHyperAIStudioTypedArtifactStore>(*Registry, Clock.ToSharedRef()))
	{
	}

	mutable FCriticalSection Mutex;
	TSharedPtr<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe> Registry;
	FString TrustedProjectRoot;
	FString CanonicalProjectId;
	TSharedPtr<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock;
	TUniquePtr<FHyperAIStudioTypedArtifactStore> Store;
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Active;
	FHyperAIStudioTypedArtifactStageReceipt ActiveReceipt;
	bool bPumpAdmissionBarrier = false;
	bool bAdmissionInProgress = false;
	FHyperAIStudioTypedArtifactStageReceipt AdmissionReceipt;
	TMap<FString, HyperAIStudio::TypedArtifactHost::Private::FArchivedStatus> Archived;
	TArray<FString> ArchiveOrder;
	TMap<FString, HyperAIStudio::TypedArtifactHost::Private::FArchivedStatus> RejectedAttempts;
	TArray<FString> RejectedAttemptOrder;
	FTSTicker::FDelegateHandle TickerHandle;
	int32 ActiveTickerCallbackCount = 0;
	int32 ActiveDispatchPermitCount = 0;
	FString ActiveDispatchPermitReceiptFingerprint;
	bool bShuttingDown = false;
};

namespace HyperAIStudio::TypedArtifactHost::Private
{
	class FServiceDispatchLease final : public IHyperAIStudioTypedArtifactDispatchLease
	{
	public:
		FServiceDispatchLease(
			TWeakPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> InState,
			FString InReceiptFingerprint)
			: State(MoveTemp(InState))
			, ReceiptFingerprint(MoveTemp(InReceiptFingerprint))
		{
		}

		virtual ~FServiceDispatchLease() override
		{
			const TSharedPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> Pinned =
				State.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}
			FString ReleasedFingerprint;
			{
				FScopeLock Lock(&Pinned->Mutex);
				if (Pinned->ActiveDispatchPermitCount > 0
					&& Pinned->ActiveDispatchPermitReceiptFingerprint == ReceiptFingerprint)
				{
					--Pinned->ActiveDispatchPermitCount;
					if (Pinned->ActiveDispatchPermitCount == 0)
					{
						ReleasedFingerprint = MoveTemp(
							Pinned->ActiveDispatchPermitReceiptFingerprint);
					}
				}
			}
			ReleasedFingerprint.Reset();
		}

	private:
		TWeakPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> State;
		FString ReceiptFingerprint;
	};

	class FServiceDispatchPermit final : public IHyperAIStudioTypedArtifactDispatchPermit
	{
	public:
		FServiceDispatchPermit(
			TWeakPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> InState,
			FHyperAIStudioTypedArtifactStageReceipt InReceipt,
			FString InReceiptFingerprint)
			: State(MoveTemp(InState))
			, Receipt(MoveTemp(InReceipt))
			, ReceiptFingerprint(MoveTemp(InReceiptFingerprint))
		{
		}

		virtual bool AcquireBeforeCommit(
			TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe>& OutLease,
			FString& OutError) const override
		{
			return AcquireExact(TEXT("commit"), OutLease, OutError);
		}

		virtual bool AcquireBeforeDispatch(
			TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe>& OutLease,
			FString& OutError) const override
		{
			return AcquireExact(TEXT("dispatch"), OutLease, OutError);
		}

	private:
		bool AcquireExact(
			const TCHAR* Phase,
			TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe>& OutLease,
			FString& OutError) const
		{
			OutLease.Reset();
			OutError.Reset();
			const TSharedPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> Pinned =
				State.Pin();
			if (!Pinned.IsValid())
			{
				OutError = TEXT("The typed-artifact host no longer owns the exact execution state.");
				return false;
			}
			bool bAdmitted = false;
			{
				FScopeLock Lock(&Pinned->Mutex);
				bAdmitted = !Pinned->bShuttingDown && Pinned->Active.IsValid()
					&& SameReceipt(Pinned->ActiveReceipt, Receipt)
					&& !ReceiptFingerprint.IsEmpty()
					&& (Pinned->ActiveDispatchPermitCount == 0
						|| Pinned->ActiveDispatchPermitReceiptFingerprint == ReceiptFingerprint);
				if (bAdmitted)
				{
					if (Pinned->ActiveDispatchPermitCount == 0)
					{
						Pinned->ActiveDispatchPermitReceiptFingerprint = ReceiptFingerprint;
					}
					++Pinned->ActiveDispatchPermitCount;
				}
			}
			if (!bAdmitted)
			{
				OutError = FString::Printf(
					TEXT("The typed-artifact host quiesced or lost the exact project lane before %s."),
					Phase);
				return false;
			}
			OutLease = MakeShared<FServiceDispatchLease, ESPMode::ThreadSafe>(
				State, ReceiptFingerprint);
			return true;
		}

		TWeakPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> State;
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		FString ReceiptFingerprint;
	};

	enum class EDurableIdentityProbe : uint8
	{
		Absent,
		Present,
		Indeterminate
	};

	EDurableIdentityProbe ProbeDurableIdentity(
		const FHyperAIStudioTypedArtifactExecutionServiceState& State,
		const FString& OperationId)
	{
		FHyperAIStudioOperationJournal Journal(State.TrustedProjectRoot);
		FString Ignore;
		if (!Journal.Load(Ignore))
		{
			return EDurableIdentityProbe::Indeterminate;
		}
		return Journal.Find(OperationId).IsSet()
			? EDurableIdentityProbe::Present
			: EDurableIdentityProbe::Absent;
	}

	void ArchiveLocked(
		FHyperAIStudioTypedArtifactExecutionServiceState& State,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FHyperAIStudioTypedArtifactOperationStatus& Status,
		const FString& ReplayCredentialFingerprint = FString(),
		const EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read)
	{
		if (Receipt.OperationId.IsEmpty())
		{
			return;
		}
		if (!State.Archived.Contains(Receipt.OperationId))
		{
			State.ArchiveOrder.Add(Receipt.OperationId);
		}
		FArchivedStatus Archived;
		Archived.Receipt = Receipt;
		Archived.Status = Status;
		Archived.ReplayCredentialFingerprint = ReplayCredentialFingerprint;
		Archived.Safety = Safety;
		State.Archived.Add(Receipt.OperationId, MoveTemp(Archived));
		while (State.ArchiveOrder.Num() > FHyperAIStudioTypedArtifactHostLimits::MaxArchivedStatuses)
		{
			State.Archived.Remove(State.ArchiveOrder[0]);
			State.ArchiveOrder.RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	bool ReconcileTerminalSession(
		const TSharedRef<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe>& State,
		const TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe>& Session,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FHyperAIStudioTypedArtifactAsyncSnapshot& Snapshot,
		FTSTicker::FDelegateHandle& OutTickerHandle)
	{
		OutTickerHandle.Reset();
		if (!Session.IsValid() || !Snapshot.bTerminal)
		{
			return false;
		}

		TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Released;
		bool bFirstTerminal = false;
		{
			FScopeLock Lock(&State->Mutex);
			if (State->Active.Get() != Session.Get()
				|| !SameReceipt(State->ActiveReceipt, Receipt))
			{
				return false;
			}
			bFirstTerminal = !State->Archived.Contains(Receipt.OperationId);
			ArchiveLocked(
				*State,
				Receipt,
				ToStatus(Receipt, Snapshot),
				Snapshot.ReplayCredentialFingerprint,
				Snapshot.Safety);
			if (!Snapshot.bOutstandingDispatch)
			{
				Released = MoveTemp(State->Active);
				State->ActiveReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
				State->bPumpAdmissionBarrier = false;
				if (State->ActiveTickerCallbackCount == 0)
				{
					OutTickerHandle = State->TickerHandle;
				}
				State->TickerHandle.Reset();
			}
		}
		// Every trusted operation ends here exactly once; the outcome feeds the Activity panel and the agent
		// scoreboard. Recorded outside the lock because the log broadcasts to the UI.
		if (bFirstTerminal && Snapshot.Safety != EHyperAIStudioDomainSafety::Read)
		{
			const EHyperAIStudioTypedArtifactExecutionState Outcome = Snapshot.Result.State;
			FHyperAIStudioActivityEntry Entry;
			Entry.Kind = Outcome == EHyperAIStudioTypedArtifactExecutionState::Completed
				|| Outcome == EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted
				? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
			Entry.PackId = Receipt.PackId;
			Entry.ToolName = Receipt.ToolName;
			Entry.OperationId = Receipt.OperationId;
			Entry.StatusCode = Snapshot.Result.Status;
			Entry.Detail = Snapshot.Result.Diagnostic.Left(512);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		}
		return Released.IsValid();
	}

	void ArchiveRejectedAttemptLocked(
		FHyperAIStudioTypedArtifactExecutionServiceState& State,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& ReceiptFingerprint,
		const FHyperAIStudioTypedArtifactOperationStatus& Status,
		const EHyperAIStudioDomainSafety Safety)
	{
		if (ReceiptFingerprint.IsEmpty())
		{
			return;
		}
		if (!State.RejectedAttempts.Contains(ReceiptFingerprint))
		{
			State.RejectedAttemptOrder.Add(ReceiptFingerprint);
		}
		FArchivedStatus Attempt;
		Attempt.Receipt = Receipt;
		Attempt.Status = Status;
		Attempt.Safety = Safety;
		State.RejectedAttempts.Add(ReceiptFingerprint, MoveTemp(Attempt));
		while (State.RejectedAttemptOrder.Num()
			> FHyperAIStudioTypedArtifactHostLimits::MaxRejectedSubmissionReceipts)
		{
			State.RejectedAttempts.Remove(State.RejectedAttemptOrder[0]);
			State.RejectedAttemptOrder.RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	bool TickService(
		const TSharedRef<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe>& State)
	{
		if (!IsInGameThread())
		{
			// Core ticker work is admitted only by the game thread; keep the tracked delegate alive.
			return true;
		}
		{
			FScopeLock Lock(&State->Mutex);
			++State->ActiveTickerCallbackCount;
		}
		ON_SCOPE_EXIT
		{
			FScopeLock Lock(&State->Mutex);
			State->ActiveTickerCallbackCount = FMath::Max(0, State->ActiveTickerCallbackCount - 1);
		};
		TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Session;
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		bool bShuttingDown = false;
		bool bPumpAdmissionBarrier = false;
		{
			FScopeLock Lock(&State->Mutex);
			bShuttingDown = State->bShuttingDown;
			if (!State->Active.IsValid())
			{
				State->bPumpAdmissionBarrier = false;
				State->TickerHandle.Reset();
				return false;
			}
			Session = State->Active;
			Receipt = State->ActiveReceipt;
			bPumpAdmissionBarrier = State->bPumpAdmissionBarrier;
			State->bPumpAdmissionBarrier = false;
		}
		if (bShuttingDown)
		{
			if (!Session->IsTerminal())
			{
				FString Ignore;
				Session->RequestCancel(TEXT("HyperAIStudio typed-artifact host shutdown"), Ignore);
			}
			const FHyperAIStudioTypedArtifactAsyncSnapshot ShutdownSnapshot = Session->GetSnapshot();
			const FHyperAIStudioTypedArtifactOperationStatus ShutdownStatus =
				ToStatus(Receipt, ShutdownSnapshot);
			const bool bKeepForDrain = ShutdownSnapshot.bOutstandingDispatch
				|| !ShutdownSnapshot.bTerminal;
			TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Released;
			{
				FScopeLock Lock(&State->Mutex);
				ArchiveLocked(
					*State, Receipt, ShutdownStatus,
					ShutdownSnapshot.ReplayCredentialFingerprint, ShutdownSnapshot.Safety);
				if (!bKeepForDrain && State->Active.Get() == Session.Get())
				{
					Released = MoveTemp(State->Active);
					State->ActiveReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
					State->bPumpAdmissionBarrier = false;
					State->TickerHandle.Reset();
				}
			}
			return bKeepForDrain;
		}

		// These calls may sample injected clocks, execute adapter code, and synchronously invoke a
		// completion. They must never run under the service lock.
		Session->TickDeadline();
		FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot = Session->GetSnapshot();
		bool bQuiescingBeforePump = false;
		{
			FScopeLock Lock(&State->Mutex);
			bQuiescingBeforePump = State->bShuttingDown;
		}
		if (!bQuiescingBeforePump
			&& !bPumpAdmissionBarrier && !Snapshot.bTerminal && !Snapshot.bOutstandingDispatch)
		{
			FString PumpError;
			Session->PumpOne(PumpError);
			Snapshot = Session->GetSnapshot();
		}
		if (!Snapshot.bTerminal)
		{
			return true;
		}

		const bool bKeepForLateCompletion = Snapshot.bOutstandingDispatch;
		FTSTicker::FDelegateHandle IgnoredTickerHandle;
		ReconcileTerminalSession(State, Session, Receipt, Snapshot, IgnoredTickerHandle);
		return bKeepForLateCompletion;
	}

	bool ScheduleTicker(
		const TSharedRef<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe>& State)
	{
		check(IsInGameThread());
		const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickService(State);
			}),
			0.0f);
		bool bRemove = false;
		{
			FScopeLock Lock(&State->Mutex);
			bRemove = !Handle.IsValid() || State->bShuttingDown || !State->Active.IsValid()
				|| State->TickerHandle.IsValid();
			if (!bRemove)
			{
				State->TickerHandle = Handle;
			}
		}
		if (bRemove && Handle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Handle);
		}
		return !bRemove;
	}
}

FHyperAIStudioTypedArtifactExecutionService::FHyperAIStudioTypedArtifactExecutionService(
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>& Registry,
	const FString& TrustedProjectRoot)
	: FHyperAIStudioTypedArtifactExecutionService(
		Registry,
		TrustedProjectRoot,
		MakeShared<HyperAIStudio::TypedArtifactHost::Private::FSystemClock, ESPMode::ThreadSafe>())
{
}

FHyperAIStudioTypedArtifactExecutionService::FHyperAIStudioTypedArtifactExecutionService(
	const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>& Registry,
	const FString& TrustedProjectRoot,
	const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock)
	: State(MakeShared<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe>(
		Registry, TrustedProjectRoot, Clock))
{
}

FHyperAIStudioTypedArtifactExecutionService::~FHyperAIStudioTypedArtifactExecutionService()
{
	Shutdown();
}

bool FHyperAIStudioTypedArtifactExecutionService::Startup(FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Typed-artifact host startup must run on the Unreal game thread.");
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Existing;
	bool bTickerInstalled = false;
	int32 ActiveTickerCallbackCount = 0;
	int32 ActiveDispatchPermitCount = 0;
	{
		FScopeLock Lock(&State->Mutex);
		Existing = State->Active;
		bTickerInstalled = State->TickerHandle.IsValid();
		ActiveTickerCallbackCount = State->ActiveTickerCallbackCount;
		ActiveDispatchPermitCount = State->ActiveDispatchPermitCount;
		if (State->bAdmissionInProgress)
		{
			bTickerInstalled = true;
		}
	}
	if (bTickerInstalled || ActiveTickerCallbackCount > 0 || ActiveDispatchPermitCount > 0
		|| (Existing.IsValid() && Existing->HasOutstandingDispatch()))
	{
		OutError = TEXT("The previous typed-artifact host lifecycle is not quiescent.");
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Released;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->CanonicalProjectId.IsEmpty())
		{
			OutError = TEXT("The core-owned project root has no strong canonical identity.");
			return false;
		}
		if (State->TickerHandle.IsValid() || State->ActiveTickerCallbackCount > 0
			|| State->ActiveDispatchPermitCount > 0
			|| State->bAdmissionInProgress || State->Active.Get() != Existing.Get())
		{
			OutError = TEXT("The previous typed-artifact host lifecycle is not quiescent.");
			return false;
		}
		Released = MoveTemp(State->Active);
		State->ActiveReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
		State->bPumpAdmissionBarrier = false;
		State->bShuttingDown = false;
	}
	return true;
}

bool FHyperAIStudioTypedArtifactExecutionService::StageExact(
	const FHyperAIStudioPreparedTypedArtifact& Prepared,
	const FString& OperationId,
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
	FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifactHost::Private;
	OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Typed-artifact staging must run on the Unreal game thread.");
		return false;
	}
	TOptional<FArchivedStatus> Archived;
	FHyperAIStudioTypedArtifactStageReceipt ActiveReceipt;
	FHyperAIStudioTypedArtifactStageReceipt AdmissionReceipt;
	bool bShuttingDown = false;
	{
		FScopeLock Lock(&State->Mutex);
		bShuttingDown = State->bShuttingDown;
		if (State->Active.IsValid() && State->ActiveReceipt.OperationId == OperationId)
		{
			ActiveReceipt = State->ActiveReceipt;
		}
		if (State->bAdmissionInProgress && State->AdmissionReceipt.OperationId == OperationId)
		{
			AdmissionReceipt = State->AdmissionReceipt;
		}
		if (const FArchivedStatus* Existing = State->Archived.Find(OperationId))
		{
			Archived = *Existing;
		}
	}
	if (bShuttingDown)
	{
		OutError = TEXT("The typed-artifact host is quiescing and accepts no staged work.");
		return false;
	}
	const auto RecoverReceiptUnlessQuiescing =
		[&](const FHyperAIStudioTypedArtifactStageReceipt& ExactReceipt)
		{
			bool bNowShuttingDown = false;
			{
				FScopeLock Lock(&State->Mutex);
				bNowShuttingDown = State->bShuttingDown;
			}
			if (bNowShuttingDown)
			{
				OutError = TEXT("The typed-artifact host quiesced while recovering the exact stage receipt.");
				return false;
			}
			OutReceipt = ExactReceipt;
			return true;
		};
	if (!ActiveReceipt.OperationId.IsEmpty())
	{
		if (PreparedPayloadMatchesReceipt(Prepared, Payload.Get(), ActiveReceipt))
		{
			return RecoverReceiptUnlessQuiescing(ActiveReceipt);
		}
		OutError = TEXT("operation_id is active with a different exact typed artifact.");
		return false;
	}
	if (!AdmissionReceipt.OperationId.IsEmpty())
	{
		if (PreparedPayloadMatchesReceipt(Prepared, Payload.Get(), AdmissionReceipt))
		{
			return RecoverReceiptUnlessQuiescing(AdmissionReceipt);
		}
		OutError = TEXT("operation_id has an in-progress admission for a different exact typed artifact.");
		return false;
	}
	if (Archived.IsSet())
	{
		if (PreparedPayloadMatchesReceipt(Prepared, Payload.Get(), Archived->Receipt))
		{
			return RecoverReceiptUnlessQuiescing(Archived->Receipt);
		}
		OutError = TEXT("operation_id is archived with a different exact typed artifact.");
		return false;
	}
	if (Prepared.Contract.Binding.CanonicalProjectId != State->CanonicalProjectId)
	{
		OutError = TEXT("The sealed artifact does not target the service-owned canonical project.");
		return false;
	}
	FHyperAIStudioTypedArtifactStageReceipt StagedReceipt;
	if (!State->Store->StageExact(Prepared, OperationId, Payload, StagedReceipt, OutError))
	{
		return false;
	}
	if (!RecoverReceiptUnlessQuiescing(StagedReceipt))
	{
		// Shutdown may have re-entered through a cloned payload or injected clock before commit.
		State->Store->Reset();
		return false;
	}
	return true;
}

bool FHyperAIStudioTypedArtifactExecutionService::SubmitExact(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	const FString& AuthorizationToken,
	const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
	FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifactHost::Private;
	OutSubmission = FHyperAIStudioTypedArtifactSubmissionReceipt{};
	OutError.Reset();
	if (!HyperAIStudio::TypedArtifact::Private::ValidateStageReceiptBounds(Receipt, OutError))
	{
		return false;
	}
	const FString ReceiptFingerprint =
		HyperAIStudio::TypedArtifact::Private::BuildStageReceiptFingerprint(Receipt);
	if (ReceiptFingerprint.IsEmpty())
	{
		OutError = TEXT("The exact typed-artifact stage receipt could not be fingerprinted.");
		return false;
	}
	if (AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
	{
		OutError = TEXT("Typed-artifact authorization exceeds the bounded server grant envelope.");
		return false;
	}
	OutSubmission.Stage = Receipt;
	if (!IsInGameThread())
	{
		OutError = TEXT("Typed-artifact submission must run on the Unreal game thread.");
		OutSubmission.Operation = RejectedStatus(Receipt, TEXT("game_thread_required"), OutError);
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> ExistingSession;
	TOptional<FArchivedStatus> Archived;
	TOptional<FArchivedStatus> RejectedAttempt;
	FHyperAIStudioTypedArtifactStageReceipt ExistingReceipt;
	FHyperAIStudioTypedArtifactStageReceipt AdmissionReceipt;
	FString ImmediateError;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShuttingDown)
		{
			ImmediateError = TEXT("The typed-artifact host is quiescing and accepts no submissions.");
		}
		else
		{
			if (const FArchivedStatus* ExistingAttempt =
				State->RejectedAttempts.Find(ReceiptFingerprint))
			{
				RejectedAttempt = *ExistingAttempt;
			}
			if (const FArchivedStatus* Existing = State->Archived.Find(Receipt.OperationId))
			{
				Archived = *Existing;
			}
			if (State->Active.IsValid())
			{
				ExistingSession = State->Active;
				ExistingReceipt = State->ActiveReceipt;
			}
			if (State->bAdmissionInProgress)
			{
				AdmissionReceipt = State->AdmissionReceipt;
			}
		}
	}
	if (!ImmediateError.IsEmpty())
	{
		OutError = ImmediateError;
		OutSubmission.Operation = RejectedStatus(Receipt, TEXT("service_shutting_down"), OutError);
		return false;
	}
	if (RejectedAttempt.IsSet())
	{
		if (!SameReceipt(RejectedAttempt->Receipt, Receipt))
		{
			OutError = TEXT("The rejected stage identity is bound to a different exact receipt.");
			OutSubmission.Operation = RejectedStatus(
				Receipt, TEXT("operation_id_conflict"), OutError);
			return false;
		}
		OutSubmission.Operation = RejectedAttempt->Status;
		OutError = RejectedAttempt->Status.Diagnostic.IsEmpty()
			? TEXT("The exact submission attempt was rejected before durable terminal adoption.")
			: RejectedAttempt->Status.Diagnostic;
		return false;
	}
	if (Archived.IsSet())
	{
		if (!SameReceipt(Archived->Receipt, Receipt))
		{
			OutError = TEXT("operation_id is archived with a different exact stage receipt.");
			OutSubmission.Operation = RejectedStatus(Receipt, TEXT("operation_id_conflict"), OutError);
			return false;
		}
		if (Archived->Status.bAccepted
			&& !VerifyReplayCredential(
				Archived->Safety,
				Archived->ReplayCredentialFingerprint,
				Receipt,
				AuthorizationToken))
		{
			OutError = TEXT("The archived risky operation requires the exact original replay credential.");
			OutSubmission.Operation = RejectedStatus(
				Receipt, TEXT("authorization_replay_mismatch"), OutError);
			return false;
		}
		OutSubmission.Operation = Archived->Status;
		OutSubmission.bOk = Archived->Status.bReplay
			|| Archived->Status.Status == TEXT("completed")
			|| Archived->Status.Status == TEXT("replay_completed");
		if (!OutSubmission.bOk)
		{
			OutError = Archived->Status.Diagnostic.IsEmpty()
				? TEXT("The exact archived operation ended without successful completion.")
				: Archived->Status.Diagnostic;
		}
		return OutSubmission.bOk;
	}
	if (ExistingSession.IsValid())
	{
		if (ExistingReceipt.OperationId == Receipt.OperationId && SameReceipt(ExistingReceipt, Receipt))
		{
			const FHyperAIStudioTypedArtifactAsyncSnapshot ExistingSnapshot =
				ExistingSession->GetSnapshot();
			if (!VerifyReplayCredential(
					ExistingSnapshot.Safety,
					ExistingSnapshot.ReplayCredentialFingerprint,
					Receipt,
					AuthorizationToken))
			{
				OutError = TEXT("The active risky operation requires the exact original replay credential.");
				OutSubmission.Operation = RejectedStatus(
					Receipt, TEXT("authorization_replay_mismatch"), OutError);
				return false;
			}
			OutSubmission.Operation = ToStatus(Receipt, ExistingSnapshot);
			OutSubmission.bOk = !ExistingSnapshot.bTerminal
				|| ExistingSnapshot.Result.bReplay
				|| ExistingSnapshot.Result.Status == TEXT("completed")
				|| ExistingSnapshot.Result.Status == TEXT("replay_completed");
			if (!OutSubmission.bOk)
			{
				OutError = OutSubmission.Operation.Diagnostic.IsEmpty()
					? TEXT("The exact active operation ended without successful completion.")
					: OutSubmission.Operation.Diagnostic;
			}
			return OutSubmission.bOk;
		}
		OutError = ExistingReceipt.OperationId == Receipt.OperationId
			? TEXT("operation_id is active with a different exact stage receipt.")
			: TEXT("Another mutation owns the one-at-a-time canonical project lane.");
		OutSubmission.Operation = RejectedStatus(
			Receipt,
			ExistingReceipt.OperationId == Receipt.OperationId
				? TEXT("operation_id_conflict") : TEXT("project_execution_busy"),
			OutError);
		return false;
	}
	if (!AdmissionReceipt.OperationId.IsEmpty())
	{
		if (SameReceipt(AdmissionReceipt, Receipt))
		{
			OutError = TEXT("The exact stage receipt already owns the bounded admission reservation; retry.");
			OutSubmission.Operation = AdmissionStatus(Receipt);
			return false;
		}
		OutError = TEXT("Another exact receipt owns the bounded project admission reservation.");
		OutSubmission.Operation = RejectedStatus(
			Receipt, TEXT("project_execution_busy"), OutError);
		return false;
	}
	if (Receipt.CanonicalProjectId != State->CanonicalProjectId)
	{
		OutError = TEXT("The stage receipt does not target the service-owned canonical project.");
		OutSubmission.Operation = RejectedStatus(Receipt, TEXT("project_identity_changed"), OutError);
		return false;
	}
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShuttingDown || State->Active.IsValid() || State->bAdmissionInProgress)
		{
			OutError = TEXT("The canonical project admission lane changed before it could be reserved.");
			OutSubmission.Operation = RejectedStatus(
				Receipt, TEXT("project_execution_race"), OutError);
			return false;
		}
		State->bAdmissionInProgress = true;
		State->AdmissionReceipt = Receipt;
	}

	FHyperAIStudioTypedArtifactClaim Claim;
	if (!State->Store->ClaimExact(Receipt, Claim, OutError))
	{
		const FHyperAIStudioTypedArtifactOperationStatus Failure = RejectedStatus(
			Receipt, TEXT("stage_claim_rejected"), OutError);
		{
			FScopeLock Lock(&State->Mutex);
			State->bAdmissionInProgress = false;
			State->AdmissionReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
			// Claim failures never establish durable operation identity. Preserve their bounded
			// response-loss recovery by full receipt so tampering cannot poison a live stage.
			ArchiveRejectedAttemptLocked(
				*State, Receipt, ReceiptFingerprint, Failure, EHyperAIStudioDomainSafety::Read);
		}
		OutSubmission.Operation = Failure;
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Session;
	const TSharedRef<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe> DispatchPermit =
		MakeShared<FServiceDispatchPermit, ESPMode::ThreadSafe>(
			TWeakPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe>(State),
			Receipt,
			ReceiptFingerprint);
	if (!FHyperAIStudioTypedArtifactExecutorInternal::CreateAsyncSession(
			MoveTemp(Claim), State->TrustedProjectRoot, AuthorizationToken, StateGate,
			DispatchPermit, Session, OutError))
	{
		const FHyperAIStudioTypedArtifactOperationStatus Failure = RejectedStatus(
			Receipt, TEXT("execution_session_rejected"), OutError);
		const EDurableIdentityProbe DurableIdentity = ProbeDurableIdentity(*State, Receipt.OperationId);
		{
			FScopeLock Lock(&State->Mutex);
			State->bAdmissionInProgress = false;
			State->AdmissionReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
			if (DurableIdentity == EDurableIdentityProbe::Absent)
			{
				ArchiveLocked(*State, Receipt, Failure);
			}
			else
			{
				ArchiveRejectedAttemptLocked(
					*State, Receipt, ReceiptFingerprint, Failure, EHyperAIStudioDomainSafety::Read);
			}
		}
		OutSubmission.Operation = Failure;
		return false;
	}

	const EHyperAIStudioTypedArtifactAsyncStartResult StartResult = Session->Start(OutError);
	const FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot = Session->GetSnapshot();
	FHyperAIStudioTypedArtifactOperationStatus Status = ToStatus(Receipt, Snapshot);
	if (StartResult != EHyperAIStudioTypedArtifactAsyncStartResult::Started || Snapshot.bTerminal)
	{
		const bool bRejectedTerminal =
			StartResult == EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
		// Release the session-owned journal/adapter lease before durable reconciliation. Journal
		// loading is an external callback boundary and must never occur under the service lock.
		Session.Reset();
		const EDurableIdentityProbe DurableIdentity = bRejectedTerminal && !Snapshot.bDurable
			? ProbeDurableIdentity(*State, Receipt.OperationId)
			: EDurableIdentityProbe::Absent;
		{
			FScopeLock Lock(&State->Mutex);
			State->bAdmissionInProgress = false;
			State->AdmissionReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
			if (bRejectedTerminal
				&& (Snapshot.bDurable || DurableIdentity != EDurableIdentityProbe::Absent))
			{
				// Keep any rejected attempt against a pre-existing durable identity recoverable
				// only by exact receipt. A wrong plan, capability, credential, or terminal-evidence
				// receipt must never poison the operation-level archive and hide a later exact replay.
				ArchiveRejectedAttemptLocked(
					*State, Receipt, ReceiptFingerprint, Status, Snapshot.Safety);
			}
			else
			{
				ArchiveLocked(
					*State, Receipt, Status,
					Snapshot.ReplayCredentialFingerprint, Snapshot.Safety);
			}
		}
		OutSubmission.Operation = Status;
		OutSubmission.bOk = StartResult == EHyperAIStudioTypedArtifactAsyncStartResult::ReplayTerminal
			&& Status.bReplay;
		if (!OutSubmission.bOk && OutError.IsEmpty())
		{
			OutError = Status.Diagnostic.IsEmpty()
				? TEXT("The durable exact operation ended without successful completion.")
				: Status.Diagnostic;
		}
		return OutSubmission.bOk;
	}

	bool bPublished = false;
	bool bQuiescedDuringAdmission = false;
	{
		FScopeLock Lock(&State->Mutex);
		bQuiescedDuringAdmission = State->bShuttingDown;
		bPublished = !State->bShuttingDown && !State->Active.IsValid()
			&& State->bAdmissionInProgress
			&& SameReceipt(State->AdmissionReceipt, Receipt);
		State->bAdmissionInProgress = false;
		State->AdmissionReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
		if (bPublished)
		{
			State->Active = Session;
			State->ActiveReceipt = Receipt;
			// UE's thread-safe ticker may execute a newly-added zero-delay delegate later in the
			// same outer Tick call. Consume one callback as an explicit no-pump admission barrier.
			State->bPumpAdmissionBarrier = true;
		}
	}
	if (!bPublished)
	{
		FString CancelError;
		if (!Session->IsTerminal())
		{
			Session->RequestCancel(
				TEXT("Typed-artifact admission reservation changed before publication"), CancelError);
		}
		const FHyperAIStudioTypedArtifactAsyncSnapshot CancelledSnapshot = Session->GetSnapshot();
		Status = ToStatus(Receipt, CancelledSnapshot);
		{
			FScopeLock Lock(&State->Mutex);
			ArchiveLocked(
				*State, Receipt, Status,
				CancelledSnapshot.ReplayCredentialFingerprint, CancelledSnapshot.Safety);
		}
		OutError = bQuiescedDuringAdmission
			? TEXT("The service quiesced during durable admission; the operation was closed fail-closed.")
			: TEXT("The admission reservation changed; the durable operation was closed fail-closed.");
		OutSubmission.Operation = Status;
		return false;
	}
	if (!ScheduleTicker(State.ToSharedRef()))
	{
		FString CancelError;
		if (!Session->IsTerminal())
		{
			Session->RequestCancel(
				TEXT("Typed-artifact ticker admission could not be published"), CancelError);
		}
		const FHyperAIStudioTypedArtifactAsyncSnapshot CancelledSnapshot = Session->GetSnapshot();
		Status = ToStatus(Receipt, CancelledSnapshot);
		TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Released;
		{
			FScopeLock Lock(&State->Mutex);
			ArchiveLocked(
				*State, Receipt, Status,
				CancelledSnapshot.ReplayCredentialFingerprint, CancelledSnapshot.Safety);
			if (State->Active.Get() == Session.Get())
			{
				Released = MoveTemp(State->Active);
				State->ActiveReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
				State->bPumpAdmissionBarrier = false;
			}
		}
		OutError = TEXT("The durable operation was closed because its game-thread ticker could not be installed.");
		OutSubmission.Operation = Status;
		return false;
	}
	Status = ToStatus(Receipt, Session->GetSnapshot());
	Status.Status = TEXT("accepted");
	Status.Diagnostic = TEXT("Durable admission succeeded; the first serial action starts on a later ticker turn.");
	Status.bAccepted = true;
	Status.bDurable = true;
	OutSubmission.Operation = Status;
	OutSubmission.bOk = true;
	return true;
}

bool FHyperAIStudioTypedArtifactExecutionService::StageAndSubmitExact(
	const FHyperAIStudioPreparedTypedArtifact& Prepared,
	const FString& OperationId,
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
	const FString& AuthorizationToken,
	const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
	FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
	FString& OutError)
{
	OutSubmission = FHyperAIStudioTypedArtifactSubmissionReceipt{};
	OutError.Reset();
	if (AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
	{
		OutError = TEXT("Typed-artifact authorization exceeds the bounded server grant envelope.");
		return false;
	}
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	if (!StageExact(Prepared, OperationId, Payload, Receipt, OutError))
	{
		return false;
	}
	return SubmitExact(Receipt, AuthorizationToken, StateGate, OutSubmission, OutError);
}

bool FHyperAIStudioTypedArtifactExecutionService::QueryStatus(
	const FString& OperationId,
	FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
	FString& OutError) const
{
	using namespace HyperAIStudio::TypedArtifactHost::Private;
	OutStatus = FHyperAIStudioTypedArtifactOperationStatus{};
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Typed-artifact status sampling and journal reconciliation require the Unreal game thread.");
		return false;
	}
	if (!FHyperAIStudioOperationJournal::IsValidOperationId(OperationId))
	{
		OutError = TEXT("A valid bounded operation_id is required.");
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Session;
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	TOptional<FArchivedStatus> Archived;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->Active.IsValid() && State->ActiveReceipt.OperationId == OperationId)
		{
			Session = State->Active;
			Receipt = State->ActiveReceipt;
		}
		else if (State->bAdmissionInProgress
			&& State->AdmissionReceipt.OperationId == OperationId)
		{
			Receipt = State->AdmissionReceipt;
		}
		else if (const FArchivedStatus* Existing = State->Archived.Find(OperationId))
		{
			Archived = *Existing;
		}
	}
	if (Session.IsValid())
	{
		OutStatus = ToStatus(Receipt, Session->GetSnapshot());
		return true;
	}
	if (!Receipt.OperationId.IsEmpty())
	{
		OutStatus = AdmissionStatus(Receipt);
		return true;
	}
	if (Archived.IsSet())
	{
		OutStatus = Archived->Status;
		return true;
	}

	// Journal Load is intentionally outside the service lock. It may perform restart
	// reconciliation, so this path is used only when no process-local operation is known.
	FHyperAIStudioOperationJournal Journal(State->TrustedProjectRoot);
	if (!Journal.Load(OutError))
	{
		return false;
	}
	const TOptional<FHyperAIStudioOperationRecord> Record = Journal.Find(OperationId);
	if (!Record.IsSet())
	{
		OutError = TEXT("No process-local or durable typed-artifact operation matches operation_id.");
		return false;
	}
	FillDurableStatus(Record.GetValue(), Journal.GetLastLoadReconciledCount(), OutStatus);
	return true;
}

bool FHyperAIStudioTypedArtifactExecutionService::Cancel(
	const FString& OperationId,
	const FString& Reason,
	FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifactHost::Private;
	OutStatus = FHyperAIStudioTypedArtifactOperationStatus{};
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Typed-artifact cancellation must run on the Unreal game thread.");
		return false;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Session;
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShuttingDown)
		{
			OutError = TEXT("The typed-artifact host is already quiescing.");
			return false;
		}
		if (State->Active.IsValid() && State->ActiveReceipt.OperationId == OperationId)
		{
			Session = State->Active;
			Receipt = State->ActiveReceipt;
		}
	}
	if (!Session.IsValid())
	{
		if (QueryStatus(OperationId, OutStatus, OutError))
		{
			OutError = TEXT("The operation is known but is not an active cancellable mutation.");
		}
		return false;
	}
	const bool bCancelled = Session->RequestCancel(Reason, OutError);
	const FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot = Session->GetSnapshot();
	OutStatus = ToStatus(Receipt, Snapshot);
	FTSTicker::FDelegateHandle TickerHandle;
	if (ReconcileTerminalSession(State.ToSharedRef(), Session, Receipt, Snapshot, TickerHandle)
		&& TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	return bCancelled;
}

int32 FHyperAIStudioTypedArtifactExecutionService::NumStaged() const
{
	return State->Store->NumStaged();
}

bool FHyperAIStudioTypedArtifactExecutionService::IsQuiescing() const
{
	FScopeLock Lock(&State->Mutex);
	return State->bShuttingDown;
}

bool FHyperAIStudioTypedArtifactExecutionService::CanShutdownSafely() const
{
	bool bTickerInstalled = false;
	bool bSessionRetained = false;
	int32 ActiveTickerCallbackCount = 0;
	int32 ActiveDispatchPermitCount = 0;
	{
		FScopeLock Lock(&State->Mutex);
		bSessionRetained = State->Active.IsValid();
		bTickerInstalled = State->TickerHandle.IsValid();
		ActiveTickerCallbackCount = State->ActiveTickerCallbackCount;
		ActiveDispatchPermitCount = State->ActiveDispatchPermitCount;
		if (State->bAdmissionInProgress)
		{
			bTickerInstalled = true;
		}
	}
	return !bTickerInstalled && ActiveTickerCallbackCount == 0 && ActiveDispatchPermitCount == 0
		&& !bSessionRetained;
}

void FHyperAIStudioTypedArtifactExecutionService::Shutdown()
{
	using namespace HyperAIStudio::TypedArtifactHost::Private;
	if (!State.IsValid())
	{
		return;
	}
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Session;
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bTickerCallbackActive = false;
	{
		FScopeLock Lock(&State->Mutex);
		State->bShuttingDown = true;
		Session = State->Active;
		Receipt = State->ActiveReceipt;
		TickerHandle = State->TickerHandle;
		bTickerCallbackActive = State->ActiveTickerCallbackCount > 0;
	}
	State->Store->Reset();
	if (!IsInGameThread() || bTickerCallbackActive)
	{
		// The tracked outer ticker owns cancellation/session sampling. Never re-enter the serial
		// runtime from an injected callback or race it from a destructor thread.
		return;
	}
	if (Session.IsValid() && !Session->IsTerminal())
	{
		FString Ignore;
		Session->RequestCancel(TEXT("HyperAIStudio typed-artifact host shutdown"), Ignore);
	}
	if (Session.IsValid())
	{
		const FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot = Session->GetSnapshot();
		const FHyperAIStudioTypedArtifactOperationStatus Status = ToStatus(Receipt, Snapshot);
		const bool bRequiresDrain = Snapshot.bOutstandingDispatch || !Snapshot.bTerminal;
		TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe> Released;
		{
			FScopeLock Lock(&State->Mutex);
			ArchiveLocked(
				*State, Receipt, Status,
				Snapshot.ReplayCredentialFingerprint, Snapshot.Safety);
			if (!bRequiresDrain && State->Active.Get() == Session.Get())
			{
				Released = MoveTemp(State->Active);
				State->ActiveReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
				State->bPumpAdmissionBarrier = false;
			}
		}
		if (bRequiresDrain)
		{
			// The installed ticker becomes a drain-only quiesce lane. Its shared state capture keeps
			// every collaborator and module lease alive until cancellation terminalizes and any late
			// callback has been observed.
			return;
		}
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		FScopeLock Lock(&State->Mutex);
		if (State->TickerHandle == TickerHandle)
		{
			State->TickerHandle.Reset();
		}
	}
}
