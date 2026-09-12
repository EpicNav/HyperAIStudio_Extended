// Games by Hyper 2026.

#include "HyperAIStudioContextSearchToolsets.h"

#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Containers/StringConv.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GraphEditor.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "IModelContextProtocolModule.h"
#include "Internationalization/Text.h"
#include "Interfaces/IPluginManager.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioContextSearchToolsets)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioContextSearch, Log, All);

namespace HyperAIStudio::ContextSearch::Private
{
	constexpr int32 MinOutputBytes = 2048;
	constexpr int32 MaxPathCharacters = 1024;
	constexpr int32 MaxQueryCharacters = 128;
	constexpr int32 MaxQueryIdCharacters = 64;
	constexpr int32 MaxDirtyPackages = 64;
	constexpr int32 MaxBlueprintsScanned = 256;
	constexpr int32 MaxBlueprintGraphsScanned = 1024;
	constexpr int32 MaxBlueprintSelectionNodesScanned = 2048;
	constexpr int32 MaxSceneSnapshotRecords = 4096;
	constexpr double MaxEditorCaptureSeconds = 0.050;
	constexpr double MaxCppIndexSeconds = 1.500;
	constexpr double MaxProjectSearchSeconds = 0.010;
	constexpr int32 MaxHashTokens = 262144;
	constexpr int32 MaxHashUtf16Characters = 24 * 1024 * 1024;
	constexpr int64 MaxHashUtf8Bytes = 64ll * 1024ll * 1024ll;

	bool HasWellFormedUtf16(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character >= 0xd800 && Character <= 0xdbff)
			{
				if (Index + 1 >= Value.Len()
					|| Value[Index + 1] < 0xdc00 || Value[Index + 1] > 0xdfff)
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

	bool IsCanonicalSha256(const FString& Value)
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

	bool IsWellFormedUtf8(const uint8* Bytes, const int32 ByteCount)
	{
		if (ByteCount < 0 || (ByteCount > 0 && !Bytes))
		{
			return false;
		}
		int32 Offset = 0;
		while (Offset < ByteCount)
		{
			const uint8 First = Bytes[Offset++];
			if (First == 0)
			{
				return false;
			}
			if (First <= 0x7fu)
			{
				continue;
			}

			int32 Continuations = 0;
			uint32 CodePoint = 0;
			uint32 Minimum = 0;
			if (First >= 0xc2u && First <= 0xdfu)
			{
				Continuations = 1;
				CodePoint = First & 0x1fu;
				Minimum = 0x80u;
			}
			else if (First >= 0xe0u && First <= 0xefu)
			{
				Continuations = 2;
				CodePoint = First & 0x0fu;
				Minimum = 0x800u;
			}
			else if (First >= 0xf0u && First <= 0xf4u)
			{
				Continuations = 3;
				CodePoint = First & 0x07u;
				Minimum = 0x10000u;
			}
			else
			{
				return false;
			}
			if (Offset + Continuations > ByteCount)
			{
				return false;
			}
			for (int32 Index = 0; Index < Continuations; ++Index)
			{
				const uint8 Continuation = Bytes[Offset++];
				if ((Continuation & 0xc0u) != 0x80u)
				{
					return false;
				}
				CodePoint = (CodePoint << 6) | (Continuation & 0x3fu);
			}
			if (CodePoint < Minimum || CodePoint > 0x10ffffu
				|| (CodePoint >= 0xd800u && CodePoint <= 0xdfffu))
			{
				return false;
			}
		}
		return true;
	}

	bool IsWellFormedUtf16Le(const uint8* Bytes, const int32 ByteCount)
	{
		if (ByteCount < 0 || (ByteCount % 2) != 0 || (ByteCount > 0 && !Bytes))
		{
			return false;
		}
		for (int32 Offset = 0; Offset < ByteCount; Offset += 2)
		{
			const uint16 Character = static_cast<uint16>(Bytes[Offset])
				| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
			if (Character == 0)
			{
				return false;
			}
			if (Character >= 0xd800u && Character <= 0xdbffu)
			{
				if (Offset + 3 >= ByteCount)
				{
					return false;
				}
				const uint16 Low = static_cast<uint16>(Bytes[Offset + 2])
					| (static_cast<uint16>(Bytes[Offset + 3]) << 8);
				if (Low < 0xdc00u || Low > 0xdfffu)
				{
					return false;
				}
				Offset += 2;
			}
			else if (Character >= 0xdc00u && Character <= 0xdfffu)
			{
				return false;
			}
		}
		return true;
	}

	class FBoundedSha256 final
	{
	public:
		bool Update(const uint8* Bytes, const int32 ByteCount)
		{
			const uint64 ByteLimit = static_cast<uint64>(MaxHashUtf8Bytes);
			if (bFinalized || ByteCount < 0 || (ByteCount > 0 && !Bytes)
				|| static_cast<uint64>(ByteCount) > ByteLimit
				|| TotalBytes > ByteLimit - static_cast<uint64>(ByteCount))
			{
				return false;
			}
			TotalBytes += static_cast<uint64>(ByteCount);
			int32 Offset = 0;
			while (Offset < ByteCount)
			{
				const int32 CopyCount = FMath::Min(ByteCount - Offset, 64 - BufferedBytes);
				FMemory::Memcpy(Buffer + BufferedBytes, Bytes + Offset, CopyCount);
				BufferedBytes += CopyCount;
				Offset += CopyCount;
				if (BufferedBytes == 64)
				{
					Compress(Buffer);
					BufferedBytes = 0;
				}
			}
			return true;
		}

		FString Finalize()
		{
			if (bFinalized)
			{
				return FString();
			}
			bFinalized = true;
			const uint64 BitCount = TotalBytes * 8u;
			Buffer[BufferedBytes++] = 0x80u;
			if (BufferedBytes > 56)
			{
				while (BufferedBytes < 64)
				{
					Buffer[BufferedBytes++] = 0u;
				}
				Compress(Buffer);
				BufferedBytes = 0;
			}
			while (BufferedBytes < 56)
			{
				Buffer[BufferedBytes++] = 0u;
			}
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Buffer[BufferedBytes++] = static_cast<uint8>((BitCount >> Shift) & 0xffu);
			}
			Compress(Buffer);

			FString Hex = TEXT("sha256:");
			Hex.Reserve(71);
			for (const uint32 Word : State)
			{
				Hex += FString::Printf(TEXT("%08x"), Word);
			}
			return Hex;
		}

	private:
		static uint32 RotateRight(const uint32 Input, const uint32 Shift)
		{
			return (Input >> Shift) | (Input << (32u - Shift));
		}

		void Compress(const uint8* Block)
		{
			static constexpr uint32 Constants[64] = {
				0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
				0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
				0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
				0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
				0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
				0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
				0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
				0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
			};
			uint32 Words[64]{};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = Index * 4;
				Words[Index] = (static_cast<uint32>(Block[Offset]) << 24)
					| (static_cast<uint32>(Block[Offset + 1]) << 16)
					| (static_cast<uint32>(Block[Offset + 2]) << 8)
					| static_cast<uint32>(Block[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 Small0 = RotateRight(Words[Index - 15], 7)
					^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const uint32 Small1 = RotateRight(Words[Index - 2], 17)
					^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
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

		uint32 State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};
		uint8 Buffer[64]{};
		int32 BufferedBytes = 0;
		uint64 TotalBytes = 0;
		bool bFinalized = false;
	};

	FString NowUtc()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	bool ContainsEmbeddedNull(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character == TEXT('\0'))
			{
				return true;
			}
		}
		return false;
	}

	FString Clip(const FString& Value, const int32 MaxCharacters)
	{
		return Value.Left(FMath::Max(0, MaxCharacters));
	}

	FString NormalizeToken(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
	}

	void AddDiagnostic(
		TArray<FHyperAIReadDiagnostic>& Diagnostics,
		const TCHAR* Code,
		const TCHAR* Severity,
		const TCHAR* Field,
		const FString& Message)
	{
		if (Diagnostics.Num() >= FHyperAIStudioContextSearchContracts::MaxDiagnostics)
		{
			return;
		}
		FHyperAIReadDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = Clip(Code, 64);
		Diagnostic.Severity = Clip(Severity, 16);
		Diagnostic.Field = Clip(Field, 64);
		Diagnostic.Message = Clip(
			Message,
			FHyperAIStudioContextSearchContracts::MaxDiagnosticCharacters);
	}

	void AddDiagnosticOnce(
		TArray<FHyperAIReadDiagnostic>& Diagnostics,
		const TCHAR* Code,
		const TCHAR* Severity,
		const TCHAR* Field,
		const FString& Message)
	{
		if (Diagnostics.ContainsByPredicate([Code](const FHyperAIReadDiagnostic& Existing)
		{
			return Existing.Code == Code;
		}))
		{
			return;
		}
		AddDiagnostic(Diagnostics, Code, Severity, Field, Message);
	}

	void AddField(
		FHyperAIContextSearchValueRecord& Record,
		const TCHAR* Name,
		const FString& Type,
		const FString& Value)
	{
		FHyperAIReadField& Field = Record.Fields.AddDefaulted_GetRef();
		Field.Name = Clip(Name, FHyperAIStudioContextSearchContracts::MaxFieldNameCharacters);
		Field.Type = Clip(Type, 32);
		Field.Value = Clip(Value, FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters);
	}

	void SortFields(FHyperAIContextSearchValueRecord& Record)
	{
		Record.Fields.Sort([](const FHyperAIReadField& Left, const FHyperAIReadField& Right)
		{
			if (Left.Name != Right.Name)
			{
				return Left.Name < Right.Name;
			}
			if (Left.Type != Right.Type)
			{
				return Left.Type < Right.Type;
			}
			return Left.Value < Right.Value;
		});
	}

	FString FindFieldValue(const FHyperAIContextSearchValueRecord& Record, const TCHAR* Name)
	{
		if (const FHyperAIReadField* Field = Record.Fields.FindByPredicate([Name](const FHyperAIReadField& Candidate)
		{
			return Candidate.Name == Name;
		}))
		{
			return Field->Value;
		}
		return FString();
	}

	bool RecordLess(
		const FHyperAIContextSearchValueRecord& Left,
		const FHyperAIContextSearchValueRecord& Right)
	{
		if (Left.QueryId != Right.QueryId)
		{
			return Left.QueryId < Right.QueryId;
		}
		if (Left.Kind != Right.Kind)
		{
			return Left.Kind < Right.Kind;
		}
		return Left.RecordId < Right.RecordId;
	}

	void SortAndDeduplicateRecords(TArray<FHyperAIContextSearchValueRecord>& Records)
	{
		for (FHyperAIContextSearchValueRecord& Record : Records)
		{
			SortFields(Record);
		}
		Records.Sort(RecordLess);
		for (int32 Index = Records.Num() - 1; Index > 0; --Index)
		{
			const FHyperAIContextSearchValueRecord& Current = Records[Index];
			const FHyperAIContextSearchValueRecord& Prior = Records[Index - 1];
			if (Current.QueryId == Prior.QueryId
				&& Current.Kind == Prior.Kind
				&& Current.RecordId == Prior.RecordId)
			{
				Records.RemoveAt(Index);
			}
		}
	}

	bool RecordsAreCanonical(const TArray<FHyperAIContextSearchValueRecord>& Records)
	{
		for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
		{
			const FHyperAIContextSearchValueRecord& Record = Records[RecordIndex];
			for (int32 FieldIndex = 1; FieldIndex < Record.Fields.Num(); ++FieldIndex)
			{
				const FHyperAIReadField& Prior = Record.Fields[FieldIndex - 1];
				const FHyperAIReadField& Current = Record.Fields[FieldIndex];
				if (Current.Name < Prior.Name
					|| (Current.Name == Prior.Name && Current.Type < Prior.Type)
					|| (Current.Name == Prior.Name && Current.Type == Prior.Type
						&& Current.Value < Prior.Value))
				{
					return false;
				}
			}
			if (RecordIndex > 0 && !RecordLess(Records[RecordIndex - 1], Record))
			{
				return false;
			}
		}
		return true;
	}

	bool StringsAreSorted(const TArray<FString>& Values)
	{
		for (int32 Index = 1; Index < Values.Num(); ++Index)
		{
			if (Values[Index] < Values[Index - 1])
			{
				return false;
			}
		}
		return true;
	}

	bool UpdateHashToken(FBoundedSha256& Hasher, const FString& Token, int32& TokenCount)
	{
		if (++TokenCount > MaxHashTokens
			|| Token.Len() > MaxHashUtf16Characters
			|| !HasWellFormedUtf16(Token))
		{
			return false;
		}
		ANSICHAR Prefix[32];
		const int32 PrefixLength = FCStringAnsi::Snprintf(
			Prefix,
			UE_ARRAY_COUNT(Prefix),
			"%d:",
			Token.Len());
		if (PrefixLength <= 0 || PrefixLength >= UE_ARRAY_COUNT(Prefix))
		{
			return false;
		}
		const FTCHARToUTF8 TokenUtf8(*Token);
		return Hasher.Update(reinterpret_cast<const uint8*>(Prefix), PrefixLength)
			&& Hasher.Update(reinterpret_cast<const uint8*>(TokenUtf8.Get()), TokenUtf8.Length());
	}

	FString ComputeCanonicalSnapshotFingerprint(
		const FHyperAIContextSearchValueSnapshot& Snapshot,
		const TArray<FHyperAIContextSearchValueRecord>& CanonicalRecords,
		const TArray<FString>& SortedDirtyPackages)
	{
		FBoundedSha256 Hasher;
		int32 TokenCount = 0;
		const auto Add = [&Hasher, &TokenCount](const FString& Token)
		{
			return UpdateHashToken(Hasher, Token, TokenCount);
		};
		if (!Add(Snapshot.ObservationScope)
			|| !Add(Snapshot.bOnDiskOnly ? TEXT("1") : TEXT("0"))
			|| !Add(Snapshot.bDirty ? TEXT("1") : TEXT("0"))
			|| !Add(Snapshot.bIncomplete ? TEXT("1") : TEXT("0"))
			|| !Add(Snapshot.bSourceTruncated ? TEXT("1") : TEXT("0"))
			|| !Add(Snapshot.bDirtyPackageListTruncated ? TEXT("1") : TEXT("0"))
			|| !Add(LexToString(Snapshot.Generation)))
		{
			return FString();
		}
		for (const FString& DirtyPackage : SortedDirtyPackages)
		{
			if (!Add(DirtyPackage))
			{
				return FString();
			}
		}
		for (const FHyperAIContextSearchValueRecord& Record : CanonicalRecords)
		{
			if (!Add(Record.QueryId) || !Add(Record.Kind) || !Add(Record.RecordId))
			{
				return FString();
			}
			for (const FHyperAIReadField& Field : Record.Fields)
			{
				if (!Add(Field.Name) || !Add(Field.Type) || !Add(Field.Value))
				{
					return FString();
				}
			}
		}
		return Hasher.Finalize();
	}

	int32 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}

	int32 EstimateRecordBytes(const FHyperAIReadRecord& Record)
	{
		int32 Bytes = 96 + Utf8Bytes(Record.QueryId) + Utf8Bytes(Record.Kind) + Utf8Bytes(Record.RecordId);
		for (const FHyperAIReadField& Field : Record.Fields)
		{
			Bytes += 48 + Utf8Bytes(Field.Name) + Utf8Bytes(Field.Type) + Utf8Bytes(Field.Value);
		}
		return Bytes;
	}

	FHyperAIReadReport InvalidReport(
		const FString& Code,
		const FString& Field,
		const FString& Message)
	{
		FHyperAIReadReport Report;
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Clip(Message, FHyperAIStudioContextSearchContracts::MaxDiagnosticCharacters);
		AddDiagnostic(Report.Diagnostics, *Code, TEXT("error"), *Field, Message);
		return Report;
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString NumberToken(const double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	FString VectorToken(const FVector& Value)
	{
		return FString::Printf(TEXT("%.9g,%.9g,%.9g"), Value.X, Value.Y, Value.Z);
	}

	FString RotatorToken(const FRotator& Value)
	{
		return FString::Printf(TEXT("%.9g,%.9g,%.9g"), Value.Pitch, Value.Yaw, Value.Roll);
	}

	bool IsValidIdentifier(const FString& Value, const int32 MaxCharacters)
	{
		if (Value.IsEmpty() || Value.Len() > MaxCharacters || ContainsEmbeddedNull(Value))
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_'))
				|| (Index == 0 && FChar::IsDigit(Character)))
			{
				return false;
			}
		}
		return true;
	}

	const TSet<FString>& ContextProjectionAllowlist()
	{
		static const TSet<FString> Values = {
			TEXT("project_name"), TEXT("project_file"), TEXT("project_root"),
			TEXT("map_name"), TEXT("world_path"), TEXT("package_name"), TEXT("dirty"),
			TEXT("location"), TEXT("rotation"), TEXT("scale"),
			TEXT("name"), TEXT("label"), TEXT("class_path"), TEXT("path"),
			TEXT("blueprint_path"), TEXT("graph_name"), TEXT("graph_path"), TEXT("graph_guid"),
			TEXT("node_guid"), TEXT("node_comment"), TEXT("node_position"),
			TEXT("pin_guid"), TEXT("pin_name"), TEXT("pin_direction"), TEXT("pin_category"),
			TEXT("linked_pin_count"), TEXT("default_value")
		};
		return Values;
	}

	const TArray<FString>& ContextProjectionDefaults()
	{
		static const TArray<FString> Values = {
			TEXT("project_name"), TEXT("project_file"), TEXT("map_name"), TEXT("world_path"),
			TEXT("dirty"), TEXT("location"), TEXT("rotation"), TEXT("name"), TEXT("label"),
			TEXT("class_path"), TEXT("path"), TEXT("package_name"), TEXT("blueprint_path"),
			TEXT("graph_name"), TEXT("graph_guid"),
			TEXT("node_guid"), TEXT("node_position"), TEXT("pin_guid"),
			TEXT("pin_name"), TEXT("pin_direction"), TEXT("pin_category"), TEXT("linked_pin_count")
		};
		return Values;
	}

	const TSet<FString>& SceneProjectionAllowlist()
	{
		static const TSet<FString> Values = {
			TEXT("name"), TEXT("label"), TEXT("class_path"), TEXT("path"), TEXT("selected"),
			TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("component_count"),
			TEXT("owner_path"), TEXT("registered"), TEXT("active"),
			TEXT("property_name"), TEXT("property_type"), TEXT("property_value"), TEXT("property_owner_path")
		};
		return Values;
	}

	const TArray<FString>& SceneProjectionDefaults()
	{
		static const TArray<FString> Values = {
			TEXT("name"), TEXT("label"), TEXT("class_path"), TEXT("path"), TEXT("selected"),
			TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("component_count"),
			TEXT("owner_path"), TEXT("registered"), TEXT("active")
		};
		return Values;
	}

	const TSet<FString>& ProjectProjectionAllowlist()
	{
		static const TSet<FString> Values = {
			TEXT("name"), TEXT("asset_name"), TEXT("asset_class_path"), TEXT("package_name"),
			TEXT("object_path"), TEXT("symbol_kind"), TEXT("relative_path"), TEXT("line"),
			TEXT("status"), TEXT("generation"), TEXT("asset_count"), TEXT("cpp_file_count"),
			TEXT("cpp_symbol_count"), TEXT("total_record_count")
		};
		return Values;
	}

	const TArray<FString>& ProjectProjectionDefaults()
	{
		static const TArray<FString> Values = {
			TEXT("name"), TEXT("asset_name"), TEXT("asset_class_path"), TEXT("package_name"),
			TEXT("object_path"), TEXT("symbol_kind"), TEXT("relative_path"), TEXT("line")
		};
		return Values;
	}

	const TSet<FString>& ProjectKinds()
	{
		static const TSet<FString> Values = { TEXT("asset"), TEXT("cpp_symbol") };
		return Values;
	}

	FString HashSingle(const FString& Value)
	{
		return FHyperAIStudioContextSearchContracts::HashTokens({ Value });
	}

	void AppendDirtyPackages(
		FHyperAIContextSearchValueSnapshot& Snapshot,
		const TArray<FString>& CandidatePackages)
	{
		TSet<FString> Unique;
		for (const FString& PackageName : CandidatePackages)
		{
			if (!PackageName.IsEmpty())
			{
				Unique.Add(Clip(PackageName, MaxPathCharacters));
			}
		}
		Snapshot.DirtyPackages = Unique.Array();
		Snapshot.DirtyPackages.Sort();
		if (Snapshot.DirtyPackages.Num() > MaxDirtyPackages)
		{
			Snapshot.DirtyPackages.SetNum(MaxDirtyPackages);
			Snapshot.bDirtyPackageListTruncated = true;
		}
		Snapshot.bDirty = !Snapshot.DirtyPackages.IsEmpty();
	}

	TArray<FString> CaptureDirtyProjectPackages()
	{
		TArray<FString> DirtyPackages;
		if (!IsInGameThread())
		{
			return DirtyPackages;
		}
		int32 Scanned = 0;
		for (TObjectIterator<UPackage> Iterator; Iterator && Scanned < 4096; ++Iterator, ++Scanned)
		{
			const UPackage* Package = *Iterator;
			if (Package && Package->IsDirty() && Package->GetName().StartsWith(TEXT("/Game/")))
			{
				DirtyPackages.Add(Package->GetName());
				if (DirtyPackages.Num() > MaxDirtyPackages)
				{
					break;
				}
			}
		}
		return DirtyPackages;
	}

	bool TryReadSafeScalarProperty(
		const UObject* Object,
		const FString& PropertyName,
		FString& OutType,
		FString& OutValue,
		FString& OutReason)
	{
		OutType.Reset();
		OutValue.Reset();
		OutReason.Reset();
		if (!Object || !IsValidIdentifier(PropertyName, 64))
		{
			OutReason = TEXT("invalid_property_name");
			return false;
		}
		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*PropertyName));
		if (!Property)
		{
			OutReason = TEXT("property_not_found");
			return false;
		}
		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Property->ArrayDim != 1)
		{
			OutReason = TEXT("property_not_read_allowlisted");
			return false;
		}
		const void* Address = Property->ContainerPtrToValuePtr<void>(Object);
		if (!Address)
		{
			OutReason = TEXT("property_address_unavailable");
			return false;
		}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			OutType = TEXT("bool");
			OutValue = BoolToken(BoolProperty->GetPropertyValue(Address));
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Numeric = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Address);
			OutType = TEXT("enum");
			OutValue = EnumProperty->GetEnum()
				? EnumProperty->GetEnum()->GetNameStringByValue(Numeric)
				: LexToString(Numeric);
		}
		else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			OutType = NumericProperty->IsInteger() ? TEXT("integer") : TEXT("number");
			OutValue = NumericProperty->GetNumericPropertyValueToString(Address);
		}
		else if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			const FName NameValue = NameProperty->GetPropertyValue(Address);
			if (NameValue.GetStringLength() > FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters)
			{
				OutReason = TEXT("property_value_oversized_or_invalid");
				return false;
			}
			OutType = TEXT("name");
			OutValue = NameValue.ToString();
		}
		else if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			const FString& StringValue = StringProperty->GetPropertyValue(Address);
			if (!FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(StringValue, OutValue))
			{
				OutReason = TEXT("property_value_preview_unbounded");
				return false;
			}
			OutType = TEXT("string");
		}
		else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			const FText& TextValue = TextProperty->GetPropertyValue(Address);
			if (!FHyperAIStudioContextSearchContracts::TryCopyBoundedTextPreview(TextValue, OutValue))
			{
				OutReason = TEXT("property_value_preview_unbounded");
				return false;
			}
			OutType = TEXT("text");
		}
		else if (CastField<FObjectPropertyBase>(Property))
		{
			// UObject path materialization can walk an arbitrarily deep outer chain before a
			// caller can clip it. Identity is already returned by typed actor/component records.
			OutReason = TEXT("loaded_object_path_preview_unbounded");
			return false;
		}
		else
		{
			OutReason = TEXT("property_type_not_read_allowlisted");
			return false;
		}

		if (ContainsEmbeddedNull(OutValue)
			|| OutValue.Len() > FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters)
		{
			OutType.Reset();
			OutValue.Reset();
			OutReason = TEXT("property_value_oversized_or_invalid");
			return false;
		}
		return true;
	}

	void AddScalarPropertyRecords(
		const UObject* Object,
		const FString& OwnerPath,
		const TArray<FString>& PropertyNames,
		FHyperAIContextSearchValueSnapshot& Snapshot)
	{
		for (const FString& PropertyName : PropertyNames)
		{
			if (Snapshot.Records.Num() >= MaxSceneSnapshotRecords)
			{
				Snapshot.bIncomplete = true;
				Snapshot.bSourceTruncated = true;
				AddDiagnosticOnce(
					Snapshot.Diagnostics,
					TEXT("scene_snapshot_record_bound_reached"),
					TEXT("warning"),
					TEXT("records"),
					TEXT("Scene identity, component, and property records reached the fixed aggregate snapshot bound."));
				return;
			}
			FString Type;
			FString Value;
			FString Reason;
			if (!TryReadSafeScalarProperty(Object, PropertyName, Type, Value, Reason))
			{
				AddDiagnosticOnce(
					Snapshot.Diagnostics,
					*Reason,
					TEXT("info"),
					TEXT("property_names"),
					FString::Printf(TEXT("Property '%s' on '%s' was omitted by the bounded scalar-read policy."), *PropertyName, *OwnerPath));
				continue;
			}
			FHyperAIContextSearchValueRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("property");
			Record.RecordId = HashSingle(OwnerPath + TEXT("|property|") + PropertyName);
			AddField(Record, TEXT("property_owner_path"), TEXT("path"), OwnerPath);
			AddField(Record, TEXT("property_name"), TEXT("name"), PropertyName);
			AddField(Record, TEXT("property_type"), TEXT("string"), Type);
			AddField(Record, TEXT("property_value"), Type, Value);
			Record.SearchTextLower = (OwnerPath + TEXT(" ") + PropertyName + TEXT(" ") + Value).ToLower();
		}
	}

	FString GraphGuidToken(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
	}

	FString PinDirectionToken(const EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input")
			: Direction == EGPD_Output ? TEXT("output")
			: TEXT("unknown");
	}

	bool IsCppExtension(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path, true).ToLower();
		return Extension == TEXT(".h")
			|| Extension == TEXT(".hpp")
			|| Extension == TEXT(".hh")
			|| Extension == TEXT(".cpp")
			|| Extension == TEXT(".cc")
			|| Extension == TEXT(".cxx")
			|| Extension == TEXT(".inl");
	}

	bool IsCppIdentifierStart(const TCHAR Character)
	{
		return FChar::IsAlpha(Character) || Character == TEXT('_');
	}

	bool IsCppIdentifierPart(const TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TEXT('_');
	}

	FString ReadIdentifierAfter(const FString& Line, const int32 StartIndex)
	{
		int32 Index = StartIndex;
		while (Index < Line.Len() && FChar::IsWhitespace(Line[Index]))
		{
			++Index;
		}
		if (!Line.IsValidIndex(Index) || !IsCppIdentifierStart(Line[Index]))
		{
			return FString();
		}
		const int32 Begin = Index;
		while (Index < Line.Len() && IsCppIdentifierPart(Line[Index]))
		{
			++Index;
		}
		return Line.Mid(Begin, Index - Begin);
	}

	FString ReadIdentifierBefore(const FString& Line, const int32 EndExclusive)
	{
		int32 Index = EndExclusive - 1;
		while (Index >= 0 && FChar::IsWhitespace(Line[Index]))
		{
			--Index;
		}
		const int32 End = Index + 1;
		while (Index >= 0 && IsCppIdentifierPart(Line[Index]))
		{
			--Index;
		}
		const int32 Begin = Index + 1;
		return End > Begin && IsCppIdentifierStart(Line[Begin])
			? Line.Mid(Begin, End - Begin)
			: FString();
	}
}

const TArray<FHyperAIContextSearchManifestEntry>& FHyperAIStudioContextSearchContracts::GetManifest()
{
	static const TArray<FHyperAIContextSearchManifestEntry> Manifest = {
		{ TEXT("hyper_batch_query"), TEXT("HyperAIStudio.HyperAIStudioBatchQueryToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_context_snapshot"), TEXT("HyperAIStudio.HyperAIStudioContextSnapshotToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_scene_inspect"), TEXT("HyperAIStudio.HyperAIStudioSceneInspectToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_scene_apply_plan"), TEXT("HyperAIStudio.HyperAIStudioSceneInspectToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_scene_validate"), TEXT("HyperAIStudio.HyperAIStudioSceneInspectToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_project_search"), TEXT("HyperAIStudio.HyperAIStudioProjectSearchToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate },
		{ TEXT("hyper_project_index_status"), TEXT("HyperAIStudio.HyperAIStudioProjectIndexStatusToolset"), FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate }
	};
	return Manifest;
}

bool FHyperAIStudioContextSearchContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(
	const FString& QualifiedToolset,
	const bool bAllowPendingForTests)
{
	TArray<const FHyperAIContextSearchManifestEntry*> Entries;
	for (const FHyperAIContextSearchManifestEntry& Candidate : GetManifest())
	{
		if (Candidate.Toolset == QualifiedToolset) Entries.Add(&Candidate);
	}
	if (Entries.IsEmpty())
	{
		return false;
	}

	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		return false;
	}
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	bool bAllAdmitted = true;
	bool bAllSourceCandidates = true;
	for (const FHyperAIContextSearchManifestEntry* Entry : Entries)
	{
		if (!Entry || Entry->AdmissionState == FHyperAIContextSearchManifestEntry::EAdmissionState::Planned)
		{
			return false;
		}
		const FHyperAIStudioCapabilityToolDefinition* CatalogTool = nullptr;
		int32 MatchCount = 0;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name == Entry->Name)
			{
				CatalogTool = &Tool;
				++MatchCount;
			}
		}
		if (MatchCount != 1 || !CatalogTool) return false;
		bAllAdmitted &= CatalogTool->AdmissionState
			== EHyperAIStudioCapabilityAdmissionState::Admitted;
		bAllSourceCandidates &= Entry->AdmissionState
				== FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate
			&& CatalogTool->AdmissionState
				== EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
	}
	return bAllAdmitted || (bAllowPendingForTests && bAllSourceCandidates);
}

FString FHyperAIStudioContextSearchContracts::AdmissionStateToString(
	const FHyperAIContextSearchManifestEntry::EAdmissionState State)
{
	switch (State)
	{
	case FHyperAIContextSearchManifestEntry::EAdmissionState::Planned:
		return TEXT("planned");
	case FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate:
		return TEXT("source_candidate");
	case FHyperAIContextSearchManifestEntry::EAdmissionState::Admitted:
		return TEXT("admitted");
	default:
		return TEXT("invalid");
	}
}

bool FHyperAIStudioContextSearchContracts::NormalizeProjection(
	const TArray<FString>& Requested,
	const TSet<FString>& Allowed,
	const TArray<FString>& Defaults,
	TArray<FString>& OutProjection,
	FString& OutErrorCode,
	FString& OutError)
{
	return NormalizeStringSet(
		Requested.IsEmpty() ? Defaults : Requested,
		Allowed,
		64,
		false,
		OutProjection,
		OutErrorCode,
		OutError);
}

bool FHyperAIStudioContextSearchContracts::NormalizeStringSet(
	const TArray<FString>& Requested,
	const TSet<FString>& Allowed,
	const int32 HardMax,
	const bool bAllowEmpty,
	TArray<FString>& OutValues,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutValues.Reset();
	OutErrorCode.Reset();
	OutError.Reset();
	if (Requested.Num() > HardMax)
	{
		OutErrorCode = TEXT("too_many_values");
		OutError = TEXT("The request exceeds the fixed collection bound.");
		return false;
	}
	TSet<FString> Unique;
	for (const FString& Raw : Requested)
	{
		if (Raw.Len() > MaxFieldNameCharacters || ContainsEmbeddedNull(Raw)
			|| !HasWellFormedUtf16(Raw))
		{
			OutErrorCode = TEXT("invalid_value");
			OutError = TEXT("A requested value is oversized, malformed, or contains an embedded null.");
			return false;
		}
		const FString Value = NormalizeToken(Raw);
		if (Value.IsEmpty())
		{
			OutErrorCode = TEXT("invalid_value");
			OutError = TEXT("A requested value is empty, oversized, or contains an embedded null.");
			return false;
		}
		if (!Allowed.Contains(Value))
		{
			OutErrorCode = TEXT("value_not_allowlisted");
			OutError = FString::Printf(TEXT("Requested value '%s' is not allowlisted."), *Value);
			return false;
		}
		Unique.Add(Value);
	}
	OutValues = Unique.Array();
	OutValues.Sort();
	if (!bAllowEmpty && OutValues.IsEmpty())
	{
		OutErrorCode = TEXT("empty_value_set");
		OutError = TEXT("At least one allowlisted value is required.");
		return false;
	}
	return true;
}

bool FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(
	const FString& Source,
	FString& OutPreview)
{
	OutPreview.Reset();
	if (Source.Len() > MaxFieldValueCharacters
		|| HyperAIStudio::ContextSearch::Private::ContainsEmbeddedNull(Source)
		|| !HyperAIStudio::ContextSearch::Private::HasWellFormedUtf16(Source))
	{
		return false;
	}
	OutPreview = Source;
	return true;
}

bool FHyperAIStudioContextSearchContracts::TryCopyBoundedTextPreview(
	const FText& Source,
	FString& OutPreview)
{
	// FText::ToString would allocate before the caller could inspect a hostile display-string
	// length. FTextInspector exposes the immutable display FString by const reference first.
	const FString& DisplayString = FTextInspector::GetDisplayString(Source);
	return TryCopyBoundedPreview(DisplayString, OutPreview);
}

FString FHyperAIStudioContextSearchContracts::HashTokens(const TArray<FString>& Tokens)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	if (Tokens.Num() > MaxHashTokens)
	{
		return FString();
	}
	FBoundedSha256 Hasher;
	int32 TokenCount = 0;
	for (const FString& Token : Tokens)
	{
		if (!UpdateHashToken(Hasher, Token, TokenCount))
		{
			return FString();
		}
	}
	return Hasher.Finalize();
}

FString FHyperAIStudioContextSearchContracts::MakeCursor(
	const FString& RequestFingerprint,
	const FString& SnapshotFingerprint,
	const int32 Offset)
{
	if (Offset < 0
		|| !HyperAIStudio::ContextSearch::Private::IsCanonicalSha256(RequestFingerprint)
		|| !HyperAIStudio::ContextSearch::Private::IsCanonicalSha256(SnapshotFingerprint))
	{
		return FString();
	}
	const FString RequestPart = RequestFingerprint.Mid(7, 20);
	const FString SnapshotPart = SnapshotFingerprint.Mid(7, 20);
	const FString Payload = FString::Printf(TEXT("h1:%s:%s:%d"), *RequestPart, *SnapshotPart, Offset);
	const FString Checksum = HashTokens({ Payload }).Mid(7, 16);
	return Payload + TEXT(":") + Checksum;
}

bool FHyperAIStudioContextSearchContracts::ParseCursor(
	const FString& Cursor,
	const FString& RequestFingerprint,
	const FString& SnapshotFingerprint,
	const int32 TotalRecords,
	int32& OutOffset,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutOffset = 0;
	OutErrorCode.Reset();
	OutError.Reset();
	if (Cursor.IsEmpty())
	{
		return true;
	}
	if (Cursor.Len() > MaxCursorCharacters || ContainsEmbeddedNull(Cursor))
	{
		OutErrorCode = TEXT("cursor_too_long");
		OutError = TEXT("Cursor exceeds the fixed input bound.");
		return false;
	}
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() != 5 || Parts[0] != TEXT("h1")
		|| Parts[1].Len() != 20 || Parts[2].Len() != 20 || Parts[4].Len() != 16)
	{
		OutErrorCode = TEXT("cursor_malformed");
		OutError = TEXT("Cursor does not match the versioned HyperAI read-cursor grammar.");
		return false;
	}
	if (Parts[1] != RequestFingerprint.Mid(7, 20))
	{
		OutErrorCode = TEXT("cursor_request_mismatch");
		OutError = TEXT("Cursor belongs to a different normalized request.");
		return false;
	}
	if (Parts[2] != SnapshotFingerprint.Mid(7, 20))
	{
		OutErrorCode = TEXT("cursor_snapshot_changed");
		OutError = TEXT("Cursor belongs to a different immutable snapshot.");
		return false;
	}
	if (!LexTryParseString(OutOffset, *Parts[3]) || OutOffset < 0 || OutOffset > TotalRecords)
	{
		OutErrorCode = TEXT("cursor_offset_invalid");
		OutError = TEXT("Cursor offset is outside the current result range.");
		return false;
	}
	const FString Payload = FString::Printf(TEXT("h1:%s:%s:%d"), *Parts[1], *Parts[2], OutOffset);
	if (HashTokens({ Payload }).Mid(7, 16) != Parts[4])
	{
		OutErrorCode = TEXT("cursor_checksum_mismatch");
		OutError = TEXT("Cursor checksum is invalid.");
		return false;
	}
	return true;
}

FString FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(
	const FHyperAIContextSearchValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	const TArray<FHyperAIContextSearchValueRecord>* Records = &Snapshot.Records;
	TArray<FHyperAIContextSearchValueRecord> CanonicalRecords;
	if (!RecordsAreCanonical(Snapshot.Records))
	{
		CanonicalRecords = Snapshot.Records;
		SortAndDeduplicateRecords(CanonicalRecords);
		Records = &CanonicalRecords;
	}
	const TArray<FString>* DirtyPackages = &Snapshot.DirtyPackages;
	TArray<FString> SortedDirtyPackages;
	if (!StringsAreSorted(Snapshot.DirtyPackages))
	{
		SortedDirtyPackages = Snapshot.DirtyPackages;
		SortedDirtyPackages.Sort();
		DirtyPackages = &SortedDirtyPackages;
	}
	return ComputeCanonicalSnapshotFingerprint(Snapshot, *Records, *DirtyPackages);
}

bool FHyperAIStudioContextSearchContracts::ProjectPage(
	const FHyperAIContextSearchValueSnapshot& Snapshot,
	const TArray<FString>& Projection,
	const FString& RequestFingerprint,
	const int32 PageSize,
	const FString& Cursor,
	const int32 MaxOutputBytesValue,
	FHyperAIReadReport& OutReport)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutReport = FHyperAIReadReport();
	if (PageSize < 1 || PageSize > MaxPageSize
		|| MaxOutputBytesValue < MinOutputBytes || MaxOutputBytesValue > MaxOutputBytes)
	{
		OutReport = InvalidReport(
			TEXT("invalid_bounds"),
			TEXT("page_size"),
			TEXT("PageSize or MaxOutputBytes is outside the fixed supported range."));
		return false;
	}
	if (Snapshot.Records.Num() > MaxProjectIndexRecords)
	{
		OutReport = InvalidReport(
			TEXT("snapshot_record_bound_exceeded"),
			TEXT("snapshot"),
			TEXT("Immutable snapshot exceeds the hard record bound."));
		return false;
	}

	const TArray<FHyperAIContextSearchValueRecord>* Records = &Snapshot.Records;
	TArray<FHyperAIContextSearchValueRecord> CanonicalRecords;
	if (!RecordsAreCanonical(Snapshot.Records))
	{
		CanonicalRecords = Snapshot.Records;
		SortAndDeduplicateRecords(CanonicalRecords);
		Records = &CanonicalRecords;
	}
	const TArray<FString>* DirtyPackages = &Snapshot.DirtyPackages;
	TArray<FString> SortedDirtyPackages;
	if (!StringsAreSorted(Snapshot.DirtyPackages))
	{
		SortedDirtyPackages = Snapshot.DirtyPackages;
		SortedDirtyPackages.Sort();
		DirtyPackages = &SortedDirtyPackages;
	}
	const FString SnapshotFingerprint =
		ComputeCanonicalSnapshotFingerprint(Snapshot, *Records, *DirtyPackages);
	if (!IsCanonicalSha256(RequestFingerprint) || !IsCanonicalSha256(SnapshotFingerprint))
	{
		OutReport = InvalidReport(TEXT("hash_failure"), TEXT("request"), TEXT("Could not bind the request and immutable snapshot."));
		return false;
	}

	int32 Offset = 0;
	FString ErrorCode;
	FString Error;
	if (!ParseCursor(Cursor, RequestFingerprint, SnapshotFingerprint, Records->Num(), Offset, ErrorCode, Error))
	{
		OutReport = InvalidReport(ErrorCode, TEXT("cursor"), Error);
		OutReport.RequestFingerprint = RequestFingerprint;
		OutReport.SnapshotFingerprint = SnapshotFingerprint;
		return false;
	}

	TSet<FString> ProjectionSet(Projection);
	OutReport.SnapshotUtc = Snapshot.SnapshotUtc;
	OutReport.ObservationScope = Snapshot.ObservationScope;
	OutReport.bOnDiskOnly = Snapshot.bOnDiskOnly;
	OutReport.bDirty = Snapshot.bDirty;
	OutReport.bIncomplete = Snapshot.bIncomplete;
	OutReport.bSourceTruncated = Snapshot.bSourceTruncated;
	OutReport.DirtyPackages = Snapshot.DirtyPackages;
	OutReport.DirtyPackageCount = Snapshot.DirtyPackages.Num();
	OutReport.bDirtyPackageListTruncated = Snapshot.bDirtyPackageListTruncated;
	OutReport.RequestFingerprint = RequestFingerprint;
	OutReport.SnapshotFingerprint = SnapshotFingerprint;
	OutReport.SnapshotGeneration = Snapshot.Generation;
	OutReport.PageOffset = Offset;
	OutReport.PageSize = PageSize;
	OutReport.TotalRecords = Records->Num();
	OutReport.Diagnostics = Snapshot.Diagnostics;

	int32 EstimatedBytes = 1024;
	const int32 RequestedEnd = FMath::Min(Offset + PageSize, Records->Num());
	int32 ActualEnd = Offset;
	for (int32 Index = Offset; Index < RequestedEnd; ++Index)
	{
		const FHyperAIContextSearchValueRecord& Source = (*Records)[Index];
		FHyperAIReadRecord Record;
		Record.QueryId = Source.QueryId;
		Record.Kind = Source.Kind;
		Record.RecordId = Source.RecordId;
		for (const FHyperAIReadField& Field : Source.Fields)
		{
			if (ProjectionSet.Contains(Field.Name))
			{
				Record.Fields.Add(Field);
			}
		}
		const int32 RecordBytes = EstimateRecordBytes(Record);
		if (EstimatedBytes + RecordBytes > MaxOutputBytesValue)
		{
			OutReport.bOutputBudgetTruncated = true;
			break;
		}
		EstimatedBytes += RecordBytes;
		OutReport.Records.Add(MoveTemp(Record));
		ActualEnd = Index + 1;
	}
	OutReport.ReturnedRecords = OutReport.Records.Num();
	const bool bHasMore = ActualEnd < Records->Num();
	if (bHasMore && ActualEnd > Offset)
	{
		OutReport.NextCursor = MakeCursor(RequestFingerprint, SnapshotFingerprint, ActualEnd);
	}
	else if (bHasMore && ActualEnd == Offset)
	{
		AddDiagnosticOnce(
			OutReport.Diagnostics,
			TEXT("output_budget_cannot_fit_record"),
			TEXT("error"),
			TEXT("max_output_bytes"),
			TEXT("No projected record fits in the requested output budget; no non-progress cursor is emitted."));
	}
	OutReport.bTruncated = bHasMore || Snapshot.bIncomplete || Snapshot.bSourceTruncated || OutReport.bOutputBudgetTruncated;
	OutReport.Status = OutReport.bTruncated ? TEXT("partial") : TEXT("complete");
	OutReport.Diagnostic = OutReport.bTruncated
		? TEXT("The bounded result is partial; inspect truncation, incomplete, dirty, and cursor fields before inferring absence.")
		: TEXT("The projected immutable snapshot is complete within the declared observation scope.");
	OutReport.bOk = !OutReport.bOutputBudgetTruncated || OutReport.ReturnedRecords > 0 || Records->IsEmpty();
	return OutReport.bOk;
}

namespace HyperAIStudio::ContextSearch::Private
{
#if PLATFORM_WINDOWS
	class FScopedWinHandle final
	{
	public:
		explicit FScopedWinHandle(HANDLE InHandle = INVALID_HANDLE_VALUE)
			: Handle(InHandle)
		{
		}

		~FScopedWinHandle()
		{
			if (IsValid())
			{
				::CloseHandle(Handle);
			}
		}

		FScopedWinHandle(const FScopedWinHandle&) = delete;
		FScopedWinHandle& operator=(const FScopedWinHandle&) = delete;
		FScopedWinHandle(FScopedWinHandle&& Other) noexcept
			: Handle(Other.Handle)
		{
			Other.Handle = INVALID_HANDLE_VALUE;
		}

		FScopedWinHandle& operator=(FScopedWinHandle&& Other) noexcept
		{
			if (this != &Other)
			{
				if (IsValid())
				{
					::CloseHandle(Handle);
				}
				Handle = Other.Handle;
				Other.Handle = INVALID_HANDLE_VALUE;
			}
			return *this;
		}

		bool IsValid() const
		{
			return Handle != nullptr && Handle != INVALID_HANDLE_VALUE;
		}

		HANDLE Get() const
		{
			return Handle;
		}

	private:
		HANDLE Handle = INVALID_HANDLE_VALUE;
	};

	FScopedWinHandle OpenDirectoryHandle(const FString& Path)
	{
		return FScopedWinHandle(::CreateFileW(
			*Path,
			FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			nullptr));
	}

	FScopedWinHandle OpenReadFileHandle(const FString& Path)
	{
		return FScopedWinHandle(::CreateFileW(
			*Path,
			GENERIC_READ | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr));
	}

	bool GetFinalHandlePath(const HANDLE Handle, FString& OutPath)
	{
		OutPath.Reset();
		if (!Handle || Handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		static constexpr DWORD MaxFinalPathCharacters = 32768;
		TArray<WCHAR> Buffer;
		Buffer.SetNumUninitialized(MaxFinalPathCharacters);
		const DWORD Length = ::GetFinalPathNameByHandleW(
			Handle,
			Buffer.GetData(),
			MaxFinalPathCharacters,
			FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (Length == 0 || Length >= MaxFinalPathCharacters)
		{
			return false;
		}
		OutPath = FString(static_cast<int32>(Length), Buffer.GetData());
		OutPath.ReplaceInline(TEXT("/"), TEXT("\\"));
		while (OutPath.EndsWith(TEXT("\\")) && OutPath.Len() > 4)
		{
			OutPath.LeftChopInline(1);
		}
		return !OutPath.IsEmpty();
	}

	bool IsFinalPathUnderRoot(
		const FString& FinalRoot,
		const FString& FinalCandidate,
		FString* OutRelativePath = nullptr)
	{
		const FString RootPrefix = FinalRoot.EndsWith(TEXT("\\"))
			? FinalRoot
			: FinalRoot + TEXT("\\");
		if (!FinalCandidate.StartsWith(RootPrefix, ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (OutRelativePath)
		{
			*OutRelativePath = FinalCandidate.Mid(RootPrefix.Len());
			OutRelativePath->ReplaceInline(TEXT("\\"), TEXT("/"));
		}
		return true;
	}

	struct FWinDirectoryEntry
	{
		FString Name;
		DWORD Attributes = 0;
	};

	bool EnumerateDirectoryHandleBounded(
		const HANDLE DirectoryHandle,
		const int32 MaxEntries,
		TArray<FWinDirectoryEntry>& OutEntries,
		bool& bOutTruncated)
	{
		OutEntries.Reset();
		bOutTruncated = false;
		if (!DirectoryHandle || DirectoryHandle == INVALID_HANDLE_VALUE || MaxEntries < 0)
		{
			return false;
		}
		alignas(8) uint8 Buffer[64 * 1024];
		bool bRestart = true;
		for (;;)
		{
			const FILE_INFO_BY_HANDLE_CLASS InfoClass = bRestart
				? FileIdBothDirectoryRestartInfo
				: FileIdBothDirectoryInfo;
			if (!::GetFileInformationByHandleEx(DirectoryHandle, InfoClass, Buffer, sizeof(Buffer)))
			{
				const DWORD Error = ::GetLastError();
				return Error == ERROR_NO_MORE_FILES;
			}
			bRestart = false;
			const FILE_ID_BOTH_DIR_INFO* Info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(Buffer);
			for (;;)
			{
				const int32 NameCharacters = static_cast<int32>(Info->FileNameLength / sizeof(WCHAR));
				if (NameCharacters > 0 && NameCharacters <= 255)
				{
					const FString Name(NameCharacters, Info->FileName);
					if (Name != TEXT(".") && Name != TEXT(".."))
					{
						if (OutEntries.Num() >= MaxEntries)
						{
							bOutTruncated = true;
							return true;
						}
						OutEntries.Add({ Name, Info->FileAttributes });
					}
				}
				if (Info->NextEntryOffset == 0)
				{
					break;
				}
				Info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
					reinterpret_cast<const uint8*>(Info) + Info->NextEntryOffset);
			}
		}
	}

	bool OpenValidatedDirectory(
		const FString& LexicalPath,
		const FString& FinalProjectRoot,
		FScopedWinHandle& OutHandle,
		FString& OutFinalPath)
	{
		FScopedWinHandle Handle = OpenDirectoryHandle(LexicalPath);
		if (!Handle.IsValid() || !GetFinalHandlePath(Handle.Get(), OutFinalPath)
			|| !IsFinalPathUnderRoot(FinalProjectRoot, OutFinalPath))
		{
			return false;
		}
		OutHandle = MoveTemp(Handle);
		return true;
	}
#endif
}

bool FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
	const FString& ProjectRoot,
	const FString& CandidatePath,
	FString& OutRelativePath)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutRelativePath.Reset();
	if (ProjectRoot.IsEmpty() || CandidatePath.IsEmpty()
		|| ContainsEmbeddedNull(ProjectRoot) || ContainsEmbeddedNull(CandidatePath)
		|| !HasWellFormedUtf16(ProjectRoot) || !HasWellFormedUtf16(CandidatePath)
		|| !IsCppExtension(CandidatePath))
	{
		return false;
	}
#if PLATFORM_WINDOWS
	const FScopedWinHandle RootHandle = OpenDirectoryHandle(FPaths::ConvertRelativePathToFull(ProjectRoot));
	const FScopedWinHandle CandidateHandle = OpenReadFileHandle(FPaths::ConvertRelativePathToFull(CandidatePath));
	FString FinalRoot;
	FString FinalCandidate;
	BY_HANDLE_FILE_INFORMATION CandidateInfo{};
	if (!RootHandle.IsValid() || !CandidateHandle.IsValid()
		|| !GetFinalHandlePath(RootHandle.Get(), FinalRoot)
		|| !GetFinalHandlePath(CandidateHandle.Get(), FinalCandidate)
		|| !::GetFileInformationByHandle(CandidateHandle.Get(), &CandidateInfo)
		|| (CandidateInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
		|| CandidateInfo.nNumberOfLinks != 1
		|| !IsCppExtension(FinalCandidate)
		|| !IsFinalPathUnderRoot(FinalRoot, FinalCandidate, &OutRelativePath))
	{
		OutRelativePath.Reset();
		return false;
	}
	return !OutRelativePath.IsEmpty();
#else
	// Shipping is Win64. Unsupported platforms fail closed; lexical containment is not a security boundary.
	return false;
#endif
}

bool FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
	const FString& ProjectRoot,
	const int32 MaxDirectories,
	const int32 MaxEntries,
	TArray<FString>& OutCandidatePaths,
	bool& bOutTruncated,
	FString& OutErrorCode)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutCandidatePaths.Reset();
	bOutTruncated = false;
	OutErrorCode.Reset();
	if (ProjectRoot.IsEmpty() || ContainsEmbeddedNull(ProjectRoot) || !HasWellFormedUtf16(ProjectRoot)
		|| MaxDirectories < 1 || MaxDirectories > MaxCppDirectories
		|| MaxEntries < 1 || MaxEntries > MaxCppDirectoryEntries)
	{
		OutErrorCode = TEXT("invalid_cpp_enumeration_bounds");
		return false;
	}
#if PLATFORM_WINDOWS
	const FString FullProjectRoot = FPaths::ConvertRelativePathToFull(ProjectRoot);
	const FScopedWinHandle ProjectHandle = OpenDirectoryHandle(FullProjectRoot);
	FString FinalProjectRoot;
	if (!ProjectHandle.IsValid() || !GetFinalHandlePath(ProjectHandle.Get(), FinalProjectRoot))
	{
		OutErrorCode = TEXT("project_root_handle_unavailable");
		return false;
	}

	TArray<FString> PendingDirectories;
	auto TryAddSourceRoot = [&](const FString& Candidate)
	{
		FScopedWinHandle CandidateHandle;
		FString FinalCandidate;
		if (OpenValidatedDirectory(Candidate, FinalProjectRoot, CandidateHandle, FinalCandidate))
		{
			PendingDirectories.AddUnique(Candidate);
		}
	};
	TryAddSourceRoot(FPaths::Combine(FullProjectRoot, TEXT("Source")));

	const FString PluginsRoot = FPaths::Combine(FullProjectRoot, TEXT("Plugins"));
	FScopedWinHandle PluginsHandle;
	FString FinalPluginsPath;
	if (OpenValidatedDirectory(PluginsRoot, FinalProjectRoot, PluginsHandle, FinalPluginsPath))
	{
		TArray<FWinDirectoryEntry> PluginEntries;
		bool bPluginEntriesTruncated = false;
		if (!EnumerateDirectoryHandleBounded(
			PluginsHandle.Get(),
			FMath::Min(MaxEntries, 512),
			PluginEntries,
			bPluginEntriesTruncated))
		{
			OutErrorCode = TEXT("plugin_directory_enumeration_failed");
			return false;
		}
		PluginEntries.Sort([](const FWinDirectoryEntry& Left, const FWinDirectoryEntry& Right)
		{
			return Left.Name < Right.Name;
		});
		bOutTruncated |= bPluginEntriesTruncated;
		for (const FWinDirectoryEntry& Entry : PluginEntries)
		{
			if ((Entry.Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				continue;
			}
			if ((Entry.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				bOutTruncated = true;
				OutErrorCode = TEXT("reparse_directory_rejected");
				continue;
			}
			TryAddSourceRoot(FPaths::Combine(PluginsRoot, Entry.Name, TEXT("Source")));
		}
	}

	PendingDirectories.Sort();
	TSet<FString> VisitedFinalDirectories;
	int32 DirectoriesProcessed = 0;
	int32 EntriesObserved = 0;
	while (!PendingDirectories.IsEmpty())
	{
		if (DirectoriesProcessed >= MaxDirectories || EntriesObserved >= MaxEntries)
		{
			bOutTruncated = true;
			OutErrorCode = TEXT("cpp_directory_work_bound_reached");
			break;
		}
		const FString Directory = PendingDirectories[0];
		PendingDirectories.RemoveAt(0);
		FScopedWinHandle DirectoryHandle;
		FString FinalDirectory;
		if (!OpenValidatedDirectory(Directory, FinalProjectRoot, DirectoryHandle, FinalDirectory))
		{
			bOutTruncated = true;
			OutErrorCode = TEXT("cpp_directory_containment_rejected");
			continue;
		}
		if (VisitedFinalDirectories.Contains(FinalDirectory))
		{
			continue;
		}
		VisitedFinalDirectories.Add(FinalDirectory);
		++DirectoriesProcessed;

		TArray<FWinDirectoryEntry> Entries;
		bool bDirectoryTruncated = false;
		if (!EnumerateDirectoryHandleBounded(
			DirectoryHandle.Get(),
			MaxEntries - EntriesObserved,
			Entries,
			bDirectoryTruncated))
		{
			bOutTruncated = true;
			OutErrorCode = TEXT("cpp_directory_enumeration_failed");
			continue;
		}
		Entries.Sort([](const FWinDirectoryEntry& Left, const FWinDirectoryEntry& Right)
		{
			return Left.Name < Right.Name;
		});
		bOutTruncated |= bDirectoryTruncated;
		EntriesObserved += Entries.Num();
		for (const FWinDirectoryEntry& Entry : Entries)
		{
			const FString Child = FPaths::Combine(Directory, Entry.Name);
			if ((Entry.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				bOutTruncated = true;
				OutErrorCode = TEXT("reparse_entry_rejected");
				continue;
			}
			if ((Entry.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				PendingDirectories.Add(Child);
			}
			else if (IsCppExtension(Child))
			{
				OutCandidatePaths.Add(Child);
			}
		}
		PendingDirectories.Sort();
		if (bDirectoryTruncated)
		{
			OutErrorCode = TEXT("cpp_directory_entry_bound_reached");
			break;
		}
	}
	OutCandidatePaths.Sort();
	if (OutCandidatePaths.Num() > MaxCppFiles)
	{
		OutCandidatePaths.SetNum(MaxCppFiles);
		bOutTruncated = true;
		OutErrorCode = TEXT("cpp_file_bound_reached");
	}
	return true;
#else
	OutErrorCode = TEXT("secure_source_enumeration_unsupported_platform");
	return false;
#endif
}

bool FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
	const FString& ProjectRoot,
	const FString& CandidatePath,
	FHyperAIProjectSourceReadResult& OutRead,
	FString& OutErrorCode,
	const int64 MaxBytes)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutRead = FHyperAIProjectSourceReadResult();
	OutErrorCode.Reset();
	if (ProjectRoot.IsEmpty() || CandidatePath.IsEmpty()
		|| ContainsEmbeddedNull(ProjectRoot) || ContainsEmbeddedNull(CandidatePath)
		|| !HasWellFormedUtf16(ProjectRoot) || !HasWellFormedUtf16(CandidatePath)
		|| !IsCppExtension(CandidatePath) || MaxBytes < 0 || MaxBytes > MaxCppFileBytes)
	{
		OutErrorCode = TEXT("invalid_source_path");
		return false;
	}
#if PLATFORM_WINDOWS
	const FScopedWinHandle RootHandle = OpenDirectoryHandle(FPaths::ConvertRelativePathToFull(ProjectRoot));
	const FScopedWinHandle SourceHandle = OpenReadFileHandle(FPaths::ConvertRelativePathToFull(CandidatePath));
	FString FinalRoot;
	FString FinalSource;
	if (!RootHandle.IsValid() || !SourceHandle.IsValid()
		|| !GetFinalHandlePath(RootHandle.Get(), FinalRoot)
		|| !GetFinalHandlePath(SourceHandle.Get(), FinalSource)
		|| !IsCppExtension(FinalSource)
		|| !IsFinalPathUnderRoot(FinalRoot, FinalSource, &OutRead.RelativePath))
	{
		OutErrorCode = TEXT("source_final_path_outside_project");
		return false;
	}
	BY_HANDLE_FILE_INFORMATION Info{};
	LARGE_INTEGER Size{};
	if (!::GetFileInformationByHandle(SourceHandle.Get(), &Info)
		|| (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		OutErrorCode = TEXT("source_file_identity_rejected");
		return false;
	}
	if (Info.nNumberOfLinks != 1)
	{
		OutErrorCode = TEXT("source_hardlink_rejected");
		return false;
	}
	if (!::GetFileSizeEx(SourceHandle.Get(), &Size)
		|| Size.QuadPart < 0 || Size.QuadPart > MaxBytes)
	{
		OutErrorCode = TEXT("source_file_size_bound_rejected");
		return false;
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size.QuadPart));
	int32 TotalRead = 0;
	while (TotalRead < Bytes.Num())
	{
		DWORD ChunkRead = 0;
		const DWORD Remaining = static_cast<DWORD>(Bytes.Num() - TotalRead);
		if (!::ReadFile(SourceHandle.Get(), Bytes.GetData() + TotalRead, Remaining, &ChunkRead, nullptr)
			|| ChunkRead == 0 || ChunkRead > Remaining)
		{
			OutErrorCode = TEXT("source_handle_read_failed");
			return false;
		}
		TotalRead += static_cast<int32>(ChunkRead);
	}
	OutRead.BytesRead = TotalRead;
	if (Bytes.IsEmpty())
	{
		return true;
	}

	if (Bytes.Num() >= 2 && Bytes[0] == 0xff && Bytes[1] == 0xfe)
	{
		const int32 PayloadBytes = Bytes.Num() - 2;
		if (!IsWellFormedUtf16Le(Bytes.GetData() + 2, PayloadBytes))
		{
			OutErrorCode = TEXT("invalid_utf16_source");
			return false;
		}
		const FUTF16ToTCHAR Converter(
			reinterpret_cast<const UTF16CHAR*>(Bytes.GetData() + 2),
			PayloadBytes / 2);
		OutRead.Text = FString(Converter.Length(), Converter.Get());
	}
	else if (Bytes.Num() >= 2 && Bytes[0] == 0xfe && Bytes[1] == 0xff)
	{
		OutErrorCode = TEXT("utf16_big_endian_not_supported");
		return false;
	}
	else
	{
		int32 Offset = 0;
		if (Bytes.Num() >= 3 && Bytes[0] == 0xef && Bytes[1] == 0xbb && Bytes[2] == 0xbf)
		{
			Offset = 3;
		}
		if (!IsWellFormedUtf8(Bytes.GetData() + Offset, Bytes.Num() - Offset))
		{
			OutErrorCode = TEXT("invalid_utf8_source");
			return false;
		}
		const FUTF8ToTCHAR Converter(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			Bytes.Num() - Offset);
		OutRead.Text = FString(Converter.Length(), Converter.Get());
	}
	if (OutRead.Text.Len() > MaxBytes || !HasWellFormedUtf16(OutRead.Text)
		|| ContainsEmbeddedNull(OutRead.Text))
	{
		OutRead.Text.Reset();
		OutErrorCode = TEXT("decoded_source_bound_or_null_rejected");
		return false;
	}
	return true;
#else
	OutErrorCode = TEXT("secure_source_read_unsupported_platform");
	return false;
#endif
}

TArray<FHyperAIContextSearchValueRecord> FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
	const FString& ProjectRelativePath,
	const FString& SourceText,
	const int32 MaxSymbols,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	bOutTruncated = false;
	TArray<FHyperAIContextSearchValueRecord> Records;
	if (ProjectRelativePath.IsEmpty() || ProjectRelativePath.Len() > MaxPathCharacters
		|| SourceText.Len() > MaxCppFileBytes || MaxSymbols < 0 || MaxSymbols > MaxCppSymbols)
	{
		bOutTruncated = !SourceText.IsEmpty();
		return Records;
	}

	if (MaxSymbols == 0)
	{
		bOutTruncated = !SourceText.IsEmpty();
		return Records;
	}
	static const TSet<FString> FunctionReject = {
		TEXT("if"), TEXT("for"), TEXT("while"), TEXT("switch"), TEXT("return"), TEXT("sizeof"),
		TEXT("alignof"), TEXT("decltype"), TEXT("static_cast"), TEXT("reinterpret_cast"),
		TEXT("const_cast"), TEXT("dynamic_cast"), TEXT("check"), TEXT("ensure"), TEXT("TEXT")
	};
	TSet<FString> SeenIds;
	auto AddSymbol = [&](const FString& Name, const TCHAR* Kind, const int32 LineNumber)
	{
		if (!IsValidIdentifier(Name, 128) || Records.Num() >= MaxSymbols)
		{
			bOutTruncated |= Records.Num() >= MaxSymbols;
			return;
		}
		const FString IdSource = ProjectRelativePath + TEXT("|") + Kind + TEXT("|") + Name + TEXT("|") + LexToString(LineNumber);
		const FString Id = HashSingle(IdSource);
		if (Id.IsEmpty() || SeenIds.Contains(Id))
		{
			return;
		}
		SeenIds.Add(Id);
		FHyperAIContextSearchValueRecord& Record = Records.AddDefaulted_GetRef();
		Record.Kind = TEXT("cpp_symbol");
		Record.RecordId = Id;
		AddField(Record, TEXT("name"), TEXT("name"), Name);
		AddField(Record, TEXT("symbol_kind"), TEXT("string"), Kind);
		AddField(Record, TEXT("relative_path"), TEXT("project_relative_path"), ProjectRelativePath);
		AddField(Record, TEXT("line"), TEXT("integer"), LexToString(LineNumber));
		Record.SearchTextLower = (Name + TEXT(" ") + Kind + TEXT(" ") + ProjectRelativePath).ToLower();
	};

	static constexpr int32 MaxSourceLinesScanned = 65536;
	static constexpr int32 MaxSourceLineCharacters = 4096;
	static constexpr double MaxSymbolScanSeconds = 0.050;
	const double ScanStartSeconds = FPlatformTime::Seconds();
	int32 Cursor = 0;
	int32 LineNumber = 0;
	while (Cursor < SourceText.Len() && LineNumber < MaxSourceLinesScanned)
	{
		if (Records.Num() >= MaxSymbols)
		{
			bOutTruncated = Cursor < SourceText.Len();
			break;
		}
		if ((LineNumber & 0xff) == 0
			&& FPlatformTime::Seconds() - ScanStartSeconds > MaxSymbolScanSeconds)
		{
			bOutTruncated = true;
			break;
		}
		const int32 LineStart = Cursor;
		while (Cursor < SourceText.Len()
			&& SourceText[Cursor] != TEXT('\n')
			&& SourceText[Cursor] != TEXT('\r'))
		{
			++Cursor;
		}
		const int32 LineLength = Cursor - LineStart;
		if (Cursor < SourceText.Len() && SourceText[Cursor] == TEXT('\r'))
		{
			++Cursor;
			if (Cursor < SourceText.Len() && SourceText[Cursor] == TEXT('\n'))
			{
				++Cursor;
			}
		}
		else if (Cursor < SourceText.Len() && SourceText[Cursor] == TEXT('\n'))
		{
			++Cursor;
		}
		++LineNumber;
		if (LineLength == 0)
		{
			continue;
		}
		if (LineLength > MaxSourceLineCharacters)
		{
			bOutTruncated = true;
			continue;
		}
		FString Line = SourceText.Mid(LineStart, LineLength);
		int32 CommentIndex = INDEX_NONE;
		if (Line.FindChar(TEXT('/'), CommentIndex)
			&& Line.IsValidIndex(CommentIndex + 1)
			&& Line[CommentIndex + 1] == TEXT('/'))
		{
			Line.LeftInline(CommentIndex);
		}
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}

		const struct { const TCHAR* Keyword; const TCHAR* Kind; } TypeKeywords[] = {
			{ TEXT("class "), TEXT("class") },
			{ TEXT("struct "), TEXT("struct") },
			{ TEXT("enum class "), TEXT("enum_class") },
			{ TEXT("enum "), TEXT("enum") }
		};
		for (const auto& Entry : TypeKeywords)
		{
			const int32 KeywordIndex = Line.Find(Entry.Keyword, ESearchCase::CaseSensitive);
			if (KeywordIndex == INDEX_NONE)
			{
				continue;
			}
			FString Name = ReadIdentifierAfter(Line, KeywordIndex + FCString::Strlen(Entry.Keyword));
			if (Name.EndsWith(TEXT("_API")))
			{
				const int32 ApiIndex = Line.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, KeywordIndex);
				Name = ReadIdentifierAfter(Line, ApiIndex + Name.Len());
			}
			AddSymbol(Name, Entry.Kind, LineNumber);
			break;
		}

		const int32 OpenParen = Line.Find(TEXT("("));
		const int32 CloseParen = OpenParen == INDEX_NONE ? INDEX_NONE : Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenParen + 1);
		if (OpenParen > 0 && CloseParen > OpenParen)
		{
			const FString FunctionName = ReadIdentifierBefore(Line, OpenParen);
			if (!FunctionName.IsEmpty()
				&& !FunctionReject.Contains(FunctionName)
				&& !FunctionName.StartsWith(TEXT("UE_"))
				&& !FunctionName.EndsWith(TEXT("_LOG")))
			{
				AddSymbol(FunctionName, TEXT("function_candidate"), LineNumber);
			}
		}
	}
	bOutTruncated |= Cursor < SourceText.Len();
	SortAndDeduplicateRecords(Records);
	return Records;
}

namespace HyperAIStudio::ContextSearch::Private
{
	FHyperAIContextSearchValueRecord MakeProjectRecord()
	{
		FHyperAIContextSearchValueRecord Record;
		Record.Kind = TEXT("project");
		Record.RecordId = TEXT("project");
		const FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		AddField(Record, TEXT("project_name"), TEXT("name"), FApp::GetProjectName());
		AddField(Record, TEXT("project_file"), TEXT("path"), ProjectFile);
		AddField(Record, TEXT("project_root"), TEXT("path"), ProjectRoot);
		Record.SearchTextLower = (FString(FApp::GetProjectName()) + TEXT(" ") + ProjectFile).ToLower();
		return Record;
	}

	void AddWorldAndViewportContext(
		FHyperAIContextSearchValueSnapshot& Snapshot,
		const bool bIncludeWorld,
		const bool bIncludeViewport)
	{
		if (!bIncludeWorld && !bIncludeViewport)
		{
			return;
		}
		if (!GEditor)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("editor_unavailable"),
				TEXT("error"),
				TEXT("editor"),
				TEXT("The Unreal editor is unavailable."));
			return;
		}

		if (bIncludeWorld)
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				Snapshot.bIncomplete = true;
				AddDiagnostic(
					Snapshot.Diagnostics,
					TEXT("editor_world_unavailable"),
					TEXT("warning"),
					TEXT("world"),
					TEXT("No loaded editor world is available."));
			}
			else
			{
				FHyperAIContextSearchValueRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
				Record.Kind = TEXT("world");
				Record.RecordId = Clip(World->GetPathName(), MaxPathCharacters);
				const UPackage* Package = World->GetPackage();
				const FString PackageName = Package ? Package->GetName() : FString();
				AddField(Record, TEXT("map_name"), TEXT("name"), World->GetMapName());
				AddField(Record, TEXT("world_path"), TEXT("object_path"), World->GetPathName());
				AddField(Record, TEXT("package_name"), TEXT("package_name"), PackageName);
				AddField(Record, TEXT("dirty"), TEXT("bool"), BoolToken(Package && Package->IsDirty()));
				Record.SearchTextLower = (World->GetMapName() + TEXT(" ") + World->GetPathName()).ToLower();
			}
		}

		if (bIncludeViewport && GCurrentLevelEditingViewportClient)
		{
			FHyperAIContextSearchValueRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("viewport");
			Record.RecordId = TEXT("current_level_viewport");
			AddField(Record, TEXT("location"), TEXT("vector3"), VectorToken(GCurrentLevelEditingViewportClient->GetViewLocation()));
			AddField(Record, TEXT("rotation"), TEXT("rotator"), RotatorToken(GCurrentLevelEditingViewportClient->GetViewRotation()));
			AddField(Record, TEXT("scale"), TEXT("vector3"), TEXT("1,1,1"));
		}
		else if (bIncludeViewport)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("viewport_unavailable"),
				TEXT("info"),
				TEXT("viewport"),
				TEXT("No current loaded level viewport is available."));
		}
	}

	void AddActorContextRecord(
		const AActor* Actor,
		const TCHAR* Kind,
		FHyperAIContextSearchValueSnapshot& Snapshot)
	{
		if (!Actor)
		{
			return;
		}
		FString BoundedActorLabel;
		if (!FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(
			Actor->GetActorLabel(false), BoundedActorLabel))
		{
			Snapshot.bIncomplete = true;
			AddDiagnosticOnce(
				Snapshot.Diagnostics,
				TEXT("actor_label_preview_unbounded"),
				TEXT("warning"),
				TEXT("label"),
				TEXT("An oversized or malformed actor label was omitted before copying."));
		}
		FHyperAIContextSearchValueRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
		Record.Kind = Kind;
		Record.RecordId = Clip(Actor->GetPathName(), MaxPathCharacters);
		AddField(Record, TEXT("name"), TEXT("name"), Actor->GetName());
		AddField(Record, TEXT("label"), TEXT("string"), BoundedActorLabel);
		AddField(Record, TEXT("class_path"), TEXT("class_path"), Actor->GetClass()->GetPathName());
		AddField(Record, TEXT("path"), TEXT("object_path"), Actor->GetPathName());
		AddField(Record, TEXT("location"), TEXT("vector3"), VectorToken(Actor->GetActorLocation()));
		AddField(Record, TEXT("rotation"), TEXT("rotator"), RotatorToken(Actor->GetActorRotation()));
		AddField(Record, TEXT("scale"), TEXT("vector3"), VectorToken(Actor->GetActorScale3D()));
		Record.SearchTextLower = (Actor->GetName() + TEXT(" ") + BoundedActorLabel
			+ TEXT(" ") + Actor->GetClass()->GetPathName() + TEXT(" ") + Actor->GetPathName()).ToLower();
	}

	void AddSelectedActors(
		FHyperAIContextSearchValueSnapshot& Snapshot,
		const int32 MaxActors)
	{
		if (!GEditor)
		{
			return;
		}
		USelection* Selection = GEditor->GetSelectedActors();
		if (!Selection)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("actor_selection_unavailable"),
				TEXT("warning"),
				TEXT("selected_actors"),
				TEXT("Selected actor state is unavailable."));
			return;
		}

		int32 Captured = 0;
		int32 Scanned = 0;
		for (FSelectionIterator Iterator(*Selection);
			Iterator && Scanned < FHyperAIStudioContextSearchContracts::MaxWorldActorsScanned;
			++Iterator, ++Scanned)
		{
			if (const AActor* Actor = Cast<AActor>(*Iterator))
			{
				if (Captured >= MaxActors)
				{
					break;
				}
				if (Snapshot.Records.Num() >= MaxSceneSnapshotRecords)
				{
					Snapshot.bIncomplete = true;
					Snapshot.bSourceTruncated = true;
					AddDiagnosticOnce(
						Snapshot.Diagnostics,
						TEXT("scene_snapshot_record_bound_reached"),
						TEXT("warning"),
						TEXT("records"),
						TEXT("Scene identity, component, and property records reached the fixed aggregate snapshot bound."));
					break;
				}
				AddActorContextRecord(Actor, TEXT("selected_actor"), Snapshot);
				++Captured;
			}
		}
		if (Selection->Num() > Scanned || Selection->Num() > Captured)
		{
			Snapshot.bSourceTruncated = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("selected_actor_bound_reached"),
				TEXT("warning"),
				TEXT("max_selected_actors"),
				TEXT("Selected actors exceeded the fixed requested/source bound."));
		}
	}

	void AddBlueprintGraphSelection(
		FHyperAIContextSearchValueSnapshot& Snapshot,
		const int32 MaxNodes,
		const bool bIncludePins)
	{
		int32 BlueprintsScanned = 0;
		int32 GraphsScanned = 0;
		int32 NodesCaptured = 0;
		int32 PinsCaptured = 0;
		TSet<FString> AddedGraphs;
		const double StartSeconds = FPlatformTime::Seconds();

		for (TObjectIterator<UBlueprint> BlueprintIterator;
			BlueprintIterator && BlueprintsScanned < MaxBlueprintsScanned;
			++BlueprintIterator, ++BlueprintsScanned)
		{
			UBlueprint* Blueprint = *BlueprintIterator;
			if (!Blueprint || Blueprint->HasAnyFlags(RF_ClassDefaultObject))
			{
				continue;
			}
			TArray<UEdGraph*> Graphs;
			const int32 RemainingGraphBudget = FMath::Max(0, MaxBlueprintGraphsScanned - GraphsScanned);
			Graphs.Reserve(RemainingGraphBudget);
			auto AppendGraphs = [&Graphs, RemainingGraphBudget, &Snapshot](const TArray<TObjectPtr<UEdGraph>>& SourceGraphs)
			{
				for (UEdGraph* Graph : SourceGraphs)
				{
					if (Graphs.Num() >= RemainingGraphBudget)
					{
						Snapshot.bSourceTruncated = true;
						break;
					}
					Graphs.Add(Graph);
				}
			};
			AppendGraphs(Blueprint->UbergraphPages);
			AppendGraphs(Blueprint->FunctionGraphs);
			AppendGraphs(Blueprint->MacroGraphs);
			AppendGraphs(Blueprint->DelegateSignatureGraphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph || GraphsScanned >= MaxBlueprintGraphsScanned
					|| FPlatformTime::Seconds() - StartSeconds > MaxEditorCaptureSeconds)
				{
					Snapshot.bSourceTruncated |= GraphsScanned >= MaxBlueprintGraphsScanned;
					break;
				}
				++GraphsScanned;
				const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
				if (!GraphEditor.IsValid())
				{
					continue;
				}
				const FGraphPanelSelectionSet& Selection = GraphEditor->GetSelectedNodes();
				if (Selection.IsEmpty())
				{
					continue;
				}

				const FString GraphPath = Clip(Graph->GetPathName(), MaxPathCharacters);
				if (!AddedGraphs.Contains(GraphPath))
				{
					AddedGraphs.Add(GraphPath);
					FHyperAIContextSearchValueRecord& GraphRecord = Snapshot.Records.AddDefaulted_GetRef();
					GraphRecord.Kind = TEXT("blueprint_graph");
					GraphRecord.RecordId = GraphPath;
					AddField(GraphRecord, TEXT("blueprint_path"), TEXT("object_path"), Blueprint->GetPathName());
					AddField(GraphRecord, TEXT("graph_name"), TEXT("name"), Graph->GetName());
					AddField(GraphRecord, TEXT("graph_path"), TEXT("object_path"), GraphPath);
					AddField(GraphRecord, TEXT("graph_guid"), TEXT("guid"), GraphGuidToken(Graph->GraphGuid));
					GraphRecord.SearchTextLower = (Blueprint->GetPathName() + TEXT(" ") + Graph->GetName()).ToLower();
				}

				const int32 RemainingNodeBudget = FMath::Max(0, MaxNodes - NodesCaptured);
				TArray<UEdGraphNode*> SelectedNodes;
				SelectedNodes.Reserve(RemainingNodeBudget);
				auto NodeLess = [](const UEdGraphNode* Left, const UEdGraphNode* Right)
				{
					if (!Left || !Right)
					{
						return Left != nullptr;
					}
					const FString LeftGuid = GraphGuidToken(Left->NodeGuid);
					const FString RightGuid = GraphGuidToken(Right->NodeGuid);
					return LeftGuid == RightGuid ? Left->GetPathName() < Right->GetPathName() : LeftGuid < RightGuid;
				};
				int32 SelectionNodesScanned = 0;
				for (UObject* SelectedObject : Selection)
				{
					if (SelectionNodesScanned >= MaxBlueprintSelectionNodesScanned
						|| FPlatformTime::Seconds() - StartSeconds > MaxEditorCaptureSeconds)
					{
						Snapshot.bSourceTruncated = true;
						break;
					}
					++SelectionNodesScanned;
					if (UEdGraphNode* Node = Cast<UEdGraphNode>(SelectedObject))
					{
						if (Node->GetGraph() == Graph)
						{
							if (RemainingNodeBudget == 0)
							{
								Snapshot.bSourceTruncated = true;
								continue;
							}
							if (SelectedNodes.Num() < RemainingNodeBudget)
							{
								SelectedNodes.Add(Node);
							}
							else
							{
								Snapshot.bSourceTruncated = true;
								int32 LargestIndex = 0;
								for (int32 CandidateIndex = 1; CandidateIndex < SelectedNodes.Num(); ++CandidateIndex)
								{
									if (NodeLess(SelectedNodes[LargestIndex], SelectedNodes[CandidateIndex]))
									{
										LargestIndex = CandidateIndex;
									}
								}
								if (NodeLess(Node, SelectedNodes[LargestIndex]))
								{
									SelectedNodes[LargestIndex] = Node;
								}
							}
						}
					}
				}
				SelectedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
				{
					const FString LeftGuid = GraphGuidToken(Left.NodeGuid);
					const FString RightGuid = GraphGuidToken(Right.NodeGuid);
					return LeftGuid == RightGuid ? Left.GetPathName() < Right.GetPathName() : LeftGuid < RightGuid;
				});

				for (UEdGraphNode* Node : SelectedNodes)
				{
					if (NodesCaptured >= MaxNodes)
					{
						Snapshot.bSourceTruncated = true;
						break;
					}
					const FString NodeGuid = GraphGuidToken(Node->NodeGuid);
					FHyperAIContextSearchValueRecord& NodeRecord = Snapshot.Records.AddDefaulted_GetRef();
					NodeRecord.Kind = TEXT("blueprint_node");
					NodeRecord.RecordId = HashSingle(GraphPath + TEXT("|node|") + NodeGuid + TEXT("|") + Node->GetPathName());
					AddField(NodeRecord, TEXT("blueprint_path"), TEXT("object_path"), Blueprint->GetPathName());
					AddField(NodeRecord, TEXT("graph_name"), TEXT("name"), Graph->GetName());
					AddField(NodeRecord, TEXT("graph_path"), TEXT("object_path"), GraphPath);
					AddField(NodeRecord, TEXT("graph_guid"), TEXT("guid"), GraphGuidToken(Graph->GraphGuid));
					AddField(NodeRecord, TEXT("node_guid"), TEXT("guid"), NodeGuid);
					AddField(NodeRecord, TEXT("class_path"), TEXT("class_path"), Node->GetClass()->GetPathName());
					AddField(NodeRecord, TEXT("path"), TEXT("object_path"), Node->GetPathName());
					if (Node->NodeComment.Len() <= FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters
						&& !ContainsEmbeddedNull(Node->NodeComment))
					{
						AddField(NodeRecord, TEXT("node_comment"), TEXT("string"), Node->NodeComment);
					}
					else
					{
						Snapshot.bIncomplete = true;
						AddDiagnosticOnce(
							Snapshot.Diagnostics,
							TEXT("blueprint_node_comment_preview_unbounded"),
							TEXT("warning"),
							TEXT("node_comment"),
							TEXT("An oversized or invalid selected-node comment was omitted before copying."));
					}
					AddField(NodeRecord, TEXT("node_position"), TEXT("int2"), FString::Printf(TEXT("%d,%d"), Node->NodePosX, Node->NodePosY));
					NodeRecord.SearchTextLower = (Graph->GetName() + TEXT(" ")
						+ Node->GetClass()->GetPathName()).ToLower();
					Snapshot.bIncomplete = true;
					AddDiagnosticOnce(
						Snapshot.Diagnostics,
						TEXT("blueprint_node_title_provider_not_hard_bounded"),
						TEXT("info"),
						TEXT("node_title"),
						TEXT("Selected-node titles are omitted because the virtual title provider can allocate before a caller-enforced bound."));
					++NodesCaptured;

					if (!bIncludePins)
					{
						continue;
					}
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						if (PinsCaptured >= FHyperAIStudioContextSearchContracts::MaxBlueprintPins)
						{
							Snapshot.bSourceTruncated = true;
							break;
						}
						const FString PinGuid = GraphGuidToken(Pin->PinId);
						FHyperAIContextSearchValueRecord& PinRecord = Snapshot.Records.AddDefaulted_GetRef();
						PinRecord.Kind = TEXT("blueprint_pin");
						PinRecord.RecordId = HashSingle(GraphPath + TEXT("|node|") + NodeGuid + TEXT("|pin|") + PinGuid + TEXT("|") + Pin->PinName.ToString());
						AddField(PinRecord, TEXT("blueprint_path"), TEXT("object_path"), Blueprint->GetPathName());
						AddField(PinRecord, TEXT("graph_name"), TEXT("name"), Graph->GetName());
						AddField(PinRecord, TEXT("graph_guid"), TEXT("guid"), GraphGuidToken(Graph->GraphGuid));
						AddField(PinRecord, TEXT("node_guid"), TEXT("guid"), NodeGuid);
						AddField(PinRecord, TEXT("pin_guid"), TEXT("guid"), PinGuid);
						AddField(PinRecord, TEXT("pin_name"), TEXT("name"), Pin->PinName.ToString());
						AddField(PinRecord, TEXT("pin_direction"), TEXT("string"), PinDirectionToken(Pin->Direction));
						AddField(PinRecord, TEXT("pin_category"), TEXT("name"), Pin->PinType.PinCategory.ToString());
						AddField(PinRecord, TEXT("linked_pin_count"), TEXT("integer"), LexToString(Pin->LinkedTo.Num()));
						if (Pin->DefaultValue.Len() <= FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters
							&& !ContainsEmbeddedNull(Pin->DefaultValue))
						{
							AddField(PinRecord, TEXT("default_value"), TEXT("string"), Pin->DefaultValue);
						}
						else
						{
							Snapshot.bIncomplete = true;
							AddDiagnosticOnce(
								Snapshot.Diagnostics,
								TEXT("blueprint_pin_default_preview_unbounded"),
								TEXT("warning"),
								TEXT("default_value"),
								TEXT("An oversized or invalid selected-pin default value was omitted before copying."));
						}
						PinRecord.SearchTextLower = (Graph->GetName() + TEXT(" ") + Pin->PinName.ToString()
							+ TEXT(" ") + Pin->PinType.PinCategory.ToString()).ToLower();
						++PinsCaptured;
					}
				}
			}
			if (FPlatformTime::Seconds() - StartSeconds > MaxEditorCaptureSeconds)
			{
				Snapshot.bSourceTruncated = true;
				break;
			}
		}
		if (Snapshot.bSourceTruncated)
		{
			AddDiagnosticOnce(
				Snapshot.Diagnostics,
				TEXT("blueprint_selection_capture_bound_reached"),
				TEXT("warning"),
				TEXT("selected_blueprint_nodes"),
				TEXT("Loaded Blueprint graph discovery reached a node, pin, graph, blueprint, or elapsed-time bound."));
		}
	}

	FHyperAIContextSearchValueSnapshot CaptureContextSnapshot(
		const TArray<FString>& Projection,
		const int32 MaxActors,
		const int32 MaxBlueprintNodes,
		const bool bIncludePins,
		const bool bCaptureDirtyPackages = true)
	{
		FHyperAIContextSearchValueSnapshot Snapshot;
		Snapshot.SnapshotUtc = NowUtc();
		Snapshot.ObservationScope = TEXT("loaded_editor_context");
		Snapshot.bOnDiskOnly = false;
		if (!IsInGameThread())
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("game_thread_required"),
				TEXT("error"),
				TEXT("thread"),
				TEXT("UObject/editor capture is permitted only on the Unreal game thread."));
			return Snapshot;
		}
		Snapshot.Generation = FMath::Max<int64>(1, static_cast<int64>(GFrameCounter));
		static const TSet<FString> ProjectFields = {
			TEXT("project_name"), TEXT("project_file"), TEXT("project_root")
		};
		static const TSet<FString> WorldAndDirtyFields = {
			TEXT("map_name"), TEXT("world_path"), TEXT("package_name"), TEXT("dirty")
		};
		static const TSet<FString> ViewportFields = {
			TEXT("location"), TEXT("rotation"), TEXT("scale")
		};
		static const TSet<FString> ActorFields = {
			TEXT("name"), TEXT("label"), TEXT("class_path"), TEXT("path")
		};
		static const TSet<FString> BlueprintFields = {
			TEXT("blueprint_path"), TEXT("graph_name"), TEXT("graph_path"), TEXT("graph_guid"),
			TEXT("node_guid"), TEXT("node_comment"), TEXT("node_position"), TEXT("pin_guid"),
			TEXT("pin_name"), TEXT("pin_direction"), TEXT("pin_category"),
			TEXT("linked_pin_count"), TEXT("default_value")
		};
		const auto WantsAny = [&Projection](const TSet<FString>& Fields)
		{
			return Projection.ContainsByPredicate([&Fields](const FString& Field)
			{
				return Fields.Contains(Field);
			});
		};
		const bool bIncludeProject = WantsAny(ProjectFields);
		const bool bIncludeWorldAndDirty = WantsAny(WorldAndDirtyFields);
		const bool bIncludeViewport = WantsAny(ViewportFields);
		const bool bIncludeActors = MaxActors > 0 && WantsAny(ActorFields);
		const bool bIncludeBlueprint = MaxBlueprintNodes > 0 && WantsAny(BlueprintFields);
		if (bIncludeProject)
		{
			Snapshot.Records.Add(MakeProjectRecord());
		}
		AddWorldAndViewportContext(Snapshot, bIncludeWorldAndDirty, bIncludeViewport);
		if (bIncludeActors)
		{
			AddSelectedActors(Snapshot, MaxActors);
		}
		if (bIncludeBlueprint)
		{
			AddBlueprintGraphSelection(Snapshot, MaxBlueprintNodes, bIncludePins);
		}
		if (bIncludeWorldAndDirty && bCaptureDirtyPackages)
		{
			AppendDirtyPackages(Snapshot, CaptureDirtyProjectPackages());
		}
		SortAndDeduplicateRecords(Snapshot.Records);
		return Snapshot;
	}

	void AddSceneActor(
		const AActor* Actor,
		const bool bSelected,
		const int32 MaxComponents,
		const TArray<FString>& PropertyNames,
		FHyperAIContextSearchValueSnapshot& Snapshot)
	{
		if (!Actor)
		{
			return;
		}
		FHyperAIContextSearchValueRecord& ActorRecord = Snapshot.Records.AddDefaulted_GetRef();
		ActorRecord.Kind = TEXT("actor");
		ActorRecord.RecordId = Clip(Actor->GetPathName(), MaxPathCharacters);
		AddField(ActorRecord, TEXT("name"), TEXT("name"), Actor->GetName());
		FString BoundedActorLabel;
		if (FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(
			Actor->GetActorLabel(false), BoundedActorLabel))
		{
			AddField(ActorRecord, TEXT("label"), TEXT("string"), BoundedActorLabel);
		}
		else
		{
			Snapshot.bIncomplete = true;
			AddDiagnosticOnce(
				Snapshot.Diagnostics,
				TEXT("actor_label_preview_unbounded"),
				TEXT("warning"),
				TEXT("label"),
				TEXT("An oversized or malformed actor label was omitted before copying."));
		}
		AddField(ActorRecord, TEXT("class_path"), TEXT("class_path"), Actor->GetClass()->GetPathName());
		AddField(ActorRecord, TEXT("path"), TEXT("object_path"), Actor->GetPathName());
		AddField(ActorRecord, TEXT("selected"), TEXT("bool"), BoolToken(bSelected));
		AddField(ActorRecord, TEXT("location"), TEXT("vector3"), VectorToken(Actor->GetActorLocation()));
		AddField(ActorRecord, TEXT("rotation"), TEXT("rotator"), RotatorToken(Actor->GetActorRotation()));
		AddField(ActorRecord, TEXT("scale"), TEXT("vector3"), VectorToken(Actor->GetActorScale3D()));
		AddField(ActorRecord, TEXT("component_count"), TEXT("integer"), LexToString(Actor->GetComponents().Num()));
		ActorRecord.SearchTextLower = (Actor->GetName() + TEXT(" ") + BoundedActorLabel
			+ TEXT(" ") + Actor->GetClass()->GetPathName() + TEXT(" ") + Actor->GetPathName()).ToLower();
		AddScalarPropertyRecords(Actor, Actor->GetPathName(), PropertyNames, Snapshot);

		int32 ComponentCount = 0;
		for (const UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component)
			{
				continue;
			}
			if (ComponentCount >= MaxComponents)
			{
				Snapshot.bSourceTruncated = true;
				break;
			}
			if (Snapshot.Records.Num() >= MaxSceneSnapshotRecords)
			{
				Snapshot.bIncomplete = true;
				Snapshot.bSourceTruncated = true;
				AddDiagnosticOnce(
					Snapshot.Diagnostics,
					TEXT("scene_snapshot_record_bound_reached"),
					TEXT("warning"),
					TEXT("records"),
					TEXT("Scene identity, component, and property records reached the fixed aggregate snapshot bound."));
				break;
			}
			FHyperAIContextSearchValueRecord& ComponentRecord = Snapshot.Records.AddDefaulted_GetRef();
			ComponentRecord.Kind = TEXT("component");
			ComponentRecord.RecordId = Clip(Component->GetPathName(), MaxPathCharacters);
			AddField(ComponentRecord, TEXT("name"), TEXT("name"), Component->GetName());
			AddField(ComponentRecord, TEXT("class_path"), TEXT("class_path"), Component->GetClass()->GetPathName());
			AddField(ComponentRecord, TEXT("path"), TEXT("object_path"), Component->GetPathName());
			AddField(ComponentRecord, TEXT("owner_path"), TEXT("object_path"), Actor->GetPathName());
			AddField(ComponentRecord, TEXT("registered"), TEXT("bool"), BoolToken(Component->IsRegistered()));
			AddField(ComponentRecord, TEXT("active"), TEXT("bool"), BoolToken(Component->IsActive()));
			ComponentRecord.SearchTextLower = (Component->GetName() + TEXT(" ")
				+ Component->GetClass()->GetPathName() + TEXT(" ") + Actor->GetPathName()).ToLower();
			AddScalarPropertyRecords(Component, Component->GetPathName(), PropertyNames, Snapshot);
			++ComponentCount;
		}
		if (Actor->GetComponents().Num() > ComponentCount)
		{
			Snapshot.bSourceTruncated = true;
			AddDiagnosticOnce(
				Snapshot.Diagnostics,
				TEXT("component_bound_reached"),
				TEXT("warning"),
				TEXT("max_components_per_actor"),
				TEXT("At least one actor owns more components than the fixed requested component bound."));
		}
	}

	FHyperAIContextSearchValueSnapshot CaptureSceneSnapshot(
		const FString& Scope,
		const FString& ActorPath,
		const int32 MaxActors,
		const int32 MaxComponents,
		const TArray<FString>& PropertyNames,
		const bool bCaptureDirtyPackages = true)
	{
		FHyperAIContextSearchValueSnapshot Snapshot;
		Snapshot.SnapshotUtc = NowUtc();
		Snapshot.ObservationScope = TEXT("loaded_editor_scene");
		Snapshot.bOnDiskOnly = false;
		if (!IsInGameThread() || !GEditor)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("editor_game_thread_required"),
				TEXT("error"),
				TEXT("thread"),
				TEXT("Loaded scene capture requires the Unreal editor game thread."));
			return Snapshot;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("editor_world_unavailable"),
				TEXT("error"),
				TEXT("world"),
				TEXT("No loaded editor world is available."));
			return Snapshot;
		}

		USelection* SelectedActors = GEditor->GetSelectedActors();
		int32 ActorsCaptured = 0;
		int32 ActorsScanned = 0;
		const double StartSeconds = FPlatformTime::Seconds();
		auto AddActorIfEligible = [&](AActor* Actor)
		{
			if (!Actor || ActorsCaptured >= MaxActors)
			{
				return;
			}
			const bool bSelected = SelectedActors && SelectedActors->IsSelected(Actor);
			AddSceneActor(Actor, bSelected, MaxComponents, PropertyNames, Snapshot);
			++ActorsCaptured;
		};

		if (Scope == TEXT("selected"))
		{
			if (!SelectedActors)
			{
				Snapshot.bIncomplete = true;
				AddDiagnostic(Snapshot.Diagnostics, TEXT("actor_selection_unavailable"), TEXT("error"), TEXT("scope"), TEXT("Selected actor state is unavailable."));
			}
			else
			{
				for (FSelectionIterator Iterator(*SelectedActors);
					Iterator && ActorsScanned < FHyperAIStudioContextSearchContracts::MaxWorldActorsScanned;
					++Iterator, ++ActorsScanned)
				{
					if (ActorsCaptured >= MaxActors)
					{
						break;
					}
					AddActorIfEligible(Cast<AActor>(*Iterator));
				}
				Snapshot.bSourceTruncated |= SelectedActors->Num() > ActorsCaptured;
			}
		}
		else if (Scope == TEXT("actor"))
		{
			AActor* ExactActor = FindObject<AActor>(nullptr, *ActorPath);
			if (IsValid(ExactActor) && ExactActor->GetWorld() == World
				&& ExactActor->GetPathName() == ActorPath)
			{
				AddActorIfEligible(ExactActor);
			}
			else
			{
				Snapshot.bIncomplete = true;
				AddDiagnostic(
					Snapshot.Diagnostics,
					TEXT("actor_not_found"),
					TEXT("error"),
					TEXT("actor_path"),
					TEXT("No already-loaded actor in the current editor world has the exact requested path; no scan or load was attempted."));
			}
		}
		else
		{
			for (TActorIterator<AActor> Iterator(World);
				Iterator && ActorsScanned < FHyperAIStudioContextSearchContracts::MaxWorldActorsScanned;
				++Iterator, ++ActorsScanned)
			{
				if (FPlatformTime::Seconds() - StartSeconds > MaxEditorCaptureSeconds)
				{
					Snapshot.bSourceTruncated = true;
					break;
				}
				AActor* Actor = *Iterator;
				if (ActorsCaptured >= MaxActors)
				{
					Snapshot.bSourceTruncated = true;
					break;
				}
				AddActorIfEligible(Actor);
			}
		}

		if (Snapshot.bSourceTruncated)
		{
			AddDiagnosticOnce(
				Snapshot.Diagnostics,
				TEXT("scene_capture_bound_reached"),
				TEXT("warning"),
				TEXT("max_actors"),
				TEXT("Scene capture reached an actor, component, or elapsed-time bound."));
		}
		if (Scope == TEXT("actor"))
		{
			if (AActor* ExactActor = FindObject<AActor>(nullptr, *ActorPath);
				IsValid(ExactActor) && ExactActor->GetWorld() == World
				&& ExactActor->GetPathName() == ActorPath)
			{
				if (UPackage* Package = ExactActor->GetOutermost(); Package && Package->IsDirty())
				{
					Snapshot.bDirty = true;
					Snapshot.DirtyPackages.Add(Package->GetName());
				}
			}
		}
		else if (bCaptureDirtyPackages)
		{
			AppendDirtyPackages(Snapshot, CaptureDirtyProjectPackages());
		}
		SortAndDeduplicateRecords(Snapshot.Records);
		return Snapshot;
	}

	FHyperAIContextSearchValueSnapshot CaptureExactSceneActorSnapshot(
		const TArray<FString>& ActorPaths,
		const int32 MaxComponents)
	{
		FHyperAIContextSearchValueSnapshot Snapshot;
		Snapshot.SnapshotUtc = NowUtc();
		Snapshot.ObservationScope = TEXT("loaded_editor_scene");
		if (!IsInGameThread() || !GEditor)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(Snapshot.Diagnostics, TEXT("editor_game_thread_required"), TEXT("error"),
				TEXT("thread"), TEXT("Exact scene-plan capture requires the editor game thread."));
			return Snapshot;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Snapshot.bIncomplete = true;
			AddDiagnostic(Snapshot.Diagnostics, TEXT("editor_world_unavailable"), TEXT("error"),
				TEXT("world"), TEXT("No loaded editor world is available."));
			return Snapshot;
		}
		const double Started = FPlatformTime::Seconds();
		for (const FString& ActorPath : ActorPaths)
		{
			if (FPlatformTime::Seconds() - Started > MaxEditorCaptureSeconds)
			{
				Snapshot.bIncomplete = true;
				Snapshot.bSourceTruncated = true;
				AddDiagnosticOnce(Snapshot.Diagnostics, TEXT("scene_plan_capture_deadline"), TEXT("error"),
					TEXT("actors"), TEXT("Exact scene-plan capture reached its monotonic game-thread deadline."));
				break;
			}
			AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
			if (!IsValid(Actor) || Actor->GetWorld() != World || Actor->GetPathName() != ActorPath)
			{
				Snapshot.bIncomplete = true;
				AddDiagnosticOnce(Snapshot.Diagnostics, TEXT("scene_plan_actor_not_loaded"), TEXT("error"),
					TEXT("actor_paths"), TEXT("Every scene-plan actor must already be loaded in the current editor world at its exact path."));
				continue;
			}
			USelection* Selection = GEditor->GetSelectedActors();
			AddSceneActor(Actor, Selection && Selection->IsSelected(Actor),
				MaxComponents, {}, Snapshot);
			if (UPackage* Package = Actor->GetOutermost(); Package && Package->IsDirty())
			{
				Snapshot.bDirty = true;
				if (Snapshot.DirtyPackages.Num() < MaxDirtyPackages)
				{
					Snapshot.DirtyPackages.AddUnique(Package->GetName());
				}
				else
				{
					Snapshot.bDirtyPackageListTruncated = true;
				}
			}
		}
		Snapshot.DirtyPackages.Sort();
		SortAndDeduplicateRecords(Snapshot.Records);
		return Snapshot;
	}

	FHyperAIContextSearchValueSnapshot SliceSceneActorSnapshot(
		const FHyperAIContextSearchValueSnapshot& Source,
		const FString& ActorPath)
	{
		FHyperAIContextSearchValueSnapshot Result;
		Result.SnapshotUtc = Source.SnapshotUtc;
		Result.ObservationScope = Source.ObservationScope;
		Result.bOnDiskOnly = Source.bOnDiskOnly;
		Result.bDirty = Source.bDirty;
		Result.bIncomplete = Source.bIncomplete;
		Result.bSourceTruncated = Source.bSourceTruncated;
		Result.bDirtyPackageListTruncated = Source.bDirtyPackageListTruncated;
		Result.Generation = Source.Generation;
		Result.DirtyPackages = Source.DirtyPackages;
		Result.Diagnostics = Source.Diagnostics;
		TSet<FString> OwnedPaths = {ActorPath};
		for (const FHyperAIContextSearchValueRecord& Record : Source.Records)
		{
			if (Record.Kind == TEXT("component")
				&& FindFieldValue(Record, TEXT("owner_path")) == ActorPath)
			{
				OwnedPaths.Add(FindFieldValue(Record, TEXT("path")));
			}
		}
		for (const FHyperAIContextSearchValueRecord& Record : Source.Records)
		{
			const bool bInclude = (Record.Kind == TEXT("actor")
					&& FindFieldValue(Record, TEXT("path")) == ActorPath)
				|| (Record.Kind == TEXT("component")
					&& FindFieldValue(Record, TEXT("owner_path")) == ActorPath)
				|| (Record.Kind == TEXT("property")
					&& OwnedPaths.Contains(FindFieldValue(Record, TEXT("property_owner_path"))));
			if (bInclude) Result.Records.Add(Record);
		}
		SortAndDeduplicateRecords(Result.Records);
		return Result;
	}

	void AddSceneValidationIssue(
		TArray<FHyperAIReadDiagnostic>& Issues,
		const int32 MaxIssues,
		bool& bTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const TCHAR* Field,
		const TCHAR* Message)
	{
		if (Issues.Num() >= MaxIssues)
		{
			bTruncated = true;
			return;
		}
		FHyperAIReadDiagnostic& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.Field = Clip(Field, FHyperAIStudioContextSearchContracts::MaxFieldNameCharacters);
		Issue.Message = Clip(Message, FHyperAIStudioContextSearchContracts::MaxDiagnosticCharacters);
	}

	TArray<FHyperAIReadDiagnostic> ValidateSceneValueSnapshot(
		const FHyperAIContextSearchValueSnapshot& Snapshot,
		const int32 MaxIssues,
		bool& bOutTruncated)
	{
		TArray<FHyperAIReadDiagnostic> Issues;
		bOutTruncated = false;
		for (const FHyperAIReadDiagnostic& Diagnostic : Snapshot.Diagnostics)
		{
			if (Issues.Num() >= MaxIssues)
			{
				bOutTruncated = true;
				break;
			}
			Issues.Add(Diagnostic);
		}
		const FString SnapshotFingerprint =
			FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Snapshot);
		if (!IsCanonicalSha256(SnapshotFingerprint) || Snapshot.bIncomplete || Snapshot.bSourceTruncated)
		{
			AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
				TEXT("scene_revision_incomplete"), TEXT("error"), TEXT("snapshot"),
				TEXT("Independent validation requires a complete bounded scene value snapshot and canonical revision."));
		}
		TSet<FString> RecordIds;
		TSet<FString> ActorPaths;
		TSet<FString> OwnedPaths;
		for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Records)
		{
			if (Record.RecordId.IsEmpty() || RecordIds.Contains(Record.RecordId))
			{
				AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
					TEXT("scene_record_identity_invalid"), TEXT("error"), TEXT("record_id"),
					TEXT("Scene record identities must be nonempty and unique."));
			}
			RecordIds.Add(Record.RecordId);
			if (Record.Kind == TEXT("actor"))
			{
				const FString Path = FindFieldValue(Record, TEXT("path"));
				if (Path.IsEmpty() || FindFieldValue(Record, TEXT("class_path")).IsEmpty()
					|| FindFieldValue(Record, TEXT("location")).IsEmpty()
					|| FindFieldValue(Record, TEXT("rotation")).IsEmpty()
					|| FindFieldValue(Record, TEXT("scale")).IsEmpty())
				{
					AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
						TEXT("actor_projection_incomplete"), TEXT("error"), TEXT("actor"),
						TEXT("Every actor requires exact path, class, location, rotation, and scale fields."));
				}
				ActorPaths.Add(Path);
				OwnedPaths.Add(Path);
			}
		}
		for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Records)
		{
			if (Record.Kind == TEXT("component"))
			{
				const FString Owner = FindFieldValue(Record, TEXT("owner_path"));
				const FString Path = FindFieldValue(Record, TEXT("path"));
				if (!ActorPaths.Contains(Owner) || Path.IsEmpty())
				{
					AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
						TEXT("component_owner_invalid"), TEXT("error"), TEXT("owner_path"),
						TEXT("Every component must reference one captured actor and expose an exact path."));
				}
				OwnedPaths.Add(Path);
			}
		}
		for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Records)
		{
			if (Record.Kind == TEXT("property")
				&& (!OwnedPaths.Contains(FindFieldValue(Record, TEXT("property_owner_path")))
					|| FindFieldValue(Record, TEXT("property_name")).IsEmpty()
					|| FindFieldValue(Record, TEXT("property_type")).IsEmpty()))
			{
				AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
					TEXT("property_owner_invalid"), TEXT("error"), TEXT("property_owner_path"),
					TEXT("Every scalar property must reference a captured actor/component and retain its exact name/type."));
			}
		}
		if (Snapshot.bDirty)
		{
			AddSceneValidationIssue(Issues, MaxIssues, bOutTruncated,
				TEXT("scene_contains_unsaved_state"), TEXT("warning"), TEXT("dirty_packages"),
				TEXT("The loaded scene snapshot includes unsaved project packages; mutation planning remains blocked."));
		}
		return Issues;
	}

	class FHyperAISceneTypedArtifactPayload final : public IHyperAIStudioTypedArtifactPayload
	{
	public:
		FString BaseRevision;
		TArray<FString> ActorPaths;
		TArray<FString> ExpectedActorRevisions;
		TArray<FVector> TargetLocations;

		virtual FString GetTypeId() const override
		{
			return TEXT("hyperai.payload.scene.layout-plan.v1");
		}

		virtual FString GetSchemaFingerprint() const override
		{
			return FHyperAIStudioContextSearchContracts::HashTokens({
				TEXT("hyperai.schema.scene.layout-plan.v1"),
				TEXT("layout_grid"), TEXT("exact_actor_snapshot_cas"), TEXT("zero_effect")});
		}

		virtual int32 GetBoundedByteSize() const override
		{
			int64 Characters = BaseRevision.Len() + 128;
			for (int32 Index = 0; Index < ActorPaths.Num(); ++Index)
			{
				Characters += ActorPaths[Index].Len()
					+ (ExpectedActorRevisions.IsValidIndex(Index)
						? ExpectedActorRevisions[Index].Len() : 0) + 128;
			}
			return Characters > MAX_int32 / static_cast<int32>(sizeof(TCHAR))
				? MAX_int32 : static_cast<int32>(Characters * sizeof(TCHAR));
		}

		virtual FString GetSemanticFingerprint() const override
		{
			TArray<FString> Tokens = {TEXT("hyperai.scene.layout-payload.v1"), BaseRevision};
			if (ActorPaths.Num() != ExpectedActorRevisions.Num()
				|| ActorPaths.Num() != TargetLocations.Num()) return FString();
			for (int32 Index = 0; Index < ActorPaths.Num(); ++Index)
			{
				Tokens.Add(ActorPaths[Index]);
				Tokens.Add(ExpectedActorRevisions[Index]);
				Tokens.Add(VectorToken(TargetLocations[Index]));
			}
			return FHyperAIStudioContextSearchContracts::HashTokens(Tokens);
		}

		virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
			CloneImmutable() const override
		{
			TSharedRef<FHyperAISceneTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
				MakeShared<FHyperAISceneTypedArtifactPayload, ESPMode::ThreadSafe>();
			Clone->BaseRevision = BaseRevision;
			Clone->ActorPaths = ActorPaths;
			Clone->ExpectedActorRevisions = ExpectedActorRevisions;
			Clone->TargetLocations = TargetLocations;
			return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
		}
	};

	bool PrepareSceneLayoutArtifact(
		const TSharedRef<const FHyperAISceneTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		const int32 MaxGameThreadMs,
		const int32 MaxOutputBytes,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		constexpr const TCHAR* PackId = TEXT("context_scene_project_search");
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding.PackId = PackId;
		Contract.Binding.ToolName = TEXT("hyper_scene_apply_plan");
		Contract.Binding.VariantId = TEXT("scene.layout_grid.preparation-only.v1");
		Contract.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Contract.Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		Contract.Binding.ExpectedAdapterFingerprint =
			FHyperAIStudioContextSearchContracts::HashTokens({
				TEXT("adapter.scene.layout-preparation-only.ue58")});
		Contract.Binding.ExpectedAdapterGeneration = 1;
		Contract.Binding.ExpectedRegistryEpoch = 1;
		Contract.Binding.Prerequisites.PackId = PackId;
		Contract.Binding.Prerequisites.bPackEnabled = true;
		Contract.Binding.Prerequisites.Revision = 1;
		const TSharedPtr<IPlugin> EditorToolset = IPluginManager::Get().FindPlugin(TEXT("EditorToolset"));
		Contract.Binding.Prerequisites.Observations = {
			{TEXT("plugin.EditorToolset"), EditorToolset.IsValid() && EditorToolset->IsEnabled()
				? EHyperAIStudioDomainPrerequisiteState::Available
				: EHyperAIStudioDomainPrerequisiteState::Disabled},
			{TEXT("module.AssetRegistry"), FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry"))
				? EHyperAIStudioDomainPrerequisiteState::Available
				: EHyperAIStudioDomainPrerequisiteState::Missing},
			{TEXT("probe.live_editor_toolset"), UToolsetRegistry::IsAvailable()
				? EHyperAIStudioDomainPrerequisiteState::Available
				: EHyperAIStudioDomainPrerequisiteState::Missing},
			{TEXT("probe.project_index"), FHyperAIStudioProjectIndexStore::Get().GetStatus().Status == TEXT("ready")
				? EHyperAIStudioDomainPrerequisiteState::Available
				: EHyperAIStudioDomainPrerequisiteState::Missing}};
		Contract.Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Contract.Binding.Prerequisites);
		Contract.Binding.Admission.PackId = PackId;
		Contract.Binding.Admission.bPackAdmitted = false;
		Contract.Binding.Admission.bEditAdmitted = false;
		Contract.Binding.Admission.Revision = 1;
		Contract.Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
				Contract.Binding.Admission);
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = TEXT("scene-layout:") + Payload->BaseRevision;
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = FMath::Clamp(Payload->ActorPaths.Num() + 4, 5, 128);
		Contract.MaxGameThreadMs = MaxGameThreadMs;
		Contract.MaxOutputBytes = MaxOutputBytes;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = false;
		Contract.bSaveOnce = false;
		// These flags describe the backend that would be required to execute this sealed
		// plan; this source-candidate wrapper itself never stages or runs either phase.
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}

	bool ValidateCommonPageBounds(
		const int32 PageSize,
		const FString& Cursor,
		const int32 MaxOutputBytes,
		FString& OutErrorCode,
		FString& OutError)
	{
		if (PageSize < 1 || PageSize > FHyperAIStudioContextSearchContracts::MaxPageSize)
		{
			OutErrorCode = TEXT("invalid_page_size");
			OutError = TEXT("PageSize is outside the fixed supported range.");
			return false;
		}
		if (Cursor.Len() > FHyperAIStudioContextSearchContracts::MaxCursorCharacters
			|| ContainsEmbeddedNull(Cursor) || !HasWellFormedUtf16(Cursor))
		{
			OutErrorCode = TEXT("invalid_cursor_size");
			OutError = TEXT("Cursor exceeds the fixed input bound or contains an embedded null.");
			return false;
		}
		if (MaxOutputBytes < MinOutputBytes || MaxOutputBytes > FHyperAIStudioContextSearchContracts::MaxOutputBytes)
		{
			OutErrorCode = TEXT("invalid_output_budget");
			OutError = TEXT("MaxOutputBytes is outside the fixed supported range.");
			return false;
		}
		return true;
	}
}

FHyperAIReadReport FHyperAIStudioContextSearchContracts::SearchProjectIndex(
	const FHyperAIProjectIndexValueSnapshot& Snapshot,
	const FHyperAIProjectSearchRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	FString ErrorCode;
	FString Error;
	if (!ValidateCommonPageBounds(Request.PageSize, Request.Cursor, Request.MaxOutputBytes, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("request"), Error);
	}
	if (Request.Query.Len() > MaxQueryCharacters || ContainsEmbeddedNull(Request.Query)
		|| !HasWellFormedUtf16(Request.Query))
	{
		return InvalidReport(
			TEXT("invalid_query"),
			TEXT("query"),
			TEXT("Query must contain 1-128 well-formed characters and no embedded null."));
	}
	FString Query = Request.Query;
	Query.TrimStartAndEndInline();
	if (Query.IsEmpty())
	{
		return InvalidReport(
			TEXT("invalid_query"),
			TEXT("query"),
			TEXT("Query must contain 1-128 characters and no embedded null."));
	}

	TArray<FString> Kinds;
	const TArray<FString> DefaultKinds = { TEXT("asset"), TEXT("cpp_symbol") };
	const TArray<FString>& RequestedKinds = Request.Kinds.IsEmpty() ? DefaultKinds : Request.Kinds;
	if (!NormalizeStringSet(RequestedKinds, ProjectKinds(), 2, false, Kinds, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("kinds"), Error);
	}
	TArray<FString> Projection;
	if (!NormalizeProjection(Request.Fields, ProjectProjectionAllowlist(), ProjectProjectionDefaults(), Projection, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("fields"), Error);
	}

	if (Snapshot.Value.Records.IsEmpty() && (Snapshot.Status == TEXT("uninitialized") || Snapshot.Status == TEXT("indexing")))
	{
		FHyperAIReadReport Report;
		Report.Status = Snapshot.Status;
		Report.Diagnostic = Snapshot.Status == TEXT("indexing")
			? TEXT("The bounded project index is being built; poll hyper_project_index_status and retry.")
			: TEXT("No immutable project index is available; request refresh through hyper_project_index_status.");
		Report.ObservationScope = TEXT("project_on_disk_index");
		Report.bOnDiskOnly = true;
		Report.bIncomplete = true;
		Report.SnapshotGeneration = Snapshot.Value.Generation;
		return Report;
	}

	TArray<FString> QueryTerms;
	Query.ToLower().ParseIntoArrayWS(QueryTerms);
	TSet<FString> KindSet(Kinds);
	FHyperAIContextSearchValueSnapshot Filtered;
	Filtered.SnapshotUtc = Snapshot.Value.SnapshotUtc;
	Filtered.ObservationScope = Snapshot.Value.ObservationScope;
	Filtered.bOnDiskOnly = Snapshot.Value.bOnDiskOnly;
	Filtered.bDirty = Snapshot.Value.bDirty;
	Filtered.bIncomplete = Snapshot.Value.bIncomplete;
	Filtered.bSourceTruncated = Snapshot.Value.bSourceTruncated;
	Filtered.bDirtyPackageListTruncated = Snapshot.Value.bDirtyPackageListTruncated;
	Filtered.Generation = Snapshot.Value.Generation;
	Filtered.DirtyPackages = Snapshot.Value.DirtyPackages;
	Filtered.Diagnostics = Snapshot.Value.Diagnostics;
	Filtered.Records.Reserve(FMath::Min(Snapshot.Value.Records.Num(), MaxProjectIndexRecords));
	Filtered.ObservationScope = TEXT("project_on_disk_index");
	Filtered.bOnDiskOnly = true;
	Filtered.bIncomplete |= Snapshot.Status != TEXT("ready");
	const double StartSeconds = FPlatformTime::Seconds();
	int32 Scanned = 0;
	for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Value.Records)
	{
		if (Scanned >= MaxProjectIndexRecords
			|| FPlatformTime::Seconds() - StartSeconds > MaxProjectSearchSeconds)
		{
			Filtered.bIncomplete = true;
			Filtered.bSourceTruncated = true;
			AddDiagnosticOnce(
				Filtered.Diagnostics,
				TEXT("project_search_work_bound_reached"),
				TEXT("warning"),
				TEXT("query"),
				TEXT("Value-only search reached its fixed record or elapsed-time work bound."));
			break;
		}
		++Scanned;
		if (!KindSet.Contains(Record.Kind))
		{
			continue;
		}
		bool bMatches = true;
		for (const FString& Term : QueryTerms)
		{
			if (!Record.SearchTextLower.Contains(Term, ESearchCase::CaseSensitive))
			{
				bMatches = false;
				break;
			}
		}
		if (bMatches)
		{
			Filtered.Records.Add(Record);
		}
	}
	SortAndDeduplicateRecords(Filtered.Records);
	TArray<FString> FingerprintTokens = {
		TEXT("project_search.v1"), Query.ToLower(), FString::Join(Kinds, TEXT(",")),
		FString::Join(Projection, TEXT(",")), LexToString(Request.PageSize), LexToString(Request.MaxOutputBytes)
	};
	const FString RequestFingerprint = HashTokens(FingerprintTokens);
	FHyperAIReadReport Report;
	ProjectPage(Filtered, Projection, RequestFingerprint, Request.PageSize, Request.Cursor, Request.MaxOutputBytes, Report);
	return Report;
}

FHyperAIStudioProjectIndexStore& FHyperAIStudioProjectIndexStore::Get()
{
	static FHyperAIStudioProjectIndexStore Store;
	return Store;
}

FHyperAIProjectIndexStatus FHyperAIStudioProjectIndexStore::GetStatus() const
{
	FScopeLock Lock(&Mutex);
	FHyperAIProjectIndexStatus Status;
	Status.Status = Snapshot.Status;
	Status.bRefreshInProgress = bWorkerRunning;
	Status.StartedUtc = Snapshot.StartedUtc;
	Status.CompletedUtc = Snapshot.CompletedUtc;
	Status.SnapshotFingerprint = Snapshot.SnapshotFingerprint;
	Status.Generation = Snapshot.Value.Generation;
	Status.AssetRecordCount = Snapshot.AssetRecordCount;
	Status.CppFileCount = Snapshot.CppFileCount;
	Status.CppSymbolCount = Snapshot.CppSymbolCount;
	Status.TotalRecordCount = Snapshot.Value.Records.Num();
	Status.CppBytesRead = Snapshot.CppBytesRead;
	Status.bIncomplete = Snapshot.Value.bIncomplete
		|| Snapshot.Status == TEXT("uninitialized")
		|| Snapshot.Status == TEXT("indexing")
		|| Snapshot.Status == TEXT("partial")
		|| Snapshot.Status == TEXT("failed");
	Status.bDirty = Snapshot.Value.bDirty;
	Status.DirtyPackages = Snapshot.Value.DirtyPackages;
	Status.DirtyPackageCount = Snapshot.Value.DirtyPackages.Num();
	Status.bDirtyPackageListTruncated = Snapshot.Value.bDirtyPackageListTruncated;
	Status.Diagnostics = Snapshot.Value.Diagnostics;
	Status.bOk = Snapshot.Status == TEXT("ready") || Snapshot.Status == TEXT("partial")
		|| (Snapshot.Status == TEXT("indexing") && !Snapshot.Value.Records.IsEmpty());
	if (bShuttingDown)
	{
		Status.Status = TEXT("shutting_down");
		Status.bOk = false;
		Status.bIncomplete = true;
		Status.Diagnostic = TEXT("The project index store is shutting down.");
	}
	else if (Snapshot.Status == TEXT("ready"))
	{
		Status.Diagnostic = TEXT("The immutable fixed-root on-disk project index is ready.");
	}
	else if (Snapshot.Status == TEXT("partial"))
	{
		Status.Diagnostic = TEXT("The immutable project index is usable but explicitly partial; inspect diagnostics and bounds.");
	}
	else if (Snapshot.Status == TEXT("indexing"))
	{
		Status.Diagnostic = Snapshot.Value.Records.IsEmpty()
			? TEXT("A bounded asynchronous project index refresh is running.")
			: TEXT("A refresh is running; the prior immutable snapshot remains available but stale/incomplete.");
	}
	else if (Snapshot.Status == TEXT("failed"))
	{
		Status.Diagnostic = TEXT("The last bounded project index refresh failed; inspect diagnostics.");
	}
	else
	{
		Status.Diagnostic = TEXT("No project index has been requested in this editor session.");
	}
	return Status;
}

FHyperAIProjectIndexValueSnapshot FHyperAIStudioProjectIndexStore::GetSnapshot() const
{
	return *GetSnapshotShared();
}

TSharedRef<const FHyperAIProjectIndexValueSnapshot, ESPMode::ThreadSafe>
FHyperAIStudioProjectIndexStore::GetSnapshotShared() const
{
	FScopeLock Lock(&Mutex);
	if (!SnapshotView.IsValid())
	{
		SnapshotView = MakeShared<FHyperAIProjectIndexValueSnapshot, ESPMode::ThreadSafe>(Snapshot);
	}
	return SnapshotView.ToSharedRef();
}

bool FHyperAIStudioProjectIndexStore::RequestRefresh(FString& OutDiagnostic)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutDiagnostic = TEXT("Project index refresh must capture loaded dirty-package evidence on the Unreal game thread.");
		return false;
	}

	int64 Generation = 0;
	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			OutDiagnostic = TEXT("Project index store is shutting down.");
			return false;
		}
		if (bWorkerRunning)
		{
			OutDiagnostic = TEXT("A bounded project index refresh is already in progress.");
			return false;
		}
		Generation = NextGeneration++;
		bWorkerRunning = true;
		SnapshotView.Reset();
		Snapshot.Status = TEXT("indexing");
		Snapshot.StartedUtc = NowUtc();
		Snapshot.CompletedUtc.Reset();
		Snapshot.Value.bIncomplete = true;
		Snapshot.Value.Generation = Generation;
	}

	FHyperAIProjectIndexValueSnapshot Captured;
	Captured.Status = TEXT("indexing");
	Captured.StartedUtc = NowUtc();
	Captured.Value.SnapshotUtc = Captured.StartedUtc;
	Captured.Value.ObservationScope = TEXT("project_on_disk_index");
	Captured.Value.bOnDiskOnly = true;
	Captured.Value.Generation = Generation;
	AppendDirtyPackages(Captured.Value, CaptureDirtyProjectPackages());
	if (Captured.Value.bDirty)
	{
		Captured.Value.bIncomplete = true;
		AddDiagnostic(
			Captured.Value.Diagnostics,
			TEXT("dirty_loaded_packages_excluded"),
			TEXT("warning"),
			TEXT("dirty_packages"),
			TEXT("The index is on-disk only; unsaved loaded package changes are outside this observation."));
	}

	// UE 5.8's public on-disk EnumerateAssets implementation first copies every
	// matching /Game asset under a blocking registry read lock, before invoking
	// the caller callback. A callback deadline/record cap therefore cannot bound
	// either the lock duration or the pre-callback allocation. Keep the C++ index
	// useful, but refuse to certify an asset index until a generation-cached,
	// incrementally populated backend can supply a bounded immutable snapshot.
	Captured.Value.bIncomplete = true;
	Captured.AssetRecordCount = 0;
	AddDiagnostic(
		Captured.Value.Diagnostics,
		TEXT("async_asset_index_backend_required"),
		TEXT("warning"),
		TEXT("asset_index"),
		TEXT("Project asset search requires a generation-cached asynchronous typed index; synchronous Asset Registry enumeration was intentionally not called."));
	SortAndDeduplicateRecords(Captured.Value.Records);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	OutDiagnostic = TEXT("Started a bounded asynchronous refresh of fixed project Source and project-plugin Source roots.");
	WorkerFuture = Async(EAsyncExecution::ThreadPool, [this, Captured = MoveTemp(Captured), ProjectRoot]() mutable
	{
		using namespace HyperAIStudio::ContextSearch::Private;
		const double StartSeconds = FPlatformTime::Seconds();
		TArray<FString> CandidatePaths;
		bool bEnumerationTruncated = false;
		FString EnumerationCode;
		const bool bEnumerationOk =
			FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
				ProjectRoot,
				FHyperAIStudioContextSearchContracts::MaxCppDirectories,
				FHyperAIStudioContextSearchContracts::MaxCppDirectoryEntries,
				CandidatePaths,
				bEnumerationTruncated,
				EnumerationCode);
		if (!bEnumerationOk || bEnumerationTruncated)
		{
			Captured.Value.bIncomplete = true;
			Captured.Value.bSourceTruncated |= bEnumerationTruncated;
			AddDiagnostic(
				Captured.Value.Diagnostics,
				EnumerationCode.IsEmpty() ? TEXT("cpp_source_enumeration_incomplete") : *EnumerationCode,
				bEnumerationOk ? TEXT("warning") : TEXT("error"),
				TEXT("cpp_index"),
				bEnumerationOk
					? TEXT("Secure fixed-root C++ source enumeration was bounded or rejected a reparse/containment entry.")
					: TEXT("Secure fixed-root C++ source enumeration is unavailable; no lexical fallback was used."));
		}
		bool bWorkBoundReached = false;
		TSet<FString> SeenPaths;
		for (const FString& Candidate : CandidatePaths)
		{
			if (FPlatformTime::Seconds() - StartSeconds > MaxCppIndexSeconds
				|| Captured.CppFileCount >= FHyperAIStudioContextSearchContracts::MaxCppFiles
				|| Captured.CppSymbolCount >= FHyperAIStudioContextSearchContracts::MaxCppSymbols
				|| Captured.CppBytesRead >= FHyperAIStudioContextSearchContracts::MaxCppTotalBytes)
			{
				bWorkBoundReached = true;
				break;
			}
			FHyperAIProjectSourceReadResult Read;
			FString ReadCode;
			const int64 RemainingBytes =
				FHyperAIStudioContextSearchContracts::MaxCppTotalBytes - Captured.CppBytesRead;
			if (!FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
				ProjectRoot,
				Candidate,
				Read,
				ReadCode,
				FMath::Min<int64>(FHyperAIStudioContextSearchContracts::MaxCppFileBytes, RemainingBytes)))
			{
				Captured.Value.bIncomplete = true;
				Captured.Value.bSourceTruncated = true;
				AddDiagnosticOnce(
					Captured.Value.Diagnostics,
					ReadCode.IsEmpty() ? TEXT("secure_cpp_source_read_rejected") : *ReadCode,
					TEXT("warning"),
					TEXT("cpp_index"),
					TEXT("A C++ source candidate was rejected by same-handle final-path, size, read, or encoding validation."));
				continue;
			}
			if (SeenPaths.Contains(Read.RelativePath))
			{
				continue;
			}
			SeenPaths.Add(Read.RelativePath);
			++Captured.CppFileCount;
			Captured.CppBytesRead += Read.BytesRead;
			bool bSymbolsTruncated = false;
			const int32 RemainingSymbols =
				FHyperAIStudioContextSearchContracts::MaxCppSymbols - Captured.CppSymbolCount;
			TArray<FHyperAIContextSearchValueRecord> Symbols =
				FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
					Read.RelativePath,
					Read.Text,
					RemainingSymbols,
					bSymbolsTruncated);
			Captured.CppSymbolCount += Symbols.Num();
			Captured.Value.Records.Append(MoveTemp(Symbols));
			if (bSymbolsTruncated)
			{
				Captured.Value.bIncomplete = true;
				Captured.Value.bSourceTruncated = true;
				AddDiagnosticOnce(
					Captured.Value.Diagnostics,
					TEXT("cpp_symbol_bound_reached"),
					TEXT("warning"),
					TEXT("cpp_index"),
					TEXT("Streaming C++ symbol extraction reached its fixed line, source, time, or total symbol bound."));
			}
		}

		if (bWorkBoundReached)
		{
			Captured.Value.bIncomplete = true;
			Captured.Value.bSourceTruncated = true;
			AddDiagnosticOnce(
				Captured.Value.Diagnostics,
				TEXT("cpp_index_work_bound_reached"),
				TEXT("warning"),
				TEXT("cpp_index"),
				TEXT("C++ indexing reached its fixed file, byte, symbol, or elapsed-time work bound."));
		}
		if (Captured.Value.Records.Num() > FHyperAIStudioContextSearchContracts::MaxProjectIndexRecords)
		{
			Captured.Value.Records.SetNum(FHyperAIStudioContextSearchContracts::MaxProjectIndexRecords);
			Captured.Value.bIncomplete = true;
			Captured.Value.bSourceTruncated = true;
			AddDiagnosticOnce(
				Captured.Value.Diagnostics,
				TEXT("project_index_record_bound_reached"),
				TEXT("warning"),
				TEXT("index"),
				TEXT("Combined asset and C++ symbol records reached the hard index record bound."));
		}

		SortAndDeduplicateRecords(Captured.Value.Records);
		Captured.CompletedUtc = NowUtc();
		Captured.Value.SnapshotUtc = Captured.CompletedUtc;
		Captured.SnapshotFingerprint =
			FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Captured.Value);
		Captured.Status = Captured.SnapshotFingerprint.IsEmpty()
			? TEXT("failed")
			: (Captured.Value.bIncomplete || Captured.Value.bSourceTruncated ? TEXT("partial") : TEXT("ready"));
		if (Captured.Status == TEXT("failed"))
		{
			Captured.Value.bIncomplete = true;
			AddDiagnostic(
				Captured.Value.Diagnostics,
				TEXT("project_index_hash_failed"),
				TEXT("error"),
				TEXT("index"),
				TEXT("Could not fingerprint the immutable project index."));
		}
		CommitWorkerResult(MoveTemp(Captured));
	});
	return true;
}

void FHyperAIStudioProjectIndexStore::CommitWorkerResult(FHyperAIProjectIndexValueSnapshot&& Result)
{
	FScopeLock Lock(&Mutex);
	if (!bShuttingDown)
	{
		Snapshot = MoveTemp(Result);
		SnapshotView.Reset();
	}
	bWorkerRunning = false;
}

void FHyperAIStudioProjectIndexStore::Shutdown()
{
	{
		FScopeLock Lock(&Mutex);
		bShuttingDown = true;
	}
	if (WorkerFuture.IsValid())
	{
		WorkerFuture.Wait();
	}
	FScopeLock Lock(&Mutex);
	bWorkerRunning = false;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioProjectIndexStore::SetSnapshotForTests(
	const FHyperAIProjectIndexValueSnapshot& InSnapshot)
{
	FScopeLock Lock(&Mutex);
	if (!bWorkerRunning)
	{
		Snapshot = InSnapshot;
		SnapshotView.Reset();
		NextGeneration = FMath::Max(NextGeneration, Snapshot.Value.Generation + 1);
	}
}
#endif

namespace HyperAIStudio::ContextSearch::Private
{
	FHyperAIContextSearchValueRecord ProjectRecordFields(
		const FHyperAIContextSearchValueRecord& Source,
		const TArray<FString>& Projection,
		const FString& QueryId)
	{
		const TSet<FString> ProjectionSet(Projection);
		FHyperAIContextSearchValueRecord Result;
		Result.QueryId = QueryId;
		Result.Kind = Source.Kind;
		Result.RecordId = Source.RecordId;
		Result.SearchTextLower = Source.SearchTextLower;
		for (const FHyperAIReadField& Field : Source.Fields)
		{
			if (ProjectionSet.Contains(Field.Name))
			{
				Result.Fields.Add(Field);
			}
		}
		SortFields(Result);
		return Result;
	}

	bool IsContextKind(const FString& Kind)
	{
		return Kind == TEXT("project") || Kind == TEXT("world") || Kind == TEXT("viewport")
			|| Kind == TEXT("selected_actor") || Kind == TEXT("selected_asset")
			|| Kind == TEXT("blueprint_graph") || Kind == TEXT("blueprint_node")
			|| Kind == TEXT("blueprint_pin");
	}

	TSet<FString> MakeSceneAllowedObjectPaths(
		const FHyperAIContextSearchValueSnapshot& Snapshot,
		const FString& Scope,
		const FString& ActorPath)
	{
		TSet<FString> Allowed;
		for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Records)
		{
			if (Record.Kind != TEXT("actor"))
			{
				continue;
			}
			const FString Path = FindFieldValue(Record, TEXT("path"));
			const bool bInclude = Scope == TEXT("world")
				|| (Scope == TEXT("selected") && FindFieldValue(Record, TEXT("selected")) == TEXT("true"))
				|| (Scope == TEXT("actor") && Path == ActorPath);
			if (bInclude)
			{
				Allowed.Add(Path);
			}
		}
		for (const FHyperAIContextSearchValueRecord& Record : Snapshot.Records)
		{
			if (Record.Kind == TEXT("component") && Allowed.Contains(FindFieldValue(Record, TEXT("owner_path"))))
			{
				Allowed.Add(FindFieldValue(Record, TEXT("path")));
			}
		}
		return Allowed;
	}

	bool SceneRecordAllowed(
		const FHyperAIContextSearchValueRecord& Record,
		const TSet<FString>& AllowedPaths)
	{
		if (Record.Kind == TEXT("actor"))
		{
			return AllowedPaths.Contains(FindFieldValue(Record, TEXT("path")));
		}
		if (Record.Kind == TEXT("component"))
		{
			return AllowedPaths.Contains(FindFieldValue(Record, TEXT("owner_path")));
		}
		if (Record.Kind == TEXT("property"))
		{
			return AllowedPaths.Contains(FindFieldValue(Record, TEXT("property_owner_path")));
		}
		return false;
	}

	void MergeDiagnostics(
		TArray<FHyperAIReadDiagnostic>& Target,
		const TArray<FHyperAIReadDiagnostic>& Source,
		const FString& QueryId)
	{
		for (const FHyperAIReadDiagnostic& Diagnostic : Source)
		{
			if (Target.Num() >= FHyperAIStudioContextSearchContracts::MaxDiagnostics)
			{
				break;
			}
			FHyperAIReadDiagnostic Copy = Diagnostic;
			Copy.Field = QueryId + TEXT(":") + Copy.Field;
			Target.Add(MoveTemp(Copy));
		}
	}
}

FHyperAIReadReport FHyperAIStudioContextSearchContracts::AnalyzeBatch(
	const FHyperAIContextSearchValueSnapshot& EditorSnapshot,
	const FHyperAIProjectIndexValueSnapshot& ProjectSnapshot,
	const FHyperAIBatchQueryRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	FString ErrorCode;
	FString Error;
	if (!ValidateCommonPageBounds(Request.PageSize, Request.Cursor, Request.MaxOutputBytes, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("request"), Error);
	}
	if (Request.Reads.IsEmpty() || Request.Reads.Num() > MaxBatchReads)
	{
		return InvalidReport(
			TEXT("invalid_batch_size"),
			TEXT("reads"),
			TEXT("Batch must contain 1-8 allowlisted read operations."));
	}

	static const TSet<FString> PrimitiveAllowlist = {
		TEXT("context"), TEXT("scene"), TEXT("project_search"), TEXT("project_index_status")
	};
	TSet<FString> QueryIds;
	FHyperAIContextSearchValueSnapshot Combined;
	Combined.SnapshotUtc = EditorSnapshot.SnapshotUtc.IsEmpty()
		? ProjectSnapshot.Value.SnapshotUtc
		: EditorSnapshot.SnapshotUtc;
	Combined.ObservationScope = TEXT("compound_loaded_editor_and_project_index");
	Combined.bOnDiskOnly = false;
	Combined.bDirty = EditorSnapshot.bDirty || ProjectSnapshot.Value.bDirty;
	Combined.bIncomplete = EditorSnapshot.bIncomplete;
	Combined.bSourceTruncated = EditorSnapshot.bSourceTruncated;
	Combined.Generation = ProjectSnapshot.Value.Generation;
	Combined.DirtyPackages = EditorSnapshot.DirtyPackages;
	Combined.DirtyPackages.Append(ProjectSnapshot.Value.DirtyPackages);
	Combined.DirtyPackages.Sort();
	for (int32 Index = Combined.DirtyPackages.Num() - 1; Index > 0; --Index)
	{
		if (Combined.DirtyPackages[Index] == Combined.DirtyPackages[Index - 1])
		{
			Combined.DirtyPackages.RemoveAt(Index);
		}
	}
	if (Combined.DirtyPackages.Num() > MaxDirtyPackages)
	{
		Combined.DirtyPackages.SetNum(MaxDirtyPackages);
		Combined.bDirtyPackageListTruncated = true;
	}
	Combined.bDirtyPackageListTruncated |= EditorSnapshot.bDirtyPackageListTruncated
		|| ProjectSnapshot.Value.bDirtyPackageListTruncated;
	Combined.Diagnostics = EditorSnapshot.Diagnostics;

	TArray<FString> RequestTokens = {
		TEXT("batch_query.v1"), LexToString(Request.PageSize), LexToString(Request.MaxOutputBytes)
	};
	TSet<FString> OuterProjectionSet;
	for (const FHyperAIBatchReadOperation& RawOperation : Request.Reads)
	{
		if (RawOperation.QueryId.Len() > MaxQueryIdCharacters
			|| RawOperation.Primitive.Len() > 32
			|| RawOperation.Scope.Len() > 16
			|| RawOperation.Query.Len() > MaxQueryCharacters
			|| RawOperation.ActorPath.Len() > MaxPathCharacters
			|| RawOperation.Kinds.Num() > 2
			|| RawOperation.Fields.Num() > 64
			|| RawOperation.PropertyNames.Num() > MaxProperties
			|| ContainsEmbeddedNull(RawOperation.QueryId)
			|| ContainsEmbeddedNull(RawOperation.Primitive)
			|| ContainsEmbeddedNull(RawOperation.Scope)
			|| ContainsEmbeddedNull(RawOperation.Query)
			|| ContainsEmbeddedNull(RawOperation.ActorPath)
			|| !HasWellFormedUtf16(RawOperation.QueryId)
			|| !HasWellFormedUtf16(RawOperation.Primitive)
			|| !HasWellFormedUtf16(RawOperation.Scope)
			|| !HasWellFormedUtf16(RawOperation.Query)
			|| !HasWellFormedUtf16(RawOperation.ActorPath))
		{
			return InvalidReport(
				TEXT("invalid_batch_scalar_bound"),
				TEXT("reads"),
				TEXT("A batch scalar is oversized, malformed UTF-16, or contains an embedded null."));
		}
		FString QueryId = RawOperation.QueryId;
		QueryId.TrimStartAndEndInline();
		FString Primitive = NormalizeToken(RawOperation.Primitive);
		FString Scope = NormalizeToken(RawOperation.Scope);
		if (QueryId.IsEmpty() || QueryId.Len() > MaxQueryIdCharacters
			|| ContainsEmbeddedNull(QueryId) || QueryIds.Contains(QueryId))
		{
			return InvalidReport(
				TEXT("invalid_or_duplicate_query_id"),
				TEXT("reads.query_id"),
				TEXT("Each batch QueryId must be unique, non-empty, bounded, and contain no embedded null."));
		}
		if (!PrimitiveAllowlist.Contains(Primitive))
		{
			return InvalidReport(
				TEXT("batch_primitive_not_allowlisted"),
				TEXT("reads.primitive"),
				TEXT("Batch primitive is not in the closed read-only allowlist."));
		}
		if (RawOperation.MaxItems < 1 || RawOperation.MaxItems > MaxPageSize)
		{
			return InvalidReport(
				TEXT("invalid_batch_item_bound"),
				TEXT("reads.max_items"),
				TEXT("Each batch MaxItems must be between 1 and 128."));
		}
		QueryIds.Add(QueryId);
		RequestTokens.Append({ QueryId, Primitive, LexToString(RawOperation.MaxItems) });

		if (Primitive == TEXT("context"))
		{
			if (!RawOperation.Scope.IsEmpty() || !RawOperation.Query.IsEmpty()
				|| !RawOperation.ActorPath.IsEmpty() || !RawOperation.Kinds.IsEmpty()
				|| !RawOperation.PropertyNames.IsEmpty())
			{
				return InvalidReport(
					TEXT("unexpected_batch_field"),
					QueryId,
					TEXT("Context reads accept only QueryId, Primitive, Fields, and MaxItems."));
			}
			TArray<FString> Projection;
			if (!NormalizeProjection(RawOperation.Fields, ContextProjectionAllowlist(), ContextProjectionDefaults(), Projection, ErrorCode, Error))
			{
				return InvalidReport(ErrorCode, QueryId + TEXT(".fields"), Error);
			}
			RequestTokens.Add(FString::Join(Projection, TEXT(",")));
			OuterProjectionSet.Append(Projection);
			int32 Added = 0;
			for (const FHyperAIContextSearchValueRecord& Record : EditorSnapshot.Records)
			{
				if (!IsContextKind(Record.Kind))
				{
					continue;
				}
				if (Added >= RawOperation.MaxItems)
				{
					Combined.bSourceTruncated = true;
					break;
				}
				Combined.Records.Add(ProjectRecordFields(Record, Projection, QueryId));
				++Added;
			}
		}
		else if (Primitive == TEXT("scene"))
		{
			if (!RawOperation.Query.IsEmpty() || !RawOperation.Kinds.IsEmpty())
			{
				return InvalidReport(
					TEXT("unexpected_batch_field"),
					QueryId,
					TEXT("Scene reads do not accept Query or Kinds."));
			}
			if (Scope.IsEmpty())
			{
				Scope = TEXT("selected");
			}
			if (Scope != TEXT("selected") && Scope != TEXT("world") && Scope != TEXT("actor"))
			{
				return InvalidReport(TEXT("invalid_scene_scope"), QueryId + TEXT(".scope"), TEXT("Scene scope must be selected, world, or actor."));
			}
			if (Scope == TEXT("actor") && (RawOperation.ActorPath.IsEmpty()
				|| RawOperation.ActorPath.Len() > MaxPathCharacters || ContainsEmbeddedNull(RawOperation.ActorPath)))
			{
				return InvalidReport(TEXT("invalid_actor_path"), QueryId + TEXT(".actor_path"), TEXT("Actor scope requires one bounded exact already-loaded actor path."));
			}
			TArray<FString> Projection;
			if (!NormalizeProjection(RawOperation.Fields, SceneProjectionAllowlist(), SceneProjectionDefaults(), Projection, ErrorCode, Error))
			{
				return InvalidReport(ErrorCode, QueryId + TEXT(".fields"), Error);
			}
			TArray<FString> PropertyNames;
			if (RawOperation.PropertyNames.Num() > MaxProperties)
			{
				return InvalidReport(TEXT("too_many_properties"), QueryId + TEXT(".property_names"), TEXT("PropertyNames exceeds the fixed bound."));
			}
			for (const FString& RawName : RawOperation.PropertyNames)
			{
				if (RawName.Len() > 64 || ContainsEmbeddedNull(RawName)
					|| !HasWellFormedUtf16(RawName))
				{
					return InvalidReport(TEXT("invalid_property_name"), QueryId + TEXT(".property_names"), TEXT("PropertyNames must be bounded exact C++ identifiers."));
				}
				FString Name = RawName;
				Name.TrimStartAndEndInline();
				if (!IsValidIdentifier(Name, 64))
				{
					return InvalidReport(TEXT("invalid_property_name"), QueryId + TEXT(".property_names"), TEXT("PropertyNames must be bounded exact C++ identifiers."));
				}
				PropertyNames.AddUnique(Name);
			}
			PropertyNames.Sort();
			RequestTokens.Add(FString::Join(Projection, TEXT(",")));
			RequestTokens.Add(FString::Join(PropertyNames, TEXT(",")));
			RequestTokens.Append({ Scope, RawOperation.ActorPath });
			OuterProjectionSet.Append(Projection);
			const TSet<FString> AllowedPaths = MakeSceneAllowedObjectPaths(EditorSnapshot, Scope, RawOperation.ActorPath);
			int32 Added = 0;
			for (const FHyperAIContextSearchValueRecord& Record : EditorSnapshot.Records)
			{
				if (!SceneRecordAllowed(Record, AllowedPaths))
				{
					continue;
				}
				if (Added >= RawOperation.MaxItems)
				{
					Combined.bSourceTruncated = true;
					break;
				}
				Combined.Records.Add(ProjectRecordFields(Record, Projection, QueryId));
				++Added;
			}
		}
		else if (Primitive == TEXT("project_search"))
		{
			if (!RawOperation.Scope.IsEmpty() || !RawOperation.ActorPath.IsEmpty()
				|| !RawOperation.PropertyNames.IsEmpty())
			{
				return InvalidReport(
					TEXT("unexpected_batch_field"),
					QueryId,
					TEXT("Project-search reads do not accept Scope, ActorPath, or PropertyNames."));
			}
			FHyperAIProjectSearchRequest SearchRequest;
			SearchRequest.Query = RawOperation.Query;
			SearchRequest.Kinds = RawOperation.Kinds;
			SearchRequest.Fields = RawOperation.Fields;
			SearchRequest.PageSize = RawOperation.MaxItems;
			SearchRequest.MaxOutputBytes = FMath::Min(Request.MaxOutputBytes, 65536);
			SearchRequest.bStartIndexIfUnavailable = false;
			const FHyperAIReadReport Search = SearchProjectIndex(ProjectSnapshot, SearchRequest);
			if (Search.Status == TEXT("invalid_request"))
			{
				return InvalidReport(TEXT("nested_project_search_invalid"), QueryId, Search.Diagnostic);
			}
			RequestTokens.Add(Search.RequestFingerprint);
			for (const FHyperAIReadRecord& Source : Search.Records)
			{
				FHyperAIContextSearchValueRecord Record;
				Record.QueryId = QueryId;
				Record.Kind = Source.Kind;
				Record.RecordId = Source.RecordId;
				Record.Fields = Source.Fields;
				Combined.Records.Add(MoveTemp(Record));
				for (const FHyperAIReadField& Field : Source.Fields)
				{
					OuterProjectionSet.Add(Field.Name);
				}
			}
			Combined.bIncomplete |= Search.bIncomplete || !Search.bOk;
			Combined.bSourceTruncated |= Search.bTruncated;
			MergeDiagnostics(Combined.Diagnostics, Search.Diagnostics, QueryId);
		}
		else
		{
			if (!RawOperation.Scope.IsEmpty() || !RawOperation.Query.IsEmpty()
				|| !RawOperation.ActorPath.IsEmpty() || !RawOperation.Kinds.IsEmpty()
				|| !RawOperation.PropertyNames.IsEmpty())
			{
				return InvalidReport(
					TEXT("unexpected_batch_field"),
					QueryId,
					TEXT("Project-index-status reads accept only QueryId, Primitive, Fields, and MaxItems."));
			}
			TArray<FString> Projection;
			if (!NormalizeProjection(RawOperation.Fields, ProjectProjectionAllowlist(),
				{ TEXT("status"), TEXT("generation"), TEXT("asset_count"), TEXT("cpp_file_count"), TEXT("cpp_symbol_count"), TEXT("total_record_count") },
				Projection, ErrorCode, Error))
			{
				return InvalidReport(ErrorCode, QueryId + TEXT(".fields"), Error);
			}
			RequestTokens.Add(FString::Join(Projection, TEXT(",")));
			OuterProjectionSet.Append(Projection);
			FHyperAIContextSearchValueRecord StatusRecord;
			StatusRecord.QueryId = QueryId;
			StatusRecord.Kind = TEXT("project_index_status");
			StatusRecord.RecordId = TEXT("project_index_status");
			AddField(StatusRecord, TEXT("status"), TEXT("string"), ProjectSnapshot.Status);
			AddField(StatusRecord, TEXT("generation"), TEXT("integer"), LexToString(ProjectSnapshot.Value.Generation));
			AddField(StatusRecord, TEXT("asset_count"), TEXT("integer"), LexToString(ProjectSnapshot.AssetRecordCount));
			AddField(StatusRecord, TEXT("cpp_file_count"), TEXT("integer"), LexToString(ProjectSnapshot.CppFileCount));
			AddField(StatusRecord, TEXT("cpp_symbol_count"), TEXT("integer"), LexToString(ProjectSnapshot.CppSymbolCount));
			AddField(StatusRecord, TEXT("total_record_count"), TEXT("integer"), LexToString(ProjectSnapshot.Value.Records.Num()));
			Combined.Records.Add(ProjectRecordFields(StatusRecord, Projection, QueryId));
			Combined.bIncomplete |= ProjectSnapshot.Status != TEXT("ready");
		}
	}

	SortAndDeduplicateRecords(Combined.Records);
	TArray<FString> OuterProjection = OuterProjectionSet.Array();
	OuterProjection.Sort();
	const FString RequestFingerprint = HashTokens(RequestTokens);
	FHyperAIReadReport Report;
	ProjectPage(
		Combined,
		OuterProjection,
		RequestFingerprint,
		Request.PageSize,
		Request.Cursor,
		Request.MaxOutputBytes,
		Report);
	return Report;
}

FHyperAIReadReport UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(
	const FHyperAIContextSnapshotRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	FString ErrorCode;
	FString Error;
	if (!ValidateCommonPageBounds(Request.PageSize, Request.Cursor, Request.MaxOutputBytes, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("request"), Error);
	}
	if (Request.MaxSelectedAssets != 0)
	{
		return InvalidReport(
			TEXT("selected_assets_not_supported"),
			TEXT("max_selected_assets"),
			TEXT("MaxSelectedAssets must be exactly zero; selected assets are outside this hard-bounded compound snapshot."));
	}
	if (Request.MaxSelectedActors < 0 || Request.MaxSelectedActors > FHyperAIStudioContextSearchContracts::MaxSelectedActors
		|| Request.MaxBlueprintNodes < 0 || Request.MaxBlueprintNodes > FHyperAIStudioContextSearchContracts::MaxBlueprintNodes)
	{
		return InvalidReport(
			TEXT("invalid_context_bound"),
			TEXT("max_selected_items"),
			TEXT("One or more context collection limits are outside their fixed supported range."));
	}
	TArray<FString> Projection;
	if (!FHyperAIStudioContextSearchContracts::NormalizeProjection(
		Request.Fields,
		ContextProjectionAllowlist(),
		ContextProjectionDefaults(),
		Projection,
		ErrorCode,
		Error))
	{
		return InvalidReport(ErrorCode, TEXT("fields"), Error);
	}
	if (!IsInGameThread())
	{
		return InvalidReport(
			TEXT("game_thread_required"),
			TEXT("thread"),
			TEXT("Context UObject/editor capture must run on the Unreal game thread."));
	}

	const FHyperAIContextSearchValueSnapshot Snapshot = CaptureContextSnapshot(
		Projection,
		Request.MaxSelectedActors,
		Request.MaxBlueprintNodes,
		Request.bIncludeBlueprintPins);
	const FString RequestFingerprint = FHyperAIStudioContextSearchContracts::HashTokens({
		TEXT("context_snapshot.v1"),
		FString::Join(Projection, TEXT(",")),
		LexToString(Request.MaxSelectedActors),
		LexToString(Request.MaxSelectedAssets),
		LexToString(Request.MaxBlueprintNodes),
		Request.bIncludeBlueprintPins ? TEXT("1") : TEXT("0"),
		LexToString(Request.PageSize),
		LexToString(Request.MaxOutputBytes)
	});
	FHyperAIReadReport Report;
	FHyperAIStudioContextSearchContracts::ProjectPage(
		Snapshot,
		Projection,
		RequestFingerprint,
		Request.PageSize,
		Request.Cursor,
		Request.MaxOutputBytes,
		Report);
	return Report;
}

FHyperAIReadReport UHyperAIStudioSceneInspectToolset::hyper_scene_inspect(
	const FHyperAISceneInspectRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	FString ErrorCode;
	FString Error;
	if (!ValidateCommonPageBounds(Request.PageSize, Request.Cursor, Request.MaxOutputBytes, ErrorCode, Error))
	{
		return InvalidReport(ErrorCode, TEXT("request"), Error);
	}
	if (Request.Scope.Len() > 16 || ContainsEmbeddedNull(Request.Scope)
		|| !HasWellFormedUtf16(Request.Scope))
	{
		return InvalidReport(
			TEXT("invalid_scene_scope"),
			TEXT("scope"),
			TEXT("Scope must be a bounded well-formed selected, world, or actor token."));
	}
	FString Scope = NormalizeToken(Request.Scope);
	if (Scope.IsEmpty())
	{
		Scope = TEXT("selected");
	}
	if (Scope != TEXT("selected") && Scope != TEXT("world") && Scope != TEXT("actor"))
	{
		return InvalidReport(
			TEXT("invalid_scene_scope"),
			TEXT("scope"),
			TEXT("Scope must be selected, world, or actor."));
	}
	if (Scope == TEXT("actor")
		&& (Request.ActorPath.IsEmpty() || Request.ActorPath.Len() > MaxPathCharacters
			|| ContainsEmbeddedNull(Request.ActorPath) || !HasWellFormedUtf16(Request.ActorPath)
			|| !Request.ActorPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| !Request.ActorPath.Contains(TEXT(":"))))
	{
		return InvalidReport(
			TEXT("invalid_actor_path"),
			TEXT("actor_path"),
			TEXT("Actor scope requires one bounded exact already-loaded actor path."));
	}
	if (Scope != TEXT("actor") && !Request.ActorPath.IsEmpty())
	{
		return InvalidReport(
			TEXT("unexpected_actor_path"),
			TEXT("actor_path"),
			TEXT("ActorPath is accepted only with actor scope."));
	}
	if (Request.MaxActors < 1 || Request.MaxActors > FHyperAIStudioContextSearchContracts::MaxWorldActors
		|| Request.MaxComponentsPerActor < 0
		|| Request.MaxComponentsPerActor > FHyperAIStudioContextSearchContracts::MaxComponentsPerActor)
	{
		return InvalidReport(
			TEXT("invalid_scene_bound"),
			TEXT("max_actors"),
			TEXT("Scene actor/component limits are outside their fixed supported range."));
	}
	TArray<FString> Projection;
	if (!FHyperAIStudioContextSearchContracts::NormalizeProjection(
		Request.Fields,
		SceneProjectionAllowlist(),
		SceneProjectionDefaults(),
		Projection,
		ErrorCode,
		Error))
	{
		return InvalidReport(ErrorCode, TEXT("fields"), Error);
	}
	if (Request.PropertyNames.Num() > FHyperAIStudioContextSearchContracts::MaxProperties)
	{
		return InvalidReport(TEXT("too_many_properties"), TEXT("property_names"), TEXT("PropertyNames exceeds the fixed bound."));
	}
	TArray<FString> PropertyNames;
	for (const FString& RawName : Request.PropertyNames)
	{
		if (RawName.Len() > 64 || ContainsEmbeddedNull(RawName) || !HasWellFormedUtf16(RawName))
		{
			return InvalidReport(
				TEXT("invalid_property_name"),
				TEXT("property_names"),
				TEXT("Every property name must be a bounded exact C++ identifier."));
		}
		FString Name = RawName;
		Name.TrimStartAndEndInline();
		if (!IsValidIdentifier(Name, 64))
		{
			return InvalidReport(
				TEXT("invalid_property_name"),
				TEXT("property_names"),
				TEXT("Every property name must be a bounded exact C++ identifier."));
		}
		PropertyNames.AddUnique(Name);
	}
	PropertyNames.Sort();
	if (!PropertyNames.IsEmpty())
	{
		for (const FString& RequiredField : { FString(TEXT("property_name")), FString(TEXT("property_type")), FString(TEXT("property_value")), FString(TEXT("property_owner_path")) })
		{
			if (!Projection.Contains(RequiredField))
			{
				Projection.Add(RequiredField);
			}
		}
		Projection.Sort();
	}
	if (!IsInGameThread())
	{
		return InvalidReport(
			TEXT("game_thread_required"),
			TEXT("thread"),
			TEXT("Scene UObject capture must run on the Unreal game thread."));
	}

	const FHyperAIContextSearchValueSnapshot Snapshot = CaptureSceneSnapshot(
		Scope,
		Request.ActorPath,
		Request.MaxActors,
		Request.MaxComponentsPerActor,
		PropertyNames);
	const FString RequestFingerprint = FHyperAIStudioContextSearchContracts::HashTokens({
		TEXT("scene_inspect.v1"), Scope, Request.ActorPath,
		FString::Join(Projection, TEXT(",")), FString::Join(PropertyNames, TEXT(",")),
		LexToString(Request.MaxActors), LexToString(Request.MaxComponentsPerActor),
		LexToString(Request.PageSize), LexToString(Request.MaxOutputBytes)
	});
	FHyperAIReadReport Report;
	FHyperAIStudioContextSearchContracts::ProjectPage(
		Snapshot,
		Projection,
		RequestFingerprint,
		Request.PageSize,
		Request.Cursor,
		Request.MaxOutputBytes,
		Report);
	return Report;
}

TArray<FHyperAIReadDiagnostic> FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(
	const FHyperAIContextSearchValueSnapshot& Snapshot,
	const int32 MaxIssues,
	bool& bOutTruncated)
{
	return HyperAIStudio::ContextSearch::Private::ValidateSceneValueSnapshot(
		Snapshot, FMath::Clamp(MaxIssues, 1, MaxDiagnostics), bOutTruncated);
}

FHyperAISceneApplyPlanReport UHyperAIStudioSceneInspectToolset::hyper_scene_apply_plan(
	const FHyperAISceneApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	constexpr int32 MaxOperations = 16;
	constexpr int32 MaxActorsPerPlan = 64;
	constexpr int32 ExactComponentsPerActor = 16;
	FHyperAISceneApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Clip(Request.OperationId, 128);
	auto Fail = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations)
	{
		return Fail(TEXT("invalid_operation_count"), TEXT("A scene layout plan requires 1..16 closed operations."));
	}
	if (Request.MaxGameThreadMs < 50
		|| Request.MaxGameThreadMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioContextSearchContracts::MaxOutputBytes)
	{
		return Fail(TEXT("invalid_execution_budget"), TEXT("Game-thread or output budget is outside the shared hard bounds."));
	}
	if (!IsInGameThread())
	{
		return Fail(TEXT("game_thread_required"), TEXT("Scene CAS capture is permitted only on the editor game thread."));
	}

	TArray<FString> ActorPaths;
	TArray<FString> ExpectedRevisions;
	TArray<FVector> TargetLocations;
	TSet<FString> UniqueActors;
	for (const FHyperAISceneLayoutOperation& Operation : Request.Operations)
	{
		if (Operation.Variant.Len() != 11 || Operation.Variant != TEXT("layout_grid")
			|| Operation.ActorPaths.Num() < 2
			|| Operation.ActorPaths.Num() != Operation.ExpectedActorRevisions.Num()
			|| Operation.Columns < 1 || Operation.Columns > MaxActorsPerPlan
			|| Operation.Origin.ContainsNaN() || Operation.Spacing.ContainsNaN()
			|| !FMath::IsFinite(Operation.Origin.X) || !FMath::IsFinite(Operation.Origin.Y)
			|| !FMath::IsFinite(Operation.Origin.Z) || !FMath::IsFinite(Operation.Spacing.X)
			|| !FMath::IsFinite(Operation.Spacing.Y) || !FMath::IsFinite(Operation.Spacing.Z)
			|| Operation.Spacing.X <= 0.0 || Operation.Spacing.Y <= 0.0
			|| Operation.Origin.GetAbsMax() > 1000000000.0
			|| Operation.Spacing.GetAbsMax() > 10000000.0)
		{
			return Fail(TEXT("invalid_operation"), TEXT("Only bounded layout_grid operations with positive finite XY spacing are supported."));
		}
		if (ActorPaths.Num() > MaxActorsPerPlan - Operation.ActorPaths.Num())
		{
			return Fail(TEXT("aggregate_actor_bound_exceeded"), TEXT("A scene layout plan targets at most 64 exact actors."));
		}
		for (int32 Index = 0; Index < Operation.ActorPaths.Num(); ++Index)
		{
			const FString& ActorPath = Operation.ActorPaths[Index];
			const FString& ExpectedRevision = Operation.ExpectedActorRevisions[Index];
			if (ActorPath.IsEmpty() || ActorPath.Len() > MaxPathCharacters
				|| ContainsEmbeddedNull(ActorPath) || !HasWellFormedUtf16(ActorPath)
				|| !ActorPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
				|| !ActorPath.Contains(TEXT(":")) || UniqueActors.Contains(ActorPath)
				|| !IsCanonicalSha256(ExpectedRevision))
			{
				return Fail(TEXT("invalid_actor_binding"), TEXT("Actor paths must be unique exact loaded /Game subobjects with one canonical revision each."));
			}
			UniqueActors.Add(ActorPath);
			const int32 Row = Index / Operation.Columns;
			const int32 Column = Index % Operation.Columns;
			const FVector Target = Operation.Origin + FVector(
				static_cast<double>(Column) * Operation.Spacing.X,
				static_cast<double>(Row) * Operation.Spacing.Y,
				static_cast<double>(Row) * Operation.Spacing.Z);
			if (Target.ContainsNaN() || Target.GetAbsMax() > 1000000000.0)
			{
				return Fail(TEXT("layout_overflow"), TEXT("A derived grid target is non-finite or outside the closed world-coordinate bound."));
			}
			ActorPaths.Add(ActorPath);
			ExpectedRevisions.Add(ExpectedRevision);
			TargetLocations.Add(Target);
		}
	}

	const FHyperAIContextSearchValueSnapshot Snapshot =
		CaptureExactSceneActorSnapshot(ActorPaths, ExactComponentsPerActor);
	if (Snapshot.bIncomplete || Snapshot.bSourceTruncated || Snapshot.bDirty
		|| Snapshot.bDirtyPackageListTruncated)
	{
		return Fail(TEXT("scene_cas_incomplete"), TEXT("Every target must have a complete clean loaded snapshot within the fixed component/deadline bounds."));
	}
	TArray<FString> BaseTokens = {TEXT("hyperai.scene-layout-base.v1")};
	for (int32 Index = 0; Index < ActorPaths.Num(); ++Index)
	{
		const FHyperAIContextSearchValueSnapshot ActorSnapshot =
			SliceSceneActorSnapshot(Snapshot, ActorPaths[Index]);
		const FString CurrentRevision =
			FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(ActorSnapshot);
		if (!IsCanonicalSha256(CurrentRevision)
			|| CurrentRevision != ExpectedRevisions[Index]
			|| !ActorSnapshot.Records.ContainsByPredicate([&](const FHyperAIContextSearchValueRecord& Record)
			{
				return Record.Kind == TEXT("actor")
					&& FindFieldValue(Record, TEXT("path")) == ActorPaths[Index];
			}))
		{
			return Fail(TEXT("scene_cas_failed"), TEXT("An actor snapshot revision changed or its exact actor record is unavailable."));
		}
		BaseTokens.Add(FHyperAIStudioContextSearchContracts::HashTokens(
			{ActorPaths[Index], CurrentRevision}));
	}
	BaseTokens.Sort();
	Report.BaseRevision = FHyperAIStudioContextSearchContracts::HashTokens(BaseTokens);
	if (!IsCanonicalSha256(Report.BaseRevision))
	{
		return Fail(TEXT("base_revision_unavailable"), TEXT("The bounded scene base could not be fingerprinted."));
	}

	TSharedRef<FHyperAISceneTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAISceneTypedArtifactPayload, ESPMode::ThreadSafe>();
	Payload->BaseRevision = Report.BaseRevision;
	Payload->ActorPaths = ActorPaths;
	Payload->ExpectedActorRevisions = ExpectedRevisions;
	Payload->TargetLocations = TargetLocations;
	const FString SemanticBeforeClone = Payload->GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (!IsCanonicalSha256(SemanticBeforeClone)
		|| &Detached.Get() == static_cast<const IHyperAIStudioTypedArtifactPayload*>(&Payload.Get())
		|| Detached->GetSemanticFingerprint() != SemanticBeforeClone
		|| Payload->GetBoundedByteSize() > 256 * 1024)
	{
		return Fail(TEXT("immutable_payload_failed"), TEXT("The closed scene layout could not produce a bounded detached immutable payload."));
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!PrepareSceneLayoutArtifact(Payload, Request.MaxGameThreadMs,
		Request.MaxOutputBytes, Prepared, PrepareError))
	{
		Report.Status = TEXT("typed_artifact_prepare_failed");
		Report.Diagnostic = Clip(PrepareError, FHyperAIStudioContextSearchContracts::MaxDiagnosticCharacters);
		return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.PreparedContractFingerprint = Prepared.ContractFingerprint;
	Report.Effects.OperationCount = Request.Operations.Num();
	Report.Effects.ActorCount = ActorPaths.Num();
	Report.Effects.bTransactionRequired = true;
	Report.Effects.bCompileRequired = false;
	Report.Effects.bSaveRequired = false;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("The exact clean scene layout and shared hashes were sealed without staging or mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		return Fail(TEXT("invalid_operation_id"), TEXT("Non-dry submission requires one valid durable operation_id."));
	}
	if (!IsCanonicalSha256(Request.ExpectedPlanHash)
		|| Request.ExpectedPlanHash != Report.PlanHash)
	{
		return Fail(TEXT("expected_plan_hash_mismatch"), TEXT("Non-dry submission must echo the exact dry-run plan hash."));
	}
	return Fail(TEXT("staged_backend_required"),
		TEXT("No scene effect was staged or submitted: an admitted transaction and fresh-verifier backend is required."));
}

FHyperAISceneValidateReport UHyperAIStudioSceneInspectToolset::hyper_scene_validate(
	const FHyperAISceneValidateRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	FHyperAISceneValidateReport Report;
	FString Scope = NormalizeToken(Request.Scope);
	if (Scope.IsEmpty()) Scope = TEXT("selected");
	if ((Scope != TEXT("selected") && Scope != TEXT("world") && Scope != TEXT("actor"))
		|| Request.MaxActors < 1
		|| Request.MaxActors > FHyperAIStudioContextSearchContracts::MaxWorldActors
		|| Request.MaxComponentsPerActor < 0
		|| Request.MaxComponentsPerActor > FHyperAIStudioContextSearchContracts::MaxComponentsPerActor
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioContextSearchContracts::MaxDiagnostics
		|| Request.PropertyNames.Num() > FHyperAIStudioContextSearchContracts::MaxProperties
		|| (Scope == TEXT("actor")
			&& (Request.ActorPath.IsEmpty() || Request.ActorPath.Len() > MaxPathCharacters
				|| ContainsEmbeddedNull(Request.ActorPath) || !HasWellFormedUtf16(Request.ActorPath)
				|| !Request.ActorPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
				|| !Request.ActorPath.Contains(TEXT(":"))))
		|| (Scope != TEXT("actor") && !Request.ActorPath.IsEmpty()))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Scene validation scope, actor path, property list, or bounds are invalid.");
		return Report;
	}
	TArray<FString> PropertyNames;
	for (const FString& RawName : Request.PropertyNames)
	{
		if (RawName.Len() > FHyperAIStudioContextSearchContracts::MaxFieldNameCharacters
			|| ContainsEmbeddedNull(RawName) || !HasWellFormedUtf16(RawName))
		{
			Report.Status = TEXT("invalid_property_name");
			Report.Diagnostic = TEXT("Every property name must be one bounded exact C++ identifier.");
			return Report;
		}
		FString Name = RawName;
		Name.TrimStartAndEndInline();
		if (!IsValidIdentifier(Name, FHyperAIStudioContextSearchContracts::MaxFieldNameCharacters))
		{
			Report.Status = TEXT("invalid_property_name");
			Report.Diagnostic = TEXT("Every property name must be one bounded exact C++ identifier.");
			return Report;
		}
		PropertyNames.AddUnique(Name);
	}
	PropertyNames.Sort();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Independent scene validation requires a fresh editor game-thread capture.");
		return Report;
	}
	const FHyperAIContextSearchValueSnapshot Snapshot = CaptureSceneSnapshot(
		Scope, Request.ActorPath, Request.MaxActors, Request.MaxComponentsPerActor, PropertyNames);
	Report.SnapshotFingerprint =
		FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Snapshot);
	Report.bRevisionComplete = !Snapshot.bIncomplete && !Snapshot.bSourceTruncated
		&& IsCanonicalSha256(Report.SnapshotFingerprint);
	Report.Issues = FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(
		Snapshot, Request.MaxIssues, Report.bTruncated);
	for (const FHyperAIReadDiagnostic& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
	}
	Report.bOk = true;
	Report.bValid = Report.bRevisionComplete && Report.ErrorCount == 0;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = TEXT("A detached scene value snapshot was independently checked for identity, ownership, transform, component, and scalar-property invariants.");
	return Report;
}

FHyperAIReadReport UHyperAIStudioProjectSearchToolset::hyper_project_search(
	const FHyperAIProjectSearchRequest& Request)
{
	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	auto Snapshot = Store.GetSnapshotShared();
	if (Request.bStartIndexIfUnavailable
		&& Snapshot->Value.Records.IsEmpty()
		&& Snapshot->Status == TEXT("uninitialized"))
	{
		FString Diagnostic;
		Store.RequestRefresh(Diagnostic);
		Snapshot = Store.GetSnapshotShared();
	}
	return FHyperAIStudioContextSearchContracts::SearchProjectIndex(*Snapshot, Request);
}

FHyperAIProjectIndexStatus UHyperAIStudioProjectIndexStatusToolset::hyper_project_index_status(
	const bool bRequestRefresh)
{
	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	FString RefreshDiagnostic;
	bool bRefreshRequested = false;
	if (bRequestRefresh)
	{
		bRefreshRequested = Store.RequestRefresh(RefreshDiagnostic);
	}
	FHyperAIProjectIndexStatus Status = Store.GetStatus();
	Status.bRefreshRequested = bRefreshRequested;
	if (bRequestRefresh && !RefreshDiagnostic.IsEmpty())
	{
		Status.Diagnostic = RefreshDiagnostic + TEXT(" ") + Status.Diagnostic;
	}
	return Status;
}

FHyperAIReadReport UHyperAIStudioBatchQueryToolset::hyper_batch_query(
	const FHyperAIBatchQueryRequest& Request)
{
	using namespace HyperAIStudio::ContextSearch::Private;
	if (!IsInGameThread())
	{
		return InvalidReport(
			TEXT("game_thread_required"),
			TEXT("thread"),
			TEXT("Batch editor capture must begin on the Unreal game thread."));
	}
	const FHyperAIProjectIndexValueSnapshot EmptyProjectSnapshot;
	if (Request.Reads.IsEmpty() || Request.Reads.Num() > FHyperAIStudioContextSearchContracts::MaxBatchReads)
	{
		return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
			FHyperAIContextSearchValueSnapshot(),
			EmptyProjectSnapshot,
			Request);
	}

	bool bNeedsContext = false;
	bool bNeedsScene = false;
	bool bNeedsProjectIndex = false;
	int32 ContextItemBound = 1;
	int32 SceneActorBound = 1;
	TSet<FString> ContextProjectionSet;
	TArray<FString> SceneProperties;
	static const TSet<FString> PrimitiveAllowlist = {
		TEXT("context"), TEXT("scene"), TEXT("project_search"), TEXT("project_index_status")
	};
	for (const FHyperAIBatchReadOperation& Operation : Request.Reads)
	{
		if (Operation.Primitive.Len() > 32 || ContainsEmbeddedNull(Operation.Primitive)
			|| !HasWellFormedUtf16(Operation.Primitive)
			|| Operation.Kinds.Num() > 2 || Operation.Fields.Num() > 64
			|| Operation.PropertyNames.Num() > FHyperAIStudioContextSearchContracts::MaxProperties
			|| Operation.MaxItems < 1 || Operation.MaxItems > FHyperAIStudioContextSearchContracts::MaxPageSize)
		{
			return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
				FHyperAIContextSearchValueSnapshot(),
				EmptyProjectSnapshot,
				Request);
		}
		const FString Primitive = NormalizeToken(Operation.Primitive);
		if (!PrimitiveAllowlist.Contains(Primitive))
		{
			return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
				FHyperAIContextSearchValueSnapshot(),
				EmptyProjectSnapshot,
				Request);
		}
		bNeedsContext |= Primitive == TEXT("context");
		bNeedsScene |= Primitive == TEXT("scene");
		bNeedsProjectIndex |= Primitive == TEXT("project_search")
			|| Primitive == TEXT("project_index_status");
		if (Primitive == TEXT("context"))
		{
			ContextItemBound = FMath::Max(ContextItemBound, FMath::Clamp(Operation.MaxItems, 1, FHyperAIStudioContextSearchContracts::MaxPageSize));
			TArray<FString> NormalizedProjection;
			FString ProjectionErrorCode;
			FString ProjectionError;
			if (FHyperAIStudioContextSearchContracts::NormalizeProjection(
				Operation.Fields,
				ContextProjectionAllowlist(),
				ContextProjectionDefaults(),
				NormalizedProjection,
				ProjectionErrorCode,
				ProjectionError))
			{
				ContextProjectionSet.Append(NormalizedProjection);
			}
		}
		if (Primitive == TEXT("scene"))
		{
			SceneActorBound = FMath::Max(SceneActorBound, FMath::Clamp(Operation.MaxItems, 1, FHyperAIStudioContextSearchContracts::MaxWorldActors));
			for (const FString& PropertyName : Operation.PropertyNames)
			{
				if (PropertyName.Len() <= 64 && !ContainsEmbeddedNull(PropertyName)
					&& HasWellFormedUtf16(PropertyName) && IsValidIdentifier(PropertyName, 64))
				{
					SceneProperties.AddUnique(PropertyName);
				}
				else
				{
					return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
						FHyperAIContextSearchValueSnapshot(),
						EmptyProjectSnapshot,
						Request);
				}
			}
		}
	}
	SceneProperties.Sort();
	if (SceneProperties.Num() > FHyperAIStudioContextSearchContracts::MaxProperties)
	{
		return InvalidReport(
			TEXT("too_many_properties"),
			TEXT("reads.property_names"),
			TEXT("The union of batch scene PropertyNames exceeds the fixed bound."));
	}

	FHyperAIContextSearchValueSnapshot EditorSnapshot;
	EditorSnapshot.SnapshotUtc = NowUtc();
	EditorSnapshot.ObservationScope = TEXT("batch_editor_capture");
	const bool bContextNeedsDirtyPackages = ContextProjectionSet.Contains(TEXT("map_name"))
		|| ContextProjectionSet.Contains(TEXT("world_path"))
		|| ContextProjectionSet.Contains(TEXT("package_name"))
		|| ContextProjectionSet.Contains(TEXT("dirty"));
	if (bNeedsContext)
	{
		TArray<FString> ContextProjection = ContextProjectionSet.Array();
		ContextProjection.Sort();
		FHyperAIContextSearchValueSnapshot Context = CaptureContextSnapshot(
			ContextProjection,
			FMath::Min(ContextItemBound, FHyperAIStudioContextSearchContracts::MaxSelectedActors),
			FMath::Min(ContextItemBound, FHyperAIStudioContextSearchContracts::MaxBlueprintNodes),
			true,
			false);
		EditorSnapshot.Records.Append(MoveTemp(Context.Records));
		EditorSnapshot.Diagnostics.Append(MoveTemp(Context.Diagnostics));
		EditorSnapshot.bIncomplete |= Context.bIncomplete;
		EditorSnapshot.bSourceTruncated |= Context.bSourceTruncated;
		EditorSnapshot.bDirty |= Context.bDirty;
		EditorSnapshot.DirtyPackages.Append(Context.DirtyPackages);
		EditorSnapshot.bDirtyPackageListTruncated |= Context.bDirtyPackageListTruncated;
	}
	if (bNeedsScene)
	{
		FHyperAIContextSearchValueSnapshot Scene = CaptureSceneSnapshot(
			TEXT("world"),
			FString(),
			SceneActorBound,
			FHyperAIStudioContextSearchContracts::MaxComponentsPerActor,
			SceneProperties,
			false);
		EditorSnapshot.Records.Append(MoveTemp(Scene.Records));
		EditorSnapshot.Diagnostics.Append(MoveTemp(Scene.Diagnostics));
		EditorSnapshot.bIncomplete |= Scene.bIncomplete;
		EditorSnapshot.bSourceTruncated |= Scene.bSourceTruncated;
		EditorSnapshot.bDirty |= Scene.bDirty;
		EditorSnapshot.DirtyPackages.Append(Scene.DirtyPackages);
		EditorSnapshot.bDirtyPackageListTruncated |= Scene.bDirtyPackageListTruncated;
	}
	if (bNeedsScene || bContextNeedsDirtyPackages)
	{
		AppendDirtyPackages(EditorSnapshot, CaptureDirtyProjectPackages());
	}
	SortAndDeduplicateRecords(EditorSnapshot.Records);
	EditorSnapshot.DirtyPackages.Sort();
	for (int32 Index = EditorSnapshot.DirtyPackages.Num() - 1; Index > 0; --Index)
	{
		if (EditorSnapshot.DirtyPackages[Index] == EditorSnapshot.DirtyPackages[Index - 1])
		{
			EditorSnapshot.DirtyPackages.RemoveAt(Index);
		}
	}
	if (bNeedsProjectIndex)
	{
		const auto ProjectSnapshot =
			FHyperAIStudioProjectIndexStore::Get().GetSnapshotShared();
		return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
			EditorSnapshot,
			*ProjectSnapshot,
			Request);
	}
	return FHyperAIStudioContextSearchContracts::AnalyzeBatch(
		EditorSnapshot,
		EmptyProjectSnapshot,
		Request);
}

namespace HyperAIStudio::ContextSearch::Private
{
	TArray<UClass*> ToolsetClasses()
	{
		return {
			UHyperAIStudioBatchQueryToolset::StaticClass(),
			UHyperAIStudioContextSnapshotToolset::StaticClass(),
			UHyperAIStudioSceneInspectToolset::StaticClass(),
			UHyperAIStudioProjectSearchToolset::StaticClass(),
			UHyperAIStudioProjectIndexStatusToolset::StaticClass()
		};
	}

	FString QualifiedToolsetName(const UClass* ToolsetClass)
	{
		return ToolsetClass
			? FString(TEXT("HyperAIStudio.")) + ToolsetClass->GetName()
			: FString();
	}
}

void FHyperAIStudioContextSearchRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this,
		&FHyperAIStudioContextSearchRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioContextSearchRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	FHyperAIStudioProjectIndexStore::Get().Shutdown();
	bool bRegistryChanged = false;
	if (IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		for (UClass* ToolsetClass : OwnedToolsets)
		{
			if (ToolsetClass)
			{
				FString Error;
				if (FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
					ToolsetClass,
					HyperAIStudio::ContextSearch::Private::QualifiedToolsetName(ToolsetClass),
					Error))
				{
					bRegistryChanged = true;
				}
				else
				{
					UE_LOG(LogHyperAIStudioContextSearch, Warning,
						TEXT("Could not unregister owned context/search toolset %s: %s"),
						*HyperAIStudio::ContextSearch::Private::QualifiedToolsetName(ToolsetClass),
						*Error);
				}
			}
		}
	}
	OwnedToolsets.Reset();
	if (bRegistryChanged && !IsEngineExitRequested())
	{
		RefreshMcpToolsIfSafe();
	}
	bStarted = false;
}

bool FHyperAIStudioContextSearchRegistration::IsRegistered() const
{
	using namespace HyperAIStudio::ContextSearch::Private;
	if (!UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return false;
	}
	const bool bAllowPending = FHyperAIStudioContextSearchContracts::IsPendingTestRegistrationEnabled();
	bool bAnyRequired = false;
	for (UClass* ToolsetClass : ToolsetClasses())
	{
		const bool bRequired = FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(
			QualifiedToolsetName(ToolsetClass),
			bAllowPending);
		bAnyRequired |= bRequired;
		if (bRequired && !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			ToolsetClass,
			QualifiedToolsetName(ToolsetClass)))
		{
			return false;
		}
	}
	return bAnyRequired;
}

void FHyperAIStudioContextSearchRegistration::RegisterAfterEngineInit()
{
	using namespace HyperAIStudio::ContextSearch::Private;
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	const bool bAllowPending = FHyperAIStudioContextSearchContracts::IsPendingTestRegistrationEnabled();
	bool bRegistryChanged = false;
	for (UClass* ToolsetClass : ToolsetClasses())
	{
		if (!ToolsetClass
			|| !FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(
				QualifiedToolsetName(ToolsetClass), bAllowPending)
			|| FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				ToolsetClass,
				QualifiedToolsetName(ToolsetClass)))
		{
			continue;
		}
		FString Error;
		if (FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			ToolsetClass,
			QualifiedToolsetName(ToolsetClass),
			Error))
		{
			OwnedToolsets.Add(ToolsetClass);
			bRegistryChanged = true;
		}
		else
		{
			UE_LOG(LogHyperAIStudioContextSearch, Error,
				TEXT("ToolsetRegistry rejected source-candidate cohort %s: %s"),
				*QualifiedToolsetName(ToolsetClass),
				*Error);
		}
	}
	if (bRegistryChanged)
	{
		RefreshMcpToolsIfSafe();
	}
}

void FHyperAIStudioContextSearchRegistration::RefreshMcpToolsIfSafe() const
{
	if (!IsInGameThread() || IsEngineExitRequested())
	{
		return;
	}
	if (IModelContextProtocolModule* ModelContextProtocol =
		FModuleManager::GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol")))
	{
		ModelContextProtocol->RefreshTools();
	}
}
