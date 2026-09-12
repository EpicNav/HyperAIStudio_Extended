// Games by Hyper 2026.

#include "HyperAIStudioManagedConfigMigration.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Crc.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::ManagedConfigMigration::Private
{
	using FCondensedWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

	struct FJsonValidationFrame
	{
		bool bObject = false;
		TSet<FString> ObjectKeys;
	};

	struct FMarkerEvent
	{
		int32 Offset = INDEX_NONE;
		bool bBegin = false;
	};

	bool IsControlCharacter(const TCHAR Character)
	{
		return Character < 0x20 || Character == 0x7f;
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (IsControlCharacter(Character))
			{
				return true;
			}
		}
		return false;
	}

	void FindOccurrences(const FString& Source, const FString& Marker, TArray<int32>& OutOffsets)
	{
		int32 SearchOffset = 0;
		while (SearchOffset <= Source.Len() - Marker.Len())
		{
			const int32 FoundOffset = Source.Find(
				Marker,
				ESearchCase::CaseSensitive,
				ESearchDir::FromStart,
				SearchOffset);
			if (FoundOffset == INDEX_NONE)
			{
				break;
			}

			OutOffsets.Add(FoundOffset);
			SearchOffset = FoundOffset + Marker.Len();
		}
	}

	bool AddJsonValueToValidationParent(
		const FString& Identifier,
		TArray<FJsonValidationFrame>& Frames,
		int32& RootValueCount,
		int32& NodeCount,
		FString& OutReasonCode)
	{
		++NodeCount;
		if (NodeCount > FHyperAIStudioCanonicalJson::MaxNodes)
		{
			OutReasonCode = TEXT("json_node_limit");
			return false;
		}

		if (Frames.IsEmpty())
		{
			++RootValueCount;
			if (RootValueCount != 1)
			{
				OutReasonCode = TEXT("json_multiple_root_values");
				return false;
			}
			return true;
		}

		FJsonValidationFrame& Parent = Frames.Last();
		if (Parent.bObject)
		{
			if (Parent.ObjectKeys.Contains(Identifier))
			{
				OutReasonCode = TEXT("json_duplicate_object_key");
				return false;
			}
			Parent.ObjectKeys.Add(Identifier);
		}
		return true;
	}

	bool ValidateJsonTokenStream(const FString& JsonText, FString& OutReasonCode)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		TArray<FJsonValidationFrame> Frames;
		int32 RootValueCount = 0;
		int32 NodeCount = 0;
		EJsonNotation Notation = EJsonNotation::Error;

		while (Reader->ReadNext(Notation))
		{
			const FString Identifier = Reader->GetIdentifier();
			switch (Notation)
			{
			case EJsonNotation::ObjectStart:
			case EJsonNotation::ArrayStart:
				if (!AddJsonValueToValidationParent(
					Identifier,
					Frames,
					RootValueCount,
					NodeCount,
					OutReasonCode))
				{
					return false;
				}
				Frames.Add({ Notation == EJsonNotation::ObjectStart, {} });
				if (Frames.Num() > FHyperAIStudioCanonicalJson::MaxDepth)
				{
					OutReasonCode = TEXT("json_depth_limit");
					return false;
				}
				break;

			case EJsonNotation::ObjectEnd:
				if (Frames.IsEmpty() || !Frames.Last().bObject)
				{
					OutReasonCode = TEXT("json_mismatched_object_end");
					return false;
				}
				Frames.Pop(EAllowShrinking::No);
				break;

			case EJsonNotation::ArrayEnd:
				if (Frames.IsEmpty() || Frames.Last().bObject)
				{
					OutReasonCode = TEXT("json_mismatched_array_end");
					return false;
				}
				Frames.Pop(EAllowShrinking::No);
				break;

			case EJsonNotation::Boolean:
			case EJsonNotation::String:
			case EJsonNotation::Number:
			case EJsonNotation::Null:
				if (!AddJsonValueToValidationParent(
					Identifier,
					Frames,
					RootValueCount,
					NodeCount,
					OutReasonCode))
				{
					return false;
				}
				break;

			case EJsonNotation::Error:
			default:
				OutReasonCode = TEXT("json_reader_error");
				return false;
			}
		}

		if (!Reader->GetErrorMessage().IsEmpty())
		{
			OutReasonCode = TEXT("json_parse_error");
			return false;
		}
		if (!Frames.IsEmpty() || RootValueCount != 1)
		{
			OutReasonCode = RootValueCount == 0
				? TEXT("json_empty_document")
				: TEXT("json_unclosed_container");
			return false;
		}
		return true;
	}

	bool ParseStrictJson(
		const FString& JsonText,
		const int32 MaxUtf8Bytes,
		TSharedPtr<FJsonValue>& OutRoot,
		FString& OutReasonCode)
	{
		OutRoot.Reset();
		OutReasonCode.Reset();
		const FTCHARToUTF8 Utf8(*JsonText);
		if (Utf8.Length() > MaxUtf8Bytes)
		{
			OutReasonCode = TEXT("json_input_too_large");
			return false;
		}
		if (!ValidateJsonTokenStream(JsonText, OutReasonCode))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(
			Reader,
			OutRoot,
			FJsonSerializer::EFlags::StoreNumbersAsStrings)
			|| !OutRoot.IsValid()
			|| !Reader->GetErrorMessage().IsEmpty())
		{
			OutRoot.Reset();
			OutReasonCode = TEXT("json_deserialize_failed");
			return false;
		}
		return true;
	}

	bool WriteCanonicalValue(
		const TSharedPtr<FJsonValue>& Value,
		FCondensedWriter& Writer,
		const FString* Identifier,
		const int32 Depth,
		FString& OutReasonCode)
	{
		if (!Value.IsValid())
		{
			OutReasonCode = TEXT("json_null_dom_value");
			return false;
		}
		if (Depth > FHyperAIStudioCanonicalJson::MaxDepth)
		{
			OutReasonCode = TEXT("json_depth_limit");
			return false;
		}

		switch (Value->Type)
		{
		case EJson::Object:
		{
			if (Identifier)
			{
				Writer.WriteObjectStart(*Identifier);
			}
			else
			{
				Writer.WriteObjectStart();
			}

			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				OutReasonCode = TEXT("json_invalid_object_value");
				return false;
			}
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Members;
			Members.Reserve(Object->Values.Num());
			for (const TPair<FJsonObject::FStringType, TSharedPtr<FJsonValue>>& Member : Object->Values)
			{
				Members.Emplace(FString(Member.Key), Member.Value);
			}
			Members.Sort([](
				const TPair<FString, TSharedPtr<FJsonValue>>& Left,
				const TPair<FString, TSharedPtr<FJsonValue>>& Right)
			{
				return Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive) < 0;
			});
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Member : Members)
			{
				if (!WriteCanonicalValue(Member.Value, Writer, &Member.Key, Depth + 1, OutReasonCode))
				{
					return false;
				}
			}
			Writer.WriteObjectEnd();
			return true;
		}

		case EJson::Array:
		{
			if (Identifier)
			{
				Writer.WriteArrayStart(*Identifier);
			}
			else
			{
				Writer.WriteArrayStart();
			}
			for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
			{
				if (!WriteCanonicalValue(Child, Writer, nullptr, Depth + 1, OutReasonCode))
				{
					return false;
				}
			}
			Writer.WriteArrayEnd();
			return true;
		}

		case EJson::String:
			if (Identifier)
			{
				Writer.WriteValue(*Identifier, Value->AsString());
			}
			else
			{
				Writer.WriteValue(Value->AsString());
			}
			return true;

		case EJson::Number:
		{
			FString RawNumber;
			if (Value->PreferStringRepresentation() && Value->TryGetString(RawNumber))
			{
				if (Identifier)
				{
					Writer.WriteRawJSONValue(*Identifier, RawNumber);
				}
				else
				{
					Writer.WriteRawJSONValue(RawNumber);
				}
			}
			else if (Identifier)
			{
				Writer.WriteValue(*Identifier, Value->AsNumber());
			}
			else
			{
				Writer.WriteValue(Value->AsNumber());
			}
			return true;
		}

		case EJson::Boolean:
			if (Identifier)
			{
				Writer.WriteValue(*Identifier, Value->AsBool());
			}
			else
			{
				Writer.WriteValue(Value->AsBool());
			}
			return true;

		case EJson::Null:
		case EJson::None:
			if (Identifier)
			{
				Writer.WriteNull(*Identifier);
			}
			else
			{
				Writer.WriteNull();
			}
			return true;

		default:
			OutReasonCode = TEXT("json_unsupported_value_type");
			return false;
		}
	}

	uint32 RotateRight(const uint32 Value, const uint32 Shift)
	{
		return (Value >> Shift) | (Value << (32u - Shift));
	}

	FString Sha256Hex(const uint8* Bytes, const int32 ByteCount)
	{
		static constexpr uint32 RoundConstants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
			0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
			0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
			0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
			0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
			0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
			0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
			0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
			0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		uint32 State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};

		const int32 PaddedByteCount = ((ByteCount + 1 + 8 + 63) / 64) * 64;
		TArray<uint8> Padded;
		Padded.SetNumZeroed(PaddedByteCount);
		if (ByteCount > 0)
		{
			FMemory::Memcpy(Padded.GetData(), Bytes, ByteCount);
		}
		Padded[ByteCount] = 0x80;
		const uint64 BitCount = static_cast<uint64>(ByteCount) * 8ull;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Padded[PaddedByteCount - 1 - Index] = static_cast<uint8>(BitCount >> (Index * 8));
		}

		for (int32 ChunkOffset = 0; ChunkOffset < PaddedByteCount; ChunkOffset += 64)
		{
			uint32 Words[64] = {};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = ChunkOffset + Index * 4;
				Words[Index] =
					(static_cast<uint32>(Padded[Offset]) << 24)
					| (static_cast<uint32>(Padded[Offset + 1]) << 16)
					| (static_cast<uint32>(Padded[Offset + 2]) << 8)
					| static_cast<uint32>(Padded[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 S0 = RotateRight(Words[Index - 15], 7)
					^ RotateRight(Words[Index - 15], 18)
					^ (Words[Index - 15] >> 3);
				const uint32 S1 = RotateRight(Words[Index - 2], 17)
					^ RotateRight(Words[Index - 2], 19)
					^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
			}

			uint32 A = State[0];
			uint32 B = State[1];
			uint32 C = State[2];
			uint32 D = State[3];
			uint32 E = State[4];
			uint32 F = State[5];
			uint32 G = State[6];
			uint32 H = State[7];

			for (int32 Index = 0; Index < 64; ++Index)
			{
				const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ (~E & G);
				const uint32 Temp1 = H + S1 + Choice + RoundConstants[Index] + Words[Index];
				const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = S0 + Majority;

				H = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}

			State[0] += A;
			State[1] += B;
			State[2] += C;
			State[3] += D;
			State[4] += E;
			State[5] += F;
			State[6] += G;
			State[7] += H;
		}

		FString Result;
		Result.Reserve(64);
		for (const uint32 Word : State)
		{
			Result.Appendf(TEXT("%08x"), Word);
		}
		return Result;
	}

	FHyperAIStudioCanonicalJsonResult CanonicalizeParsedObject(const TSharedPtr<FJsonValue>& Root)
	{
		FHyperAIStudioCanonicalJsonResult Result;
		if (!Root.IsValid() || Root->Type != EJson::Object)
		{
			Result.ReasonCode = TEXT("json_root_not_object");
			return Result;
		}

		const TSharedRef<FCondensedWriter> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result.CanonicalJson);
		if (!WriteCanonicalValue(Root, *Writer, nullptr, 1, Result.ReasonCode) || !Writer->Close())
		{
			Result.CanonicalJson.Reset();
			if (Result.ReasonCode.IsEmpty())
			{
				Result.ReasonCode = TEXT("json_canonical_serialize_failed");
			}
			return Result;
		}

		const FTCHARToUTF8 Utf8(*Result.CanonicalJson);
		if (Utf8.Length() > FHyperAIStudioCanonicalJson::MaxCanonicalUtf8Bytes)
		{
			Result.CanonicalJson.Reset();
			Result.ReasonCode = TEXT("json_canonical_output_too_large");
			return Result;
		}

		Result.ObjectHash = TEXT("sha256:") + Sha256Hex(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length());
		Result.bSuccess = true;
		Result.ReasonCode = TEXT("json_object_hashed");
		return Result;
	}

	bool HasExactFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Fields)
	{
		if (!Object.IsValid() || Object->Values.Num() != Fields.Num())
		{
			return false;
		}
		for (const FString& Field : Fields)
		{
			if (!Object->HasField(Field))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadStringField(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		return Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(OutValue);
	}

	bool ReadIntField(const TSharedPtr<FJsonObject>& Object, const FString& Field, int32& OutValue)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(OutValue);
	}

	TSharedPtr<FJsonObject> ReadObjectField(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		return Value.IsValid() && Value->Type == EJson::Object
			? Value->AsObject()
			: nullptr;
	}

	bool NormalizeRelativePath(FString& InOutPath)
	{
		if (InOutPath.IsEmpty()
			|| InOutPath.Len() > FHyperAIStudioManagedConfigLedgerCodec::MaxRelativePathCharacters
			|| HasControlCharacter(InOutPath)
			|| InOutPath.Contains(TEXT(":")))
		{
			return false;
		}

		InOutPath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		if (InOutPath.StartsWith(TEXT("/"))
			|| InOutPath.EndsWith(TEXT("/"))
			|| InOutPath.Contains(TEXT("//")))
		{
			return false;
		}

		TArray<FString> Segments;
		InOutPath.ParseIntoArray(Segments, TEXT("/"), false);
		if (Segments.IsEmpty())
		{
			return false;
		}
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
			{
				return false;
			}
		}
		return true;
	}

	bool IsStableClientId(const FString& ClientId)
	{
		if (ClientId.IsEmpty()
			|| ClientId.Len() > FHyperAIStudioManagedConfigLedgerCodec::MaxClientIdCharacters)
		{
			return false;
		}
		for (const TCHAR Character : ClientId)
		{
			if (!((Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('-')
				|| Character == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeEntryId(const FString& EntryId)
	{
		return !EntryId.IsEmpty()
			&& EntryId.Len() <= FHyperAIStudioManagedConfigLedgerCodec::MaxEntryIdCharacters
			&& !HasControlCharacter(EntryId);
	}

	bool IsSupportedClientCoordinate(
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container)
	{
		if (ClientId == TEXT("claude"))
		{
			return RelativePath == TEXT(".mcp.json") && Container == TEXT("mcpServers");
		}
		if (ClientId == TEXT("cursor"))
		{
			return RelativePath == TEXT(".cursor/mcp.json") && Container == TEXT("mcpServers");
		}
		if (ClientId == TEXT("vscode"))
		{
			return RelativePath == TEXT(".vscode/mcp.json") && Container == TEXT("servers");
		}
		if (ClientId == TEXT("gemini"))
		{
			return RelativePath == TEXT(".gemini/settings.json") && Container == TEXT("mcpServers");
		}
		return false;
	}

	bool ValidateAndNormalizeLedger(
		FHyperAIStudioManagedConfigOwnershipLedger& Ledger,
		FString& OutReasonCode)
	{
		if (Ledger.SchemaVersion != FHyperAIStudioManagedConfigLedgerCodec::CurrentSchemaVersion)
		{
			OutReasonCode = TEXT("ledger_unsupported_schema");
			return false;
		}
		if (Ledger.Generator != TEXT("HyperAIStudio"))
		{
			OutReasonCode = TEXT("ledger_wrong_generator");
			return false;
		}
		if (Ledger.Clients.Num() > FHyperAIStudioManagedConfigLedgerCodec::MaxClients)
		{
			OutReasonCode = TEXT("ledger_client_limit");
			return false;
		}

		TSet<FString> ClientIds;
		for (FHyperAIStudioManagedConfigLedgerClient& Client : Ledger.Clients)
		{
			if (!IsStableClientId(Client.ClientId) || ClientIds.Contains(Client.ClientId))
			{
				OutReasonCode = TEXT("ledger_invalid_or_duplicate_client_id");
				return false;
			}
			ClientIds.Add(Client.ClientId);
			if (!NormalizeRelativePath(Client.RelativePath))
			{
				OutReasonCode = TEXT("ledger_invalid_relative_path");
				return false;
			}
			if (Client.Container.IsEmpty()
				|| Client.Container.Len() > FHyperAIStudioManagedConfigLedgerCodec::MaxContainerCharacters
				|| HasControlCharacter(Client.Container)
				|| !IsSupportedClientCoordinate(Client.ClientId, Client.RelativePath, Client.Container))
			{
				OutReasonCode = TEXT("ledger_unsupported_client_coordinate");
				return false;
			}
			if (Client.Entries.Num() > FHyperAIStudioManagedConfigLedgerCodec::MaxEntriesPerClient)
			{
				OutReasonCode = TEXT("ledger_entry_limit");
				return false;
			}

			TSet<FString> EntryIds;
			for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : Client.Entries)
			{
				if (!IsSafeEntryId(Entry.Id) || EntryIds.Contains(Entry.Id))
				{
					OutReasonCode = TEXT("ledger_invalid_or_duplicate_entry_id");
					return false;
				}
				EntryIds.Add(Entry.Id);
				if (!FHyperAIStudioCanonicalJson::IsValidObjectHash(Entry.ObjectHash))
				{
					OutReasonCode = TEXT("ledger_invalid_object_hash");
					return false;
				}
				if (Entry.WriterVersion
					!= FHyperAIStudioManagedConfigLedgerCodec::CurrentWriterVersion)
				{
					OutReasonCode = TEXT("ledger_unsupported_writer_version");
					return false;
				}
			}

			Client.Entries.Sort([](
				const FHyperAIStudioManagedConfigLedgerEntry& Left,
				const FHyperAIStudioManagedConfigLedgerEntry& Right)
			{
				return Left.Id.Compare(Right.Id, ESearchCase::CaseSensitive) < 0;
			});
		}

		Ledger.Clients.Sort([](
			const FHyperAIStudioManagedConfigLedgerClient& Left,
			const FHyperAIStudioManagedConfigLedgerClient& Right)
		{
			return Left.ClientId.Compare(Right.ClientId, ESearchCase::CaseSensitive) < 0;
		});
		return true;
	}

	FHyperAIStudioManagedEntryReconcileResult ConflictResult(const FString& ReasonCode)
	{
		FHyperAIStudioManagedEntryReconcileResult Result;
		Result.Decision = EHyperAIStudioManagedEntryDecision::Conflict;
		Result.ReasonCode = ReasonCode;
		Result.bBlocksMigration = true;
		return Result;
	}
}

FHyperAIStudioManagedBlockParseResult FHyperAIStudioManagedBlockParser::Parse(
	const FString& Source,
	const FString& BeginMarker,
	const FString& EndMarker)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	FHyperAIStudioManagedBlockParseResult Result;
	if (BeginMarker.IsEmpty()
		|| EndMarker.IsEmpty()
		|| BeginMarker.Len() > MaxMarkerCharacters
		|| EndMarker.Len() > MaxMarkerCharacters
		|| BeginMarker == EndMarker
		|| BeginMarker.Contains(EndMarker, ESearchCase::CaseSensitive)
		|| EndMarker.Contains(BeginMarker, ESearchCase::CaseSensitive))
	{
		Result.State = EHyperAIStudioManagedBlockState::InvalidMarkers;
		Result.ReasonCode = TEXT("managed_block_invalid_markers");
		return Result;
	}
	if (Source.Len() > MaxTextCharacters)
	{
		Result.State = EHyperAIStudioManagedBlockState::InputTooLarge;
		Result.ReasonCode = TEXT("managed_block_input_too_large");
		return Result;
	}

	TArray<int32> BeginOffsets;
	TArray<int32> EndOffsets;
	FindOccurrences(Source, BeginMarker, BeginOffsets);
	FindOccurrences(Source, EndMarker, EndOffsets);

	if (BeginOffsets.IsEmpty() && EndOffsets.IsEmpty())
	{
		Result.State = EHyperAIStudioManagedBlockState::Missing;
		Result.Prefix = Source;
		Result.ReasonCode = TEXT("managed_block_missing");
		return Result;
	}
	if (!BeginOffsets.IsEmpty() && EndOffsets.IsEmpty())
	{
		Result.State = BeginOffsets.Num() == 1
			? EHyperAIStudioManagedBlockState::BeginWithoutEnd
			: EHyperAIStudioManagedBlockState::DuplicateMarkers;
		Result.ReasonCode = BeginOffsets.Num() == 1
			? TEXT("managed_block_begin_without_end")
			: TEXT("managed_block_duplicate_markers");
		return Result;
	}
	if (BeginOffsets.IsEmpty() && !EndOffsets.IsEmpty())
	{
		Result.State = EndOffsets.Num() == 1
			? EHyperAIStudioManagedBlockState::EndWithoutBegin
			: EHyperAIStudioManagedBlockState::DuplicateMarkers;
		Result.ReasonCode = EndOffsets.Num() == 1
			? TEXT("managed_block_end_without_begin")
			: TEXT("managed_block_duplicate_markers");
		return Result;
	}
	if (BeginOffsets.Num() == 1 && EndOffsets.Num() == 1)
	{
		const int32 BeginOffset = BeginOffsets[0];
		const int32 EndOffset = EndOffsets[0];
		if (EndOffset < BeginOffset + BeginMarker.Len())
		{
			Result.State = EHyperAIStudioManagedBlockState::Reversed;
			Result.ReasonCode = TEXT("managed_block_reversed");
			return Result;
		}

		Result.State = EHyperAIStudioManagedBlockState::ValidSinglePair;
		Result.BeginOffset = BeginOffset;
		Result.EndOffsetExclusive = EndOffset + EndMarker.Len();
		Result.Prefix = Source.Left(BeginOffset);
		Result.InclusiveBlock = Source.Mid(BeginOffset, Result.EndOffsetExclusive - BeginOffset);
		Result.Suffix = Source.Mid(Result.EndOffsetExclusive);
		Result.ReasonCode = TEXT("managed_block_valid_single_pair");
		return Result;
	}

	TArray<FMarkerEvent> Events;
	for (const int32 Offset : BeginOffsets)
	{
		Events.Add({ Offset, true });
	}
	for (const int32 Offset : EndOffsets)
	{
		Events.Add({ Offset, false });
	}
	Events.Sort([](const FMarkerEvent& Left, const FMarkerEvent& Right)
	{
		return Left.Offset < Right.Offset;
	});

	int32 Depth = 0;
	int32 CompletePairs = 0;
	bool bNested = false;
	bool bUnmatchedEnd = false;
	for (const FMarkerEvent& Event : Events)
	{
		if (Event.bBegin)
		{
			bNested |= Depth > 0;
			++Depth;
		}
		else if (Depth == 0)
		{
			bUnmatchedEnd = true;
		}
		else
		{
			--Depth;
			CompletePairs += Depth == 0 ? 1 : 0;
		}
	}

	if (bNested)
	{
		Result.State = EHyperAIStudioManagedBlockState::Nested;
		Result.ReasonCode = TEXT("managed_block_nested");
	}
	else if (!bUnmatchedEnd && Depth == 0 && CompletePairs > 1)
	{
		Result.State = EHyperAIStudioManagedBlockState::DuplicatePairs;
		Result.ReasonCode = TEXT("managed_block_duplicate_pairs");
	}
	else
	{
		Result.State = EHyperAIStudioManagedBlockState::DuplicateMarkers;
		Result.ReasonCode = TEXT("managed_block_duplicate_markers");
	}
	return Result;
}

FHyperAIStudioCanonicalJsonResult FHyperAIStudioCanonicalJson::HashObject(const FString& JsonText)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	TSharedPtr<FJsonValue> Root;
	FString ReasonCode;
	if (!ParseStrictJson(JsonText, MaxInputUtf8Bytes, Root, ReasonCode))
	{
		FHyperAIStudioCanonicalJsonResult Result;
		Result.ReasonCode = MoveTemp(ReasonCode);
		return Result;
	}
	return CanonicalizeParsedObject(Root);
}

bool FHyperAIStudioCanonicalJson::IsValidObjectHash(const FString& ObjectHash)
{
	if (ObjectHash.Len() != 71 || !ObjectHash.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 7; Index < ObjectHash.Len(); ++Index)
	{
		const TCHAR Character = ObjectHash[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

const FHyperAIStudioManagedConfigLedgerEntry* FHyperAIStudioManagedConfigOwnershipLedger::FindEntry(
	const FString& ClientId,
	const FString& RelativePath,
	const FString& Container,
	const FString& EntryId) const
{
	for (const FHyperAIStudioManagedConfigLedgerClient& Client : Clients)
	{
		if (Client.ClientId == ClientId
			&& Client.RelativePath == RelativePath
			&& Client.Container == Container)
		{
			for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : Client.Entries)
			{
				if (Entry.Id == EntryId)
				{
					return &Entry;
				}
			}
			return nullptr;
		}
	}
	return nullptr;
}

bool FHyperAIStudioManagedConfigLedgerCodec::Parse(
	const FString& LedgerJson,
	FHyperAIStudioManagedConfigOwnershipLedger& OutLedger,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	OutLedger = {};
	OutReasonCode.Reset();
	TSharedPtr<FJsonValue> RootValue;
	if (!ParseStrictJson(LedgerJson, MaxLedgerUtf8Bytes, RootValue, OutReasonCode))
	{
		return false;
	}
	if (!RootValue.IsValid() || RootValue->Type != EJson::Object)
	{
		OutReasonCode = TEXT("ledger_root_not_object");
		return false;
	}

	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
	if (!HasExactFields(Root, { TEXT("schemaVersion"), TEXT("generator"), TEXT("clients") }))
	{
		OutReasonCode = TEXT("ledger_invalid_root_fields");
		return false;
	}

	FHyperAIStudioManagedConfigOwnershipLedger Parsed;
	if (!ReadIntField(Root, TEXT("schemaVersion"), Parsed.SchemaVersion)
		|| !ReadStringField(Root, TEXT("generator"), Parsed.Generator))
	{
		OutReasonCode = TEXT("ledger_invalid_root_types");
		return false;
	}
	const TSharedPtr<FJsonObject> ClientsObject = ReadObjectField(Root, TEXT("clients"));
	if (!ClientsObject.IsValid())
	{
		OutReasonCode = TEXT("ledger_clients_not_object");
		return false;
	}
	if (ClientsObject->Values.Num() > MaxClients)
	{
		OutReasonCode = TEXT("ledger_client_limit");
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& ClientPair : ClientsObject->Values)
	{
		if (!ClientPair.Value.IsValid() || ClientPair.Value->Type != EJson::Object)
		{
			OutReasonCode = TEXT("ledger_client_not_object");
			return false;
		}
		const TSharedPtr<FJsonObject> ClientObject = ClientPair.Value->AsObject();
		if (!HasExactFields(ClientObject, { TEXT("path"), TEXT("container"), TEXT("entries") }))
		{
			OutReasonCode = TEXT("ledger_invalid_client_fields");
			return false;
		}

		FHyperAIStudioManagedConfigLedgerClient Client;
		Client.ClientId = ClientPair.Key;
		if (!ReadStringField(ClientObject, TEXT("path"), Client.RelativePath)
			|| !ReadStringField(ClientObject, TEXT("container"), Client.Container))
		{
			OutReasonCode = TEXT("ledger_invalid_client_types");
			return false;
		}
		const TSharedPtr<FJsonObject> EntriesObject = ReadObjectField(ClientObject, TEXT("entries"));
		if (!EntriesObject.IsValid())
		{
			OutReasonCode = TEXT("ledger_entries_not_object");
			return false;
		}
		if (EntriesObject->Values.Num() > MaxEntriesPerClient)
		{
			OutReasonCode = TEXT("ledger_entry_limit");
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& EntryPair : EntriesObject->Values)
		{
			if (!EntryPair.Value.IsValid() || EntryPair.Value->Type != EJson::Object)
			{
				OutReasonCode = TEXT("ledger_entry_not_object");
				return false;
			}
			const TSharedPtr<FJsonObject> EntryObject = EntryPair.Value->AsObject();
			if (!HasExactFields(EntryObject, { TEXT("objectHash"), TEXT("writerVersion") }))
			{
				OutReasonCode = TEXT("ledger_invalid_entry_fields");
				return false;
			}

			FHyperAIStudioManagedConfigLedgerEntry Entry;
			Entry.Id = EntryPair.Key;
			if (!ReadStringField(EntryObject, TEXT("objectHash"), Entry.ObjectHash)
				|| !ReadIntField(EntryObject, TEXT("writerVersion"), Entry.WriterVersion))
			{
				OutReasonCode = TEXT("ledger_invalid_entry_types");
				return false;
			}
			Client.Entries.Add(MoveTemp(Entry));
		}
		Parsed.Clients.Add(MoveTemp(Client));
	}

	if (!ValidateAndNormalizeLedger(Parsed, OutReasonCode))
	{
		return false;
	}
	OutLedger = MoveTemp(Parsed);
	OutReasonCode = TEXT("ledger_parsed");
	return true;
}

bool FHyperAIStudioManagedConfigLedgerCodec::Serialize(
	const FHyperAIStudioManagedConfigOwnershipLedger& Ledger,
	FString& OutLedgerJson,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	OutLedgerJson.Reset();
	OutReasonCode.Reset();
	FHyperAIStudioManagedConfigOwnershipLedger Validated = Ledger;
	if (!ValidateAndNormalizeLedger(Validated, OutReasonCode))
	{
		return false;
	}

	const TSharedRef<FCondensedWriter> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutLedgerJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schemaVersion"), Validated.SchemaVersion);
	Writer->WriteValue(TEXT("generator"), Validated.Generator);
	Writer->WriteObjectStart(TEXT("clients"));
	for (const FHyperAIStudioManagedConfigLedgerClient& Client : Validated.Clients)
	{
		Writer->WriteObjectStart(Client.ClientId);
		Writer->WriteValue(TEXT("path"), Client.RelativePath);
		Writer->WriteValue(TEXT("container"), Client.Container);
		Writer->WriteObjectStart(TEXT("entries"));
		for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : Client.Entries)
		{
			Writer->WriteObjectStart(Entry.Id);
			Writer->WriteValue(TEXT("objectHash"), Entry.ObjectHash);
			Writer->WriteValue(TEXT("writerVersion"), Entry.WriterVersion);
			Writer->WriteObjectEnd();
		}
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		OutLedgerJson.Reset();
		OutReasonCode = TEXT("ledger_serialize_failed");
		return false;
	}

	const FTCHARToUTF8 Utf8(*OutLedgerJson);
	if (Utf8.Length() > MaxLedgerUtf8Bytes)
	{
		OutLedgerJson.Reset();
		OutReasonCode = TEXT("ledger_output_too_large");
		return false;
	}
	OutReasonCode = TEXT("ledger_serialized");
	return true;
}

namespace HyperAIStudio::ManagedConfigMigration::Private
{
	bool IsLowerHexTransactionId(const FString& Value)
	{
		if (Value.Len() != FHyperAIStudioManagedConfigPendingCodec::MaxTransactionIdCharacters)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool ValidateExistenceHash(
		const bool bExists,
		const FString& Hash,
		const TCHAR* InvalidReason,
		FString& OutReasonCode)
	{
		if (bExists != FHyperAIStudioCanonicalJson::IsValidObjectHash(Hash))
		{
			OutReasonCode = InvalidReason;
			return false;
		}
		return true;
	}

	bool ValidateAndNormalizePending(
		FHyperAIStudioManagedConfigPendingTransaction& Evidence,
		FString& OutReasonCode)
	{
		if (Evidence.SchemaVersion != FHyperAIStudioManagedConfigPendingCodec::CurrentSchemaVersion
			|| Evidence.Generator != TEXT("HyperAIStudio"))
		{
			OutReasonCode = TEXT("pending_unsupported_identity");
			return false;
		}
		if (Evidence.Phase == TEXT("idle"))
		{
			if (!Evidence.TransactionId.IsEmpty() || !Evidence.Targets.IsEmpty())
			{
				OutReasonCode = TEXT("pending_invalid_idle_state");
				return false;
			}
			return true;
		}
		if (Evidence.Phase != TEXT("targets_prepared")
			|| !IsLowerHexTransactionId(Evidence.TransactionId)
			|| Evidence.Targets.IsEmpty()
			|| Evidence.Targets.Num() > FHyperAIStudioManagedConfigPendingCodec::MaxTargets)
		{
			OutReasonCode = TEXT("pending_invalid_prepared_state");
			return false;
		}

		TSet<FString> Coordinates;
		for (FHyperAIStudioManagedConfigPendingTarget& Target : Evidence.Targets)
		{
			if (!IsSupportedClientCoordinate(Target.ClientId, Target.RelativePath, Target.Container))
			{
				OutReasonCode = TEXT("pending_unsupported_coordinate");
				return false;
			}
			const FString Coordinate = Target.ClientId + TEXT("\n") + Target.RelativePath + TEXT("\n") + Target.Container;
			if (Coordinates.Contains(Coordinate))
			{
				OutReasonCode = TEXT("pending_duplicate_coordinate");
				return false;
			}
			Coordinates.Add(Coordinate);
			if (!ValidateExistenceHash(
				Target.bOriginalExists,
				Target.OriginalFileHash,
				TEXT("pending_invalid_original_hash"),
				OutReasonCode)
				|| !ValidateExistenceHash(
					Target.bDesiredExists,
					Target.DesiredFileHash,
					TEXT("pending_invalid_desired_hash"),
					OutReasonCode))
			{
				return false;
			}
			if (!Target.bDesiredExists)
			{
				OutReasonCode = TEXT("pending_desired_target_must_exist");
				return false;
			}
			if (Target.bOriginalExists == Target.bDesiredExists
				&& Target.OriginalFileHash == Target.DesiredFileHash)
			{
				OutReasonCode = TEXT("pending_identical_states");
				return false;
			}
			if (Target.DesiredLedgerEntries.Num() > FHyperAIStudioManagedConfigLedgerCodec::MaxEntriesPerClient)
			{
				OutReasonCode = TEXT("pending_entry_limit");
				return false;
			}
			TSet<FString> EntryIds;
			for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : Target.DesiredLedgerEntries)
			{
				if (!IsSafeEntryId(Entry.Id)
					|| EntryIds.Contains(Entry.Id)
					|| !FHyperAIStudioCanonicalJson::IsValidObjectHash(Entry.ObjectHash)
					|| Entry.WriterVersion != FHyperAIStudioManagedConfigLedgerCodec::CurrentWriterVersion)
				{
					OutReasonCode = TEXT("pending_invalid_ledger_entry");
					return false;
				}
				EntryIds.Add(Entry.Id);
			}
			Target.DesiredLedgerEntries.Sort([](
				const FHyperAIStudioManagedConfigLedgerEntry& Left,
				const FHyperAIStudioManagedConfigLedgerEntry& Right)
			{
				return Left.Id.Compare(Right.Id, ESearchCase::CaseSensitive) < 0;
			});
		}
		Evidence.Targets.Sort([](
			const FHyperAIStudioManagedConfigPendingTarget& Left,
			const FHyperAIStudioManagedConfigPendingTarget& Right)
		{
			return Left.ClientId.Compare(Right.ClientId, ESearchCase::CaseSensitive) < 0;
		});
		return true;
	}

	bool ValidateSettingsEvidence(
		const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
		FString& OutReasonCode)
	{
		if (Evidence.SchemaVersion != FHyperAIStudioManagedConfigSettingsCodec::CurrentSchemaVersion
			|| Evidence.Generator != TEXT("HyperAIStudio"))
		{
			OutReasonCode = TEXT("settings_evidence_unsupported_identity");
			return false;
		}
		if (Evidence.Phase == TEXT("idle"))
		{
			if (!Evidence.TransactionId.IsEmpty()
				|| Evidence.bOriginalExists
				|| !Evidence.OriginalFileHash.IsEmpty()
				|| !Evidence.StagedFileHash.IsEmpty())
			{
				OutReasonCode = TEXT("settings_evidence_invalid_idle_state");
				return false;
			}
			return true;
		}
		if (Evidence.Phase != TEXT("settings_replace_prepared")
			|| !IsLowerHexTransactionId(Evidence.TransactionId)
			|| !ValidateExistenceHash(
				Evidence.bOriginalExists,
				Evidence.OriginalFileHash,
				TEXT("settings_evidence_invalid_original_hash"),
				OutReasonCode)
			|| !FHyperAIStudioCanonicalJson::IsValidObjectHash(Evidence.StagedFileHash))
		{
			if (OutReasonCode.IsEmpty())
			{
				OutReasonCode = TEXT("settings_evidence_invalid_prepared_state");
			}
			return false;
		}
		if (Evidence.bOriginalExists && Evidence.OriginalFileHash == Evidence.StagedFileHash)
		{
			OutReasonCode = TEXT("settings_evidence_identical_states");
			return false;
		}
		return true;
	}
}

const FHyperAIStudioManagedConfigPendingTarget* FHyperAIStudioManagedConfigPendingTransaction::FindTarget(
	const FString& ClientId,
	const FString& RelativePath,
	const FString& Container) const
{
	return Targets.FindByPredicate([&](const FHyperAIStudioManagedConfigPendingTarget& Target)
	{
		return Target.ClientId == ClientId
			&& Target.RelativePath == RelativePath
			&& Target.Container == Container;
	});
}

FString FHyperAIStudioManagedConfigPendingCodec::HashBytes(const TArray<uint8>& Bytes)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	return TEXT("sha256:") + Sha256Hex(Bytes.GetData(), Bytes.Num());
}

EHyperAIStudioTransactionEvidenceMatch FHyperAIStudioManagedConfigPendingCodec::Classify(
	const FHyperAIStudioManagedConfigPendingTarget& Target,
	const bool bCurrentExists,
	const FString& CurrentFileHash)
{
	if (bCurrentExists == Target.bDesiredExists
		&& CurrentFileHash == Target.DesiredFileHash)
	{
		return EHyperAIStudioTransactionEvidenceMatch::ExactDesired;
	}
	if (bCurrentExists == Target.bOriginalExists
		&& CurrentFileHash == Target.OriginalFileHash)
	{
		return EHyperAIStudioTransactionEvidenceMatch::ExactOriginal;
	}
	return EHyperAIStudioTransactionEvidenceMatch::Conflict;
}

bool FHyperAIStudioManagedConfigPendingCodec::Parse(
	const FString& Json,
	FHyperAIStudioManagedConfigPendingTransaction& OutEvidence,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	OutEvidence = {};
	TSharedPtr<FJsonValue> RootValue;
	if (!ParseStrictJson(Json, MaxEvidenceUtf8Bytes, RootValue, OutReasonCode)
		|| !RootValue.IsValid()
		|| RootValue->Type != EJson::Object)
	{
		if (OutReasonCode.IsEmpty()) OutReasonCode = TEXT("pending_root_not_object");
		return false;
	}
	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
	if (!HasExactFields(Root, { TEXT("schemaVersion"), TEXT("generator"), TEXT("transactionId"), TEXT("phase"), TEXT("targets") })
		|| !ReadIntField(Root, TEXT("schemaVersion"), OutEvidence.SchemaVersion)
		|| !ReadStringField(Root, TEXT("generator"), OutEvidence.Generator)
		|| !ReadStringField(Root, TEXT("transactionId"), OutEvidence.TransactionId)
		|| !ReadStringField(Root, TEXT("phase"), OutEvidence.Phase))
	{
		OutReasonCode = TEXT("pending_invalid_root_fields");
		return false;
	}
	const TSharedPtr<FJsonObject> Targets = ReadObjectField(Root, TEXT("targets"));
	if (!Targets.IsValid() || Targets->Values.Num() > MaxTargets)
	{
		OutReasonCode = TEXT("pending_targets_not_bounded_object");
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Targets->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
		{
			OutReasonCode = TEXT("pending_target_not_object");
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
		if (!HasExactFields(Object, { TEXT("path"), TEXT("container"), TEXT("originalExists"), TEXT("originalHash"), TEXT("desiredExists"), TEXT("desiredHash"), TEXT("entries") }))
		{
			OutReasonCode = TEXT("pending_invalid_target_fields");
			return false;
		}
		FHyperAIStudioManagedConfigPendingTarget Target;
		Target.ClientId = Pair.Key;
		if (!ReadStringField(Object, TEXT("path"), Target.RelativePath)
			|| !ReadStringField(Object, TEXT("container"), Target.Container)
			|| !Object->TryGetBoolField(TEXT("originalExists"), Target.bOriginalExists)
			|| !ReadStringField(Object, TEXT("originalHash"), Target.OriginalFileHash)
			|| !Object->TryGetBoolField(TEXT("desiredExists"), Target.bDesiredExists)
			|| !ReadStringField(Object, TEXT("desiredHash"), Target.DesiredFileHash))
		{
			OutReasonCode = TEXT("pending_invalid_target_types");
			return false;
		}
		const TSharedPtr<FJsonObject> Entries = ReadObjectField(Object, TEXT("entries"));
		if (!Entries.IsValid()
			|| Entries->Values.Num() > FHyperAIStudioManagedConfigLedgerCodec::MaxEntriesPerClient)
		{
			OutReasonCode = TEXT("pending_entries_not_bounded_object");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& EntryPair : Entries->Values)
		{
			if (!EntryPair.Value.IsValid() || EntryPair.Value->Type != EJson::Object)
			{
				OutReasonCode = TEXT("pending_entry_not_object");
				return false;
			}
			const TSharedPtr<FJsonObject> EntryObject = EntryPair.Value->AsObject();
			FHyperAIStudioManagedConfigLedgerEntry Entry;
			Entry.Id = EntryPair.Key;
			if (!HasExactFields(EntryObject, { TEXT("objectHash"), TEXT("writerVersion") })
				|| !ReadStringField(EntryObject, TEXT("objectHash"), Entry.ObjectHash)
				|| !ReadIntField(EntryObject, TEXT("writerVersion"), Entry.WriterVersion))
			{
				OutReasonCode = TEXT("pending_invalid_entry_fields");
				return false;
			}
			Target.DesiredLedgerEntries.Add(MoveTemp(Entry));
		}
		OutEvidence.Targets.Add(MoveTemp(Target));
	}
	if (!ValidateAndNormalizePending(OutEvidence, OutReasonCode))
	{
		return false;
	}
	OutReasonCode = TEXT("pending_parsed");
	return true;
}

bool FHyperAIStudioManagedConfigPendingCodec::Serialize(
	const FHyperAIStudioManagedConfigPendingTransaction& Evidence,
	FString& OutJson,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	FHyperAIStudioManagedConfigPendingTransaction Validated = Evidence;
	OutJson.Reset();
	if (!ValidateAndNormalizePending(Validated, OutReasonCode))
	{
		return false;
	}
	const TSharedRef<FCondensedWriter> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schemaVersion"), Validated.SchemaVersion);
	Writer->WriteValue(TEXT("generator"), Validated.Generator);
	Writer->WriteValue(TEXT("transactionId"), Validated.TransactionId);
	Writer->WriteValue(TEXT("phase"), Validated.Phase);
	Writer->WriteObjectStart(TEXT("targets"));
	for (const FHyperAIStudioManagedConfigPendingTarget& Target : Validated.Targets)
	{
		Writer->WriteObjectStart(Target.ClientId);
		Writer->WriteValue(TEXT("path"), Target.RelativePath);
		Writer->WriteValue(TEXT("container"), Target.Container);
		Writer->WriteValue(TEXT("originalExists"), Target.bOriginalExists);
		Writer->WriteValue(TEXT("originalHash"), Target.OriginalFileHash);
		Writer->WriteValue(TEXT("desiredExists"), Target.bDesiredExists);
		Writer->WriteValue(TEXT("desiredHash"), Target.DesiredFileHash);
		Writer->WriteObjectStart(TEXT("entries"));
		for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : Target.DesiredLedgerEntries)
		{
			Writer->WriteObjectStart(Entry.Id);
			Writer->WriteValue(TEXT("objectHash"), Entry.ObjectHash);
			Writer->WriteValue(TEXT("writerVersion"), Entry.WriterVersion);
			Writer->WriteObjectEnd();
		}
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close() || FTCHARToUTF8(*OutJson).Length() > MaxEvidenceUtf8Bytes)
	{
		OutJson.Reset();
		OutReasonCode = TEXT("pending_serialize_failed_or_too_large");
		return false;
	}
	OutReasonCode = TEXT("pending_serialized");
	return true;
}

EHyperAIStudioTransactionEvidenceMatch FHyperAIStudioManagedConfigSettingsCodec::Classify(
	const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
	const bool bCurrentExists,
	const FString& CurrentFileHash)
{
	if (bCurrentExists && CurrentFileHash == Evidence.StagedFileHash)
	{
		return EHyperAIStudioTransactionEvidenceMatch::ExactDesired;
	}
	if (bCurrentExists == Evidence.bOriginalExists
		&& CurrentFileHash == Evidence.OriginalFileHash)
	{
		return EHyperAIStudioTransactionEvidenceMatch::ExactOriginal;
	}
	return EHyperAIStudioTransactionEvidenceMatch::Conflict;
}

bool FHyperAIStudioManagedConfigSettingsCodec::Parse(
	const FString& Json,
	FHyperAIStudioManagedConfigSettingsTransaction& OutEvidence,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	OutEvidence = {};
	TSharedPtr<FJsonValue> RootValue;
	if (!ParseStrictJson(Json, MaxEvidenceUtf8Bytes, RootValue, OutReasonCode)
		|| !RootValue.IsValid()
		|| RootValue->Type != EJson::Object)
	{
		if (OutReasonCode.IsEmpty()) OutReasonCode = TEXT("settings_evidence_root_not_object");
		return false;
	}
	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
	if (!HasExactFields(Root, { TEXT("schemaVersion"), TEXT("generator"), TEXT("transactionId"), TEXT("phase"), TEXT("originalExists"), TEXT("originalHash"), TEXT("stagedHash") })
		|| !ReadIntField(Root, TEXT("schemaVersion"), OutEvidence.SchemaVersion)
		|| !ReadStringField(Root, TEXT("generator"), OutEvidence.Generator)
		|| !ReadStringField(Root, TEXT("transactionId"), OutEvidence.TransactionId)
		|| !ReadStringField(Root, TEXT("phase"), OutEvidence.Phase)
		|| !Root->TryGetBoolField(TEXT("originalExists"), OutEvidence.bOriginalExists)
		|| !ReadStringField(Root, TEXT("originalHash"), OutEvidence.OriginalFileHash)
		|| !ReadStringField(Root, TEXT("stagedHash"), OutEvidence.StagedFileHash))
	{
		OutReasonCode = TEXT("settings_evidence_invalid_fields");
		return false;
	}
	if (!ValidateSettingsEvidence(OutEvidence, OutReasonCode))
	{
		return false;
	}
	OutReasonCode = TEXT("settings_evidence_parsed");
	return true;
}

bool FHyperAIStudioManagedConfigSettingsCodec::Serialize(
	const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
	FString& OutJson,
	FString& OutReasonCode)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	OutJson.Reset();
	if (!ValidateSettingsEvidence(Evidence, OutReasonCode))
	{
		return false;
	}
	const TSharedRef<FCondensedWriter> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schemaVersion"), Evidence.SchemaVersion);
	Writer->WriteValue(TEXT("generator"), Evidence.Generator);
	Writer->WriteValue(TEXT("transactionId"), Evidence.TransactionId);
	Writer->WriteValue(TEXT("phase"), Evidence.Phase);
	Writer->WriteValue(TEXT("originalExists"), Evidence.bOriginalExists);
	Writer->WriteValue(TEXT("originalHash"), Evidence.OriginalFileHash);
	Writer->WriteValue(TEXT("stagedHash"), Evidence.StagedFileHash);
	Writer->WriteObjectEnd();
	if (!Writer->Close() || FTCHARToUTF8(*OutJson).Length() > MaxEvidenceUtf8Bytes)
	{
		OutJson.Reset();
		OutReasonCode = TEXT("settings_evidence_serialize_failed_or_too_large");
		return false;
	}
	OutReasonCode = TEXT("settings_evidence_serialized");
	return true;
}

FHyperAIStudioJsonEntryInspection FHyperAIStudioJsonEntryInspector::InspectExistingFile(
	const FString& JsonText,
	const FString& Container,
	const FString& EntryId)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	FHyperAIStudioJsonEntryInspection Result;
	if (Container.IsEmpty()
		|| Container.Len() > FHyperAIStudioManagedConfigLedgerCodec::MaxContainerCharacters
		|| EntryId.IsEmpty()
		|| EntryId.Len() > FHyperAIStudioManagedConfigLedgerCodec::MaxEntryIdCharacters
		|| HasControlCharacter(Container)
		|| HasControlCharacter(EntryId))
	{
		Result.State = EHyperAIStudioJsonEntryState::InvalidJson;
		Result.ReasonCode = TEXT("json_entry_invalid_coordinate");
		return Result;
	}
	if (JsonText.TrimStartAndEnd().IsEmpty())
	{
		Result.State = EHyperAIStudioJsonEntryState::EmptyFile;
		Result.ReasonCode = TEXT("json_entry_empty_file");
		return Result;
	}

	TSharedPtr<FJsonValue> RootValue;
	if (!ParseStrictJson(
		JsonText,
		FHyperAIStudioCanonicalJson::MaxInputUtf8Bytes,
		RootValue,
		Result.ReasonCode))
	{
		Result.State = EHyperAIStudioJsonEntryState::InvalidJson;
		return Result;
	}
	if (!RootValue.IsValid() || RootValue->Type != EJson::Object)
	{
		Result.State = EHyperAIStudioJsonEntryState::RootNotObject;
		Result.ReasonCode = TEXT("json_entry_root_not_object");
		return Result;
	}

	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
	const TSharedPtr<FJsonValue> ContainerValue = Root->TryGetField(Container);
	if (!ContainerValue.IsValid())
	{
		Result.State = EHyperAIStudioJsonEntryState::ContainerMissing;
		Result.ReasonCode = TEXT("json_entry_container_missing");
		return Result;
	}
	if (ContainerValue->Type != EJson::Object)
	{
		Result.State = EHyperAIStudioJsonEntryState::ContainerNotObject;
		Result.ReasonCode = TEXT("json_entry_container_not_object");
		return Result;
	}

	const TSharedPtr<FJsonObject> ContainerObject = ContainerValue->AsObject();
	const TSharedPtr<FJsonValue> EntryValue = ContainerObject->TryGetField(EntryId);
	if (!EntryValue.IsValid())
	{
		Result.State = EHyperAIStudioJsonEntryState::EntryMissing;
		Result.ReasonCode = TEXT("json_entry_missing");
		return Result;
	}
	if (EntryValue->Type != EJson::Object)
	{
		Result.State = EHyperAIStudioJsonEntryState::EntryNotObject;
		Result.ReasonCode = TEXT("json_entry_not_object");
		return Result;
	}

	const FHyperAIStudioCanonicalJsonResult HashResult = CanonicalizeParsedObject(EntryValue);
	if (!HashResult.bSuccess)
	{
		Result.State = EHyperAIStudioJsonEntryState::InvalidJson;
		Result.ReasonCode = HashResult.ReasonCode;
		return Result;
	}
	Result.State = EHyperAIStudioJsonEntryState::EntryObject;
	Result.ExistingObjectHash = HashResult.ObjectHash;
	Result.ReasonCode = TEXT("json_entry_object_hashed");
	return Result;
}

FHyperAIStudioManagedEntryReconcileResult FHyperAIStudioManagedEntryReconciler::Plan(
	const FHyperAIStudioManagedEntryReconcileInput& Input)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	if (Input.bDesiredEntry && !Input.bClientEnabledForGeneration)
	{
		return ConflictResult(TEXT("reconcile_disabled_client_has_desired_entry"));
	}
	if (Input.bDesiredEntry != FHyperAIStudioCanonicalJson::IsValidObjectHash(Input.DesiredObjectHash))
	{
		return ConflictResult(TEXT("reconcile_invalid_desired_hash_state"));
	}
	if (!Input.LedgerObjectHash.IsEmpty()
		&& !FHyperAIStudioCanonicalJson::IsValidObjectHash(Input.LedgerObjectHash))
	{
		return ConflictResult(TEXT("reconcile_invalid_ledger_hash"));
	}
	if (Input.bExactHistoricalSettingsOwnership
		!= FHyperAIStudioCanonicalJson::IsValidObjectHash(Input.HistoricalObjectHash))
	{
		return ConflictResult(TEXT("reconcile_invalid_historical_evidence"));
	}

	const bool bEntryObject = Input.ExistingState == EHyperAIStudioJsonEntryState::EntryObject;
	if (bEntryObject != FHyperAIStudioCanonicalJson::IsValidObjectHash(Input.ExistingObjectHash))
	{
		return ConflictResult(TEXT("reconcile_invalid_existing_hash_state"));
	}

	const bool bHasLedger = !Input.LedgerObjectHash.IsEmpty();
	FHyperAIStudioManagedEntryReconcileResult Result;
	Result.bDropLedgerRecordAfterSuccess = bHasLedger;

	if (Input.ExistingState == EHyperAIStudioJsonEntryState::InvalidJson
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::RootNotObject
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::ContainerNotObject
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::EntryNotObject)
	{
		return ConflictResult(TEXT("reconcile_unsafe_json_structure"));
	}

	if (Input.ExistingState == EHyperAIStudioJsonEntryState::MissingFile
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::EmptyFile
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::ContainerMissing
		|| Input.ExistingState == EHyperAIStudioJsonEntryState::EntryMissing)
	{
		if (!Input.bDesiredEntry)
		{
			Result.Decision = EHyperAIStudioManagedEntryDecision::NoOp;
			Result.ReasonCode = Input.ExistingState == EHyperAIStudioJsonEntryState::MissingFile
				? TEXT("reconcile_missing_file_no_cleanup_creation")
				: TEXT("reconcile_entry_absent");
			return Result;
		}

		Result.Decision = EHyperAIStudioManagedEntryDecision::Write;
		Result.ReasonCode = TEXT("reconcile_write_missing_desired_entry");
		Result.bMayRecordLedgerAfterSuccessfulWrite = true;
		Result.bDropLedgerRecordAfterSuccess = false;
		return Result;
	}

	if (!bEntryObject)
	{
		return ConflictResult(TEXT("reconcile_unknown_entry_state"));
	}

	const bool bLedgerExact = bHasLedger && Input.LedgerObjectHash == Input.ExistingObjectHash;
	const bool bHistoricalExact = Input.bExactHistoricalSettingsOwnership
		&& Input.HistoricalObjectHash == Input.ExistingObjectHash;
	if (bLedgerExact && bHistoricalExact)
	{
		Result.Ownership = EHyperAIStudioManagedEntryOwnership::LedgerAndHistoricalExact;
	}
	else if (bLedgerExact)
	{
		Result.Ownership = EHyperAIStudioManagedEntryOwnership::LedgerExact;
	}
	else if (bHistoricalExact)
	{
		Result.Ownership = EHyperAIStudioManagedEntryOwnership::HistoricalExact;
	}
	const bool bOwnedExact = bLedgerExact || bHistoricalExact;
	const bool bEvidenceMismatch = (bHasLedger && !bLedgerExact)
		|| (Input.bExactHistoricalSettingsOwnership && !bHistoricalExact);

	if (Input.bDesiredEntry)
	{
		if (Input.ExistingObjectHash == Input.DesiredObjectHash)
		{
			Result.Decision = EHyperAIStudioManagedEntryDecision::NoOp;
			Result.ReasonCode = TEXT("reconcile_desired_entry_already_exact");
			Result.bDropLedgerRecordAfterSuccess = bHasLedger && !bLedgerExact;
			return Result;
		}

		if (bOwnedExact || Input.bAllowReplaceUnownedDesired)
		{
			Result.Decision = EHyperAIStudioManagedEntryDecision::Write;
			Result.ReasonCode = bOwnedExact
				? TEXT("reconcile_replace_owned_desired_entry")
				: TEXT("reconcile_replace_authorized_unowned_desired_entry");
			Result.bMayRecordLedgerAfterSuccessfulWrite = true;
			Result.bDropLedgerRecordAfterSuccess = false;
			return Result;
		}

		return ConflictResult(bEvidenceMismatch
			? TEXT("reconcile_modified_owned_desired_conflict")
			: TEXT("reconcile_unowned_desired_id_conflict"));
	}

	if (bOwnedExact)
	{
		Result.Decision = EHyperAIStudioManagedEntryDecision::Remove;
		Result.ReasonCode = bLedgerExact
			? TEXT("reconcile_remove_ledger_owned_entry")
			: TEXT("reconcile_remove_exact_historical_entry");
		return Result;
	}
	if (bEvidenceMismatch)
	{
		return ConflictResult(TEXT("reconcile_modified_owned_cleanup_conflict"));
	}

	Result.Decision = EHyperAIStudioManagedEntryDecision::Preserve;
	Result.ReasonCode = TEXT("reconcile_preserve_unowned_entry");
	Result.bDropLedgerRecordAfterSuccess = false;
	return Result;
}

namespace HyperAIStudio::ManagedConfigMigration::Private
{
	enum class ETomlStringMode : uint8
	{
		None,
		Basic,
		Literal,
		MultilineBasic,
		MultilineLiteral
	};

	bool IsTomlBareKeyCharacter(const TCHAR Character)
	{
		return (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_')
			|| Character == TEXT('-');
	}

	bool IsTomlInlineWhitespace(const TCHAR Character)
	{
		return Character == TEXT(' ') || Character == TEXT('\t');
	}

	bool IsForbiddenTomlControl(const TCHAR Character)
	{
		return (Character < 0x20
			&& Character != TEXT('\t')
			&& Character != TEXT('\r')
			&& Character != TEXT('\n'))
			|| Character == 0x7f;
	}

	bool IsHexCharacter(const TCHAR Character)
	{
		return (Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))
			|| (Character >= TEXT('A') && Character <= TEXT('F'));
	}

	bool AppendValidatedTomlEscape(
		const FString& Source,
		int32& InOutIndex,
		FString& OutText,
		const bool bMultiline,
		FString& OutReasonCode)
	{
		if (InOutIndex + 1 >= Source.Len())
		{
			OutReasonCode = TEXT("codex_toml_trailing_escape");
			return false;
		}

		const TCHAR Escaped = Source[InOutIndex + 1];
		if (bMultiline && (Escaped == TEXT('\n') || Escaped == TEXT('\r')))
		{
			int32 Cursor = InOutIndex + 1;
			if (Source[Cursor] == TEXT('\r'))
			{
				if (Cursor + 1 >= Source.Len() || Source[Cursor + 1] != TEXT('\n'))
				{
					OutReasonCode = TEXT("codex_toml_bare_carriage_return");
					return false;
				}
				++Cursor;
			}
			while (Cursor + 1 < Source.Len()
				&& (Source[Cursor + 1] == TEXT(' ')
					|| Source[Cursor + 1] == TEXT('\t')
					|| Source[Cursor + 1] == TEXT('\n')
					|| Source[Cursor + 1] == TEXT('\r')))
			{
				++Cursor;
			}
			OutText.AppendChar(TEXT(' '));
			InOutIndex = Cursor;
			return true;
		}

		const bool bSimpleEscape = Escaped == TEXT('"')
			|| Escaped == TEXT('\\')
			|| Escaped == TEXT('b')
			|| Escaped == TEXT('t')
			|| Escaped == TEXT('n')
			|| Escaped == TEXT('f')
			|| Escaped == TEXT('r');
		if (bSimpleEscape)
		{
			OutText.AppendChar(TEXT('\\'));
			OutText.AppendChar(Escaped);
			++InOutIndex;
			return true;
		}

		const int32 HexCharacters = Escaped == TEXT('u') ? 4 : (Escaped == TEXT('U') ? 8 : 0);
		if (HexCharacters == 0 || InOutIndex + 1 + HexCharacters >= Source.Len())
		{
			OutReasonCode = TEXT("codex_toml_invalid_escape");
			return false;
		}
		OutText.AppendChar(TEXT('\\'));
		OutText.AppendChar(Escaped);
		for (int32 Offset = 1; Offset <= HexCharacters; ++Offset)
		{
			const TCHAR Hex = Source[InOutIndex + 1 + Offset];
			if (!IsHexCharacter(Hex))
			{
				OutReasonCode = TEXT("codex_toml_invalid_unicode_escape");
				return false;
			}
			OutText.AppendChar(Hex);
		}
		InOutIndex += 1 + HexCharacters;
		return true;
	}

	bool AddTomlStatement(
		FString& InOutCurrent,
		TArray<FString>& OutStatements,
		FString& OutReasonCode)
	{
		FString Statement = InOutCurrent.TrimStartAndEnd();
		InOutCurrent.Reset();
		if (Statement.IsEmpty())
		{
			return true;
		}
		if (Statement.Len() > FHyperAIStudioCodexTomlCollisionInspector::MaxStatementCharacters)
		{
			OutReasonCode = TEXT("codex_toml_statement_too_large");
			return false;
		}
		if (OutStatements.Num() >= FHyperAIStudioCodexTomlCollisionInspector::MaxStatements)
		{
			OutReasonCode = TEXT("codex_toml_statement_limit");
			return false;
		}
		OutStatements.Add(MoveTemp(Statement));
		return true;
	}

	bool TokenizeTomlRegion(
		const FString& Source,
		TArray<FString>& OutStatements,
		FString& OutReasonCode)
	{
		OutStatements.Reset();
		FString Current;
		Current.Reserve(FMath::Min(Source.Len(), 4096));
		ETomlStringMode StringMode = ETomlStringMode::None;
		int32 SquareDepth = 0;
		int32 BraceDepth = 0;

		for (int32 Index = 0; Index < Source.Len(); ++Index)
		{
			const TCHAR Character = Source[Index];
			if (IsForbiddenTomlControl(Character))
			{
				OutReasonCode = TEXT("codex_toml_forbidden_control_character");
				return false;
			}

			if (StringMode == ETomlStringMode::Basic)
			{
				if (Character == TEXT('\r') || Character == TEXT('\n'))
				{
					OutReasonCode = TEXT("codex_toml_newline_in_basic_string");
					return false;
				}
				if (Character == TEXT('\\'))
				{
					if (!AppendValidatedTomlEscape(Source, Index, Current, false, OutReasonCode))
					{
						return false;
					}
					continue;
				}
				Current.AppendChar(Character);
				if (Character == TEXT('"'))
				{
					StringMode = ETomlStringMode::None;
				}
				continue;
			}

			if (StringMode == ETomlStringMode::Literal)
			{
				if (Character == TEXT('\r') || Character == TEXT('\n'))
				{
					OutReasonCode = TEXT("codex_toml_newline_in_literal_string");
					return false;
				}
				Current.AppendChar(Character);
				if (Character == TEXT('\''))
				{
					StringMode = ETomlStringMode::None;
				}
				continue;
			}

			if (StringMode == ETomlStringMode::MultilineBasic)
			{
				if (Character == TEXT('"')
					&& Index + 2 < Source.Len()
					&& Source[Index + 1] == TEXT('"')
					&& Source[Index + 2] == TEXT('"'))
				{
					Current += TEXT("\"\"\"");
					Index += 2;
					StringMode = ETomlStringMode::None;
					continue;
				}
				if (Character == TEXT('\\'))
				{
					if (!AppendValidatedTomlEscape(Source, Index, Current, true, OutReasonCode))
					{
						return false;
					}
					continue;
				}
				if (Character == TEXT('\r'))
				{
					if (Index + 1 >= Source.Len() || Source[Index + 1] != TEXT('\n'))
					{
						OutReasonCode = TEXT("codex_toml_bare_carriage_return");
						return false;
					}
					++Index;
					Current.AppendChar(TEXT('\n'));
					continue;
				}
				Current.AppendChar(Character);
				continue;
			}

			if (StringMode == ETomlStringMode::MultilineLiteral)
			{
				if (Character == TEXT('\'')
					&& Index + 2 < Source.Len()
					&& Source[Index + 1] == TEXT('\'')
					&& Source[Index + 2] == TEXT('\''))
				{
					Current += TEXT("'''");
					Index += 2;
					StringMode = ETomlStringMode::None;
					continue;
				}
				if (Character == TEXT('\r'))
				{
					if (Index + 1 >= Source.Len() || Source[Index + 1] != TEXT('\n'))
					{
						OutReasonCode = TEXT("codex_toml_bare_carriage_return");
						return false;
					}
					++Index;
					Current.AppendChar(TEXT('\n'));
					continue;
				}
				Current.AppendChar(Character);
				continue;
			}

			if (Character == TEXT('#'))
			{
				while (Index + 1 < Source.Len()
					&& Source[Index + 1] != TEXT('\r')
					&& Source[Index + 1] != TEXT('\n'))
				{
					++Index;
				}
				continue;
			}
			if (Character == TEXT('"'))
			{
				if (Index + 2 < Source.Len()
					&& Source[Index + 1] == TEXT('"')
					&& Source[Index + 2] == TEXT('"'))
				{
					Current += TEXT("\"\"\"");
					Index += 2;
					StringMode = ETomlStringMode::MultilineBasic;
				}
				else
				{
					Current.AppendChar(Character);
					StringMode = ETomlStringMode::Basic;
				}
				continue;
			}
			if (Character == TEXT('\''))
			{
				if (Index + 2 < Source.Len()
					&& Source[Index + 1] == TEXT('\'')
					&& Source[Index + 2] == TEXT('\''))
				{
					Current += TEXT("'''");
					Index += 2;
					StringMode = ETomlStringMode::MultilineLiteral;
				}
				else
				{
					Current.AppendChar(Character);
					StringMode = ETomlStringMode::Literal;
				}
				continue;
			}

			if (Character == TEXT('['))
			{
				++SquareDepth;
			}
			else if (Character == TEXT(']'))
			{
				if (SquareDepth == 0)
				{
					OutReasonCode = TEXT("codex_toml_mismatched_square_bracket");
					return false;
				}
				--SquareDepth;
			}
			else if (Character == TEXT('{'))
			{
				++BraceDepth;
			}
			else if (Character == TEXT('}'))
			{
				if (BraceDepth == 0)
				{
					OutReasonCode = TEXT("codex_toml_mismatched_brace");
					return false;
				}
				--BraceDepth;
			}
			if (SquareDepth > FHyperAIStudioCodexTomlCollisionInspector::MaxNestingDepth
				|| BraceDepth > FHyperAIStudioCodexTomlCollisionInspector::MaxNestingDepth)
			{
				OutReasonCode = TEXT("codex_toml_nesting_limit");
				return false;
			}

			if (Character == TEXT('\r') || Character == TEXT('\n'))
			{
				if (Character == TEXT('\r'))
				{
					if (Index + 1 >= Source.Len() || Source[Index + 1] != TEXT('\n'))
					{
						OutReasonCode = TEXT("codex_toml_bare_carriage_return");
						return false;
					}
					++Index;
				}
				if (BraceDepth > 0)
				{
					OutReasonCode = TEXT("codex_toml_multiline_inline_table");
					return false;
				}
				if (SquareDepth > 0)
				{
					if (Current.TrimStart().StartsWith(TEXT("[")))
					{
						OutReasonCode = TEXT("codex_toml_multiline_table_header");
						return false;
					}
					Current.AppendChar(TEXT(' '));
				}
				else if (!AddTomlStatement(Current, OutStatements, OutReasonCode))
				{
					return false;
				}
				continue;
			}

			Current.AppendChar(Character);
			if (Current.Len() > FHyperAIStudioCodexTomlCollisionInspector::MaxStatementCharacters)
			{
				OutReasonCode = TEXT("codex_toml_statement_too_large");
				return false;
			}
		}

		if (StringMode != ETomlStringMode::None)
		{
			OutReasonCode = TEXT("codex_toml_unclosed_string");
			return false;
		}
		if (SquareDepth != 0 || BraceDepth != 0)
		{
			OutReasonCode = TEXT("codex_toml_unclosed_container");
			return false;
		}
		return AddTomlStatement(Current, OutStatements, OutReasonCode);
	}

	bool ParseTomlKeyPath(
		const FString& Text,
		TArray<FString>& OutSegments,
		FString& OutReasonCode)
	{
		OutSegments.Reset();
		int32 Cursor = 0;
		while (Cursor < Text.Len())
		{
			while (Cursor < Text.Len() && IsTomlInlineWhitespace(Text[Cursor]))
			{
				++Cursor;
			}
			if (Cursor >= Text.Len())
			{
				OutReasonCode = TEXT("codex_toml_empty_key_segment");
				return false;
			}

			FString Segment;
			const TCHAR First = Text[Cursor];
			if (First == TEXT('"') || First == TEXT('\''))
			{
				const TCHAR Quote = First;
				++Cursor;
				bool bClosed = false;
				while (Cursor < Text.Len())
				{
					const TCHAR Character = Text[Cursor++];
					if (Character == Quote)
					{
						bClosed = true;
						break;
					}
					if (IsForbiddenTomlControl(Character)
						|| Character == TEXT('\r')
						|| Character == TEXT('\n'))
					{
						OutReasonCode = TEXT("codex_toml_invalid_quoted_key_character");
						return false;
					}
					if (Quote == TEXT('"') && Character == TEXT('\\'))
					{
						if (Cursor >= Text.Len())
						{
							OutReasonCode = TEXT("codex_toml_trailing_key_escape");
							return false;
						}
						const TCHAR Escaped = Text[Cursor++];
						if (Escaped != TEXT('"') && Escaped != TEXT('\\'))
						{
							OutReasonCode = TEXT("codex_toml_ambiguous_key_escape");
							return false;
						}
						Segment.AppendChar(Escaped);
					}
					else
					{
						Segment.AppendChar(Character);
					}
				}
				if (!bClosed)
				{
					OutReasonCode = TEXT("codex_toml_unclosed_quoted_key");
					return false;
				}
			}
			else
			{
				const int32 Start = Cursor;
				while (Cursor < Text.Len() && IsTomlBareKeyCharacter(Text[Cursor]))
				{
					++Cursor;
				}
				if (Cursor == Start)
				{
					OutReasonCode = TEXT("codex_toml_invalid_bare_key");
					return false;
				}
				Segment = Text.Mid(Start, Cursor - Start);
			}

			if (Segment.Len() > FHyperAIStudioCodexTomlCollisionInspector::MaxKeySegmentCharacters)
			{
				OutReasonCode = TEXT("codex_toml_key_segment_too_large");
				return false;
			}
			OutSegments.Add(MoveTemp(Segment));
			if (OutSegments.Num() > FHyperAIStudioCodexTomlCollisionInspector::MaxKeySegments)
			{
				OutReasonCode = TEXT("codex_toml_key_segment_limit");
				return false;
			}

			while (Cursor < Text.Len() && IsTomlInlineWhitespace(Text[Cursor]))
			{
				++Cursor;
			}
			if (Cursor >= Text.Len())
			{
				break;
			}
			if (Text[Cursor] != TEXT('.'))
			{
				OutReasonCode = TEXT("codex_toml_invalid_key_separator");
				return false;
			}
			++Cursor;
		}
		if (OutSegments.IsEmpty())
		{
			OutReasonCode = TEXT("codex_toml_empty_key_path");
			return false;
		}
		return true;
	}

	bool FindTomlAssignmentEquals(
		const FString& Statement,
		int32& OutEqualsOffset,
		FString& OutReasonCode)
	{
		OutEqualsOffset = INDEX_NONE;
		ETomlStringMode Mode = ETomlStringMode::None;
		int32 SquareDepth = 0;
		int32 BraceDepth = 0;
		for (int32 Index = 0; Index < Statement.Len(); ++Index)
		{
			const TCHAR Character = Statement[Index];
			if (Mode == ETomlStringMode::Basic)
			{
				if (Character == TEXT('\\'))
				{
					++Index;
				}
				else if (Character == TEXT('"'))
				{
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Mode == ETomlStringMode::Literal)
			{
				if (Character == TEXT('\''))
				{
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Mode == ETomlStringMode::MultilineBasic)
			{
				if (Character == TEXT('\\'))
				{
					++Index;
				}
				else if (Character == TEXT('"')
					&& Index + 2 < Statement.Len()
					&& Statement[Index + 1] == TEXT('"')
					&& Statement[Index + 2] == TEXT('"'))
				{
					Index += 2;
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Mode == ETomlStringMode::MultilineLiteral)
			{
				if (Character == TEXT('\'')
					&& Index + 2 < Statement.Len()
					&& Statement[Index + 1] == TEXT('\'')
					&& Statement[Index + 2] == TEXT('\''))
				{
					Index += 2;
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Character == TEXT('"'))
			{
				if (Index + 2 < Statement.Len()
					&& Statement[Index + 1] == TEXT('"')
					&& Statement[Index + 2] == TEXT('"'))
				{
					Index += 2;
					Mode = ETomlStringMode::MultilineBasic;
				}
				else
				{
					Mode = ETomlStringMode::Basic;
				}
				continue;
			}
			if (Character == TEXT('\''))
			{
				if (Index + 2 < Statement.Len()
					&& Statement[Index + 1] == TEXT('\'')
					&& Statement[Index + 2] == TEXT('\''))
				{
					Index += 2;
					Mode = ETomlStringMode::MultilineLiteral;
				}
				else
				{
					Mode = ETomlStringMode::Literal;
				}
				continue;
			}
			if (Character == TEXT('['))
			{
				++SquareDepth;
			}
			else if (Character == TEXT(']'))
			{
				--SquareDepth;
			}
			else if (Character == TEXT('{'))
			{
				++BraceDepth;
			}
			else if (Character == TEXT('}'))
			{
				--BraceDepth;
			}
			else if (Character == TEXT('=') && SquareDepth == 0 && BraceDepth == 0)
			{
				if (OutEqualsOffset != INDEX_NONE)
				{
					OutReasonCode = TEXT("codex_toml_multiple_assignment_operators");
					return false;
				}
				OutEqualsOffset = Index;
			}
		}
		if (OutEqualsOffset == INDEX_NONE)
		{
			OutReasonCode = TEXT("codex_toml_missing_assignment_operator");
			return false;
		}
		return true;
	}

	FString EncodeTomlPath(const TArray<FString>& Path)
	{
		FString Encoded;
		for (const FString& Segment : Path)
		{
			Encoded += FString::FromInt(Segment.Len());
			Encoded.AppendChar(TEXT(':'));
			Encoded += Segment;
			Encoded.AppendChar(TCHAR(0x1f));
		}
		return Encoded;
	}

	bool ParseTomlTableHeader(
		const FString& Statement,
		TArray<FString>& OutPath,
		bool& bOutArrayTable,
		FString& OutReasonCode)
	{
		const FString Trimmed = Statement.TrimStartAndEnd();
		bOutArrayTable = Trimmed.StartsWith(TEXT("[["));
		if (bOutArrayTable)
		{
			if (Trimmed.Len() < 5 || !Trimmed.EndsWith(TEXT("]]")))
			{
				OutReasonCode = TEXT("codex_toml_invalid_array_table_header");
				return false;
			}
			return ParseTomlKeyPath(Trimmed.Mid(2, Trimmed.Len() - 4), OutPath, OutReasonCode);
		}
		if (Trimmed.Len() < 3 || !Trimmed.StartsWith(TEXT("[")) || !Trimmed.EndsWith(TEXT("]")))
		{
			OutReasonCode = TEXT("codex_toml_invalid_table_header");
			return false;
		}
		return ParseTomlKeyPath(Trimmed.Mid(1, Trimmed.Len() - 2), OutPath, OutReasonCode);
	}

	bool IsMcpServersPath(const TArray<FString>& Path)
	{
		return !Path.IsEmpty() && Path[0] == TEXT("mcp_servers");
	}

	bool SplitTomlInlineEntries(
		const FString& Interior,
		TArray<FString>& OutEntries,
		FString& OutReasonCode)
	{
		OutEntries.Reset();
		ETomlStringMode Mode = ETomlStringMode::None;
		int32 SquareDepth = 0;
		int32 BraceDepth = 0;
		int32 EntryStart = 0;
		for (int32 Index = 0; Index < Interior.Len(); ++Index)
		{
			const TCHAR Character = Interior[Index];
			if (Mode == ETomlStringMode::Basic)
			{
				if (Character == TEXT('\\'))
				{
					++Index;
				}
				else if (Character == TEXT('"'))
				{
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Mode == ETomlStringMode::Literal)
			{
				if (Character == TEXT('\''))
				{
					Mode = ETomlStringMode::None;
				}
				continue;
			}
			if (Character == TEXT('"'))
			{
				Mode = ETomlStringMode::Basic;
				continue;
			}
			if (Character == TEXT('\''))
			{
				Mode = ETomlStringMode::Literal;
				continue;
			}
			if (Character == TEXT('['))
			{
				++SquareDepth;
			}
			else if (Character == TEXT(']'))
			{
				--SquareDepth;
			}
			else if (Character == TEXT('{'))
			{
				++BraceDepth;
			}
			else if (Character == TEXT('}'))
			{
				--BraceDepth;
			}
			else if (Character == TEXT(',') && SquareDepth == 0 && BraceDepth == 0)
			{
				const FString Entry = Interior.Mid(EntryStart, Index - EntryStart).TrimStartAndEnd();
				if (Entry.IsEmpty())
				{
					OutReasonCode = TEXT("codex_toml_empty_inline_entry");
					return false;
				}
				OutEntries.Add(Entry);
				if (OutEntries.Num() > FHyperAIStudioCodexTomlCollisionInspector::MaxStatements)
				{
					OutReasonCode = TEXT("codex_toml_inline_entry_limit");
					return false;
				}
				EntryStart = Index + 1;
			}
		}

		const FString Tail = Interior.Mid(EntryStart).TrimStartAndEnd();
		if (Tail.IsEmpty())
		{
			if (!OutEntries.IsEmpty())
			{
				OutReasonCode = TEXT("codex_toml_inline_trailing_comma");
				return false;
			}
			return true;
		}
		OutEntries.Add(Tail);
		if (OutEntries.Num() > FHyperAIStudioCodexTomlCollisionInspector::MaxStatements)
		{
			OutReasonCode = TEXT("codex_toml_inline_entry_limit");
			return false;
		}
		return true;
	}

	bool IsTomlPathPrefix(const TArray<FString>& Left, const TArray<FString>& Right)
	{
		if (Left.Num() > Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (Left[Index] != Right[Index])
			{
				return false;
			}
		}
		return true;
	}

	struct FCodexCaseSensitiveStringSetFuncs : BaseKeyFuncs<FString, FString, false>
	{
		static const FString& GetSetKey(const FString& Element)
		{
			return Element;
		}

		static bool Matches(const FString& Left, const FString& Right)
		{
			return Left.Equals(Right, ESearchCase::CaseSensitive);
		}

		static uint32 GetKeyHash(const FString& Key)
		{
			return FCrc::StrCrc32(*Key);
		}
	};

	using FCodexCaseSensitiveStringSet = TSet<FString, FCodexCaseSensitiveStringSetFuncs>;

	struct FCodexTomlScanState
	{
		explicit FCodexTomlScanState(const FCodexCaseSensitiveStringSet& InDesiredIds)
			: DesiredIds(InDesiredIds)
		{
		}

		const FCodexCaseSensitiveStringSet& DesiredIds;
		FCodexCaseSensitiveStringSet CollidingIds;
		TArray<FString> CurrentTable;
		bool bCurrentArrayTable = false;
		bool bSawInlineMcpServers = false;
		bool bSawOtherMcpShape = false;
		FCodexCaseSensitiveStringSet ExplicitTablePaths;
		FCodexCaseSensitiveStringSet RelevantAssignmentPaths;
		int32 StatementCount = 0;

		void RecordCollision(const TArray<FString>& Path)
		{
			if (Path.Num() >= 2 && DesiredIds.Contains(Path[1]))
			{
				CollidingIds.Add(Path[1]);
			}
		}

		bool InspectInlineMcpServers(
			const FString& Value,
			FString& OutReasonCode)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.Len() < 2
				|| !Trimmed.StartsWith(TEXT("{"))
				|| !Trimmed.EndsWith(TEXT("}")))
			{
				OutReasonCode = TEXT("codex_toml_mcp_servers_not_table");
				return false;
			}

			TArray<FString> Entries;
			if (!SplitTomlInlineEntries(
				Trimmed.Mid(1, Trimmed.Len() - 2),
				Entries,
				OutReasonCode))
			{
				return false;
			}
			TArray<TArray<FString>> SeenPaths;
			for (const FString& Entry : Entries)
			{
				int32 EqualsOffset = INDEX_NONE;
				if (!FindTomlAssignmentEquals(Entry, EqualsOffset, OutReasonCode))
				{
					return false;
				}
				TArray<FString> KeyPath;
				if (!ParseTomlKeyPath(Entry.Left(EqualsOffset), KeyPath, OutReasonCode)
					|| Entry.Mid(EqualsOffset + 1).TrimStartAndEnd().IsEmpty())
				{
					if (OutReasonCode.IsEmpty())
					{
						OutReasonCode = TEXT("codex_toml_empty_inline_value");
					}
					return false;
				}
				for (const TArray<FString>& Seen : SeenPaths)
				{
					if (IsTomlPathPrefix(Seen, KeyPath) || IsTomlPathPrefix(KeyPath, Seen))
					{
						OutReasonCode = TEXT("codex_toml_duplicate_inline_key");
						return false;
					}
				}
				SeenPaths.Add(KeyPath);
				if (DesiredIds.Contains(KeyPath[0]))
				{
					CollidingIds.Add(KeyPath[0]);
				}
			}
			return true;
		}

		bool ProcessTableHeader(const FString& Statement, FString& OutReasonCode)
		{
			TArray<FString> Path;
			bool bArrayTable = false;
			if (!ParseTomlTableHeader(Statement, Path, bArrayTable, OutReasonCode))
			{
				return false;
			}
			CurrentTable = Path;
			bCurrentArrayTable = bArrayTable;
			if (!bArrayTable)
			{
				const FString Encoded = EncodeTomlPath(Path);
				if (ExplicitTablePaths.Contains(Encoded))
				{
					OutReasonCode = TEXT("codex_toml_duplicate_table_header");
					return false;
				}
				ExplicitTablePaths.Add(Encoded);
			}

			if (IsMcpServersPath(Path))
			{
				if (bArrayTable)
				{
					OutReasonCode = TEXT("codex_toml_mcp_servers_array_table");
					return false;
				}
				if (bSawInlineMcpServers)
				{
					OutReasonCode = TEXT("codex_toml_multiple_mcp_server_shapes");
					return false;
				}
				bSawOtherMcpShape = true;
				RecordCollision(Path);
			}
			return true;
		}

		bool ProcessAssignment(const FString& Statement, FString& OutReasonCode)
		{
			int32 EqualsOffset = INDEX_NONE;
			if (!FindTomlAssignmentEquals(Statement, EqualsOffset, OutReasonCode))
			{
				return false;
			}
			TArray<FString> RelativePath;
			if (!ParseTomlKeyPath(Statement.Left(EqualsOffset), RelativePath, OutReasonCode))
			{
				return false;
			}
			const FString Value = Statement.Mid(EqualsOffset + 1).TrimStartAndEnd();
			if (Value.IsEmpty())
			{
				OutReasonCode = TEXT("codex_toml_empty_assignment_value");
				return false;
			}

			TArray<FString> AbsolutePath = CurrentTable;
			AbsolutePath.Append(RelativePath);
			if (!IsMcpServersPath(AbsolutePath))
			{
				return true;
			}

			if (AbsolutePath.Num() == 1)
			{
				if (!CurrentTable.IsEmpty() || bSawInlineMcpServers || bSawOtherMcpShape)
				{
					OutReasonCode = TEXT("codex_toml_multiple_mcp_server_shapes");
					return false;
				}
				if (!InspectInlineMcpServers(Value, OutReasonCode))
				{
					return false;
				}
				bSawInlineMcpServers = true;
				return true;
			}

			if (bSawInlineMcpServers)
			{
				OutReasonCode = TEXT("codex_toml_multiple_mcp_server_shapes");
				return false;
			}
			bSawOtherMcpShape = true;
			const FString Encoded = EncodeTomlPath(AbsolutePath);
			if (!bCurrentArrayTable && RelevantAssignmentPaths.Contains(Encoded))
			{
				OutReasonCode = TEXT("codex_toml_duplicate_mcp_assignment");
				return false;
			}
			RelevantAssignmentPaths.Add(Encoded);
			RecordCollision(AbsolutePath);
			return true;
		}

		bool ScanRegion(
			const FString& Region,
			const bool bRequireLeadingTableHeader,
			FString& OutReasonCode)
		{
			TArray<FString> Statements;
			if (!TokenizeTomlRegion(Region, Statements, OutReasonCode))
			{
				return false;
			}
			if (StatementCount > FHyperAIStudioCodexTomlCollisionInspector::MaxStatements - Statements.Num())
			{
				OutReasonCode = TEXT("codex_toml_statement_limit");
				return false;
			}
			StatementCount += Statements.Num();
			if (bRequireLeadingTableHeader && !Statements.IsEmpty()
				&& !Statements[0].TrimStart().StartsWith(TEXT("[")))
			{
				OutReasonCode = TEXT("codex_toml_suffix_context_ambiguous");
				return false;
			}

			CurrentTable.Reset();
			bCurrentArrayTable = false;
			for (const FString& Statement : Statements)
			{
				if (Statement.TrimStart().StartsWith(TEXT("[")))
				{
					if (!ProcessTableHeader(Statement, OutReasonCode))
					{
						return false;
					}
				}
				else if (!ProcessAssignment(Statement, OutReasonCode))
				{
					return false;
				}
			}
			return true;
		}
	};
}

FHyperAIStudioCodexTomlCollisionInspection FHyperAIStudioCodexTomlCollisionInspector::Inspect(
	const FString& Source,
	const FString& BeginMarker,
	const FString& EndMarker,
	const TArray<FString>& DesiredServerIds)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Private;
	FHyperAIStudioCodexTomlCollisionInspection Result;
	if (Source.Len() > MaxTomlCharacters)
	{
		Result.ReasonCode = TEXT("codex_toml_input_too_large");
		return Result;
	}
	if (DesiredServerIds.Num() > MaxDesiredServerIds)
	{
		Result.ReasonCode = TEXT("codex_toml_desired_id_limit");
		return Result;
	}

	FCodexCaseSensitiveStringSet DesiredIds;
	for (const FString& ServerId : DesiredServerIds)
	{
		if (ServerId.IsEmpty()
			|| ServerId.Len() > MaxServerIdCharacters
			|| HasControlCharacter(ServerId)
			|| DesiredIds.Contains(ServerId))
		{
			Result.ReasonCode = TEXT("codex_toml_invalid_or_duplicate_desired_id");
			return Result;
		}
		DesiredIds.Add(ServerId);
	}

	const FHyperAIStudioManagedBlockParseResult ManagedBlock =
		FHyperAIStudioManagedBlockParser::Parse(Source, BeginMarker, EndMarker);
	if (ManagedBlock.State != EHyperAIStudioManagedBlockState::Missing
		&& ManagedBlock.State != EHyperAIStudioManagedBlockState::ValidSinglePair)
	{
		Result.ReasonCode = TEXT("codex_toml_invalid_managed_markers");
		return Result;
	}

	FCodexTomlScanState ScanState(DesiredIds);
	if (!ScanState.ScanRegion(ManagedBlock.Prefix, false, Result.ReasonCode))
	{
		return Result;
	}
	if (ManagedBlock.State == EHyperAIStudioManagedBlockState::ValidSinglePair
		&& !ScanState.ScanRegion(ManagedBlock.Suffix, true, Result.ReasonCode))
	{
		return Result;
	}

	for (const FString& Collision : ScanState.CollidingIds)
	{
		Result.CollidingServerIds.Add(Collision);
	}
	Result.CollidingServerIds.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	if (!Result.CollidingServerIds.IsEmpty())
	{
		Result.State = EHyperAIStudioCodexTomlCollisionState::Collision;
		Result.ReasonCode = TEXT("codex_toml_unmanaged_id_collision");
		return Result;
	}
	if (ScanState.bSawInlineMcpServers)
	{
		Result.ReasonCode = TEXT("codex_toml_inline_mcp_servers_not_extendable");
		return Result;
	}

	Result.State = EHyperAIStudioCodexTomlCollisionState::Safe;
	Result.ReasonCode = TEXT("codex_toml_no_unmanaged_collision");
	return Result;
}
