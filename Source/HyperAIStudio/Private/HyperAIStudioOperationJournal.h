// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

enum class EHyperAIStudioOperationState : uint8
{
	Queued,
	Running,
	CommitStarted,
	Completed,
	Failed,
	CancelRequested,
	RolledBack,
	Partial,
	OutcomeUnknown
};

enum class EHyperAIStudioRollbackState : uint8
{
	NotNeeded,
	Complete,
	Partial,
	Unsupported,
	Unknown
};

enum class EHyperAIStudioOperationBeginResult : uint8
{
	Created,
	Existing,
	ReplayCompleted,
	Conflict,
	Invalid,
	CapacityExceeded,
	OwnerUnavailable,
	PersistenceFailure
};

/** Evidence-gated terminal resolutions for a previously unknown mutation outcome. */
enum class EHyperAIStudioUnknownResolution : uint8
{
	Completed,
	RolledBack,
	Partial
};

/** Server-verified validator receipt claims safe to persist (the bearer/signature itself is never stored). */
struct FHyperAIStudioOperationEvidence
{
	static constexpr int32 CurrentVersion = 1;

	int32 Version = 0;
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString ActionNonce;
	FString ValidatorId;
	FString ApprovedValidatorFingerprint;
	FString PostconditionHash;
	FString ReceiptFingerprint;
	int64 IssuedUtcMs = 0;
	int64 ExpiresUtcMs = 0;

	bool IsPresent() const { return Version != 0; }
};

struct FHyperAIStudioOperationRecord
{
	FString OperationId;
	FString CanonicalProjectId;
	FString PlanHash;
	FString ExecutionInstanceId;
	FString CapabilityHash;
	EHyperAIStudioOperationState State = EHyperAIStudioOperationState::Queued;
	EHyperAIStudioRollbackState RollbackState = EHyperAIStudioRollbackState::NotNeeded;
	FString CreatedUtc;
	FString UpdatedUtc;
	FString StatusCode;
	FString ResolutionValidatorHash;
	FString ResolutionPostconditionHash;
	FString ResolvedByExecutionInstanceId;
	FHyperAIStudioOperationEvidence TerminalEvidence;
	FString EvidenceValidatedByExecutionInstanceId;
	int64 EvidenceAcceptedUtcMs = 0;
	bool bRetrySafe = false;
	bool bPartialCommit = false;

	bool IsTerminal() const;
	bool NeedsReconciliation() const;
};

/**
 * Durable, bounded, per-project idempotency journal for HyperAI-managed mutations.
 *
 * Persistence uses two complete generations. Saving only overwrites the inactive/older
 * generation and promotes it after a read-back checksum succeeds, so the last validated
 * generation remains recoverable from detected corruption. This is not yet an fsync-backed
 * power-loss durability claim. A system-wide mutex permits one journal owner per canonical
 * physical project directory and prevents stale multi-editor overwrites.
 *
 * The journal stores no tool arguments, prompts, secrets, or arbitrary payloads.
 */
class FHyperAIStudioOperationJournal
{
public:
	explicit FHyperAIStudioOperationJournal(const FString& InProjectRoot, int32 InMaxRecords = 512);
	~FHyperAIStudioOperationJournal();

	FHyperAIStudioOperationJournal(const FHyperAIStudioOperationJournal&) = delete;
	FHyperAIStudioOperationJournal& operator=(const FHyperAIStudioOperationJournal&) = delete;

	/** Must succeed before any read-modify-write operation is permitted. */
	bool Load(FString& OutError);

	/**
	 * Loads the newest validated generation without taking mutation ownership, reconciliation, or
	 * writing either slot. Intended for status/read tools while another editor owner is active.
	 */
	bool LoadReadOnly(FString& OutError);

	EHyperAIStudioOperationBeginResult BeginOperation(
		const FString& OperationId,
		const FString& PlanHash,
		const FString& CapabilityHash,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);

	bool Transition(
		const FString& OperationId,
		EHyperAIStudioOperationState ExpectedState,
		EHyperAIStudioOperationState NewState,
		EHyperAIStudioRollbackState RollbackState,
		bool bRetrySafe,
		bool bPartialCommit,
		const FString& StatusCode,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);
	/**
	 * Core-internal durable certificate for CommitStarted -> Failed when a dispatcher returns false
	 * synchronously under the strict no-effect/no-callback contract.
	 */
	bool TransitionCertifiedNoEffect(
		const FString& OperationId,
		const FString& EffectFingerprint,
		const FString& ActionNonce,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);
	bool Transition(
		const FString& OperationId,
		EHyperAIStudioOperationState ExpectedState,
		EHyperAIStudioOperationState NewState,
		EHyperAIStudioRollbackState RollbackState,
		bool bRetrySafe,
		bool bPartialCommit,
		const FString& StatusCode,
		const FHyperAIStudioOperationEvidence& Evidence,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);

	/**
	 * Resolves outcome_unknown only after an allowlisted validator produced durable postcondition evidence.
	 * The trusted execution-instance identity is generated and attached by the journal, never supplied by a tool client.
	 */
	bool ResolveUnknownWithEvidence(
		const FString& OperationId,
		EHyperAIStudioUnknownResolution Resolution,
		const FString& ValidatorHash,
		const FString& PostconditionHash,
		EHyperAIStudioRollbackState PartialRollbackState,
		const FString& StatusCode,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);
	bool ResolveUnknownWithEvidence(
		const FString& OperationId,
		EHyperAIStudioUnknownResolution Resolution,
		const FHyperAIStudioOperationEvidence& Evidence,
		EHyperAIStudioRollbackState PartialRollbackState,
		const FString& StatusCode,
		FHyperAIStudioOperationRecord& OutRecord,
		FString& OutError);

	TOptional<FHyperAIStudioOperationRecord> Find(const FString& OperationId) const;
	TArray<FHyperAIStudioOperationRecord> GetRecordsSnapshot() const;
	bool IsLoaded() const;
	int64 GetGeneration() const;
	int32 GetLastLoadReconciledCount() const;
	const FString& GetSlotAPath() const { return SlotAPath; }
	const FString& GetSlotBPath() const { return SlotBPath; }
	const FString& GetCanonicalProjectId() const { return CanonicalProjectId; }
	const FString& GetTrustedExecutionInstanceId() const { return TrustedExecutionInstanceId; }

	/**
	 * Resolves an existing project directory to a path-independent identity and hashes it as UTF-8.
	 * Returns an empty string when a strong identity cannot be established; callers must fail closed.
	 */
	static FString MakeCanonicalProjectId(const FString& ProjectRoot);
	static bool IsValidOperationId(const FString& OperationId);
	static bool IsValidStatusCode(const FString& StatusCode);
	static const TCHAR* LexToString(EHyperAIStudioOperationState State);
	static const TCHAR* LexToString(EHyperAIStudioRollbackState State);

private:
	enum class EJournalSlot : uint8
	{
		None,
		A,
		B
	};

	struct FLoadedGeneration
	{
		bool bExists = false;
		int32 SchemaVersion = 0;
		int64 Generation = 0;
		FString Checksum;
		TArray<FHyperAIStudioOperationRecord> Records;
	};

	bool Save(FString& OutError);
	bool LoadInternal(bool bForMutation, FString& OutError);
	bool VerifyCanonicalProjectIdentity(FString& OutError) const;
	bool LoadGeneration(const FString& Path, FLoadedGeneration& OutGeneration, FString& OutError) const;
	bool SerializeGeneration(int64 InGeneration, const TArray<FHyperAIStudioOperationRecord>& InRecords, FString& OutJson, FString& OutChecksum, FString& OutError) const;
	bool ValidateRecord(const FHyperAIStudioOperationRecord& Record, FString& OutError) const;
	bool ValidateEvidenceForRecord(
		const FHyperAIStudioOperationRecord& Record,
		const FHyperAIStudioOperationEvidence& Evidence,
		int64 AcceptedUtcMs,
		FString& OutError) const;
	static bool ValidateStateInvariants(const FHyperAIStudioOperationRecord& Record, FString& OutError);
	bool ReconcileUnfinishedForCurrentOwner(int32& OutReconciledCount, FString& OutError);
	static bool IsLegalTransition(EHyperAIStudioOperationState From, EHyperAIStudioOperationState To);
	static bool TryParseOperationState(const FString& Value, EHyperAIStudioOperationState& OutState);
	static bool TryParseRollbackState(const FString& Value, EHyperAIStudioRollbackState& OutState);
	static bool IsValidSha256Token(const FString& Value);
	static bool IsValidExecutionInstanceId(const FString& Value);
	static bool IsValidEvidenceNonce(const FString& Value);
	static bool IsValidValidatorId(const FString& Value);
	static FString ComputeGenerationChecksum(
		int32 InSchemaVersion,
		int64 InGeneration,
		const FString& InCanonicalProjectId,
		const TArray<FHyperAIStudioOperationRecord>& InRecords);
	static bool TryResolveProjectIdentity(
		const FString& InProjectRoot,
		FString& OutResolvedProjectRoot,
		FString& OutCanonicalProjectId,
		FString& OutError);

	FString ProjectRoot;
	FString CanonicalProjectId;
	FString CanonicalIdentityError;
	FString TrustedExecutionInstanceId;
	FString SlotAPath;
	FString SlotBPath;
	int32 MaxRecords = 512;
	int64 Generation = 0;
	EJournalSlot ActiveSlot = EJournalSlot::None;
	TArray<FHyperAIStudioOperationRecord> Records;
	TUniquePtr<FSystemWideCriticalSection> SystemWideMutex;
	bool bLoaded = false;
	bool bOwnershipReconciled = false;
	int32 LastLoadReconciledCount = 0;
	mutable FCriticalSection Mutex;
};
