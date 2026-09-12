// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * A managed marker file is editable only when it has no markers or exactly one
 * ordered pair. Every other state is deliberately fail-closed.
 */
enum class EHyperAIStudioManagedBlockState : uint8
{
	InvalidMarkers,
	InputTooLarge,
	Missing,
	ValidSinglePair,
	BeginWithoutEnd,
	EndWithoutBegin,
	Reversed,
	Nested,
	DuplicatePairs,
	DuplicateMarkers
};

struct FHyperAIStudioManagedBlockParseResult
{
	EHyperAIStudioManagedBlockState State = EHyperAIStudioManagedBlockState::InvalidMarkers;
	FString Prefix;
	FString InclusiveBlock;
	FString Suffix;
	FString ReasonCode;
	int32 BeginOffset = INDEX_NONE;
	int32 EndOffsetExclusive = INDEX_NONE;

	bool HasEditableSinglePair() const
	{
		return State == EHyperAIStudioManagedBlockState::ValidSinglePair;
	}

	bool MayAppendNewBlock() const
	{
		return State == EHyperAIStudioManagedBlockState::Missing;
	}
};

class FHyperAIStudioManagedBlockParser final
{
public:
	static constexpr int32 MaxTextCharacters = 4 * 1024 * 1024;
	static constexpr int32 MaxMarkerCharacters = 1024;

	/** Prefix and suffix are exact FString slices; this function performs no I/O or newline normalization. */
	static FHyperAIStudioManagedBlockParseResult Parse(
		const FString& Source,
		const FString& BeginMarker,
		const FString& EndMarker);
};

enum class EHyperAIStudioCodexTomlCollisionState : uint8
{
	Safe,
	Collision,
	Rejected
};

struct FHyperAIStudioCodexTomlCollisionInspection
{
	EHyperAIStudioCodexTomlCollisionState State = EHyperAIStudioCodexTomlCollisionState::Rejected;
	TArray<FString> CollidingServerIds;
	FString ReasonCode;

	bool MayWriteManagedBlock() const
	{
		return State == EHyperAIStudioCodexTomlCollisionState::Safe;
	}
};

/**
 * Pure, bounded inspection of Codex TOML before a managed block is added or
 * replaced. Only definitions outside the exact managed marker pair participate
 * in collision detection. Unsupported or structurally ambiguous TOML is
 * rejected so callers never need to guess whether a write is safe.
 */
class FHyperAIStudioCodexTomlCollisionInspector final
{
public:
	static constexpr int32 MaxTomlCharacters = 4 * 1024 * 1024;
	static constexpr int32 MaxDesiredServerIds = 1024;
	static constexpr int32 MaxServerIdCharacters = 256;
	static constexpr int32 MaxStatementCharacters = 256 * 1024;
	static constexpr int32 MaxStatements = 65536;
	static constexpr int32 MaxKeySegments = 64;
	static constexpr int32 MaxKeySegmentCharacters = 1024;
	static constexpr int32 MaxNestingDepth = 64;

	static FHyperAIStudioCodexTomlCollisionInspection Inspect(
		const FString& Source,
		const FString& BeginMarker,
		const FString& EndMarker,
		const TArray<FString>& DesiredServerIds);
};

struct FHyperAIStudioCanonicalJsonResult
{
	bool bSuccess = false;
	FString CanonicalJson;
	FString ObjectHash;
	FString ReasonCode;
};

/** Strict, bounded canonicalization for ownership identity. The root must be a JSON object. */
class FHyperAIStudioCanonicalJson final
{
public:
	static constexpr int32 MaxInputUtf8Bytes = 2 * 1024 * 1024;
	static constexpr int32 MaxCanonicalUtf8Bytes = 2 * 1024 * 1024;
	static constexpr int32 MaxDepth = 64;
	static constexpr int32 MaxNodes = 65536;

	static FHyperAIStudioCanonicalJsonResult HashObject(const FString& JsonText);
	static bool IsValidObjectHash(const FString& ObjectHash);
};

struct FHyperAIStudioManagedConfigLedgerEntry
{
	FString Id;
	FString ObjectHash;
	int32 WriterVersion = 0;
};

struct FHyperAIStudioManagedConfigLedgerClient
{
	FString ClientId;
	FString RelativePath;
	FString Container;
	TArray<FHyperAIStudioManagedConfigLedgerEntry> Entries;
};

/**
 * Ownership metadata only. The model intentionally has no command, argument,
 * environment, URL, token, or arbitrary metadata fields.
 */
struct FHyperAIStudioManagedConfigOwnershipLedger
{
	int32 SchemaVersion = 1;
	FString Generator = TEXT("HyperAIStudio");
	TArray<FHyperAIStudioManagedConfigLedgerClient> Clients;

	const FHyperAIStudioManagedConfigLedgerEntry* FindEntry(
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container,
		const FString& EntryId) const;
};

class FHyperAIStudioManagedConfigLedgerCodec final
{
public:
	static constexpr int32 CurrentSchemaVersion = 1;
	static constexpr int32 CurrentWriterVersion = 1;
	static constexpr int32 MaxLedgerUtf8Bytes = 1024 * 1024;
	static constexpr int32 MaxClients = 4;
	static constexpr int32 MaxEntriesPerClient = 1024;
	static constexpr int32 MaxClientIdCharacters = 32;
	static constexpr int32 MaxRelativePathCharacters = 512;
	static constexpr int32 MaxContainerCharacters = 64;
	static constexpr int32 MaxEntryIdCharacters = 256;
	static constexpr int32 MaxWriterVersion = CurrentWriterVersion;

	/** Strictly rejects unknown fields so a ledger cannot smuggle configuration or secrets. */
	static bool Parse(
		const FString& LedgerJson,
		FHyperAIStudioManagedConfigOwnershipLedger& OutLedger,
		FString& OutReasonCode);

	/** Deterministic, compact serialization after validating every ownership coordinate. */
	static bool Serialize(
		const FHyperAIStudioManagedConfigOwnershipLedger& Ledger,
		FString& OutLedgerJson,
		FString& OutReasonCode);
};

enum class EHyperAIStudioTransactionEvidenceMatch : uint8
{
	ExactOriginal,
	ExactDesired,
	Conflict
};

struct FHyperAIStudioManagedConfigPendingTarget
{
	FString ClientId;
	FString RelativePath;
	FString Container;
	bool bOriginalExists = false;
	FString OriginalFileHash;
	bool bDesiredExists = false;
	FString DesiredFileHash;
	TArray<FHyperAIStudioManagedConfigLedgerEntry> DesiredLedgerEntries;
};

/**
 * Payload-free crash evidence for one managed JSON transaction. File contents,
 * commands, URLs, arguments, environment values, and secrets are never stored.
 */
struct FHyperAIStudioManagedConfigPendingTransaction
{
	int32 SchemaVersion = 1;
	FString Generator = TEXT("HyperAIStudio");
	FString TransactionId;
	FString Phase = TEXT("idle");
	TArray<FHyperAIStudioManagedConfigPendingTarget> Targets;

	bool IsIdle() const { return Phase == TEXT("idle"); }
	const FHyperAIStudioManagedConfigPendingTarget* FindTarget(
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container) const;
};

class FHyperAIStudioManagedConfigPendingCodec final
{
public:
	static constexpr int32 CurrentSchemaVersion = 1;
	static constexpr int32 MaxEvidenceUtf8Bytes = 1024 * 1024;
	static constexpr int32 MaxTargets = 4;
	static constexpr int32 MaxTransactionIdCharacters = 32;

	static bool Parse(
		const FString& Json,
		FHyperAIStudioManagedConfigPendingTransaction& OutEvidence,
		FString& OutReasonCode);
	static bool Serialize(
		const FHyperAIStudioManagedConfigPendingTransaction& Evidence,
		FString& OutJson,
		FString& OutReasonCode);
	static FString HashBytes(const TArray<uint8>& Bytes);
	static EHyperAIStudioTransactionEvidenceMatch Classify(
		const FHyperAIStudioManagedConfigPendingTarget& Target,
		bool bCurrentExists,
		const FString& CurrentFileHash);
};

/** Payload-free evidence written before replacing GEditorPerProjectIni. */
struct FHyperAIStudioManagedConfigSettingsTransaction
{
	int32 SchemaVersion = 1;
	FString Generator = TEXT("HyperAIStudio");
	FString TransactionId;
	FString Phase = TEXT("idle");
	bool bOriginalExists = false;
	FString OriginalFileHash;
	FString StagedFileHash;

	bool IsIdle() const { return Phase == TEXT("idle"); }
};

class FHyperAIStudioManagedConfigSettingsCodec final
{
public:
	static constexpr int32 CurrentSchemaVersion = 1;
	static constexpr int32 MaxEvidenceUtf8Bytes = 16 * 1024;

	static bool Parse(
		const FString& Json,
		FHyperAIStudioManagedConfigSettingsTransaction& OutEvidence,
		FString& OutReasonCode);
	static bool Serialize(
		const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
		FString& OutJson,
		FString& OutReasonCode);
	static EHyperAIStudioTransactionEvidenceMatch Classify(
		const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
		bool bCurrentExists,
		const FString& CurrentFileHash);
};

enum class EHyperAIStudioJsonEntryState : uint8
{
	MissingFile,
	EmptyFile,
	InvalidJson,
	RootNotObject,
	ContainerMissing,
	ContainerNotObject,
	EntryMissing,
	EntryNotObject,
	EntryObject
};

struct FHyperAIStudioJsonEntryInspection
{
	EHyperAIStudioJsonEntryState State = EHyperAIStudioJsonEntryState::InvalidJson;
	FString ExistingObjectHash;
	FString ReasonCode;
};

/** Read-only inspection of one exact container/id coordinate in an already-loaded JSON file. */
class FHyperAIStudioJsonEntryInspector final
{
public:
	static FHyperAIStudioJsonEntryInspection InspectExistingFile(
		const FString& JsonText,
		const FString& Container,
		const FString& EntryId);
};

enum class EHyperAIStudioManagedEntryDecision : uint8
{
	Preserve,
	Write,
	Remove,
	Conflict,
	NoOp
};

enum class EHyperAIStudioManagedEntryOwnership : uint8
{
	None,
	LedgerExact,
	HistoricalExact,
	LedgerAndHistoricalExact
};

/**
 * Pure inputs for one file/container/id decision. LedgerObjectHash must come
 * from an exact client/path/container/id lookup. Exact historical evidence is
 * transient: the caller must have matched both the legacy settings fingerprint
 * and the known historical object for this same client/profile.
 */
struct FHyperAIStudioManagedEntryReconcileInput
{
	bool bClientEnabledForGeneration = false;
	bool bDesiredEntry = false;
	bool bAllowReplaceUnownedDesired = false;
	EHyperAIStudioJsonEntryState ExistingState = EHyperAIStudioJsonEntryState::MissingFile;
	FString ExistingObjectHash;
	FString DesiredObjectHash;
	FString LedgerObjectHash;
	bool bExactHistoricalSettingsOwnership = false;
	FString HistoricalObjectHash;
};

struct FHyperAIStudioManagedEntryReconcileResult
{
	EHyperAIStudioManagedEntryDecision Decision = EHyperAIStudioManagedEntryDecision::Conflict;
	EHyperAIStudioManagedEntryOwnership Ownership = EHyperAIStudioManagedEntryOwnership::None;
	FString ReasonCode;
	bool bMayRecordLedgerAfterSuccessfulWrite = false;
	bool bDropLedgerRecordAfterSuccess = false;
	bool bBlocksMigration = false;
};

/** No I/O and no mutation: this planner only classifies the next safe action. */
class FHyperAIStudioManagedEntryReconciler final
{
public:
	static FHyperAIStudioManagedEntryReconcileResult Plan(
		const FHyperAIStudioManagedEntryReconcileInput& Input);
};
