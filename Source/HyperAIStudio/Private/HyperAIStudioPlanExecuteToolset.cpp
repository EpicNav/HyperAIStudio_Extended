// Games by Hyper 2026.

#include "HyperAIStudioPlanExecuteToolset.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioService.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/Toolset.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioPlanExecuteToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPlanExecute, Log, All);

namespace HyperAIStudio::PlanExecute::Private
{
	constexpr int32 MaxDiagnosticChars = 512;
	constexpr int32 MaxStagedPatches = 64;
	constexpr int32 MaxAuthorizations = 64;
	constexpr int32 MaxValidatorReceipts = 128;
	constexpr int32 MaxArchivedStatuses = 64;
	constexpr int64 MaxStagedPatchCharacters = 1024ll * 1024ll;
	constexpr int64 MaxFreshPackageBytes = 64ll * 1024ll * 1024ll;
	constexpr int64 MaxArtifactLifetimeMs = 5ll * 60ll * 1000ll;
	constexpr int64 ReceiptLifetimeMs = 60ll * 1000ll;
	const FString FoundationInspect = TEXT("foundation.inspect");
	const FString BlueprintApply = FHyperAIStudioBlueprintPatch::EditTypedOperationType;
	const FString BlueprintDelete = FHyperAIStudioBlueprintPatch::DeleteTypedOperationType;
	const FString CurrentLevelSubject = TEXT("editor.current_level");
	const FString SceneToolset = TEXT("EditorToolset.SceneTools");
	const FString CurrentLevelTool = TEXT("get_current_level");
	const FString AssetToolset = TEXT("EditorToolset.AssetTools");
	const FString AssetExistsTool = TEXT("exists");

	int64 NowUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}

	bool IsSha256(const FString& Value)
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

	bool HasWellFormedUtf16(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const uint32 Character = static_cast<uint32>(Value[Index]);
			if (Character == 0u)
			{
				return false;
			}
			if (Character >= 0xd800u && Character <= 0xdbffu)
			{
				if (Index + 1 >= Value.Len())
				{
					return false;
				}
				const uint32 Low = static_cast<uint32>(Value[Index + 1]);
				if (Low < 0xdc00u || Low > 0xdfffu)
				{
					return false;
				}
				++Index;
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
		explicit FBoundedSha256(const uint64 InByteLimit)
			: ByteLimit(InByteLimit)
		{
		}

		bool Update(const uint8* Bytes, const int32 ByteCount)
		{
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
				0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u,
				0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu,
				0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
				0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
				0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u,
				0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
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
		uint64 ByteLimit = 0;
		bool bFinalized = false;
	};

	FString HashBytes(const uint8* Bytes, const int32 ByteCount, const uint64 ByteLimit)
	{
		FBoundedSha256 Hasher(ByteLimit);
		return Hasher.Update(Bytes, ByteCount) ? Hasher.Finalize() : FString();
	}

	FString HashUtf8(const FString& Value)
	{
		if (Value.Len() > FHyperAIStudioPlanExecuteContracts::MaxHashInputBytes
			|| !HasWellFormedUtf16(Value))
		{
			return FString();
		}
		const FTCHARToUTF8 Utf8(*Value);
		if (Utf8.Length() < 0
			|| Utf8.Length() > FHyperAIStudioPlanExecuteContracts::MaxHashInputBytes)
		{
			return FString();
		}
		return HashBytes(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length(),
			FHyperAIStudioPlanExecuteContracts::MaxHashInputBytes);
	}

	void AppendToken(FString& Canonical, const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		Canonical += FString::Printf(TEXT("%d:"), Utf8.Length());
		Canonical += Value;
		Canonical += TEXT(";");
	}

	int32 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}

	bool GetStringArgument(
		const FHyperAIStudioPlanStep& Step,
		const FString& Name,
		FString& OutValue)
	{
		const FHyperAIStudioPlanValue* Value = Step.Arguments.Find(Name);
		if (!Value || Value->Type != EHyperAIStudioPlanValueType::String)
		{
			return false;
		}
		OutValue = Value->StringValue;
		return true;
	}

	const FHyperAIStudioPlanStep* FindStep(
		const FHyperAIStudioValidatedPlan& Plan,
		const FString& StepId)
	{
		return Plan.Steps.FindByPredicate(
			[&StepId](const FHyperAIStudioPlanStep& Step) { return Step.StepId == StepId; });
	}

	bool SameStringArray(const TArray<FString>& Values, const TArray<FString>& Expected)
	{
		return Values == Expected;
	}

	struct FEpicDelegateRoute
	{
		FString Toolset;
		FString Tool;
		FString JsonInput;
	};

	bool ResolveEpicInspectRoute(
		const FHyperAIStudioPlanStep& Step,
		FEpicDelegateRoute& OutRoute,
		FString& OutError)
	{
		if (Step.OperationType != FoundationInspect)
		{
			OutError = TEXT("Only the closed foundation.inspect operation may use an Epic delegate route.");
			return false;
		}
		if (!Step.Preconditions.IsEmpty())
		{
			OutError = TEXT("The current exact Epic read delegates admit no unevaluated preconditions.");
			return false;
		}
		FString Subject;
		if (!GetStringArgument(Step, TEXT("subject"), Subject))
		{
			OutError = TEXT("foundation.inspect is missing its typed subject.");
			return false;
		}
		TArray<FString> Fields;
		if (const FHyperAIStudioPlanValue* Value = Step.Arguments.Find(TEXT("fields")))
		{
			if (Value->Type != EHyperAIStudioPlanValueType::StringArray)
			{
				OutError = TEXT("foundation.inspect fields are not a typed string array.");
				return false;
			}
			Fields = Value->StringArrayValue;
		}

		if (Subject == CurrentLevelSubject
			&& (Fields.IsEmpty() || SameStringArray(Fields, {TEXT("path")})))
		{
			OutRoute.Toolset = SceneToolset;
			OutRoute.Tool = CurrentLevelTool;
			OutRoute.JsonInput = TEXT("{}");
		}
		else if (Subject.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			&& SameStringArray(Fields, {TEXT("exists")}))
		{
			OutRoute.Toolset = AssetToolset;
			OutRoute.Tool = AssetExistsTool;
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("path"), Subject);
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutRoute.JsonInput);
			if (!FJsonSerializer::Serialize(Object, Writer))
			{
				OutError = TEXT("The exact asset-existence delegate input could not be encoded.");
				return false;
			}
		}
		else
		{
			OutError = TEXT("foundation.inspect has no exact admitted backend variant for this subject/field projection.");
			return false;
		}

		if (OutRoute.Toolset.StartsWith(TEXT("HyperAIStudio."), ESearchCase::IgnoreCase)
			|| OutRoute.Tool.StartsWith(TEXT("hyper_"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Recursive HyperAI tool routing is categorically prohibited.");
			return false;
		}
		return true;
	}

	bool ValidateBlueprintStepContract(
		const FHyperAIStudioPlanStep& Step,
		FString& OutError)
	{
		FString AssetPath;
		FString PatchId;
		if ((Step.OperationType != BlueprintApply && Step.OperationType != BlueprintDelete)
			|| Step.Arguments.Num() != 2
			|| !GetStringArgument(Step, TEXT("asset_path"), AssetPath)
			|| !GetStringArgument(Step, TEXT("patch_id"), PatchId)
			|| !AssetPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| Step.Preconditions.Num() != 2)
		{
			OutError = TEXT("The Blueprint backend requires its exact two arguments and two closed preconditions.");
			return false;
		}
		const FHyperAIStudioPlanPrecondition* Exists = nullptr;
		const FHyperAIStudioPlanPrecondition* Revision = nullptr;
		for (const FHyperAIStudioPlanPrecondition& Condition : Step.Preconditions)
		{
			if (Condition.Kind == EHyperAIStudioPlanPreconditionKind::ObjectExists)
			{
				Exists = &Condition;
			}
			else if (Condition.Kind == EHyperAIStudioPlanPreconditionKind::RevisionEquals)
			{
				Revision = &Condition;
			}
		}
		if (!Exists || !Revision
			|| Exists->Target != AssetPath || !Exists->Field.IsEmpty() || !Exists->Expected.IsEmpty()
			|| Revision->Target != AssetPath || !Revision->Field.IsEmpty() || !IsSha256(Revision->Expected))
		{
			OutError = TEXT("The Blueprint backend requires exact object-exists and SHA-256 revision preconditions for its target.");
			return false;
		}

		const int32 ExpectedEffectCount = Step.OperationType == BlueprintDelete ? 2 : 1;
		if (Step.Effects.Num() != ExpectedEffectCount)
		{
			OutError = TEXT("The Blueprint backend requires its exact closed declared-effect set.");
			return false;
		}
		bool bUpdated = false;
		bool bDeleted = false;
		for (const FHyperAIStudioPlanEffect& Effect : Step.Effects)
		{
			if (Effect.Target != AssetPath)
			{
				OutError = TEXT("Every Blueprint effect must bind to the exact target asset.");
				return false;
			}
			if (Effect.Kind == EHyperAIStudioPlanEffectKind::ObjectUpdated
				&& Effect.ValidatorId == (Step.OperationType == BlueprintDelete
					? TEXT("blueprint.delete_compile_validate") : TEXT("blueprint.compile_validate"))
				&& Effect.Expected == TEXT("compiled"))
			{
				bUpdated = true;
			}
			else if (Step.OperationType == BlueprintDelete
				&& Effect.Kind == EHyperAIStudioPlanEffectKind::ObjectDeleted
				&& Effect.ValidatorId == TEXT("blueprint.delete_compile_validate")
				&& Effect.Expected == TEXT("requested_nodes_absent"))
			{
				bDeleted = true;
			}
			else
			{
				OutError = TEXT("A Blueprint effect does not match the exact compile/delete validator contract.");
				return false;
			}
		}
		if (!bUpdated || (Step.OperationType == BlueprintDelete && !bDeleted))
		{
			OutError = TEXT("The Blueprint declared-effect set is incomplete or duplicated.");
			return false;
		}
		return true;
	}

	FString MakeBlueprintObjectPath(const FString& AssetPath)
	{
		if (!AssetPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		{
			return FString();
		}
		if (AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		const FString Name = FPackageName::GetShortName(AssetPath);
		return Name.IsEmpty() ? FString() : AssetPath + TEXT(".") + Name;
	}

	UBlueprint* FindLoadedBlueprintExact(const FString& AssetPath)
	{
		const FString ObjectPath = MakeBlueprintObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		UBlueprint* Blueprint = FindObject<UBlueprint>(
			nullptr, ObjectPath, EFindObjectFlags::None);
		if (!Blueprint)
		{
			return nullptr;
		}
		const FString PackagePath = Blueprint->GetOutermost()->GetName();
		return (AssetPath == ObjectPath || AssetPath == PackagePath) ? Blueprint : nullptr;
	}

	class FProductionClock final : public IHyperAIStudioPlanClock
	{
	public:
		virtual int64 NowMonotonicMs() const override
		{
			return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
		}

		virtual int64 NowUtcMs() const override
		{
			return Private::NowUtcMs();
		}
	};

	struct FAuthorizationEntry
	{
		FHyperAIStudioPlanAuthorizationRequest Request;
		FString Nonce;
		int64 ExpiresUtcMs = 0;
		bool bConsumed = false;
	};

	/** In-process server authority. Tokens are random lookup keys; clients cannot forge claims. */
	class FServerSecurity final
		: public IHyperAIStudioPlanAuthorizationGate
		, public IHyperAIStudioPlanValidatorReceiptGate
	{
	public:
		bool IssueAuthorization(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& CanonicalProjectId,
			FString& OutToken,
			FString& OutError)
		{
			FScopeLock Lock(&Mutex);
			PruneLocked(NowUtcMs());
			if (!Plan.bValidated || (!Plan.bHasDestructive && !Plan.bHasExternalEffect)
				|| !FHyperAIStudioOperationJournal::IsValidOperationId(Plan.OperationId)
				|| !IsSha256(CanonicalProjectId) || !IsSha256(Plan.AuthorizationPlanHash)
				|| Authorizations.Num() >= MaxAuthorizations)
			{
				OutError = TEXT("A bounded validated destructive/external plan and exact project identity are required for server authorization.");
				return false;
			}
			OutToken = TEXT("auth-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			FAuthorizationEntry Entry;
			Entry.Request.Token = OutToken;
			Entry.Request.CanonicalProjectId = CanonicalProjectId;
			Entry.Request.OperationId = Plan.OperationId;
			Entry.Request.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
			Entry.Request.Safety = Plan.MaximumSafety;
			Entry.Nonce = TEXT("grant-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Entry.ExpiresUtcMs = NowUtcMs() + MaxArtifactLifetimeMs;
			Authorizations.Add(OutToken, MoveTemp(Entry));
			return true;
		}

		bool AdmitCompletedReplay(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& CanonicalProjectId,
			const FHyperAIStudioOperationRecord& Record,
			FString& OutError)
		{
			if (!Plan.bValidated || (!Plan.bHasDestructive && !Plan.bHasExternalEffect)
				|| Plan.AuthorizationToken.IsEmpty()
				|| Record.State != EHyperAIStudioOperationState::Completed
				|| Record.CanonicalProjectId != CanonicalProjectId
				|| Record.OperationId != Plan.OperationId
				|| Record.PlanHash != Plan.PlanHash
				|| Record.CapabilityHash != Plan.CapabilityHash
				|| Record.TerminalEvidence.Version != FHyperAIStudioOperationEvidence::CurrentVersion
				|| Record.TerminalEvidence.EffectFingerprint != Plan.EffectFingerprint)
			{
				OutError = TEXT("Only an exact evidence-backed completed journal record may authorize an idempotent risky replay.");
				return false;
			}
			FScopeLock Lock(&Mutex);
			PruneLocked(NowUtcMs());
			if (Authorizations.Num() >= MaxAuthorizations
				&& !Authorizations.Contains(Plan.AuthorizationToken))
			{
				OutError = TEXT("The bounded replay-authorization table is full.");
				return false;
			}
			FAuthorizationEntry Entry;
			Entry.Request.Token = Plan.AuthorizationToken;
			Entry.Request.CanonicalProjectId = CanonicalProjectId;
			Entry.Request.OperationId = Plan.OperationId;
			Entry.Request.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
			Entry.Request.Safety = Plan.MaximumSafety;
			Entry.Nonce = TEXT("replay-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Entry.ExpiresUtcMs = NowUtcMs() + ReceiptLifetimeMs;
			Entry.bConsumed = true;
			if (const FAuthorizationEntry* ExistingGrant = Authorizations.Find(Plan.AuthorizationToken))
			{
				if (!SameAuthorizationRequest(ExistingGrant->Request, Entry.Request))
				{
					OutError = TEXT("The supplied opaque token is already bound to another server authorization.");
					return false;
				}
			}
			Authorizations.Add(Plan.AuthorizationToken, MoveTemp(Entry));
			return true;
		}

		virtual bool Inspect(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 InNowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			FScopeLock Lock(&Mutex);
			PruneLocked(InNowUtcMs);
			const FAuthorizationEntry* Entry = Authorizations.Find(Request.Token);
			if (!Entry || !SameAuthorizationRequest(Entry->Request, Request)
				|| Entry->ExpiresUtcMs <= InNowUtcMs)
			{
				OutError = TEXT("The opaque server authorization is absent, expired, or bound to another operation.");
				return false;
			}
			OutReceipt.BoundRequest = Entry->Request;
			OutReceipt.Nonce = Entry->Nonce;
			OutReceipt.ExpiresUtcMs = Entry->ExpiresUtcMs;
			OutReceipt.State = Entry->bConsumed
				? EHyperAIStudioPlanAuthorizationState::AlreadyConsumed
				: EHyperAIStudioPlanAuthorizationState::Available;
			return true;
		}

		virtual bool Consume(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 InNowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			FScopeLock Lock(&Mutex);
			PruneLocked(InNowUtcMs);
			FAuthorizationEntry* Entry = Authorizations.Find(Request.Token);
			if (!Entry || Entry->bConsumed || !SameAuthorizationRequest(Entry->Request, Request)
				|| Entry->ExpiresUtcMs <= InNowUtcMs)
			{
				OutError = TEXT("The opaque server authorization cannot be consumed exactly once for this operation.");
				return false;
			}
			Entry->bConsumed = true;
			OutReceipt.BoundRequest = Entry->Request;
			OutReceipt.Nonce = Entry->Nonce;
			OutReceipt.ExpiresUtcMs = Entry->ExpiresUtcMs;
			OutReceipt.State = EHyperAIStudioPlanAuthorizationState::AlreadyConsumed;
			return true;
		}

		FHyperAIStudioPlanValidatorReceipt IssueFreshReceipt(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& CanonicalProjectId,
			const FString& ActionNonce,
			const FString& PostconditionHash)
		{
			FHyperAIStudioPlanValidatorReceipt Receipt;
			if (!IsSha256(PostconditionHash))
			{
				return Receipt;
			}
			Receipt.ServerReceiptToken = TEXT("receipt-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Receipt.CanonicalProjectId = CanonicalProjectId;
			Receipt.OperationId = Plan.OperationId;
			Receipt.PlanHash = Plan.PlanHash;
			Receipt.CapabilityHash = Plan.CapabilityHash;
			Receipt.EffectFingerprint = Plan.EffectFingerprint;
			Receipt.ActionNonce = ActionNonce;
			Receipt.ValidatorId = FHyperAIStudioTypedPlanValidator::FreshValidatorId();
			Receipt.ApprovedValidatorFingerprint = FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint();
			Receipt.PostconditionHash = PostconditionHash;
			Receipt.IssuedUtcMs = NowUtcMs();
			Receipt.ExpiresUtcMs = Receipt.IssuedUtcMs + ReceiptLifetimeMs;
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.validator-receipt.v1"));
			AppendToken(Canonical, Receipt.ServerReceiptToken);
			AppendToken(Canonical, Receipt.CanonicalProjectId);
			AppendToken(Canonical, Receipt.OperationId);
			AppendToken(Canonical, Receipt.PlanHash);
			AppendToken(Canonical, Receipt.CapabilityHash);
			AppendToken(Canonical, Receipt.EffectFingerprint);
			AppendToken(Canonical, Receipt.ActionNonce);
			AppendToken(Canonical, Receipt.ValidatorId);
			AppendToken(Canonical, Receipt.ApprovedValidatorFingerprint);
			AppendToken(Canonical, Receipt.PostconditionHash);
			AppendToken(Canonical, FString::Printf(TEXT("%lld"), static_cast<long long>(Receipt.IssuedUtcMs)));
			AppendToken(Canonical, FString::Printf(TEXT("%lld"), static_cast<long long>(Receipt.ExpiresUtcMs)));
			Receipt.ReceiptFingerprint = HashUtf8(Canonical);
			if (!IsSha256(Receipt.ReceiptFingerprint))
			{
				return FHyperAIStudioPlanValidatorReceipt{};
			}
			FScopeLock Lock(&Mutex);
			PruneLocked(Receipt.IssuedUtcMs);
			if (ValidatorReceipts.Num() >= MaxValidatorReceipts)
			{
				return FHyperAIStudioPlanValidatorReceipt{};
			}
			ValidatorReceipts.Add(Receipt.ServerReceiptToken, Receipt);
			return Receipt;
		}

		virtual bool Verify(
			const FHyperAIStudioPlanValidatorReceipt& Receipt,
			const int64 InNowUtcMs,
			FString& OutError) override
		{
			FScopeLock Lock(&Mutex);
			PruneLocked(InNowUtcMs);
			const FHyperAIStudioPlanValidatorReceipt* Expected =
				ValidatorReceipts.Find(Receipt.ServerReceiptToken);
			if (!Expected || !SameReceipt(*Expected, Receipt) || Receipt.ExpiresUtcMs <= InNowUtcMs)
			{
				OutError = TEXT("The fresh validator receipt was not issued by this server epoch or its exact claims changed.");
				return false;
			}
			return true;
		}

		void Reset()
		{
			FScopeLock Lock(&Mutex);
			Authorizations.Reset();
			ValidatorReceipts.Reset();
		}

	private:
		static bool SameAuthorizationRequest(
			const FHyperAIStudioPlanAuthorizationRequest& A,
			const FHyperAIStudioPlanAuthorizationRequest& B)
		{
			return A.Token == B.Token
				&& A.CanonicalProjectId == B.CanonicalProjectId
				&& A.OperationId == B.OperationId
				&& A.AuthorizationPlanHash == B.AuthorizationPlanHash
				&& A.Safety == B.Safety;
		}

		static bool SameReceipt(
			const FHyperAIStudioPlanValidatorReceipt& A,
			const FHyperAIStudioPlanValidatorReceipt& B)
		{
			return A.ServerReceiptToken == B.ServerReceiptToken
				&& A.ReceiptFingerprint == B.ReceiptFingerprint
				&& A.CanonicalProjectId == B.CanonicalProjectId
				&& A.OperationId == B.OperationId
				&& A.PlanHash == B.PlanHash
				&& A.CapabilityHash == B.CapabilityHash
				&& A.EffectFingerprint == B.EffectFingerprint
				&& A.ActionNonce == B.ActionNonce
				&& A.ValidatorId == B.ValidatorId
				&& A.ApprovedValidatorFingerprint == B.ApprovedValidatorFingerprint
				&& A.PostconditionHash == B.PostconditionHash
				&& A.IssuedUtcMs == B.IssuedUtcMs
				&& A.ExpiresUtcMs == B.ExpiresUtcMs;
		}

		void PruneLocked(const int64 InNowUtcMs)
		{
			for (auto It = Authorizations.CreateIterator(); It; ++It)
			{
				if (It.Value().ExpiresUtcMs <= InNowUtcMs)
				{
					It.RemoveCurrent();
				}
			}
			for (auto It = ValidatorReceipts.CreateIterator(); It; ++It)
			{
				if (It.Value().ExpiresUtcMs <= InNowUtcMs)
				{
					It.RemoveCurrent();
				}
			}
		}

		FCriticalSection Mutex;
		TMap<FString, FAuthorizationEntry> Authorizations;
		TMap<FString, FHyperAIStudioPlanValidatorReceipt> ValidatorReceipts;
	};

	struct FFileIdentitySnapshot
	{
		FString FinalPath;
		uint64 VolumeSerial = 0;
		uint64 FileId = 0;
		uint64 LastWriteTicks = 0;
		int64 Size = 0;
	};

	#if PLATFORM_WINDOWS
	class FScopedWinReadLease final
	{
	public:
		explicit FScopedWinReadLease(const HANDLE InHandle)
			: Handle(InHandle)
		{
		}

		~FScopedWinReadLease()
		{
			if (IsValid())
			{
				::CloseHandle(Handle);
			}
		}

		FScopedWinReadLease(const FScopedWinReadLease&) = delete;
		FScopedWinReadLease& operator=(const FScopedWinReadLease&) = delete;

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
	#endif

	struct FFileHashResult
	{
		bool bOk = false;
		FString Hash;
		FString IdentityFingerprint;
		FString Error;
		int64 Size = 0;
		FFileIdentitySnapshot Identity;
	#if PLATFORM_WINDOWS
		TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> ReadLease;
	#endif
	};

	#if PLATFORM_WINDOWS
	TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> OpenPackageReadLease(const FString& Filename)
	{
		const HANDLE Handle = ::CreateFileW(
			*Filename,
			GENERIC_READ | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr);
		if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
		{
			return nullptr;
		}
		return MakeShared<FScopedWinReadLease, ESPMode::ThreadSafe>(Handle);
	}

	TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> OpenDirectoryReadLease(const FString& Directory)
	{
		const HANDLE Handle = ::CreateFileW(
			*Directory,
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
		{
			return nullptr;
		}
		return MakeShared<FScopedWinReadLease, ESPMode::ThreadSafe>(Handle);
	}

	bool GetFinalHandlePath(const HANDLE Handle, FString& OutPath)
	{
		OutPath.Reset();
		if (!Handle || Handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		constexpr DWORD MaxFinalPathCharacters = 32768;
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

	bool IsFinalPathUnderRoot(const FString& FinalRoot, const FString& FinalCandidate)
	{
		const FString RootPrefix = FinalRoot.EndsWith(TEXT("\\"))
			? FinalRoot
			: FinalRoot + TEXT("\\");
		return FinalCandidate.StartsWith(RootPrefix, ESearchCase::IgnoreCase);
	}

	bool CaptureFileIdentity(const HANDLE Handle, FFileIdentitySnapshot& OutIdentity)
	{
		BY_HANDLE_FILE_INFORMATION Info{};
		LARGE_INTEGER Size{};
		if (!GetFinalHandlePath(Handle, OutIdentity.FinalPath)
			|| !::GetFileInformationByHandle(Handle, &Info)
			|| !::GetFileSizeEx(Handle, &Size)
			|| (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
			|| Info.nNumberOfLinks != 1
			|| Size.QuadPart <= 0 || Size.QuadPart > MaxFreshPackageBytes)
		{
			return false;
		}
		OutIdentity.VolumeSerial = static_cast<uint64>(Info.dwVolumeSerialNumber);
		OutIdentity.FileId = (static_cast<uint64>(Info.nFileIndexHigh) << 32)
			| static_cast<uint64>(Info.nFileIndexLow);
		OutIdentity.LastWriteTicks = (static_cast<uint64>(Info.ftLastWriteTime.dwHighDateTime) << 32)
			| static_cast<uint64>(Info.ftLastWriteTime.dwLowDateTime);
		OutIdentity.Size = Size.QuadPart;
		return true;
	}

	bool IsSameFileIdentity(const FFileIdentitySnapshot& A, const FFileIdentitySnapshot& B)
	{
		return A.FinalPath.Equals(B.FinalPath, ESearchCase::IgnoreCase)
			&& A.VolumeSerial == B.VolumeSerial
			&& A.FileId == B.FileId
			&& A.LastWriteTicks == B.LastWriteTicks
			&& A.Size == B.Size;
	}

	bool CaptureContainedPackageIdentity(
		const FString& Filename,
		const HANDLE FileHandle,
		FFileIdentitySnapshot& OutIdentity)
	{
		const FString FullContentRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		const TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> RootLease =
			OpenDirectoryReadLease(FullContentRoot);
		FString FinalContentRoot;
		return RootLease.IsValid() && RootLease->IsValid()
			&& GetFinalHandlePath(RootLease->Get(), FinalContentRoot)
			&& CaptureFileIdentity(FileHandle, OutIdentity)
			&& IsFinalPathUnderRoot(FinalContentRoot, OutIdentity.FinalPath)
			&& OutIdentity.FinalPath.EndsWith(
				FPackageName::GetAssetPackageExtension(), ESearchCase::IgnoreCase)
			&& FPaths::ConvertRelativePathToFull(Filename).EndsWith(
				FPackageName::GetAssetPackageExtension(), ESearchCase::IgnoreCase);
	}
	#endif

	FFileHashResult HashPackageFile(const FString& Filename)
	{
		FFileHashResult Result;
	#if PLATFORM_WINDOWS
		Result.ReadLease = OpenPackageReadLease(FPaths::ConvertRelativePathToFull(Filename));
		if (!Result.ReadLease.IsValid() || !Result.ReadLease->IsValid()
			|| !CaptureContainedPackageIdentity(Filename, Result.ReadLease->Get(), Result.Identity))
		{
			Result.Error = TEXT("The saved package did not resolve to one contained, non-hardlinked /Game file identity.");
			return Result;
		}
		Result.Size = Result.Identity.Size;
		constexpr int32 ReadChunkBytes = 64 * 1024;
		TArray<uint8> ReadBuffer;
		ReadBuffer.SetNumUninitialized(ReadChunkBytes);
		FBoundedSha256 Hasher(static_cast<uint64>(MaxFreshPackageBytes));
		int64 Remaining = Result.Size;
		while (Remaining > 0)
		{
			const DWORD BytesToRead = static_cast<DWORD>(FMath::Min<int64>(Remaining, ReadChunkBytes));
			DWORD BytesRead = 0;
			if (!::ReadFile(
					Result.ReadLease->Get(), ReadBuffer.GetData(), BytesToRead, &BytesRead, nullptr)
				|| BytesRead != BytesToRead
				|| !Hasher.Update(ReadBuffer.GetData(), static_cast<int32>(BytesRead)))
			{
				Result.Error = TEXT("The saved package fresh readback was incomplete or exceeded its hash bound.");
				return Result;
			}
			Remaining -= static_cast<int64>(BytesRead);
		}
		Result.Hash = Hasher.Finalize();
		FFileIdentitySnapshot IdentityAfterRead;
		const TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> Reopened =
			OpenPackageReadLease(FPaths::ConvertRelativePathToFull(Filename));
		FFileIdentitySnapshot ReopenedIdentity;
		if (!IsSha256(Result.Hash)
			|| !CaptureFileIdentity(Result.ReadLease->Get(), IdentityAfterRead)
			|| !IsSameFileIdentity(Result.Identity, IdentityAfterRead)
			|| !Reopened.IsValid() || !Reopened->IsValid()
			|| !CaptureContainedPackageIdentity(Filename, Reopened->Get(), ReopenedIdentity)
			|| !IsSameFileIdentity(Result.Identity, ReopenedIdentity))
		{
			Result.Error = TEXT("The package identity changed during its fresh persisted readback.");
			return Result;
		}
		FString IdentityCanonical;
		AppendToken(IdentityCanonical, TEXT("hyperai.package-file-identity.v1"));
		AppendToken(IdentityCanonical, Result.Identity.FinalPath.ToLower());
		AppendToken(IdentityCanonical, FString::Printf(TEXT("%llu"), Result.Identity.VolumeSerial));
		AppendToken(IdentityCanonical, FString::Printf(TEXT("%llu"), Result.Identity.FileId));
		AppendToken(IdentityCanonical, FString::Printf(TEXT("%llu"), Result.Identity.LastWriteTicks));
		AppendToken(IdentityCanonical, FString::Printf(TEXT("%lld"), Result.Identity.Size));
		AppendToken(IdentityCanonical, Result.Hash);
		Result.IdentityFingerprint = HashUtf8(IdentityCanonical);
		Result.bOk = IsSha256(Result.IdentityFingerprint);
		if (!Result.bOk)
		{
			Result.Error = TEXT("The package file identity proof could not be hashed.");
		}
	#else
		Result.Error = TEXT("Secure package identity readback is unavailable on this platform.");
	#endif
		return Result;
	}

	bool VerifyPackageFileIdentity(const FString& Filename, const FFileHashResult& Result)
	{
	#if PLATFORM_WINDOWS
		if (!Result.bOk || !Result.ReadLease.IsValid() || !Result.ReadLease->IsValid())
		{
			return false;
		}
		FFileIdentitySnapshot LeaseIdentity;
		const TSharedPtr<FScopedWinReadLease, ESPMode::ThreadSafe> Reopened =
			OpenPackageReadLease(FPaths::ConvertRelativePathToFull(Filename));
		FFileIdentitySnapshot ReopenedIdentity;
		return CaptureFileIdentity(Result.ReadLease->Get(), LeaseIdentity)
			&& IsSameFileIdentity(Result.Identity, LeaseIdentity)
			&& Reopened.IsValid() && Reopened->IsValid()
			&& CaptureContainedPackageIdentity(Filename, Reopened->Get(), ReopenedIdentity)
			&& IsSameFileIdentity(Result.Identity, ReopenedIdentity);
	#else
		return false;
	#endif
	}

	class FProductionDispatcher final : public IHyperAIStudioPlanAsyncDispatcher
	{
	public:
		FProductionDispatcher(
			TSharedRef<FServerSecurity> InSecurity,
			TArray<FHyperAIStudioStagedBlueprintPatchArtifact> InArtifacts)
			: Security(MoveTemp(InSecurity))
			, Artifacts(MoveTemp(InArtifacts))
		{
		}

		virtual bool Prepare(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& InCanonicalProjectId,
			FString& OutError) override
		{
			CanonicalProjectId = InCanonicalProjectId;
			int32 BlueprintStepCount = 0;
			for (const FHyperAIStudioPlanStep& Step : Plan.Steps)
			{
				if (Step.OperationType == FoundationInspect)
				{
					FEpicDelegateRoute Route;
					if (!ResolveEpicInspectRoute(Step, Route, OutError))
					{
						return false;
					}
					continue;
				}
				if (Step.OperationType != BlueprintApply && Step.OperationType != BlueprintDelete)
				{
					OutError = TEXT("The typed operation exists as metadata but has no production execution backend.");
					return false;
				}
					++BlueprintStepCount;
					FString AssetPath;
					FString PatchId;
					if (!ValidateBlueprintStepContract(Step, OutError)
						|| !GetStringArgument(Step, TEXT("asset_path"), AssetPath)
						|| !GetStringArgument(Step, TEXT("patch_id"), PatchId))
				{
					OutError = TEXT("The Blueprint step lacks its exact typed asset_path/patch_id binding.");
					return false;
				}
				const FHyperAIStudioStagedBlueprintPatchArtifact* Artifact = Artifacts.FindByPredicate(
					[&](const FHyperAIStudioStagedBlueprintPatchArtifact& Candidate)
					{
						return Candidate.PatchId == PatchId;
					});
				if (!Artifact
					|| Artifact->CanonicalProjectId != CanonicalProjectId
					|| Artifact->OperationId != Plan.OperationId
					|| Artifact->PlanHash != Plan.PlanHash
					|| Artifact->CapabilityHash != Plan.CapabilityHash
					|| Artifact->EffectFingerprint != Plan.EffectFingerprint
					|| Artifact->AssetPath != AssetPath
					|| Artifact->Patch.TargetAssetPath != AssetPath
					|| Artifact->ExpiresUtcMs <= NowUtcMs())
				{
					OutError = TEXT("No unexpired server-staged Blueprint patch matches every project/operation/plan/capability/effect/target binding.");
					return false;
				}
				const FHyperAIStudioPlanPrecondition* Revision = Step.Preconditions.FindByPredicate(
					[](const FHyperAIStudioPlanPrecondition& Item)
					{
						return Item.Kind == EHyperAIStudioPlanPreconditionKind::RevisionEquals;
					});
				if (!Revision || Artifact->Patch.ExpectedRevision != Revision->Expected)
				{
					OutError = TEXT("The staged Blueprint patch is not bound to the plan's exact expected revision.");
					return false;
				}
				BlueprintArtifact = *Artifact;
			}
			if (BlueprintStepCount > 1)
			{
				OutError = TEXT("The current Blueprint backend admits exactly one patch step so one transaction and one compile can be proven.");
				return false;
			}
			return true;
		}

		virtual bool Dispatch(
			const FHyperAIStudioValidatedPlan& Plan,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError) override
		{
			if (!IsInGameThread())
			{
				OutError = TEXT("Typed action dispatch must begin on the Unreal game thread.");
				return false;
			}
			switch (Action.Kind)
			{
			case EHyperAIStudioPlanActionKind::Step:
			{
				const FHyperAIStudioPlanStep* Step = FindStep(Plan, Action.StepId);
				if (!Step || Step->OperationType != Action.OperationType)
				{
					OutError = TEXT("The scheduled step no longer matches the sealed plan.");
					return false;
				}
				if (Step->OperationType == FoundationInspect)
				{
					return DispatchEpic(*Step, Action, MoveTemp(Completion), OutError);
				}
				return DispatchBlueprint(Plan, *Step, Action, MoveTemp(Completion), OutError);
			}
			case EHyperAIStudioPlanActionKind::CompileOnce:
				return CompleteCompileBarrier(MoveTemp(Completion), OutError);
			case EHyperAIStudioPlanActionKind::ValidateOnce:
				return DispatchValidate(MoveTemp(Completion), OutError);
			case EHyperAIStudioPlanActionKind::SaveOnce:
				return DispatchSave(MoveTemp(Completion), OutError);
			case EHyperAIStudioPlanActionKind::VerifyFreshOnce:
				return DispatchFreshVerify(Plan, Action, MoveTemp(Completion), OutError);
			default:
				OutError = TEXT("The scheduled action kind has no closed production backend.");
				return false;
			}
		}

		virtual void Finish(const EHyperAIStudioPlanCoordinatorState FinalState) override
		{
			Artifacts.Reset();
			if (FinalState != EHyperAIStudioPlanCoordinatorState::OutcomeUnknown)
			{
				ActiveBlueprint.Reset();
			}
		}

	private:
		bool DispatchEpic(
			const FHyperAIStudioPlanStep& Step,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError)
		{
			FEpicDelegateRoute Route;
			if (!ResolveEpicInspectRoute(Step, Route, OutError))
			{
				return false;
			}
			auto RegistrySubsystem = UToolsetRegistrySubsystem::Get(TEXT("HyperAI exact delegate"));
			if (RegistrySubsystem.HasError())
			{
				OutError = RegistrySubsystem.StealError();
				return false;
			}
			auto& Registry = RegistrySubsystem.GetValue()->ToolsetRegistry;
			TSharedPtr<UE::ToolsetRegistry::FToolset> Handler = Registry.Find(Route.Toolset, false, &OutError);
			const FString FullName = Route.Toolset + TEXT(".") + Route.Tool;
			if (!Handler || !Handler->IsEnabled() || !Handler->IsToolEnabled(FullName)
				|| !Handler->ListToolNames().Contains(FullName))
			{
				OutError = TEXT("The exact allowlisted Epic delegate is absent or disabled.");
				return false;
			}
			const double StartSeconds = FPlatformTime::Seconds();
			UE::ToolsetRegistry::FToolDescriptor Descriptor;
				Descriptor.ToolsetName = Route.Toolset;
				Descriptor.ToolName = Route.Tool;
				Registry.ExecuteTool(Descriptor, Route.JsonInput).Next(
					[Completion = MoveTemp(Completion), StartSeconds,
						MaxOutputBytes = Action.Budget.MaxOutputBytes](
						TValueOrError<FString, FString>&& ToolResult) mutable
					{
						const bool bToolSucceeded = ToolResult.HasValue();
						const FString Value = bToolSucceeded ? ToolResult.StealValue() : FString();
						bool bSucceeded = bToolSucceeded && MaxOutputBytes >= 0
							&& Value.Len() <= MaxOutputBytes && HasWellFormedUtf16(Value);
						int32 ResultBytes = 0;
						if (bSucceeded)
						{
							ResultBytes = Utf8Bytes(Value);
							bSucceeded = ResultBytes >= 0 && ResultBytes <= MaxOutputBytes;
						}
						if (bToolSucceeded && !bSucceeded)
						{
							ResultBytes = MaxOutputBytes < MAX_int32 ? MaxOutputBytes + 1 : MAX_int32;
						}
					const int32 LatencyMs = FMath::Max(
						0, FMath::CeilToInt((FPlatformTime::Seconds() - StartSeconds) * 1000.0));
					auto FinishOnGameThread = [Completion = MoveTemp(Completion),
						bSucceeded, ResultBytes, LatencyMs]() mutable
					{
						FHyperAIStudioPlanActionResult Result;
						FHyperAIStudioPlanBackendTelemetry Telemetry;
						Telemetry.NativeOperations = 1;
						Telemetry.EpicDelegateCount = 1;
						Telemetry.EpicDelegateLatencyMs = LatencyMs;
						Telemetry.OutputBytes = ResultBytes;
						Result.Outcome = bSucceeded
							? EHyperAIStudioPlanActionOutcome::Succeeded
							: EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
						Telemetry.GameThreadMs = 0;
						Result.Usage.NativeOperations = Telemetry.NativeOperations;
						Result.Usage.GameThreadMs = Telemetry.GameThreadMs;
						Result.Usage.OutputBytes = Telemetry.OutputBytes;
						Completion(MoveTemp(Result), Telemetry);
					};
					if (IsInGameThread())
					{
						FinishOnGameThread();
					}
					else
					{
						AsyncTask(ENamedThreads::GameThread, MoveTemp(FinishOnGameThread));
					}
				});
			return true;
		}

		bool DispatchBlueprint(
			const FHyperAIStudioValidatedPlan& Plan,
			const FHyperAIStudioPlanStep& Step,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError)
		{
			if (!BlueprintArtifact.IsSet())
			{
				OutError = TEXT("The exact server-staged Blueprint patch is unavailable.");
				return false;
			}
			UBlueprint* Blueprint = FindLoadedBlueprintExact(BlueprintArtifact->AssetPath);
			if (!Blueprint)
			{
				OutError = TEXT("The target Blueprint is not already loaded; execution never loads assets implicitly.");
				return false;
			}
			const double StartSeconds = FPlatformTime::Seconds();
			FHyperAIBlueprintPatchResult PatchResult = FHyperAIStudioBlueprintPatch::ApplyLoaded(
				Blueprint, BlueprintArtifact->Patch, Step.OperationType);
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			Telemetry.GameThreadMs = FMath::Max(
				0, FMath::CeilToInt((FPlatformTime::Seconds() - StartSeconds) * 1000.0));
			Telemetry.NativeOperations = 1 + BlueprintArtifact->Patch.Operations.Num()
				+ (PatchResult.bCompileAttempted ? 1 : 0);
			Telemetry.OutputBytes = 256;
			Telemetry.BlueprintPatchCount = PatchResult.bMutationStarted ? 1 : 0;
			Telemetry.CompileCount = PatchResult.bCompileAttempted ? 1 : 0;
			FHyperAIStudioPlanActionResult Result;
			Result.Usage.NativeOperations = Telemetry.NativeOperations;
			Result.Usage.GameThreadMs = Telemetry.GameThreadMs;
			Result.Usage.OutputBytes = Telemetry.OutputBytes;
			if (PatchResult.bOk && PatchResult.bMutationStarted
				&& PatchResult.bCompileAttempted && PatchResult.bPreSaveValidated
				&& PatchResult.bCallerSaveRequired && !PatchResult.bSavePerformed)
			{
				ActiveBlueprint = Blueprint;
				ExpectedRevisionAfter = PatchResult.RevisionAfter;
				++ActualCompileCount;
				Result.Outcome = EHyperAIStudioPlanActionOutcome::Succeeded;
			}
			else if (!PatchResult.bMutationStarted)
			{
				Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
			}
			else if (PatchResult.Status == TEXT("outcome_unknown"))
			{
				Result.Outcome = EHyperAIStudioPlanActionOutcome::OutcomeUnknown;
				Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
			}
			else
			{
				// Backend-local undo is never promoted to a whole-plan rollback claim here.
				Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect;
				Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
			}
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}

		bool CompleteCompileBarrier(FCompletion Completion, FString& OutError)
		{
			if (!ActiveBlueprint.IsValid() || ActualCompileCount != 1)
			{
				OutError = TEXT("The Blueprint patch backend did not prove exactly one integrated compile.");
				return false;
			}
			FHyperAIStudioPlanActionResult Result;
			Result.Outcome = EHyperAIStudioPlanActionOutcome::Succeeded;
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			// ApplyLoaded performed the one compile; this scheduled barrier cannot compile again.
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}

		bool DispatchValidate(FCompletion Completion, FString& OutError)
		{
			UBlueprint* Blueprint = ActiveBlueprint.Get();
			if (!Blueprint || ActualValidateCount != 0)
			{
				OutError = TEXT("The Blueprint validation target is unavailable or validation was already consumed.");
				return false;
			}
			const double StartSeconds = FPlatformTime::Seconds();
			const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			Telemetry.NativeOperations = 1;
			Telemetry.GameThreadMs = FMath::Max(
				0, FMath::CeilToInt((FPlatformTime::Seconds() - StartSeconds) * 1000.0));
			Telemetry.OutputBytes = 256;
			Telemetry.ValidateCount = 1;
			++ActualValidateCount;
			FHyperAIStudioPlanActionResult Result;
			Result.Usage = {Telemetry.NativeOperations, Telemetry.GameThreadMs, Telemetry.OutputBytes};
			Result.Outcome = Snapshot.bComplete && Snapshot.Revision == ExpectedRevisionAfter
				&& (Snapshot.CompileStatus == TEXT("up_to_date")
					|| Snapshot.CompileStatus == TEXT("up_to_date_with_warnings"))
				? EHyperAIStudioPlanActionOutcome::Succeeded
				: EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}

		bool DispatchSave(FCompletion Completion, FString& OutError)
		{
			UBlueprint* Blueprint = ActiveBlueprint.Get();
			if (!Blueprint || ActualSaveCount != 0 || ActualValidateCount != 1)
			{
				OutError = TEXT("The exact validated Blueprint package cannot enter its one save action.");
				return false;
			}
			UPackage* Package = Blueprint->GetOutermost();
			if (!Package || !Package->GetName().StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
			{
				OutError = TEXT("Only a project-contained /Game Blueprint package may be saved.");
				return false;
			}
			if (!Package->IsFullyLoaded())
			{
				OutError = TEXT("The loaded-only executor will not implicitly fully-load a partial Blueprint package before save.");
				return false;
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.Error = GWarn;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bSaved = UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			Telemetry.NativeOperations = 1;
			Telemetry.GameThreadMs = FMath::Max(
				0, FMath::CeilToInt((FPlatformTime::Seconds() - StartSeconds) * 1000.0));
			Telemetry.OutputBytes = 128;
			Telemetry.SaveCount = 1;
			++ActualSaveCount;
			FHyperAIStudioPlanActionResult Result;
			Result.Usage = {Telemetry.NativeOperations, Telemetry.GameThreadMs, Telemetry.OutputBytes};
				Result.Outcome = FHyperAIStudioPlanExecuteContracts::ClassifySaveAttempt(
					bSaved, Package->IsDirty());
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}

		bool DispatchFreshVerify(
			const FHyperAIStudioValidatedPlan& Plan,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError)
		{
			UBlueprint* Blueprint = ActiveBlueprint.Get();
			if (!Blueprint || ActualFreshVerifyCount != 0 || ActualSaveCount != 1)
			{
				OutError = TEXT("Fresh verification requires the exact once-saved Blueprint target.");
				return false;
			}
			const FHyperAIBlueprintPatchSnapshot Before = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
			UPackage* Package = Blueprint->GetOutermost();
			if (!Before.bComplete || Before.Revision != ExpectedRevisionAfter || !Package || Package->IsDirty())
			{
				OutError = TEXT("The once-saved Blueprint changed before fresh persisted verification.");
				return false;
			}
				const FString PackageName = Package->GetName();
				const FString Filename = FPackageName::LongPackageNameToFilename(
					PackageName, FPackageName::GetAssetPackageExtension());
				if (!PackageName.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
					|| Filename.IsEmpty())
				{
					OutError = TEXT("The project package has no /Game fresh-readback filename.");
					return false;
				}
			++ActualFreshVerifyCount;
			TWeakObjectPtr<UBlueprint> WeakBlueprint(Blueprint);
			const FString ExpectedRevision = ExpectedRevisionAfter;
			const FString ProjectId = CanonicalProjectId;
				TSharedRef<FServerSecurity> SecurityRef = Security;
				Async(EAsyncExecution::ThreadPool, [Filename]() { return HashPackageFile(Filename); })
					.Next([Completion = MoveTemp(Completion), WeakBlueprint, ExpectedRevision,
						Filename, Plan, Action, ProjectId, SecurityRef](FFileHashResult&& FileResult) mutable
					{
						AsyncTask(ENamedThreads::GameThread,
							[Completion = MoveTemp(Completion), WeakBlueprint, ExpectedRevision,
								Filename, Plan, Action, ProjectId,
								SecurityRef, FileResult = MoveTemp(FileResult)]() mutable
						{
							FHyperAIStudioPlanBackendTelemetry Telemetry;
							Telemetry.NativeOperations = 1;
							Telemetry.OutputBytes = 256;
							Telemetry.FreshVerifyCount = 1;
							FHyperAIStudioPlanActionResult Result;
							Result.Usage = {Telemetry.NativeOperations, 0, Telemetry.OutputBytes};
							UBlueprint* Current = WeakBlueprint.Get();
							const FHyperAIBlueprintPatchSnapshot After = Current
								? FHyperAIStudioBlueprintPatch::CaptureLoaded(Current)
								: FHyperAIBlueprintPatchSnapshot{};
								const bool bUnchanged = VerifyPackageFileIdentity(Filename, FileResult)
									&& Current && Current->GetOutermost() && !Current->GetOutermost()->IsDirty()
								&& After.bComplete && After.Revision == ExpectedRevision;
							if (!bUnchanged)
							{
								Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
								Completion(MoveTemp(Result), Telemetry);
								return;
							}
							FString Canonical;
							AppendToken(Canonical, TEXT("hyperai.blueprint-fresh-proof.v1"));
							AppendToken(Canonical, ProjectId);
							AppendToken(Canonical, Plan.OperationId);
							AppendToken(Canonical, Plan.PlanHash);
							AppendToken(Canonical, Plan.CapabilityHash);
							AppendToken(Canonical, Plan.EffectFingerprint);
								AppendToken(Canonical, ExpectedRevision);
								AppendToken(Canonical, FileResult.Hash);
								AppendToken(Canonical, FileResult.IdentityFingerprint);
							const FString PostconditionHash = HashUtf8(Canonical);
							Result.Evidence.ValidatorReceipt = SecurityRef->IssueFreshReceipt(
								Plan, ProjectId, Action.ActionNonce, PostconditionHash);
							Result.Outcome = Result.Evidence.ValidatorReceipt.IsPresent()
								? EHyperAIStudioPlanActionOutcome::Succeeded
								: EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
							Completion(MoveTemp(Result), Telemetry);
						});
				});
			return true;
		}

		TSharedRef<FServerSecurity> Security;
		TArray<FHyperAIStudioStagedBlueprintPatchArtifact> Artifacts;
		TOptional<FHyperAIStudioStagedBlueprintPatchArtifact> BlueprintArtifact;
		FString CanonicalProjectId;
		TWeakObjectPtr<UBlueprint> ActiveBlueprint;
		FString ExpectedRevisionAfter;
		int32 ActualCompileCount = 0;
		int32 ActualValidateCount = 0;
		int32 ActualSaveCount = 0;
		int32 ActualFreshVerifyCount = 0;
	};

	struct FExecutionSession
	{
		FHyperAIStudioTypedOperationRegistry Registry;
		TUniquePtr<FHyperAIStudioOperationJournal> Journal;
		TUniquePtr<FHyperAIStudioOperationJournalPlanAdapter> JournalAdapter;
		TUniquePtr<FProductionDispatcher> Dispatcher;
		FProductionClock Clock;
		FHyperAIStudioPlanExecutionRuntime Runtime;
		FTSTicker::FDelegateHandle TickerHandle;
	};

	struct FServiceState
	{
		FCriticalSection Mutex;
		TSharedRef<FServerSecurity> Security = MakeShared<FServerSecurity>();
		TSharedPtr<FExecutionSession> Active;
		TMap<FString, FHyperAIStudioPlanExecutionStatus> Archived;
		TArray<FString> ArchiveOrder;
		TMap<FString, FHyperAIStudioStagedBlueprintPatchArtifact> StagedPatches;
		bool bShuttingDown = false;
	};

	FServiceState& GetServiceState()
	{
		static FServiceState State;
		return State;
	}

	bool ExactStatusBinding(
		const FHyperAIStudioPlanExecutionStatus& Status,
		const FHyperAIStudioValidatedPlan& Plan,
		const FString& CanonicalProjectId)
	{
		return Status.OperationId == Plan.OperationId
			&& Status.CanonicalProjectId == CanonicalProjectId
			&& Status.PlanHash == Plan.PlanHash
			&& Status.AuthorizationPlanHash == Plan.AuthorizationPlanHash
			&& Status.CapabilityHash == Plan.CapabilityHash
			&& Status.EffectFingerprint == Plan.EffectFingerprint;
	}

	void ArchiveStatusLocked(FServiceState& State, const FHyperAIStudioPlanExecutionStatus& Status)
	{
		if (Status.OperationId.IsEmpty())
		{
			return;
		}
		if (!State.Archived.Contains(Status.OperationId))
		{
			State.ArchiveOrder.Add(Status.OperationId);
		}
		State.Archived.Add(Status.OperationId, Status);
		while (State.ArchiveOrder.Num() > MaxArchivedStatuses)
		{
			State.Archived.Remove(State.ArchiveOrder[0]);
			State.ArchiveOrder.RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	FHyperAIPlanExecuteResponse ToResponse(
		const FHyperAIStudioPlanExecutionStatus& Status,
		const bool bOk)
	{
		FHyperAIPlanExecuteResponse Response;
		Response.bOk = bOk;
		Response.bAccepted = Status.bAccepted;
		Response.bTerminal = Status.bTerminal;
		Response.bReplay = Status.bReplay;
		Response.bOutcomeUnknown = Status.bOutcomeUnknown;
		Response.Status = Status.Status;
		Response.Diagnostic = FHyperAIStudioPlanExecuteContracts::ClipText(
			Status.Diagnostic, MaxDiagnosticChars);
		Response.OperationId = Status.OperationId;
		Response.ProjectIdentityHash = Status.CanonicalProjectId;
		Response.PlanHash = Status.PlanHash;
		Response.AuthorizationPlanHash = Status.AuthorizationPlanHash;
		Response.CapabilityHash = Status.CapabilityHash;
		Response.EffectFingerprint = Status.EffectFingerprint;
		Response.ActiveActionKind = Status.ActiveActionKind;
		Response.ActiveStepId = Status.ActiveStepId;
		Response.CompletedActionCount = Status.CompletedActionCount;
		Response.ScheduledActionCount = Status.ScheduledActionCount;
		Response.SupportedOperationTypes = FHyperAIStudioPlanExecutionRuntime::GetSupportedOperationTypes();
		Response.Telemetry = Status.Telemetry;
		return Response;
	}

	bool IsBoundedStagedPatch(const FHyperAIBlueprintPatch& Patch)
	{
		if (Patch.TargetAssetPath.IsEmpty()
			|| !Patch.TargetAssetPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| Patch.TargetAssetPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters
			|| !HasWellFormedUtf16(Patch.TargetAssetPath)
			|| !IsSha256(Patch.ExpectedRevision)
			|| Patch.Operations.IsEmpty()
			|| Patch.Operations.Num() > FHyperAIStudioBlueprintPatch::MaxOperations)
		{
			return false;
		}

		int64 CharacterCount = Patch.TargetAssetPath.Len() + Patch.ExpectedRevision.Len();
		int32 ReferencedLayoutNodes = 0;
		auto AddText = [&CharacterCount](const FString& Value, const int32 PerValueLimit)
		{
			if (Value.Len() > PerValueLimit || !HasWellFormedUtf16(Value)
				|| CharacterCount > MaxStagedPatchCharacters - Value.Len())
			{
				return false;
			}
			CharacterCount += Value.Len();
			return true;
		};
		auto IsCoordinate = [](const int32 Value)
		{
			return FMath::Abs(static_cast<int64>(Value))
				<= FHyperAIStudioBlueprintPatch::MaxCoordinate;
		};
		auto AddGraph = [&AddText](const FHyperAIBlueprintGraphId& Graph)
		{
			return Graph.IsValid()
				&& AddText(Graph.GraphPath, FHyperAIStudioBlueprintPatch::MaxPathCharacters);
		};
		auto AddNode = [&AddGraph](const FHyperAIBlueprintNodeId& Node)
		{
			return Node.IsValid() && AddGraph(Node.Graph);
		};
		auto AddPin = [&AddNode](const FHyperAIBlueprintPinId& Pin)
		{
			return Pin.IsValid() && AddNode(Pin.Node);
		};

		for (const FHyperAIBlueprintPatchOperation& Operation : Patch.Operations)
		{
			if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
				if (!AddNode(Value.Node) || !IsCoordinate(Value.X) || !IsCoordinate(Value.Y))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
				if (!AddGraph(Value.Graph) || !Value.NewNodeGuid.IsValid()
					|| static_cast<uint8>(Value.Kind) >= FHyperAIStudioBlueprintPatch::AllowedCreateNodeKindCount
					|| !IsCoordinate(Value.X) || !IsCoordinate(Value.Y))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCompileOnly>())
			{
				continue;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
				if (!AddPin(Value.A) || !AddPin(Value.B))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
				if (!AddPin(Value.A) || !AddPin(Value.B))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
				if (!AddPin(Value.Pin)
					|| !AddText(Value.Value, FHyperAIStudioBlueprintPatch::MaxLiteralCharacters))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				if (!AddNode(Value.Node)
					|| (Value.Mode != EHyperAIBlueprintDeleteMode::OrphanOnly
						&& Value.Mode != EHyperAIBlueprintDeleteMode::ExplicitConfirmed))
				{
					return false;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
				if (!AddGraph(Value.Graph) || Value.Nodes.IsEmpty()
					|| Value.Nodes.Num() > FHyperAIStudioBlueprintPatch::MaxLayoutNodes
					|| ReferencedLayoutNodes > FHyperAIStudioBlueprintPatch::MaxNodes - Value.Nodes.Num()
					|| Value.Columns <= 0
					|| !IsCoordinate(Value.OriginX) || !IsCoordinate(Value.OriginY)
					|| !IsCoordinate(Value.HorizontalSpacing) || !IsCoordinate(Value.VerticalSpacing))
				{
					return false;
				}
				ReferencedLayoutNodes += Value.Nodes.Num();
				for (const FHyperAIBlueprintNodeId& Node : Value.Nodes)
				{
					if (!AddNode(Node))
					{
						return false;
					}
				}
			}
			else
			{
				return false;
			}
		}
		return CharacterCount <= MaxStagedPatchCharacters;
	}

	FString ComputeStagedPatchFingerprint(const FHyperAIBlueprintPatch& Patch)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.staged-blueprint-patch.v1"));
		AppendToken(Canonical, Patch.TargetAssetPath);
		AppendToken(Canonical, Patch.ExpectedRevision);
		AppendToken(Canonical, FString::FromInt(Patch.Operations.Num()));
		for (const FHyperAIBlueprintPatchOperation& Operation : Patch.Operations)
		{
			AppendToken(Canonical, Operation.Kind());
			if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
				AppendToken(Canonical, Value.Node.StableKey());
				AppendToken(Canonical, FString::FromInt(Value.X));
				AppendToken(Canonical, FString::FromInt(Value.Y));
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
				AppendToken(Canonical, Value.Graph.StableKey());
				AppendToken(Canonical, Value.NewNodeGuid.ToString(EGuidFormats::Digits));
				AppendToken(Canonical, FString::FromInt(static_cast<uint8>(Value.Kind)));
				AppendToken(Canonical, FString::FromInt(Value.X));
				AppendToken(Canonical, FString::FromInt(Value.Y));
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
				AppendToken(Canonical, Value.A.StableKey());
				AppendToken(Canonical, Value.B.StableKey());
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
				AppendToken(Canonical, Value.A.StableKey());
				AppendToken(Canonical, Value.B.StableKey());
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
				AppendToken(Canonical, Value.Pin.StableKey());
				AppendToken(Canonical, Value.Value);
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				AppendToken(Canonical, Value.Node.StableKey());
				AppendToken(Canonical, FString::FromInt(static_cast<uint8>(Value.Mode)));
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
				AppendToken(Canonical, Value.Graph.StableKey());
				AppendToken(Canonical, FString::FromInt(Value.Nodes.Num()));
				for (const FHyperAIBlueprintNodeId& Node : Value.Nodes)
				{
					AppendToken(Canonical, Node.StableKey());
				}
				AppendToken(Canonical, FString::FromInt(Value.OriginX));
				AppendToken(Canonical, FString::FromInt(Value.OriginY));
				AppendToken(Canonical, FString::FromInt(Value.Columns));
				AppendToken(Canonical, FString::FromInt(Value.HorizontalSpacing));
				AppendToken(Canonical, FString::FromInt(Value.VerticalSpacing));
			}
		}
		return HashUtf8(Canonical);
	}

	bool SameStagedArtifact(
		const FHyperAIStudioStagedBlueprintPatchArtifact& A,
		const FHyperAIStudioStagedBlueprintPatchArtifact& B)
	{
		return A.CanonicalProjectId == B.CanonicalProjectId
			&& A.OperationId == B.OperationId
			&& A.PlanHash == B.PlanHash
			&& A.CapabilityHash == B.CapabilityHash
			&& A.EffectFingerprint == B.EffectFingerprint
			&& A.PatchId == B.PatchId
			&& A.AssetPath == B.AssetPath
			&& ComputeStagedPatchFingerprint(A.Patch) == ComputeStagedPatchFingerprint(B.Patch);
	}

	void TickSession(const TSharedRef<FExecutionSession>& Session)
	{
		Session->Runtime.Tick();
		if (!Session->Runtime.IsTerminal() && !Session->Runtime.HasInFlightAction())
		{
			FString PumpError;
			Session->Runtime.Pump(PumpError);
		}
	}

	void ScheduleSession(const TSharedRef<FExecutionSession>& Session)
	{
		check(IsInGameThread());
		const FTSTicker::FDelegateHandle NewTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Session](float)
			{
				FServiceState& State = GetServiceState();
				{
					FScopeLock Lock(&State.Mutex);
					if (State.bShuttingDown)
					{
						const bool bAwaitLateCompletion = Session->Runtime.HasOutstandingDispatch();
						if (!bAwaitLateCompletion)
						{
							Session->TickerHandle.Reset();
						}
						return bAwaitLateCompletion;
					}
				}
				TickSession(Session);
				if (!Session->Runtime.IsTerminal())
				{
					return true;
				}
				const FHyperAIStudioPlanExecutionStatus Status = Session->Runtime.GetStatus();
				const bool bKeepTicking = Session->Runtime.HasOutstandingDispatch();
				{
					FScopeLock Lock(&State.Mutex);
					ArchiveStatusLocked(State, Status);
					if (State.Active.Get() == &Session.Get()
						&& !Status.bOutcomeUnknown
						&& !Session->Runtime.HasOutstandingDispatch())
					{
						State.Active.Reset();
					}
					if (!bKeepTicking)
					{
						Session->TickerHandle.Reset();
					}
				}
				// Keep the session and mutation lease alive for outcome_unknown, or until a late
				// delegate has actually returned and can no longer call into the runtime.
				return bKeepTicking;
			}),
			0.0f);
		bool bRemoveImmediately = false;
		FServiceState& State = GetServiceState();
		{
			FScopeLock Lock(&State.Mutex);
			bRemoveImmediately = State.bShuttingDown || State.Active.Get() != &Session.Get();
			if (!bRemoveImmediately)
			{
				Session->TickerHandle = NewTickerHandle;
			}
		}
		if (bRemoveImmediately)
		{
			FTSTicker::GetCoreTicker().RemoveTicker(NewTickerHandle);
		}
	}
}

bool FHyperAIStudioPlanExecutionRuntime::Start(
	const FHyperAIStudioValidatedPlan& Plan,
	const FHyperAIStudioTypedOperationRegistry& Registry,
	const FString& InCanonicalProjectId,
	IHyperAIStudioPlanJournalGate* JournalGate,
	IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
	IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
	IHyperAIStudioPlanClock* Clock,
	IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
	FString& OutError)
{
	return StartInternal(
		Plan, Registry, InCanonicalProjectId, JournalGate, AuthorizationGate,
		ValidatorReceiptGate, Clock, Dispatcher, true, OutError);
}

bool FHyperAIStudioPlanExecutionRuntime::StartTypedArtifact(
	const FHyperAIStudioValidatedPlan& Plan,
	const FHyperAIStudioTypedOperationRegistry& Registry,
	const FString& InCanonicalProjectId,
	IHyperAIStudioPlanJournalGate* JournalGate,
	IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
	IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
	IHyperAIStudioPlanClock* Clock,
	IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
	FString& OutError)
{
	return StartInternal(
		Plan, Registry, InCanonicalProjectId, JournalGate, AuthorizationGate,
		ValidatorReceiptGate, Clock, Dispatcher, false, OutError);
}

bool FHyperAIStudioPlanExecutionRuntime::StartInternal(
	const FHyperAIStudioValidatedPlan& Plan,
	const FHyperAIStudioTypedOperationRegistry& Registry,
	const FString& InCanonicalProjectId,
	IHyperAIStudioPlanJournalGate* JournalGate,
	IHyperAIStudioPlanAuthorizationGate* AuthorizationGate,
	IHyperAIStudioPlanValidatorReceiptGate* ValidatorReceiptGate,
	IHyperAIStudioPlanClock* Clock,
	IHyperAIStudioPlanAsyncDispatcher* Dispatcher,
	const bool bRequireProductionShape,
	FString& OutError)
{
	OutError.Reset();
	auto IsCanonicalProjectIdentity = [](const FString& Value)
	{
		if (HyperAIStudio::PlanExecute::Private::IsSha256(Value))
		{
			return true;
		}
		if (Value.Len() != 40)
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
	};
	if (bStarted || !Clock || !Dispatcher || !IsCanonicalProjectIdentity(InCanonicalProjectId))
	{
		OutError = TEXT("The execution runtime requires one fresh instance, an exact project identity, clock and typed dispatcher.");
		return false;
	}
	if (bRequireProductionShape)
	{
		if (!ValidateExecutablePlanShape(Plan, OutError))
		{
			return false;
		}
	}
	else if (!Plan.bValidated || Plan.bDryRun || Plan.Steps.Num() != 1
		|| !FHyperAIStudioOperationJournal::IsValidOperationId(Plan.OperationId))
	{
		OutError = TEXT("Typed-artifact execution requires exactly one closed execute-mode operation.");
		return false;
	}
	if (!Dispatcher->Prepare(Plan, InCanonicalProjectId, OutError))
	{
		return false;
	}
	ActivePlan = Plan;
	CanonicalProjectId = InCanonicalProjectId;
	ActiveClock = Clock;
	ActiveDispatcher = Dispatcher;
	bStarted = true;
	const int64 StartMs = Clock->NowMonotonicMs();
	DeadlineMonotonicMs = StartMs > TNumericLimits<int64>::Max() - Plan.Budget.DeadlineMs
		? TNumericLimits<int64>::Max()
		: StartMs + Plan.Budget.DeadlineMs;
	if (!Coordinator.Start(
		Plan, Registry, CanonicalProjectId, JournalGate, AuthorizationGate,
		ValidatorReceiptGate, Clock, OutError))
	{
		LastDiagnostic = OutError;
		RefreshTerminalState();
		return false;
	}
	RefreshTerminalState();
	return true;
}

bool FHyperAIStudioPlanExecutionRuntime::Pump(FString& OutError)
{
	OutError.Reset();
	if (!bStarted || !ActiveDispatcher || IsTerminal())
	{
		OutError = TEXT("The execution runtime is not available for another action.");
		return false;
	}
	if (bActionInFlight || HasOutstandingDispatch())
	{
		OutError = TEXT("Exactly one typed action is already in flight; overlap is prohibited.");
		return false;
	}
	if (Coordinator.GetState() != EHyperAIStudioPlanCoordinatorState::Ready)
	{
		OutError = TEXT("The serial coordinator is not ready to dispatch.");
		return false;
	}
	const int32 NextActionIndex = Coordinator.GetCompletedActionCount();
	const TArray<FHyperAIStudioPlanScheduledAction>& Schedule = Coordinator.GetSchedule();
	if (!Coordinator.HasMutationCommitStarted() && Schedule.IsValidIndex(NextActionIndex))
	{
		const FHyperAIStudioPlanScheduledAction& Candidate = Schedule[NextActionIndex];
		if (Candidate.Kind == EHyperAIStudioPlanActionKind::Step
			&& Candidate.Safety != EHyperAIStudioPlanSafety::Read)
		{
			FString PreflightError;
			if (!ActiveDispatcher->PreflightBeforeCommit(ActivePlan, Candidate, PreflightError))
			{
				FString AbortError;
				Coordinator.AbortBeforeFirstDispatch(PreflightError, AbortError);
				OutError = AbortError.IsEmpty() ? PreflightError : AbortError;
				LastDiagnostic = OutError;
				RefreshTerminalState();
				return false;
			}
		}
	}
	if (!Coordinator.AcquireNextAction(ActiveAction, OutError))
	{
		LastDiagnostic = OutError;
		RefreshTerminalState();
		return false;
	}
	bActionInFlight = true;
	++DispatchGeneration;
	++OutstandingDispatchCount;
	++Telemetry.DispatchedActionCount;
	Telemetry.PeakInFlightActionCount = FMath::Max(Telemetry.PeakInFlightActionCount, 1);
	const uint64 Generation = DispatchGeneration;
	FString DispatchError;
	const bool bDispatched = ActiveDispatcher->Dispatch(
		ActivePlan,
		ActiveAction,
		[this, Generation](
			FHyperAIStudioPlanActionResult Result,
			FHyperAIStudioPlanBackendTelemetry BackendTelemetry)
		{
			Complete(Generation, MoveTemp(Result), BackendTelemetry);
		},
		DispatchError);
	if (!bDispatched)
	{
		--OutstandingDispatchCount;
		bActionInFlight = false;
		Telemetry.DispatchedActionCount = FMath::Max(0, Telemetry.DispatchedActionCount - 1);
		FString ClosureError;
		if (Coordinator.HasMutationCommitStarted())
		{
			Coordinator.CertifyCurrentActionNoEffect(DispatchError, ClosureError);
		}
		else
		{
			FHyperAIStudioPlanActionResult Result;
			Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
			Coordinator.CompleteCurrentAction(Result, ClosureError);
		}
		OutError = ClosureError.IsEmpty() ? DispatchError : ClosureError;
		LastDiagnostic = OutError;
		ActiveAction = FHyperAIStudioPlanScheduledAction{};
		RefreshTerminalState();
		return false;
	}
	return true;
}

void FHyperAIStudioPlanExecutionRuntime::ForgetAuthorizationToken()
{
	ActivePlan.AuthorizationToken.Reset();
	Coordinator.ForgetAuthorizationToken();
}

void FHyperAIStudioPlanExecutionRuntime::Tick()
{
	if (!bStarted || IsTerminal() || !ActiveClock)
	{
		return;
	}
	if (ActiveClock->NowMonotonicMs() >= DeadlineMonotonicMs)
	{
		ForceUnknownOrPreEffectFailure(TEXT("The execution deadline elapsed; any late result is ignored and no rollback is claimed."));
	}
}

bool FHyperAIStudioPlanExecutionRuntime::AbortBeforeDispatch(
	const FString& Reason,
	FString& OutError)
{
	OutError.Reset();
	if (!bStarted || IsTerminal() || bActionInFlight || HasOutstandingDispatch())
	{
		OutError = TEXT("The operation is not in a pre-dispatch state.");
		return false;
	}
	const bool bClosed = Coordinator.AbortBeforeFirstDispatch(Reason, OutError);
	LastDiagnostic = OutError;
	RefreshTerminalState();
	return bClosed;
}

bool FHyperAIStudioPlanExecutionRuntime::FailReadyActionBeforeDispatch(
	const FString& Reason,
	FString& OutError)
{
	OutError.Reset();
	if (!bStarted || IsTerminal() || bActionInFlight || HasOutstandingDispatch()
		|| Coordinator.GetState() != EHyperAIStudioPlanCoordinatorState::Ready)
	{
		OutError = TEXT("The operation is not waiting between undispatched actions.");
		return false;
	}

	if (Coordinator.GetCompletedActionCount() == 0
		&& !Coordinator.HasMutationCommitStarted()
		&& Telemetry.DispatchedActionCount == 0)
	{
		const bool bClosed = Coordinator.AbortBeforeFirstDispatch(Reason, OutError);
		LastDiagnostic = OutError.IsEmpty() ? Reason : OutError;
		RefreshTerminalState();
		return bClosed;
	}

	ActiveAction = FHyperAIStudioPlanScheduledAction{};
	FString AcquireError;
	if (!Coordinator.AcquireNextAction(ActiveAction, AcquireError))
	{
		OutError = AcquireError.IsEmpty() ? Reason : AcquireError;
		LastDiagnostic = OutError;
		ActiveAction = FHyperAIStudioPlanScheduledAction{};
		RefreshTerminalState();
		return IsTerminal();
	}

	FHyperAIStudioPlanActionResult Result;
	Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
	FString ClosureError;
	const bool bFirstMutationWasNeverDispatched =
		Coordinator.HasMutationCommitStarted()
		&& !Coordinator.HasKnownCommittedEffect()
		&& ActiveAction.Kind == EHyperAIStudioPlanActionKind::Step
		&& ActiveAction.Safety != EHyperAIStudioPlanSafety::Read;
	const bool bClosed = bFirstMutationWasNeverDispatched
		? Coordinator.CertifyCurrentActionNoEffect(Reason, ClosureError)
		: Coordinator.CompleteCurrentAction(Result, ClosureError);
	++Telemetry.CompletedActionCount;
	ActiveAction = FHyperAIStudioPlanScheduledAction{};
	OutError = ClosureError.IsEmpty() ? Reason : ClosureError;
	LastDiagnostic = OutError;
	RefreshTerminalState();
	return bClosed;
}

bool FHyperAIStudioPlanExecutionRuntime::RequestCancel(const FString& Reason, FString& OutError)
{
	OutError.Reset();
	if (!bStarted || IsTerminal())
	{
		OutError = TEXT("The operation is not active.");
		return false;
	}
	if (!bActionInFlight && !HasOutstandingDispatch()
		&& Coordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
	{
		const FString Diagnostic = Reason.IsEmpty()
			? TEXT("Cancellation was requested before the next dispatch.")
			: TEXT("Cancellation was requested before the next dispatch: ")
				+ FHyperAIStudioPlanExecuteContracts::ClipText(Reason, 256);
		return FailReadyActionBeforeDispatch(Diagnostic, OutError);
	}
	ForceUnknownOrPreEffectFailure(
		Reason.IsEmpty()
			? TEXT("Cancellation was requested; no rollback is claimed.")
			: TEXT("Cancellation was requested: ") + FHyperAIStudioPlanExecuteContracts::ClipText(Reason, 256));
	return true;
}

void FHyperAIStudioPlanExecutionRuntime::Complete(
	const uint64 InDispatchGeneration,
	FHyperAIStudioPlanActionResult Result,
	const FHyperAIStudioPlanBackendTelemetry& BackendTelemetry)
{
	OutstandingDispatchCount = FMath::Max(0, OutstandingDispatchCount - 1);
	if (!bStarted || !bActionInFlight || InDispatchGeneration != DispatchGeneration || IsTerminal())
	{
		++Telemetry.LateResultCount;
		return;
	}
	bActionInFlight = false;
	Result.Usage.NativeOperations = BackendTelemetry.NativeOperations;
	Result.Usage.GameThreadMs = BackendTelemetry.GameThreadMs;
	Result.Usage.OutputBytes = BackendTelemetry.OutputBytes;
	Telemetry.NativeOperationCount += BackendTelemetry.NativeOperations;
	Telemetry.GameThreadMs += BackendTelemetry.GameThreadMs;
	Telemetry.OutputBytes += BackendTelemetry.OutputBytes;
	Telemetry.EpicDelegateCount += BackendTelemetry.EpicDelegateCount;
	Telemetry.EpicDelegateLatencyMs += BackendTelemetry.EpicDelegateLatencyMs;
	Telemetry.BlueprintPatchCount += BackendTelemetry.BlueprintPatchCount;
	Telemetry.CompileCount += BackendTelemetry.CompileCount;
	Telemetry.ValidateCount += BackendTelemetry.ValidateCount;
	Telemetry.SaveCount += BackendTelemetry.SaveCount;
	Telemetry.FreshVerifyCount += BackendTelemetry.FreshVerifyCount;
	FString Error;
	if (!Coordinator.CompleteCurrentAction(Result, Error))
	{
		LastDiagnostic = Error;
	}
	else if (Result.Outcome != EHyperAIStudioPlanActionOutcome::Succeeded)
	{
		LastDiagnostic = TEXT("A typed backend action ended the plan without successful completion.");
	}
	++Telemetry.CompletedActionCount;
	ActiveAction = FHyperAIStudioPlanScheduledAction{};
	RefreshTerminalState();
}

void FHyperAIStudioPlanExecutionRuntime::ForceUnknownOrPreEffectFailure(const FString& Reason)
{
	if (IsTerminal())
	{
		return;
	}
	if (!bActionInFlight && !HasOutstandingDispatch()
		&& Coordinator.GetState() == EHyperAIStudioPlanCoordinatorState::Ready)
	{
		FString Error;
		FailReadyActionBeforeDispatch(Reason, Error);
		LastDiagnostic = Error.IsEmpty() ? Reason : Error;
		return;
	}
	if (bActionInFlight || HasOutstandingDispatch())
	{
		++DispatchGeneration;
		bActionInFlight = false;
		bAmbiguousOutcome = true;
		FHyperAIStudioPlanActionResult Result;
		Result.Outcome = EHyperAIStudioPlanActionOutcome::OutcomeUnknown;
		FString Error;
		Coordinator.CompleteCurrentAction(Result, Error);
		if (!Error.IsEmpty())
		{
			LastDiagnostic = Reason + TEXT(" ") + Error;
		}
		else
		{
			LastDiagnostic = Reason;
		}
		++Telemetry.CompletedActionCount;
	}
	RefreshTerminalState();
}

void FHyperAIStudioPlanExecutionRuntime::RefreshTerminalState(const FString& Diagnostic)
{
	if (!Diagnostic.IsEmpty())
	{
		LastDiagnostic = Diagnostic;
	}
	if (IsTerminal() && ActiveDispatcher && !bDispatcherFinished)
	{
		bDispatcherFinished = true;
		ActiveDispatcher->Finish(GetEffectiveState());
	}
}

FHyperAIStudioPlanExecutionStatus FHyperAIStudioPlanExecutionRuntime::GetStatus() const
{
	FHyperAIStudioPlanExecutionStatus Status;
	Status.CoordinatorState = GetEffectiveState();
	Status.Status = StateToStatus(Status.CoordinatorState);
	Status.Diagnostic = LastDiagnostic;
	if (Status.Diagnostic.IsEmpty() && Status.CoordinatorState == EHyperAIStudioPlanCoordinatorState::Completed)
	{
		Status.Diagnostic = TEXT("The serial plan completed after its required lifecycle and fresh verification gates.");
	}
	else if (Status.Diagnostic.IsEmpty()
		&& Status.CoordinatorState == EHyperAIStudioPlanCoordinatorState::ReplayCompleted)
	{
		Status.Diagnostic = TEXT("The exact evidence-backed completed operation was replayed without redispatch.");
	}
	Status.OperationId = ActivePlan.OperationId;
	Status.CanonicalProjectId = CanonicalProjectId;
	Status.PlanHash = ActivePlan.PlanHash;
	Status.AuthorizationPlanHash = ActivePlan.AuthorizationPlanHash;
	Status.CapabilityHash = ActivePlan.CapabilityHash;
	Status.EffectFingerprint = ActivePlan.EffectFingerprint;
	Status.CompletedActionCount = Coordinator.GetCompletedActionCount();
	Status.ScheduledActionCount = Coordinator.GetSchedule().Num();
	Status.bAccepted = bStarted && Status.CoordinatorState != EHyperAIStudioPlanCoordinatorState::Failed;
	Status.bTerminal = IsTerminal();
	Status.bReplay = Status.CoordinatorState == EHyperAIStudioPlanCoordinatorState::ReplayCompleted;
	Status.bOutcomeUnknown = Status.CoordinatorState == EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
	if (bActionInFlight)
	{
		Status.ActiveActionKind = FHyperAIStudioTypedPlanValidator::ActionKindToString(ActiveAction.Kind);
		Status.ActiveStepId = ActiveAction.StepId;
	}
	Status.Telemetry = Telemetry;
	return Status;
}

bool FHyperAIStudioPlanExecutionRuntime::IsTerminal() const
{
	switch (GetEffectiveState())
	{
	case EHyperAIStudioPlanCoordinatorState::Completed:
	case EHyperAIStudioPlanCoordinatorState::Failed:
	case EHyperAIStudioPlanCoordinatorState::Partial:
	case EHyperAIStudioPlanCoordinatorState::RolledBack:
	case EHyperAIStudioPlanCoordinatorState::OutcomeUnknown:
	case EHyperAIStudioPlanCoordinatorState::ReplayCompleted:
		return true;
	default:
		return false;
	}
}

bool FHyperAIStudioPlanExecutionRuntime::IsOutcomeUnknown() const
{
	return GetEffectiveState() == EHyperAIStudioPlanCoordinatorState::OutcomeUnknown;
}

EHyperAIStudioPlanCoordinatorState FHyperAIStudioPlanExecutionRuntime::GetEffectiveState() const
{
	return bAmbiguousOutcome
		? EHyperAIStudioPlanCoordinatorState::OutcomeUnknown
		: Coordinator.GetState();
}

TArray<FString> FHyperAIStudioPlanExecutionRuntime::GetSupportedOperationTypes()
{
	return {
		HyperAIStudio::PlanExecute::Private::FoundationInspect,
		HyperAIStudio::PlanExecute::Private::BlueprintApply,
		HyperAIStudio::PlanExecute::Private::BlueprintDelete};
}

bool FHyperAIStudioPlanExecutionRuntime::IsOperationTypeSupported(const FString& OperationType)
{
	return GetSupportedOperationTypes().Contains(OperationType);
}

bool FHyperAIStudioPlanExecutionRuntime::ValidateExecutablePlanShape(
	const FHyperAIStudioValidatedPlan& Plan,
	FString& OutError)
{
	if (!Plan.bValidated || Plan.bDryRun
		|| !FHyperAIStudioOperationJournal::IsValidOperationId(Plan.OperationId))
	{
		OutError = TEXT("Execution requires a validated execute-mode plan with a bounded client operation_id.");
		return false;
	}
	int32 BlueprintMutationSteps = 0;
	for (const FHyperAIStudioPlanStep& Step : Plan.Steps)
	{
		if (!IsOperationTypeSupported(Step.OperationType))
		{
			OutError = TEXT("An operation metadata fixture has no production backend and is unavailable.");
			return false;
		}
		if (Step.OperationType == HyperAIStudio::PlanExecute::Private::FoundationInspect)
		{
			HyperAIStudio::PlanExecute::Private::FEpicDelegateRoute Route;
			if (!HyperAIStudio::PlanExecute::Private::ResolveEpicInspectRoute(Step, Route, OutError))
			{
				return false;
			}
		}
		if (Step.OperationType == HyperAIStudio::PlanExecute::Private::BlueprintApply
			|| Step.OperationType == HyperAIStudio::PlanExecute::Private::BlueprintDelete)
		{
			if (!HyperAIStudio::PlanExecute::Private::ValidateBlueprintStepContract(Step, OutError))
			{
				return false;
			}
			++BlueprintMutationSteps;
		}
	}
	if (BlueprintMutationSteps > 1)
	{
		OutError = TEXT("Exactly one Blueprint patch step is currently admitted per plan to prove one compile/save/verify lifecycle.");
		return false;
	}
	FHyperAIStudioPlanDryRunResult DryRun;
	if (!FHyperAIStudioTypedPlanValidator::BuildDryRun(Plan, DryRun, OutError))
	{
		return false;
	}
	if (BlueprintMutationSteps == 1)
	{
		int32 Compile = 0;
		int32 Validate = 0;
		int32 Save = 0;
		int32 Fresh = 0;
		for (const FHyperAIStudioPlanScheduledAction& Action : DryRun.Schedule)
		{
			Compile += Action.Kind == EHyperAIStudioPlanActionKind::CompileOnce ? 1 : 0;
			Validate += Action.Kind == EHyperAIStudioPlanActionKind::ValidateOnce ? 1 : 0;
			Save += Action.Kind == EHyperAIStudioPlanActionKind::SaveOnce ? 1 : 0;
			Fresh += Action.Kind == EHyperAIStudioPlanActionKind::VerifyFreshOnce ? 1 : 0;
		}
		if (Compile != 1 || Validate != 1 || Save != 1 || Fresh != 1)
		{
			OutError = TEXT("The Blueprint plan did not derive exactly one compile, validate, save and fresh-verification action.");
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioPlanExecutionRuntime::StateToStatus(
	const EHyperAIStudioPlanCoordinatorState State)
{
	switch (State)
	{
	case EHyperAIStudioPlanCoordinatorState::Idle: return TEXT("idle");
	case EHyperAIStudioPlanCoordinatorState::Ready: return TEXT("accepted");
	case EHyperAIStudioPlanCoordinatorState::AwaitingActionResult: return TEXT("running");
	case EHyperAIStudioPlanCoordinatorState::Completed: return TEXT("completed");
	case EHyperAIStudioPlanCoordinatorState::Failed: return TEXT("failed");
	case EHyperAIStudioPlanCoordinatorState::Partial: return TEXT("partial");
	case EHyperAIStudioPlanCoordinatorState::RolledBack: return TEXT("rolled_back");
	case EHyperAIStudioPlanCoordinatorState::OutcomeUnknown: return TEXT("outcome_unknown");
	case EHyperAIStudioPlanCoordinatorState::ReplayCompleted: return TEXT("replay_completed");
	default: return TEXT("unknown");
	}
}

FHyperAIPlanExecuteResponse UHyperAIStudioPlanExecuteToolset::hyper_plan_execute(
	const FString& PlanJson)
{
	return FHyperAIStudioPlanExecutionService::Submit(PlanJson);
}

void FHyperAIStudioPlanExecutionService::Startup()
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	if (State.Active.IsValid() && State.Active->Runtime.HasOutstandingDispatch())
	{
		// An uncancellable engine future still owns code in this module. Keep the service
		// closed; SupportsAutomaticShutdown likewise keeps the DLL resident until exit.
		return;
	}
	State.Active.Reset();
	State.StagedPatches.Reset();
	State.Archived.Reset();
	State.ArchiveOrder.Reset();
	State.Security->Reset();
	State.bShuttingDown = false;
}

FHyperAIPlanExecuteResponse FHyperAIStudioPlanExecutionService::Submit(const FString& PlanJson)
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FHyperAIPlanExecuteResponse Failure;
	Failure.SupportedOperationTypes = FHyperAIStudioPlanExecutionRuntime::GetSupportedOperationTypes();
	if (!IsInGameThread())
	{
		Failure.Status = TEXT("game_thread_required");
		Failure.Diagnostic = TEXT("Plan acceptance must begin on the Unreal game thread.");
		return Failure;
	}
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ProjectRoot);
	if (!IsSha256(CanonicalProjectId))
	{
		Failure.Status = TEXT("project_identity_unavailable");
		Failure.Diagnostic = TEXT("A strong canonical project identity is unavailable.");
		return Failure;
	}

	FHyperAIStudioTypedOperationRegistry Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
	FHyperAIStudioValidatedPlan Plan;
	FHyperAIStudioPlanDryRunResult DryRun;
	if (!FHyperAIStudioTypedPlanValidator::ValidateJson(PlanJson, Registry, Plan, DryRun))
	{
		Failure.Status = DryRun.Diagnostics.IsEmpty() ? TEXT("invalid_plan") : DryRun.Diagnostics[0].Code;
		Failure.Diagnostic = DryRun.Diagnostics.IsEmpty()
			? TEXT("The strict typed plan was rejected.")
			: DryRun.Diagnostics[0].Message;
		return Failure;
	}
	FString ShapeError;
	if (!FHyperAIStudioPlanExecutionRuntime::ValidateExecutablePlanShape(Plan, ShapeError))
	{
		Failure.Status = TEXT("backend_unavailable");
		Failure.Diagnostic = ShapeError;
		Failure.OperationId = Plan.OperationId;
		Failure.ProjectIdentityHash = CanonicalProjectId;
		Failure.PlanHash = Plan.PlanHash;
		Failure.CapabilityHash = Plan.CapabilityHash;
		Failure.EffectFingerprint = Plan.EffectFingerprint;
		return Failure;
	}

	FServiceState& State = GetServiceState();
	TArray<FHyperAIStudioStagedBlueprintPatchArtifact> Artifacts;
	{
		FScopeLock Lock(&State.Mutex);
		if (State.bShuttingDown)
		{
			Failure.Status = TEXT("service_shutting_down");
			Failure.Diagnostic = TEXT("The serial execution service is quiescing and accepts no new work.");
			return Failure;
		}
		if (State.Active.IsValid())
		{
			const FHyperAIStudioPlanExecutionStatus Existing = State.Active->Runtime.GetStatus();
			if (Existing.OperationId == Plan.OperationId)
			{
				if (!ExactStatusBinding(Existing, Plan, CanonicalProjectId))
				{
					Failure.Status = TEXT("operation_id_conflict");
					Failure.Diagnostic = TEXT("operation_id is already bound to a different exact execution artifact.");
					return Failure;
				}
				FHyperAIPlanExecuteResponse Response = ToResponse(Existing, true);
				Response.Status = Existing.bOutcomeUnknown ? TEXT("outcome_unknown") : TEXT("already_in_progress");
				Response.bAccepted = !Existing.bOutcomeUnknown;
				return Response;
			}
			Failure.Status = TEXT("project_execution_busy");
			Failure.Diagnostic = TEXT("Another project plan owns the one-at-a-time execution lane.");
			return Failure;
		}
		if (const FHyperAIStudioPlanExecutionStatus* Archived = State.Archived.Find(Plan.OperationId))
		{
			if (!ExactStatusBinding(*Archived, Plan, CanonicalProjectId))
			{
				Failure.Status = TEXT("operation_id_conflict");
				Failure.Diagnostic = TEXT("operation_id is archived with a different exact execution artifact.");
				return Failure;
			}
			FHyperAIPlanExecuteResponse Response = ToResponse(*Archived, true);
			Response.bReplay = true;
			Response.bAccepted = false;
			return Response;
		}
		for (const FHyperAIStudioPlanStep& Step : Plan.Steps)
		{
			if (Step.OperationType != BlueprintApply && Step.OperationType != BlueprintDelete)
			{
				continue;
			}
			FString PatchId;
			if (GetStringArgument(Step, TEXT("patch_id"), PatchId))
			{
				if (const FHyperAIStudioStagedBlueprintPatchArtifact* Artifact = State.StagedPatches.Find(PatchId))
				{
					Artifacts.Add(*Artifact);
				}
			}
		}
	}

	TSharedRef<FExecutionSession> Session = MakeShared<FExecutionSession>();
	Session->Registry = MoveTemp(Registry);
	if (Plan.bHasMutation)
	{
		Session->Journal = MakeUnique<FHyperAIStudioOperationJournal>(ProjectRoot);
		FString LoadError;
		if (!Session->Journal->Load(LoadError))
		{
			Failure.Status = LoadError.Contains(TEXT("Another HyperAIStudio"))
				? TEXT("project_execution_busy") : TEXT("journal_unavailable");
			Failure.Diagnostic = LoadError;
			return Failure;
		}
		if (Session->Journal->GetCanonicalProjectId() != CanonicalProjectId)
		{
			Failure.Status = TEXT("project_identity_changed");
			Failure.Diagnostic = TEXT("The journal and accepted plan resolved different physical project identities.");
			return Failure;
		}
		Session->JournalAdapter = MakeUnique<FHyperAIStudioOperationJournalPlanAdapter>(*Session->Journal);
		if (Plan.bHasDestructive || Plan.bHasExternalEffect)
		{
			const TOptional<FHyperAIStudioOperationRecord> Existing =
				Session->Journal->Find(Plan.OperationId);
			if (Existing.IsSet() && Existing->State == EHyperAIStudioOperationState::Completed)
			{
				FString ReplayAuthorizationError;
				if (!State.Security->AdmitCompletedReplay(
					Plan, CanonicalProjectId, Existing.GetValue(), ReplayAuthorizationError))
				{
					Failure.Status = TEXT("replay_evidence_invalid");
					Failure.Diagnostic = ReplayAuthorizationError;
					return Failure;
				}
			}
		}
	}
	Session->Dispatcher = MakeUnique<FProductionDispatcher>(State.Security, Artifacts);
	FString StartError;
	const bool bStarted = Session->Runtime.Start(
		Plan,
		Session->Registry,
		CanonicalProjectId,
		Session->JournalAdapter.Get(),
		(Plan.bHasDestructive || Plan.bHasExternalEffect) ? &State.Security.Get() : nullptr,
		Plan.bHasMutation ? &State.Security.Get() : nullptr,
		&Session->Clock,
		Session->Dispatcher.Get(),
		StartError);
	const FHyperAIStudioPlanExecutionStatus InitialStatus = Session->Runtime.GetStatus();
	if (!bStarted && InitialStatus.CoordinatorState != EHyperAIStudioPlanCoordinatorState::ReplayCompleted)
	{
		Failure = ToResponse(InitialStatus, false);
		Failure.Status = InitialStatus.bOutcomeUnknown ? TEXT("outcome_unknown") : TEXT("execution_rejected");
		Failure.Diagnostic = StartError;
		return Failure;
	}
	if (InitialStatus.bReplay)
	{
		FHyperAIPlanExecuteResponse Response = ToResponse(InitialStatus, true);
		Response.bAccepted = false;
		Response.bReplay = true;
		return Response;
	}
	{
		FScopeLock Lock(&State.Mutex);
		if (State.Active.IsValid())
		{
			Failure.Status = TEXT("project_execution_race");
			Failure.Diagnostic = TEXT("The project execution lane changed during bounded acceptance.");
			return Failure;
		}
		State.Active = Session;
		for (const FHyperAIStudioStagedBlueprintPatchArtifact& Artifact : Artifacts)
		{
			State.StagedPatches.Remove(Artifact.PatchId);
		}
	}
	ScheduleSession(Session);
	FHyperAIPlanExecuteResponse Response = ToResponse(InitialStatus, true);
	Response.bAccepted = true;
	Response.Status = TEXT("accepted");
	Response.Diagnostic = TEXT("The exact plan was accepted; serial execution starts after this bounded response returns.");
	return Response;
}

bool FHyperAIStudioPlanExecutionService::QueryStatus(
	const FString& OperationId,
	FHyperAIStudioPlanExecutionStatus& OutStatus)
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	if (State.Active.IsValid() && State.Active->Runtime.GetStatus().OperationId == OperationId)
	{
		OutStatus = State.Active->Runtime.GetStatus();
		return true;
	}
	if (const FHyperAIStudioPlanExecutionStatus* Archived = State.Archived.Find(OperationId))
	{
		OutStatus = *Archived;
		return true;
	}
	return false;
}

bool FHyperAIStudioPlanExecutionService::Cancel(
	const FString& OperationId,
	const FString& Reason,
	FString& OutError)
{
	using namespace HyperAIStudio::PlanExecute::Private;
	if (!IsInGameThread())
	{
		OutError = TEXT("Cancellation must be serialized on the Unreal game thread.");
		return false;
	}
	FServiceState& State = GetServiceState();
	TSharedPtr<FExecutionSession> Session;
	{
		FScopeLock Lock(&State.Mutex);
		if (State.bShuttingDown)
		{
			OutError = TEXT("The serial execution service is quiescing.");
			return false;
		}
		Session = State.Active;
	}
	if (!Session.IsValid() || Session->Runtime.GetStatus().OperationId != OperationId)
	{
		OutError = TEXT("The operation_id is not the active project execution.");
		return false;
	}
	return Session->Runtime.RequestCancel(Reason, OutError);
}

bool FHyperAIStudioPlanExecutionService::StageBlueprintPatch(
	const FHyperAIStudioStagedBlueprintPatchArtifact& Artifact,
	FString& OutError)
{
	using namespace HyperAIStudio::PlanExecute::Private;
	OutError.Reset();
	const int64 CurrentUtcMs = NowUtcMs();
	if (!IsSha256(Artifact.CanonicalProjectId)
		|| !FHyperAIStudioOperationJournal::IsValidOperationId(Artifact.OperationId)
		|| !IsSha256(Artifact.PlanHash) || !IsSha256(Artifact.CapabilityHash)
		|| !IsSha256(Artifact.EffectFingerprint)
		|| Artifact.PatchId.IsEmpty() || Artifact.PatchId.Len() > 128
		|| Artifact.AssetPath.IsEmpty() || Artifact.AssetPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters
		|| Artifact.Patch.TargetAssetPath != Artifact.AssetPath
		|| !IsSha256(Artifact.Patch.ExpectedRevision)
		|| !IsBoundedStagedPatch(Artifact.Patch)
		|| Artifact.ExpiresUtcMs <= CurrentUtcMs
		|| Artifact.ExpiresUtcMs - CurrentUtcMs > MaxArtifactLifetimeMs)
	{
		OutError = TEXT("The staged Blueprint patch artifact is incomplete, unbounded, expired, or lacks exact execution bindings.");
		return false;
	}
	const FString PatchFingerprint = ComputeStagedPatchFingerprint(Artifact.Patch);
	if (!IsSha256(PatchFingerprint))
	{
		OutError = TEXT("The bounded staged Blueprint patch could not produce its exact content fingerprint.");
		return false;
	}
	FServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	if (State.bShuttingDown)
	{
		OutError = TEXT("The serial execution service is quiescing and accepts no staged artifacts.");
		return false;
	}
	for (auto It = State.StagedPatches.CreateIterator(); It; ++It)
	{
		if (It.Value().ExpiresUtcMs <= CurrentUtcMs)
		{
			It.RemoveCurrent();
		}
	}
	if (const FHyperAIStudioStagedBlueprintPatchArtifact* Existing =
		State.StagedPatches.Find(Artifact.PatchId))
	{
		if (SameStagedArtifact(*Existing, Artifact))
		{
			return true;
		}
		OutError = TEXT("patch_id is already bound to a different exact staged artifact.");
		return false;
	}
	if (State.StagedPatches.Num() >= MaxStagedPatches)
	{
		OutError = TEXT("The bounded patch staging table is full.");
		return false;
	}
	State.StagedPatches.Add(Artifact.PatchId, Artifact);
	return true;
}

bool FHyperAIStudioPlanExecutionService::IssueAuthorization(
	const FHyperAIStudioValidatedPlan& Plan,
	const FString& CanonicalProjectId,
	FString& OutOpaqueToken,
	FString& OutError)
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	if (State.bShuttingDown)
	{
		OutError = TEXT("The serial execution service is quiescing and issues no grants.");
		return false;
	}
	return State.Security->IssueAuthorization(
		Plan, CanonicalProjectId, OutOpaqueToken, OutError);
}

bool FHyperAIStudioPlanExecutionService::CanShutdownSafely()
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	return !State.Active.IsValid() || !State.Active->Runtime.HasOutstandingDispatch();
}

void FHyperAIStudioPlanExecutionService::Shutdown()
{
	using namespace HyperAIStudio::PlanExecute::Private;
	FServiceState& State = GetServiceState();
	TSharedPtr<FExecutionSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	{
		FScopeLock Lock(&State.Mutex);
		State.bShuttingDown = true;
		Session = State.Active;
		if (Session.IsValid())
		{
			TickerHandle = Session->TickerHandle;
			Session->TickerHandle.Reset();
		}
	}
	if (TickerHandle.IsValid() && IsInGameThread())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	if (Session.IsValid() && !Session->Runtime.IsTerminal() && IsInGameThread())
	{
		FString Ignore;
		Session->Runtime.RequestCancel(TEXT("HyperAIStudio module shutdown"), Ignore);
	}
	{
		FScopeLock Lock(&State.Mutex);
		State.StagedPatches.Reset();
		State.Archived.Reset();
		State.ArchiveOrder.Reset();
		if (!Session.IsValid() || !Session->Runtime.HasOutstandingDispatch())
		{
			State.Active.Reset();
			State.Security->Reset();
		}
	}
}

FString FHyperAIStudioPlanExecuteContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioPlanExecuteToolset");
}

bool FHyperAIStudioPlanExecuteContracts::IsPendingNativeToolsTestEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioPlanExecuteContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	TArray<FString> Errors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(Errors))
	{
		return false;
	}
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	const FHyperAIStudioCapabilityToolDefinition* Match = nullptr;
	int32 MatchCount = 0;
	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		if (Tool.Name == TEXT("hyper_plan_execute"))
		{
			Match = &Tool;
			++MatchCount;
		}
	}
	if (!Match || MatchCount != 1 || Match->PackId != TEXT("shared_foundation"))
	{
		return false;
	}
	if (Match->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted)
	{
		// Admitted can originate only from the checked-in generated catalog/evidence pipeline.
		return true;
	}
	return bAllowSourceCandidateForDev
		&& Match->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
}

FString FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(const FString& Value)
{
	return HyperAIStudio::PlanExecute::Private::HashUtf8(Value);
}

FString FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256Bytes(const TArray<uint8>& Bytes)
{
	if (Bytes.Num() > MaxHashInputBytes)
	{
		return FString();
	}
	return HyperAIStudio::PlanExecute::Private::HashBytes(
		Bytes.GetData(), Bytes.Num(), MaxHashInputBytes);
}

EHyperAIStudioPlanActionOutcome FHyperAIStudioPlanExecuteContracts::ClassifySaveAttempt(
	const bool bSaveReturnedSuccess,
	const bool bPackageIsDirty)
{
	return bSaveReturnedSuccess && !bPackageIsDirty
		? EHyperAIStudioPlanActionOutcome::Succeeded
		: EHyperAIStudioPlanActionOutcome::OutcomeUnknown;
}

FString FHyperAIStudioPlanExecuteContracts::ClipText(
	const FString& Value,
	const int32 MaxCharacters)
{
	if (MaxCharacters <= 0)
	{
		return FString();
	}
	return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
}

void FHyperAIStudioPlanExecuteToolRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	FHyperAIStudioPlanExecutionService::Startup();
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPlanExecuteToolRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPlanExecuteToolRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	FHyperAIStudioPlanExecutionService::Shutdown();
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPlanExecuteToolset::StaticClass(),
			FHyperAIStudioPlanExecuteContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioPlanExecute, Warning,
				TEXT("Could not unregister the owned plan-execute toolset: %s"), *Error);
		}
	}
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioPlanExecuteToolRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioPlanExecuteContracts::IsRegistrationAllowed(
			FHyperAIStudioPlanExecuteContracts::IsPendingNativeToolsTestEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPlanExecuteToolset::StaticClass(),
			FHyperAIStudioPlanExecuteContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioPlanExecuteToolRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioPlanExecuteContracts::IsRegistrationAllowed(
			FHyperAIStudioPlanExecuteContracts::IsPendingNativeToolsTestEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioPlanExecuteToolset::StaticClass(),
		FHyperAIStudioPlanExecuteContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioPlanExecuteToolset::StaticClass(),
			FHyperAIStudioPlanExecuteContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioPlanExecute, Error,
				TEXT("ToolsetRegistry rejected the owned hyper_plan_execute SourceCandidate: %s"),
				*Error);
		}
	}
}
