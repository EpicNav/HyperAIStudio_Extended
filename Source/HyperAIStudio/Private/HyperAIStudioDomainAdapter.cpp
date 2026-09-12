// Games by Hyper 2026.

#include "HyperAIStudioDomainAdapter.h"

#include "HyperAIStudioExtensionRuntime.h"

#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::DomainAdapter::Private
{
	struct FRegisteredAdapter
	{
		FHyperAIStudioDomainAdapterDescriptor Descriptor;
		TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
		uint64 Generation = 0;
		int32 ActiveLeases = 0;
		bool bOutcomeUnknown = false;
		FString UnknownOperationId;
		FString UnknownPlanHash;
	};

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

	bool AppendToken(FString& Buffer, const FString& Value)
	{
		if (!HasWellFormedUtf16(Value))
		{
			return false;
		}
		const FTCHARToUTF8 Utf8(*Value);
		Buffer.Appendf(TEXT("%d:"), Utf8.Length());
		Buffer += Value;
		Buffer += TEXT("|");
		return true;
	}

	bool AppendUInt(FString& Buffer, const uint64 Value)
	{
		return AppendToken(Buffer, FString::Printf(TEXT("%llu"), Value));
	}

	bool AppendBool(FString& Buffer, const bool bValue)
	{
		return AppendToken(Buffer, bValue ? TEXT("1") : TEXT("0"));
	}

	bool AppendInt64(FString& Buffer, const int64 Value)
	{
		return AppendToken(Buffer, FString::Printf(TEXT("%lld"), Value));
	}

	FString HashUtf8Sha256(const FString& Value)
	{
		if (!HasWellFormedUtf16(Value))
		{
			return FString();
		}
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

		FString Hex = TEXT("sha256:");
		Hex.Reserve(71);
		for (const uint32 Word : State)
		{
			Hex += FString::Printf(TEXT("%08x"), Word);
		}
		return Hex;
	}

	bool IsSha256Fingerprint(const FString& Value)
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

	bool IsSafeId(const FString& Value, const int32 MaxChars, const bool bRequireHyperTool = false)
	{
		if (Value.IsEmpty() || Value.Len() > MaxChars
			|| (bRequireHyperTool && !Value.StartsWith(TEXT("hyper_"), ESearchCase::CaseSensitive)))
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(':');
			if (!bAllowed)
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeTypedObjectId(const FString& Value, const TCHAR* RequiredPrefix)
	{
		if (!IsSafeId(Value, FHyperAIStudioDomainLimits::MaxTypeIdChars)
			|| RequiredPrefix == nullptr
			|| !Value.StartsWith(RequiredPrefix, ESearchCase::CaseSensitive))
		{
			return false;
		}

		const FString Lower = Value.ToLower();
		static constexpr const TCHAR* ProhibitedFragments[] = {
			TEXT(".raw_script"), TEXT(".script_dispatch"), TEXT(".shell"),
			TEXT(".filesystem_dispatch"), TEXT(".tool_dispatch"), TEXT(".python"),
			TEXT(".lua"), TEXT(".console_command")};
		for (const TCHAR* Fragment : ProhibitedFragments)
		{
			if (Lower.Contains(Fragment, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafePayloadTypeId(const FString& Value)
	{
		return IsSafeTypedObjectId(Value, TEXT("hyperai.payload."));
	}

	bool IsSafeResultTypeId(const FString& Value)
	{
		return IsSafeTypedObjectId(Value, TEXT("hyperai.result."));
	}

	bool IsSafeStatusCode(const FString& Value)
	{
		return Value.IsEmpty() || IsSafeId(Value, FHyperAIStudioDomainLimits::MaxStatusCodeChars);
	}

	bool IsSafetyAdmitted(
		const FHyperAIStudioDomainAdmissionSnapshot& Admission,
		const EHyperAIStudioDomainSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIStudioDomainSafety::Read:
			return Admission.bReadAdmitted;
		case EHyperAIStudioDomainSafety::Edit:
			return Admission.bEditAdmitted;
		case EHyperAIStudioDomainSafety::Destructive:
			return Admission.bDestructiveAdmitted;
		case EHyperAIStudioDomainSafety::ExternalEffect:
			return Admission.bExternalEffectAdmitted;
		default:
			return false;
		}
	}

	bool SameTrustSeal(
		const FHyperAIStudioDomainTrustSeal& A,
		const FHyperAIStudioDomainTrustSeal& B)
	{
		return A.SchemaVersion == B.SchemaVersion
			&& A.CanonicalProjectId == B.CanonicalProjectId
			&& A.CatalogFingerprint == B.CatalogFingerprint
			&& A.ApprovedPlanFingerprint == B.ApprovedPlanFingerprint
			&& A.AdmissionMatrixFingerprint == B.AdmissionMatrixFingerprint
			&& A.SourceLedgerFingerprint == B.SourceLedgerFingerprint
			&& A.SourceArtifactFingerprint == B.SourceArtifactFingerprint
			&& A.AtomicCohortId == B.AtomicCohortId
			&& A.CohortSourceArtifactFingerprint == B.CohortSourceArtifactFingerprint
			&& A.CatalogGeneration == B.CatalogGeneration
			&& A.AdapterRegistryGeneration == B.AdapterRegistryGeneration
			&& A.ObservedMonotonicMs == B.ObservedMonotonicMs
			&& A.ExpiresMonotonicMs == B.ExpiresMonotonicMs
			&& A.bPreAdmissionEvidence == B.bPreAdmissionEvidence;
	}

	bool AppendTrustSeal(FString& Canonical, const FHyperAIStudioDomainTrustSeal& Seal)
	{
		return AppendUInt(Canonical, Seal.SchemaVersion)
			&& AppendToken(Canonical, Seal.CanonicalProjectId)
			&& AppendToken(Canonical, Seal.CatalogFingerprint)
			&& AppendToken(Canonical, Seal.ApprovedPlanFingerprint)
			&& AppendToken(Canonical, Seal.AdmissionMatrixFingerprint)
			&& AppendToken(Canonical, Seal.SourceLedgerFingerprint)
			&& AppendToken(Canonical, Seal.SourceArtifactFingerprint)
			&& AppendToken(Canonical, Seal.AtomicCohortId)
			&& AppendToken(Canonical, Seal.CohortSourceArtifactFingerprint)
			&& AppendUInt(Canonical, Seal.CatalogGeneration)
			&& AppendUInt(Canonical, Seal.AdapterRegistryGeneration)
			&& AppendInt64(Canonical, Seal.ObservedMonotonicMs)
			&& AppendInt64(Canonical, Seal.ExpiresMonotonicMs)
			&& AppendBool(Canonical, Seal.bPreAdmissionEvidence);
	}

		bool ValidateTrustSeal(
			const FHyperAIStudioDomainTrustSeal& Seal,
			const FHyperAIStudioDomainBinding& Binding)
		{
			// The trusted host alone authors this seal after checking the current Preview/Stable policy.
			return Seal.SchemaVersion == 2
			&& Seal.CanonicalProjectId == Binding.CanonicalProjectId
			&& IsSha256Fingerprint(Seal.CatalogFingerprint)
			&& IsSha256Fingerprint(Seal.ApprovedPlanFingerprint)
			&& IsSha256Fingerprint(Seal.AdmissionMatrixFingerprint)
			&& IsSha256Fingerprint(Seal.SourceLedgerFingerprint)
			&& IsSha256Fingerprint(Seal.SourceArtifactFingerprint)
			&& IsSafeId(Seal.AtomicCohortId, FHyperAIStudioDomainLimits::MaxTypeIdChars)
			&& IsSha256Fingerprint(Seal.CohortSourceArtifactFingerprint)
			&& Seal.CatalogGeneration != 0
			&& Seal.AdapterRegistryGeneration != 0
			&& Seal.AdapterRegistryGeneration == Binding.ExpectedRegistryEpoch
			&& Seal.ObservedMonotonicMs >= 0
			&& Seal.ExpiresMonotonicMs > Seal.ObservedMonotonicMs;
	}

	const FHyperAIStudioDomainVariantDescriptor* FindVariant(
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		const FString& ToolName,
		const FString& VariantId)
	{
		return Descriptor.Variants.FindByPredicate(
			[&ToolName, &VariantId](const FHyperAIStudioDomainVariantDescriptor& Variant)
			{
				return Variant.ToolName == ToolName && Variant.VariantId == VariantId;
			});
	}

	bool SameAuthorizationRequest(
		const FHyperAIStudioDomainAuthorizationRequest& A,
		const FHyperAIStudioDomainAuthorizationRequest& B)
	{
		return A.OpaqueToken == B.OpaqueToken
			&& A.CanonicalProjectId == B.CanonicalProjectId
			&& A.OperationId == B.OperationId
			&& A.PlanHash == B.PlanHash
			&& A.PackId == B.PackId
			&& A.ToolName == B.ToolName
			&& A.VariantId == B.VariantId
			&& A.AdapterFingerprint == B.AdapterFingerprint
			&& A.AdmissionFingerprint == B.AdmissionFingerprint
			&& A.PrerequisiteFingerprint == B.PrerequisiteFingerprint
			&& A.Safety == B.Safety
			&& A.AdapterGeneration == B.AdapterGeneration
			&& A.RegistryEpoch == B.RegistryEpoch;
	}

	int64 CurrentUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}

	bool ValidateDescriptor(
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor,
		FString& OutError)
	{
		OutError.Reset();
		if (!IsSafeId(Descriptor.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
			|| !IsSafeId(Descriptor.AdapterId, FHyperAIStudioDomainLimits::MaxAdapterIdChars)
			|| !IsSafeId(Descriptor.SemanticVersion, FHyperAIStudioDomainLimits::MaxVersionChars)
			|| Descriptor.AdapterVersion == 0 || Descriptor.Variants.IsEmpty()
			|| Descriptor.Variants.Num() > 128)
		{
			OutError = TEXT("adapter_descriptor_invalid");
			return false;
		}

		TSet<FString> RequirementGroupIds;
		for (const FString& GroupId : Descriptor.ApplicableNonBlockingRequirementGroupIds)
		{
			if (!IsSafeId(GroupId, FHyperAIStudioDomainLimits::MaxTypeIdChars)
				|| RequirementGroupIds.Contains(GroupId))
			{
				OutError = TEXT("adapter_requirement_group_invalid");
				return false;
			}
			RequirementGroupIds.Add(GroupId);
		}

		TSet<FString> ExactBindings;
		for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
		{
			if (!IsSafeId(Variant.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars, true)
				|| !IsSafeId(Variant.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
				|| !IsSafePayloadTypeId(Variant.RequestTypeId)
				|| !IsSha256Fingerprint(Variant.RequestSchemaFingerprint)
				|| !IsSafeResultTypeId(Variant.ResultTypeId)
				|| !IsSha256Fingerprint(Variant.ResultSchemaFingerprint)
				|| static_cast<uint8>(Variant.Safety)
					> static_cast<uint8>(EHyperAIStudioDomainSafety::ExternalEffect))
			{
				OutError = TEXT("adapter_variant_invalid");
				return false;
			}

			const FString Key = Variant.ToolName + TEXT("\n") + Variant.VariantId;
			if (ExactBindings.Contains(Key))
			{
				OutError = TEXT("adapter_variant_collision");
				return false;
			}
			ExactBindings.Add(Key);
		}

		const FString Contract = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
		const FString Fingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
		if (!IsSha256Fingerprint(Contract) || Descriptor.ContractFingerprint != Contract)
		{
			OutError = TEXT("adapter_contract_fingerprint_mismatch");
			return false;
		}
		if (!IsSha256Fingerprint(Fingerprint) || Descriptor.AdapterFingerprint != Fingerprint)
		{
			OutError = TEXT("adapter_fingerprint_mismatch");
			return false;
		}
		if (!Descriptor.ReplacesAdapterFingerprint.IsEmpty()
			&& !IsSha256Fingerprint(Descriptor.ReplacesAdapterFingerprint))
		{
			OutError = TEXT("adapter_replacement_fingerprint_invalid");
			return false;
		}
		return true;
	}

	bool ValidateBindingSnapshots(
		const FHyperAIStudioDomainBinding& Binding,
		FHyperAIStudioDomainResolveResult& OutResult)
	{
		OutResult = FHyperAIStudioDomainResolveResult{};
		OutResult.AdapterFingerprint = Binding.ExpectedAdapterFingerprint;
		if (!IsSafeId(Binding.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
			|| !IsSafeId(Binding.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars, true)
			|| !IsSafeId(Binding.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
			|| !IsSafeId(Binding.CanonicalProjectId, FHyperAIStudioDomainLimits::MaxCanonicalProjectIdChars)
			|| !IsSha256Fingerprint(Binding.ExpectedAdapterFingerprint))
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("binding_invalid");
			return false;
		}

		const bool bPrerequisiteV2 = Binding.Prerequisites.TrustSeal.SchemaVersion != 0;
		const bool bAdmissionV2 = Binding.Admission.TrustSeal.SchemaVersion != 0;
		if (Binding.Prerequisites.PackId != Binding.PackId
			|| Binding.Admission.PackId != Binding.PackId
			|| Binding.Prerequisites.Observations.IsEmpty()
			|| Binding.Prerequisites.Observations.Num() > FHyperAIStudioDomainLimits::MaxPrerequisites
			|| Binding.Prerequisites.Fingerprint
				!= FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites)
			|| Binding.Admission.Fingerprint
				!= FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission))
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("trusted_snapshot_invalid");
			return false;
		}
		if (bPrerequisiteV2 != bAdmissionV2
			|| (bPrerequisiteV2
				&& (!SameTrustSeal(Binding.Prerequisites.TrustSeal, Binding.Admission.TrustSeal)
					|| !ValidateTrustSeal(Binding.Prerequisites.TrustSeal, Binding)
					|| Binding.Prerequisites.Revision
						!= Binding.Prerequisites.TrustSeal.CatalogGeneration
					|| Binding.Admission.Revision
						!= Binding.Admission.TrustSeal.CatalogGeneration)))
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("trusted_authority_seal_invalid");
			return false;
		}

		TSet<FString> PrerequisiteIds;
		bool bDisabled = false;
		bool bMissing = false;
		bool bRestartRequired = false;
		for (const FHyperAIStudioDomainPrerequisiteObservation& Observation : Binding.Prerequisites.Observations)
		{
			if (!IsSafeId(Observation.Id, FHyperAIStudioDomainLimits::MaxTypeIdChars)
				|| PrerequisiteIds.Contains(Observation.Id)
				|| static_cast<uint8>(Observation.State)
					> static_cast<uint8>(EHyperAIStudioDomainPrerequisiteState::RestartRequired))
			{
				OutResult.State = EHyperAIStudioDomainState::Disabled;
				OutResult.DiagnosticCode = TEXT("trusted_snapshot_invalid");
				return false;
			}
			PrerequisiteIds.Add(Observation.Id);
			bDisabled |= Observation.State == EHyperAIStudioDomainPrerequisiteState::Disabled;
			bMissing |= Observation.State == EHyperAIStudioDomainPrerequisiteState::Missing;
			bRestartRequired |= Observation.State == EHyperAIStudioDomainPrerequisiteState::RestartRequired;
		}

		if (!Binding.Prerequisites.bPackEnabled)
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("pack_disabled");
			return false;
		}
		if (bRestartRequired)
		{
			OutResult.State = EHyperAIStudioDomainState::RestartRequired;
			OutResult.DiagnosticCode = TEXT("prerequisite_restart_required");
			return false;
		}
		if (bDisabled)
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("prerequisite_disabled");
			return false;
		}
		if (bMissing)
		{
			OutResult.State = EHyperAIStudioDomainState::MissingPrerequisite;
			OutResult.DiagnosticCode = TEXT("prerequisite_missing");
			return false;
		}
		if (!Binding.Admission.bPackAdmitted
			|| !IsSafetyAdmitted(Binding.Admission, Binding.ExpectedSafety))
		{
			OutResult.State = EHyperAIStudioDomainState::Disabled;
			OutResult.DiagnosticCode = TEXT("safety_not_admitted");
			return false;
		}
		if (bAdmissionV2)
		{
			const int32 AdmittedSafetyCount = static_cast<int32>(Binding.Admission.bReadAdmitted)
				+ static_cast<int32>(Binding.Admission.bEditAdmitted)
				+ static_cast<int32>(Binding.Admission.bDestructiveAdmitted)
				+ static_cast<int32>(Binding.Admission.bExternalEffectAdmitted);
			if (AdmittedSafetyCount != 1)
			{
				OutResult.State = EHyperAIStudioDomainState::Disabled;
				OutResult.DiagnosticCode = TEXT("safety_admission_not_exact");
				return false;
			}
		}
		return true;
	}
}

struct FHyperAIStudioDomainRegistryState
{
	static constexpr int32 MaxFastValidatedBindings = 256;

	mutable FCriticalSection Mutex;
	mutable FCriticalSection FastValidationMutex;
	TMap<FString, HyperAIStudio::DomainAdapter::Private::FRegisteredAdapter> Entries;
	TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> AuthorizationGate;
	TOptional<EHyperAIStudioNativeExecutionMode> ExecutionModeOverride;
	mutable TMap<uint32, TArray<FHyperAIStudioDomainBinding>> FastValidatedBindings;
	mutable int32 FastValidatedBindingCount = 0;
	uint64 Epoch = 1;
	uint64 NextGeneration = 1;
};

namespace HyperAIStudio::DomainAdapter::Private
{
	bool SamePrerequisiteSnapshot(
		const FHyperAIStudioDomainPrerequisiteSnapshot& A,
		const FHyperAIStudioDomainPrerequisiteSnapshot& B)
	{
		if (A.PackId != B.PackId || A.bPackEnabled != B.bPackEnabled
			|| A.Revision != B.Revision || A.Fingerprint != B.Fingerprint
			|| !SameTrustSeal(A.TrustSeal, B.TrustSeal)
			|| A.Observations.Num() != B.Observations.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Observations.Num(); ++Index)
		{
			if (A.Observations[Index].Id != B.Observations[Index].Id
				|| A.Observations[Index].State != B.Observations[Index].State)
			{
				return false;
			}
		}
		return true;
	}

	bool SameAdmissionSnapshot(
		const FHyperAIStudioDomainAdmissionSnapshot& A,
		const FHyperAIStudioDomainAdmissionSnapshot& B)
	{
		return A.PackId == B.PackId
			&& A.bPackAdmitted == B.bPackAdmitted
			&& A.bReadAdmitted == B.bReadAdmitted
			&& A.bEditAdmitted == B.bEditAdmitted
			&& A.bDestructiveAdmitted == B.bDestructiveAdmitted
			&& A.bExternalEffectAdmitted == B.bExternalEffectAdmitted
			&& A.Revision == B.Revision
			&& A.Fingerprint == B.Fingerprint
			&& SameTrustSeal(A.TrustSeal, B.TrustSeal);
	}

	bool SameBindingAuthority(
		const FHyperAIStudioDomainBinding& A,
		const FHyperAIStudioDomainBinding& B)
	{
		return A.PackId == B.PackId
			&& A.ToolName == B.ToolName
			&& A.VariantId == B.VariantId
			&& A.ExpectedSafety == B.ExpectedSafety
			&& A.CanonicalProjectId == B.CanonicalProjectId
			&& A.ExpectedAdapterFingerprint == B.ExpectedAdapterFingerprint
			&& A.ExpectedAdapterGeneration == B.ExpectedAdapterGeneration
			&& A.ExpectedRegistryEpoch == B.ExpectedRegistryEpoch
			&& SamePrerequisiteSnapshot(A.Prerequisites, B.Prerequisites)
			&& SameAdmissionSnapshot(A.Admission, B.Admission);
	}

	uint32 GetFastBindingCacheKey(const FHyperAIStudioDomainBinding& Binding)
	{
		uint32 Key = GetTypeHash(Binding.ExpectedAdapterFingerprint);
		Key = HashCombine(Key, GetTypeHash(Binding.ToolName));
		Key = HashCombine(Key, GetTypeHash(Binding.Prerequisites.Fingerprint));
		return HashCombine(Key, GetTypeHash(Binding.Admission.Fingerprint));
	}

	bool ValidateBindingSnapshotsWithPolicy(
		const FHyperAIStudioDomainRegistryState& State,
		const FHyperAIStudioDomainBinding& Binding,
		FHyperAIStudioDomainResolveResult& OutResult,
		const bool bStrictValidation)
	{
		if (!bStrictValidation)
		{
			FScopeLock CacheLock(&State.FastValidationMutex);
			const TArray<FHyperAIStudioDomainBinding>* Bucket =
				State.FastValidatedBindings.Find(GetFastBindingCacheKey(Binding));
			if (Bucket && Bucket->ContainsByPredicate(
					[&Binding](const FHyperAIStudioDomainBinding& Cached)
					{
						return SameBindingAuthority(Cached, Binding);
					}))
			{
				OutResult = FHyperAIStudioDomainResolveResult{};
				OutResult.AdapterFingerprint = Binding.ExpectedAdapterFingerprint;
				return true;
			}
		}

		// A cache miss always executes the original complete hash/seal validator. A changed field with
		// a reused fingerprint therefore cannot turn a miss into trusted authority.
		if (!ValidateBindingSnapshots(Binding, OutResult))
		{
			return false;
		}
		if (!bStrictValidation)
		{
			FScopeLock CacheLock(&State.FastValidationMutex);
			if (State.FastValidatedBindingCount
				>= FHyperAIStudioDomainRegistryState::MaxFastValidatedBindings)
			{
				State.FastValidatedBindings.Reset();
				State.FastValidatedBindingCount = 0;
			}
			TArray<FHyperAIStudioDomainBinding>& Bucket =
				State.FastValidatedBindings.FindOrAdd(GetFastBindingCacheKey(Binding));
			if (!Bucket.ContainsByPredicate(
					[&Binding](const FHyperAIStudioDomainBinding& Cached)
					{
						return SameBindingAuthority(Cached, Binding);
					}))
			{
				Bucket.Add(Binding);
				++State.FastValidatedBindingCount;
			}
		}
		return true;
	}

	/** Core owner for an aliasing result pointer; release the optional payload before its module pin. */
	class FResultPayloadLifetimeOwner final
	{
	public:
		FResultPayloadLifetimeOwner(
			FHyperAIStudioDomainAdapterLease&& InLease,
			const TSharedRef<const IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& InPayload)
			: Lease(MoveTemp(InLease)), Payload(InPayload)
		{
		}

		~FResultPayloadLifetimeOwner()
		{
			Payload.Reset();
			Lease.Reset();
		}

	private:
		FHyperAIStudioDomainAdapterLease Lease;
		TSharedPtr<const IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe> Payload;
	};

	FHyperAIStudioDomainResolveResult ResolveLocked(
		const FHyperAIStudioDomainRegistryState& State,
		const FHyperAIStudioDomainBinding& Binding,
		const bool bAllowActiveLease)
	{
		FHyperAIStudioDomainResolveResult Result;
		Result.RegistryEpoch = State.Epoch;
		Result.AdapterFingerprint = Binding.ExpectedAdapterFingerprint;

		const FRegisteredAdapter* Entry = State.Entries.Find(Binding.ExpectedAdapterFingerprint);
		if (!Entry)
		{
			if (Binding.ExpectedRegistryEpoch != 0 && Binding.ExpectedRegistryEpoch != State.Epoch)
			{
				Result.State = EHyperAIStudioDomainState::Disabled;
				Result.DiagnosticCode = TEXT("stale_registry_epoch");
				return Result;
			}
			Result.State = EHyperAIStudioDomainState::MissingPrerequisite;
			Result.DiagnosticCode = TEXT("adapter_explicit_registration_required");
			return Result;
		}

		Result.AdapterGeneration = Entry->Generation;
		Result.AdapterFingerprint = Entry->Descriptor.AdapterFingerprint;
		if (Entry->Descriptor.PackId != Binding.PackId
			|| Entry->Descriptor.AdapterFingerprint != Binding.ExpectedAdapterFingerprint)
		{
			Result.State = EHyperAIStudioDomainState::Disabled;
			Result.DiagnosticCode = TEXT("adapter_pack_binding_mismatch");
			return Result;
		}
		if (Binding.ExpectedAdapterGeneration != 0
			&& Binding.ExpectedAdapterGeneration != Entry->Generation)
		{
			Result.State = EHyperAIStudioDomainState::Disabled;
			Result.DiagnosticCode = TEXT("stale_adapter_generation");
			return Result;
		}

		const FHyperAIStudioDomainVariantDescriptor* Variant = FindVariant(
			Entry->Descriptor, Binding.ToolName, Binding.VariantId);
		if (!Variant || Variant->Safety != Binding.ExpectedSafety)
		{
			Result.State = EHyperAIStudioDomainState::Disabled;
			Result.DiagnosticCode = TEXT("exact_variant_binding_mismatch");
			return Result;
		}
		if (Entry->bOutcomeUnknown)
		{
			Result.State = EHyperAIStudioDomainState::OutcomeUnknown;
			Result.DiagnosticCode = TEXT("adapter_outcome_unknown");
			return Result;
		}
		if (Entry->ActiveLeases > 0 && !bAllowActiveLease)
		{
			Result.State = EHyperAIStudioDomainState::Busy;
			Result.DiagnosticCode = TEXT("adapter_busy");
			return Result;
		}

		Result.State = EHyperAIStudioDomainState::Ready;
		Result.DiagnosticCode = TEXT("adapter_ready");
		return Result;
	}
}

FHyperAIStudioDomainAdapterLease::FHyperAIStudioDomainAdapterLease() = default;

FHyperAIStudioDomainAdapterLease::FHyperAIStudioDomainAdapterLease(
	const TSharedRef<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe>& InState,
	const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& InAdapter,
	const FHyperAIStudioDomainAdapterDescriptor& InDescriptor,
	const FString& InPackId,
	const uint64 InAdapterGeneration,
	const uint64 InRegistryEpoch)
	: State(InState)
	, Adapter(InAdapter)
	, Descriptor(&InAdapter->GetDescriptor())
	, PackId(InPackId)
	, AdapterId(InDescriptor.AdapterId)
	, AdapterFingerprint(InDescriptor.AdapterFingerprint)
	, AdapterGeneration(InAdapterGeneration)
	, RegistryEpoch(InRegistryEpoch)
{
}

FHyperAIStudioDomainAdapterLease::~FHyperAIStudioDomainAdapterLease()
{
	Reset();
}

FHyperAIStudioDomainAdapterLease::FHyperAIStudioDomainAdapterLease(
	FHyperAIStudioDomainAdapterLease&& Other) noexcept
	: State(MoveTemp(Other.State))
	, Adapter(MoveTemp(Other.Adapter))
	, Descriptor(Other.Descriptor)
	, PackId(MoveTemp(Other.PackId))
	, AdapterId(MoveTemp(Other.AdapterId))
	, AdapterFingerprint(MoveTemp(Other.AdapterFingerprint))
	, AdapterGeneration(Other.AdapterGeneration)
	, RegistryEpoch(Other.RegistryEpoch)
{
	Other.AdapterGeneration = 0;
	Other.RegistryEpoch = 0;
	Other.Descriptor = nullptr;
}

FHyperAIStudioDomainAdapterLease& FHyperAIStudioDomainAdapterLease::operator=(
	FHyperAIStudioDomainAdapterLease&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		State = MoveTemp(Other.State);
		Adapter = MoveTemp(Other.Adapter);
		Descriptor = Other.Descriptor;
		PackId = MoveTemp(Other.PackId);
		AdapterId = MoveTemp(Other.AdapterId);
		AdapterFingerprint = MoveTemp(Other.AdapterFingerprint);
		AdapterGeneration = Other.AdapterGeneration;
		RegistryEpoch = Other.RegistryEpoch;
		Other.AdapterGeneration = 0;
		Other.RegistryEpoch = 0;
		Other.Descriptor = nullptr;
	}
	return *this;
}

void FHyperAIStudioDomainAdapterLease::Reset()
{
	if (State.IsValid() && Adapter.IsValid())
	{
		FScopeLock Lock(&State->Mutex);
		if (HyperAIStudio::DomainAdapter::Private::FRegisteredAdapter* Entry =
			State->Entries.Find(AdapterFingerprint))
		{
			if (Entry->Descriptor.PackId == PackId
				&& Entry->Descriptor.AdapterId == AdapterId
				&& Entry->Generation == AdapterGeneration
				&& Entry->Adapter == Adapter && Entry->ActiveLeases > 0)
			{
				--Entry->ActiveLeases;
			}
		}
	}
	Adapter.Reset();
	State.Reset();
	Descriptor = nullptr;
	PackId.Reset();
	AdapterId.Reset();
	AdapterFingerprint.Reset();
	AdapterGeneration = 0;
	RegistryEpoch = 0;
}

FHyperAIStudioDomainExecutionLease::FHyperAIStudioDomainExecutionLease() = default;

FHyperAIStudioDomainExecutionLease::FHyperAIStudioDomainExecutionLease(
	const TSharedRef<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe>& InState,
	FHyperAIStudioDomainAdapterLease&& InAdapterLease,
	const FHyperAIStudioDomainRequestEnvelope& InRequest,
	const FHyperAIStudioDomainVariantDescriptor& InVariant)
	: State(InState)
	, AdapterLease(MoveTemp(InAdapterLease))
	, Request(InRequest)
	, Variant(InVariant)
{
	// Bearer material is consumed later, after the journal has admitted a new operation.
	Request.AuthorizationToken.Reset();
}

FHyperAIStudioDomainExecutionLease::~FHyperAIStudioDomainExecutionLease()
{
	Reset();
}

FHyperAIStudioDomainExecutionLease::FHyperAIStudioDomainExecutionLease(
	FHyperAIStudioDomainExecutionLease&& Other) noexcept
	: State(MoveTemp(Other.State))
	, AdapterLease(MoveTemp(Other.AdapterLease))
	, Request(MoveTemp(Other.Request))
	, Variant(MoveTemp(Other.Variant))
	, VerifiedAuthorizationNonce(MoveTemp(Other.VerifiedAuthorizationNonce))
	, AuthorizationExpiresUtcMs(Other.AuthorizationExpiresUtcMs)
	, ConsumedActionNonces(MoveTemp(Other.ConsumedActionNonces))
	, bAuthorized(Other.bAuthorized)
	, bOutcomeUnknown(Other.bOutcomeUnknown)
{
	Other.AuthorizationExpiresUtcMs = 0;
	Other.bAuthorized = false;
	Other.bOutcomeUnknown = false;
}

FHyperAIStudioDomainExecutionLease& FHyperAIStudioDomainExecutionLease::operator=(
	FHyperAIStudioDomainExecutionLease&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		State = MoveTemp(Other.State);
		AdapterLease = MoveTemp(Other.AdapterLease);
		Request = MoveTemp(Other.Request);
		Variant = MoveTemp(Other.Variant);
		VerifiedAuthorizationNonce = MoveTemp(Other.VerifiedAuthorizationNonce);
		AuthorizationExpiresUtcMs = Other.AuthorizationExpiresUtcMs;
		ConsumedActionNonces = MoveTemp(Other.ConsumedActionNonces);
		bAuthorized = Other.bAuthorized;
		bOutcomeUnknown = Other.bOutcomeUnknown;
		Other.AuthorizationExpiresUtcMs = 0;
		Other.bAuthorized = false;
		Other.bOutcomeUnknown = false;
	}
	return *this;
}

void FHyperAIStudioDomainExecutionLease::Reset()
{
	// Destroy optional-module payloads while the adapter/module pin is still held.
	Request.Payload.Reset();
	Request = FHyperAIStudioDomainRequestEnvelope{};
	Variant = FHyperAIStudioDomainVariantDescriptor{};
	VerifiedAuthorizationNonce.Reset();
	AuthorizationExpiresUtcMs = 0;
	ConsumedActionNonces.Reset();
	bAuthorized = false;
	bOutcomeUnknown = false;
	AdapterLease.Reset();
	State.Reset();
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainExecutionLease::RevalidateExact() const
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FHyperAIStudioDomainResolveResult Result;
	Result.AdapterFingerprint = Request.Binding.ExpectedAdapterFingerprint;
	Result.AdapterGeneration = AdapterLease.GetAdapterGeneration();
	Result.RegistryEpoch = AdapterLease.GetRegistryEpoch();
	if (!IsValid() || !State.IsValid())
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("execution_lease_invalid");
		return Result;
	}

	FScopeLock Lock(&State->Mutex);
	Result.RegistryEpoch = State->Epoch;
	const FRegisteredAdapter* Entry = State->Entries.Find(
		Request.Binding.ExpectedAdapterFingerprint);
	if (!Entry || Entry->Adapter != AdapterLease.Adapter
		|| Entry->Descriptor.PackId != Request.Binding.PackId
		|| Entry->Generation != AdapterLease.GetAdapterGeneration()
		|| Entry->Descriptor.AdapterFingerprint != Request.Binding.ExpectedAdapterFingerprint)
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("execution_adapter_drift");
		return Result;
	}
	if (Entry->bOutcomeUnknown || bOutcomeUnknown)
	{
		Result.State = EHyperAIStudioDomainState::OutcomeUnknown;
		Result.DiagnosticCode = TEXT("adapter_outcome_unknown");
		return Result;
	}
	Result.State = EHyperAIStudioDomainState::Ready;
	Result.DiagnosticCode = TEXT("execution_lease_ready");
	return Result;
}

bool FHyperAIStudioDomainExecutionLease::BuildAuthorizationRequest(
	const FString& OpaqueToken,
	FHyperAIStudioDomainAuthorizationRequest& OutRequest,
	FString& OutError) const
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutRequest = FHyperAIStudioDomainAuthorizationRequest{};
	OutError.Reset();
	if (!IsValid() || !State.IsValid() || bAuthorized)
	{
		OutError = bAuthorized ? TEXT("execution_authorization_already_consumed") : TEXT("execution_lease_invalid");
		return false;
	}
	const FHyperAIStudioDomainResolveResult Current = RevalidateExact();
	if (Current.State != EHyperAIStudioDomainState::Ready)
	{
		OutError = Current.DiagnosticCode;
		return false;
	}

	if (Variant.Safety == EHyperAIStudioDomainSafety::Read
		|| Variant.Safety == EHyperAIStudioDomainSafety::Edit)
	{
		OutError = TEXT("external_authorization_not_valid_for_safety_class");
		return false;
	}
	if (!IsSafeId(Request.OperationId, FHyperAIStudioDomainLimits::MaxOperationIdChars)
		|| !IsSha256Fingerprint(Request.PlanHash)
		|| OpaqueToken.Len() < 16
		|| OpaqueToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars
		|| !State->AuthorizationGate)
	{
		OutError = TEXT("mutation_authorization_required");
		return false;
	}

	OutRequest.OpaqueToken = OpaqueToken;
	OutRequest.CanonicalProjectId = Request.Binding.CanonicalProjectId;
	OutRequest.OperationId = Request.OperationId;
	OutRequest.PlanHash = Request.PlanHash;
	OutRequest.PackId = Request.Binding.PackId;
	OutRequest.ToolName = Request.Binding.ToolName;
	OutRequest.VariantId = Request.Binding.VariantId;
	OutRequest.AdapterFingerprint = AdapterLease.GetDescriptor().AdapterFingerprint;
	OutRequest.AdmissionFingerprint = Request.Binding.Admission.Fingerprint;
	OutRequest.PrerequisiteFingerprint = Request.Binding.Prerequisites.Fingerprint;
	OutRequest.Safety = Variant.Safety;
	OutRequest.AdapterGeneration = AdapterLease.GetAdapterGeneration();
	OutRequest.RegistryEpoch = AdapterLease.GetRegistryEpoch();
	return true;
}

bool FHyperAIStudioDomainExecutionLease::InspectAuthorizationExact(
	const FString& OpaqueToken,
	const int64 NowUtcMs,
	FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
	FString& OutError) const
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
	FHyperAIStudioDomainAuthorizationRequest AuthorizationRequest;
	if (NowUtcMs < 0 || !BuildAuthorizationRequest(OpaqueToken, AuthorizationRequest, OutError))
	{
		return false;
	}
	FString AuthorizationError;
	if (!State->AuthorizationGate->Inspect(
			AuthorizationRequest, NowUtcMs, OutReceipt, AuthorizationError)
		|| !SameAuthorizationRequest(OutReceipt.BoundRequest, AuthorizationRequest)
		|| !IsSafeId(OutReceipt.Nonce, FHyperAIStudioDomainLimits::MaxAuthorizationNonceChars)
		|| OutReceipt.IssuedUtcMs < 0 || OutReceipt.IssuedUtcMs > NowUtcMs
		|| OutReceipt.ExpiresUtcMs <= NowUtcMs || OutReceipt.ExpiresUtcMs <= OutReceipt.IssuedUtcMs
		|| OutReceipt.ExpiresUtcMs - OutReceipt.IssuedUtcMs > FHyperAIStudioDomainLimits::MaxAuthorizationLifetimeMs)
	{
		OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
		OutError = TEXT("mutation_authorization_inspection_rejected");
		return false;
	}
	if (RevalidateExact().State != EHyperAIStudioDomainState::Ready)
	{
		OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
		OutError = TEXT("execution_binding_drift_after_authorization_inspection");
		return false;
	}
	return true;
}

bool FHyperAIStudioDomainExecutionLease::AuthorizeExact(
	const FString& OpaqueToken,
	const int64 NowUtcMs,
	FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
	FString& OutError)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
	FHyperAIStudioDomainAuthorizationRequest AuthorizationRequest;
	if (NowUtcMs < 0 || !BuildAuthorizationRequest(OpaqueToken, AuthorizationRequest, OutError))
	{
		return false;
	}
	FString AuthorizationError;
	if (!State->AuthorizationGate->Consume(
			AuthorizationRequest, NowUtcMs, OutReceipt, AuthorizationError)
		|| !OutReceipt.bConsumed
		|| !SameAuthorizationRequest(OutReceipt.BoundRequest, AuthorizationRequest)
		|| !IsSafeId(OutReceipt.Nonce, FHyperAIStudioDomainLimits::MaxAuthorizationNonceChars)
		|| OutReceipt.IssuedUtcMs < 0 || OutReceipt.IssuedUtcMs > NowUtcMs
		|| OutReceipt.ExpiresUtcMs <= NowUtcMs || OutReceipt.ExpiresUtcMs <= OutReceipt.IssuedUtcMs
		|| OutReceipt.ExpiresUtcMs - OutReceipt.IssuedUtcMs > FHyperAIStudioDomainLimits::MaxAuthorizationLifetimeMs)
	{
		OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
		OutError = TEXT("mutation_authorization_rejected");
		return false;
	}
	if (RevalidateExact().State != EHyperAIStudioDomainState::Ready)
	{
		OutReceipt = FHyperAIStudioDomainAuthorizationReceipt{};
		OutError = TEXT("execution_binding_drift_after_authorization");
		return false;
	}
	VerifiedAuthorizationNonce = OutReceipt.Nonce;
	AuthorizationExpiresUtcMs = OutReceipt.ExpiresUtcMs;
	bAuthorized = true;
	return true;
}

bool FHyperAIStudioDomainExecutionLease::AuthorizeJournalAdmittedEdit(FString& OutError)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutError.Reset();
	if (!IsValid() || bAuthorized || Variant.Safety != EHyperAIStudioDomainSafety::Edit
		|| !IsSafeId(Request.OperationId, FHyperAIStudioDomainLimits::MaxOperationIdChars)
		|| !IsSha256Fingerprint(Request.PlanHash))
	{
		OutError = TEXT("journal_admitted_edit_authorization_invalid");
		return false;
	}
	const FHyperAIStudioDomainResolveResult Current = RevalidateExact();
	if (Current.State != EHyperAIStudioDomainState::Ready)
	{
		OutError = Current.DiagnosticCode;
		return false;
	}
	VerifiedAuthorizationNonce =
		TEXT("journal-edit-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	AuthorizationExpiresUtcMs = 0;
	bAuthorized = true;
	return true;
}

void FHyperAIStudioDomainExecutionLease::LatchOutcomeUnknown()
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	if (!IsValid() || !State.IsValid() || Variant.Safety == EHyperAIStudioDomainSafety::Read)
	{
		return;
	}
	FScopeLock Lock(&State->Mutex);
	if (FRegisteredAdapter* Entry = State->Entries.Find(
		Request.Binding.ExpectedAdapterFingerprint))
	{
		if (Entry->Generation == AdapterLease.GetAdapterGeneration()
			&& Entry->Adapter == AdapterLease.Adapter && !Entry->bOutcomeUnknown)
		{
			Entry->bOutcomeUnknown = true;
			Entry->UnknownOperationId = Request.OperationId;
			Entry->UnknownPlanHash = Request.PlanHash;
			++State->Epoch;
		}
	}
	bOutcomeUnknown = true;
}

FHyperAIStudioDomainDispatchResult FHyperAIStudioDomainExecutionLease::DispatchAction(
	const FHyperAIStudioDomainExecutionAction& Action)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FHyperAIStudioDomainDispatchResult Output;
	Output.AdapterFingerprint = Request.Binding.ExpectedAdapterFingerprint;
	Output.AdapterGeneration = AdapterLease.GetAdapterGeneration();
	Output.RegistryEpoch = AdapterLease.GetRegistryEpoch();
	if (!IsValid() || !bAuthorized)
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("execution_lease_not_authorized");
		return Output;
	}
	if (static_cast<uint8>(Action.Kind) > static_cast<uint8>(EHyperAIStudioDomainExecutionActionKind::VerifyFresh)
		|| (Variant.Safety == EHyperAIStudioDomainSafety::Read
			&& Action.Kind != EHyperAIStudioDomainExecutionActionKind::Apply)
		|| Action.ActionNonce.Len() < 16
		|| !IsSafeId(Action.ActionNonce, FHyperAIStudioDomainLimits::MaxAuthorizationNonceChars)
		|| ConsumedActionNonces.Num() >= 16 || ConsumedActionNonces.Contains(Action.ActionNonce))
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("execution_action_invalid_or_replayed");
		return Output;
	}
	const FHyperAIStudioDomainResolveResult Current = RevalidateExact();
	Output.State = Current.State;
	Output.RegistryEpoch = Current.RegistryEpoch;
	if (Current.State != EHyperAIStudioDomainState::Ready)
	{
		Output.StatusCode = Current.DiagnosticCode;
		return Output;
	}
	ConsumedActionNonces.Add(Action.ActionNonce);

	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding = Request.Binding;
	Context.Safety = Variant.Safety;
	Context.OperationId = Request.OperationId;
	Context.PlanHash = Request.PlanHash;
	Context.VerifiedAuthorizationNonce = VerifiedAuthorizationNonce;
	Context.ActionKind = Action.Kind;
	Context.ActionNonce = Action.ActionNonce;
	Context.AdapterGeneration = AdapterLease.GetAdapterGeneration();
	Context.RegistryEpoch = AdapterLease.GetRegistryEpoch();

	FHyperAIStudioDomainAdapterResult AdapterResult = AdapterLease.Adapter->Execute(Context, *Request.Payload);
	if (static_cast<uint8>(AdapterResult.Outcome)
		> static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown)
		|| (Variant.Safety == EHyperAIStudioDomainSafety::Read
			&& AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect))
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("adapter_outcome_invalid");
		Output.Diagnostic = TEXT("The adapter returned an outcome invalid for the exact safety class.");
		return Output;
	}
	Output.Outcome = AdapterResult.Outcome;
	Output.StatusCode = IsSafeStatusCode(AdapterResult.StatusCode)
		? AdapterResult.StatusCode : TEXT("adapter_status_invalid");
	Output.Diagnostic = AdapterResult.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
	Output.State = EHyperAIStudioDomainState::Ready;

	if (AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
	{
		const FString ResultTypeId = AdapterResult.Payload.IsValid() ? AdapterResult.Payload->GetTypeId() : FString();
		const FString ResultSchema = AdapterResult.Payload.IsValid() ? AdapterResult.Payload->GetSchemaFingerprint() : FString();
		const int32 ResultBytes = AdapterResult.Payload.IsValid() ? AdapterResult.Payload->GetBoundedByteSize() : -1;
		if (!AdapterResult.Payload.IsValid() || ResultTypeId != Variant.ResultTypeId
			|| ResultSchema != Variant.ResultSchemaFingerprint || !IsSafeResultTypeId(ResultTypeId)
			|| ResultBytes < 0 || ResultBytes > Request.MaxResultBytes
			|| ResultBytes > FHyperAIStudioDomainLimits::MaxResultBytes)
		{
			Output.Outcome = Variant.Safety == EHyperAIStudioDomainSafety::Read
				? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
				: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
			Output.StatusCode = TEXT("result_envelope_invalid");
			Output.Diagnostic = TEXT("The adapter result type or byte size violated the exact bounded contract.");
		}
		else
		{
			Output.Payload = AdapterResult.Payload;
		}
	}
	if (AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown)
	{
		if (Variant.Safety == EHyperAIStudioDomainSafety::Read)
		{
			Output.State = EHyperAIStudioDomainState::Disabled;
			Output.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
			Output.StatusCode = TEXT("read_outcome_unknown_invalid");
		}
		else
		{
			LatchOutcomeUnknown();
			Output.State = EHyperAIStudioDomainState::OutcomeUnknown;
			Output.StatusCode = TEXT("outcome_unknown");
			Output.RegistryEpoch = RevalidateExact().RegistryEpoch;
		}
		Output.Payload.Reset();
	}
	else if (RevalidateExact().State != EHyperAIStudioDomainState::Ready)
	{
		Output.Payload.Reset();
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.Outcome = Variant.Safety == EHyperAIStudioDomainSafety::Read
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		Output.StatusCode = TEXT("execution_binding_drift_after_dispatch");
	}
	return Output;
}

FHyperAIStudioDomainAdapterRegistry::FHyperAIStudioDomainAdapterRegistry(
	IHyperAIStudioDomainAdapterLoader* InLoader,
	TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> InAuthorizationGate,
	TOptional<EHyperAIStudioNativeExecutionMode> InExecutionModeOverride)
	: State(MakeShared<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe>())
{
	(void)InLoader;
	State->AuthorizationGate = MoveTemp(InAuthorizationGate);
	State->ExecutionModeOverride = InExecutionModeOverride;
}

FHyperAIStudioDomainAdapterRegistry::~FHyperAIStudioDomainAdapterRegistry() = default;

FString FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
	const FHyperAIStudioDomainPrerequisiteSnapshot& Snapshot)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	TArray<FHyperAIStudioDomainPrerequisiteObservation> Sorted = Snapshot.Observations;
	Sorted.Sort([](const FHyperAIStudioDomainPrerequisiteObservation& A,
		const FHyperAIStudioDomainPrerequisiteObservation& B)
	{
		return A.Id < B.Id;
	});

	FString Canonical;
	const bool bV2 = Snapshot.TrustSeal.SchemaVersion != 0;
	if (!AppendToken(Canonical, bV2
			? TEXT("hyperai.domain-prerequisites.v2")
			: TEXT("hyperai.domain-prerequisites.v1"))
		|| !AppendToken(Canonical, Snapshot.PackId)
		|| !AppendBool(Canonical, Snapshot.bPackEnabled)
		|| !AppendUInt(Canonical, Snapshot.Revision)
		|| !AppendUInt(Canonical, static_cast<uint64>(Sorted.Num()))
		|| (bV2 && !AppendTrustSeal(Canonical, Snapshot.TrustSeal)))
	{
		return FString();
	}
	for (const FHyperAIStudioDomainPrerequisiteObservation& Observation : Sorted)
	{
		if (!AppendToken(Canonical, Observation.Id)
			|| !AppendUInt(Canonical, static_cast<uint64>(Observation.State)))
		{
			return FString();
		}
	}
	return HashUtf8Sha256(Canonical);
}

FString FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
	const FHyperAIStudioDomainAdmissionSnapshot& Snapshot)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FString Canonical;
	const bool bV2 = Snapshot.TrustSeal.SchemaVersion != 0;
	if (!AppendToken(Canonical, bV2
			? TEXT("hyperai.domain-admission.v2")
			: TEXT("hyperai.domain-admission.v1"))
		|| !AppendToken(Canonical, Snapshot.PackId)
		|| !AppendBool(Canonical, Snapshot.bPackAdmitted)
		|| !AppendBool(Canonical, Snapshot.bReadAdmitted)
		|| !AppendBool(Canonical, Snapshot.bEditAdmitted)
		|| !AppendBool(Canonical, Snapshot.bDestructiveAdmitted)
		|| !AppendBool(Canonical, Snapshot.bExternalEffectAdmitted)
		|| !AppendUInt(Canonical, Snapshot.Revision)
		|| (bV2 && !AppendTrustSeal(Canonical, Snapshot.TrustSeal)))
	{
		return FString();
	}
	return HashUtf8Sha256(Canonical);
}

FString FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	TArray<FHyperAIStudioDomainVariantDescriptor> Sorted = Descriptor.Variants;
	Sorted.Sort([](const FHyperAIStudioDomainVariantDescriptor& A,
		const FHyperAIStudioDomainVariantDescriptor& B)
	{
		if (A.ToolName != B.ToolName)
		{
			return A.ToolName < B.ToolName;
		}
		return A.VariantId < B.VariantId;
	});

	TArray<FString> SortedRequirementGroups = Descriptor.ApplicableNonBlockingRequirementGroupIds;
	SortedRequirementGroups.Sort();
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.domain-contract.v2"))
		|| !AppendToken(Canonical, Descriptor.PackId)
		|| !AppendUInt(Canonical, static_cast<uint64>(SortedRequirementGroups.Num())))
	{
		return FString();
	}
	for (const FString& GroupId : SortedRequirementGroups)
	{
		if (!AppendToken(Canonical, GroupId))
		{
			return FString();
		}
	}
	if (!AppendUInt(Canonical, static_cast<uint64>(Sorted.Num())))
	{
		return FString();
	}
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Sorted)
	{
		if (!AppendToken(Canonical, Variant.ToolName)
			|| !AppendToken(Canonical, Variant.VariantId)
			|| !AppendToken(Canonical, Variant.RequestTypeId)
			|| !AppendToken(Canonical, Variant.RequestSchemaFingerprint)
			|| !AppendToken(Canonical, Variant.ResultTypeId)
			|| !AppendToken(Canonical, Variant.ResultSchemaFingerprint)
			|| !AppendUInt(Canonical, static_cast<uint64>(Variant.Safety)))
		{
			return FString();
		}
	}
	return HashUtf8Sha256(Canonical);
}

FString FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FString Canonical;
	const FString ContractFingerprint = ComputeContractFingerprint(Descriptor);
	if (!IsSha256Fingerprint(ContractFingerprint)
		|| !AppendToken(Canonical, TEXT("hyperai.domain-adapter.v2"))
		|| !AppendToken(Canonical, Descriptor.PackId)
		|| !AppendToken(Canonical, Descriptor.AdapterId)
		|| !AppendToken(Canonical, Descriptor.SemanticVersion)
		|| !AppendUInt(Canonical, Descriptor.AdapterVersion)
		|| !AppendToken(Canonical, ContractFingerprint))
	{
		return FString();
	}
	return HashUtf8Sha256(Canonical);
}

bool FHyperAIStudioDomainAdapterRegistry::RegisterAdapter(
	const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
	FHyperAIStudioDomainRegistrationHandle& OutHandle,
	FString& OutError)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutHandle = FHyperAIStudioDomainRegistrationHandle{};
	OutError.Reset();
	const FHyperAIStudioDomainAdapterDescriptor Descriptor = Adapter->GetDescriptor();
	if (!ValidateDescriptor(Descriptor, OutError))
	{
		return false;
	}

	// Keep a replaced adapter alive until after the registry mutex is released; its destructor
	// belongs to an optional module and must never run under the core lock.
	TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> ReplacedAdapter;
	FScopeLock Lock(&State->Mutex);
	FRegisteredAdapter* Existing = Descriptor.ReplacesAdapterFingerprint.IsEmpty()
		? nullptr : State->Entries.Find(Descriptor.ReplacesAdapterFingerprint);
	if (!Descriptor.ReplacesAdapterFingerprint.IsEmpty() && !Existing)
	{
		OutError = TEXT("adapter_replacement_target_missing");
		return false;
	}
	if (Existing)
	{
		if (Descriptor.ReplacesAdapterFingerprint != Existing->Descriptor.AdapterFingerprint
			|| Descriptor.PackId != Existing->Descriptor.PackId
			|| Descriptor.AdapterId != Existing->Descriptor.AdapterId
			|| Descriptor.ContractFingerprint != Existing->Descriptor.ContractFingerprint
			|| Descriptor.AdapterVersion <= Existing->Descriptor.AdapterVersion)
		{
			OutError = TEXT("adapter_exact_replacement_mismatch");
			return false;
		}
		if (Existing->bOutcomeUnknown)
		{
			OutError = TEXT("adapter_outcome_unknown");
			return false;
		}
		if (Existing->ActiveLeases > 0)
		{
			OutError = TEXT("adapter_busy");
			return false;
		}
		ReplacedAdapter = Existing->Adapter;
	}

	for (const TPair<FString, FRegisteredAdapter>& Pair : State->Entries)
	{
		if (Existing && Pair.Key == Existing->Descriptor.AdapterFingerprint)
		{
			continue;
		}
		if ((Pair.Value.Descriptor.PackId == Descriptor.PackId
				&& Pair.Value.Descriptor.AdapterId == Descriptor.AdapterId)
			|| Pair.Value.Descriptor.AdapterFingerprint == Descriptor.AdapterFingerprint)
		{
			OutError = TEXT("adapter_identity_collision");
			return false;
		}
		for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
		{
			if (Pair.Value.Descriptor.Variants.ContainsByPredicate(
				[&Variant](const FHyperAIStudioDomainVariantDescriptor& Other)
				{
					return Other.ToolName == Variant.ToolName;
				}))
			{
				OutError = TEXT("adapter_tool_collision");
				return false;
			}
		}
	}

	FRegisteredAdapter NewEntry;
	NewEntry.Descriptor = Descriptor;
	NewEntry.Adapter = Adapter;
	NewEntry.Generation = State->NextGeneration++;
	if (Existing)
	{
		State->Entries.Remove(Descriptor.ReplacesAdapterFingerprint);
	}
	State->Entries.Add(Descriptor.AdapterFingerprint, MoveTemp(NewEntry));
	++State->Epoch;

	const FRegisteredAdapter& Registered = State->Entries.FindChecked(Descriptor.AdapterFingerprint);
	OutHandle.PackId = Descriptor.PackId;
	OutHandle.AdapterId = Descriptor.AdapterId;
	OutHandle.AdapterFingerprint = Descriptor.AdapterFingerprint;
	OutHandle.AdapterGeneration = Registered.Generation;
	OutHandle.RegistryEpoch = State->Epoch;
	return true;
}

EHyperAIStudioDomainUnregisterResult FHyperAIStudioDomainAdapterRegistry::UnregisterAdapter(
	const FHyperAIStudioDomainRegistrationHandle& Handle,
	FString& OutError)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutError.Reset();
	// Same rule as replacement: optional-module destruction happens only after the lock releases.
	TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> RemovedAdapter;
	FScopeLock Lock(&State->Mutex);
	FRegisteredAdapter* Existing = State->Entries.Find(Handle.AdapterFingerprint);
	if (!Existing)
	{
		OutError = TEXT("adapter_not_found");
		return EHyperAIStudioDomainUnregisterResult::NotFound;
	}
	if (!Handle.IsValid() || Existing->Generation != Handle.AdapterGeneration
		|| Existing->Descriptor.PackId != Handle.PackId
		|| Existing->Descriptor.AdapterId != Handle.AdapterId
		|| Existing->Descriptor.AdapterFingerprint != Handle.AdapterFingerprint)
	{
		OutError = TEXT("adapter_stale_handle");
		return EHyperAIStudioDomainUnregisterResult::StaleHandle;
	}
	if (Existing->bOutcomeUnknown)
	{
		OutError = TEXT("adapter_outcome_unknown");
		return EHyperAIStudioDomainUnregisterResult::OutcomeUnknown;
	}
	if (Existing->ActiveLeases > 0)
	{
		OutError = TEXT("adapter_busy");
		return EHyperAIStudioDomainUnregisterResult::Busy;
	}

	RemovedAdapter = Existing->Adapter;
	State->Entries.Remove(Handle.AdapterFingerprint);
	++State->Epoch;
	return EHyperAIStudioDomainUnregisterResult::Removed;
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainAdapterRegistry::ResolveStatus(
	const FHyperAIStudioDomainBinding& Binding) const
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FHyperAIStudioDomainResolveResult Result;
	if (!ValidateBindingSnapshotsWithPolicy(
			*State,
			Binding,
			Result,
			FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
				State->ExecutionModeOverride.IsSet()
					? State->ExecutionModeOverride.GetValue()
					: FHyperAIStudioExtensionRuntime::GetNativeExecutionMode(),
				Binding.ExpectedSafety)))
	{
		FScopeLock Lock(&State->Mutex);
		Result.RegistryEpoch = State->Epoch;
		return Result;
	}

	FScopeLock Lock(&State->Mutex);
	return ResolveLocked(*State, Binding, false);
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainAdapterRegistry::AcquireExact(
	const FHyperAIStudioDomainBinding& Binding,
	FHyperAIStudioDomainAdapterLease& OutLease,
	const bool bAllowActiveLease)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutLease.Reset();
	FHyperAIStudioDomainResolveResult Result;
	if (!ValidateBindingSnapshotsWithPolicy(
			*State,
			Binding,
			Result,
			FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
				State->ExecutionModeOverride.IsSet()
					? State->ExecutionModeOverride.GetValue()
					: FHyperAIStudioExtensionRuntime::GetNativeExecutionMode(),
				Binding.ExpectedSafety)))
	{
		FScopeLock Lock(&State->Mutex);
		Result.RegistryEpoch = State->Epoch;
		return Result;
	}

	FScopeLock Lock(&State->Mutex);
	Result = ResolveLocked(*State, Binding, bAllowActiveLease);
	if (Result.State == EHyperAIStudioDomainState::Ready)
	{
		FRegisteredAdapter& Entry = State->Entries.FindChecked(
			Binding.ExpectedAdapterFingerprint);
		++Entry.ActiveLeases;
		OutLease = FHyperAIStudioDomainAdapterLease(
			State,
			Entry.Adapter.ToSharedRef(),
			Entry.Descriptor,
			Binding.PackId,
			Entry.Generation,
			State->Epoch);
	}
	return Result;
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainAdapterRegistry::WarmExact(
	const FHyperAIStudioDomainBinding& Binding)
{
	FHyperAIStudioDomainAdapterLease Lease;
	FHyperAIStudioDomainResolveResult Result = AcquireExact(Binding, Lease);
	Lease.Reset();
	return Result;
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainAdapterRegistry::AcquireReadyLease(
	const FHyperAIStudioDomainBinding& Binding,
	FHyperAIStudioDomainAdapterLease& OutLease)
{
	return AcquireExact(Binding, OutLease);
}

FHyperAIStudioDomainResolveResult FHyperAIStudioDomainAdapterRegistry::AcquireExecutionLease(
	const FHyperAIStudioDomainRequestEnvelope& Request,
	FHyperAIStudioDomainExecutionLease& OutLease)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	OutLease.Reset();
	FHyperAIStudioDomainResolveResult Result;
	Result.AdapterFingerprint = Request.Binding.ExpectedAdapterFingerprint;
	Result.RegistryEpoch = GetRegistryEpoch();
	if (!Request.AuthorizationToken.IsEmpty())
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("execution_acquire_rejects_bearer_storage");
		return Result;
	}
	if (!Request.Payload.IsValid())
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("request_envelope_invalid");
		return Result;
	}
	const FString RequestTypeId = Request.Payload->GetTypeId();
	const FString RequestSchema = Request.Payload->GetSchemaFingerprint();
	const int32 RequestBytes = Request.Payload->GetBoundedByteSize();
	if (RequestBytes < 0 || RequestBytes > FHyperAIStudioDomainLimits::MaxRequestBytes
		|| !IsSafePayloadTypeId(RequestTypeId) || !IsSha256Fingerprint(RequestSchema)
		|| Request.MaxResultBytes < 0 || Request.MaxResultBytes > FHyperAIStudioDomainLimits::MaxResultBytes)
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("request_envelope_invalid");
		return Result;
	}

	FHyperAIStudioDomainAdapterLease AdapterLease;
	// The bounded stage store may pin several immutable artifacts for the same exact
	// registered generation. Project-lane serialization still happens at submission,
	// while every pin continues to veto replacement and module unload.
	Result = AcquireExact(Request.Binding, AdapterLease, true);
	if (!AdapterLease.IsValid())
	{
		return Result;
	}
	const FHyperAIStudioDomainVariantDescriptor* Variant = FindVariant(
		AdapterLease.GetDescriptor(), Request.Binding.ToolName, Request.Binding.VariantId);
	if (!Variant || Variant->Safety != Request.Binding.ExpectedSafety
		|| Variant->RequestTypeId != RequestTypeId
		|| Variant->RequestSchemaFingerprint != RequestSchema)
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("typed_payload_binding_mismatch");
		return Result;
	}
	if (Variant->Safety == EHyperAIStudioDomainSafety::Read)
	{
		if (!Request.OperationId.IsEmpty() || !Request.PlanHash.IsEmpty())
		{
			Result.State = EHyperAIStudioDomainState::Disabled;
			Result.DiagnosticCode = TEXT("read_authorization_fields_rejected");
			return Result;
		}
	}
	else if (!IsSafeId(Request.OperationId, FHyperAIStudioDomainLimits::MaxOperationIdChars)
		|| !IsSha256Fingerprint(Request.PlanHash))
	{
		Result.State = EHyperAIStudioDomainState::Disabled;
		Result.DiagnosticCode = TEXT("mutation_identity_required");
		return Result;
	}

	Result.State = EHyperAIStudioDomainState::Ready;
	Result.DiagnosticCode = TEXT("execution_lease_ready");
	Result.AdapterFingerprint = AdapterLease.GetDescriptor().AdapterFingerprint;
	Result.AdapterGeneration = AdapterLease.GetAdapterGeneration();
	Result.RegistryEpoch = AdapterLease.GetRegistryEpoch();
	const FHyperAIStudioDomainVariantDescriptor ExactVariant = *Variant;
	OutLease = FHyperAIStudioDomainExecutionLease(State, MoveTemp(AdapterLease), Request, ExactVariant);
	return Result;
}

void FHyperAIStudioDomainAdapterRegistry::MarkOutcomeUnknown(
	const FHyperAIStudioDomainAdapterLease& Lease,
	const FString& OperationId,
	const FString& PlanHash)
{
	if (!Lease.IsValid())
	{
		return;
	}
	FScopeLock Lock(&State->Mutex);
	if (HyperAIStudio::DomainAdapter::Private::FRegisteredAdapter* Entry =
		State->Entries.Find(Lease.AdapterFingerprint))
	{
		if (Entry->Generation == Lease.AdapterGeneration && Entry->Adapter == Lease.Adapter)
		{
			Entry->bOutcomeUnknown = true;
			Entry->UnknownOperationId = OperationId;
			Entry->UnknownPlanHash = PlanHash;
			++State->Epoch;
		}
	}
}

FHyperAIStudioDomainDispatchResult FHyperAIStudioDomainAdapterRegistry::DispatchExact(
	const FHyperAIStudioDomainRequestEnvelope& Request)
{
	using namespace HyperAIStudio::DomainAdapter::Private;
	FHyperAIStudioDomainDispatchResult Output;
	Output.AdapterFingerprint = Request.Binding.ExpectedAdapterFingerprint;
	if (!Request.Payload.IsValid())
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("request_envelope_invalid");
		Output.Diagnostic = TEXT("The typed request payload is required.");
		return Output;
	}
	const FString RequestTypeId = Request.Payload->GetTypeId();
	const FString RequestSchemaFingerprint = Request.Payload->GetSchemaFingerprint();
	const int32 RequestBytes = Request.Payload->GetBoundedByteSize();
	if (RequestBytes < 0
		|| RequestBytes > FHyperAIStudioDomainLimits::MaxRequestBytes
		|| !IsSafePayloadTypeId(RequestTypeId)
		|| !IsSha256Fingerprint(RequestSchemaFingerprint)
		|| Request.MaxResultBytes < 0
		|| Request.MaxResultBytes > FHyperAIStudioDomainLimits::MaxResultBytes)
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("request_envelope_invalid");
		Output.Diagnostic = TEXT("The typed request or result budget exceeded the shared runtime bounds.");
		return Output;
	}

	FHyperAIStudioDomainAdapterLease Lease;
	const FHyperAIStudioDomainResolveResult Resolution = AcquireExact(Request.Binding, Lease);
	Output.State = Resolution.State;
	Output.Diagnostic = Resolution.DiagnosticCode;
	Output.AdapterFingerprint = Resolution.AdapterFingerprint;
	Output.AdapterGeneration = Resolution.AdapterGeneration;
	Output.RegistryEpoch = Resolution.RegistryEpoch;
	if (!Lease.IsValid())
	{
		Output.StatusCode = TEXT("adapter_not_ready");
		return Output;
	}

	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = Lease.GetDescriptor();
	const FHyperAIStudioDomainVariantDescriptor* Variant = FindVariant(
		Descriptor, Request.Binding.ToolName, Request.Binding.VariantId);
	if (!Variant || Variant->Safety != Request.Binding.ExpectedSafety
		|| Variant->RequestTypeId != RequestTypeId
		|| Variant->RequestSchemaFingerprint != RequestSchemaFingerprint)
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.StatusCode = TEXT("typed_payload_binding_mismatch");
		Output.Diagnostic = TEXT("The request payload type does not match the exact registered variant.");
		return Output;
	}
	if (Variant->Safety != EHyperAIStudioDomainSafety::Read)
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
		Output.StatusCode = TEXT("mutation_requires_typed_executor");
		Output.Diagnostic = TEXT("Mutating domain variants are executable only through the durable typed-artifact journal coordinator.");
		return Output;
	}

	FString VerifiedAuthorizationNonce;
	const bool bMutation = Variant->Safety != EHyperAIStudioDomainSafety::Read;
	if (!bMutation)
	{
		if (!Request.OperationId.IsEmpty() || !Request.PlanHash.IsEmpty() || !Request.AuthorizationToken.IsEmpty())
		{
			Output.State = EHyperAIStudioDomainState::Disabled;
			Output.StatusCode = TEXT("read_authorization_fields_rejected");
			Output.Diagnostic = TEXT("Read variants do not accept mutation identity or authorization fields.");
			return Output;
		}
	}
	else
	{
		if (!IsSafeId(Request.OperationId, FHyperAIStudioDomainLimits::MaxOperationIdChars)
			|| !IsSha256Fingerprint(Request.PlanHash)
			|| Request.AuthorizationToken.Len() < 16
			|| Request.AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars
			|| !State->AuthorizationGate)
		{
			Output.State = EHyperAIStudioDomainState::Disabled;
			Output.StatusCode = TEXT("mutation_authorization_required");
			Output.Diagnostic = TEXT("Every mutating variant requires operation_id, plan hash, and a server authorization token.");
			return Output;
		}

		FHyperAIStudioDomainAuthorizationRequest AuthorizationRequest;
		AuthorizationRequest.OpaqueToken = Request.AuthorizationToken;
		AuthorizationRequest.CanonicalProjectId = Request.Binding.CanonicalProjectId;
		AuthorizationRequest.OperationId = Request.OperationId;
		AuthorizationRequest.PlanHash = Request.PlanHash;
		AuthorizationRequest.PackId = Request.Binding.PackId;
		AuthorizationRequest.ToolName = Request.Binding.ToolName;
		AuthorizationRequest.VariantId = Request.Binding.VariantId;
		AuthorizationRequest.AdapterFingerprint = Descriptor.AdapterFingerprint;
		AuthorizationRequest.AdmissionFingerprint = Request.Binding.Admission.Fingerprint;
		AuthorizationRequest.PrerequisiteFingerprint = Request.Binding.Prerequisites.Fingerprint;
		AuthorizationRequest.Safety = Variant->Safety;
		AuthorizationRequest.AdapterGeneration = Lease.GetAdapterGeneration();
		AuthorizationRequest.RegistryEpoch = Lease.GetRegistryEpoch();

		const int64 NowUtcMs = CurrentUtcMs();
		FHyperAIStudioDomainAuthorizationReceipt Receipt;
		FString AuthorizationError;
		if (!State->AuthorizationGate->Consume(
				AuthorizationRequest, NowUtcMs, Receipt, AuthorizationError)
			|| !Receipt.bConsumed
			|| !SameAuthorizationRequest(Receipt.BoundRequest, AuthorizationRequest)
			|| !IsSafeId(Receipt.Nonce, FHyperAIStudioDomainLimits::MaxAuthorizationNonceChars)
			|| Receipt.IssuedUtcMs < 0 || Receipt.IssuedUtcMs > NowUtcMs
			|| Receipt.ExpiresUtcMs <= NowUtcMs || Receipt.ExpiresUtcMs <= Receipt.IssuedUtcMs
			|| Receipt.ExpiresUtcMs - Receipt.IssuedUtcMs > FHyperAIStudioDomainLimits::MaxAuthorizationLifetimeMs)
		{
			Output.State = EHyperAIStudioDomainState::Disabled;
			Output.StatusCode = TEXT("mutation_authorization_rejected");
			Output.Diagnostic = TEXT("The server authorization receipt did not bind exactly to this mutation.");
			return Output;
		}
		VerifiedAuthorizationNonce = Receipt.Nonce;
	}

	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding = Request.Binding;
	Context.Safety = Variant->Safety;
	Context.OperationId = Request.OperationId;
	Context.PlanHash = Request.PlanHash;
	Context.VerifiedAuthorizationNonce = VerifiedAuthorizationNonce;
	Context.AdapterGeneration = Lease.GetAdapterGeneration();
	Context.RegistryEpoch = Lease.GetRegistryEpoch();

	FHyperAIStudioDomainAdapterResult AdapterResult = Lease.Adapter->Execute(Context, *Request.Payload);
	if (static_cast<uint8>(AdapterResult.Outcome)
		> static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown)
		|| (!bMutation
			&& AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect))
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
		Output.StatusCode = TEXT("adapter_outcome_invalid");
		Output.Diagnostic = TEXT("The adapter returned an outcome that is invalid for this safety class.");
		return Output;
	}
	Output.Outcome = AdapterResult.Outcome;
	Output.StatusCode = IsSafeStatusCode(AdapterResult.StatusCode)
		? AdapterResult.StatusCode
		: TEXT("adapter_status_invalid");
	Output.Diagnostic = AdapterResult.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
	Output.State = EHyperAIStudioDomainState::Ready;

	if (AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
	{
		const FString ResultTypeId = AdapterResult.Payload.IsValid()
			? AdapterResult.Payload->GetTypeId() : FString();
		const FString ResultSchemaFingerprint = AdapterResult.Payload.IsValid()
			? AdapterResult.Payload->GetSchemaFingerprint() : FString();
		const int32 ResultBytes = AdapterResult.Payload.IsValid()
			? AdapterResult.Payload->GetBoundedByteSize() : -1;
		if (!AdapterResult.Payload.IsValid()
			|| ResultTypeId != Variant->ResultTypeId
			|| ResultSchemaFingerprint != Variant->ResultSchemaFingerprint
			|| !IsSafeResultTypeId(ResultTypeId)
			|| ResultBytes < 0
			|| ResultBytes > Request.MaxResultBytes
			|| ResultBytes > FHyperAIStudioDomainLimits::MaxResultBytes)
		{
			Output.Outcome = bMutation
				? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect
				: EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
			Output.StatusCode = TEXT("result_envelope_invalid");
			Output.Diagnostic = TEXT("The adapter result type or byte size violated the exact bounded contract.");
			Output.Payload.Reset();
		}
		else
		{
			const TSharedRef<FResultPayloadLifetimeOwner, ESPMode::ThreadSafe> Owner =
				MakeShared<FResultPayloadLifetimeOwner, ESPMode::ThreadSafe>(
					MoveTemp(Lease), AdapterResult.Payload.ToSharedRef());
			Output.Payload = TSharedPtr<const IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>(
				Owner, AdapterResult.Payload.Get());
		}
	}

	if (AdapterResult.Outcome == EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown)
	{
		Output.State = EHyperAIStudioDomainState::Disabled;
		Output.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
		Output.StatusCode = TEXT("read_outcome_unknown_invalid");
		Output.Diagnostic = TEXT("A read-only adapter cannot report an ambiguous mutation outcome.");
		Output.Payload.Reset();
	}
	return Output;
}

uint64 FHyperAIStudioDomainAdapterRegistry::GetRegistryEpoch() const
{
	FScopeLock Lock(&State->Mutex);
	return State->Epoch;
}

const TCHAR* FHyperAIStudioDomainAdapterRegistry::LexToString(const EHyperAIStudioDomainState StateValue)
{
	switch (StateValue)
	{
	case EHyperAIStudioDomainState::Available: return TEXT("available");
	case EHyperAIStudioDomainState::Disabled: return TEXT("disabled");
	case EHyperAIStudioDomainState::MissingPrerequisite: return TEXT("missing_prerequisite");
	case EHyperAIStudioDomainState::RestartRequired: return TEXT("restart_required");
	case EHyperAIStudioDomainState::Loading: return TEXT("loading");
	case EHyperAIStudioDomainState::Ready: return TEXT("ready");
	case EHyperAIStudioDomainState::Busy: return TEXT("busy");
	case EHyperAIStudioDomainState::OutcomeUnknown: return TEXT("outcome_unknown");
	default: return TEXT("missing_prerequisite");
	}
}
