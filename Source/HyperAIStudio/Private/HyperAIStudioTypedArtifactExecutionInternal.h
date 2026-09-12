// Games by Hyper 2026.

#pragma once

#include "HyperAIStudioTypedArtifactExecution.h"

class IHyperAIStudioTypedArtifactClock
{
public:
	virtual ~IHyperAIStudioTypedArtifactClock() = default;
	virtual int64 NowMonotonicMs() const = 0;
	virtual int64 NowUtcMs() const = 0;
};

/** Core-owned state seam; optional packs cannot provide admission or prerequisite snapshots. */
class IHyperAIStudioTypedArtifactStateGate
{
public:
	virtual ~IHyperAIStudioTypedArtifactStateGate() = default;
	virtual bool Revalidate(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const IHyperAIStudioTypedArtifactPayload& Payload,
		EHyperAIStudioDomainExecutionActionKind ActionKind,
		FHyperAIStudioDomainPrerequisiteSnapshot& OutPrerequisites,
		FHyperAIStudioDomainAdmissionSnapshot& OutAdmission,
		FString& OutError) = 0;
	virtual bool VerifyFresh(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const IHyperAIStudioTypedArtifactPayload& Payload,
		const IHyperAIStudioDomainResultPayload& AdapterResult,
		FString& OutPostconditionHash,
		FString& OutError) = 0;
};

struct FHyperAIStudioTypedArtifactClaimState;
struct FHyperAIStudioTypedArtifactStoreState;
class IHyperAIStudioTypedArtifactAsyncSession;
class IHyperAIStudioTypedArtifactDispatchPermit;

class FHyperAIStudioTypedArtifactClaim
{
public:
	FHyperAIStudioTypedArtifactClaim();
	~FHyperAIStudioTypedArtifactClaim();
	FHyperAIStudioTypedArtifactClaim(FHyperAIStudioTypedArtifactClaim&& Other) noexcept;
	FHyperAIStudioTypedArtifactClaim& operator=(FHyperAIStudioTypedArtifactClaim&& Other) noexcept;
	FHyperAIStudioTypedArtifactClaim(const FHyperAIStudioTypedArtifactClaim&) = delete;
	FHyperAIStudioTypedArtifactClaim& operator=(const FHyperAIStudioTypedArtifactClaim&) = delete;
	bool IsValid() const;
	void Reset();

private:
	friend class FHyperAIStudioTypedArtifactStore;
	friend class FHyperAIStudioTypedArtifactExecutorInternal;
	TUniquePtr<FHyperAIStudioTypedArtifactClaimState> State;
};

class FHyperAIStudioTypedArtifactStore final
{
public:
	explicit FHyperAIStudioTypedArtifactStore(FHyperAIStudioDomainAdapterRegistry& Registry);
	FHyperAIStudioTypedArtifactStore(
		FHyperAIStudioDomainAdapterRegistry& Registry,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock);
	~FHyperAIStudioTypedArtifactStore();
	FHyperAIStudioTypedArtifactStore(const FHyperAIStudioTypedArtifactStore&) = delete;
	FHyperAIStudioTypedArtifactStore& operator=(const FHyperAIStudioTypedArtifactStore&) = delete;

	bool StageExact(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const FString& OperationId,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
		FString& OutError);
	bool ClaimExact(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FHyperAIStudioTypedArtifactClaim& OutClaim,
		FString& OutError);
	int32 NumStaged() const;
	void Reset();

private:
	TUniquePtr<FHyperAIStudioTypedArtifactStoreState> State;
};

class FHyperAIStudioTypedArtifactExecutorInternal final
{
public:
	static bool ExecuteClaimed(
		FHyperAIStudioTypedArtifactClaim&& Claim,
		const FString& ProjectRoot,
		const FString& AuthorizationToken,
		IHyperAIStudioTypedArtifactStateGate& StateGate,
		FHyperAIStudioTypedArtifactExecutionResult& OutResult,
		FString& OutError);
	static bool CreateAsyncSession(
		FHyperAIStudioTypedArtifactClaim&& Claim,
		const FString& TrustedProjectRoot,
		const FString& AuthorizationToken,
		const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
		const TSharedRef<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe>& DispatchPermit,
		TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe>& OutSession,
		FString& OutError);
};

/** Bounded process-local view used only between the sealed executor and its core ticker host. */
struct FHyperAIStudioTypedArtifactAsyncSnapshot
{
	FHyperAIStudioTypedArtifactExecutionResult Result;
	FString ActiveActionKind;
	FString ActiveStepId;
	/** Server-derived exact bearer binding; the raw bearer is erased before admission returns. */
	FString ReplayCredentialFingerprint;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read;
	bool bStarted = false;
	bool bAccepted = false;
	bool bTerminal = false;
	bool bOutstandingDispatch = false;
	bool bDurable = false;
	int32 LateResultCount = 0;
};

enum class EHyperAIStudioTypedArtifactAsyncStartResult : uint8
{
	Started,
	ReplayTerminal,
	RejectedTerminal
};

/** RAII proof that one exact service-owned dispatch won the race against quiesce. */
class IHyperAIStudioTypedArtifactDispatchLease
{
public:
	virtual ~IHyperAIStudioTypedArtifactDispatchLease() = default;
};

/** Core-private quiesce gate. Optional packs cannot provide or bypass this permit. */
class IHyperAIStudioTypedArtifactDispatchPermit
{
public:
	virtual ~IHyperAIStudioTypedArtifactDispatchPermit() = default;
	virtual bool AcquireBeforeCommit(
		TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe>& OutLease,
		FString& OutError) const = 0;
	virtual bool AcquireBeforeDispatch(
		TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe>& OutLease,
		FString& OutError) const = 0;
};

/** Core-private non-waiting session. Every method is serialized on the game thread. */
class IHyperAIStudioTypedArtifactAsyncSession
{
public:
	virtual ~IHyperAIStudioTypedArtifactAsyncSession() = default;
	virtual EHyperAIStudioTypedArtifactAsyncStartResult Start(FString& OutError) = 0;
	/** Admit at most one already-sealed action; completion may arrive asynchronously. */
	virtual bool PumpOne(FString& OutError) = 0;
	virtual void TickDeadline() = 0;
	virtual bool RequestCancel(const FString& Reason, FString& OutError) = 0;
	virtual FHyperAIStudioTypedArtifactAsyncSnapshot GetSnapshot() const = 0;
	virtual bool IsTerminal() const = 0;
	virtual bool HasOutstandingDispatch() const = 0;
};

namespace HyperAIStudio::TypedArtifact::Private
{
	/** Private bridge keeps the exact mutation-lease acquisition absent from the exported registry API. */
	class FRegistryExecutionAccess final
	{
	public:
		static FHyperAIStudioDomainResolveResult Acquire(
			FHyperAIStudioDomainAdapterRegistry& Registry,
			const FHyperAIStudioDomainRequestEnvelope& Request,
			FHyperAIStudioDomainExecutionLease& OutLease)
		{
			return Registry.AcquireExecutionLease(Request, OutLease);
		}
	};

	/** Full bounded receipt validation before any host copy, lookup or reservation. */
	bool ValidateStageReceiptBounds(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FString& OutError);
	/** Bounded exact rejected-attempt key; empty when any receipt binding is malformed. */
	FString BuildStageReceiptFingerprint(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt);
	/** Process-local replay binding; returns empty for malformed bearer or receipt material. */
	FString BuildReplayCredentialFingerprint(
		const FString& AuthorizationToken,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt);
}
