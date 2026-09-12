// Games by Hyper 2026.

#include "HyperAIStudioLogTailToolset.h"

#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioLogTailToolset)

namespace HyperAIStudio::LogTail::Private
{
	FString VerbosityString(const ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity & ELogVerbosity::VerbosityMask)
		{
		case ELogVerbosity::Fatal: return TEXT("fatal");
		case ELogVerbosity::Error: return TEXT("error");
		case ELogVerbosity::Warning: return TEXT("warning");
		case ELogVerbosity::Display: return TEXT("display");
		case ELogVerbosity::Log: return TEXT("log");
		case ELogVerbosity::Verbose: return TEXT("verbose");
		case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
		default: return TEXT("unknown");
		}
	}

	int32 VerbosityValue(const FString& Value)
	{
		if (Value == TEXT("fatal")) return ELogVerbosity::Fatal;
		if (Value == TEXT("error")) return ELogVerbosity::Error;
		if (Value == TEXT("warning")) return ELogVerbosity::Warning;
		if (Value == TEXT("display")) return ELogVerbosity::Display;
		if (Value == TEXT("log")) return ELogVerbosity::Log;
		if (Value == TEXT("verbose")) return ELogVerbosity::Verbose;
		if (Value == TEXT("very_verbose")) return ELogVerbosity::VeryVerbose;
		return INDEX_NONE;
	}

	int32 EntryVerbosityValue(const FString& Value)
	{
		const int32 Parsed = VerbosityValue(Value);
		return Parsed == INDEX_NONE ? ELogVerbosity::VeryVerbose + 1 : Parsed;
	}

	bool IsSafeCategory(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioLogTailContracts::MaxCategoryCharacters
			|| FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(Value)
			|| !FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(Value))
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_')
				|| Character == TEXT('-') || Character == TEXT('.')))
			{
				return false;
			}
		}
		return true;
	}

	FString StripSha1(const FString& Value)
	{
		return Value.StartsWith(TEXT("sha1:")) ? Value.RightChop(5) : Value;
	}

	bool IsUnsignedInt64Text(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 19)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
		}
		return Value.Len() < 19
			|| Value.Compare(TEXT("9223372036854775807"), ESearchCase::CaseSensitive) <= 0;
	}

	FString CursorChecksum(
		const FString& Epoch,
		const FString& RequestHex,
		const FString& NextSequence)
	{
		return StripSha1(FHyperAIStudioDiagnosticsCommon::HashTokens({
			TEXT("hyperai.log-tail.cursor.v1"), Epoch, RequestHex, NextSequence
		})).Left(16);
	}

	FString MakeCursor(
		const FString& Epoch,
		const FString& RequestFingerprint,
		const int64 NextSequence)
	{
		const FString RequestHex = StripSha1(RequestFingerprint);
		const FString SequenceText = FString::Printf(TEXT("%lld"), NextSequence);
		return FString::Printf(
			TEXT("hlog1.%s.%s.%s.%s"),
			*Epoch,
			*RequestHex,
			*SequenceText,
			*CursorChecksum(Epoch, RequestHex, SequenceText));
	}

	bool ParseCursor(
		const FString& Cursor,
		const FString& Epoch,
		const FString& RequestFingerprint,
		int64& OutNextSequence,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutNextSequence = 0;
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 5 || Parts[0] != TEXT("hlog1")
			|| !FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(Parts[1], 16, 64)
			|| Parts[2].Len() != 40 || !IsUnsignedInt64Text(Parts[3]) || Parts[4].Len() != 16)
		{
			OutErrorCode = TEXT("invalid_cursor");
			OutError = TEXT("Log cursor is malformed or from a different schema.");
			return false;
		}
		if (Parts[1] != Epoch)
		{
			OutErrorCode = TEXT("cursor_epoch_changed");
			OutError = TEXT("Log ring epoch changed, usually because the plugin or editor restarted.");
			return false;
		}
		const FString RequestHex = StripSha1(RequestFingerprint);
		if (Parts[2] != RequestHex)
		{
			OutErrorCode = TEXT("cursor_request_mismatch");
			OutError = TEXT("Log cursor belongs to different filters.");
			return false;
		}
		if (Parts[4] != CursorChecksum(Parts[1], Parts[2], Parts[3]))
		{
			OutErrorCode = TEXT("cursor_checksum_mismatch");
			OutError = TEXT("Log cursor integrity check failed.");
			return false;
		}
		OutNextSequence = FCString::Atoi64(*Parts[3]);
		if (OutNextSequence < 1)
		{
			OutErrorCode = TEXT("cursor_sequence_out_of_range");
			OutError = TEXT("Log cursor sequence is outside the valid range.");
			return false;
		}
		return true;
	}

	bool Matches(
		const FHyperAIStudioDiagnosticsLogEntry& Entry,
		const FHyperAIStudioDiagnosticsNormalizedLogTailRequest& Request)
	{
		if (!Request.Categories.IsEmpty() && !Request.Categories.Contains(Entry.Category))
		{
			return false;
		}
		if (EntryVerbosityValue(Entry.Verbosity) > Request.MaxVerbosityValue)
		{
			return false;
		}
		return Request.ContainsLower.IsEmpty()
			|| Entry.Message.ToLower().Contains(Request.ContainsLower);
	}

	int32 EstimateEntryBytes(const FHyperAIStudioDiagnosticsLogEntry& Entry)
	{
		return 96
			+ FHyperAIStudioDiagnosticsCommon::Utf8Bytes(Entry.Category)
			+ FHyperAIStudioDiagnosticsCommon::Utf8Bytes(Entry.Verbosity)
			+ FHyperAIStudioDiagnosticsCommon::Utf8Bytes(Entry.Message);
	}

	FHyperAIStudioDiagnosticsLogTailResult InvalidResult(
		const FString& Code,
		const FString& Field,
		const FString& Message,
		const FString& Cursor)
	{
		FHyperAIStudioDiagnosticsLogTailResult Result;
		Result.Status = TEXT("invalid_request");
		Result.bIncomplete = false;
		Result.CursorStatus = Cursor.IsEmpty() ? TEXT("none") : TEXT("rejected");
		Result.CursorDiagnosticCode = Cursor.IsEmpty() ? FString() : Code;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, Code, TEXT("error"), Field, Message);
		return Result;
	}
}

bool FHyperAIStudioLogTailContracts::NormalizeRequest(
	const FHyperAIStudioDiagnosticsLogTailRequest& Request,
	FHyperAIStudioDiagnosticsNormalizedLogTailRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::LogTail::Private;
	OutRequest = FHyperAIStudioDiagnosticsNormalizedLogTailRequest();
	if (Request.Categories.Num() > MaxCategories)
	{
		OutErrorCode = TEXT("too_many_categories");
		OutError = TEXT("Category filters exceed the hard input bound.");
		return false;
	}
	TSet<FString> Categories;
	for (const FString& Category : Request.Categories)
	{
		if (!IsSafeCategory(Category))
		{
			OutErrorCode = TEXT("invalid_category_filter");
			OutError = TEXT("Each category must be a bounded exact identifier.");
			return false;
		}
		Categories.Add(Category);
	}
	OutRequest.Categories = Categories.Array();
	OutRequest.Categories.Sort();
	OutRequest.MaxVerbosityValue = VerbosityValue(Request.MaxVerbosity);
	if (OutRequest.MaxVerbosityValue == INDEX_NONE)
	{
		OutErrorCode = TEXT("unsupported_verbosity");
		OutError = TEXT("MaxVerbosity must be a closed supported value.");
		return false;
	}
	OutRequest.MaxVerbosity = Request.MaxVerbosity;
	if (Request.Contains.Len() > MaxContainsCharacters
		|| FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(Request.Contains)
		|| !FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(Request.Contains))
	{
		OutErrorCode = TEXT("invalid_contains_filter");
		OutError = TEXT("Contains exceeds its fixed bound or contains malformed text.");
		return false;
	}
	OutRequest.ContainsLower = Request.Contains.ToLower();
	if (!FHyperAIStudioDiagnosticsCommon::ValidatePageBounds(
		Request.PageSize, Request.Cursor, Request.MaxOutputBytes, OutErrorCode, OutError))
	{
		return false;
	}
	OutRequest.PageSize = Request.PageSize;
	OutRequest.Cursor = Request.Cursor;
	OutRequest.MaxOutputBytes = Request.MaxOutputBytes;
	OutRequest.RequestFingerprint = FHyperAIStudioDiagnosticsCommon::HashTokens({
		TEXT("hyperai.log-tail.request.v1"),
		FString::Join(OutRequest.Categories, TEXT(",")),
		OutRequest.MaxVerbosity,
		OutRequest.ContainsLower
	});
	if (OutRequest.RequestFingerprint.IsEmpty())
	{
		OutErrorCode = TEXT("request_fingerprint_failed");
		OutError = TEXT("Log filter request could not be fingerprinted within fixed bounds.");
		return false;
	}
	return true;
}

FHyperAIStudioDiagnosticsLogTailResult FHyperAIStudioLogTailContracts::Analyze(
	const FHyperAIStudioDiagnosticsLogSnapshot& Snapshot,
	const FHyperAIStudioDiagnosticsLogTailRequest& Request)
{
	using namespace HyperAIStudio::LogTail::Private;
	FHyperAIStudioDiagnosticsNormalizedLogTailRequest Normalized;
	FString ErrorCode;
	FString Error;
	if (!NormalizeRequest(Request, Normalized, ErrorCode, Error))
	{
		return InvalidResult(ErrorCode, TEXT("request"), Error, Request.Cursor);
	}
	FHyperAIStudioDiagnosticsLogTailResult Result;
	Result.RequestFingerprint = Normalized.RequestFingerprint;
	Result.BufferEpoch = Snapshot.Epoch;
	Result.bAttached = Snapshot.bAttached;
	Result.EarliestRetainedSequence = Snapshot.EarliestSequence;
	Result.LatestRetainedSequence = Snapshot.LatestSequence;
	if (!Snapshot.bAttached || Snapshot.Epoch.IsEmpty())
	{
		Result.Status = TEXT("unavailable");
		Result.bIncomplete = true;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics,
			TEXT("log_ring_unavailable"),
			TEXT("error"),
			TEXT("log_ring"),
			TEXT("The lifecycle-managed bounded process log ring is not attached."));
		return Result;
	}

	TArray<const FHyperAIStudioDiagnosticsLogEntry*> Matching;
	Matching.Reserve(FMath::Min(Snapshot.Entries.Num(), MaxRingEntries));
	for (const FHyperAIStudioDiagnosticsLogEntry& Entry : Snapshot.Entries)
	{
		if (Matches(Entry, Normalized))
		{
			Matching.Add(&Entry);
		}
	}

	int64 NextSequence = Snapshot.EarliestSequence > 0 ? Snapshot.EarliestSequence : 1;
	int32 MatchingIndex = 0;
	if (Normalized.Cursor.IsEmpty())
	{
		MatchingIndex = FMath::Max(0, Matching.Num() - Normalized.PageSize);
		Result.bInitialTailOmittedOlderMatches = MatchingIndex > 0;
		if (MatchingIndex < Matching.Num())
		{
			NextSequence = Matching[MatchingIndex]->Sequence;
		}
	}
	else
	{
		if (!ParseCursor(
			Normalized.Cursor,
			Snapshot.Epoch,
			Normalized.RequestFingerprint,
			NextSequence,
			ErrorCode,
			Error))
		{
			return InvalidResult(ErrorCode, TEXT("cursor"), Error, Request.Cursor);
		}
		Result.CursorStatus = TEXT("accepted");
		Result.CursorDiagnosticCode = TEXT("cursor_accepted");
		if (Snapshot.EarliestSequence > 0 && NextSequence < Snapshot.EarliestSequence)
		{
			Result.bHistoryGap = true;
			Result.bIncomplete = true;
			NextSequence = Snapshot.EarliestSequence;
			FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
				Result.Diagnostics,
				TEXT("log_history_gap"),
				TEXT("warning"),
				TEXT("cursor"),
				TEXT("Requested log sequence was overwritten by the bounded ring; results resume at the earliest retained entry."));
		}
		if (NextSequence > Snapshot.LatestSequence + 1)
		{
			return InvalidResult(
				TEXT("cursor_sequence_in_future"),
				TEXT("cursor"),
				TEXT("Log cursor sequence is newer than this buffer snapshot."),
				Request.Cursor);
		}
		while (MatchingIndex < Matching.Num() && Matching[MatchingIndex]->Sequence < NextSequence)
		{
			++MatchingIndex;
		}
	}

	int32 EstimatedBytes = 1024;
	int32 Index = MatchingIndex;
	while (Index < Matching.Num() && Result.Entries.Num() < Normalized.PageSize)
	{
		const int32 EntryBytes = EstimateEntryBytes(*Matching[Index]);
		if (EstimatedBytes + EntryBytes > Normalized.MaxOutputBytes)
		{
			Result.bOutputBudgetReached = true;
			if (Result.Entries.IsEmpty())
			{
				// Advance past one individually over-budget record so a polling
				// cursor cannot stall; bOutputBudgetReached/bTruncated disclose it.
				++Index;
			}
			break;
		}
		EstimatedBytes += EntryBytes;
		Result.Entries.Add(*Matching[Index]);
		++Index;
	}
	Result.ReturnedRecords = Result.Entries.Num();
	Result.bHasMore = Index < Matching.Num();
	const int64 CursorSequence = Result.bHasMore
		? Matching[Index]->Sequence
		: (Snapshot.LatestSequence < MAX_int64 ? Snapshot.LatestSequence + 1 : MAX_int64);
	Result.NextCursor = MakeCursor(
		Snapshot.Epoch,
		Normalized.RequestFingerprint,
		FMath::Max<int64>(1, CursorSequence));
	Result.bIncomplete |= Result.bHistoryGap;
	Result.bTruncated = Result.bIncomplete || Result.bHasMore
		|| Result.bOutputBudgetReached || Result.bInitialTailOmittedOlderMatches;
	Result.Status = Result.bIncomplete ? TEXT("partial") : TEXT("complete");
	if (Normalized.Cursor.IsEmpty())
	{
		Result.CursorStatus = TEXT("none");
	}
	if (Result.bInitialTailOmittedOlderMatches)
	{
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("initial_tail_omitted_older_matches"),
			TEXT("info"),
			TEXT("page_size"),
			TEXT("Initial tail returns only the newest matching page; use the emitted cursor for later entries, not historical backfill."));
	}
	if (Result.bOutputBudgetReached)
	{
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("output_budget_reached"),
			TEXT("warning"),
			TEXT("max_output_bytes"),
			TEXT("Log projection reached the fixed approximate serialized output budget."));
	}
	return Result;
}

FHyperAIStudioDiagnosticsLogBuffer& FHyperAIStudioDiagnosticsLogBuffer::Get()
{
	static FHyperAIStudioDiagnosticsLogBuffer Buffer;
	return Buffer;
}

void FHyperAIStudioDiagnosticsLogBuffer::Startup()
{
	{
		FScopeLock Lock(&Mutex);
		if (bAttached)
		{
			return;
		}
		Slots.SetNum(FHyperAIStudioLogTailContracts::MaxRingEntries);
		Count = 0;
		NextSlot = 0;
		TotalCharacters = 0;
		NextSequence = 1;
		Epoch = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	if (GLog)
	{
		GLog->AddOutputDevice(this);
		FScopeLock Lock(&Mutex);
		bAttached = true;
	}
}

void FHyperAIStudioDiagnosticsLogBuffer::Shutdown()
{
	bool bWasAttached = false;
	{
		FScopeLock Lock(&Mutex);
		bWasAttached = bAttached;
		bAttached = false;
	}
	if (bWasAttached && GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
	FScopeLock Lock(&Mutex);
	Slots.Reset();
	Count = 0;
	NextSlot = 0;
	TotalCharacters = 0;
	Epoch.Reset();
}

void FHyperAIStudioDiagnosticsLogBuffer::Serialize(
	const TCHAR* Value,
	const ELogVerbosity::Type Verbosity,
	const FName& Category)
{
	Serialize(Value, Verbosity, Category, FPlatformTime::Seconds());
}

void FHyperAIStudioDiagnosticsLogBuffer::Serialize(
	const TCHAR* Value,
	const ELogVerbosity::Type Verbosity,
	const FName& Category,
	const double Time)
{
	if (!Value)
	{
		return;
	}
	FHyperAIStudioDiagnosticsLogEntry Entry;
	Entry.TimeSeconds = FMath::IsFinite(Time) ? Time : 0.0;
	Entry.Category = FHyperAIStudioDiagnosticsCommon::Clip(
		Category.ToString(), FHyperAIStudioLogTailContracts::MaxCategoryCharacters);
	Entry.Verbosity = HyperAIStudio::LogTail::Private::VerbosityString(Verbosity);
	const int32 BoundedLength = FCString::Strnlen(
		Value, FHyperAIStudioLogTailContracts::MaxMessageCharacters + 1);
	Entry.Message = FHyperAIStudioDiagnosticsCommon::Clip(
		FString(
			FMath::Min(BoundedLength, FHyperAIStudioLogTailContracts::MaxMessageCharacters),
			Value),
		FHyperAIStudioLogTailContracts::MaxMessageCharacters);
	FScopeLock Lock(&Mutex);
	if (bAttached && !Entry.Message.IsEmpty())
	{
		AppendLocked(MoveTemp(Entry));
	}
}

void FHyperAIStudioDiagnosticsLogBuffer::AppendLocked(
	FHyperAIStudioDiagnosticsLogEntry&& Entry)
{
	const int32 EntryCharacters = Entry.Category.Len() + Entry.Verbosity.Len() + Entry.Message.Len();
	while (Count > 0 && (Count >= FHyperAIStudioLogTailContracts::MaxRingEntries
		|| TotalCharacters + EntryCharacters > FHyperAIStudioLogTailContracts::MaxRingCharacters))
	{
		RemoveOldestLocked();
	}
	if (EntryCharacters > FHyperAIStudioLogTailContracts::MaxRingCharacters || Slots.IsEmpty())
	{
		return;
	}
	Entry.Sequence = NextSequence++;
	if (Count < Slots.Num())
	{
		++Count;
	}
	else
	{
		TotalCharacters -= Slots[NextSlot].Category.Len()
			+ Slots[NextSlot].Verbosity.Len() + Slots[NextSlot].Message.Len();
	}
	TotalCharacters += EntryCharacters;
	Slots[NextSlot] = MoveTemp(Entry);
	NextSlot = (NextSlot + 1) % Slots.Num();
}

void FHyperAIStudioDiagnosticsLogBuffer::RemoveOldestLocked()
{
	if (Count <= 0 || Slots.IsEmpty())
	{
		return;
	}
	const int32 Oldest = (NextSlot - Count + Slots.Num()) % Slots.Num();
	TotalCharacters -= Slots[Oldest].Category.Len()
		+ Slots[Oldest].Verbosity.Len() + Slots[Oldest].Message.Len();
	Slots[Oldest] = FHyperAIStudioDiagnosticsLogEntry();
	--Count;
}

FHyperAIStudioDiagnosticsLogSnapshot FHyperAIStudioDiagnosticsLogBuffer::Snapshot() const
{
	FScopeLock Lock(&Mutex);
	FHyperAIStudioDiagnosticsLogSnapshot Result;
	Result.Epoch = Epoch;
	Result.bAttached = bAttached;
	if (Count <= 0 || Slots.IsEmpty())
	{
		Result.EarliestSequence = NextSequence;
		Result.LatestSequence = NextSequence - 1;
		return Result;
	}
	const int32 Oldest = (NextSlot - Count + Slots.Num()) % Slots.Num();
	Result.Entries.Reserve(Count);
	for (int32 Offset = 0; Offset < Count; ++Offset)
	{
		Result.Entries.Add(Slots[(Oldest + Offset) % Slots.Num()]);
	}
	Result.EarliestSequence = Result.Entries[0].Sequence;
	Result.LatestSequence = Result.Entries.Last().Sequence;
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioDiagnosticsLogBuffer::ResetForTests(const FString& InEpoch)
{
	FScopeLock Lock(&Mutex);
	Slots.SetNum(FHyperAIStudioLogTailContracts::MaxRingEntries);
	Count = 0;
	NextSlot = 0;
	TotalCharacters = 0;
	NextSequence = 1;
	Epoch = InEpoch;
	bAttached = true;
}

void FHyperAIStudioDiagnosticsLogBuffer::AppendForTests(
	const FHyperAIStudioDiagnosticsLogEntry& InEntry)
{
	FHyperAIStudioDiagnosticsLogEntry Entry = InEntry;
	Entry.Category = FHyperAIStudioDiagnosticsCommon::Clip(
		Entry.Category, FHyperAIStudioLogTailContracts::MaxCategoryCharacters);
	Entry.Message = FHyperAIStudioDiagnosticsCommon::Clip(
		Entry.Message, FHyperAIStudioLogTailContracts::MaxMessageCharacters);
	FScopeLock Lock(&Mutex);
	AppendLocked(MoveTemp(Entry));
}
#endif

FHyperAIStudioDiagnosticsLogTailResult UHyperAIStudioLogTailToolset::hyper_log_tail(
	const FHyperAIStudioDiagnosticsLogTailRequest& Request)
{
	return FHyperAIStudioLogTailContracts::Analyze(
		FHyperAIStudioDiagnosticsLogBuffer::Get().Snapshot(), Request);
}
