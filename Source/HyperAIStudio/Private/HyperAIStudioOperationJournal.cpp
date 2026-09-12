// Games by Hyper 2026.

#include "HyperAIStudioOperationJournal.h"

#include "HyperAIStudioExtensionRuntime.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace HyperAIStudio::OperationJournal::Private
{
	constexpr int32 LegacySchemaVersion = 2;
	constexpr int32 SchemaVersion = 3;
	constexpr int64 MaxJournalBytes = 4 * 1024 * 1024;
	constexpr int64 CertifiedNoEffectLifetimeMs = 60 * 1000;
	constexpr const TCHAR* CertifiedNoEffectStatus = TEXT("certified_no_effect");
	constexpr const TCHAR* CertifiedNoEffectValidatorId = TEXT("hyperai.core.certified_no_effect");
#if PLATFORM_WINDOWS
	constexpr uint32 MaxResolvedProjectPathCharacters = 32768;
#endif

	FString NowUtc()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	int64 NowUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}

	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Field, OutValue) && !OutValue.IsEmpty();
	}

	bool TryParseCanonicalNonNegativeInt64(const FString& Text, int64& OutValue)
	{
		if (Text.IsEmpty() || (Text.Len() > 1 && Text[0] == TEXT('0')))
		{
			return false;
		}
		int64 Value = 0;
		for (const TCHAR Character : Text)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const int32 Digit = Character - TEXT('0');
			if (Value > (TNumericLimits<int64>::Max() - Digit) / 10)
			{
				return false;
			}
			Value = Value * 10 + Digit;
		}
		OutValue = Value;
		return true;
	}

	void AppendChecksumToken(FString& Buffer, const FString& Value)
	{
		Buffer += FString::FromInt(Value.Len());
		Buffer += TEXT(":");
		Buffer += Value;
		Buffer += TEXT("|");
	}

	FString Sha1Utf8(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		FString Hex;
		Hex.Reserve(FSHA1::DigestSize * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	FString Sha256Utf8(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FString CertifiedNoEffectValidatorFingerprint()
	{
		static const FString Fingerprint = Sha256Utf8(TEXT("hyperai.core.certified-no-effect.validator.v1"));
		return Fingerprint;
	}

	FString BuildCertifiedNoEffectPostcondition(
		const FHyperAIStudioOperationRecord& Record,
		const FString& EffectFingerprint,
		const FString& ActionNonce)
	{
		FString Canonical;
		AppendChecksumToken(Canonical, TEXT("hyperai.certified-no-effect.v1"));
		AppendChecksumToken(Canonical, Record.CanonicalProjectId);
		AppendChecksumToken(Canonical, Record.OperationId);
		AppendChecksumToken(Canonical, Record.PlanHash);
		AppendChecksumToken(Canonical, Record.CapabilityHash);
		AppendChecksumToken(Canonical, EffectFingerprint);
		AppendChecksumToken(Canonical, ActionNonce);
		return Sha256Utf8(Canonical);
	}

	FString BuildCertifiedNoEffectReceiptFingerprint(const FHyperAIStudioOperationEvidence& Evidence)
	{
		FString Canonical;
		AppendChecksumToken(Canonical, TEXT("hyperai.certified-no-effect.receipt.v1"));
		AppendChecksumToken(Canonical, Evidence.CanonicalProjectId);
		AppendChecksumToken(Canonical, Evidence.OperationId);
		AppendChecksumToken(Canonical, Evidence.PlanHash);
		AppendChecksumToken(Canonical, Evidence.CapabilityHash);
		AppendChecksumToken(Canonical, Evidence.EffectFingerprint);
		AppendChecksumToken(Canonical, Evidence.ActionNonce);
		AppendChecksumToken(Canonical, Evidence.ValidatorId);
		AppendChecksumToken(Canonical, Evidence.ApprovedValidatorFingerprint);
		AppendChecksumToken(Canonical, Evidence.PostconditionHash);
		AppendChecksumToken(Canonical, FString::Printf(TEXT("%lld"), static_cast<long long>(Evidence.IssuedUtcMs)));
		AppendChecksumToken(Canonical, FString::Printf(TEXT("%lld"), static_cast<long long>(Evidence.ExpiresUtcMs)));
		return Sha256Utf8(Canonical);
	}

	bool IsCertifiedNoEffectEvidence(
		const FHyperAIStudioOperationRecord& Record,
		const FHyperAIStudioOperationEvidence& Evidence)
	{
		return Evidence.Version == FHyperAIStudioOperationEvidence::CurrentVersion
			&& Evidence.ValidatorId == CertifiedNoEffectValidatorId
			&& Evidence.ApprovedValidatorFingerprint == CertifiedNoEffectValidatorFingerprint()
			&& Evidence.PostconditionHash == BuildCertifiedNoEffectPostcondition(
				Record, Evidence.EffectFingerprint, Evidence.ActionNonce)
			&& Evidence.ReceiptFingerprint == BuildCertifiedNoEffectReceiptFingerprint(Evidence);
	}

	FString MakeTrustedExecutionInstanceId()
	{
		return FString::Printf(
			TEXT("editor-%08x-%s"),
			FPlatformProcess::GetCurrentProcessId(),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
	}

#if PLATFORM_WINDOWS
	bool TryReadFinalDirectoryPath(HANDLE DirectoryHandle, FString& OutPath, FString& OutError)
	{
		OutPath.Reset();
		for (uint32 BufferCharacters = 512; BufferCharacters <= MaxResolvedProjectPathCharacters;)
		{
			TArray<TCHAR> Buffer;
			Buffer.SetNumZeroed(static_cast<int32>(BufferCharacters));
			const DWORD Length = ::GetFinalPathNameByHandleW(
				DirectoryHandle,
				Buffer.GetData(),
				BufferCharacters,
				FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (Length == 0)
			{
				OutError = FString::Printf(
					TEXT("Could not resolve the project directory's final path (Win32 error %u)."),
					static_cast<uint32>(::GetLastError()));
				return false;
			}
			if (Length < BufferCharacters)
			{
				OutPath = FString(static_cast<int32>(Length), Buffer.GetData());
				break;
			}
			if (Length >= MaxResolvedProjectPathCharacters)
			{
				OutError = TEXT("The resolved project directory path exceeds the supported Win32 bound.");
				return false;
			}
			BufferCharacters = Length + 1;
		}

		if (OutPath.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::IgnoreCase))
		{
			OutPath = TEXT("\\\\") + OutPath.RightChop(8);
		}
		else if (OutPath.StartsWith(TEXT("\\\\?\\"), ESearchCase::IgnoreCase))
		{
			OutPath.RightChopInline(4);
		}
		FPaths::NormalizeDirectoryName(OutPath);
		if (OutPath.IsEmpty())
		{
			OutError = TEXT("The project directory resolved to an empty canonical path.");
			return false;
		}
		return true;
	}
#endif
}

bool FHyperAIStudioOperationRecord::IsTerminal() const
{
	return State == EHyperAIStudioOperationState::Completed
		|| State == EHyperAIStudioOperationState::Failed
		|| State == EHyperAIStudioOperationState::RolledBack
		|| State == EHyperAIStudioOperationState::Partial
		|| State == EHyperAIStudioOperationState::OutcomeUnknown;
}

bool FHyperAIStudioOperationRecord::NeedsReconciliation() const
{
	return State == EHyperAIStudioOperationState::Queued
		|| State == EHyperAIStudioOperationState::Running
		|| State == EHyperAIStudioOperationState::CommitStarted
		|| State == EHyperAIStudioOperationState::CancelRequested;
}

FHyperAIStudioOperationJournal::FHyperAIStudioOperationJournal(const FString& InProjectRoot, const int32 InMaxRecords)
	: TrustedExecutionInstanceId(HyperAIStudio::OperationJournal::Private::MakeTrustedExecutionInstanceId())
	, MaxRecords(FMath::Clamp(InMaxRecords, 16, 4096))
{
	ProjectRoot = FPaths::ConvertRelativePathToFull(InProjectRoot);
	FPaths::NormalizeDirectoryName(ProjectRoot);
	FString ResolvedProjectRoot;
	if (TryResolveProjectIdentity(ProjectRoot, ResolvedProjectRoot, CanonicalProjectId, CanonicalIdentityError))
	{
		ProjectRoot = MoveTemp(ResolvedProjectRoot);
	}
	const FString RuntimeDirectory = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("runtime"));
	SlotAPath = FPaths::Combine(RuntimeDirectory, TEXT("operation-journal.a.json"));
	SlotBPath = FPaths::Combine(RuntimeDirectory, TEXT("operation-journal.b.json"));
}

FHyperAIStudioOperationJournal::~FHyperAIStudioOperationJournal() = default;

bool FHyperAIStudioOperationJournal::Load(FString& OutError)
{
	return LoadInternal(true, OutError);
}

bool FHyperAIStudioOperationJournal::LoadReadOnly(FString& OutError)
{
	return LoadInternal(false, OutError);
}

bool FHyperAIStudioOperationJournal::LoadInternal(const bool bForMutation, FString& OutError)
{
	FScopeLock Lock(&Mutex);
	OutError.Reset();
	if (!bForMutation)
	{
		SystemWideMutex.Reset();
	}
	bLoaded = false;
	bOwnershipReconciled = false;
	LastLoadReconciledCount = 0;
	Generation = 0;
	ActiveSlot = EJournalSlot::None;
	Records.Reset();

	if (CanonicalProjectId.IsEmpty())
	{
		OutError = CanonicalIdentityError.IsEmpty()
			? TEXT("A strong canonical project-directory identity is unavailable.")
			: CanonicalIdentityError;
		return false;
	}
	if (!VerifyCanonicalProjectIdentity(OutError))
	{
		return false;
	}
	if (bForMutation)
	{
		if (!SystemWideMutex.IsValid() || !SystemWideMutex->IsValid())
		{
			SystemWideMutex.Reset();
			const FString MutexName = FString::Printf(
				TEXT("HyperAIStudio.OperationJournal.%s"), *CanonicalProjectId);
			SystemWideMutex = MakeUnique<FSystemWideCriticalSection>(MutexName, FTimespan::Zero());
		}
		if (!SystemWideMutex.IsValid() || !SystemWideMutex->IsValid())
		{
			SystemWideMutex.Reset();
			OutError = TEXT("Another HyperAIStudio journal owner is active for this project.");
			return false;
		}
	}

	FLoadedGeneration SlotA;
	FLoadedGeneration SlotB;
	FString ErrorA;
	FString ErrorB;
	const bool bAValid = LoadGeneration(SlotAPath, SlotA, ErrorA);
	const bool bBValid = LoadGeneration(SlotBPath, SlotB, ErrorB);

	if (!SlotA.bExists && !SlotB.bExists)
	{
		bLoaded = true;
		bOwnershipReconciled = bForMutation;
		return true;
	}

	if (!bAValid && !bBValid)
	{
		OutError = FString::Printf(TEXT("No valid operation-journal generation. Slot A: %s Slot B: %s"), *ErrorA, *ErrorB);
		return false;
	}

	const FLoadedGeneration* Selected = nullptr;
	if (bAValid && bBValid)
	{
		if (SlotA.Generation == SlotB.Generation && SlotA.Checksum != SlotB.Checksum)
		{
			OutError = TEXT("Operation-journal generations conflict at the same generation number.");
			return false;
		}
		if (SlotA.Generation >= SlotB.Generation)
		{
			Selected = &SlotA;
			ActiveSlot = EJournalSlot::A;
		}
		else
		{
			Selected = &SlotB;
			ActiveSlot = EJournalSlot::B;
		}
	}
	else if (bAValid)
	{
		Selected = &SlotA;
		ActiveSlot = EJournalSlot::A;
	}
	else
	{
		Selected = &SlotB;
		ActiveSlot = EJournalSlot::B;
	}

	check(Selected);
	Generation = Selected->Generation;
	Records = Selected->Records;
	bLoaded = true;
	if (!bForMutation)
	{
		return true;
	}
	if (!ReconcileUnfinishedForCurrentOwner(LastLoadReconciledCount, OutError))
	{
		bLoaded = false;
		return false;
	}
	bOwnershipReconciled = true;
	return true;
}

EHyperAIStudioOperationBeginResult FHyperAIStudioOperationJournal::BeginOperation(
	const FString& OperationId,
	const FString& PlanHash,
	const FString& CapabilityHash,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)
{
	FScopeLock Lock(&Mutex);
	OutError.Reset();

	if (!bLoaded || !bOwnershipReconciled)
	{
		OutError = TEXT("Operation journal ownership must load and reconcile successfully before mutation.");
		return EHyperAIStudioOperationBeginResult::OwnerUnavailable;
	}
	if (!IsValidOperationId(OperationId)
		|| !IsValidSha256Token(PlanHash)
		|| !IsValidSha256Token(CapabilityHash))
	{
		OutError = TEXT("operation_id and exact sha256 plan/capability identifiers are required.");
		return EHyperAIStudioOperationBeginResult::Invalid;
	}

	if (FHyperAIStudioOperationRecord* Existing = Records.FindByPredicate([&OperationId](const FHyperAIStudioOperationRecord& Record)
	{
		return Record.OperationId == OperationId;
	}))
	{
		OutRecord = *Existing;
		if (Existing->PlanHash != PlanHash || Existing->CapabilityHash != CapabilityHash)
		{
			OutError = TEXT("operation_id is already bound to a different plan or capability inventory.");
			return EHyperAIStudioOperationBeginResult::Conflict;
		}
		if (Existing->NeedsReconciliation() && Existing->ExecutionInstanceId != TrustedExecutionInstanceId)
		{
			OutError = TEXT("A non-terminal operation belongs to another execution instance and was not reconciled.");
			return EHyperAIStudioOperationBeginResult::OwnerUnavailable;
		}
		if (Existing->State == EHyperAIStudioOperationState::Completed)
		{
			if (Existing->TerminalEvidence.Version == FHyperAIStudioOperationEvidence::CurrentVersion)
			{
				return EHyperAIStudioOperationBeginResult::ReplayCompleted;
			}
			OutError = TEXT("Completed legacy record has no fully bound validator receipt and cannot be replayed.");
		}
		return EHyperAIStudioOperationBeginResult::Existing;
	}

	if (Records.Num() >= MaxRecords)
	{
		OutError = TEXT("Operation journal capacity is exhausted; records are never pruned implicitly.");
		return EHyperAIStudioOperationBeginResult::CapacityExceeded;
	}

	const TArray<FHyperAIStudioOperationRecord> PreviousRecords = Records;
	FHyperAIStudioOperationRecord Record;
	Record.OperationId = OperationId;
	Record.CanonicalProjectId = CanonicalProjectId;
	Record.PlanHash = PlanHash;
	Record.ExecutionInstanceId = TrustedExecutionInstanceId;
	Record.CapabilityHash = CapabilityHash;
	Record.State = EHyperAIStudioOperationState::Queued;
	Record.RollbackState = EHyperAIStudioRollbackState::NotNeeded;
	Record.CreatedUtc = HyperAIStudio::OperationJournal::Private::NowUtc();
	Record.UpdatedUtc = Record.CreatedUtc;
	Records.Add(Record);

	if (!Save(OutError))
	{
		Records = PreviousRecords;
		return EHyperAIStudioOperationBeginResult::PersistenceFailure;
	}

	OutRecord = Record;
	return EHyperAIStudioOperationBeginResult::Created;
}

bool FHyperAIStudioOperationJournal::Transition(
	const FString& OperationId,
	const EHyperAIStudioOperationState ExpectedState,
	const EHyperAIStudioOperationState NewState,
	const EHyperAIStudioRollbackState RollbackState,
	const bool bRetrySafe,
	const bool bPartialCommit,
	const FString& StatusCode,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)

{
	return Transition(
		OperationId,
		ExpectedState,
		NewState,
		RollbackState,
		bRetrySafe,
		bPartialCommit,
		StatusCode,
		{},
		OutRecord,
		OutError);
}

bool FHyperAIStudioOperationJournal::TransitionCertifiedNoEffect(
	const FString& OperationId,
	const FString& EffectFingerprint,
	const FString& ActionNonce,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)
{
	using namespace HyperAIStudio::OperationJournal::Private;
	OutRecord = FHyperAIStudioOperationRecord{};
	OutError.Reset();
	const TOptional<FHyperAIStudioOperationRecord> Existing = Find(OperationId);
	if (!Existing.IsSet() || Existing->State != EHyperAIStudioOperationState::CommitStarted
		|| !IsValidSha256Token(EffectFingerprint) || !IsValidEvidenceNonce(ActionNonce))
	{
		OutError = TEXT("Certified no-effect closure requires the exact live commit marker, effect fingerprint and action nonce.");
		return false;
	}
	const int64 IssuedUtcMs = NowUtcMs();
	if (IssuedUtcMs <= 0 || IssuedUtcMs > TNumericLimits<int64>::Max() - CertifiedNoEffectLifetimeMs)
	{
		OutError = TEXT("Certified no-effect evidence time is unavailable.");
		return false;
	}

	FHyperAIStudioOperationEvidence Evidence;
	Evidence.Version = FHyperAIStudioOperationEvidence::CurrentVersion;
	Evidence.CanonicalProjectId = Existing->CanonicalProjectId;
	Evidence.OperationId = Existing->OperationId;
	Evidence.PlanHash = Existing->PlanHash;
	Evidence.CapabilityHash = Existing->CapabilityHash;
	Evidence.EffectFingerprint = EffectFingerprint;
	Evidence.ActionNonce = ActionNonce;
	Evidence.ValidatorId = CertifiedNoEffectValidatorId;
	Evidence.ApprovedValidatorFingerprint = CertifiedNoEffectValidatorFingerprint();
	Evidence.PostconditionHash = BuildCertifiedNoEffectPostcondition(
		Existing.GetValue(), EffectFingerprint, ActionNonce);
	Evidence.IssuedUtcMs = IssuedUtcMs;
	Evidence.ExpiresUtcMs = IssuedUtcMs + CertifiedNoEffectLifetimeMs;
	Evidence.ReceiptFingerprint = BuildCertifiedNoEffectReceiptFingerprint(Evidence);
	return Transition(
		OperationId,
		EHyperAIStudioOperationState::CommitStarted,
		EHyperAIStudioOperationState::Failed,
		EHyperAIStudioRollbackState::NotNeeded,
		true,
		false,
		CertifiedNoEffectStatus,
		Evidence,
		OutRecord,
		OutError);
}

bool FHyperAIStudioOperationJournal::Transition(
	const FString& OperationId,
	const EHyperAIStudioOperationState ExpectedState,
	const EHyperAIStudioOperationState NewState,
	const EHyperAIStudioRollbackState RollbackState,
	const bool bRetrySafe,
	const bool bPartialCommit,
	const FString& StatusCode,
	const FHyperAIStudioOperationEvidence& Evidence,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)
{
	FScopeLock Lock(&Mutex);
	OutError.Reset();
	if (!bLoaded || !bOwnershipReconciled)
	{
		OutError = TEXT("Operation journal ownership must load and reconcile successfully before mutation.");
		return false;
	}

	FHyperAIStudioOperationRecord* Record = Records.FindByPredicate([&OperationId](const FHyperAIStudioOperationRecord& Candidate)
	{
		return Candidate.OperationId == OperationId;
	});
	if (!Record)
	{
		OutError = TEXT("Unknown operation_id.");
		return false;
	}
	if (Record->NeedsReconciliation() && Record->ExecutionInstanceId != TrustedExecutionInstanceId)
	{
		OutError = TEXT("Only the trusted execution instance that owns a non-terminal operation may transition it.");
		OutRecord = *Record;
		return false;
	}
	if (!IsValidStatusCode(StatusCode))
	{
		OutError = TEXT("status_code must be empty or 1-64 lowercase ASCII URL-safe characters.");
		OutRecord = *Record;
		return false;
	}
	if (Record->State != ExpectedState)
	{
		OutError = FString::Printf(TEXT("Expected state %s but journal contains %s."), LexToString(ExpectedState), LexToString(Record->State));
		OutRecord = *Record;
		return false;
	}
	if (!IsLegalTransition(Record->State, NewState))
	{
		OutError = FString::Printf(TEXT("Illegal operation transition %s -> %s."), LexToString(Record->State), LexToString(NewState));
		OutRecord = *Record;
		return false;
	}
	if (Record->State == EHyperAIStudioOperationState::Running
		&& NewState == EHyperAIStudioOperationState::Completed
		&& StatusCode != TEXT("no_changes"))
	{
		OutError = TEXT("running -> completed is reserved for an explicitly validated no-op; mutations must enter commit_started first.");
		OutRecord = *Record;
		return false;
	}
	const bool bCertifiedNoEffect = NewState == EHyperAIStudioOperationState::Failed
		&& ExpectedState == EHyperAIStudioOperationState::CommitStarted
		&& StatusCode == HyperAIStudio::OperationJournal::Private::CertifiedNoEffectStatus;
	if (ExpectedState == EHyperAIStudioOperationState::CommitStarted
		&& NewState == EHyperAIStudioOperationState::Failed && !bCertifiedNoEffect)
	{
		OutError = TEXT("Commit-started work can become retry-safe failed only through core-certified no-effect evidence.");
		OutRecord = *Record;
		return false;
	}
	const bool bRequiresTerminalEvidence = NewState == EHyperAIStudioOperationState::Completed
		|| NewState == EHyperAIStudioOperationState::RolledBack || bCertifiedNoEffect;
	const int64 AcceptedUtcMs = HyperAIStudio::OperationJournal::Private::NowUtcMs();
	if ((bRequiresTerminalEvidence
			&& !ValidateEvidenceForRecord(*Record, Evidence, AcceptedUtcMs, OutError))
		|| (bCertifiedNoEffect
			&& !HyperAIStudio::OperationJournal::Private::IsCertifiedNoEffectEvidence(*Record, Evidence))
		|| (!bRequiresTerminalEvidence && Evidence.IsPresent()))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Validator evidence is admitted only for completed or rolled-back terminal transitions.");
		}
		OutRecord = *Record;
		return false;
	}

	const FHyperAIStudioOperationRecord Previous = *Record;
	Record->State = NewState;
	Record->RollbackState = RollbackState;
	Record->bRetrySafe = bRetrySafe;
	Record->bPartialCommit = bPartialCommit;
	Record->StatusCode = StatusCode;
	Record->UpdatedUtc = HyperAIStudio::OperationJournal::Private::NowUtc();
	if (bRequiresTerminalEvidence)
	{
		Record->TerminalEvidence = Evidence;
		Record->EvidenceValidatedByExecutionInstanceId = TrustedExecutionInstanceId;
		Record->EvidenceAcceptedUtcMs = AcceptedUtcMs;
	}
	if (!ValidateStateInvariants(*Record, OutError))
	{
		*Record = Previous;
		OutRecord = *Record;
		return false;
	}
	if (!Save(OutError))
	{
		*Record = Previous;
		OutRecord = *Record;
		return false;
	}

	OutRecord = *Record;
	return true;
}

bool FHyperAIStudioOperationJournal::ResolveUnknownWithEvidence(
	const FString& OperationId,
	const EHyperAIStudioUnknownResolution Resolution,
	const FString& ValidatorHash,
	const FString& PostconditionHash,
	const EHyperAIStudioRollbackState PartialRollbackState,
	const FString& StatusCode,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)

{
	(void)Resolution;
	(void)ValidatorHash;
	(void)PostconditionHash;
	(void)PartialRollbackState;
	(void)StatusCode;
	OutError = TEXT("Legacy hash-only unknown-outcome resolution is disabled; a server-verified bound validator receipt is required.");
	const TOptional<FHyperAIStudioOperationRecord> Existing = Find(OperationId);
	OutRecord = Existing.IsSet() ? *Existing : FHyperAIStudioOperationRecord{};
	return false;
}

bool FHyperAIStudioOperationJournal::ResolveUnknownWithEvidence(
	const FString& OperationId,
	const EHyperAIStudioUnknownResolution Resolution,
	const FHyperAIStudioOperationEvidence& Evidence,
	const EHyperAIStudioRollbackState PartialRollbackState,
	const FString& StatusCode,
	FHyperAIStudioOperationRecord& OutRecord,
	FString& OutError)
{
	FScopeLock Lock(&Mutex);
	OutError.Reset();
	if (!bLoaded || !bOwnershipReconciled)
	{
		OutError = TEXT("Operation journal ownership must load and reconcile successfully before unknown-outcome resolution.");
		return false;
	}
	if (StatusCode.IsEmpty()
		|| !IsValidStatusCode(StatusCode))
	{
		OutError = TEXT("Unknown-outcome resolution requires a valid status code.");
		return false;
	}

	FHyperAIStudioOperationRecord* Record = Records.FindByPredicate([&OperationId](const FHyperAIStudioOperationRecord& Candidate)
	{
		return Candidate.OperationId == OperationId;
	});
	if (!Record)
	{
		OutError = TEXT("Unknown operation_id.");
		return false;
	}
	if (Record->State != EHyperAIStudioOperationState::OutcomeUnknown)
	{
		OutError = TEXT("Only outcome_unknown records can be resolved with postcondition evidence.");
		OutRecord = *Record;
		return false;
	}
	const int64 AcceptedUtcMs = HyperAIStudio::OperationJournal::Private::NowUtcMs();
	if (!ValidateEvidenceForRecord(*Record, Evidence, AcceptedUtcMs, OutError))
	{
		OutRecord = *Record;
		return false;
	}

	const FHyperAIStudioOperationRecord Previous = *Record;
	Record->ResolutionValidatorHash = Evidence.ApprovedValidatorFingerprint;
	Record->ResolutionPostconditionHash = Evidence.PostconditionHash;
	Record->ResolvedByExecutionInstanceId = TrustedExecutionInstanceId;
	Record->TerminalEvidence = Evidence;
	Record->EvidenceValidatedByExecutionInstanceId = TrustedExecutionInstanceId;
	Record->EvidenceAcceptedUtcMs = AcceptedUtcMs;
	Record->StatusCode = StatusCode;
	Record->UpdatedUtc = HyperAIStudio::OperationJournal::Private::NowUtc();
	Record->bRetrySafe = false;
	switch (Resolution)
	{
	case EHyperAIStudioUnknownResolution::Completed:
		Record->State = EHyperAIStudioOperationState::Completed;
		Record->RollbackState = EHyperAIStudioRollbackState::NotNeeded;
		Record->bPartialCommit = false;
		break;
	case EHyperAIStudioUnknownResolution::RolledBack:
		Record->State = EHyperAIStudioOperationState::RolledBack;
		Record->RollbackState = EHyperAIStudioRollbackState::Complete;
		Record->bPartialCommit = false;
		break;
	case EHyperAIStudioUnknownResolution::Partial:
		if (PartialRollbackState != EHyperAIStudioRollbackState::Partial
			&& PartialRollbackState != EHyperAIStudioRollbackState::Unsupported
			&& PartialRollbackState != EHyperAIStudioRollbackState::Unknown)
		{
			OutError = TEXT("A resolved partial outcome requires partial, unsupported, or unknown rollback evidence.");
			*Record = Previous;
			OutRecord = *Record;
			return false;
		}
		Record->State = EHyperAIStudioOperationState::Partial;
		Record->RollbackState = PartialRollbackState;
		Record->bPartialCommit = true;
		break;
	default:
		OutError = TEXT("Unknown terminal resolution kind.");
		*Record = Previous;
		OutRecord = *Record;
		return false;
	}

	if (!ValidateStateInvariants(*Record, OutError) || !Save(OutError))
	{
		*Record = Previous;
		OutRecord = *Record;
		return false;
	}
	OutRecord = *Record;
	return true;
}

bool FHyperAIStudioOperationJournal::ReconcileUnfinishedForCurrentOwner(int32& OutReconciledCount, FString& OutError)
{
	OutReconciledCount = 0;
	OutError.Reset();
	if (!bLoaded || !IsValidExecutionInstanceId(TrustedExecutionInstanceId))
	{
		OutError = TEXT("A loaded journal and trusted execution-instance identifier are required for reconciliation.");
		return false;
	}

	const TArray<FHyperAIStudioOperationRecord> Previous = Records;
	for (FHyperAIStudioOperationRecord& Record : Records)
	{
		if (!Record.NeedsReconciliation() || Record.ExecutionInstanceId == TrustedExecutionInstanceId)
		{
			continue;
		}

		if (Record.State == EHyperAIStudioOperationState::CommitStarted)
		{
			Record.State = EHyperAIStudioOperationState::OutcomeUnknown;
			Record.RollbackState = EHyperAIStudioRollbackState::Unknown;
			Record.bRetrySafe = false;
			Record.bPartialCommit = true;
			Record.StatusCode = TEXT("previous_instance_commit_unknown");
		}
		else
		{
			// Queued/running/cancel_requested are guaranteed pre-commit by the transition matrix.
			Record.State = EHyperAIStudioOperationState::Failed;
			Record.RollbackState = EHyperAIStudioRollbackState::NotNeeded;
			Record.bRetrySafe = true;
			Record.bPartialCommit = false;
			Record.StatusCode = TEXT("previous_instance_precommit");
		}
		Record.UpdatedUtc = HyperAIStudio::OperationJournal::Private::NowUtc();
		++OutReconciledCount;
	}

	if (OutReconciledCount > 0 && !Save(OutError))
	{
		Records = Previous;
		OutReconciledCount = 0;
		return false;
	}
	return true;
}

TOptional<FHyperAIStudioOperationRecord> FHyperAIStudioOperationJournal::Find(const FString& OperationId) const
{
	FScopeLock Lock(&Mutex);
	if (!bLoaded)
	{
		return {};
	}
	if (const FHyperAIStudioOperationRecord* Record = Records.FindByPredicate([&OperationId](const FHyperAIStudioOperationRecord& Candidate)
	{
		return Candidate.OperationId == OperationId;
	}))
	{
		return *Record;
	}
	return {};
}

TArray<FHyperAIStudioOperationRecord> FHyperAIStudioOperationJournal::GetRecordsSnapshot() const
{
	FScopeLock Lock(&Mutex);
	return bLoaded ? Records : TArray<FHyperAIStudioOperationRecord>();
}

bool FHyperAIStudioOperationJournal::IsLoaded() const
{
	FScopeLock Lock(&Mutex);
	return bLoaded;
}

int64 FHyperAIStudioOperationJournal::GetGeneration() const
{
	FScopeLock Lock(&Mutex);
	return Generation;
}

int32 FHyperAIStudioOperationJournal::GetLastLoadReconciledCount() const
{
	FScopeLock Lock(&Mutex);
	return LastLoadReconciledCount;
}

FString FHyperAIStudioOperationJournal::MakeCanonicalProjectId(const FString& ProjectRoot)
{
	FString ResolvedProjectRoot;
	FString CanonicalId;
	FString Error;
	return TryResolveProjectIdentity(ProjectRoot, ResolvedProjectRoot, CanonicalId, Error)
		? CanonicalId
		: FString();
}

bool FHyperAIStudioOperationJournal::TryResolveProjectIdentity(
	const FString& InProjectRoot,
	FString& OutResolvedProjectRoot,
	FString& OutCanonicalProjectId,
	FString& OutError)
{
	OutResolvedProjectRoot.Reset();
	OutCanonicalProjectId.Reset();
	OutError.Reset();

	FString RequestedRoot = FPaths::ConvertRelativePathToFull(InProjectRoot);
	FPaths::NormalizeDirectoryName(RequestedRoot);
	if (RequestedRoot.IsEmpty())
	{
		OutError = TEXT("A non-empty project directory is required for journal ownership.");
		return false;
	}

#if PLATFORM_WINDOWS
	const HANDLE DirectoryHandle = ::CreateFileW(
		*RequestedRoot,
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);
	if (DirectoryHandle == INVALID_HANDLE_VALUE)
	{
		OutError = FString::Printf(
			TEXT("Could not open the existing project directory for strong identity resolution (Win32 error %u)."),
			static_cast<uint32>(::GetLastError()));
		return false;
	}
	ON_SCOPE_EXIT
	{
		::CloseHandle(DirectoryHandle);
	};

	FILE_STANDARD_INFO StandardInfo = {};
	if (!::GetFileInformationByHandleEx(DirectoryHandle, FileStandardInfo, &StandardInfo, sizeof(StandardInfo))
		|| !StandardInfo.Directory)
	{
		OutError = TEXT("The project root is not an existing directory with readable Win32 identity metadata.");
		return false;
	}

	FILE_ID_INFO DirectoryId = {};
	if (!::GetFileInformationByHandleEx(DirectoryHandle, FileIdInfo, &DirectoryId, sizeof(DirectoryId)))
	{
		OutError = FString::Printf(
			TEXT("Could not read the project directory's stable file identity (Win32 error %u)."),
			static_cast<uint32>(::GetLastError()));
		return false;
	}

	bool bHasFileIdentifier = false;
	FString FileIdentifierHex;
	FileIdentifierHex.Reserve(UE_ARRAY_COUNT(DirectoryId.FileId.Identifier) * 2);
	for (const uint8 Byte : DirectoryId.FileId.Identifier)
	{
		bHasFileIdentifier |= Byte != 0;
		FileIdentifierHex += FString::Printf(TEXT("%02x"), Byte);
	}
	if (!bHasFileIdentifier)
	{
		OutError = TEXT("The filesystem did not provide a strong non-zero project-directory file identity.");
		return false;
	}
	if (!HyperAIStudio::OperationJournal::Private::TryReadFinalDirectoryPath(
		DirectoryHandle,
		OutResolvedProjectRoot,
		OutError))
	{
		return false;
	}

	const FString IdentityMaterial = FString::Printf(
		TEXT("hyperai-win64-directory-id-v1:%016llx:%s"),
		static_cast<unsigned long long>(DirectoryId.VolumeSerialNumber),
		*FileIdentifierHex);
	OutCanonicalProjectId = HyperAIStudio::OperationJournal::Private::Sha1Utf8(IdentityMaterial);
#else
	if (!IFileManager::Get().DirectoryExists(*RequestedRoot))
	{
		OutError = TEXT("The existing project directory is required for journal ownership.");
		return false;
	}
	OutResolvedProjectRoot = RequestedRoot;
	OutCanonicalProjectId = HyperAIStudio::OperationJournal::Private::Sha1Utf8(
		TEXT("hyperai-canonical-directory-path-v1:") + RequestedRoot);
#endif

	return !OutCanonicalProjectId.IsEmpty();
}

bool FHyperAIStudioOperationJournal::IsValidOperationId(const FString& OperationId)
{
	if (OperationId.Len() < 8 || OperationId.Len() > 128)
	{
		return false;
	}
	for (const TCHAR Character : OperationId)
	{
		if (!(Character >= TEXT('a') && Character <= TEXT('z'))
			&& !(Character >= TEXT('A') && Character <= TEXT('Z'))
			&& !(Character >= TEXT('0') && Character <= TEXT('9'))
			&& Character != TEXT('-') && Character != TEXT('_') && Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioOperationJournal::IsValidStatusCode(const FString& StatusCode)
{
	if (StatusCode.IsEmpty())
	{
		return true;
	}
	if (StatusCode.Len() > 64)
	{
		return false;
	}
	for (const TCHAR Character : StatusCode)
	{
		if (!(Character >= TEXT('a') && Character <= TEXT('z'))
			&& !(Character >= TEXT('0') && Character <= TEXT('9'))
			&& Character != TEXT('-') && Character != TEXT('_') && Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioOperationJournal::Save(FString& OutError)
{
	if (!bLoaded || !SystemWideMutex.IsValid() || !SystemWideMutex->IsValid())
	{
		OutError = TEXT("Operation journal has no valid loaded owner.");
		return false;
	}
	if (!VerifyCanonicalProjectIdentity(OutError))
	{
		return false;
	}
	if (Records.Num() > MaxRecords || Generation == MAX_int64)
	{
		OutError = TEXT("Operation journal cannot advance beyond its configured bounds.");
		return false;
	}

	const int64 NextGeneration = Generation + 1;
	const EJournalSlot TargetSlot = ActiveSlot == EJournalSlot::A ? EJournalSlot::B : EJournalSlot::A;
	const FString& TargetPath = TargetSlot == EJournalSlot::A ? SlotAPath : SlotBPath;
	FString JsonText;
	FString ExpectedChecksum;
	if (!SerializeGeneration(NextGeneration, Records, JsonText, ExpectedChecksum, OutError))
	{
		return false;
	}

	const FString Directory = FPaths::GetPath(TargetPath);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = FString::Printf(TEXT("Could not create operation journal directory: %s"), *Directory);
		return false;
	}
	if (!FFileHelper::SaveStringToFile(JsonText, *TargetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write inactive operation-journal generation: %s"), *TargetPath);
		return false;
	}

	FLoadedGeneration Verified;
	FString VerifyError;
	if (!LoadGeneration(TargetPath, Verified, VerifyError)
		|| Verified.Generation != NextGeneration
		|| Verified.Checksum != ExpectedChecksum)
	{
		OutError = FString::Printf(TEXT("Operation-journal generation failed read-back verification: %s"), *VerifyError);
		return false;
	}

	Generation = NextGeneration;
	ActiveSlot = TargetSlot;
	return true;
}

bool FHyperAIStudioOperationJournal::VerifyCanonicalProjectIdentity(FString& OutError) const
{
	FString ResolvedRoot;
	FString ResolvedId;
	FString ResolutionError;
	if (!TryResolveProjectIdentity(ProjectRoot, ResolvedRoot, ResolvedId, ResolutionError))
	{
		OutError = FString::Printf(TEXT("Canonical project-directory identity could not be revalidated: %s"), *ResolutionError);
		return false;
	}
	const ESearchCase::Type PathCase =
#if PLATFORM_WINDOWS
		ESearchCase::IgnoreCase;
#else
		ESearchCase::CaseSensitive;
#endif
	if (ResolvedId != CanonicalProjectId || !ResolvedRoot.Equals(ProjectRoot, PathCase))
	{
		OutError = TEXT("The project directory identity changed after journal ownership was established.");
		return false;
	}
	return true;
}

bool FHyperAIStudioOperationJournal::LoadGeneration(const FString& Path, FLoadedGeneration& OutGeneration, FString& OutError) const
{
	OutGeneration = {};
	OutError.Reset();
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.FileExists(*Path))
	{
		return false;
	}
	OutGeneration.bExists = true;
	const int64 FileSize = FileManager.FileSize(*Path);
	if (FileSize <= 0 || FileSize > HyperAIStudio::OperationJournal::Private::MaxJournalBytes)
	{
		OutError = TEXT("Journal generation is empty or exceeds the 4 MiB bound.");
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		OutError = TEXT("Could not read journal generation.");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("Journal generation is not valid JSON.");
		return false;
	}

	int32 Version = 0;
	FString GenerationText;
	FString StoredProjectId;
	if (!Root->TryGetNumberField(TEXT("schema_version"), Version)
		|| (Version != HyperAIStudio::OperationJournal::Private::LegacySchemaVersion
			&& Version != HyperAIStudio::OperationJournal::Private::SchemaVersion)
		|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Root, TEXT("generation"), GenerationText)
		|| !HyperAIStudio::OperationJournal::Private::TryParseCanonicalNonNegativeInt64(GenerationText, OutGeneration.Generation)
		|| OutGeneration.Generation <= 0
		|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Root, TEXT("canonical_project_id"), StoredProjectId)
		|| StoredProjectId != CanonicalProjectId
		|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Root, TEXT("checksum_sha1"), OutGeneration.Checksum)
		|| OutGeneration.Checksum.Len() != FSHA1::DigestSize * 2)
	{
		OutError = TEXT("Journal generation header is invalid or belongs to another project.");
		return false;
	}
	OutGeneration.SchemaVersion = Version;

	const TArray<TSharedPtr<FJsonValue>>* JsonRecords = nullptr;
	if (!Root->TryGetArrayField(TEXT("records"), JsonRecords) || !JsonRecords || JsonRecords->Num() > MaxRecords)
	{
		OutError = TEXT("Journal records array is missing or exceeds the configured bound.");
		return false;
	}

	TSet<FString> SeenIds;
	OutGeneration.Records.Reserve(JsonRecords->Num());
	for (const TSharedPtr<FJsonValue>& Value : *JsonRecords)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
		{
			OutError = TEXT("Journal contains a non-object record.");
			return false;
		}

		const TSharedPtr<FJsonObject>& Object = *ObjectPtr;
		FHyperAIStudioOperationRecord Record;
		FString StateText;
		FString RollbackText;
		if (!HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("operation_id"), Record.OperationId)
			|| SeenIds.Contains(Record.OperationId)
			|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("canonical_project_id"), Record.CanonicalProjectId)
			|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("plan_hash"), Record.PlanHash)
			|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("execution_instance_id"), Record.ExecutionInstanceId)
				|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("capability_hash"), Record.CapabilityHash)
				|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("state"), StateText)
				|| !TryParseOperationState(StateText, Record.State)
				|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("rollback_state"), RollbackText)
				|| !TryParseRollbackState(RollbackText, Record.RollbackState)
				|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("created_utc"), Record.CreatedUtc)
				|| !HyperAIStudio::OperationJournal::Private::ReadRequiredString(Object, TEXT("updated_utc"), Record.UpdatedUtc)
				|| !Object->TryGetStringField(TEXT("status_code"), Record.StatusCode)
				|| !Object->TryGetStringField(TEXT("resolution_validator_hash"), Record.ResolutionValidatorHash)
				|| !Object->TryGetStringField(TEXT("resolution_postcondition_hash"), Record.ResolutionPostconditionHash)
				|| !Object->TryGetStringField(TEXT("resolved_by_execution_instance_id"), Record.ResolvedByExecutionInstanceId)
				|| !Object->TryGetBoolField(TEXT("retry_safe"), Record.bRetrySafe)
				|| !Object->TryGetBoolField(TEXT("partial_commit"), Record.bPartialCommit))
		{
			OutError = TEXT("Journal contains an incomplete or duplicate record.");
			return false;
		}
		if (Version == HyperAIStudio::OperationJournal::Private::SchemaVersion)
		{
			FString EvidenceIssuedUtcMs;
			FString EvidenceExpiresUtcMs;
			FString EvidenceAcceptedUtcMs;
			if (!Object->TryGetNumberField(TEXT("terminal_evidence_version"), Record.TerminalEvidence.Version)
				|| !Object->TryGetStringField(TEXT("evidence_canonical_project_id"), Record.TerminalEvidence.CanonicalProjectId)
				|| !Object->TryGetStringField(TEXT("evidence_operation_id"), Record.TerminalEvidence.OperationId)
				|| !Object->TryGetStringField(TEXT("evidence_plan_hash"), Record.TerminalEvidence.PlanHash)
				|| !Object->TryGetStringField(TEXT("evidence_capability_hash"), Record.TerminalEvidence.CapabilityHash)
				|| !Object->TryGetStringField(TEXT("evidence_effect_fingerprint"), Record.TerminalEvidence.EffectFingerprint)
				|| !Object->TryGetStringField(TEXT("evidence_action_nonce"), Record.TerminalEvidence.ActionNonce)
				|| !Object->TryGetStringField(TEXT("evidence_validator_id"), Record.TerminalEvidence.ValidatorId)
				|| !Object->TryGetStringField(TEXT("evidence_validator_fingerprint"), Record.TerminalEvidence.ApprovedValidatorFingerprint)
				|| !Object->TryGetStringField(TEXT("evidence_postcondition_hash"), Record.TerminalEvidence.PostconditionHash)
				|| !Object->TryGetStringField(TEXT("evidence_receipt_fingerprint"), Record.TerminalEvidence.ReceiptFingerprint)
				|| !Object->TryGetStringField(TEXT("evidence_issued_utc_ms"), EvidenceIssuedUtcMs)
				|| !Object->TryGetStringField(TEXT("evidence_expires_utc_ms"), EvidenceExpiresUtcMs)
				|| !Object->TryGetStringField(TEXT("evidence_accepted_utc_ms"), EvidenceAcceptedUtcMs)
				|| !Object->TryGetStringField(TEXT("evidence_validated_by_execution_instance_id"), Record.EvidenceValidatedByExecutionInstanceId)
				|| !HyperAIStudio::OperationJournal::Private::TryParseCanonicalNonNegativeInt64(EvidenceIssuedUtcMs, Record.TerminalEvidence.IssuedUtcMs)
				|| !HyperAIStudio::OperationJournal::Private::TryParseCanonicalNonNegativeInt64(EvidenceExpiresUtcMs, Record.TerminalEvidence.ExpiresUtcMs)
				|| !HyperAIStudio::OperationJournal::Private::TryParseCanonicalNonNegativeInt64(EvidenceAcceptedUtcMs, Record.EvidenceAcceptedUtcMs))
			{
				OutError = TEXT("Journal contains incomplete terminal-evidence fields.");
				return false;
			}
		}
			if (!ValidateRecord(Record, OutError))
		{
			return false;
		}
		SeenIds.Add(Record.OperationId);
		OutGeneration.Records.Add(MoveTemp(Record));
	}

	const FString ComputedChecksum = ComputeGenerationChecksum(
		Version, OutGeneration.Generation, CanonicalProjectId, OutGeneration.Records);
	if (!OutGeneration.Checksum.Equals(ComputedChecksum, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Journal generation checksum does not match its bounded records.");
		return false;
	}
	return true;
}

bool FHyperAIStudioOperationJournal::SerializeGeneration(
	const int64 InGeneration,
	const TArray<FHyperAIStudioOperationRecord>& InRecords,
	FString& OutJson,
	FString& OutChecksum,
	FString& OutError) const
{
	OutJson.Reset();
	OutError.Reset();
	for (const FHyperAIStudioOperationRecord& Record : InRecords)
	{
		if (!ValidateRecord(Record, OutError))
		{
			return false;
		}
	}
	OutChecksum = ComputeGenerationChecksum(
		HyperAIStudio::OperationJournal::Private::SchemaVersion, InGeneration, CanonicalProjectId, InRecords);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), HyperAIStudio::OperationJournal::Private::SchemaVersion);
	Root->SetStringField(TEXT("generation"), FString::Printf(TEXT("%lld"), static_cast<long long>(InGeneration)));
	Root->SetStringField(TEXT("canonical_project_id"), CanonicalProjectId);
	Root->SetStringField(TEXT("checksum_sha1"), OutChecksum);
	TArray<TSharedPtr<FJsonValue>> JsonRecords;
	JsonRecords.Reserve(InRecords.Num());
	for (const FHyperAIStudioOperationRecord& Record : InRecords)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("operation_id"), Record.OperationId);
		Object->SetStringField(TEXT("canonical_project_id"), Record.CanonicalProjectId);
		Object->SetStringField(TEXT("plan_hash"), Record.PlanHash);
		Object->SetStringField(TEXT("execution_instance_id"), Record.ExecutionInstanceId);
		Object->SetStringField(TEXT("capability_hash"), Record.CapabilityHash);
			Object->SetStringField(TEXT("state"), LexToString(Record.State));
			Object->SetStringField(TEXT("rollback_state"), LexToString(Record.RollbackState));
			Object->SetStringField(TEXT("created_utc"), Record.CreatedUtc);
			Object->SetStringField(TEXT("updated_utc"), Record.UpdatedUtc);
			Object->SetStringField(TEXT("status_code"), Record.StatusCode);
			Object->SetStringField(TEXT("resolution_validator_hash"), Record.ResolutionValidatorHash);
			Object->SetStringField(TEXT("resolution_postcondition_hash"), Record.ResolutionPostconditionHash);
			Object->SetStringField(TEXT("resolved_by_execution_instance_id"), Record.ResolvedByExecutionInstanceId);
			Object->SetNumberField(TEXT("terminal_evidence_version"), Record.TerminalEvidence.Version);
			Object->SetStringField(TEXT("evidence_canonical_project_id"), Record.TerminalEvidence.CanonicalProjectId);
			Object->SetStringField(TEXT("evidence_operation_id"), Record.TerminalEvidence.OperationId);
			Object->SetStringField(TEXT("evidence_plan_hash"), Record.TerminalEvidence.PlanHash);
			Object->SetStringField(TEXT("evidence_capability_hash"), Record.TerminalEvidence.CapabilityHash);
			Object->SetStringField(TEXT("evidence_effect_fingerprint"), Record.TerminalEvidence.EffectFingerprint);
			Object->SetStringField(TEXT("evidence_action_nonce"), Record.TerminalEvidence.ActionNonce);
			Object->SetStringField(TEXT("evidence_validator_id"), Record.TerminalEvidence.ValidatorId);
			Object->SetStringField(TEXT("evidence_validator_fingerprint"), Record.TerminalEvidence.ApprovedValidatorFingerprint);
			Object->SetStringField(TEXT("evidence_postcondition_hash"), Record.TerminalEvidence.PostconditionHash);
			Object->SetStringField(TEXT("evidence_receipt_fingerprint"), Record.TerminalEvidence.ReceiptFingerprint);
			Object->SetStringField(TEXT("evidence_issued_utc_ms"), FString::Printf(TEXT("%lld"), static_cast<long long>(Record.TerminalEvidence.IssuedUtcMs)));
			Object->SetStringField(TEXT("evidence_expires_utc_ms"), FString::Printf(TEXT("%lld"), static_cast<long long>(Record.TerminalEvidence.ExpiresUtcMs)));
			Object->SetStringField(TEXT("evidence_accepted_utc_ms"), FString::Printf(TEXT("%lld"), static_cast<long long>(Record.EvidenceAcceptedUtcMs)));
			Object->SetStringField(TEXT("evidence_validated_by_execution_instance_id"), Record.EvidenceValidatedByExecutionInstanceId);
			Object->SetBoolField(TEXT("retry_safe"), Record.bRetrySafe);
		Object->SetBoolField(TEXT("partial_commit"), Record.bPartialCommit);
		JsonRecords.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("records"), JsonRecords);

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Could not serialize operation-journal generation.");
		return false;
	}
	const FTCHARToUTF8 Utf8(*OutJson);
	if (Utf8.Length() > HyperAIStudio::OperationJournal::Private::MaxJournalBytes)
	{
		OutJson.Reset();
		OutError = TEXT("Serialized operation-journal generation exceeds the 4 MiB bound.");
		return false;
	}
	return true;
}

bool FHyperAIStudioOperationJournal::ValidateRecord(const FHyperAIStudioOperationRecord& Record, FString& OutError) const
{
	if (!IsValidOperationId(Record.OperationId)
		|| Record.CanonicalProjectId != CanonicalProjectId
		|| !IsValidSha256Token(Record.PlanHash)
		|| !IsValidExecutionInstanceId(Record.ExecutionInstanceId)
		|| !IsValidSha256Token(Record.CapabilityHash)
		|| Record.CreatedUtc.Len() > 64
		|| Record.UpdatedUtc.Len() > 64
		|| !IsValidStatusCode(Record.StatusCode))
	{
		OutError = TEXT("Journal record contains an invalid or unbounded identifier.");
		return false;
	}
	FDateTime ParsedCreated;
	FDateTime ParsedUpdated;
	if (!FDateTime::ParseIso8601(*Record.CreatedUtc, ParsedCreated)
		|| !FDateTime::ParseIso8601(*Record.UpdatedUtc, ParsedUpdated)
		|| ParsedUpdated < ParsedCreated)
	{
		OutError = TEXT("Journal record timestamps are not bounded ISO-8601 values.");
		return false;
	}
	if (Record.TerminalEvidence.IsPresent()
		&& (!ValidateEvidenceForRecord(Record, Record.TerminalEvidence, Record.EvidenceAcceptedUtcMs, OutError)
			|| !IsValidExecutionInstanceId(Record.EvidenceValidatedByExecutionInstanceId)))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Terminal evidence is not tied to a trusted execution instance.");
		}
		return false;
	}
	return ValidateStateInvariants(Record, OutError);
}

bool FHyperAIStudioOperationJournal::ValidateEvidenceForRecord(
	const FHyperAIStudioOperationRecord& Record,
	const FHyperAIStudioOperationEvidence& Evidence,
	const int64 AcceptedUtcMs,
	FString& OutError) const
{
	static constexpr int64 MaxReceiptLifetimeMs = 5 * 60 * 1000;
	if (Evidence.Version != FHyperAIStudioOperationEvidence::CurrentVersion
		|| Evidence.CanonicalProjectId != Record.CanonicalProjectId
		|| Evidence.OperationId != Record.OperationId
		|| Evidence.PlanHash != Record.PlanHash
		|| Evidence.CapabilityHash != Record.CapabilityHash
		|| !IsValidSha256Token(Evidence.EffectFingerprint)
		|| !IsValidEvidenceNonce(Evidence.ActionNonce)
		|| !IsValidValidatorId(Evidence.ValidatorId)
		|| !IsValidSha256Token(Evidence.ApprovedValidatorFingerprint)
		|| !IsValidSha256Token(Evidence.PostconditionHash)
		|| !IsValidSha256Token(Evidence.ReceiptFingerprint)
		|| Evidence.IssuedUtcMs <= 0
		|| Evidence.ExpiresUtcMs <= Evidence.IssuedUtcMs
		|| Evidence.ExpiresUtcMs - Evidence.IssuedUtcMs > MaxReceiptLifetimeMs
		|| AcceptedUtcMs < Evidence.IssuedUtcMs
		|| AcceptedUtcMs >= Evidence.ExpiresUtcMs)
	{
		OutError = TEXT("Validator evidence is not exactly bound, bounded, current, or temporally valid for this operation record.");
		return false;
	}
	return true;
}

bool FHyperAIStudioOperationJournal::ValidateStateInvariants(const FHyperAIStudioOperationRecord& Record, FString& OutError)
{
	const bool bHasValidator = !Record.ResolutionValidatorHash.IsEmpty();
	const bool bHasPostconditionHash = !Record.ResolutionPostconditionHash.IsEmpty();
	const bool bHasResolver = !Record.ResolvedByExecutionInstanceId.IsEmpty();
	const bool bHasAnyResolutionEvidence = bHasValidator || bHasPostconditionHash || bHasResolver;
	const bool bHasCompleteResolutionEvidence = bHasValidator && bHasPostconditionHash && bHasResolver;
	const FHyperAIStudioOperationEvidence& Terminal = Record.TerminalEvidence;
	const bool bHasAnyTerminalEvidence = Terminal.Version != 0
		|| !Terminal.CanonicalProjectId.IsEmpty() || !Terminal.OperationId.IsEmpty()
		|| !Terminal.PlanHash.IsEmpty() || !Terminal.CapabilityHash.IsEmpty()
		|| !Terminal.EffectFingerprint.IsEmpty() || !Terminal.ActionNonce.IsEmpty()
		|| !Terminal.ValidatorId.IsEmpty() || !Terminal.ApprovedValidatorFingerprint.IsEmpty()
		|| !Terminal.PostconditionHash.IsEmpty() || !Terminal.ReceiptFingerprint.IsEmpty()
		|| Terminal.IssuedUtcMs != 0 || Terminal.ExpiresUtcMs != 0
		|| !Record.EvidenceValidatedByExecutionInstanceId.IsEmpty() || Record.EvidenceAcceptedUtcMs != 0;
	const bool bHasCompleteTerminalEvidence = Terminal.Version == FHyperAIStudioOperationEvidence::CurrentVersion
		&& !Record.EvidenceValidatedByExecutionInstanceId.IsEmpty() && Record.EvidenceAcceptedUtcMs > 0;
	if (bHasAnyTerminalEvidence != bHasCompleteTerminalEvidence)
	{
		OutError = TEXT("Terminal validator evidence must be wholly absent or a complete current-version receipt.");
		return false;
	}
	if (bHasAnyResolutionEvidence != bHasCompleteResolutionEvidence
		|| (bHasCompleteResolutionEvidence
			&& (!IsValidSha256Token(Record.ResolutionValidatorHash)
				|| !IsValidSha256Token(Record.ResolutionPostconditionHash)
				|| !IsValidExecutionInstanceId(Record.ResolvedByExecutionInstanceId))))
	{
		OutError = TEXT("Unknown-outcome resolution evidence must be complete, bounded, and tied to a trusted execution instance.");
		return false;
	}
	if (bHasCompleteResolutionEvidence
		&& Record.State != EHyperAIStudioOperationState::Completed
		&& Record.State != EHyperAIStudioOperationState::RolledBack
		&& Record.State != EHyperAIStudioOperationState::Partial)
	{
		OutError = TEXT("Resolution evidence may exist only on an evidence-resolved terminal state.");
		return false;
	}

	switch (Record.State)
	{
	case EHyperAIStudioOperationState::Queued:
	case EHyperAIStudioOperationState::Running:
	case EHyperAIStudioOperationState::CommitStarted:
	case EHyperAIStudioOperationState::CancelRequested:
		if (Record.RollbackState != EHyperAIStudioRollbackState::NotNeeded
			|| Record.bRetrySafe
			|| Record.bPartialCommit
			|| bHasAnyResolutionEvidence
			|| bHasAnyTerminalEvidence)
		{
			OutError = TEXT("Non-terminal states must remain retry-unsafe, pre-resolution, and free of rollback/partial claims.");
			return false;
		}
		break;
	case EHyperAIStudioOperationState::Completed:
		if (Record.RollbackState != EHyperAIStudioRollbackState::NotNeeded || Record.bPartialCommit)
		{
			OutError = TEXT("Completed operations cannot claim rollback or a partial commit.");
			return false;
		}
		break;
	case EHyperAIStudioOperationState::Failed:
	{
		const bool bCertifiedNoEffect = Record.StatusCode
			== HyperAIStudio::OperationJournal::Private::CertifiedNoEffectStatus;
		if (Record.RollbackState != EHyperAIStudioRollbackState::NotNeeded
			|| Record.bPartialCommit
			|| bHasAnyResolutionEvidence
			|| bHasAnyTerminalEvidence != bCertifiedNoEffect
			|| (bCertifiedNoEffect
				&& (!Record.bRetrySafe
					|| !HyperAIStudio::OperationJournal::Private::IsCertifiedNoEffectEvidence(
						Record, Record.TerminalEvidence))))
		{
			OutError = TEXT("Failed must be pre-commit, or carry the exact core-certified no-effect receipt after a commit marker.");
			return false;
		}
		break;
	}
	case EHyperAIStudioOperationState::RolledBack:
		if (Record.RollbackState != EHyperAIStudioRollbackState::Complete || Record.bPartialCommit)
		{
			OutError = TEXT("Rolled-back operations require complete rollback without partial commit.");
			return false;
		}
		break;
	case EHyperAIStudioOperationState::Partial:
		if ((Record.RollbackState != EHyperAIStudioRollbackState::Partial
				&& Record.RollbackState != EHyperAIStudioRollbackState::Unsupported
				&& Record.RollbackState != EHyperAIStudioRollbackState::Unknown)
			|| !Record.bPartialCommit
			|| Record.bRetrySafe)
		{
			OutError = TEXT("Partial outcomes require possible committed effects, retry-unsafe status, and incomplete/unsupported/unknown rollback.");
			return false;
		}
		break;
	case EHyperAIStudioOperationState::OutcomeUnknown:
		if (Record.RollbackState != EHyperAIStudioRollbackState::Unknown
			|| !Record.bPartialCommit
			|| Record.bRetrySafe
			|| bHasAnyResolutionEvidence
			|| bHasAnyTerminalEvidence)
		{
			OutError = TEXT("Unknown outcomes require possible committed effects, unknown rollback, no retry, and no unverified resolution evidence.");
			return false;
		}
		break;
	default:
		OutError = TEXT("Journal record contains an unknown operation state.");
		return false;
	}
	return true;
}

bool FHyperAIStudioOperationJournal::IsValidSha256Token(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!(Character >= TEXT('0') && Character <= TEXT('9'))
			&& !(Character >= TEXT('a') && Character <= TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioOperationJournal::IsValidExecutionInstanceId(const FString& Value)
{
	if (Value.Len() != 48
		|| !Value.StartsWith(TEXT("editor-"), ESearchCase::CaseSensitive)
		|| Value[15] != TEXT('-'))
	{
		return false;
	}
	bool bHasNonZeroGuidDigit = false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		if (Index == 15)
		{
			continue;
		}
		const TCHAR Character = Value[Index];
		if (!(Character >= TEXT('0') && Character <= TEXT('9'))
			&& !(Character >= TEXT('a') && Character <= TEXT('f')))
		{
			return false;
		}
		if (Index >= 16 && Character != TEXT('0'))
		{
			bHasNonZeroGuidDigit = true;
		}
	}
	return bHasNonZeroGuidDigit;
}

bool FHyperAIStudioOperationJournal::IsValidEvidenceNonce(const FString& Value)
{
	if (Value.Len() < 16 || Value.Len() > 128)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(Character >= TEXT('a') && Character <= TEXT('z'))
			&& !(Character >= TEXT('A') && Character <= TEXT('Z'))
			&& !(Character >= TEXT('0') && Character <= TEXT('9'))
			&& Character != TEXT('-') && Character != TEXT('_') && Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioOperationJournal::IsValidValidatorId(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > 128 || Value[0] < TEXT('a') || Value[0] > TEXT('z'))
	{
		return false;
	}
	TCHAR Previous = 0;
	for (const TCHAR Character : Value)
	{
		const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_') || Character == TEXT('-') || Character == TEXT('.');
		if (!bAllowed || (Character == TEXT('.') && Previous == TEXT('.')))
		{
			return false;
		}
		Previous = Character;
	}
	return Previous != TEXT('.') && Previous != TEXT('-') && Previous != TEXT('_');
}

FString FHyperAIStudioOperationJournal::ComputeGenerationChecksum(
	const int32 InSchemaVersion,
	const int64 InGeneration,
	const FString& InCanonicalProjectId,
	const TArray<FHyperAIStudioOperationRecord>& InRecords)
{
	using HyperAIStudio::OperationJournal::Private::AppendChecksumToken;
	FString Buffer;
	Buffer.Reserve(InRecords.Num() * 256 + 128);
	if (InSchemaVersion >= HyperAIStudio::OperationJournal::Private::SchemaVersion)
	{
		AppendChecksumToken(Buffer, FString::FromInt(InSchemaVersion));
	}
	AppendChecksumToken(Buffer, FString::Printf(TEXT("%lld"), static_cast<long long>(InGeneration)));
	AppendChecksumToken(Buffer, InCanonicalProjectId);
	AppendChecksumToken(Buffer, FString::FromInt(InRecords.Num()));
	for (const FHyperAIStudioOperationRecord& Record : InRecords)
	{
		AppendChecksumToken(Buffer, Record.OperationId);
		AppendChecksumToken(Buffer, Record.CanonicalProjectId);
		AppendChecksumToken(Buffer, Record.PlanHash);
		AppendChecksumToken(Buffer, Record.ExecutionInstanceId);
		AppendChecksumToken(Buffer, Record.CapabilityHash);
		AppendChecksumToken(Buffer, LexToString(Record.State));
		AppendChecksumToken(Buffer, LexToString(Record.RollbackState));
		AppendChecksumToken(Buffer, Record.CreatedUtc);
		AppendChecksumToken(Buffer, Record.UpdatedUtc);
		AppendChecksumToken(Buffer, Record.StatusCode);
		AppendChecksumToken(Buffer, Record.ResolutionValidatorHash);
		AppendChecksumToken(Buffer, Record.ResolutionPostconditionHash);
		AppendChecksumToken(Buffer, Record.ResolvedByExecutionInstanceId);
		if (InSchemaVersion >= HyperAIStudio::OperationJournal::Private::SchemaVersion)
		{
			AppendChecksumToken(Buffer, FString::FromInt(Record.TerminalEvidence.Version));
			AppendChecksumToken(Buffer, Record.TerminalEvidence.CanonicalProjectId);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.OperationId);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.PlanHash);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.CapabilityHash);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.EffectFingerprint);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.ActionNonce);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.ValidatorId);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.ApprovedValidatorFingerprint);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.PostconditionHash);
			AppendChecksumToken(Buffer, Record.TerminalEvidence.ReceiptFingerprint);
			AppendChecksumToken(Buffer, FString::Printf(TEXT("%lld"), static_cast<long long>(Record.TerminalEvidence.IssuedUtcMs)));
			AppendChecksumToken(Buffer, FString::Printf(TEXT("%lld"), static_cast<long long>(Record.TerminalEvidence.ExpiresUtcMs)));
			AppendChecksumToken(Buffer, FString::Printf(TEXT("%lld"), static_cast<long long>(Record.EvidenceAcceptedUtcMs)));
			AppendChecksumToken(Buffer, Record.EvidenceValidatedByExecutionInstanceId);
		}
		AppendChecksumToken(Buffer, Record.bRetrySafe ? TEXT("1") : TEXT("0"));
		AppendChecksumToken(Buffer, Record.bPartialCommit ? TEXT("1") : TEXT("0"));
	}
	return HyperAIStudio::OperationJournal::Private::Sha1Utf8(Buffer);
}

bool FHyperAIStudioOperationJournal::IsLegalTransition(const EHyperAIStudioOperationState From, const EHyperAIStudioOperationState To)
{
	switch (From)
	{
	case EHyperAIStudioOperationState::Queued:
		return To == EHyperAIStudioOperationState::Running
			|| To == EHyperAIStudioOperationState::Failed
			|| To == EHyperAIStudioOperationState::CancelRequested;
	case EHyperAIStudioOperationState::Running:
		return To == EHyperAIStudioOperationState::CommitStarted
			|| To == EHyperAIStudioOperationState::Completed
			|| To == EHyperAIStudioOperationState::Failed
			|| To == EHyperAIStudioOperationState::CancelRequested;
	case EHyperAIStudioOperationState::CommitStarted:
		return To == EHyperAIStudioOperationState::Completed
			|| To == EHyperAIStudioOperationState::Failed
			|| To == EHyperAIStudioOperationState::RolledBack
			|| To == EHyperAIStudioOperationState::Partial
			|| To == EHyperAIStudioOperationState::OutcomeUnknown;
	case EHyperAIStudioOperationState::CancelRequested:
		return To == EHyperAIStudioOperationState::Failed;
	default:
		return false;
	}
}

const TCHAR* FHyperAIStudioOperationJournal::LexToString(const EHyperAIStudioOperationState State)
{
	switch (State)
	{
	case EHyperAIStudioOperationState::Queued: return TEXT("queued");
	case EHyperAIStudioOperationState::Running: return TEXT("running");
	case EHyperAIStudioOperationState::CommitStarted: return TEXT("commit_started");
	case EHyperAIStudioOperationState::Completed: return TEXT("completed");
	case EHyperAIStudioOperationState::Failed: return TEXT("failed");
	case EHyperAIStudioOperationState::CancelRequested: return TEXT("cancel_requested");
	case EHyperAIStudioOperationState::RolledBack: return TEXT("rolled_back");
	case EHyperAIStudioOperationState::Partial: return TEXT("partial");
	case EHyperAIStudioOperationState::OutcomeUnknown: return TEXT("outcome_unknown");
	default: return TEXT("invalid");
	}
}

const TCHAR* FHyperAIStudioOperationJournal::LexToString(const EHyperAIStudioRollbackState State)
{
	switch (State)
	{
	case EHyperAIStudioRollbackState::NotNeeded: return TEXT("not_needed");
	case EHyperAIStudioRollbackState::Complete: return TEXT("complete");
	case EHyperAIStudioRollbackState::Partial: return TEXT("partial");
	case EHyperAIStudioRollbackState::Unsupported: return TEXT("unsupported");
	case EHyperAIStudioRollbackState::Unknown: return TEXT("unknown");
	default: return TEXT("invalid");
	}
}

bool FHyperAIStudioOperationJournal::TryParseOperationState(const FString& Value, EHyperAIStudioOperationState& OutState)
{
#define HYPER_PARSE_OPERATION_STATE(EnumValue) if (Value == LexToString(EHyperAIStudioOperationState::EnumValue)) { OutState = EHyperAIStudioOperationState::EnumValue; return true; }
	HYPER_PARSE_OPERATION_STATE(Queued)
	HYPER_PARSE_OPERATION_STATE(Running)
	HYPER_PARSE_OPERATION_STATE(CommitStarted)
	HYPER_PARSE_OPERATION_STATE(Completed)
	HYPER_PARSE_OPERATION_STATE(Failed)
	HYPER_PARSE_OPERATION_STATE(CancelRequested)
	HYPER_PARSE_OPERATION_STATE(RolledBack)
	HYPER_PARSE_OPERATION_STATE(Partial)
	HYPER_PARSE_OPERATION_STATE(OutcomeUnknown)
#undef HYPER_PARSE_OPERATION_STATE
	return false;
}

bool FHyperAIStudioOperationJournal::TryParseRollbackState(const FString& Value, EHyperAIStudioRollbackState& OutState)
{
#define HYPER_PARSE_ROLLBACK_STATE(EnumValue) if (Value == LexToString(EHyperAIStudioRollbackState::EnumValue)) { OutState = EHyperAIStudioRollbackState::EnumValue; return true; }
	HYPER_PARSE_ROLLBACK_STATE(NotNeeded)
	HYPER_PARSE_ROLLBACK_STATE(Complete)
	HYPER_PARSE_ROLLBACK_STATE(Partial)
	HYPER_PARSE_ROLLBACK_STATE(Unsupported)
	HYPER_PARSE_ROLLBACK_STATE(Unknown)
#undef HYPER_PARSE_ROLLBACK_STATE
	return false;
}
