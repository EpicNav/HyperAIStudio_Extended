// Games by Hyper 2026.

#include "HyperAIStudioTypedPlan.h"

#include "HyperAIStudioBlueprintPatch.h"
#include "HyperAIStudioOperationJournal.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::TypedPlan::Private
{
	constexpr int32 MaxTypeIdChars = 96;
	constexpr int32 MaxArgumentNameChars = 64;
	constexpr int32 CompileNativeOperationsPerTarget = 1;
	constexpr int32 CompileOutputBytesPerTarget = 256;
	constexpr int32 ValidateNativeOperationsPerTarget = 1;
	constexpr int32 ValidateOutputBytesPerTarget = 512;
	constexpr int32 SaveNativeOperationsPerTarget = 1;
	constexpr int32 SaveOutputBytesPerTarget = 256;
	constexpr int32 VerifyFreshNativeOperationsPerTarget = 1;
	constexpr int32 VerifyFreshOutputBytesPerTarget = 512;
	constexpr const TCHAR* FreshVerificationValidatorId = TEXT("hyperai.verify_fresh");
	constexpr const TCHAR* FreshVerificationValidatorFingerprint = TEXT("sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	constexpr const TCHAR* RollbackVerificationValidatorId = TEXT("hyperai.verify_rollback");
	constexpr const TCHAR* RollbackVerificationValidatorFingerprint = TEXT("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
	constexpr int64 MaxValidatorReceiptLifetimeMs = 5 * 60 * 1000;

	class FSystemPlanClock final : public IHyperAIStudioPlanClock
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

	FSystemPlanClock& GetSystemPlanClock()
	{
		static FSystemPlanClock Clock;
		return Clock;
	}

	void AppendToken(FString& Buffer, const FString& Value)
	{
		Buffer.Appendf(TEXT("%d:"), Value.Len());
		Buffer += Value;
		Buffer += TEXT("|");
	}

	void AppendInt(FString& Buffer, const int64 Value)
	{
		AppendToken(Buffer, FString::Printf(TEXT("%lld"), Value));
	}

	void AppendBool(FString& Buffer, const bool bValue)
	{
		AppendToken(Buffer, bValue ? TEXT("1") : TEXT("0"));
	}

	FString HashUtf8Sha256(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		static constexpr uint32 Constants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
		uint32 State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
		const int64 ByteCount = Utf8.Length();
		if (ByteCount < 0 || ByteCount > MAX_int32 - 72)
		{
			return FString();
		}
		TArray<uint8> Padded;
		Padded.Append(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<int32>(ByteCount));
		Padded.Add(0x80u);
		while ((Padded.Num() % 64) != 56)
		{
			Padded.Add(0u);
		}
		const uint64 BitCount = static_cast<uint64>(ByteCount) * 8u;
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Padded.Add(static_cast<uint8>((BitCount >> Shift) & 0xffu));
		}

		auto RotateRight = [](const uint32 Input, const uint32 Shift)
		{
			return (Input >> Shift) | (Input << (32u - Shift));
		};
		for (int32 Block = 0; Block < Padded.Num(); Block += 64)
		{
			uint32 Words[64]{};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = Block + Index * 4;
				Words[Index] = (static_cast<uint32>(Padded[Offset]) << 24)
					| (static_cast<uint32>(Padded[Offset + 1]) << 16)
					| (static_cast<uint32>(Padded[Offset + 2]) << 8)
					| static_cast<uint32>(Padded[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 Small0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const uint32 Small1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + Small0 + Words[Index - 7] + Small1;
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
				const uint32 Big1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ ((~E) & G);
				const uint32 Temp1 = H + Big1 + Choice + Constants[Index] + Words[Index];
				const uint32 Big0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = Big0 + Majority;
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

		FString Hex = TEXT("sha256:");
		Hex.Reserve(71);
		for (const uint32 Word : State)
		{
			Hex += FString::Printf(TEXT("%08x"), Word);
		}
		return Hex;
	}

	bool IsLowerIdentifier(const FString& Value, const int32 MaxChars, const bool bAllowDot)
	{
		if (Value.IsEmpty() || Value.Len() > MaxChars
			|| !(Value[0] >= TEXT('a') && Value[0] <= TEXT('z')))
		{
			return false;
		}
		TCHAR Previous = 0;
		for (const TCHAR Character : Value)
		{
			const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_') || Character == TEXT('-')
				|| (bAllowDot && Character == TEXT('.'));
			if (!bAllowed || (Character == TEXT('.') && Previous == TEXT('.')))
			{
				return false;
			}
			Previous = Character;
		}
		return Previous != TEXT('.') && Previous != TEXT('-') && Previous != TEXT('_');
	}

	bool HasWellFormedUtf16(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character >= 0xd800 && Character <= 0xdbff)
			{
				if (Index + 1 >= Value.Len() || Value[Index + 1] < 0xdc00 || Value[Index + 1] > 0xdfff)
				{
					return false;
				}
				++Index;
			}
			else if (Character >= 0xdc00 && Character <= 0xdfff)
			{
				return false;
			}
		}
		return true;
	}

	bool IsBoundedText(const FString& Value, const int32 MaxChars, const bool bAllowEmpty)
	{
		if ((!bAllowEmpty && Value.IsEmpty()) || Value.Len() > MaxChars
			|| Value != Value.TrimStartAndEnd() || !HasWellFormedUtf16(Value))
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character < TEXT(' ') || Character == 0x7f)
			{
				return false;
			}
			if (Character >= 0xd800 && Character <= 0xdbff)
			{
				++Index;
			}
		}
		return true;
	}

	bool IsSha256Token(const FString& Value)
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

	bool HasNoOutcomeEvidence(const FHyperAIStudioPlanOutcomeEvidence& Evidence)
	{
		return Evidence.RollbackState == EHyperAIStudioPlanRollbackState::NotNeeded
			&& !Evidence.ValidatorReceipt.IsPresent();
	}

	bool HasConsistentNonCompleteRollbackEvidence(const FHyperAIStudioPlanOutcomeEvidence& Evidence)
	{
		if (Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Partial
			&& Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Unsupported
			&& Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Unknown)
		{
			return false;
		}
		return !Evidence.ValidatorReceipt.IsPresent();
	}

	bool HasCompleteRollbackEvidence(const FHyperAIStudioPlanOutcomeEvidence& Evidence)
	{
		return Evidence.RollbackState == EHyperAIStudioPlanRollbackState::Complete
			&& Evidence.ValidatorReceipt.IsPresent();
	}

	bool HasFreshPostconditionEvidence(const FHyperAIStudioPlanOutcomeEvidence& Evidence)
	{
		return Evidence.RollbackState == EHyperAIStudioPlanRollbackState::NotNeeded
			&& Evidence.ValidatorReceipt.IsPresent();
	}

	/** Strict shape scan performed before FJsonSerializer can collapse duplicate object keys. */
	class FJsonShapeScanner
	{
	public:
		explicit FJsonShapeScanner(const FString& InJson)
			: Json(InJson)
		{
		}

		bool Scan(FString& OutCode, FString& OutMessage)
		{
			OutCode.Reset();
			OutMessage.Reset();
			SkipWhitespace();
			if (!ScanValue(1, OutCode, OutMessage))
			{
				return false;
			}
			SkipWhitespace();
			if (Index != Json.Len())
			{
				OutCode = TEXT("invalid_json");
				OutMessage = TEXT("Unexpected content follows the root JSON value.");
				return false;
			}
			return true;
		}

	private:
		void SkipWhitespace()
		{
			while (Index < Json.Len())
			{
				const TCHAR Character = Json[Index];
				if (Character != TEXT(' ') && Character != TEXT('\t')
					&& Character != TEXT('\r') && Character != TEXT('\n'))
				{
					break;
				}
				++Index;
			}
		}

		bool CountNode(FString& OutCode, FString& OutMessage)
		{
			++NodeCount;
			if (NodeCount > FHyperAIStudioPlanLimits::MaxJsonNodes)
			{
				OutCode = TEXT("json_node_limit");
				OutMessage = TEXT("The JSON node bound was exceeded before semantic validation.");
				return false;
			}
			return true;
		}

		bool ScanValue(const int32 Depth, FString& OutCode, FString& OutMessage)
		{
			if (Depth > FHyperAIStudioPlanLimits::MaxJsonDepth)
			{
				OutCode = TEXT("json_depth_limit");
				OutMessage = TEXT("The JSON nesting-depth bound was exceeded.");
				return false;
			}
			if (!CountNode(OutCode, OutMessage))
			{
				return false;
			}
			SkipWhitespace();
			if (Index >= Json.Len())
			{
				return SyntaxError(TEXT("Unexpected end of JSON while reading a value."), OutCode, OutMessage);
			}
			switch (Json[Index])
			{
			case TEXT('{'):
				return ScanObject(Depth, OutCode, OutMessage);
			case TEXT('['):
				return ScanArray(Depth, OutCode, OutMessage);
			case TEXT('"'):
			{
				FString Ignored;
				return ScanString(Ignored, OutCode, OutMessage);
			}
			case TEXT('t'):
				return ScanLiteral(TEXT("true"), OutCode, OutMessage);
			case TEXT('f'):
				return ScanLiteral(TEXT("false"), OutCode, OutMessage);
			case TEXT('n'):
				return ScanLiteral(TEXT("null"), OutCode, OutMessage);
			default:
				return ScanNumber(OutCode, OutMessage);
			}
		}

		bool ScanObject(const int32 Depth, FString& OutCode, FString& OutMessage)
		{
			++Index;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return true;
			}
			TSet<FString> Keys;
			while (Index < Json.Len())
			{
				FString Key;
				if (!ScanString(Key, OutCode, OutMessage))
				{
					return false;
				}
				if (!CountNode(OutCode, OutMessage))
				{
					return false;
				}
				if (Keys.Contains(Key))
				{
					OutCode = TEXT("duplicate_json_key");
					OutMessage = FString::Printf(TEXT("Duplicate JSON object key '%s' is prohibited."), *Key.Left(128));
					return false;
				}
				Keys.Add(Key);
				SkipWhitespace();
				if (!Consume(TEXT(':')))
				{
					return SyntaxError(TEXT("A JSON object key must be followed by ':'."), OutCode, OutMessage);
				}
				if (!ScanValue(Depth + 1, OutCode, OutMessage))
				{
					return false;
				}
				SkipWhitespace();
				if (Consume(TEXT('}')))
				{
					return true;
				}
				if (!Consume(TEXT(',')))
				{
					return SyntaxError(TEXT("A JSON object member must be followed by ',' or '}'."), OutCode, OutMessage);
				}
				SkipWhitespace();
			}
			return SyntaxError(TEXT("Unterminated JSON object."), OutCode, OutMessage);
		}

		bool ScanArray(const int32 Depth, FString& OutCode, FString& OutMessage)
		{
			++Index;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return true;
			}
			while (Index < Json.Len())
			{
				if (!ScanValue(Depth + 1, OutCode, OutMessage))
				{
					return false;
				}
				SkipWhitespace();
				if (Consume(TEXT(']')))
				{
					return true;
				}
				if (!Consume(TEXT(',')))
				{
					return SyntaxError(TEXT("A JSON array value must be followed by ',' or ']'."), OutCode, OutMessage);
				}
				SkipWhitespace();
			}
			return SyntaxError(TEXT("Unterminated JSON array."), OutCode, OutMessage);
		}

		bool ScanString(FString& OutDecoded, FString& OutCode, FString& OutMessage)
		{
			OutDecoded.Reset();
			SkipWhitespace();
			if (!Consume(TEXT('"')))
			{
				return SyntaxError(TEXT("A JSON string was required."), OutCode, OutMessage);
			}
			while (Index < Json.Len())
			{
				const TCHAR Character = Json[Index++];
				if (Character == TEXT('"'))
				{
					return true;
				}
				if (Character < TEXT(' '))
				{
					return SyntaxError(TEXT("Unescaped control character in JSON string."), OutCode, OutMessage);
				}
				if (Character != TEXT('\\'))
				{
					if (Character >= 0xd800 && Character <= 0xdbff)
					{
						if (Index >= Json.Len() || Json[Index] < 0xdc00 || Json[Index] > 0xdfff)
						{
							return SyntaxError(TEXT("A raw high surrogate must be followed by a raw low surrogate."), OutCode, OutMessage);
						}
						OutDecoded.AppendChar(Character);
						OutDecoded.AppendChar(Json[Index++]);
						continue;
					}
					if (Character >= 0xdc00 && Character <= 0xdfff)
					{
						return SyntaxError(TEXT("A raw low surrogate cannot appear without a high surrogate."), OutCode, OutMessage);
					}
					OutDecoded.AppendChar(Character);
					continue;
				}
				if (Index >= Json.Len())
				{
					return SyntaxError(TEXT("Unterminated JSON string escape."), OutCode, OutMessage);
				}
				const TCHAR Escape = Json[Index++];
				switch (Escape)
				{
				case TEXT('"'): OutDecoded.AppendChar(TEXT('"')); break;
				case TEXT('\\'): OutDecoded.AppendChar(TEXT('\\')); break;
				case TEXT('/'): OutDecoded.AppendChar(TEXT('/')); break;
				case TEXT('b'): OutDecoded.AppendChar(TEXT('\b')); break;
				case TEXT('f'): OutDecoded.AppendChar(TEXT('\f')); break;
				case TEXT('n'): OutDecoded.AppendChar(TEXT('\n')); break;
				case TEXT('r'): OutDecoded.AppendChar(TEXT('\r')); break;
				case TEXT('t'): OutDecoded.AppendChar(TEXT('\t')); break;
				case TEXT('u'):
				{
					uint16 CodeUnit = 0;
					if (!ScanHexCodeUnit(CodeUnit))
					{
						return SyntaxError(TEXT("Invalid \\u escape in JSON string."), OutCode, OutMessage);
					}
					if (CodeUnit >= 0xd800 && CodeUnit <= 0xdbff)
					{
						if (Index + 6 > Json.Len() || Json[Index] != TEXT('\\') || Json[Index + 1] != TEXT('u'))
						{
							return SyntaxError(TEXT("A high surrogate must be followed by a low-surrogate \\u escape."), OutCode, OutMessage);
						}
						Index += 2;
						uint16 Low = 0;
						if (!ScanHexCodeUnit(Low) || Low < 0xdc00 || Low > 0xdfff)
						{
							return SyntaxError(TEXT("Invalid low surrogate in JSON string."), OutCode, OutMessage);
						}
						OutDecoded.AppendChar(static_cast<TCHAR>(CodeUnit));
						OutDecoded.AppendChar(static_cast<TCHAR>(Low));
					}
					else if (CodeUnit >= 0xdc00 && CodeUnit <= 0xdfff)
					{
						return SyntaxError(TEXT("A low surrogate cannot appear without a high surrogate."), OutCode, OutMessage);
					}
					else
					{
						OutDecoded.AppendChar(static_cast<TCHAR>(CodeUnit));
					}
					break;
				}
				default:
					return SyntaxError(TEXT("Unknown JSON string escape."), OutCode, OutMessage);
				}
			}
			return SyntaxError(TEXT("Unterminated JSON string."), OutCode, OutMessage);
		}

		bool ScanHexCodeUnit(uint16& OutValue)
		{
			if (Index + 4 > Json.Len())
			{
				return false;
			}
			uint16 Value = 0;
			for (int32 Offset = 0; Offset < 4; ++Offset)
			{
				const int32 Digit = HexValue(Json[Index + Offset]);
				if (Digit < 0)
				{
					return false;
				}
				Value = static_cast<uint16>((Value << 4) | Digit);
			}
			Index += 4;
			OutValue = Value;
			return true;
		}

		static int32 HexValue(const TCHAR Character)
		{
			if (Character >= TEXT('0') && Character <= TEXT('9')) { return Character - TEXT('0'); }
			if (Character >= TEXT('a') && Character <= TEXT('f')) { return Character - TEXT('a') + 10; }
			if (Character >= TEXT('A') && Character <= TEXT('F')) { return Character - TEXT('A') + 10; }
			return -1;
		}

		bool ScanLiteral(const TCHAR* Literal, FString& OutCode, FString& OutMessage)
		{
			const int32 Length = FCString::Strlen(Literal);
			if (Index + Length > Json.Len() || Json.Mid(Index, Length) != Literal)
			{
				return SyntaxError(TEXT("Invalid JSON literal."), OutCode, OutMessage);
			}
			Index += Length;
			return true;
		}

		bool ScanNumber(FString& OutCode, FString& OutMessage)
		{
			const int32 Start = Index;
			Consume(TEXT('-'));
			if (Consume(TEXT('0')))
			{
				if (Index < Json.Len() && Json[Index] >= TEXT('0') && Json[Index] <= TEXT('9'))
				{
					return SyntaxError(TEXT("JSON numbers cannot contain leading zeroes."), OutCode, OutMessage);
				}
			}
			else
			{
				if (Index >= Json.Len() || Json[Index] < TEXT('1') || Json[Index] > TEXT('9'))
				{
					return SyntaxError(TEXT("Invalid JSON number."), OutCode, OutMessage);
				}
				while (Index < Json.Len() && Json[Index] >= TEXT('0') && Json[Index] <= TEXT('9')) { ++Index; }
			}
			if (Consume(TEXT('.')))
			{
				const int32 FractionStart = Index;
				while (Index < Json.Len() && Json[Index] >= TEXT('0') && Json[Index] <= TEXT('9')) { ++Index; }
				if (Index == FractionStart)
				{
					return SyntaxError(TEXT("JSON fractions require digits."), OutCode, OutMessage);
				}
			}
			if (Index < Json.Len() && (Json[Index] == TEXT('e') || Json[Index] == TEXT('E')))
			{
				++Index;
				if (Index < Json.Len() && (Json[Index] == TEXT('+') || Json[Index] == TEXT('-'))) { ++Index; }
				const int32 ExponentStart = Index;
				while (Index < Json.Len() && Json[Index] >= TEXT('0') && Json[Index] <= TEXT('9')) { ++Index; }
				if (Index == ExponentStart)
				{
					return SyntaxError(TEXT("JSON exponents require digits."), OutCode, OutMessage);
				}
			}
			return Index > Start;
		}

		bool Consume(const TCHAR Expected)
		{
			if (Index < Json.Len() && Json[Index] == Expected)
			{
				++Index;
				return true;
			}
			return false;
		}

		static bool SyntaxError(const FString& Message, FString& OutCode, FString& OutMessage)
		{
			OutCode = TEXT("invalid_json");
			OutMessage = Message;
			return false;
		}

		const FString& Json;
		int32 Index = 0;
		int32 NodeCount = 0;
	};

	FString ValueTypeToString(const EHyperAIStudioPlanValueType Type)
	{
		switch (Type)
		{
		case EHyperAIStudioPlanValueType::String: return TEXT("string");
		case EHyperAIStudioPlanValueType::Integer: return TEXT("integer");
		case EHyperAIStudioPlanValueType::Boolean: return TEXT("boolean");
		case EHyperAIStudioPlanValueType::StringArray: return TEXT("string_array");
		default: return TEXT("unknown");
		}
	}

	FString PreconditionKindToString(const EHyperAIStudioPlanPreconditionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanPreconditionKind::ObjectExists: return TEXT("object_exists");
		case EHyperAIStudioPlanPreconditionKind::ObjectAbsent: return TEXT("object_absent");
		case EHyperAIStudioPlanPreconditionKind::RevisionEquals: return TEXT("revision_equals");
		case EHyperAIStudioPlanPreconditionKind::PropertyEquals: return TEXT("property_equals");
		case EHyperAIStudioPlanPreconditionKind::PluginAvailable: return TEXT("plugin_available");
		case EHyperAIStudioPlanPreconditionKind::EditorStateEquals: return TEXT("editor_state_equals");
		default: return TEXT("unknown");
		}
	}

	bool TryParsePreconditionKind(const FString& Value, EHyperAIStudioPlanPreconditionKind& OutKind)
	{
		if (Value == TEXT("object_exists")) { OutKind = EHyperAIStudioPlanPreconditionKind::ObjectExists; return true; }
		if (Value == TEXT("object_absent")) { OutKind = EHyperAIStudioPlanPreconditionKind::ObjectAbsent; return true; }
		if (Value == TEXT("revision_equals")) { OutKind = EHyperAIStudioPlanPreconditionKind::RevisionEquals; return true; }
		if (Value == TEXT("property_equals")) { OutKind = EHyperAIStudioPlanPreconditionKind::PropertyEquals; return true; }
		if (Value == TEXT("plugin_available")) { OutKind = EHyperAIStudioPlanPreconditionKind::PluginAvailable; return true; }
		if (Value == TEXT("editor_state_equals")) { OutKind = EHyperAIStudioPlanPreconditionKind::EditorStateEquals; return true; }
		return false;
	}

	FString EffectKindToString(const EHyperAIStudioPlanEffectKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanEffectKind::ObjectCreated: return TEXT("object_created");
		case EHyperAIStudioPlanEffectKind::ObjectUpdated: return TEXT("object_updated");
		case EHyperAIStudioPlanEffectKind::ObjectDeleted: return TEXT("object_deleted");
		case EHyperAIStudioPlanEffectKind::RuntimeExternalEffect: return TEXT("runtime_external_effect");
		default: return TEXT("unknown");
		}
	}

	bool TryParseEffectKind(const FString& Value, EHyperAIStudioPlanEffectKind& OutKind)
	{
		if (Value == TEXT("object_created")) { OutKind = EHyperAIStudioPlanEffectKind::ObjectCreated; return true; }
		if (Value == TEXT("object_updated")) { OutKind = EHyperAIStudioPlanEffectKind::ObjectUpdated; return true; }
		if (Value == TEXT("object_deleted")) { OutKind = EHyperAIStudioPlanEffectKind::ObjectDeleted; return true; }
		if (Value == TEXT("runtime_external_effect")) { OutKind = EHyperAIStudioPlanEffectKind::RuntimeExternalEffect; return true; }
		return false;
	}

	int32 SafetyRank(const EHyperAIStudioPlanSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIStudioPlanSafety::Read: return 0;
		case EHyperAIStudioPlanSafety::Edit: return 1;
		case EHyperAIStudioPlanSafety::Destructive: return 2;
		case EHyperAIStudioPlanSafety::ExternalEffect: return 3;
		default: return 4;
		}
	}

	bool IsValidSafety(const EHyperAIStudioPlanSafety Safety)
	{
		return Safety == EHyperAIStudioPlanSafety::Read
			|| Safety == EHyperAIStudioPlanSafety::Edit
			|| Safety == EHyperAIStudioPlanSafety::Destructive
			|| Safety == EHyperAIStudioPlanSafety::ExternalEffect;
	}

	bool IsValidValueType(const EHyperAIStudioPlanValueType Type)
	{
		return Type == EHyperAIStudioPlanValueType::String
			|| Type == EHyperAIStudioPlanValueType::Integer
			|| Type == EHyperAIStudioPlanValueType::Boolean
			|| Type == EHyperAIStudioPlanValueType::StringArray;
	}

	bool IsValidPreconditionKind(const EHyperAIStudioPlanPreconditionKind Kind)
	{
		return Kind == EHyperAIStudioPlanPreconditionKind::ObjectExists
			|| Kind == EHyperAIStudioPlanPreconditionKind::ObjectAbsent
			|| Kind == EHyperAIStudioPlanPreconditionKind::RevisionEquals
			|| Kind == EHyperAIStudioPlanPreconditionKind::PropertyEquals
			|| Kind == EHyperAIStudioPlanPreconditionKind::PluginAvailable
			|| Kind == EHyperAIStudioPlanPreconditionKind::EditorStateEquals;
	}

	bool IsValidEffectKind(const EHyperAIStudioPlanEffectKind Kind)
	{
		return Kind == EHyperAIStudioPlanEffectKind::ObjectCreated
			|| Kind == EHyperAIStudioPlanEffectKind::ObjectUpdated
			|| Kind == EHyperAIStudioPlanEffectKind::ObjectDeleted
			|| Kind == EHyperAIStudioPlanEffectKind::RuntimeExternalEffect;
	}

	EHyperAIStudioPlanSafety RequiredSafetyForEffect(const EHyperAIStudioPlanEffectKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanEffectKind::ObjectCreated:
		case EHyperAIStudioPlanEffectKind::ObjectUpdated:
			return EHyperAIStudioPlanSafety::Edit;
		case EHyperAIStudioPlanEffectKind::ObjectDeleted:
			return EHyperAIStudioPlanSafety::Destructive;
		case EHyperAIStudioPlanEffectKind::RuntimeExternalEffect:
			return EHyperAIStudioPlanSafety::ExternalEffect;
		default:
			return static_cast<EHyperAIStudioPlanSafety>(255);
		}
	}

	bool IsValidOpaqueAuthorizationToken(const FString& Value)
	{
		if (Value.Len() < 16 || Value.Len() > FHyperAIStudioPlanLimits::MaxAuthorizationTokenChars)
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

	struct FParseContext
	{
		FHyperAIStudioPlanDryRunResult& Result;

		bool Error(const FString& Code, const FString& Path, const FString& Message)
		{
			if (Result.Diagnostics.Num() < FHyperAIStudioPlanLimits::MaxDiagnostics)
			{
				Result.Diagnostics.Add({Code, Path, Message});
			}
			return false;
		}
	};

	bool HasOnlyFields(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<FString>& Allowed,
		const FString& Path,
		FParseContext& Context)
	{
		TArray<FString> Keys;
		Keys.Reserve(Object->Values.Num());
		for (const TPair<FJsonObject::FStringType, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			Keys.Add(FString(Field.Key.ToView()));
		}
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			if (!Allowed.Contains(Key))
			{
				return Context.Error(TEXT("unknown_field"), Path + TEXT(".") + Key, TEXT("Unknown fields are rejected by the closed plan schema."));
			}
		}
		return true;
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const FString& Path,
		FString& OutValue,
		FParseContext& Context)
	{
		if (!Object->TryGetStringField(Field, OutValue))
		{
			return Context.Error(TEXT("required_string"), Path + TEXT(".") + Field, TEXT("A string value is required."));
		}
		return true;
	}

	bool ReadRequiredBool(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const FString& Path,
		bool& OutValue,
		FParseContext& Context)
	{
		if (!Object->TryGetBoolField(Field, OutValue))
		{
			return Context.Error(TEXT("required_boolean"), Path + TEXT(".") + Field, TEXT("A boolean value is required."));
		}
		return true;
	}

	/** StoreNumbersAsStrings preserves the lexeme, but UE's integer TryGetNumber accepts numeric prefixes. */
	bool TryParseExactJsonInteger(const TSharedPtr<FJsonValue>& JsonValue, int64& OutValue)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number)
		{
			return false;
		}
		FString Lexeme;
		if (!JsonValue->TryGetString(Lexeme) || Lexeme.IsEmpty())
		{
			return false;
		}
		int32 Index = 0;
		const bool bNegative = Lexeme[0] == TEXT('-');
		if (bNegative && ++Index >= Lexeme.Len())
		{
			return false;
		}
		if (Lexeme[Index] == TEXT('0') && Index + 1 != Lexeme.Len())
		{
			return false;
		}
		uint64 Magnitude = 0;
		constexpr uint64 ExactLimit = static_cast<uint64>(FHyperAIStudioPlanLimits::MaxExactJsonInteger);
		for (; Index < Lexeme.Len(); ++Index)
		{
			const TCHAR Character = Lexeme[Index];
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
			if (Magnitude > (ExactLimit - Digit) / 10)
			{
				return false;
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		OutValue = bNegative ? -static_cast<int64>(Magnitude) : static_cast<int64>(Magnitude);
		return true;
	}

	bool ReadRequiredInteger(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const FString& Path,
		const int64 Minimum,
		const int64 Maximum,
		int32& OutValue,
		FParseContext& Context)
	{
		int64 Number = 0;
		if (!TryParseExactJsonInteger(Object->TryGetField(Field), Number)
			|| Number < Minimum
			|| Number > Maximum)
		{
			return Context.Error(TEXT("bounded_integer"), Path + TEXT(".") + Field,
				FString::Printf(TEXT("An integer in [%lld, %lld] is required."), Minimum, Maximum));
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool ReadRequiredObject(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const FString& Path,
		TSharedPtr<FJsonObject>& OutObject,
		FParseContext& Context)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (!Object->TryGetObjectField(Field, Found) || !Found || !Found->IsValid())
		{
			return Context.Error(TEXT("required_object"), Path + TEXT(".") + Field, TEXT("An object value is required."));
		}
		OutObject = *Found;
		return true;
	}

	bool ReadRequiredArray(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const FString& Path,
		const TArray<TSharedPtr<FJsonValue>>*& OutArray,
		FParseContext& Context)
	{
		if (!Object->TryGetArrayField(Field, OutArray) || !OutArray)
		{
			return Context.Error(TEXT("required_array"), Path + TEXT(".") + Field, TEXT("An array value is required."));
		}
		return true;
	}

	bool ParseBudget(
		const TSharedPtr<FJsonObject>& Object,
		FHyperAIStudioPlanBudget& OutBudget,
		FParseContext& Context)
	{
		if (!HasOnlyFields(Object,
			{TEXT("deadline_ms"), TEXT("max_game_thread_ms"), TEXT("max_mutations"), TEXT("max_native_operations"), TEXT("max_output_bytes"), TEXT("max_steps")},
			TEXT("$.budget"), Context))
		{
			return false;
		}
		return ReadRequiredInteger(Object, TEXT("deadline_ms"), TEXT("$.budget"), 100, FHyperAIStudioPlanLimits::MaxDeadlineMs, OutBudget.DeadlineMs, Context)
			&& ReadRequiredInteger(Object, TEXT("max_steps"), TEXT("$.budget"), 1, FHyperAIStudioPlanLimits::MaxSteps, OutBudget.MaxSteps, Context)
			&& ReadRequiredInteger(Object, TEXT("max_mutations"), TEXT("$.budget"), 0, FHyperAIStudioPlanLimits::MaxMutationSteps, OutBudget.MaxMutations, Context)
			&& ReadRequiredInteger(Object, TEXT("max_native_operations"), TEXT("$.budget"), 1, FHyperAIStudioPlanLimits::MaxNativeOperations, OutBudget.MaxNativeOperations, Context)
			&& ReadRequiredInteger(Object, TEXT("max_game_thread_ms"), TEXT("$.budget"), 1, FHyperAIStudioPlanLimits::MaxGameThreadMs, OutBudget.MaxGameThreadMs, Context)
			&& ReadRequiredInteger(Object, TEXT("max_output_bytes"), TEXT("$.budget"), 256, FHyperAIStudioPlanLimits::MaxOutputBytes, OutBudget.MaxOutputBytes, Context);
	}

	bool ParseStepBudget(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		FHyperAIStudioPlanStepBudget& OutBudget,
		FParseContext& Context)
	{
		if (!HasOnlyFields(Object,
			{TEXT("max_game_thread_ms"), TEXT("max_native_operations"), TEXT("max_output_bytes")}, Path, Context))
		{
			return false;
		}
		return ReadRequiredInteger(Object, TEXT("max_native_operations"), Path, 1, FHyperAIStudioPlanLimits::MaxNativeOperations, OutBudget.MaxNativeOperations, Context)
			&& ReadRequiredInteger(Object, TEXT("max_game_thread_ms"), Path, 1, FHyperAIStudioPlanLimits::MaxGameThreadMs, OutBudget.MaxGameThreadMs, Context)
			&& ReadRequiredInteger(Object, TEXT("max_output_bytes"), Path, 128, FHyperAIStudioPlanLimits::MaxOutputBytes, OutBudget.MaxOutputBytes, Context);
	}

	bool ParseArgumentValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		const FHyperAIStudioPlanArgumentSpec& Spec,
		const FString& Path,
		FHyperAIStudioPlanValue& OutValue,
		FParseContext& Context)
	{
		OutValue.Type = Spec.Type;
		switch (Spec.Type)
		{
		case EHyperAIStudioPlanValueType::String:
		{
			if (!JsonValue.IsValid() || !JsonValue->TryGetString(OutValue.StringValue)
				|| !IsBoundedText(OutValue.StringValue, Spec.MaxStringChars, false)
				|| (!Spec.AllowedStrings.IsEmpty() && !Spec.AllowedStrings.Contains(OutValue.StringValue)))
			{
				return Context.Error(TEXT("invalid_argument"), Path, TEXT("The string argument violates its declared bounds or enum allowlist."));
			}
			return true;
		}
		case EHyperAIStudioPlanValueType::Integer:
		{
			int64 Number = 0;
			if (!TryParseExactJsonInteger(JsonValue, Number)
				|| Number < Spec.MinInteger
				|| Number > Spec.MaxInteger)
			{
				return Context.Error(TEXT("invalid_argument"), Path, TEXT("The integer argument violates its declared bounds."));
			}
			OutValue.IntegerValue = Number;
			return true;
		}
		case EHyperAIStudioPlanValueType::Boolean:
			if (!JsonValue.IsValid() || !JsonValue->TryGetBool(OutValue.bBooleanValue))
			{
				return Context.Error(TEXT("invalid_argument"), Path, TEXT("A boolean argument is required."));
			}
			return true;
		case EHyperAIStudioPlanValueType::StringArray:
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!JsonValue.IsValid() || !JsonValue->TryGetArray(Values) || !Values
				|| Values->Num() > Spec.MaxArrayItems)
			{
				return Context.Error(TEXT("invalid_argument"), Path, TEXT("The string array argument violates its item bound."));
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				FString Item;
				if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Item)
					|| !IsBoundedText(Item, Spec.MaxStringChars, false)
					|| OutValue.StringArrayValue.Contains(Item))
				{
					return Context.Error(TEXT("invalid_argument"), FString::Printf(TEXT("%s[%d]"), *Path, Index),
						TEXT("String-array items must be bounded, non-empty, and unique."));
				}
				OutValue.StringArrayValue.Add(Item);
			}
			return true;
		}
		default:
			return Context.Error(TEXT("invalid_argument_schema"), Path, TEXT("The operation registry contains an unknown argument type."));
		}
	}

	bool ParsePrecondition(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		FHyperAIStudioPlanPrecondition& OutCondition,
		FParseContext& Context)
	{
		if (!HasOnlyFields(Object, {TEXT("expected"), TEXT("field"), TEXT("kind"), TEXT("target")}, Path, Context))
		{
			return false;
		}
		FString Kind;
		if (!ReadRequiredString(Object, TEXT("kind"), Path, Kind, Context)
			|| !TryParsePreconditionKind(Kind, OutCondition.Kind))
		{
			return Context.Error(TEXT("unknown_precondition"), Path + TEXT(".kind"), TEXT("The precondition kind is not allowlisted."));
		}
		if (!ReadRequiredString(Object, TEXT("target"), Path, OutCondition.Target, Context)
			|| !IsBoundedText(OutCondition.Target, FHyperAIStudioPlanLimits::MaxTargetChars, false))
		{
			return Context.Error(TEXT("invalid_target"), Path + TEXT(".target"), TEXT("A bounded target without control characters is required."));
		}
		if (Object->HasField(TEXT("field"))
			&& (!Object->TryGetStringField(TEXT("field"), OutCondition.Field)
				|| !IsLowerIdentifier(OutCondition.Field, FHyperAIStudioPlanLimits::MaxFieldChars, true)))
		{
			return Context.Error(TEXT("invalid_field"), Path + TEXT(".field"), TEXT("field must be a bounded lowercase identifier."));
		}
		if (Object->HasField(TEXT("expected"))
			&& (!Object->TryGetStringField(TEXT("expected"), OutCondition.Expected)
				|| !IsBoundedText(OutCondition.Expected, FHyperAIStudioPlanLimits::MaxTargetChars, false)))
		{
			return Context.Error(TEXT("invalid_expected"), Path + TEXT(".expected"), TEXT("expected must be a bounded string."));
		}

		const bool bNeedsField = OutCondition.Kind == EHyperAIStudioPlanPreconditionKind::PropertyEquals;
		const bool bNeedsExpected = OutCondition.Kind == EHyperAIStudioPlanPreconditionKind::RevisionEquals
			|| OutCondition.Kind == EHyperAIStudioPlanPreconditionKind::PropertyEquals
			|| OutCondition.Kind == EHyperAIStudioPlanPreconditionKind::EditorStateEquals;
		if (bNeedsField != !OutCondition.Field.IsEmpty())
		{
			return Context.Error(TEXT("precondition_shape"), Path, TEXT("Only property_equals requires field."));
		}
		if (bNeedsExpected != !OutCondition.Expected.IsEmpty())
		{
			return Context.Error(TEXT("precondition_shape"), Path, TEXT("This precondition has an invalid expected-value shape."));
		}
		return true;
	}

	bool ParseEffect(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		FHyperAIStudioPlanEffect& OutEffect,
		FParseContext& Context)
	{
		if (!HasOnlyFields(Object, {TEXT("expected"), TEXT("kind"), TEXT("target"), TEXT("validator_id")}, Path, Context))
		{
			return false;
		}
		FString Kind;
		if (!ReadRequiredString(Object, TEXT("kind"), Path, Kind, Context)
			|| !TryParseEffectKind(Kind, OutEffect.Kind))
		{
			return Context.Error(TEXT("unknown_effect"), Path + TEXT(".kind"), TEXT("The effect kind is not allowlisted."));
		}
		if (!ReadRequiredString(Object, TEXT("target"), Path, OutEffect.Target, Context)
			|| !IsBoundedText(OutEffect.Target, FHyperAIStudioPlanLimits::MaxTargetChars, false))
		{
			return Context.Error(TEXT("invalid_target"), Path + TEXT(".target"), TEXT("A bounded target without control characters is required."));
		}
		if (!ReadRequiredString(Object, TEXT("validator_id"), Path, OutEffect.ValidatorId, Context)
			|| !IsLowerIdentifier(OutEffect.ValidatorId, FHyperAIStudioPlanLimits::MaxValidatorIdChars, true))
		{
			return Context.Error(TEXT("invalid_validator"), Path + TEXT(".validator_id"), TEXT("A bounded typed validator identifier is required."));
		}
		if (Object->HasField(TEXT("expected"))
			&& (!Object->TryGetStringField(TEXT("expected"), OutEffect.Expected)
				|| !IsBoundedText(OutEffect.Expected, FHyperAIStudioPlanLimits::MaxTargetChars, false)))
		{
			return Context.Error(TEXT("invalid_expected"), Path + TEXT(".expected"), TEXT("expected must be a bounded string."));
		}
		return true;
	}

	bool ContainsRequiredPreconditions(
		const TArray<FHyperAIStudioPlanPrecondition>& Actual,
		const TArray<EHyperAIStudioPlanPreconditionKind>& Required)
	{
		for (const EHyperAIStudioPlanPreconditionKind Kind : Required)
		{
			if (!Actual.ContainsByPredicate([Kind](const FHyperAIStudioPlanPrecondition& Item) { return Item.Kind == Kind; }))
			{
				return false;
			}
		}
		return true;
	}

	bool ContainsRequiredEffects(
		const TArray<FHyperAIStudioPlanEffect>& Actual,
		const TArray<EHyperAIStudioPlanEffectKind>& Required)
	{
		for (const EHyperAIStudioPlanEffectKind Kind : Required)
		{
			if (!Actual.ContainsByPredicate([Kind](const FHyperAIStudioPlanEffect& Item) { return Item.Kind == Kind; }))
			{
				return false;
			}
		}
		return true;
	}
}

bool FHyperAIStudioTypedOperationRegistry::Add(
	const FHyperAIStudioTypedOperationMetadata& Metadata,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	OutError.Reset();
	if (!IsValidSafety(Metadata.Safety))
	{
		OutError = TEXT("Operation metadata contains an unknown safety enum.");
		return false;
	}
	if (!IsLowerIdentifier(Metadata.TypeId, MaxTypeIdChars, true))
	{
		OutError = TEXT("Operation type_id must be a bounded lowercase dotted identifier.");
		return false;
	}
	if (Find(Metadata.TypeId))
	{
		OutError = TEXT("Operation type_id is already registered.");
		return false;
	}
	if (Metadata.Arguments.Num() > FHyperAIStudioPlanLimits::MaxArgumentsPerStep
		|| Metadata.EstimatedNativeOperations < 1
		|| Metadata.EstimatedNativeOperations > FHyperAIStudioPlanLimits::MaxNativeOperations
		|| Metadata.EstimatedGameThreadMs < 1
		|| Metadata.EstimatedGameThreadMs > FHyperAIStudioPlanLimits::MaxGameThreadMs)
	{
		OutError = TEXT("Operation metadata exceeds a hard schema or estimate bound.");
		return false;
	}
	TArray<FString> ArgumentNames;
	for (const FHyperAIStudioPlanArgumentSpec& Spec : Metadata.Arguments)
	{
		if (!IsValidValueType(Spec.Type)
			|| !IsLowerIdentifier(Spec.Name, MaxArgumentNameChars, false) || ArgumentNames.Contains(Spec.Name))
		{
			OutError = TEXT("Argument names must be unique bounded lowercase identifiers.");
			return false;
		}
		ArgumentNames.Add(Spec.Name);
		if ((Spec.Type == EHyperAIStudioPlanValueType::String
				|| Spec.Type == EHyperAIStudioPlanValueType::StringArray)
			&& (Spec.MaxStringChars < 1 || Spec.MaxStringChars > FHyperAIStudioPlanLimits::MaxArgumentStringChars))
		{
			OutError = TEXT("String argument metadata must declare a valid character bound.");
			return false;
		}
		if (Spec.Type == EHyperAIStudioPlanValueType::StringArray
			&& (Spec.MaxArrayItems < 1 || Spec.MaxArrayItems > FHyperAIStudioPlanLimits::MaxArgumentArrayItems))
		{
			OutError = TEXT("String-array argument metadata must declare a valid item bound.");
			return false;
		}
		if (Spec.Type == EHyperAIStudioPlanValueType::Integer
			&& (Spec.MinInteger > Spec.MaxInteger
				|| Spec.MinInteger < -FHyperAIStudioPlanLimits::MaxExactJsonInteger
				|| Spec.MaxInteger > FHyperAIStudioPlanLimits::MaxExactJsonInteger))
		{
			OutError = TEXT("Integer argument metadata must stay inside JSON's exact-integer range.");
			return false;
		}
		TSet<FString> SeenAllowedStrings;
		if (!Spec.AllowedStrings.IsEmpty() && Spec.Type != EHyperAIStudioPlanValueType::String)
		{
			OutError = TEXT("Only string arguments can declare an enum allowlist.");
			return false;
		}
		for (const FString& Allowed : Spec.AllowedStrings)
		{
			if (!IsBoundedText(Allowed, Spec.MaxStringChars, false) || SeenAllowedStrings.Contains(Allowed))
			{
				OutError = TEXT("Argument enum values must be unique bounded strings.");
				return false;
			}
			SeenAllowedStrings.Add(Allowed);
		}
	}

	TSet<EHyperAIStudioPlanPreconditionKind> SeenAllowedPreconditions;
	TSet<EHyperAIStudioPlanPreconditionKind> SeenRequiredPreconditions;
	for (const EHyperAIStudioPlanPreconditionKind Kind : Metadata.AllowedPreconditions)
	{
		if (!IsValidPreconditionKind(Kind) || SeenAllowedPreconditions.Contains(Kind))
		{
			OutError = TEXT("Allowed precondition kinds must be known and unique.");
			return false;
		}
		SeenAllowedPreconditions.Add(Kind);
	}
	for (const EHyperAIStudioPlanPreconditionKind Kind : Metadata.RequiredPreconditions)
	{
		if (!IsValidPreconditionKind(Kind) || SeenRequiredPreconditions.Contains(Kind)
			|| !Metadata.AllowedPreconditions.Contains(Kind))
		{
			OutError = TEXT("Every required precondition must be known, unique, and allowlisted.");
			return false;
		}
		SeenRequiredPreconditions.Add(Kind);
	}
	TSet<EHyperAIStudioPlanEffectKind> SeenAllowedEffects;
	TSet<EHyperAIStudioPlanEffectKind> SeenRequiredEffects;
	for (const EHyperAIStudioPlanEffectKind Kind : Metadata.AllowedEffects)
	{
		if (!IsValidEffectKind(Kind) || SeenAllowedEffects.Contains(Kind)
			|| SafetyRank(RequiredSafetyForEffect(Kind)) > SafetyRank(Metadata.Safety))
		{
			OutError = TEXT("Allowed effects must be known, unique, and no riskier than the operation safety class.");
			return false;
		}
		SeenAllowedEffects.Add(Kind);
	}
	for (const EHyperAIStudioPlanEffectKind Kind : Metadata.RequiredEffects)
	{
		if (!IsValidEffectKind(Kind) || SeenRequiredEffects.Contains(Kind)
			|| !Metadata.AllowedEffects.Contains(Kind))
		{
			OutError = TEXT("Every required effect must be known, unique, and allowlisted.");
			return false;
		}
		SeenRequiredEffects.Add(Kind);
	}
	if (Metadata.Safety == EHyperAIStudioPlanSafety::Read && !Metadata.AllowedEffects.IsEmpty())
	{
		OutError = TEXT("Read operations cannot declare effects.");
		return false;
	}
	if (Metadata.Safety == EHyperAIStudioPlanSafety::Read
		&& (Metadata.bCompileOnce || Metadata.bValidateOnce || Metadata.bSaveOnce || Metadata.bVerifyFreshOnce))
	{
		OutError = TEXT("Read operations cannot schedule execution finalizers.");
		return false;
	}
	if (Metadata.Safety != EHyperAIStudioPlanSafety::Read
		&& (Metadata.RequiredPreconditions.IsEmpty()
			|| Metadata.RequiredEffects.IsEmpty()
			|| Metadata.PostconditionValidatorId.IsEmpty()
			|| !Metadata.bValidateOnce
			|| !Metadata.bVerifyFreshOnce
			|| !Metadata.bSupportsDryRun))
	{
		OutError = TEXT("Mutating/external operations require dry-run support, preconditions/effects, a typed validator, pre-save validation, and fresh postcondition verification.");
		return false;
	}
	if (Metadata.Safety == EHyperAIStudioPlanSafety::Destructive
		&& !Metadata.RequiredEffects.Contains(EHyperAIStudioPlanEffectKind::ObjectDeleted))
	{
		OutError = TEXT("Destructive operations must require an object_deleted effect.");
		return false;
	}
	if (Metadata.Safety == EHyperAIStudioPlanSafety::ExternalEffect
		&& !Metadata.RequiredEffects.Contains(EHyperAIStudioPlanEffectKind::RuntimeExternalEffect))
	{
		OutError = TEXT("External operations must require a runtime_external_effect.");
		return false;
	}
	if (!Metadata.PostconditionValidatorId.IsEmpty()
		&& !IsLowerIdentifier(Metadata.PostconditionValidatorId, FHyperAIStudioPlanLimits::MaxValidatorIdChars, true))
	{
		OutError = TEXT("postcondition_validator_id must be a bounded lowercase dotted identifier.");
		return false;
	}
	if (Metadata.bCompileOnce && !Metadata.bSaveOnce)
	{
		OutError = TEXT("A compile-once operation must also schedule a single save.");
		return false;
	}

	TSet<EHyperAIStudioPlanPreconditionKind> BoundPreconditionKinds;
	for (const FHyperAIStudioPlanPreconditionTargetBinding& Binding : Metadata.PreconditionTargetBindings)
	{
		const bool bArgument = !Binding.ArgumentName.IsEmpty();
		const bool bFixed = !Binding.FixedTarget.IsEmpty();
		const FHyperAIStudioPlanArgumentSpec* Argument = Metadata.Arguments.FindByPredicate(
			[&Binding](const FHyperAIStudioPlanArgumentSpec& Spec) { return Spec.Name == Binding.ArgumentName; });
		if (!IsValidPreconditionKind(Binding.Kind) || !Metadata.AllowedPreconditions.Contains(Binding.Kind)
			|| BoundPreconditionKinds.Contains(Binding.Kind) || bArgument == bFixed
			|| (bArgument && (!Argument || Argument->Type != EHyperAIStudioPlanValueType::String))
			|| (bFixed && !IsBoundedText(Binding.FixedTarget, FHyperAIStudioPlanLimits::MaxTargetChars, false)))
		{
			OutError = TEXT("Each precondition target binding must be unique and use exactly one valid string argument or fixed target.");
			return false;
		}
		BoundPreconditionKinds.Add(Binding.Kind);
	}
	for (const EHyperAIStudioPlanPreconditionKind Kind : Metadata.AllowedPreconditions)
	{
		if (!BoundPreconditionKinds.Contains(Kind))
		{
			OutError = TEXT("Every admitted precondition needs an independently declared target binding.");
			return false;
		}
	}

	TSet<EHyperAIStudioPlanEffectKind> BoundEffectKinds;
	for (const FHyperAIStudioPlanEffectTargetBinding& Binding : Metadata.EffectTargetBindings)
	{
		const bool bArgument = !Binding.ArgumentName.IsEmpty();
		const bool bFixed = !Binding.FixedTarget.IsEmpty();
		const FHyperAIStudioPlanArgumentSpec* Argument = Metadata.Arguments.FindByPredicate(
			[&Binding](const FHyperAIStudioPlanArgumentSpec& Spec) { return Spec.Name == Binding.ArgumentName; });
		if (!IsValidEffectKind(Binding.Kind) || !Metadata.AllowedEffects.Contains(Binding.Kind)
			|| BoundEffectKinds.Contains(Binding.Kind) || bArgument == bFixed
			|| (bArgument && (!Argument || Argument->Type != EHyperAIStudioPlanValueType::String))
			|| (bFixed && !IsBoundedText(Binding.FixedTarget, FHyperAIStudioPlanLimits::MaxTargetChars, false)))
		{
			OutError = TEXT("Each effect target binding must be unique and use exactly one valid string argument or fixed target.");
			return false;
		}
		BoundEffectKinds.Add(Binding.Kind);
	}
	for (const EHyperAIStudioPlanEffectKind Kind : Metadata.AllowedEffects)
	{
		if (!BoundEffectKinds.Contains(Kind))
		{
			OutError = TEXT("Every admitted effect needs an independently declared target binding.");
			return false;
		}
	}
	for (const FString& Prerequisite : Metadata.Prerequisites)
	{
		if (!IsBoundedText(Prerequisite, 128, false))
		{
			OutError = TEXT("Prerequisite names must be bounded text.");
			return false;
		}
	}
	Operations.Add(Metadata);
	return true;
}

const FHyperAIStudioTypedOperationMetadata* FHyperAIStudioTypedOperationRegistry::Find(const FString& TypeId) const
{
	return Operations.FindByPredicate([&TypeId](const FHyperAIStudioTypedOperationMetadata& Item)
	{
		return Item.TypeId == TypeId;
	});
}

FString FHyperAIStudioTypedOperationRegistry::ComputeCapabilityHash() const
{
	using namespace HyperAIStudio::TypedPlan::Private;
	TArray<const FHyperAIStudioTypedOperationMetadata*> Sorted;
	for (const FHyperAIStudioTypedOperationMetadata& Operation : Operations)
	{
		Sorted.Add(&Operation);
	}
	Sorted.Sort([](const FHyperAIStudioTypedOperationMetadata& Left, const FHyperAIStudioTypedOperationMetadata& Right)
	{
		return Left.TypeId < Right.TypeId;
	});

	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.operation-registry.v1"));
	AppendInt(Canonical, Sorted.Num());
	for (const FHyperAIStudioTypedOperationMetadata* Operation : Sorted)
	{
		AppendToken(Canonical, Operation->TypeId);
		AppendToken(Canonical, FHyperAIStudioTypedPlanValidator::SafetyToString(Operation->Safety));
		TArray<FHyperAIStudioPlanArgumentSpec> Arguments = Operation->Arguments;
		Arguments.Sort([](const FHyperAIStudioPlanArgumentSpec& Left, const FHyperAIStudioPlanArgumentSpec& Right)
		{
			return Left.Name < Right.Name;
		});
		AppendInt(Canonical, Arguments.Num());
		for (const FHyperAIStudioPlanArgumentSpec& Spec : Arguments)
		{
			AppendToken(Canonical, Spec.Name);
			AppendToken(Canonical, ValueTypeToString(Spec.Type));
			AppendBool(Canonical, Spec.bRequired);
			AppendInt(Canonical, Spec.MaxStringChars);
			AppendInt(Canonical, Spec.MaxArrayItems);
			AppendInt(Canonical, Spec.MinInteger);
			AppendInt(Canonical, Spec.MaxInteger);
			TArray<FString> Allowed = Spec.AllowedStrings;
			Allowed.Sort();
			AppendInt(Canonical, Allowed.Num());
			for (const FString& Item : Allowed) { AppendToken(Canonical, Item); }
		}

		TArray<FString> Conditions;
		for (const EHyperAIStudioPlanPreconditionKind Kind : Operation->AllowedPreconditions) { Conditions.Add(PreconditionKindToString(Kind)); }
		Conditions.Sort();
		for (const FString& Item : Conditions) { AppendToken(Canonical, TEXT("allow-pre:") + Item); }
		Conditions.Reset();
		for (const EHyperAIStudioPlanPreconditionKind Kind : Operation->RequiredPreconditions) { Conditions.Add(PreconditionKindToString(Kind)); }
		Conditions.Sort();
		for (const FString& Item : Conditions) { AppendToken(Canonical, TEXT("require-pre:") + Item); }
		TArray<FString> Effects;
		for (const EHyperAIStudioPlanEffectKind Kind : Operation->AllowedEffects) { Effects.Add(EffectKindToString(Kind)); }
		Effects.Sort();
		for (const FString& Item : Effects) { AppendToken(Canonical, TEXT("allow-effect:") + Item); }
		Effects.Reset();
		for (const EHyperAIStudioPlanEffectKind Kind : Operation->RequiredEffects) { Effects.Add(EffectKindToString(Kind)); }
		Effects.Sort();
		for (const FString& Item : Effects) { AppendToken(Canonical, TEXT("require-effect:") + Item); }
		TArray<FString> Bindings;
		for (const FHyperAIStudioPlanPreconditionTargetBinding& Binding : Operation->PreconditionTargetBindings)
		{
			FString Encoded;
			AppendToken(Encoded, PreconditionKindToString(Binding.Kind));
			AppendToken(Encoded, Binding.ArgumentName);
			AppendToken(Encoded, Binding.FixedTarget);
			Bindings.Add(TEXT("pre:") + Encoded);
		}
		for (const FHyperAIStudioPlanEffectTargetBinding& Binding : Operation->EffectTargetBindings)
		{
			FString Encoded;
			AppendToken(Encoded, EffectKindToString(Binding.Kind));
			AppendToken(Encoded, Binding.ArgumentName);
			AppendToken(Encoded, Binding.FixedTarget);
			Bindings.Add(TEXT("effect:") + Encoded);
		}
		Bindings.Sort();
		AppendInt(Canonical, Bindings.Num());
		for (const FString& Item : Bindings) { AppendToken(Canonical, Item); }

		TArray<FString> Prerequisites = Operation->Prerequisites;
		Prerequisites.Sort();
		for (const FString& Item : Prerequisites) { AppendToken(Canonical, TEXT("prerequisite:") + Item); }
		AppendToken(Canonical, Operation->PostconditionValidatorId);
		AppendInt(Canonical, Operation->EstimatedNativeOperations);
		AppendInt(Canonical, Operation->EstimatedGameThreadMs);
		AppendBool(Canonical, Operation->bCompileOnce);
		AppendBool(Canonical, Operation->bSaveOnce);
		AppendBool(Canonical, Operation->bValidateOnce);
		AppendBool(Canonical, Operation->bSupportsDryRun);
		AppendBool(Canonical, Operation->bVerifyFreshOnce);
	}
	return HashUtf8Sha256(Canonical);
}

FHyperAIStudioTypedOperationRegistry FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry()
{
	FHyperAIStudioTypedOperationRegistry Registry;
	FString Error;

	FHyperAIStudioTypedOperationMetadata Inspect;
	Inspect.TypeId = TEXT("foundation.inspect");
	Inspect.Safety = EHyperAIStudioPlanSafety::Read;
	Inspect.Arguments = {
		{TEXT("subject"), EHyperAIStudioPlanValueType::String, true, 1024, 0, 0, 0, {}},
		{TEXT("fields"), EHyperAIStudioPlanValueType::StringArray, false, 128, 32, 0, 0, {}}};
	Inspect.AllowedPreconditions = {EHyperAIStudioPlanPreconditionKind::ObjectExists};
	Inspect.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::ObjectExists, TEXT("subject"), FString()}};
	Inspect.EstimatedNativeOperations = 1;
	Inspect.EstimatedGameThreadMs = 2;
	Registry.Add(Inspect, Error);

	FHyperAIStudioTypedOperationMetadata Blueprint;
	Blueprint.TypeId = FHyperAIStudioBlueprintPatch::EditTypedOperationType;
	Blueprint.Safety = EHyperAIStudioPlanSafety::Edit;
	Blueprint.Arguments = {
		{TEXT("asset_path"), EHyperAIStudioPlanValueType::String, true, 1024, 0, 0, 0, {}},
		{TEXT("patch_id"), EHyperAIStudioPlanValueType::String, true, 128, 0, 0, 0, {}}};
	Blueprint.AllowedPreconditions = {
		EHyperAIStudioPlanPreconditionKind::ObjectExists,
		EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	Blueprint.RequiredPreconditions = {
		EHyperAIStudioPlanPreconditionKind::ObjectExists,
		EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	Blueprint.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::ObjectExists, TEXT("asset_path"), FString()},
		{EHyperAIStudioPlanPreconditionKind::RevisionEquals, TEXT("asset_path"), FString()}};
	Blueprint.AllowedEffects = {EHyperAIStudioPlanEffectKind::ObjectUpdated};
	Blueprint.RequiredEffects = {EHyperAIStudioPlanEffectKind::ObjectUpdated};
	Blueprint.EffectTargetBindings = {
		{EHyperAIStudioPlanEffectKind::ObjectUpdated, TEXT("asset_path"), FString()}};
	Blueprint.Prerequisites = {TEXT("BlueprintGraph"), TEXT("KismetCompiler")};
	Blueprint.PostconditionValidatorId = TEXT("blueprint.compile_validate");
	Blueprint.EstimatedNativeOperations = 4;
	Blueprint.EstimatedGameThreadMs = 20;
	Blueprint.bCompileOnce = true;
	Blueprint.bSaveOnce = true;
	Blueprint.bValidateOnce = true;
	Blueprint.bVerifyFreshOnce = true;
	Registry.Add(Blueprint, Error);

	FHyperAIStudioTypedOperationMetadata BlueprintDelete;
	BlueprintDelete.TypeId = FHyperAIStudioBlueprintPatch::DeleteTypedOperationType;
	BlueprintDelete.Safety = EHyperAIStudioPlanSafety::Destructive;
	BlueprintDelete.Arguments = {
		{TEXT("asset_path"), EHyperAIStudioPlanValueType::String, true, 1024, 0, 0, 0, {}},
		{TEXT("patch_id"), EHyperAIStudioPlanValueType::String, true, 128, 0, 0, 0, {}}};
	BlueprintDelete.AllowedPreconditions = {
		EHyperAIStudioPlanPreconditionKind::ObjectExists,
		EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	BlueprintDelete.RequiredPreconditions = {
		EHyperAIStudioPlanPreconditionKind::ObjectExists,
		EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	BlueprintDelete.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::ObjectExists, TEXT("asset_path"), FString()},
		{EHyperAIStudioPlanPreconditionKind::RevisionEquals, TEXT("asset_path"), FString()}};
	BlueprintDelete.AllowedEffects = {
		EHyperAIStudioPlanEffectKind::ObjectUpdated,
		EHyperAIStudioPlanEffectKind::ObjectDeleted};
	BlueprintDelete.RequiredEffects = {
		EHyperAIStudioPlanEffectKind::ObjectUpdated,
		EHyperAIStudioPlanEffectKind::ObjectDeleted};
	BlueprintDelete.EffectTargetBindings = {
		{EHyperAIStudioPlanEffectKind::ObjectUpdated, TEXT("asset_path"), FString()},
		{EHyperAIStudioPlanEffectKind::ObjectDeleted, TEXT("asset_path"), FString()}};
	BlueprintDelete.Prerequisites = {TEXT("BlueprintGraph"), TEXT("KismetCompiler")};
	BlueprintDelete.PostconditionValidatorId = TEXT("blueprint.delete_compile_validate");
	BlueprintDelete.EstimatedNativeOperations = 4;
	BlueprintDelete.EstimatedGameThreadMs = 20;
	BlueprintDelete.bCompileOnce = true;
	BlueprintDelete.bSaveOnce = true;
	BlueprintDelete.bValidateOnce = true;
	BlueprintDelete.bVerifyFreshOnce = true;
	Registry.Add(BlueprintDelete, Error);

	FHyperAIStudioTypedOperationMetadata Update;
	Update.TypeId = TEXT("asset.apply_update");
	Update.Safety = EHyperAIStudioPlanSafety::Edit;
	Update.Arguments = {
		{TEXT("asset_path"), EHyperAIStudioPlanValueType::String, true, 1024, 0, 0, 0, {}},
		{TEXT("change_set_id"), EHyperAIStudioPlanValueType::String, true, 128, 0, 0, 0, {}}};
	Update.AllowedPreconditions = {EHyperAIStudioPlanPreconditionKind::ObjectExists, EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	Update.RequiredPreconditions = {EHyperAIStudioPlanPreconditionKind::ObjectExists};
	Update.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::ObjectExists, TEXT("asset_path"), FString()},
		{EHyperAIStudioPlanPreconditionKind::RevisionEquals, TEXT("asset_path"), FString()}};
	Update.AllowedEffects = {EHyperAIStudioPlanEffectKind::ObjectUpdated};
	Update.RequiredEffects = {EHyperAIStudioPlanEffectKind::ObjectUpdated};
	Update.EffectTargetBindings = {
		{EHyperAIStudioPlanEffectKind::ObjectUpdated, TEXT("asset_path"), FString()}};
	Update.PostconditionValidatorId = TEXT("asset.fresh_read");
	Update.EstimatedNativeOperations = 2;
	Update.EstimatedGameThreadMs = 8;
	Update.bSaveOnce = true;
	Update.bValidateOnce = true;
	Update.bVerifyFreshOnce = true;
	Registry.Add(Update, Error);

	FHyperAIStudioTypedOperationMetadata Delete;
	Delete.TypeId = TEXT("asset.delete");
	Delete.Safety = EHyperAIStudioPlanSafety::Destructive;
	Delete.Arguments = {{TEXT("asset_path"), EHyperAIStudioPlanValueType::String, true, 1024, 0, 0, 0, {}}};
	Delete.AllowedPreconditions = {EHyperAIStudioPlanPreconditionKind::ObjectExists, EHyperAIStudioPlanPreconditionKind::RevisionEquals};
	Delete.RequiredPreconditions = {EHyperAIStudioPlanPreconditionKind::ObjectExists};
	Delete.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::ObjectExists, TEXT("asset_path"), FString()},
		{EHyperAIStudioPlanPreconditionKind::RevisionEquals, TEXT("asset_path"), FString()}};
	Delete.AllowedEffects = {EHyperAIStudioPlanEffectKind::ObjectDeleted};
	Delete.RequiredEffects = {EHyperAIStudioPlanEffectKind::ObjectDeleted};
	Delete.EffectTargetBindings = {
		{EHyperAIStudioPlanEffectKind::ObjectDeleted, TEXT("asset_path"), FString()}};
	Delete.PostconditionValidatorId = TEXT("asset.absence");
	Delete.EstimatedNativeOperations = 2;
	Delete.EstimatedGameThreadMs = 10;
	Delete.bSaveOnce = true;
	Delete.bValidateOnce = true;
	Delete.bVerifyFreshOnce = true;
	Registry.Add(Delete, Error);

	FHyperAIStudioTypedOperationMetadata Playtest;
	Playtest.TypeId = TEXT("playtest.run");
	Playtest.Safety = EHyperAIStudioPlanSafety::ExternalEffect;
	Playtest.Arguments = {
		{TEXT("scenario_id"), EHyperAIStudioPlanValueType::String, true, 128, 0, 0, 0, {}},
		{TEXT("max_seconds"), EHyperAIStudioPlanValueType::Integer, true, 0, 0, 1, 120, {}}};
	Playtest.AllowedPreconditions = {EHyperAIStudioPlanPreconditionKind::EditorStateEquals};
	Playtest.RequiredPreconditions = {EHyperAIStudioPlanPreconditionKind::EditorStateEquals};
	Playtest.PreconditionTargetBindings = {
		{EHyperAIStudioPlanPreconditionKind::EditorStateEquals, FString(), TEXT("pie")}};
	Playtest.AllowedEffects = {EHyperAIStudioPlanEffectKind::RuntimeExternalEffect};
	Playtest.RequiredEffects = {EHyperAIStudioPlanEffectKind::RuntimeExternalEffect};
	Playtest.EffectTargetBindings = {
		{EHyperAIStudioPlanEffectKind::RuntimeExternalEffect, FString(), TEXT("pie")}};
	Playtest.Prerequisites = {TEXT("UnrealEd")};
	Playtest.PostconditionValidatorId = TEXT("playtest.assertions");
	Playtest.EstimatedNativeOperations = 3;
	Playtest.EstimatedGameThreadMs = 10;
	Playtest.bValidateOnce = true;
	Playtest.bVerifyFreshOnce = true;
	Registry.Add(Playtest, Error);

	return Registry;
}

namespace HyperAIStudio::TypedPlan::Private
{
	bool ResolveBoundTarget(
		const FString& ArgumentName,
		const FString& FixedTarget,
		const FHyperAIStudioPlanStep& Step,
		FString& OutTarget)
	{
		if (!FixedTarget.IsEmpty())
		{
			OutTarget = FixedTarget;
			return true;
		}
		const FHyperAIStudioPlanValue* Value = Step.Arguments.Find(ArgumentName);
		if (!Value || Value->Type != EHyperAIStudioPlanValueType::String)
		{
			return false;
		}
		OutTarget = Value->StringValue;
		return true;
	}

	bool ParseStep(
		const TSharedPtr<FJsonObject>& Object,
		const int32 StepIndex,
		const FHyperAIStudioTypedOperationRegistry& Registry,
		FHyperAIStudioPlanStep& OutStep,
		FParseContext& Context)
	{
		const FString Path = FString::Printf(TEXT("$.steps[%d]"), StepIndex);
		if (!HasOnlyFields(Object,
			{TEXT("arguments"), TEXT("budget"), TEXT("depends_on"), TEXT("effects"), TEXT("id"), TEXT("operation"), TEXT("preconditions")},
			Path, Context))
		{
			return false;
		}
		if (!ReadRequiredString(Object, TEXT("id"), Path, OutStep.StepId, Context)
			|| !IsLowerIdentifier(OutStep.StepId, FHyperAIStudioPlanLimits::MaxStepIdChars, false))
		{
			return Context.Error(TEXT("invalid_step_id"), Path + TEXT(".id"),
				TEXT("Step ids must be bounded unique lowercase identifiers."));
		}
		if (!ReadRequiredString(Object, TEXT("operation"), Path, OutStep.OperationType, Context))
		{
			return false;
		}
		const FHyperAIStudioTypedOperationMetadata* Metadata = Registry.Find(OutStep.OperationType);
		if (!Metadata)
		{
			return Context.Error(TEXT("operation_not_allowlisted"), Path + TEXT(".operation"),
				TEXT("Only typed operations present in the active first-party registry are accepted."));
		}
		OutStep.Safety = Metadata->Safety;
		OutStep.bCompileOnce = Metadata->bCompileOnce;
		OutStep.bSaveOnce = Metadata->bSaveOnce;
		OutStep.bValidateOnce = Metadata->bValidateOnce;
		OutStep.bVerifyFreshOnce = Metadata->bVerifyFreshOnce;

		const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
		if (!ReadRequiredArray(Object, TEXT("depends_on"), Path, Dependencies, Context)
			|| Dependencies->Num() > FHyperAIStudioPlanLimits::MaxDependenciesPerStep)
		{
			return Context.Error(TEXT("dependency_limit"), Path + TEXT(".depends_on"), TEXT("The dependency bound was exceeded."));
		}
		for (int32 Index = 0; Index < Dependencies->Num(); ++Index)
		{
			FString Dependency;
			if (!(*Dependencies)[Index].IsValid() || !(*Dependencies)[Index]->TryGetString(Dependency)
				|| !IsLowerIdentifier(Dependency, FHyperAIStudioPlanLimits::MaxStepIdChars, false)
				|| Dependency == OutStep.StepId || OutStep.DependsOn.Contains(Dependency))
			{
				return Context.Error(TEXT("invalid_dependency"),
					FString::Printf(TEXT("%s.depends_on[%d]"), *Path, Index),
					TEXT("Dependencies must be unique valid step ids and cannot reference the step itself."));
			}
			OutStep.DependsOn.Add(Dependency);
		}

		TSharedPtr<FJsonObject> Arguments;
		if (!ReadRequiredObject(Object, TEXT("arguments"), Path, Arguments, Context)
			|| Arguments->Values.Num() > FHyperAIStudioPlanLimits::MaxArgumentsPerStep)
		{
			return Context.Error(TEXT("argument_limit"), Path + TEXT(".arguments"), TEXT("The argument field bound was exceeded."));
		}
		TArray<FString> ArgumentKeys;
		ArgumentKeys.Reserve(Arguments->Values.Num());
		for (const TPair<FJsonObject::FStringType, TSharedPtr<FJsonValue>>& Field : Arguments->Values)
		{
			ArgumentKeys.Add(FString(Field.Key.ToView()));
		}
		ArgumentKeys.Sort();
		for (const FString& Key : ArgumentKeys)
		{
			const FHyperAIStudioPlanArgumentSpec* Spec = Metadata->Arguments.FindByPredicate([&Key](const FHyperAIStudioPlanArgumentSpec& Item)
			{
				return Item.Name == Key;
			});
			if (!Spec)
			{
				return Context.Error(TEXT("unknown_argument"), Path + TEXT(".arguments.") + Key,
					TEXT("The typed operation does not declare this argument."));
			}
			const TSharedPtr<FJsonValue> ArgumentValue = Arguments->TryGetField(Key);
			if (!ArgumentValue.IsValid())
			{
				return Context.Error(TEXT("invalid_argument"), Path + TEXT(".arguments.") + Key,
					TEXT("The argument value is invalid."));
			}
			FHyperAIStudioPlanValue Parsed;
			if (!ParseArgumentValue(ArgumentValue, *Spec, Path + TEXT(".arguments.") + Key, Parsed, Context))
			{
				return false;
			}
			OutStep.Arguments.Add(Key, MoveTemp(Parsed));
		}
		for (const FHyperAIStudioPlanArgumentSpec& Spec : Metadata->Arguments)
		{
			if (Spec.bRequired && !OutStep.Arguments.Contains(Spec.Name))
			{
				return Context.Error(TEXT("missing_argument"), Path + TEXT(".arguments.") + Spec.Name,
					TEXT("A required typed argument is missing."));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Preconditions = nullptr;
		if (!ReadRequiredArray(Object, TEXT("preconditions"), Path, Preconditions, Context)
			|| Preconditions->Num() > FHyperAIStudioPlanLimits::MaxPreconditionsPerStep)
		{
			return Context.Error(TEXT("precondition_limit"), Path + TEXT(".preconditions"), TEXT("The precondition bound was exceeded."));
		}
		TArray<FString> SeenPreconditions;
		for (int32 Index = 0; Index < Preconditions->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* ConditionObject = nullptr;
			const FString ConditionPath = FString::Printf(TEXT("%s.preconditions[%d]"), *Path, Index);
			if (!(*Preconditions)[Index].IsValid() || !(*Preconditions)[Index]->TryGetObject(ConditionObject)
				|| !ConditionObject || !ConditionObject->IsValid())
			{
				return Context.Error(TEXT("precondition_object"), ConditionPath, TEXT("Each precondition must be an object."));
			}
			FHyperAIStudioPlanPrecondition Condition;
			if (!ParsePrecondition(*ConditionObject, ConditionPath, Condition, Context))
			{
				return false;
			}
			if (!Metadata->AllowedPreconditions.Contains(Condition.Kind))
			{
				return Context.Error(TEXT("precondition_not_allowed"), ConditionPath + TEXT(".kind"),
					TEXT("This typed operation does not allow the declared precondition."));
			}
			if (const FHyperAIStudioPlanPreconditionTargetBinding* Binding =
				Metadata->PreconditionTargetBindings.FindByPredicate(
					[&Condition](const FHyperAIStudioPlanPreconditionTargetBinding& Item) { return Item.Kind == Condition.Kind; }))
			{
				FString ExpectedTarget;
				if (!ResolveBoundTarget(Binding->ArgumentName, Binding->FixedTarget, OutStep, ExpectedTarget)
					|| Condition.Target != ExpectedTarget)
				{
					return Context.Error(TEXT("precondition_target_mismatch"), ConditionPath + TEXT(".target"),
						TEXT("The precondition target must equal its registry-bound typed argument or fixed target."));
				}
			}
			const FString Fingerprint = PreconditionKindToString(Condition.Kind) + TEXT("\n") + Condition.Target
				+ TEXT("\n") + Condition.Field + TEXT("\n") + Condition.Expected;
			if (SeenPreconditions.Contains(Fingerprint))
			{
				return Context.Error(TEXT("duplicate_precondition"), ConditionPath, TEXT("Duplicate preconditions are rejected."));
			}
			SeenPreconditions.Add(Fingerprint);
			OutStep.Preconditions.Add(MoveTemp(Condition));
		}
		if (!ContainsRequiredPreconditions(OutStep.Preconditions, Metadata->RequiredPreconditions))
		{
			return Context.Error(TEXT("missing_precondition"), Path + TEXT(".preconditions"),
				TEXT("The operation is missing a metadata-required precondition."));
		}

		const TArray<TSharedPtr<FJsonValue>>* Effects = nullptr;
		if (!ReadRequiredArray(Object, TEXT("effects"), Path, Effects, Context)
			|| Effects->Num() > FHyperAIStudioPlanLimits::MaxEffectsPerStep)
		{
			return Context.Error(TEXT("effect_limit"), Path + TEXT(".effects"), TEXT("The effect bound was exceeded."));
		}
		TArray<FString> SeenEffects;
		for (int32 Index = 0; Index < Effects->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* EffectObject = nullptr;
			const FString EffectPath = FString::Printf(TEXT("%s.effects[%d]"), *Path, Index);
			if (!(*Effects)[Index].IsValid() || !(*Effects)[Index]->TryGetObject(EffectObject)
				|| !EffectObject || !EffectObject->IsValid())
			{
				return Context.Error(TEXT("effect_object"), EffectPath, TEXT("Each effect must be an object."));
			}
			FHyperAIStudioPlanEffect Effect;
			if (!ParseEffect(*EffectObject, EffectPath, Effect, Context))
			{
				return false;
			}
			if (!Metadata->AllowedEffects.Contains(Effect.Kind))
			{
				return Context.Error(TEXT("effect_not_allowed"), EffectPath + TEXT(".kind"),
					TEXT("The registry-derived safety class does not allow this effect."));
			}
			if (Effect.ValidatorId != Metadata->PostconditionValidatorId)
			{
				return Context.Error(TEXT("validator_mismatch"), EffectPath + TEXT(".validator_id"),
					TEXT("The effect must use the operation's independently declared validator."));
			}
			const FHyperAIStudioPlanEffectTargetBinding* Binding = Metadata->EffectTargetBindings.FindByPredicate(
				[&Effect](const FHyperAIStudioPlanEffectTargetBinding& Item) { return Item.Kind == Effect.Kind; });
			FString ExpectedTarget;
			if (!Binding || !ResolveBoundTarget(Binding->ArgumentName, Binding->FixedTarget, OutStep, ExpectedTarget)
				|| Effect.Target != ExpectedTarget)
			{
				return Context.Error(TEXT("effect_target_mismatch"), EffectPath + TEXT(".target"),
					TEXT("The effect target must equal its registry-bound typed argument or fixed target."));
			}
			const FString Fingerprint = EffectKindToString(Effect.Kind) + TEXT("\n") + Effect.Target
				+ TEXT("\n") + Effect.ValidatorId + TEXT("\n") + Effect.Expected;
			if (SeenEffects.Contains(Fingerprint))
			{
				return Context.Error(TEXT("duplicate_effect"), EffectPath, TEXT("Duplicate effects are rejected."));
			}
			SeenEffects.Add(Fingerprint);
			OutStep.Effects.Add(MoveTemp(Effect));
		}
		if (!ContainsRequiredEffects(OutStep.Effects, Metadata->RequiredEffects))
		{
			return Context.Error(TEXT("missing_effect"), Path + TEXT(".effects"),
				TEXT("The operation is missing a metadata-required effect."));
		}
		if (Metadata->Safety == EHyperAIStudioPlanSafety::Read && !OutStep.Effects.IsEmpty())
		{
			return Context.Error(TEXT("read_declares_effect"), Path + TEXT(".effects"),
				TEXT("Read operations cannot declare mutation or external effects."));
		}

		TSharedPtr<FJsonObject> Budget;
		if (!ReadRequiredObject(Object, TEXT("budget"), Path, Budget, Context)
			|| !ParseStepBudget(Budget, Path + TEXT(".budget"), OutStep.Budget, Context))
		{
			return false;
		}
		if (OutStep.Budget.MaxNativeOperations < Metadata->EstimatedNativeOperations
			|| OutStep.Budget.MaxGameThreadMs < Metadata->EstimatedGameThreadMs)
		{
			return Context.Error(TEXT("step_budget_too_small"), Path + TEXT(".budget"),
				TEXT("The step budget cannot cover the allowlisted operation's conservative estimate."));
		}
		return true;
	}

	bool BuildStableTopologicalOrder(
		FHyperAIStudioValidatedPlan& Plan,
		FParseContext& Context)
	{
		TMap<FString, int32> Indices;
		for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
		{
			if (Indices.Contains(Plan.Steps[Index].StepId))
			{
				return Context.Error(TEXT("duplicate_step_id"), FString::Printf(TEXT("$.steps[%d].id"), Index),
					TEXT("Step ids must be unique."));
			}
			Indices.Add(Plan.Steps[Index].StepId, Index);
		}

		TArray<int32> InDegree;
		InDegree.Init(0, Plan.Steps.Num());
		TArray<TArray<int32>> Dependents;
		Dependents.SetNum(Plan.Steps.Num());
		int32 TotalDependencies = 0;
		for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
		{
			for (const FString& Dependency : Plan.Steps[Index].DependsOn)
			{
				const int32* DependencyIndex = Indices.Find(Dependency);
				if (!DependencyIndex)
				{
					return Context.Error(TEXT("missing_dependency"), FString::Printf(TEXT("$.steps[%d].depends_on"), Index),
						TEXT("A dependency does not name a step in this plan."));
				}
				++InDegree[Index];
				Dependents[*DependencyIndex].Add(Index);
				++TotalDependencies;
				if (TotalDependencies > FHyperAIStudioPlanLimits::MaxTotalDependencies)
				{
					return Context.Error(TEXT("dependency_limit"), TEXT("$.steps"), TEXT("The total dependency bound was exceeded."));
				}
			}
		}

		TArray<bool> Selected;
		Selected.Init(false, Plan.Steps.Num());
		for (int32 Position = 0; Position < Plan.Steps.Num(); ++Position)
		{
			int32 Candidate = INDEX_NONE;
			for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
			{
				if (!Selected[Index] && InDegree[Index] == 0)
				{
					Candidate = Index;
					break;
				}
			}
			if (Candidate == INDEX_NONE)
			{
				return Context.Error(TEXT("dependency_cycle"), TEXT("$.steps"),
					TEXT("The step dependency graph contains a cycle."));
			}
			Selected[Candidate] = true;
			Plan.OrderedStepIndices.Add(Candidate);
			for (const int32 Dependent : Dependents[Candidate])
			{
				--InDegree[Dependent];
			}
		}
		return true;
	}
}

bool FHyperAIStudioTypedPlanValidator::ValidateJson(
	const FString& Json,
	const FHyperAIStudioTypedOperationRegistry& Registry,
	FHyperAIStudioValidatedPlan& OutPlan,
	FHyperAIStudioPlanDryRunResult& OutResult)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	OutPlan = FHyperAIStudioValidatedPlan{};
	OutResult = FHyperAIStudioPlanDryRunResult{};
	FParseContext Context{OutResult};

	if (Json.IsEmpty() || Json.Len() > FHyperAIStudioPlanLimits::MaxPlanJsonBytes)
	{
		return Context.Error(TEXT("plan_size"), TEXT("$"), TEXT("The plan payload is empty or exceeds the hard character bound."));
	}
	if (!HasWellFormedUtf16(Json))
	{
		return Context.Error(TEXT("invalid_json"), TEXT("$"), TEXT("The plan payload contains malformed UTF-16."));
	}
	const FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > FHyperAIStudioPlanLimits::MaxPlanJsonBytes)
	{
		return Context.Error(TEXT("plan_size"), TEXT("$"), TEXT("The UTF-8 plan payload is empty or exceeds the hard byte bound."));
	}
	FString ShapeCode;
	FString ShapeMessage;
	FJsonShapeScanner ShapeScanner(Json);
	if (!ShapeScanner.Scan(ShapeCode, ShapeMessage))
	{
		return Context.Error(ShapeCode, TEXT("$"), ShapeMessage);
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(
		TJsonReaderFactory<>::Create(Json), Root, FJsonSerializer::EFlags::StoreNumbersAsStrings) || !Root.IsValid())
	{
		return Context.Error(TEXT("invalid_json"), TEXT("$"), TEXT("A single valid JSON object is required."));
	}
	if (!HasOnlyFields(Root,
		{TEXT("authorization_token"), TEXT("budget"), TEXT("capability_hash"), TEXT("dry_run"), TEXT("expected_plan_hash"), TEXT("operation_id"), TEXT("schema"), TEXT("steps")},
		TEXT("$"), Context))
	{
		return false;
	}

	FString Schema;
	if (!ReadRequiredString(Root, TEXT("schema"), TEXT("$"), Schema, Context)
		|| Schema != FHyperAIStudioValidatedPlan::SchemaVersion)
	{
		return Context.Error(TEXT("unsupported_schema"), TEXT("$.schema"), TEXT("Only hyperai.plan.v1 is accepted."));
	}
	if (!ReadRequiredBool(Root, TEXT("dry_run"), TEXT("$"), OutPlan.bDryRun, Context))
	{
		return false;
	}
	if (Root->HasField(TEXT("operation_id")))
	{
		if (!Root->TryGetStringField(TEXT("operation_id"), OutPlan.OperationId)
			|| (!OutPlan.OperationId.IsEmpty() && !FHyperAIStudioOperationJournal::IsValidOperationId(OutPlan.OperationId)))
		{
			return Context.Error(TEXT("invalid_operation_id"), TEXT("$.operation_id"),
				TEXT("operation_id must be empty or 8-128 URL-safe identifier characters."));
		}
	}
	if (!ReadRequiredString(Root, TEXT("capability_hash"), TEXT("$"), OutPlan.CapabilityHash, Context)
		|| !IsSha256Token(OutPlan.CapabilityHash))
	{
		return Context.Error(TEXT("invalid_capability_hash"), TEXT("$.capability_hash"), TEXT("An exact lowercase sha256 token is required."));
	}
	const FString RegistryHash = Registry.ComputeCapabilityHash();
	if (RegistryHash.IsEmpty() || OutPlan.CapabilityHash != RegistryHash)
	{
		return Context.Error(TEXT("capability_mismatch"), TEXT("$.capability_hash"),
			TEXT("The plan is bound to a different typed operation registry."));
	}
	if (Root->HasField(TEXT("expected_plan_hash")))
	{
		if (!Root->TryGetStringField(TEXT("expected_plan_hash"), OutPlan.ExpectedPlanHash)
			|| !IsSha256Token(OutPlan.ExpectedPlanHash))
		{
			return Context.Error(TEXT("invalid_expected_plan_hash"), TEXT("$.expected_plan_hash"),
				TEXT("expected_plan_hash must be an exact lowercase sha256 token."));
		}
	}

	if (Root->HasField(TEXT("authorization_token")))
	{
		if (!Root->TryGetStringField(TEXT("authorization_token"), OutPlan.AuthorizationToken)
			|| !IsValidOpaqueAuthorizationToken(OutPlan.AuthorizationToken))
		{
			return Context.Error(TEXT("invalid_authorization_token"), TEXT("$.authorization_token"),
				TEXT("authorization_token must be an opaque bounded URL-safe bearer token."));
		}
	}

	TSharedPtr<FJsonObject> Budget;
	if (!ReadRequiredObject(Root, TEXT("budget"), TEXT("$"), Budget, Context)
		|| !ParseBudget(Budget, OutPlan.Budget, Context))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!ReadRequiredArray(Root, TEXT("steps"), TEXT("$"), Steps, Context)
		|| Steps->IsEmpty() || Steps->Num() > FHyperAIStudioPlanLimits::MaxSteps
		|| Steps->Num() > OutPlan.Budget.MaxSteps)
	{
		return Context.Error(TEXT("step_limit"), TEXT("$.steps"), TEXT("The plan must contain 1..max_steps bounded steps."));
	}

	int64 TotalNativeOperations = 0;
	int64 TotalGameThreadMs = 0;
	int64 TotalOutputBytes = 0;
	int32 TotalPreconditions = 0;
	int32 TotalEffects = 0;
	int32 MutationCount = 0;
	int32 CompileTargetCount = 0;
	int32 SaveTargetCount = 0;
	int32 ValidateTargetCount = 0;
	int32 VerifyFreshTargetCount = 0;
	for (int32 Index = 0; Index < Steps->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* StepObject = nullptr;
		if (!(*Steps)[Index].IsValid() || !(*Steps)[Index]->TryGetObject(StepObject)
			|| !StepObject || !StepObject->IsValid())
		{
			return Context.Error(TEXT("step_object"), FString::Printf(TEXT("$.steps[%d]"), Index), TEXT("Each step must be an object."));
		}
		FHyperAIStudioPlanStep Step;
		if (!ParseStep(*StepObject, Index, Registry, Step, Context))
		{
			return false;
		}
		if (OutPlan.bDryRun && !Registry.Find(Step.OperationType)->bSupportsDryRun)
		{
			return Context.Error(TEXT("dry_run_unsupported"), FString::Printf(TEXT("$.steps[%d].operation"), Index),
				TEXT("The typed operation does not support dry-run validation."));
		}
		TotalNativeOperations += Step.Budget.MaxNativeOperations;
		TotalGameThreadMs += Step.Budget.MaxGameThreadMs;
		TotalOutputBytes += Step.Budget.MaxOutputBytes;
		TotalPreconditions += Step.Preconditions.Num();
		TotalEffects += Step.Effects.Num();
		if (Step.Safety != EHyperAIStudioPlanSafety::Read)
		{
			++MutationCount;
			OutPlan.bHasMutation = true;
		}
		OutPlan.bHasDestructive |= Step.Safety == EHyperAIStudioPlanSafety::Destructive;
		OutPlan.bHasExternalEffect |= Step.Safety == EHyperAIStudioPlanSafety::ExternalEffect;
		for (const FHyperAIStudioPlanEffect& Effect : Step.Effects)
		{
			OutPlan.bHasDestructive |= Effect.Kind == EHyperAIStudioPlanEffectKind::ObjectDeleted;
			OutPlan.bHasExternalEffect |= Effect.Kind == EHyperAIStudioPlanEffectKind::RuntimeExternalEffect;
		}
		CompileTargetCount += Step.bCompileOnce ? 1 : 0;
		SaveTargetCount += Step.bSaveOnce ? 1 : 0;
		ValidateTargetCount += Step.bValidateOnce ? 1 : 0;
		VerifyFreshTargetCount += Step.bVerifyFreshOnce ? 1 : 0;
		if (SafetyRank(Step.Safety) > SafetyRank(OutPlan.MaximumSafety))
		{
			OutPlan.MaximumSafety = Step.Safety;
		}
		OutPlan.Steps.Add(MoveTemp(Step));
	}
	TotalNativeOperations += static_cast<int64>(CompileTargetCount) * CompileNativeOperationsPerTarget
		+ static_cast<int64>(ValidateTargetCount) * ValidateNativeOperationsPerTarget
		+ static_cast<int64>(SaveTargetCount) * SaveNativeOperationsPerTarget
		+ static_cast<int64>(VerifyFreshTargetCount) * VerifyFreshNativeOperationsPerTarget;
	TotalGameThreadMs += static_cast<int64>(CompileTargetCount) * OutPlan.FinalizerBudgets.CompileGameThreadMs
		+ static_cast<int64>(ValidateTargetCount) * OutPlan.FinalizerBudgets.ValidateGameThreadMs
		+ static_cast<int64>(SaveTargetCount) * OutPlan.FinalizerBudgets.SaveGameThreadMs
		+ static_cast<int64>(VerifyFreshTargetCount) * OutPlan.FinalizerBudgets.VerifyFreshGameThreadMs;
	TotalOutputBytes += static_cast<int64>(CompileTargetCount) * CompileOutputBytesPerTarget
		+ static_cast<int64>(ValidateTargetCount) * ValidateOutputBytesPerTarget
		+ static_cast<int64>(SaveTargetCount) * SaveOutputBytesPerTarget
		+ static_cast<int64>(VerifyFreshTargetCount) * VerifyFreshOutputBytesPerTarget;
	if (TotalPreconditions > FHyperAIStudioPlanLimits::MaxTotalPreconditions
		|| TotalEffects > FHyperAIStudioPlanLimits::MaxTotalEffects)
	{
		return Context.Error(TEXT("declaration_limit"), TEXT("$.steps"), TEXT("The aggregate precondition/effect bound was exceeded."));
	}
	if (MutationCount > OutPlan.Budget.MaxMutations
		|| TotalNativeOperations > OutPlan.Budget.MaxNativeOperations
		|| TotalGameThreadMs > OutPlan.Budget.MaxGameThreadMs
		|| TotalOutputBytes > OutPlan.Budget.MaxOutputBytes)
	{
		return Context.Error(TEXT("aggregate_budget"), TEXT("$.budget"),
			TEXT("The sum of step envelopes exceeds a declared plan budget."));
	}
	if (OutPlan.bHasMutation && OutPlan.OperationId.IsEmpty())
	{
		return Context.Error(TEXT("operation_id_required"), TEXT("$.operation_id"),
			TEXT("Every edit, destructive, or external-effect plan requires a client operation_id."));
	}
	const bool bRequiresAuthorization = OutPlan.bHasDestructive || OutPlan.bHasExternalEffect;
	if (!OutPlan.bDryRun && bRequiresAuthorization && OutPlan.AuthorizationToken.IsEmpty())
	{
		return Context.Error(TEXT("authorization_required"), TEXT("$.authorization_token"),
			TEXT("Execute-mode destructive or external-effect plans require a server-issued single-use authorization token."));
	}
	if ((OutPlan.bDryRun || !bRequiresAuthorization) && !OutPlan.AuthorizationToken.IsEmpty())
	{
		return Context.Error(TEXT("authorization_not_applicable"), TEXT("$.authorization_token"),
			TEXT("Bearer authorization tokens are accepted only by execute-mode destructive/external plans."));
	}
	if (!BuildStableTopologicalOrder(OutPlan, Context))
	{
		return false;
	}

	OutPlan.EffectFingerprint = ComputeEffectFingerprint(OutPlan);
	if (!IsSha256Token(OutPlan.EffectFingerprint))
	{
		return Context.Error(TEXT("hash_failure"), TEXT("$"), TEXT("The platform could not compute the declared-effect fingerprint."));
	}
	OutPlan.AuthorizationPlanHash = ComputeAuthorizationPlanHash(OutPlan);
	OutPlan.PlanHash = ComputePlanHash(OutPlan);
	if (!IsSha256Token(OutPlan.PlanHash))
	{
		return Context.Error(TEXT("hash_failure"), TEXT("$"), TEXT("The platform could not compute the canonical SHA-256 plan hash."));
	}
	if (!OutPlan.ExpectedPlanHash.IsEmpty() && OutPlan.ExpectedPlanHash != OutPlan.PlanHash)
	{
		return Context.Error(TEXT("plan_hash_mismatch"), TEXT("$.expected_plan_hash"),
			TEXT("The client-supplied expected hash does not match the canonical plan."));
	}
	OutPlan.bValidated = true;
	FString DryRunError;
	if (!BuildDryRun(OutPlan, OutResult, DryRunError))
	{
		return Context.Error(TEXT("dry_run_failure"), TEXT("$"), DryRunError);
	}
	return true;
}

FString FHyperAIStudioTypedPlanValidator::ComputeAuthorizationPlanHash(const FHyperAIStudioValidatedPlan& Plan)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.authorization-plan.v1"));
	AppendToken(Canonical, Plan.CapabilityHash);
	AppendBool(Canonical, Plan.bHasMutation);
	AppendBool(Canonical, Plan.bHasDestructive);
	AppendBool(Canonical, Plan.bHasExternalEffect);
	AppendToken(Canonical, SafetyToString(Plan.MaximumSafety));
	AppendInt(Canonical, Plan.Budget.DeadlineMs);
	AppendInt(Canonical, Plan.Budget.MaxSteps);
	AppendInt(Canonical, Plan.Budget.MaxMutations);
	AppendInt(Canonical, Plan.Budget.MaxNativeOperations);
	AppendInt(Canonical, Plan.Budget.MaxGameThreadMs);
	AppendInt(Canonical, Plan.Budget.MaxOutputBytes);
	// Sealed only when raised, so every general plan keeps the hash it had before per-plan budgets existed.
	if (!Plan.FinalizerBudgets.IsDefault())
	{
		AppendToken(Canonical, TEXT("finalizer-budgets"));
		AppendInt(Canonical, Plan.FinalizerBudgets.CompileGameThreadMs);
		AppendInt(Canonical, Plan.FinalizerBudgets.ValidateGameThreadMs);
		AppendInt(Canonical, Plan.FinalizerBudgets.SaveGameThreadMs);
		AppendInt(Canonical, Plan.FinalizerBudgets.VerifyFreshGameThreadMs);
	}
	AppendInt(Canonical, Plan.Steps.Num());

	TArray<const FHyperAIStudioPlanStep*> CanonicalSteps;
	CanonicalSteps.Reserve(Plan.Steps.Num());
	for (const FHyperAIStudioPlanStep& Step : Plan.Steps)
	{
		CanonicalSteps.Add(&Step);
	}
	CanonicalSteps.Sort([](const FHyperAIStudioPlanStep& Left, const FHyperAIStudioPlanStep& Right)
	{
		return Left.StepId < Right.StepId;
	});
	for (const FHyperAIStudioPlanStep* StepPtr : CanonicalSteps)
	{
		const FHyperAIStudioPlanStep& Step = *StepPtr;
		AppendToken(Canonical, Step.StepId);
		AppendToken(Canonical, Step.OperationType);
		AppendToken(Canonical, SafetyToString(Step.Safety));
		TArray<FString> Dependencies = Step.DependsOn;
		Dependencies.Sort();
		AppendInt(Canonical, Dependencies.Num());
		for (const FString& Item : Dependencies) { AppendToken(Canonical, Item); }

		TArray<FString> ArgumentNames;
		Step.Arguments.GetKeys(ArgumentNames);
		ArgumentNames.Sort();
		AppendInt(Canonical, ArgumentNames.Num());
		for (const FString& Name : ArgumentNames)
		{
			const FHyperAIStudioPlanValue& Value = Step.Arguments[Name];
			AppendToken(Canonical, Name);
			AppendToken(Canonical, ValueTypeToString(Value.Type));
			switch (Value.Type)
			{
			case EHyperAIStudioPlanValueType::String:
				AppendToken(Canonical, Value.StringValue);
				break;
			case EHyperAIStudioPlanValueType::Integer:
				AppendInt(Canonical, Value.IntegerValue);
				break;
			case EHyperAIStudioPlanValueType::Boolean:
				AppendBool(Canonical, Value.bBooleanValue);
				break;
			case EHyperAIStudioPlanValueType::StringArray:
				AppendInt(Canonical, Value.StringArrayValue.Num());
				for (const FString& Item : Value.StringArrayValue) { AppendToken(Canonical, Item); }
				break;
			default:
				AppendToken(Canonical, TEXT("unknown"));
				break;
			}
		}

		TArray<FString> Preconditions;
		for (const FHyperAIStudioPlanPrecondition& Item : Step.Preconditions)
		{
			FString Encoded;
			AppendToken(Encoded, PreconditionKindToString(Item.Kind));
			AppendToken(Encoded, Item.Target);
			AppendToken(Encoded, Item.Field);
			AppendToken(Encoded, Item.Expected);
			Preconditions.Add(MoveTemp(Encoded));
		}
		Preconditions.Sort();
		AppendInt(Canonical, Preconditions.Num());
		for (const FString& Item : Preconditions) { AppendToken(Canonical, Item); }

		TArray<FString> Effects;
		for (const FHyperAIStudioPlanEffect& Item : Step.Effects)
		{
			FString Encoded;
			AppendToken(Encoded, EffectKindToString(Item.Kind));
			AppendToken(Encoded, Item.Target);
			AppendToken(Encoded, Item.ValidatorId);
			AppendToken(Encoded, Item.Expected);
			Effects.Add(MoveTemp(Encoded));
		}
		Effects.Sort();
		AppendInt(Canonical, Effects.Num());
		for (const FString& Item : Effects) { AppendToken(Canonical, Item); }
		AppendInt(Canonical, Step.Budget.MaxNativeOperations);
		AppendInt(Canonical, Step.Budget.MaxGameThreadMs);
		AppendInt(Canonical, Step.Budget.MaxOutputBytes);
	}
	return HashUtf8Sha256(Canonical);
}

FString FHyperAIStudioTypedPlanValidator::ComputePlanHash(const FHyperAIStudioValidatedPlan& Plan)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	FString Canonical;
	AppendToken(Canonical, FHyperAIStudioValidatedPlan::SchemaVersion);
	AppendToken(Canonical, ComputeAuthorizationPlanHash(Plan));
	return HashUtf8Sha256(Canonical);
}

FString FHyperAIStudioTypedPlanValidator::ComputeEffectFingerprint(const FHyperAIStudioValidatedPlan& Plan)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.effect-contract.v1"));
	TArray<const FHyperAIStudioPlanStep*> Steps;
	Steps.Reserve(Plan.Steps.Num());
	for (const FHyperAIStudioPlanStep& Step : Plan.Steps)
	{
		Steps.Add(&Step);
	}
	Steps.Sort([](const FHyperAIStudioPlanStep& Left, const FHyperAIStudioPlanStep& Right)
	{
		return Left.StepId < Right.StepId;
	});
	AppendInt(Canonical, Steps.Num());
	for (const FHyperAIStudioPlanStep* Step : Steps)
	{
		AppendToken(Canonical, Step->StepId);
		AppendToken(Canonical, Step->OperationType);
		TArray<FString> Effects;
		Effects.Reserve(Step->Effects.Num());
		for (const FHyperAIStudioPlanEffect& Effect : Step->Effects)
		{
			FString Encoded;
			AppendToken(Encoded, EffectKindToString(Effect.Kind));
			AppendToken(Encoded, Effect.Target);
			AppendToken(Encoded, Effect.ValidatorId);
			AppendToken(Encoded, Effect.Expected);
			Effects.Add(MoveTemp(Encoded));
		}
		Effects.Sort();
		AppendInt(Canonical, Effects.Num());
		for (const FString& Effect : Effects)
		{
			AppendToken(Canonical, Effect);
		}
	}
	return HashUtf8Sha256(Canonical);
}

const TCHAR* FHyperAIStudioTypedPlanValidator::FreshValidatorId()
{
	return HyperAIStudio::TypedPlan::Private::FreshVerificationValidatorId;
}

const TCHAR* FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint()
{
	return HyperAIStudio::TypedPlan::Private::FreshVerificationValidatorFingerprint;
}

const TCHAR* FHyperAIStudioTypedPlanValidator::RollbackValidatorId()
{
	return HyperAIStudio::TypedPlan::Private::RollbackVerificationValidatorId;
}

const TCHAR* FHyperAIStudioTypedPlanValidator::RollbackValidatorFingerprint()
{
	return HyperAIStudio::TypedPlan::Private::RollbackVerificationValidatorFingerprint;
}

bool FHyperAIStudioTypedPlanValidator::BuildDryRun(
	const FHyperAIStudioValidatedPlan& Plan,
	FHyperAIStudioPlanDryRunResult& OutResult,
	FString& OutError)
{
	OutResult = FHyperAIStudioPlanDryRunResult{};
	OutError.Reset();
	if (!Plan.bValidated || Plan.OrderedStepIndices.Num() != Plan.Steps.Num())
	{
		OutError = TEXT("Only a validated plan with a complete dependency order can produce a dry-run result.");
		return false;
	}
	OutResult.bValid = true;
	OutResult.bWouldMutate = Plan.bHasMutation;
	OutResult.bRequiresOperationId = Plan.bHasMutation;
	OutResult.bRequiresDestructiveAuthorization = Plan.bHasDestructive;
	OutResult.bRequiresExternalEffectAuthorization = Plan.bHasExternalEffect;
	OutResult.OperationId = Plan.OperationId;
	OutResult.PlanHash = Plan.PlanHash;
	OutResult.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
	OutResult.EffectFingerprint = Plan.EffectFingerprint;
	OutResult.CapabilityHash = Plan.CapabilityHash;
	OutResult.MaximumSafety = Plan.MaximumSafety;
	OutResult.StepCount = Plan.Steps.Num();

	int32 CompileTargets = 0;
	int32 SaveTargets = 0;
	int32 ValidateTargets = 0;
	int32 VerifyFreshTargets = 0;
	for (const int32 StepIndex : Plan.OrderedStepIndices)
	{
		if (!Plan.Steps.IsValidIndex(StepIndex))
		{
			OutError = TEXT("The dependency order contains an invalid step index.");
			return false;
		}
		const FHyperAIStudioPlanStep& Step = Plan.Steps[StepIndex];
		OutResult.OrderedStepIds.Add(Step.StepId);
		OutResult.Schedule.Add({EHyperAIStudioPlanActionKind::Step, Step.StepId, Step.OperationType, FString(), Step.Safety, Step.Budget});
		OutResult.PlannedNativeOperationBudget += Step.Budget.MaxNativeOperations;
		OutResult.PlannedGameThreadBudgetMs += Step.Budget.MaxGameThreadMs;
		OutResult.PlannedOutputBudgetBytes += Step.Budget.MaxOutputBytes;
		if (Step.Safety != EHyperAIStudioPlanSafety::Read)
		{
			++OutResult.MutationStepCount;
		}
		CompileTargets += Step.bCompileOnce ? 1 : 0;
		SaveTargets += Step.bSaveOnce ? 1 : 0;
		ValidateTargets += Step.bValidateOnce ? 1 : 0;
		VerifyFreshTargets += Step.bVerifyFreshOnce ? 1 : 0;
	}
	if (CompileTargets > 0)
	{
		const FHyperAIStudioPlanStepBudget Budget{
			CompileTargets * HyperAIStudio::TypedPlan::Private::CompileNativeOperationsPerTarget,
			CompileTargets * Plan.FinalizerBudgets.CompileGameThreadMs,
			CompileTargets * HyperAIStudio::TypedPlan::Private::CompileOutputBytesPerTarget};
		OutResult.Schedule.Add({EHyperAIStudioPlanActionKind::CompileOnce, FString(), FString(), FString(), EHyperAIStudioPlanSafety::Edit, Budget});
		OutResult.PlannedNativeOperationBudget += Budget.MaxNativeOperations;
		OutResult.PlannedGameThreadBudgetMs += Budget.MaxGameThreadMs;
		OutResult.PlannedOutputBudgetBytes += Budget.MaxOutputBytes;
	}
	if (ValidateTargets > 0)
	{
		const FHyperAIStudioPlanStepBudget Budget{
			ValidateTargets * HyperAIStudio::TypedPlan::Private::ValidateNativeOperationsPerTarget,
			ValidateTargets * Plan.FinalizerBudgets.ValidateGameThreadMs,
			ValidateTargets * HyperAIStudio::TypedPlan::Private::ValidateOutputBytesPerTarget};
		OutResult.Schedule.Add({EHyperAIStudioPlanActionKind::ValidateOnce, FString(), FString(), FString(), EHyperAIStudioPlanSafety::Read, Budget});
		OutResult.PlannedNativeOperationBudget += Budget.MaxNativeOperations;
		OutResult.PlannedGameThreadBudgetMs += Budget.MaxGameThreadMs;
		OutResult.PlannedOutputBudgetBytes += Budget.MaxOutputBytes;
	}
	if (SaveTargets > 0)
	{
		const FHyperAIStudioPlanStepBudget Budget{
			SaveTargets * HyperAIStudio::TypedPlan::Private::SaveNativeOperationsPerTarget,
			SaveTargets * Plan.FinalizerBudgets.SaveGameThreadMs,
			SaveTargets * HyperAIStudio::TypedPlan::Private::SaveOutputBytesPerTarget};
		OutResult.Schedule.Add({EHyperAIStudioPlanActionKind::SaveOnce, FString(), FString(), FString(), EHyperAIStudioPlanSafety::Edit, Budget});
		OutResult.PlannedNativeOperationBudget += Budget.MaxNativeOperations;
		OutResult.PlannedGameThreadBudgetMs += Budget.MaxGameThreadMs;
		OutResult.PlannedOutputBudgetBytes += Budget.MaxOutputBytes;
	}
	if (VerifyFreshTargets > 0)
	{
		const FHyperAIStudioPlanStepBudget Budget{
			VerifyFreshTargets * HyperAIStudio::TypedPlan::Private::VerifyFreshNativeOperationsPerTarget,
			VerifyFreshTargets * Plan.FinalizerBudgets.VerifyFreshGameThreadMs,
			VerifyFreshTargets * HyperAIStudio::TypedPlan::Private::VerifyFreshOutputBytesPerTarget};
		OutResult.Schedule.Add({EHyperAIStudioPlanActionKind::VerifyFreshOnce, FString(), FString(), FString(), EHyperAIStudioPlanSafety::Read, Budget});
		OutResult.PlannedNativeOperationBudget += Budget.MaxNativeOperations;
		OutResult.PlannedGameThreadBudgetMs += Budget.MaxGameThreadMs;
		OutResult.PlannedOutputBudgetBytes += Budget.MaxOutputBytes;
	}
	return true;
}

bool FHyperAIStudioTypedPlanValidator::SerializeDryRunJson(
	const FHyperAIStudioPlanDryRunResult& Result,
	FString& OutJson,
	FString& OutError)
{
	OutJson.Reset();
	OutError.Reset();
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), FHyperAIStudioPlanDryRunResult::SchemaVersion);
	Root->SetBoolField(TEXT("valid"), Result.bValid);
	Root->SetBoolField(TEXT("would_mutate"), Result.bWouldMutate);
	Root->SetBoolField(TEXT("requires_operation_id"), Result.bRequiresOperationId);
	Root->SetBoolField(TEXT("requires_destructive_authorization"), Result.bRequiresDestructiveAuthorization);
	Root->SetBoolField(TEXT("requires_external_effect_authorization"), Result.bRequiresExternalEffectAuthorization);
	Root->SetStringField(TEXT("operation_id"), Result.OperationId);
	Root->SetStringField(TEXT("plan_hash"), Result.PlanHash);
	Root->SetStringField(TEXT("authorization_plan_hash"), Result.AuthorizationPlanHash);
	Root->SetStringField(TEXT("effect_fingerprint"), Result.EffectFingerprint);
	Root->SetStringField(TEXT("capability_hash"), Result.CapabilityHash);
	Root->SetStringField(TEXT("maximum_safety"), SafetyToString(Result.MaximumSafety));
	Root->SetNumberField(TEXT("step_count"), Result.StepCount);
	Root->SetNumberField(TEXT("mutation_step_count"), Result.MutationStepCount);
	Root->SetNumberField(TEXT("planned_native_operation_budget"), Result.PlannedNativeOperationBudget);
	Root->SetNumberField(TEXT("planned_game_thread_budget_ms"), Result.PlannedGameThreadBudgetMs);
	Root->SetNumberField(TEXT("planned_output_budget_bytes"), Result.PlannedOutputBudgetBytes);

	TArray<TSharedPtr<FJsonValue>> Ordered;
	for (const FString& StepId : Result.OrderedStepIds)
	{
		Ordered.Add(MakeShared<FJsonValueString>(StepId));
	}
	Root->SetArrayField(TEXT("ordered_step_ids"), MoveTemp(Ordered));
	TArray<TSharedPtr<FJsonValue>> ScheduleValues;
	for (const FHyperAIStudioPlanScheduledAction& Action : Result.Schedule)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("kind"), ActionKindToString(Action.Kind));
		Item->SetStringField(TEXT("step_id"), Action.StepId);
		Item->SetStringField(TEXT("operation"), Action.OperationType);
		Item->SetStringField(TEXT("safety"), SafetyToString(Action.Safety));
		Item->SetNumberField(TEXT("max_native_operations"), Action.Budget.MaxNativeOperations);
		Item->SetNumberField(TEXT("max_game_thread_ms"), Action.Budget.MaxGameThreadMs);
		Item->SetNumberField(TEXT("max_output_bytes"), Action.Budget.MaxOutputBytes);
		ScheduleValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("schedule"), MoveTemp(ScheduleValues));
	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	for (const FHyperAIStudioPlanDiagnostic& Diagnostic : Result.Diagnostics)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("code"), Diagnostic.Code);
		Item->SetStringField(TEXT("path"), Diagnostic.Path);
		Item->SetStringField(TEXT("message"), Diagnostic.Message);
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("diagnostics"), MoveTemp(DiagnosticValues));

	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Could not serialize the bounded plan-validation result.");
		return false;
	}
	return true;
}

FString FHyperAIStudioTypedPlanValidator::SafetyToString(const EHyperAIStudioPlanSafety Safety)
{
	switch (Safety)
	{
	case EHyperAIStudioPlanSafety::Read: return TEXT("read");
	case EHyperAIStudioPlanSafety::Edit: return TEXT("edit");
	case EHyperAIStudioPlanSafety::Destructive: return TEXT("destructive");
	case EHyperAIStudioPlanSafety::ExternalEffect: return TEXT("external_effect");
	default: return TEXT("unknown");
	}
}

FString FHyperAIStudioTypedPlanValidator::ActionKindToString(const EHyperAIStudioPlanActionKind Kind)
{
	switch (Kind)
	{
	case EHyperAIStudioPlanActionKind::Step: return TEXT("step");
	case EHyperAIStudioPlanActionKind::CompileOnce: return TEXT("compile_once");
	case EHyperAIStudioPlanActionKind::ValidateOnce: return TEXT("validate_once");
	case EHyperAIStudioPlanActionKind::SaveOnce: return TEXT("save_once");
	case EHyperAIStudioPlanActionKind::VerifyFreshOnce: return TEXT("verify_fresh_once");
	default: return TEXT("unknown");
	}
}

EHyperAIStudioPlanJournalBegin FHyperAIStudioOperationJournalPlanAdapter::Begin(
	const FString& OperationId,
	const FString& PlanHash,
	const FString& CapabilityHash,
	FString& OutError)
{
	FHyperAIStudioOperationRecord Record;
	const EHyperAIStudioOperationBeginResult Result = Journal.BeginOperation(
		OperationId, PlanHash, CapabilityHash, Record, OutError);
	switch (Result)
	{
	case EHyperAIStudioOperationBeginResult::Created:
		return EHyperAIStudioPlanJournalBegin::ProceedNew;
	case EHyperAIStudioOperationBeginResult::ReplayCompleted:
		return EHyperAIStudioPlanJournalBegin::ReplayCompleted;
	case EHyperAIStudioOperationBeginResult::Conflict:
		return EHyperAIStudioPlanJournalBegin::Conflict;
	case EHyperAIStudioOperationBeginResult::Existing:
		switch (Record.State)
		{
		case EHyperAIStudioOperationState::OutcomeUnknown:
			return EHyperAIStudioPlanJournalBegin::OutcomeUnknown;
		case EHyperAIStudioOperationState::Queued:
		case EHyperAIStudioOperationState::Running:
		case EHyperAIStudioOperationState::CommitStarted:
		case EHyperAIStudioOperationState::CancelRequested:
			return EHyperAIStudioPlanJournalBegin::AlreadyInProgress;
		case EHyperAIStudioOperationState::Completed:
			// BeginOperation returns ReplayCompleted only after validating current bound evidence.
			return EHyperAIStudioPlanJournalBegin::ExistingTerminal;
		default:
			return EHyperAIStudioPlanJournalBegin::ExistingTerminal;
		}
	default:
		return EHyperAIStudioPlanJournalBegin::Unavailable;
	}
}

bool FHyperAIStudioOperationJournalPlanAdapter::Transition(
	const FString& OperationId,
	const EHyperAIStudioPlanJournalTransition TransitionKind,
	const FHyperAIStudioPlanOutcomeEvidence& Evidence,
	FString& OutError)
{
	const TOptional<FHyperAIStudioOperationRecord> Existing = Journal.Find(OperationId);
	if (!Existing.IsSet())
	{
		OutError = TEXT("The operation journal does not contain this operation_id.");
		return false;
	}

	EHyperAIStudioOperationState Expected = Existing->State;
	EHyperAIStudioOperationState NewState = Existing->State;
	EHyperAIStudioRollbackState Rollback = EHyperAIStudioRollbackState::NotNeeded;
	FHyperAIStudioOperationEvidence TerminalEvidence;
	bool bRetrySafe = false;
	bool bPartialCommit = false;
	FString StatusCode;
	switch (TransitionKind)
	{
	case EHyperAIStudioPlanJournalTransition::Running:
		Expected = EHyperAIStudioOperationState::Queued;
		NewState = EHyperAIStudioOperationState::Running;
		StatusCode = TEXT("running");
		break;
	case EHyperAIStudioPlanJournalTransition::CommitStarted:
		Expected = EHyperAIStudioOperationState::Running;
		NewState = EHyperAIStudioOperationState::CommitStarted;
		StatusCode = TEXT("commit_started");
		break;
	case EHyperAIStudioPlanJournalTransition::Completed:
		if (!HyperAIStudio::TypedPlan::Private::HasFreshPostconditionEvidence(Evidence)
			|| Evidence.ValidatorReceipt.ValidatorId != FHyperAIStudioTypedPlanValidator::FreshValidatorId()
			|| Evidence.ValidatorReceipt.ApprovedValidatorFingerprint != FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint())
		{
			OutError = TEXT("Completed mutation requires trusted fresh persisted postcondition evidence.");
			return false;
		}
		Expected = EHyperAIStudioOperationState::CommitStarted;
		StatusCode = TEXT("fresh_verified");
		NewState = EHyperAIStudioOperationState::Completed;
		TerminalEvidence.Version = FHyperAIStudioOperationEvidence::CurrentVersion;
		break;
	case EHyperAIStudioPlanJournalTransition::FailedPreCommit:
		if (Existing->State != EHyperAIStudioOperationState::Queued
			&& Existing->State != EHyperAIStudioOperationState::Running)
		{
			OutError = TEXT("A pre-commit failure can transition only queued or running work.");
			return false;
		}
		Expected = Existing->State;
		NewState = EHyperAIStudioOperationState::Failed;
		bRetrySafe = true;
		StatusCode = TEXT("failed_precommit");
		break;
	case EHyperAIStudioPlanJournalTransition::RolledBack:
		if (Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Complete
			|| !Evidence.ValidatorReceipt.IsPresent()
			|| Evidence.ValidatorReceipt.ValidatorId != FHyperAIStudioTypedPlanValidator::RollbackValidatorId()
			|| Evidence.ValidatorReceipt.ApprovedValidatorFingerprint != FHyperAIStudioTypedPlanValidator::RollbackValidatorFingerprint())
		{
			OutError = TEXT("Complete rollback requires trusted whole-plan validator/postcondition evidence and proof of no persistent or external residue.");
			return false;
		}
		Expected = EHyperAIStudioOperationState::CommitStarted;
		NewState = EHyperAIStudioOperationState::RolledBack;
		Rollback = EHyperAIStudioRollbackState::Complete;
		bRetrySafe = true;
		StatusCode = TEXT("rolled_back");
		TerminalEvidence.Version = FHyperAIStudioOperationEvidence::CurrentVersion;
		break;
	case EHyperAIStudioPlanJournalTransition::Partial:
		if (Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Partial
			&& Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Unsupported
			&& Evidence.RollbackState != EHyperAIStudioPlanRollbackState::Unknown)
		{
			OutError = TEXT("A partial result must carry an exact partial, unsupported, or unknown rollback state.");
			return false;
		}
		Expected = EHyperAIStudioOperationState::CommitStarted;
		NewState = EHyperAIStudioOperationState::Partial;
		Rollback = Evidence.RollbackState == EHyperAIStudioPlanRollbackState::Partial
			? EHyperAIStudioRollbackState::Partial
			: (Evidence.RollbackState == EHyperAIStudioPlanRollbackState::Unsupported
				? EHyperAIStudioRollbackState::Unsupported
				: EHyperAIStudioRollbackState::Unknown);
		bPartialCommit = true;
		StatusCode = TEXT("partial");
		break;
	case EHyperAIStudioPlanJournalTransition::OutcomeUnknown:
		if (Existing->State == EHyperAIStudioOperationState::Running)
		{
			FHyperAIStudioOperationRecord BarrierRecord;
			if (!Journal.Transition(
				OperationId,
				EHyperAIStudioOperationState::Running,
				EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioRollbackState::NotNeeded,
				false,
				false,
				TEXT("contract_effect_before_barrier"),
				BarrierRecord,
				OutError))
			{
				return false;
			}
		}
		Expected = EHyperAIStudioOperationState::CommitStarted;
		NewState = EHyperAIStudioOperationState::OutcomeUnknown;
		Rollback = EHyperAIStudioRollbackState::Unknown;
		bPartialCommit = true;
		StatusCode = TEXT("outcome_unknown");
		break;
	default:
		OutError = TEXT("Unknown plan-journal transition.");
		return false;
	}
	if (TerminalEvidence.Version == FHyperAIStudioOperationEvidence::CurrentVersion)
	{
		const FHyperAIStudioPlanValidatorReceipt& Receipt = Evidence.ValidatorReceipt;
		TerminalEvidence.CanonicalProjectId = Receipt.CanonicalProjectId;
		TerminalEvidence.OperationId = Receipt.OperationId;
		TerminalEvidence.PlanHash = Receipt.PlanHash;
		TerminalEvidence.CapabilityHash = Receipt.CapabilityHash;
		TerminalEvidence.EffectFingerprint = Receipt.EffectFingerprint;
		TerminalEvidence.ActionNonce = Receipt.ActionNonce;
		TerminalEvidence.ValidatorId = Receipt.ValidatorId;
		TerminalEvidence.ApprovedValidatorFingerprint = Receipt.ApprovedValidatorFingerprint;
		TerminalEvidence.PostconditionHash = Receipt.PostconditionHash;
		TerminalEvidence.ReceiptFingerprint = Receipt.ReceiptFingerprint;
		TerminalEvidence.IssuedUtcMs = Receipt.IssuedUtcMs;
		TerminalEvidence.ExpiresUtcMs = Receipt.ExpiresUtcMs;
	}

	FHyperAIStudioOperationRecord Updated;
	return Journal.Transition(
		OperationId,
		Expected,
		NewState,
		Rollback,
		bRetrySafe,
		bPartialCommit,
		StatusCode,
		TerminalEvidence,
		Updated,
		OutError);
}

bool FHyperAIStudioOperationJournalPlanAdapter::CertifyNoEffect(
	const FString& OperationId,
	const FString& EffectFingerprint,
	const FString& ActionNonce,
	FString& OutError)
{
	FHyperAIStudioOperationRecord Updated;
	return Journal.TransitionCertifiedNoEffect(
		OperationId, EffectFingerprint, ActionNonce, Updated, OutError);
}

void FHyperAIStudioSerialPlanCoordinator::Reset()
{
	State = EHyperAIStudioPlanCoordinatorState::Idle;
	ActivePlan = FHyperAIStudioValidatedPlan{};
	Schedule.Reset();
	ActiveJournalGate = nullptr;
	ActiveAuthorizationGate = nullptr;
	ActiveValidatorReceiptGate = nullptr;
	ActiveClock = nullptr;
	ActiveCanonicalProjectId.Reset();
	ActiveActionNonce.Reset();
	NextActionIndex = 0;
	DeadlineMonotonicMs = 0;
	UsedNativeOperations = 0;
	UsedGameThreadMs = 0;
	UsedOutputBytes = 0;
	bCommitStarted = false;
	bKnownCommittedEffect = false;
	bFreshVerificationComplete = false;
	FreshVerificationEvidence = FHyperAIStudioPlanOutcomeEvidence{};
}

bool FHyperAIStudioSerialPlanCoordinator::ValidateExecutionArtifact(
	const FHyperAIStudioTypedOperationRegistry& Registry,
	FString& OutError) const
{
	using namespace HyperAIStudio::TypedPlan::Private;
	if (!ActivePlan.bValidated || ActivePlan.Steps.IsEmpty()
		|| ActivePlan.Steps.Num() > FHyperAIStudioPlanLimits::MaxSteps
		|| ActivePlan.CapabilityHash != Registry.ComputeCapabilityHash()
		|| !IsSha256Token(ActivePlan.CapabilityHash))
	{
		OutError = TEXT("The execution artifact is not bound to the active closed operation registry.");
		return false;
	}

	bool bHasMutation = false;
	bool bHasDestructive = false;
	bool bHasExternal = false;
	EHyperAIStudioPlanSafety MaximumSafety = EHyperAIStudioPlanSafety::Read;
	for (const FHyperAIStudioPlanStep& Step : ActivePlan.Steps)
	{
		const FHyperAIStudioTypedOperationMetadata* Metadata = Registry.Find(Step.OperationType);
		if (!Metadata || Step.Safety != Metadata->Safety
			|| Step.bCompileOnce != Metadata->bCompileOnce
			|| Step.bSaveOnce != Metadata->bSaveOnce
			|| Step.bValidateOnce != Metadata->bValidateOnce
			|| Step.bVerifyFreshOnce != Metadata->bVerifyFreshOnce
			|| Step.Budget.MaxNativeOperations < Metadata->EstimatedNativeOperations
			|| Step.Budget.MaxGameThreadMs < Metadata->EstimatedGameThreadMs)
		{
			OutError = TEXT("A validated step was altered or no longer matches its registry metadata.");
			return false;
		}

		for (const TPair<FString, FHyperAIStudioPlanValue>& Argument : Step.Arguments)
		{
			const FHyperAIStudioPlanArgumentSpec* Spec = Metadata->Arguments.FindByPredicate(
				[&Argument](const FHyperAIStudioPlanArgumentSpec& Item) { return Item.Name == Argument.Key; });
			if (!Spec || Argument.Value.Type != Spec->Type)
			{
				OutError = TEXT("A typed argument was altered after validation.");
				return false;
			}
			if (Spec->Type == EHyperAIStudioPlanValueType::String
				&& (!IsBoundedText(Argument.Value.StringValue, Spec->MaxStringChars, false)
					|| (!Spec->AllowedStrings.IsEmpty() && !Spec->AllowedStrings.Contains(Argument.Value.StringValue))))
			{
				OutError = TEXT("A string argument no longer satisfies its registry contract.");
				return false;
			}
			if (Spec->Type == EHyperAIStudioPlanValueType::Integer
				&& (Argument.Value.IntegerValue < Spec->MinInteger || Argument.Value.IntegerValue > Spec->MaxInteger))
			{
				OutError = TEXT("An integer argument no longer satisfies its exact registry range.");
				return false;
			}
			if (Spec->Type == EHyperAIStudioPlanValueType::StringArray)
			{
				TSet<FString> Unique;
				if (Argument.Value.StringArrayValue.Num() > Spec->MaxArrayItems)
				{
					OutError = TEXT("A string-array argument exceeds its registry bound.");
					return false;
				}
				for (const FString& Item : Argument.Value.StringArrayValue)
				{
					if (!IsBoundedText(Item, Spec->MaxStringChars, false) || Unique.Contains(Item))
					{
						OutError = TEXT("A string-array argument no longer satisfies its registry contract.");
						return false;
					}
					Unique.Add(Item);
				}
			}
		}
		for (const FHyperAIStudioPlanArgumentSpec& Spec : Metadata->Arguments)
		{
			if (Spec.bRequired && !Step.Arguments.Contains(Spec.Name))
			{
				OutError = TEXT("A required typed argument is absent from the execution artifact.");
				return false;
			}
		}

		if (!ContainsRequiredPreconditions(Step.Preconditions, Metadata->RequiredPreconditions)
			|| !ContainsRequiredEffects(Step.Effects, Metadata->RequiredEffects))
		{
			OutError = TEXT("The execution artifact lost a required condition or effect declaration.");
			return false;
		}
		for (const FHyperAIStudioPlanPrecondition& Condition : Step.Preconditions)
		{
			if (!Metadata->AllowedPreconditions.Contains(Condition.Kind))
			{
				OutError = TEXT("The execution artifact contains a non-allowlisted precondition.");
				return false;
			}
			if (const FHyperAIStudioPlanPreconditionTargetBinding* Binding = Metadata->PreconditionTargetBindings.FindByPredicate(
				[&Condition](const FHyperAIStudioPlanPreconditionTargetBinding& Item) { return Item.Kind == Condition.Kind; }))
			{
				FString ExpectedTarget;
				if (!ResolveBoundTarget(Binding->ArgumentName, Binding->FixedTarget, Step, ExpectedTarget)
					|| Condition.Target != ExpectedTarget)
				{
					OutError = TEXT("A precondition target was altered after validation.");
					return false;
				}
			}
		}
		for (const FHyperAIStudioPlanEffect& Effect : Step.Effects)
		{
			const FHyperAIStudioPlanEffectTargetBinding* Binding = Metadata->EffectTargetBindings.FindByPredicate(
				[&Effect](const FHyperAIStudioPlanEffectTargetBinding& Item) { return Item.Kind == Effect.Kind; });
			FString ExpectedTarget;
			if (!Metadata->AllowedEffects.Contains(Effect.Kind)
				|| Effect.ValidatorId != Metadata->PostconditionValidatorId
				|| !Binding || !ResolveBoundTarget(Binding->ArgumentName, Binding->FixedTarget, Step, ExpectedTarget)
				|| Effect.Target != ExpectedTarget)
			{
				OutError = TEXT("An effect or validator binding was altered after validation.");
				return false;
			}
			bHasDestructive |= Effect.Kind == EHyperAIStudioPlanEffectKind::ObjectDeleted;
			bHasExternal |= Effect.Kind == EHyperAIStudioPlanEffectKind::RuntimeExternalEffect;
		}

		bHasMutation |= Step.Safety != EHyperAIStudioPlanSafety::Read;
		bHasDestructive |= Step.Safety == EHyperAIStudioPlanSafety::Destructive;
		bHasExternal |= Step.Safety == EHyperAIStudioPlanSafety::ExternalEffect;
		if (SafetyRank(Step.Safety) > SafetyRank(MaximumSafety))
		{
			MaximumSafety = Step.Safety;
		}
	}

	if (bHasMutation != ActivePlan.bHasMutation
		|| bHasDestructive != ActivePlan.bHasDestructive
		|| bHasExternal != ActivePlan.bHasExternalEffect
		|| MaximumSafety != ActivePlan.MaximumSafety
		|| (bHasMutation && !FHyperAIStudioOperationJournal::IsValidOperationId(ActivePlan.OperationId)))
	{
		OutError = TEXT("Derived execution safety/idempotency invariants do not match the sealed plan.");
		return false;
	}

	FHyperAIStudioValidatedPlan Reordered = ActivePlan;
	Reordered.OrderedStepIndices.Reset();
	FHyperAIStudioPlanDryRunResult OrderingResult;
	FParseContext OrderingContext{OrderingResult};
	if (!BuildStableTopologicalOrder(Reordered, OrderingContext)
		|| Reordered.OrderedStepIndices != ActivePlan.OrderedStepIndices)
	{
		OutError = TEXT("The sealed dependency order is invalid or was altered.");
		return false;
	}

	const FString AuthorizationHash = FHyperAIStudioTypedPlanValidator::ComputeAuthorizationPlanHash(ActivePlan);
	const FString EffectFingerprint = FHyperAIStudioTypedPlanValidator::ComputeEffectFingerprint(ActivePlan);
	if (!IsSha256Token(AuthorizationHash) || AuthorizationHash != ActivePlan.AuthorizationPlanHash
		|| !IsSha256Token(EffectFingerprint) || EffectFingerprint != ActivePlan.EffectFingerprint
		|| FHyperAIStudioTypedPlanValidator::ComputePlanHash(ActivePlan) != ActivePlan.PlanHash)
	{
		OutError = TEXT("The execution artifact no longer matches its canonical content hashes.");
		return false;
	}

	FHyperAIStudioPlanDryRunResult DryRun;
	if (!FHyperAIStudioTypedPlanValidator::BuildDryRun(ActivePlan, DryRun, OutError)
		|| DryRun.PlannedNativeOperationBudget > ActivePlan.Budget.MaxNativeOperations
		|| DryRun.PlannedGameThreadBudgetMs > ActivePlan.Budget.MaxGameThreadMs
		|| DryRun.PlannedOutputBudgetBytes > ActivePlan.Budget.MaxOutputBytes)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The complete step/finalizer schedule exceeds its sealed execution budgets.");
		}
		return false;
	}
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::ValidateAuthorizationReceipt(
	const FHyperAIStudioPlanAuthorizationRequest& Request,
	const FHyperAIStudioPlanAuthorizationReceipt& Receipt,
	const int64 NowUtcMs,
	FString& OutError) const
{
	using namespace HyperAIStudio::TypedPlan::Private;
	const FHyperAIStudioPlanAuthorizationRequest& Bound = Receipt.BoundRequest;
	if (Bound.Token != Request.Token
		|| Bound.CanonicalProjectId != Request.CanonicalProjectId
		|| Bound.OperationId != Request.OperationId
		|| Bound.AuthorizationPlanHash != Request.AuthorizationPlanHash
		|| Bound.Safety != Request.Safety
		|| (Receipt.State != EHyperAIStudioPlanAuthorizationState::Available
			&& Receipt.State != EHyperAIStudioPlanAuthorizationState::AlreadyConsumed)
		|| !IsBoundedText(Receipt.Nonce, FHyperAIStudioPlanLimits::MaxAuthorizationNonceChars, false)
		|| Receipt.ExpiresUtcMs <= NowUtcMs)
	{
		OutError = TEXT("The server authorization receipt is expired or is not exactly bound to this project, operation, plan and safety tier.");
		return false;
	}
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::ValidateValidatorReceipt(
	const FHyperAIStudioPlanValidatorReceipt& Receipt,
	const TCHAR* ExpectedValidatorId,
	const TCHAR* ExpectedValidatorFingerprint,
	const int64 NowUtcMs,
	FString& OutError) const
{
	using namespace HyperAIStudio::TypedPlan::Private;
	if (!ActiveValidatorReceiptGate
		|| !IsBoundedText(Receipt.ServerReceiptToken, FHyperAIStudioPlanLimits::MaxValidatorReceiptTokenChars, false)
		|| !IsSha256Token(Receipt.ReceiptFingerprint)
		|| Receipt.CanonicalProjectId != ActiveCanonicalProjectId
		|| Receipt.OperationId != ActivePlan.OperationId
		|| Receipt.PlanHash != ActivePlan.PlanHash
		|| Receipt.CapabilityHash != ActivePlan.CapabilityHash
		|| Receipt.EffectFingerprint != ActivePlan.EffectFingerprint
		|| Receipt.ActionNonce != ActiveActionNonce
		|| !IsBoundedText(Receipt.ActionNonce, FHyperAIStudioPlanLimits::MaxActionNonceChars, false)
		|| Receipt.ValidatorId != ExpectedValidatorId
		|| Receipt.ApprovedValidatorFingerprint != ExpectedValidatorFingerprint
		|| !IsSha256Token(Receipt.ApprovedValidatorFingerprint)
		|| !IsSha256Token(Receipt.PostconditionHash)
		|| Receipt.IssuedUtcMs <= 0 || Receipt.IssuedUtcMs > NowUtcMs
		|| Receipt.ExpiresUtcMs <= NowUtcMs || Receipt.ExpiresUtcMs <= Receipt.IssuedUtcMs
		|| Receipt.ExpiresUtcMs - Receipt.IssuedUtcMs > MaxValidatorReceiptLifetimeMs)
	{
		OutError = TEXT("The validator receipt is expired, malformed, or not exactly bound to the active project, operation, plan, effect contract and action nonce.");
		return false;
	}
	if (!ActiveValidatorReceiptGate->Verify(Receipt, NowUtcMs, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The trusted validator-receipt gate rejected the server-issued receipt.");
		}
		return false;
	}
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::Start(
	const FHyperAIStudioValidatedPlan& Plan,
	const FHyperAIStudioTypedOperationRegistry& Registry,
	const FString& CanonicalProjectId,
	IHyperAIStudioPlanJournalGate* JournalGate,
	IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
	IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
	IHyperAIStudioPlanClock* Clock,
	FString& OutError)
{
	Reset();
	OutError.Reset();
	ActivePlan = Plan;
	ActiveJournalGate = JournalGate;
	ActiveAuthorizationGate = AuthorizationGate;
	ActiveValidatorReceiptGate = ValidatorReceiptGate;
	ActiveClock = Clock ? Clock : &HyperAIStudio::TypedPlan::Private::GetSystemPlanClock();
	ActiveCanonicalProjectId = CanonicalProjectId;
	if (!ValidateExecutionArtifact(Registry, OutError))
	{
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}
	if (Plan.bDryRun)
	{
		OutError = TEXT("A dry-run plan is validation-only and cannot enter the execution coordinator.");
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}

	FHyperAIStudioPlanDryRunResult DryRun;
	if (!FHyperAIStudioTypedPlanValidator::BuildDryRun(ActivePlan, DryRun, OutError))
	{
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}
	Schedule = DryRun.Schedule;
	const int64 StartMs = ActiveClock->NowMonotonicMs();
	const int64 DeadlineDelta = static_cast<int64>(ActivePlan.Budget.DeadlineMs);
	DeadlineMonotonicMs = StartMs > TNumericLimits<int64>::Max() - DeadlineDelta
		? TNumericLimits<int64>::Max()
		: StartMs + DeadlineDelta;

	const bool bRequiresAuthorization = ActivePlan.bHasDestructive || ActivePlan.bHasExternalEffect;
	FHyperAIStudioPlanAuthorizationRequest AuthorizationRequest;
	FHyperAIStudioPlanAuthorizationReceipt InspectedAuthorization;
	if (bRequiresAuthorization)
	{
		using namespace HyperAIStudio::TypedPlan::Private;
		if (!ActiveAuthorizationGate
			|| !IsBoundedText(ActiveCanonicalProjectId, FHyperAIStudioPlanLimits::MaxCanonicalProjectIdChars, false))
		{
			OutError = TEXT("Risky execute plans require a trusted authorization gate and bounded canonical project identity.");
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
		AuthorizationRequest = {
			ActivePlan.AuthorizationToken,
			ActiveCanonicalProjectId,
			ActivePlan.OperationId,
			ActivePlan.AuthorizationPlanHash,
			ActivePlan.MaximumSafety};
		const int64 NowUtcMs = ActiveClock->NowUtcMs();
		if (!ActiveAuthorizationGate->Inspect(AuthorizationRequest, NowUtcMs, InspectedAuthorization, OutError)
			|| !ValidateAuthorizationReceipt(AuthorizationRequest, InspectedAuthorization, NowUtcMs, OutError))
		{
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
	}

	if (ActivePlan.bHasMutation)
	{
		using namespace HyperAIStudio::TypedPlan::Private;
		if (!ActiveJournalGate || !ActiveValidatorReceiptGate
			|| !IsBoundedText(ActiveCanonicalProjectId, FHyperAIStudioPlanLimits::MaxCanonicalProjectIdChars, false))
		{
			OutError = TEXT("Mutating/external plans require a durable journal, trusted validator-receipt gate and bounded canonical project identity.");
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
		const EHyperAIStudioPlanJournalBegin BeginResult = ActiveJournalGate->Begin(
			ActivePlan.OperationId, ActivePlan.PlanHash, ActivePlan.CapabilityHash, OutError);
		switch (BeginResult)
		{
		case EHyperAIStudioPlanJournalBegin::ProceedNew:
			if (bRequiresAuthorization)
			{
				if (InspectedAuthorization.State != EHyperAIStudioPlanAuthorizationState::Available)
				{
					OutError = TEXT("The single-use authorization token was already consumed for a non-completed operation.");
					ClosePreCommitAfterFailure(OutError);
					State = EHyperAIStudioPlanCoordinatorState::Failed;
					return false;
				}
			}
			if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::Running, {}, OutError))
			{
				ClosePreCommitAfterFailure(OutError);
				return false;
			}
			if (bRequiresAuthorization)
			{
				FHyperAIStudioPlanAuthorizationReceipt ConsumedAuthorization;
				const int64 NowUtcMs = ActiveClock->NowUtcMs();
				if (!ActiveAuthorizationGate->Consume(
					AuthorizationRequest, NowUtcMs, ConsumedAuthorization, OutError)
					|| !ValidateAuthorizationReceipt(AuthorizationRequest, ConsumedAuthorization, NowUtcMs, OutError)
					|| ConsumedAuthorization.Nonce != InspectedAuthorization.Nonce
					|| ConsumedAuthorization.ExpiresUtcMs != InspectedAuthorization.ExpiresUtcMs
					|| ConsumedAuthorization.State != EHyperAIStudioPlanAuthorizationState::AlreadyConsumed)
				{
					ClosePreCommitAfterFailure(OutError);
					State = EHyperAIStudioPlanCoordinatorState::Failed;
					return false;
				}
			}
			break;
		case EHyperAIStudioPlanJournalBegin::ReplayCompleted:
			Schedule.Reset();
			State = EHyperAIStudioPlanCoordinatorState::ReplayCompleted;
			return true;
		case EHyperAIStudioPlanJournalBegin::OutcomeUnknown:
			State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The operation_id has outcome_unknown and requires evidence-based reconciliation.");
			}
			return false;
		case EHyperAIStudioPlanJournalBegin::Conflict:
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The operation_id is bound to a different plan or capability hash.");
			}
			return false;
		case EHyperAIStudioPlanJournalBegin::AlreadyInProgress:
		case EHyperAIStudioPlanJournalBegin::ExistingTerminal:
		case EHyperAIStudioPlanJournalBegin::Unavailable:
		default:
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The operation_id cannot safely begin or retry.");
			}
			return false;
		}
	}

	if (Schedule.IsEmpty())
	{
		OutError = TEXT("A validated plan produced an empty execution schedule.");
		ClosePreCommitAfterFailure(OutError);
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}
	State = EHyperAIStudioPlanCoordinatorState::Ready;
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::CheckBeforeDispatch(
	const FHyperAIStudioPlanScheduledAction& Action,
	FString& OutError)
{
	if (!ActiveClock || ActiveClock->NowMonotonicMs() >= DeadlineMonotonicMs)
	{
		return FailForRuntimeLimit(TEXT("The execution deadline elapsed before the next typed action."), OutError);
	}
	if (UsedNativeOperations + Action.Budget.MaxNativeOperations > ActivePlan.Budget.MaxNativeOperations
		|| UsedGameThreadMs + Action.Budget.MaxGameThreadMs > ActivePlan.Budget.MaxGameThreadMs
		|| UsedOutputBytes + Action.Budget.MaxOutputBytes > ActivePlan.Budget.MaxOutputBytes)
	{
		return FailForRuntimeLimit(TEXT("The remaining plan budget cannot admit the next typed action."), OutError);
	}
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::AbortBeforeFirstDispatch(
	const FString& Reason,
	FString& OutError)
{
	OutError = Reason.IsEmpty()
		? TEXT("The admitted operation was aborted before its first dispatch.")
		: Reason;
	if (State != EHyperAIStudioPlanCoordinatorState::Ready || NextActionIndex != 0
		|| bCommitStarted || bKnownCommittedEffect)
	{
		OutError = TEXT("Pre-dispatch abort is legal only before the first commit-marked action.");
		return false;
	}
	if (ActivePlan.bHasMutation)
	{
		const FString PrimaryError = OutError;
		FString JournalError;
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, JournalError))
		{
			OutError = PrimaryError + TEXT(" Journal pre-commit closure failed: ") + JournalError;
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
		OutError = PrimaryError;
	}
	State = EHyperAIStudioPlanCoordinatorState::Failed;
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::AcquireNextAction(
	FHyperAIStudioPlanScheduledAction& OutAction,
	FString& OutError)
{
	OutError.Reset();
	if (State != EHyperAIStudioPlanCoordinatorState::Ready)
	{
		OutError = TEXT("The coordinator is not ready for another action; overlapping acquisition is prohibited.");
		return false;
	}
	if (!Schedule.IsValidIndex(NextActionIndex))
	{
		return FinishSuccessfully(OutError);
	}
	const FHyperAIStudioPlanScheduledAction& Candidate = Schedule[NextActionIndex];
	if (!CheckBeforeDispatch(Candidate, OutError))
	{
		return false;
	}
	if (ActivePlan.bHasMutation && !bCommitStarted
		&& Candidate.Kind == EHyperAIStudioPlanActionKind::Step
		&& Candidate.Safety != EHyperAIStudioPlanSafety::Read)
	{
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::CommitStarted, {}, OutError))
		{
			ClosePreCommitAfterFailure(OutError);
			return false;
		}
		bCommitStarted = true;
	}
	OutAction = Candidate;
	ActiveActionNonce = FString(TEXT("action-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	OutAction.ActionNonce = ActiveActionNonce;
	State = EHyperAIStudioPlanCoordinatorState::AwaitingActionResult;
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::AccountUsage(
	const FHyperAIStudioPlanActionResult& Result,
	FString& OutError)
{
	const FHyperAIStudioPlanScheduledAction& Action = Schedule[NextActionIndex];
	if (Result.Usage.NativeOperations < 0 || Result.Usage.GameThreadMs < 0 || Result.Usage.OutputBytes < 0)
	{
		OutError = TEXT("Action usage counters cannot be negative.");
		return false;
	}
	UsedNativeOperations += Result.Usage.NativeOperations;
	UsedGameThreadMs += Result.Usage.GameThreadMs;
	UsedOutputBytes += Result.Usage.OutputBytes;
	if (Result.Usage.NativeOperations > Action.Budget.MaxNativeOperations
		|| Result.Usage.GameThreadMs > Action.Budget.MaxGameThreadMs
		|| Result.Usage.OutputBytes > Action.Budget.MaxOutputBytes
		|| UsedNativeOperations > ActivePlan.Budget.MaxNativeOperations
		|| UsedGameThreadMs > ActivePlan.Budget.MaxGameThreadMs
		|| UsedOutputBytes > ActivePlan.Budget.MaxOutputBytes)
	{
		OutError = TEXT("The action exceeded its sealed action or aggregate runtime budget.");
		return false;
	}
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::CompleteCurrentAction(
	const FHyperAIStudioPlanActionResult& Result,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedPlan::Private;
	OutError.Reset();
	if (State != EHyperAIStudioPlanCoordinatorState::AwaitingActionResult
		|| !Schedule.IsValidIndex(NextActionIndex))
	{
		OutError = TEXT("No single in-flight typed action is awaiting completion.");
		return false;
	}
	const FHyperAIStudioPlanScheduledAction& Action = Schedule[NextActionIndex];
	auto RejectUntrustedResult = [this, &OutError](const FString& Reason)
	{
		OutError = Reason;
		if (!ActivePlan.bHasMutation)
		{
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
		FString JournalError;
		const EHyperAIStudioPlanJournalTransition Closure = bCommitStarted
			? EHyperAIStudioPlanJournalTransition::OutcomeUnknown
			: EHyperAIStudioPlanJournalTransition::FailedPreCommit;
		if (!TransitionJournal(Closure, {}, JournalError))
		{
			OutError += TEXT(" Durable journal closure failed: ") + JournalError;
		}
		State = bCommitStarted
			? EHyperAIStudioPlanCoordinatorState::OutcomeUnknown
			: EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	};

	switch (Result.Outcome)
	{
	case EHyperAIStudioPlanActionOutcome::Succeeded:
		if ((Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce
				&& (!HasFreshPostconditionEvidence(Result.Evidence)
					|| !ValidateValidatorReceipt(
						Result.Evidence.ValidatorReceipt,
						FHyperAIStudioTypedPlanValidator::FreshValidatorId(),
						FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint(),
						ActiveClock ? ActiveClock->NowUtcMs() : 0,
						OutError)))
			|| (Action.Kind != EHyperAIStudioPlanActionKind::VerifyFreshOnce
				&& !HasNoOutcomeEvidence(Result.Evidence)))
		{
			const FString ReceiptError = OutError;
			return RejectUntrustedResult(ReceiptError.IsEmpty()
				? TEXT("Successful fresh verification requires trusted whole-plan postcondition evidence; other successful actions cannot carry evidence.")
				: ReceiptError);
		}
		break;
	case EHyperAIStudioPlanActionOutcome::FailedBeforeEffect:
		if ((!bKnownCommittedEffect && !HasNoOutcomeEvidence(Result.Evidence))
			|| (bKnownCommittedEffect && !HasNoOutcomeEvidence(Result.Evidence)
				&& !HasConsistentNonCompleteRollbackEvidence(Result.Evidence)))
		{
			return RejectUntrustedResult(TEXT("A before-effect failure carries evidence inconsistent with earlier known effects."));
		}
		break;
	case EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect:
		if (!HasConsistentNonCompleteRollbackEvidence(Result.Evidence))
		{
			return RejectUntrustedResult(TEXT("A known-effect failure requires exact partial, unsupported, or unknown rollback evidence."));
		}
		break;
	case EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence:
		if (!HasCompleteRollbackEvidence(Result.Evidence)
			|| !ValidateValidatorReceipt(
				Result.Evidence.ValidatorReceipt,
				FHyperAIStudioTypedPlanValidator::RollbackValidatorId(),
				FHyperAIStudioTypedPlanValidator::RollbackValidatorFingerprint(),
				ActiveClock ? ActiveClock->NowUtcMs() : 0,
				OutError))
		{
			const FString ReceiptError = OutError;
			return RejectUntrustedResult(ReceiptError.IsEmpty()
				? TEXT("Rolled-back outcome requires complete trusted whole-plan/no-residue evidence.")
				: ReceiptError);
		}
		break;
	case EHyperAIStudioPlanActionOutcome::OutcomeUnknown:
		if (!HasNoOutcomeEvidence(Result.Evidence)
			&& !(Result.Evidence.RollbackState == EHyperAIStudioPlanRollbackState::Unknown
				&& !Result.Evidence.ValidatorReceipt.IsPresent()))
		{
			return RejectUntrustedResult(TEXT("An unknown outcome cannot claim trusted rollback or postcondition evidence."));
		}
		break;
	default:
		return RejectUntrustedResult(TEXT("Unknown typed action outcome."));
	}
	if ((Result.Outcome == EHyperAIStudioPlanActionOutcome::Succeeded
			&& Action.Kind == EHyperAIStudioPlanActionKind::Step && Action.Safety != EHyperAIStudioPlanSafety::Read)
		|| Result.Outcome == EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect)
	{
		bKnownCommittedEffect = true;
	}
	FString UsageError;
	if (!AccountUsage(Result, UsageError))
	{
		return FailForRuntimeLimit(UsageError, OutError);
	}
	if (!ActiveClock || ActiveClock->NowMonotonicMs() >= DeadlineMonotonicMs)
	{
		return FailForRuntimeLimit(TEXT("The execution deadline elapsed while the typed action was in flight."), OutError);
	}
	ActiveActionNonce.Reset();

	switch (Result.Outcome)
	{
	case EHyperAIStudioPlanActionOutcome::Succeeded:
		if (Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce)
		{
			bFreshVerificationComplete = true;
			FreshVerificationEvidence = Result.Evidence;
		}
		++NextActionIndex;
		if (NextActionIndex >= Schedule.Num())
		{
			return FinishSuccessfully(OutError);
		}
		State = EHyperAIStudioPlanCoordinatorState::Ready;
		return true;

	case EHyperAIStudioPlanActionOutcome::FailedBeforeEffect:
		if (ActivePlan.bHasMutation)
		{
			EHyperAIStudioPlanJournalTransition Transition = EHyperAIStudioPlanJournalTransition::FailedPreCommit;
			FHyperAIStudioPlanOutcomeEvidence Evidence = Result.Evidence;
			if (bCommitStarted)
			{
				Transition = bKnownCommittedEffect
					? EHyperAIStudioPlanJournalTransition::Partial
					: EHyperAIStudioPlanJournalTransition::OutcomeUnknown;
				if (bKnownCommittedEffect && Evidence.RollbackState == EHyperAIStudioPlanRollbackState::NotNeeded)
				{
					Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
				}
			}
			if (!TransitionJournal(Transition, Evidence, OutError))
			{
				State = bCommitStarted
					? EHyperAIStudioPlanCoordinatorState::OutcomeUnknown
					: EHyperAIStudioPlanCoordinatorState::Failed;
				return false;
			}
		}
		State = bCommitStarted
			? (bKnownCommittedEffect
				? EHyperAIStudioPlanCoordinatorState::Partial
				: EHyperAIStudioPlanCoordinatorState::OutcomeUnknown)
			: EHyperAIStudioPlanCoordinatorState::Failed;
		return true;

	case EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect:
		if (Action.Safety == EHyperAIStudioPlanSafety::Read)
		{
			if (ActivePlan.bHasMutation
				&& !TransitionJournal(EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, OutError))
			{
				State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
				return false;
			}
			State = ActivePlan.bHasMutation
				? EHyperAIStudioPlanCoordinatorState::OutcomeUnknown
				: EHyperAIStudioPlanCoordinatorState::Failed;
			OutError = TEXT("A read-class action reported a known effect; the operation contract is violated and retry is prohibited.");
			return false;
		}
		if (ActivePlan.bHasMutation
			&& !TransitionJournal(EHyperAIStudioPlanJournalTransition::Partial, Result.Evidence, OutError))
		{
			State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
			return false;
		}
		State = EHyperAIStudioPlanCoordinatorState::Partial;
		return true;

	case EHyperAIStudioPlanActionOutcome::RolledBackWithEvidence:
		if (!ActivePlan.bHasMutation || !bCommitStarted)
		{
			OutError = TEXT("Rollback evidence is valid only after a mutation commit marker.");
			State = EHyperAIStudioPlanCoordinatorState::Failed;
			return false;
		}
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::RolledBack, Result.Evidence, OutError))
		{
			State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
			return false;
		}
		State = EHyperAIStudioPlanCoordinatorState::RolledBack;
		return true;

	case EHyperAIStudioPlanActionOutcome::OutcomeUnknown:
		if (ActivePlan.bHasMutation)
		{
			const EHyperAIStudioPlanJournalTransition Transition = bCommitStarted
				? EHyperAIStudioPlanJournalTransition::OutcomeUnknown
				: EHyperAIStudioPlanJournalTransition::FailedPreCommit;
			FString JournalError;
			if (!TransitionJournal(Transition, {}, JournalError))
			{
				State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
				OutError = TEXT("The ambiguous action could not be terminally persisted: ") + JournalError;
				return false;
			}
		}
		State = bCommitStarted
			? EHyperAIStudioPlanCoordinatorState::OutcomeUnknown
			: EHyperAIStudioPlanCoordinatorState::Failed;
		if (OutError.IsEmpty())
		{
			OutError = bCommitStarted
				? TEXT("The typed action outcome is unknown after commit start; no retry or fallback is permitted.")
				: TEXT("The pre-commit action outcome was ambiguous, but no mutation was exposed; the journal was closed as failed_precommit.");
		}
		return true;
	default:
		return RejectUntrustedResult(TEXT("Unknown typed action outcome."));
	}
}

bool FHyperAIStudioSerialPlanCoordinator::CertifyCurrentActionNoEffect(
	const FString& Reason,
	FString& OutError)
{
	OutError = Reason.IsEmpty()
		? TEXT("The core runtime certified that the acquired action was never dispatched and caused no effect or callback.")
		: Reason;
	if (State != EHyperAIStudioPlanCoordinatorState::AwaitingActionResult
		|| !Schedule.IsValidIndex(NextActionIndex)
		|| !ActivePlan.bHasMutation || !bCommitStarted || bKnownCommittedEffect
		|| Schedule[NextActionIndex].Kind != EHyperAIStudioPlanActionKind::Step
		|| Schedule[NextActionIndex].Safety == EHyperAIStudioPlanSafety::Read
		|| !HyperAIStudio::TypedPlan::Private::IsSha256Token(ActivePlan.EffectFingerprint)
		|| !HyperAIStudio::TypedPlan::Private::IsBoundedText(
			ActiveActionNonce, FHyperAIStudioPlanLimits::MaxActionNonceChars, false)
		|| !ActiveJournalGate)
	{
		OutError = TEXT("Certified no-effect closure is valid only for the exact first commit-marked mutation action.");
		State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
		return false;
	}
	const FString PrimaryError = OutError;
	FString JournalError;
	if (!ActiveJournalGate->CertifyNoEffect(
			ActivePlan.OperationId, ActivePlan.EffectFingerprint, ActiveActionNonce, JournalError))
	{
		OutError = PrimaryError + TEXT(" Durable no-effect certification failed: ") + JournalError;
		State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
		return false;
	}
	ActiveActionNonce.Reset();
	State = EHyperAIStudioPlanCoordinatorState::Failed;
	OutError = PrimaryError;
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::TransitionJournal(
	const EHyperAIStudioPlanJournalTransition Transition,
	const FHyperAIStudioPlanOutcomeEvidence& Evidence,
	FString& OutError)
{
	if (!ActiveJournalGate)
	{
		OutError = TEXT("The required durable journal gate is unavailable.");
		return false;
	}
	return ActiveJournalGate->Transition(ActivePlan.OperationId, Transition, Evidence, OutError);
}

bool FHyperAIStudioSerialPlanCoordinator::ClosePreCommitAfterFailure(FString& OutError)
{
	State = EHyperAIStudioPlanCoordinatorState::Failed;
	if (!ActivePlan.bHasMutation || !ActiveJournalGate || bCommitStarted)
	{
		return false;
	}
	const FString PrimaryError = OutError;
	FString CloseError;
	if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, CloseError))
	{
		OutError = PrimaryError + TEXT(" Pre-commit journal closure also failed: ") + CloseError;
		return false;
	}
	OutError = PrimaryError;
	return true;
}

bool FHyperAIStudioSerialPlanCoordinator::FailForRuntimeLimit(const FString& Reason, FString& OutError)
{
	OutError = Reason;
	if (!ActivePlan.bHasMutation)
	{
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}
	FString JournalError;
	if (!bCommitStarted)
	{
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, JournalError))
		{
			OutError += TEXT(" Journal closure failed: ") + JournalError;
		}
		State = EHyperAIStudioPlanCoordinatorState::Failed;
		return false;
	}
	if (bKnownCommittedEffect)
	{
		FHyperAIStudioPlanOutcomeEvidence Evidence;
		Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::Partial, Evidence, JournalError))
		{
			State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
			OutError += TEXT(" Partial outcome could not be persisted: ") + JournalError;
			return false;
		}
		State = EHyperAIStudioPlanCoordinatorState::Partial;
		return false;
	}
	if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, JournalError))
	{
		OutError += TEXT(" Unknown outcome could not be persisted: ") + JournalError;
	}
	State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
	return false;
}

bool FHyperAIStudioSerialPlanCoordinator::FinishSuccessfully(FString& OutError)
{
	if (ActivePlan.bHasMutation)
	{
		if (!bFreshVerificationComplete
			|| !HyperAIStudio::TypedPlan::Private::HasFreshPostconditionEvidence(FreshVerificationEvidence))
		{
			return FailForRuntimeLimit(
				TEXT("Mutation completion requires a successful fresh persisted readback/postcondition verification."), OutError);
		}
		if (!TransitionJournal(EHyperAIStudioPlanJournalTransition::Completed, FreshVerificationEvidence, OutError))
		{
			const FString CompletionError = OutError;
			if (bCommitStarted)
			{
				FString UnknownError;
				TransitionJournal(EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, UnknownError);
			}
			State = EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
			OutError = TEXT("Completion could not be persisted after possible effects: ") + CompletionError;
			return false;
		}
	}
	State = EHyperAIStudioPlanCoordinatorState::Completed;
	return true;
}
