// Games by Hyper 2026.

#include "HyperAIStudioDiagnosticsCommon.h"

#include "Containers/StringConv.h"
#include "Misc/SecureHash.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioDiagnosticsCommon)

namespace HyperAIStudio::Diagnostics::Common::Private
{
	constexpr int32 MaxHashTokens = 65536;
	constexpr int32 MaxHashUtf16Characters = 4 * 1024 * 1024;
	constexpr int64 MaxHashUtf8Bytes = 8ll * 1024ll * 1024ll;

	class FBoundedSha1Tokens final
	{
	public:
		bool Add(const FString& Token)
		{
			if (++TokenCount > MaxHashTokens
				|| !FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(Token)
				|| FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(Token))
			{
				return false;
			}
			TotalUtf16Characters += Token.Len();
			if (TotalUtf16Characters > MaxHashUtf16Characters)
			{
				return false;
			}
			const FTCHARToUTF8 Utf8(*Token);
			TotalUtf8Bytes += Utf8.Length();
			if (TotalUtf8Bytes > MaxHashUtf8Bytes)
			{
				return false;
			}
			const uint64 Length = static_cast<uint64>(Utf8.Length());
			uint8 LengthBytes[8];
			for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
			{
				LengthBytes[ByteIndex] = static_cast<uint8>((Length >> (ByteIndex * 8)) & 0xff);
			}
			Hasher.Update(LengthBytes, UE_ARRAY_COUNT(LengthBytes));
			if (Utf8.Length() > 0)
			{
				Hasher.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			}
			return true;
		}

		FString Finalize()
		{
			Hasher.Final();
			uint8 Digest[FSHA1::DigestSize];
			Hasher.GetHash(Digest);
			FString Hex;
			Hex.Reserve(FSHA1::DigestSize * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha1:") + Hex;
		}

	private:
		FSHA1 Hasher;
		int32 TokenCount = 0;
		int64 TotalUtf8Bytes = 0;
		int64 TotalUtf16Characters = 0;
	};

	FString StripSha1Prefix(const FString& Fingerprint)
	{
		return Fingerprint.StartsWith(TEXT("sha1:")) ? Fingerprint.RightChop(5) : Fingerprint;
	}

	bool IsSha1Hex(const FString& Value)
	{
		if (Value.Len() != FSHA1::DigestSize * 2)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	bool IsUnsignedDecimal(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 10)
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
		return true;
	}

	FString CursorChecksum(
		const FString& RequestHex,
		const FString& SnapshotHex,
		const FString& Offset)
	{
		return StripSha1Prefix(FHyperAIStudioDiagnosticsCommon::HashTokens({
			TEXT("hyperai.diagnostics.cursor.v1"),
			RequestHex,
			SnapshotHex,
			Offset
		})).Left(16);
	}

	FString MakeCursor(
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		const int32 Offset)
	{
		const FString RequestHex = StripSha1Prefix(RequestFingerprint);
		const FString SnapshotHex = StripSha1Prefix(SnapshotFingerprint);
		const FString OffsetText = FString::FromInt(Offset);
		return FString::Printf(
			TEXT("hdg1.%s.%s.%s.%s"),
			*OffsetText,
			*RequestHex,
			*SnapshotHex,
			*CursorChecksum(RequestHex, SnapshotHex, OffsetText));
	}

	bool ParseCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		const int32 TotalRecords,
		int32& OutOffset,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty())
		{
			return true;
		}
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 5 || Parts[0] != TEXT("hdg1")
			|| !IsUnsignedDecimal(Parts[1]) || !IsSha1Hex(Parts[2]) || !IsSha1Hex(Parts[3])
			|| Parts[4].Len() != 16)
		{
			OutErrorCode = TEXT("invalid_cursor");
			OutError = TEXT("Cursor is malformed or from a different diagnostics schema.");
			return false;
		}
		const FString RequestHex = StripSha1Prefix(RequestFingerprint);
		const FString SnapshotHex = StripSha1Prefix(SnapshotFingerprint);
		if (Parts[2] != RequestHex)
		{
			OutErrorCode = TEXT("cursor_request_mismatch");
			OutError = TEXT("Cursor belongs to a different normalized request.");
			return false;
		}
		if (Parts[3] != SnapshotHex)
		{
			OutErrorCode = TEXT("cursor_snapshot_changed");
			OutError = TEXT("Diagnostics state changed after the cursor was issued.");
			return false;
		}
		if (Parts[4] != CursorChecksum(RequestHex, SnapshotHex, Parts[1]))
		{
			OutErrorCode = TEXT("cursor_checksum_mismatch");
			OutError = TEXT("Cursor integrity check failed.");
			return false;
		}
		const int64 Parsed = FCString::Atoi64(*Parts[1]);
		if (Parsed < 0 || Parsed > TotalRecords || Parsed > MAX_int32)
		{
			OutErrorCode = TEXT("cursor_offset_out_of_range");
			OutError = TEXT("Cursor offset is outside the bounded result set.");
			return false;
		}
		OutOffset = static_cast<int32>(Parsed);
		return true;
	}
}

bool FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(const FString& Value)
{
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		if (Value[Index] == TEXT('\0'))
		{
			return true;
		}
	}
	return false;
}

bool FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(const FString& Value)
{
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (Character >= 0xd800 && Character <= 0xdbff)
		{
			if (++Index >= Value.Len())
			{
				return false;
			}
			const TCHAR Low = Value[Index];
			if (Low < 0xdc00 || Low > 0xdfff)
			{
				return false;
			}
		}
		else if (Character >= 0xdc00 && Character <= 0xdfff)
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(
	const FString& Value,
	const int32 MinCharacters,
	const int32 MaxCharacters)
{
	if (Value.Len() < MinCharacters || Value.Len() > MaxCharacters
		|| ContainsEmbeddedNull(Value) || !HasWellFormedUtf16(Value))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bAsciiAlpha = (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('A') && Character <= TEXT('Z'));
		const bool bAsciiDigit = Character >= TEXT('0') && Character <= TEXT('9');
		if (!(bAsciiAlpha || bAsciiDigit || Character == TEXT('_') || Character == TEXT('-')))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioDiagnosticsCommon::Clip(const FString& Value, const int32 MaxCharacters)
{
	if (!HasWellFormedUtf16(Value) || ContainsEmbeddedNull(Value) || MaxCharacters <= 0)
	{
		return FString();
	}
	FString Result = Value.Left(MaxCharacters);
	if (!Result.IsEmpty())
	{
		const TCHAR Last = Result[Result.Len() - 1];
		if (Last >= 0xd800 && Last <= 0xdbff)
		{
			Result.LeftChopInline(1);
		}
	}
	return Result;
}

FString FHyperAIStudioDiagnosticsCommon::HashTokens(const TArray<FString>& Tokens)
{
	using namespace HyperAIStudio::Diagnostics::Common::Private;
	if (Tokens.Num() > MaxHashTokens)
	{
		return FString();
	}
	FBoundedSha1Tokens Hasher;
	for (const FString& Token : Tokens)
	{
		if (!Hasher.Add(Token))
		{
			return FString();
		}
	}
	return Hasher.Finalize();
}

FString FHyperAIStudioDiagnosticsCommon::MakeRecordId(
	const FString& Kind,
	const FString& Subject,
	const FString& Code)
{
	return TEXT("finding:") + HashTokens({ TEXT("hyperai.diagnostics.finding.v1"), Kind, Subject, Code });
}

int32 FHyperAIStudioDiagnosticsCommon::Utf8Bytes(const FString& Value)
{
	if (!HasWellFormedUtf16(Value))
	{
		return MAX_int32;
	}
	return FTCHARToUTF8(*Value).Length();
}

int32 FHyperAIStudioDiagnosticsCommon::EstimateFindingBytes(
	const FHyperAIStudioDiagnosticsFinding& Finding)
{
	int64 Bytes = 128;
	Bytes += Utf8Bytes(Finding.RecordId);
	Bytes += Utf8Bytes(Finding.Kind);
	Bytes += Utf8Bytes(Finding.Severity);
	Bytes += Utf8Bytes(Finding.Code);
	Bytes += Utf8Bytes(Finding.Subject);
	Bytes += Utf8Bytes(Finding.Message);
	for (const FHyperAIStudioDiagnosticsField& Field : Finding.Fields)
	{
		Bytes += 32 + Utf8Bytes(Field.Name) + Utf8Bytes(Field.Value);
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

void FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
	TArray<FHyperAIStudioDiagnosticsDiagnostic>& Diagnostics,
	const FString& Code,
	const FString& Severity,
	const FString& Field,
	const FString& Message)
{
	if (Diagnostics.Num() >= MaxDiagnostics)
	{
		return;
	}
	FHyperAIStudioDiagnosticsDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Code = Clip(Code, 64);
	Diagnostic.Severity = Clip(Severity, 16);
	Diagnostic.Field = Clip(Field, 128);
	Diagnostic.Message = Clip(Message, MaxDiagnosticCharacters);
}

void FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
	TArray<FHyperAIStudioDiagnosticsDiagnostic>& Diagnostics,
	const FString& Code,
	const FString& Severity,
	const FString& Field,
	const FString& Message)
{
	if (!Diagnostics.ContainsByPredicate([&Code](const FHyperAIStudioDiagnosticsDiagnostic& Existing)
	{
		return Existing.Code == Code;
	}))
	{
		AddDiagnostic(Diagnostics, Code, Severity, Field, Message);
	}
}

void FHyperAIStudioDiagnosticsCommon::AddFindingField(
	FHyperAIStudioDiagnosticsFinding& Finding,
	const FString& Name,
	const FString& Value)
{
	if (Finding.Fields.Num() >= MaxFindingFields)
	{
		return;
	}
	FHyperAIStudioDiagnosticsField& Field = Finding.Fields.AddDefaulted_GetRef();
	Field.Name = Clip(Name, MaxFieldNameCharacters);
	Field.Value = Clip(Value, MaxFieldValueCharacters);
}

bool FHyperAIStudioDiagnosticsCommon::NormalizeAllowlist(
	const TArray<FString>& Requested,
	const TSet<FString>& Allowed,
	const TArray<FString>& Defaults,
	const int32 HardMax,
	TArray<FString>& OutValues,
	FString& OutErrorCode,
	FString& OutError)
{
	OutValues.Reset();
	const TArray<FString>& Source = Requested.IsEmpty() ? Defaults : Requested;
	if (Source.IsEmpty() || Source.Num() > HardMax)
	{
		OutErrorCode = TEXT("invalid_allowlist_size");
		OutError = TEXT("Requested allowlist is empty or exceeds its fixed entry bound.");
		return false;
	}
	TSet<FString> Seen;
	for (const FString& Value : Source)
	{
		if (Value.IsEmpty() || Value.Len() > 64 || ContainsEmbeddedNull(Value)
			|| !HasWellFormedUtf16(Value) || !Allowed.Contains(Value))
		{
			OutErrorCode = TEXT("unsupported_allowlist_value");
			OutError = TEXT("Requested allowlist contains an unsupported value.");
			return false;
		}
		Seen.Add(Value);
	}
	OutValues = Seen.Array();
	OutValues.Sort();
	return true;
}

bool FHyperAIStudioDiagnosticsCommon::ValidatePageBounds(
	const int32 PageSize,
	const FString& Cursor,
	const int32 InMaxOutputBytes,
	FString& OutErrorCode,
	FString& OutError)
{
	if (PageSize < 1 || PageSize > MaxPageSize)
	{
		OutErrorCode = TEXT("invalid_page_size");
		OutError = TEXT("PageSize is outside the fixed supported range.");
		return false;
	}
	if (Cursor.Len() > MaxCursorCharacters || ContainsEmbeddedNull(Cursor) || !HasWellFormedUtf16(Cursor))
	{
		OutErrorCode = TEXT("invalid_cursor_size");
		OutError = TEXT("Cursor exceeds its fixed input bound or is malformed Unicode.");
		return false;
	}
	if (InMaxOutputBytes < MinOutputBytes || InMaxOutputBytes > MaxOutputBytes)
	{
		OutErrorCode = TEXT("invalid_output_budget");
		OutError = TEXT("MaxOutputBytes is outside the fixed supported range.");
		return false;
	}
	return true;
}

FString FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint(
	const TArray<FHyperAIStudioDiagnosticsFinding>& Findings,
	const TArray<FString>& StateTokens)
{
	using namespace HyperAIStudio::Diagnostics::Common::Private;
	FBoundedSha1Tokens Hasher;
	if (!Hasher.Add(TEXT("hyperai.diagnostics.snapshot.v1")))
	{
		return FString();
	}
	for (const FString& Token : StateTokens)
	{
		if (!Hasher.Add(Token))
		{
			return FString();
		}
	}
	for (const FHyperAIStudioDiagnosticsFinding& Finding : Findings)
	{
		if (!Hasher.Add(Finding.RecordId)
			|| !Hasher.Add(Finding.Kind)
			|| !Hasher.Add(Finding.Severity)
			|| !Hasher.Add(Finding.Code)
			|| !Hasher.Add(Finding.Subject)
			|| !Hasher.Add(Finding.Message))
		{
			return FString();
		}
		for (const FHyperAIStudioDiagnosticsField& Field : Finding.Fields)
		{
			if (!Hasher.Add(Field.Name) || !Hasher.Add(Field.Value))
			{
				return FString();
			}
		}
	}
	return Hasher.Finalize();
}

bool FHyperAIStudioDiagnosticsCommon::ProjectFindings(
	const TArray<FHyperAIStudioDiagnosticsFinding>& SortedFindings,
	const FString& RequestFingerprint,
	const FString& SnapshotFingerprint,
	const int32 PageSize,
	const FString& Cursor,
	const int32 InMaxOutputBytes,
	FHyperAIStudioDiagnosticsProjectedPage& OutPage,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Diagnostics::Common::Private;
	OutPage = FHyperAIStudioDiagnosticsProjectedPage();
	OutPage.TotalRecords = SortedFindings.Num();
	int32 Offset = 0;
	if (!ParseCursor(
		Cursor,
		RequestFingerprint,
		SnapshotFingerprint,
		SortedFindings.Num(),
		Offset,
		OutErrorCode,
		OutError))
	{
		OutPage.CursorStatus = TEXT("rejected");
		OutPage.CursorDiagnosticCode = OutErrorCode;
		return false;
	}
	OutPage.CursorStatus = Cursor.IsEmpty() ? TEXT("none") : TEXT("accepted");
	OutPage.CursorDiagnosticCode = Cursor.IsEmpty() ? FString() : TEXT("cursor_accepted");
	OutPage.PageOffset = Offset;
	int32 EstimatedBytes = 1024;
	int32 Index = Offset;
	while (Index < SortedFindings.Num() && OutPage.Findings.Num() < PageSize)
	{
		const int32 FindingBytes = EstimateFindingBytes(SortedFindings[Index]);
		if (EstimatedBytes + FindingBytes > InMaxOutputBytes)
		{
			OutPage.bOutputBudgetReached = true;
			if (OutPage.Findings.IsEmpty())
			{
				// Progress past one individually over-budget record; callers expose
				// bOutputBudgetReached/bTruncated so omission is never silent.
				++Index;
			}
			break;
		}
		EstimatedBytes += FindingBytes;
		OutPage.Findings.Add(SortedFindings[Index]);
		++Index;
	}
	OutPage.ReturnedRecords = OutPage.Findings.Num();
	OutPage.bHasMore = Index < SortedFindings.Num();
	if (OutPage.bHasMore || OutPage.bOutputBudgetReached)
	{
		// A single finding can exceed the minimum response budget by itself. In
		// that case Index was advanced deliberately so the same omitted record is
		// not retried forever. Emit the resulting terminal cursor as evidence of
		// that progress even when no later records remain.
		OutPage.NextCursor = MakeCursor(RequestFingerprint, SnapshotFingerprint, Index);
	}
	return true;
}
