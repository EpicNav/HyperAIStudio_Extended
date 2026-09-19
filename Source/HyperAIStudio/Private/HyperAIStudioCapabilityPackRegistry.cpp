// Games by Hyper 2026.

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityCatalogCounts.h"

#include "Misc/SecureHash.h"

#include "HyperAIStudioCapabilityPackCatalog.generated.inl"

namespace HyperAIStudio::CapabilityPacks::Private
{
	bool IsSha256(const FString& Value)
	{
		if (!Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive) || Value.Len() != 71)
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!FChar::IsDigit(Character) && !(Character >= TEXT('a') && Character <= TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

	FString Sha1(const FString& Canonical)
	{
		FTCHARToUTF8 Utf8(*Canonical);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		FString Hex;
		Hex.Reserve(5 + FSHA1::DigestSize * 2);
		Hex = TEXT("sha1:");
		for (uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex;
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

	bool AppendCanonicalToken(FString& Buffer, const FString& Value)
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

	bool AppendCanonicalInt(FString& Buffer, const int64 Value)
	{
		return AppendCanonicalToken(Buffer, FString::Printf(TEXT("%lld"), Value));
	}

	bool AppendCanonicalBool(FString& Buffer, const bool bValue)
	{
		return AppendCanonicalToken(Buffer, bValue ? TEXT("1") : TEXT("0"));
	}

	bool AppendCanonicalSequence(FString& Buffer, TArray<FString> Values)
	{
		Values.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
		});
		if (!AppendCanonicalInt(Buffer, Values.Num()))
		{
			return false;
		}
		for (const FString& Value : Values)
		{
			if (!AppendCanonicalToken(Buffer, Value))
			{
				return false;
			}
		}
		return true;
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

	FString BuildCatalogFingerprint(const FHyperAIStudioCapabilityCatalog& Catalog)
	{
		FString Canonical;
			if (!AppendCanonicalToken(Canonical, TEXT("hyperai.capability-pack-catalog.semantic.v2"))
			|| !AppendCanonicalToken(Canonical, Catalog.Schema)
			|| !AppendCanonicalToken(Canonical, Catalog.ApprovedPlanFingerprint)
			|| !AppendCanonicalToken(Canonical, Catalog.AdmissionMatrixFingerprint)
			|| !AppendCanonicalToken(Canonical, Catalog.SourceLedgerFingerprint)
			|| !AppendCanonicalToken(Canonical, Catalog.SourceArtifactFingerprint)
			|| !AppendCanonicalInt(Canonical, Catalog.SourceArtifactCount))
		{
			return FString();
		}

		TArray<FString> PrerequisiteRows;
		for (const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition : Catalog.Prerequisites)
		{
			FString Row;
			if (!AppendCanonicalToken(Row, TEXT("prerequisite"))
				|| !AppendCanonicalToken(Row, Definition.Id)
				|| !AppendCanonicalInt(Row, static_cast<uint8>(Definition.Kind)))
			{
				return FString();
			}
			PrerequisiteRows.Add(MoveTemp(Row));
		}
		if (!AppendCanonicalSequence(Canonical, MoveTemp(PrerequisiteRows)))
		{
			return FString();
		}

		TArray<FString> PackRows;
		for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
		{
			TArray<FString> RequirementRows;
			for (const FHyperAIStudioCapabilityRequirementGroup& Requirement : Pack.Requirements)
			{
				FString Row;
				if (!AppendCanonicalToken(Row, TEXT("requirement"))
					|| !AppendCanonicalToken(Row, Requirement.Id)
					|| !AppendCanonicalInt(Row, static_cast<uint8>(Requirement.Mode))
					|| !AppendCanonicalBool(Row, Requirement.bBlocking)
					|| !AppendCanonicalSequence(Row, Requirement.PrerequisiteIds))
				{
					return FString();
				}
				RequirementRows.Add(MoveTemp(Row));
			}

			FString Row;
			if (!AppendCanonicalToken(Row, TEXT("pack"))
				|| !AppendCanonicalToken(Row, Pack.Id)
				|| !AppendCanonicalInt(Row, static_cast<uint8>(Pack.Tier))
				|| !AppendCanonicalInt(Row, static_cast<uint8>(Pack.AdmissionState))
				|| !AppendCanonicalBool(Row, Pack.bContainsExternalEffects)
				|| !AppendCanonicalSequence(Row, Pack.DependsOnPackIds)
				|| !AppendCanonicalSequence(Row, Pack.AtomicCohortIds)
				|| !AppendCanonicalSequence(Row, Pack.ToolNames)
				|| !AppendCanonicalSequence(Row, MoveTemp(RequirementRows)))
			{
				return FString();
			}
			PackRows.Add(MoveTemp(Row));
		}
		if (!AppendCanonicalSequence(Canonical, MoveTemp(PackRows)))
		{
			return FString();
		}

		TArray<FString> ToolRows;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			TArray<FString> SafetyRows;
			for (const EHyperAIStudioCapabilitySafetyClass Safety : Tool.AllowedSafetyClasses)
			{
				SafetyRows.Add(FString::Printf(TEXT("%u"), static_cast<uint32>(Safety)));
			}
			FString Row;
			if (!AppendCanonicalToken(Row, TEXT("tool"))
				|| !AppendCanonicalToken(Row, Tool.Name)
				|| !AppendCanonicalToken(Row, Tool.PackId)
				|| !AppendCanonicalToken(Row, Tool.AtomicCohortId)
				|| !AppendCanonicalSequence(Row, Tool.RequiredNonBlockingRequirementGroupIds)
				|| !AppendCanonicalSequence(Row, MoveTemp(SafetyRows))
				|| !AppendCanonicalInt(Row, static_cast<uint8>(Tool.AdmissionState))
				|| !AppendCanonicalBool(Row, Tool.bMayCauseExternalEffects)
				|| !AppendCanonicalInt(Row, Tool.SourceArtifactCount)
				|| !AppendCanonicalToken(Row, Tool.SourceArtifactFingerprint)
				|| !AppendCanonicalInt(Row, Tool.SourceLedgerRowCount)
				|| !AppendCanonicalToken(Row, Tool.SourceLedgerFingerprint)
				|| !AppendCanonicalInt(Row, static_cast<uint8>(Tool.SourceLedgerCoverage))
				|| !AppendCanonicalInt(Row, Tool.PlatformEvidenceRows)
				|| !AppendCanonicalInt(Row, Tool.ProductEvidenceRows)
				|| !AppendCanonicalInt(Row, Tool.SupplementalEvidenceRows))
			{
				return FString();
			}
			ToolRows.Add(MoveTemp(Row));
		}
		if (!AppendCanonicalSequence(Canonical, MoveTemp(ToolRows)))
		{
			return FString();
		}
		return HashUtf8Sha256(Canonical);
	}

	int32 StatusPriority(EHyperAIStudioCapabilityPackStatus Status)
	{
		switch (Status)
		{
		case EHyperAIStudioCapabilityPackStatus::Ready:
			return 0;
		case EHyperAIStudioCapabilityPackStatus::PendingAdmission:
			return 1;
		case EHyperAIStudioCapabilityPackStatus::Unavailable:
			return 2;
		case EHyperAIStudioCapabilityPackStatus::Disabled:
			return 3;
		case EHyperAIStudioCapabilityPackStatus::RestartRequired:
			return 4;
		default:
			return 5;
		}
	}

	EHyperAIStudioCapabilityPackStatus WorseStatus(
		EHyperAIStudioCapabilityPackStatus Left,
		EHyperAIStudioCapabilityPackStatus Right)
	{
		return StatusPriority(Right) > StatusPriority(Left) ? Right : Left;
	}

	EHyperAIStudioCapabilityPackStatus EvaluatePrerequisite(
		const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition,
		const FHyperAIStudioCapabilityPrerequisiteObservation* Observation)
	{
		if (!Observation)
		{
			return EHyperAIStudioCapabilityPackStatus::Unavailable;
		}
		switch (Definition.Kind)
		{
		case EHyperAIStudioCapabilityPrerequisiteKind::Plugin:
			if (!Observation->bPluginInstalled)
			{
				return EHyperAIStudioCapabilityPackStatus::Unavailable;
			}
			if (Observation->bRestartRequired)
			{
				return EHyperAIStudioCapabilityPackStatus::RestartRequired;
			}
			return Observation->bPluginEnabled
				? EHyperAIStudioCapabilityPackStatus::Ready
				: EHyperAIStudioCapabilityPackStatus::Disabled;
		case EHyperAIStudioCapabilityPrerequisiteKind::Module:
			if (Observation->bRestartRequired)
			{
				return EHyperAIStudioCapabilityPackStatus::RestartRequired;
			}
			return Observation->bModuleLoaded
				? EHyperAIStudioCapabilityPackStatus::Ready
				: EHyperAIStudioCapabilityPackStatus::Unavailable;
		case EHyperAIStudioCapabilityPrerequisiteKind::Probe:
			if (Observation->bRestartRequired)
			{
				return EHyperAIStudioCapabilityPackStatus::RestartRequired;
			}
			return Observation->bProbeSucceeded
				? EHyperAIStudioCapabilityPackStatus::Ready
				: EHyperAIStudioCapabilityPackStatus::Unavailable;
		default:
			return EHyperAIStudioCapabilityPackStatus::Unavailable;
		}
	}

	EHyperAIStudioCapabilityAdmissionState DeriveAdmission(
		const TArray<const FHyperAIStudioCapabilityToolDefinition*>& Tools)
	{
		bool bHasSourceCandidate = false;
		for (const FHyperAIStudioCapabilityToolDefinition* Tool : Tools)
		{
			if (Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Planned)
			{
				return EHyperAIStudioCapabilityAdmissionState::Planned;
			}
			bHasSourceCandidate |= Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
		}
		return bHasSourceCandidate
			? EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			: EHyperAIStudioCapabilityAdmissionState::Admitted;
	}

	FString DiagnosticForStatus(EHyperAIStudioCapabilityPackStatus Status, bool bDependency)
	{
		const TCHAR* Prefix = bDependency ? TEXT("dependency_") : TEXT("prerequisite_");
		switch (Status)
		{
		case EHyperAIStudioCapabilityPackStatus::Ready:
			return TEXT("ready");
		case EHyperAIStudioCapabilityPackStatus::Disabled:
			return FString(Prefix) + TEXT("disabled");
		case EHyperAIStudioCapabilityPackStatus::Unavailable:
			return FString(Prefix) + TEXT("unavailable");
		case EHyperAIStudioCapabilityPackStatus::RestartRequired:
			return FString(Prefix) + TEXT("restart_required");
		case EHyperAIStudioCapabilityPackStatus::PendingAdmission:
			return bDependency ? TEXT("dependency_pending_admission") : TEXT("pending_admission");
		default:
			return TEXT("unavailable");
		}
	}

	void SortUnique(TArray<FString>& Values)
	{
		Values.Sort();
		for (int32 Index = Values.Num() - 1; Index > 0; --Index)
		{
			if (Values[Index] == Values[Index - 1])
			{
				Values.RemoveAt(Index);
			}
		}
	}

	FString BuildResolutionFingerprint(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FHyperAIStudioCapabilityResolveInput& Input,
		const FHyperAIStudioCapabilityResolution& Resolution)
	{
		FString Canonical = FString::Printf(TEXT("catalog=%s\nvalid=%d\n"),
			*Catalog.GeneratedFingerprint, Resolution.bCatalogValid ? 1 : 0);
		TArray<FString> PrerequisiteIds;
		for (const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition : Catalog.Prerequisites)
		{
			PrerequisiteIds.Add(Definition.Id);
		}
		PrerequisiteIds.Sort();
		for (const FString& Id : PrerequisiteIds)
		{
			const FHyperAIStudioCapabilityPrerequisiteObservation* Observation = Input.Observations.Find(Id);
			Canonical += FString::Printf(
				TEXT("observation=%s|present=%d|installed=%d|enabled=%d|loaded=%d|restart=%d|probe=%d\n"),
				*Id,
				Observation ? 1 : 0,
				Observation && Observation->bPluginInstalled ? 1 : 0,
				Observation && Observation->bPluginEnabled ? 1 : 0,
				Observation && Observation->bModuleLoaded ? 1 : 0,
				Observation && Observation->bRestartRequired ? 1 : 0,
				Observation && Observation->bProbeSucceeded ? 1 : 0);
		}
		TArray<FString> Errors = Resolution.ValidationErrors;
		Errors.Sort();
		for (const FString& Error : Errors)
		{
			Canonical += TEXT("error=") + Error + TEXT("\n");
		}
		for (const FHyperAIStudioCapabilityPackResolution& Pack : Resolution.Packs)
		{
			TArray<FString> Blocking = Pack.BlockingPrerequisiteIds;
			TArray<FString> Limited = Pack.LimitedVariantPrerequisiteIds;
			Blocking.Sort();
			Limited.Sort();
			Canonical += FString::Printf(
				TEXT("pack=%s|status=%s|admission=%s|external=%d|diagnostic=%s|blocking=%s|limited=%s\n"),
				*Pack.PackId,
				FHyperAIStudioCapabilityPackRegistry::LexToString(Pack.Status),
				FHyperAIStudioCapabilityPackRegistry::LexToString(Pack.AdmissionState),
				Pack.bContainsExternalEffects ? 1 : 0,
				*Pack.DiagnosticCode,
				*FString::Join(Blocking, TEXT(",")),
				*FString::Join(Limited, TEXT(",")));
		}
		return Sha1(Canonical);
	}
}

const FHyperAIStudioCapabilityCatalog& FHyperAIStudioCapabilityPackRegistry::GetCatalog()
{
	static const FHyperAIStudioCapabilityCatalog Catalog = []
	{
		FHyperAIStudioCapabilityCatalog Value;
		PopulateGeneratedCapabilityPackCatalog(Value);
		return Value;
	}();
	return Catalog;
}

FString FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(
	const FHyperAIStudioCapabilityCatalog& Catalog)
{
	return HyperAIStudio::CapabilityPacks::Private::BuildCatalogFingerprint(Catalog);
}

bool FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(
	const FHyperAIStudioCapabilityCatalog& Catalog,
	TArray<FString>& OutErrors,
	int32 ExpectedToolCount,
	int32 ExpectedPackCount)
{
	using namespace HyperAIStudio::CapabilityPacks::Private;
	OutErrors.Reset();
	auto AddError = [&OutErrors](const FString& Error)
	{
		OutErrors.Add(Error);
	};

	if (Catalog.Schema.IsEmpty())
	{
		AddError(TEXT("catalog_schema_missing"));
	}
	for (const FString* Fingerprint : {
		&Catalog.GeneratedFingerprint,
		&Catalog.ApprovedPlanFingerprint,
		&Catalog.AdmissionMatrixFingerprint,
		&Catalog.SourceLedgerFingerprint,
		&Catalog.SourceArtifactFingerprint })
	{
		if (!IsSha256(*Fingerprint))
		{
			AddError(TEXT("catalog_fingerprint_invalid"));
		}
	}
	const FString RecomputedFingerprint = ComputeCatalogFingerprint(Catalog);
	if (!IsSha256(RecomputedFingerprint)
		|| RecomputedFingerprint != Catalog.GeneratedFingerprint)
	{
		AddError(TEXT("catalog_fingerprint_mismatch"));
	}
	if (ExpectedToolCount != INDEX_NONE && Catalog.Tools.Num() != ExpectedToolCount)
	{
		AddError(FString::Printf(TEXT("tool_count_mismatch:%d:%d"), Catalog.Tools.Num(), ExpectedToolCount));
	}
	if (ExpectedPackCount != INDEX_NONE && Catalog.Packs.Num() != ExpectedPackCount)
	{
		AddError(FString::Printf(TEXT("pack_count_mismatch:%d:%d"), Catalog.Packs.Num(), ExpectedPackCount));
	}

	TMap<FString, const FHyperAIStudioCapabilityPrerequisiteDefinition*> PrerequisitesById;
	for (const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition : Catalog.Prerequisites)
	{
		if (Definition.Id.IsEmpty())
		{
			AddError(TEXT("prerequisite_id_missing"));
		}
		else if (PrerequisitesById.Contains(Definition.Id))
		{
			AddError(TEXT("duplicate_prerequisite:") + Definition.Id);
		}
		else
		{
			PrerequisitesById.Add(Definition.Id, &Definition);
		}
		if (Definition.Kind != EHyperAIStudioCapabilityPrerequisiteKind::Plugin
			&& Definition.Kind != EHyperAIStudioCapabilityPrerequisiteKind::Module
			&& Definition.Kind != EHyperAIStudioCapabilityPrerequisiteKind::Probe)
		{
			AddError(TEXT("prerequisite_kind_invalid:") + Definition.Id);
		}
	}

	TMap<FString, const FHyperAIStudioCapabilityPackDefinition*> PacksById;
	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		if (Pack.Id.IsEmpty())
		{
			AddError(TEXT("pack_id_missing"));
		}
		else if (PacksById.Contains(Pack.Id))
		{
			AddError(TEXT("duplicate_pack:") + Pack.Id);
		}
		else
		{
			PacksById.Add(Pack.Id, &Pack);
		}
		if (Pack.Tier != EHyperAIStudioCapabilityPackTier::Core
			&& Pack.Tier != EHyperAIStudioCapabilityPackTier::Optional)
		{
			AddError(TEXT("pack_tier_invalid:") + Pack.Id);
		}
		if (Pack.AdmissionState != EHyperAIStudioCapabilityAdmissionState::Planned
			&& Pack.AdmissionState != EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			&& Pack.AdmissionState != EHyperAIStudioCapabilityAdmissionState::Admitted)
		{
			AddError(TEXT("pack_admission_invalid:") + Pack.Id);
		}
		TSet<FString> RequirementIds;
		for (const FHyperAIStudioCapabilityRequirementGroup& Requirement : Pack.Requirements)
		{
			if (Requirement.Id.IsEmpty() || RequirementIds.Contains(Requirement.Id))
			{
				AddError(TEXT("duplicate_or_empty_requirement:") + Pack.Id + TEXT(":") + Requirement.Id);
			}
			RequirementIds.Add(Requirement.Id);
			if (Requirement.PrerequisiteIds.IsEmpty())
			{
				AddError(TEXT("empty_requirement:") + Pack.Id + TEXT(":") + Requirement.Id);
			}
			if (Requirement.Mode != EHyperAIStudioCapabilityRequirementMode::AllOf
				&& Requirement.Mode != EHyperAIStudioCapabilityRequirementMode::AnyOf)
			{
				AddError(TEXT("requirement_mode_invalid:") + Pack.Id + TEXT(":") + Requirement.Id);
			}
			TSet<FString> LocalPrerequisites;
			for (const FString& PrerequisiteId : Requirement.PrerequisiteIds)
			{
				if (!PrerequisitesById.Contains(PrerequisiteId))
				{
					AddError(TEXT("unknown_prerequisite:") + Pack.Id + TEXT(":") + PrerequisiteId);
				}
				if (LocalPrerequisites.Contains(PrerequisiteId))
				{
					AddError(TEXT("duplicate_requirement_prerequisite:") + Pack.Id + TEXT(":") + PrerequisiteId);
				}
				LocalPrerequisites.Add(PrerequisiteId);
			}
		}
	}

	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		for (const FString& Dependency : Pack.DependsOnPackIds)
		{
			if (!PacksById.Contains(Dependency))
			{
				AddError(TEXT("unknown_pack_dependency:") + Pack.Id + TEXT(":") + Dependency);
			}
		}
	}

	TSet<FString> Visiting;
	TSet<FString> Visited;
	TFunction<void(const FString&)> Visit = [&](const FString& PackId)
	{
		if (Visiting.Contains(PackId))
		{
			AddError(TEXT("pack_dependency_cycle:") + PackId);
			return;
		}
		if (Visited.Contains(PackId))
		{
			return;
		}
		const FHyperAIStudioCapabilityPackDefinition* const* Found = PacksById.Find(PackId);
		if (!Found)
		{
			return;
		}
		Visiting.Add(PackId);
		for (const FString& Dependency : (*Found)->DependsOnPackIds)
		{
			Visit(Dependency);
		}
		Visiting.Remove(PackId);
		Visited.Add(PackId);
	};
	TArray<FString> SortedPackIds;
	PacksById.GetKeys(SortedPackIds);
	SortedPackIds.Sort();
	for (const FString& PackId : SortedPackIds)
	{
		Visit(PackId);
	}

	TMap<FString, TArray<const FHyperAIStudioCapabilityToolDefinition*>> ToolsByPack;
	TMap<FString, TSet<EHyperAIStudioCapabilityAdmissionState>> StatesByCohort;
	TMap<FString, TArray<FString>> RequirementGroupsByCohort;
	TSet<FString> ToolNames;
	int32 SourceArtifactCount = 0;
	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		if (Tool.Name.IsEmpty() || ToolNames.Contains(Tool.Name))
		{
			AddError(TEXT("duplicate_or_empty_tool:") + Tool.Name);
		}
		ToolNames.Add(Tool.Name);
		if (!PacksById.Contains(Tool.PackId))
		{
			AddError(TEXT("unknown_tool_pack:") + Tool.Name + TEXT(":") + Tool.PackId);
		}
		ToolsByPack.FindOrAdd(Tool.PackId).Add(&Tool);
		if (Tool.AtomicCohortId.IsEmpty())
		{
			AddError(TEXT("tool_cohort_missing:") + Tool.Name);
		}
		if (Tool.AdmissionState != EHyperAIStudioCapabilityAdmissionState::Planned
			&& Tool.AdmissionState != EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			&& Tool.AdmissionState != EHyperAIStudioCapabilityAdmissionState::Admitted)
		{
			AddError(TEXT("tool_admission_invalid:") + Tool.Name);
		}
		TSet<uint8> ExactSafetyClasses;
		bool bSafetyAllowsExternalEffect = false;
		if (Tool.AllowedSafetyClasses.IsEmpty())
		{
			AddError(TEXT("tool_safety_authority_missing:") + Tool.Name);
		}
		for (const EHyperAIStudioCapabilitySafetyClass Safety : Tool.AllowedSafetyClasses)
		{
			const uint8 Value = static_cast<uint8>(Safety);
			if (Value > static_cast<uint8>(EHyperAIStudioCapabilitySafetyClass::ExternalEffect))
			{
				AddError(TEXT("tool_safety_authority_invalid:") + Tool.Name);
				continue;
			}
			if (ExactSafetyClasses.Contains(Value))
			{
				AddError(TEXT("tool_safety_authority_duplicate:") + Tool.Name);
			}
			ExactSafetyClasses.Add(Value);
			bSafetyAllowsExternalEffect |=
				Safety == EHyperAIStudioCapabilitySafetyClass::ExternalEffect;
		}
		if (Tool.bMayCauseExternalEffects != bSafetyAllowsExternalEffect)
		{
			AddError(TEXT("tool_external_effect_safety_mismatch:") + Tool.Name);
		}
		StatesByCohort.FindOrAdd(Tool.AtomicCohortId).Add(Tool.AdmissionState);
		const FHyperAIStudioCapabilityPackDefinition* const* ToolPack = PacksById.Find(Tool.PackId);
		TArray<FString> ToolRequirementGroups = Tool.RequiredNonBlockingRequirementGroupIds;
		ToolRequirementGroups.Sort();
		for (int32 Index = ToolRequirementGroups.Num() - 1; Index > 0; --Index)
		{
			if (ToolRequirementGroups[Index] == ToolRequirementGroups[Index - 1])
			{
				AddError(TEXT("duplicate_tool_nonblocking_requirement:") + Tool.Name
					+ TEXT(":") + ToolRequirementGroups[Index]);
			}
		}
		if (ToolPack)
		{
			for (const FString& GroupId : ToolRequirementGroups)
			{
				const FHyperAIStudioCapabilityRequirementGroup* Group =
					(*ToolPack)->Requirements.FindByPredicate([&GroupId](const auto& Item)
					{
						return Item.Id == GroupId;
					});
				if (!Group || Group->bBlocking)
				{
					AddError(TEXT("tool_nonblocking_requirement_invalid:") + Tool.Name
						+ TEXT(":") + GroupId);
				}
			}
		}
		if (TArray<FString>* ExpectedGroups = RequirementGroupsByCohort.Find(Tool.AtomicCohortId))
		{
			if (*ExpectedGroups != ToolRequirementGroups)
			{
				AddError(TEXT("mixed_atomic_cohort_requirements:") + Tool.AtomicCohortId);
			}
		}
		else
		{
			RequirementGroupsByCohort.Add(Tool.AtomicCohortId, MoveTemp(ToolRequirementGroups));
		}
		if (Tool.SourceArtifactCount < 0 || !IsSha256(Tool.SourceArtifactFingerprint))
		{
			AddError(TEXT("tool_source_artifact_binding_invalid:") + Tool.Name);
		}
		SourceArtifactCount += FMath::Max(0, Tool.SourceArtifactCount);
		const int32 SourceRowSum = Tool.PlatformEvidenceRows + Tool.ProductEvidenceRows
			+ Tool.SupplementalEvidenceRows;
		if (Tool.SourceLedgerRowCount < 0 || SourceRowSum != Tool.SourceLedgerRowCount
			|| !IsSha256(Tool.SourceLedgerFingerprint))
		{
			AddError(TEXT("tool_source_ledger_binding_invalid:") + Tool.Name);
		}
		if ((Tool.SourceLedgerRowCount == 0
				&& Tool.SourceLedgerCoverage != EHyperAIStudioCapabilityLedgerCoverage::ApprovedPlanExtension)
			|| (Tool.SourceLedgerRowCount > 0
				&& Tool.SourceLedgerCoverage != EHyperAIStudioCapabilityLedgerCoverage::MappedSourceRows))
		{
			AddError(TEXT("tool_source_ledger_coverage_invalid:") + Tool.Name);
		}
	}
	if (SourceArtifactCount != Catalog.SourceArtifactCount)
	{
		AddError(FString::Printf(TEXT("source_artifact_count_mismatch:%d:%d"),
			SourceArtifactCount, Catalog.SourceArtifactCount));
	}
	for (const TPair<FString, TSet<EHyperAIStudioCapabilityAdmissionState>>& Pair : StatesByCohort)
	{
		if (Pair.Value.Num() != 1)
		{
			AddError(TEXT("mixed_atomic_cohort_admission:") + Pair.Key);
		}
	}

	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		const TArray<const FHyperAIStudioCapabilityToolDefinition*>* PackTools = ToolsByPack.Find(Pack.Id);
		const TArray<const FHyperAIStudioCapabilityToolDefinition*> Empty;
		const TArray<const FHyperAIStudioCapabilityToolDefinition*>& ActualTools = PackTools ? *PackTools : Empty;
		if (ActualTools.IsEmpty())
		{
			AddError(TEXT("pack_without_tools:") + Pack.Id);
		}
		TSet<FString> DeclaredNames;
		for (const FString& Name : Pack.ToolNames)
		{
			if (DeclaredNames.Contains(Name))
			{
				AddError(TEXT("duplicate_pack_tool:") + Pack.Id + TEXT(":") + Name);
			}
			DeclaredNames.Add(Name);
		}
		TSet<FString> ActualNames;
		TSet<FString> ActualCohorts;
		bool bActualExternalEffects = false;
		for (const FHyperAIStudioCapabilityToolDefinition* Tool : ActualTools)
		{
			ActualNames.Add(Tool->Name);
			ActualCohorts.Add(Tool->AtomicCohortId);
			bActualExternalEffects |= Tool->bMayCauseExternalEffects;
		}
		if (DeclaredNames.Difference(ActualNames).Num() > 0 || ActualNames.Difference(DeclaredNames).Num() > 0)
		{
			AddError(TEXT("pack_tool_membership_mismatch:") + Pack.Id);
		}
		TSet<FString> DeclaredCohorts;
		for (const FString& Cohort : Pack.AtomicCohortIds)
		{
			DeclaredCohorts.Add(Cohort);
		}
		if (DeclaredCohorts.Difference(ActualCohorts).Num() > 0
			|| ActualCohorts.Difference(DeclaredCohorts).Num() > 0)
		{
			AddError(TEXT("pack_cohort_membership_mismatch:") + Pack.Id);
		}
		if (Pack.AdmissionState != DeriveAdmission(ActualTools))
		{
			AddError(TEXT("pack_admission_mismatch:") + Pack.Id);
		}
		if (Pack.bContainsExternalEffects != bActualExternalEffects)
		{
			AddError(TEXT("pack_external_effect_mismatch:") + Pack.Id);
		}
	}

	SortUnique(OutErrors);
	return OutErrors.IsEmpty();
}

bool FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(TArray<FString>& OutErrors)
{
	return ValidateCatalog(GetCatalog(), OutErrors,
		HyperAIStudio::CapabilityCatalog::GeneratedToolCount, HyperAIStudio::CapabilityCatalog::GeneratedPackCount);
}

FHyperAIStudioCapabilityResolution FHyperAIStudioCapabilityPackRegistry::Resolve(
	const FHyperAIStudioCapabilityResolveInput& Input)
{
	return ResolveCatalog(GetCatalog(), Input);
}

FHyperAIStudioCapabilityResolution FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(
	const FHyperAIStudioCapabilityCatalog& Catalog,
	const FHyperAIStudioCapabilityResolveInput& Input)
{
	using namespace HyperAIStudio::CapabilityPacks::Private;
	FHyperAIStudioCapabilityResolution Resolution;
	Resolution.CatalogFingerprint = Catalog.GeneratedFingerprint;
	Resolution.bCatalogValid = ValidateCatalog(Catalog, Resolution.ValidationErrors);
	if (!Resolution.bCatalogValid)
	{
		Resolution.ResolutionFingerprint = BuildResolutionFingerprint(Catalog, Input, Resolution);
		return Resolution;
	}

	TMap<FString, const FHyperAIStudioCapabilityPrerequisiteDefinition*> PrerequisitesById;
	for (const FHyperAIStudioCapabilityPrerequisiteDefinition& Definition : Catalog.Prerequisites)
	{
		PrerequisitesById.Add(Definition.Id, &Definition);
	}

	TMap<FString, FHyperAIStudioCapabilityPackResolution> DirectResults;
	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		FHyperAIStudioCapabilityPackResolution PackResult;
		PackResult.PackId = Pack.Id;
		PackResult.Tier = Pack.Tier;
		PackResult.AdmissionState = Pack.AdmissionState;
		PackResult.bContainsExternalEffects = Pack.bContainsExternalEffects;
		PackResult.Status = EHyperAIStudioCapabilityPackStatus::Ready;

		for (const FHyperAIStudioCapabilityRequirementGroup& Requirement : Pack.Requirements)
		{
			EHyperAIStudioCapabilityPackStatus GroupStatus =
				Requirement.Mode == EHyperAIStudioCapabilityRequirementMode::AnyOf
				? EHyperAIStudioCapabilityPackStatus::Unavailable
				: EHyperAIStudioCapabilityPackStatus::Ready;
			bool bAnyReady = false;
			TArray<FString> FailedIds;
			for (const FString& PrerequisiteId : Requirement.PrerequisiteIds)
			{
				const FHyperAIStudioCapabilityPrerequisiteDefinition* const* Definition =
					PrerequisitesById.Find(PrerequisiteId);
				const EHyperAIStudioCapabilityPackStatus State = EvaluatePrerequisite(
					**Definition, Input.Observations.Find(PrerequisiteId));
				bAnyReady |= State == EHyperAIStudioCapabilityPackStatus::Ready;
				if (State != EHyperAIStudioCapabilityPackStatus::Ready)
				{
					FailedIds.Add(PrerequisiteId);
				}
				if (Requirement.Mode == EHyperAIStudioCapabilityRequirementMode::AllOf)
				{
					GroupStatus = WorseStatus(GroupStatus, State);
				}
				else if (!bAnyReady)
				{
					GroupStatus = WorseStatus(GroupStatus, State);
				}
			}
			if (Requirement.Mode == EHyperAIStudioCapabilityRequirementMode::AnyOf && bAnyReady)
			{
				GroupStatus = EHyperAIStudioCapabilityPackStatus::Ready;
				FailedIds.Reset();
			}
			if (GroupStatus != EHyperAIStudioCapabilityPackStatus::Ready)
			{
				if (Requirement.bBlocking)
				{
					PackResult.Status = WorseStatus(PackResult.Status, GroupStatus);
					PackResult.BlockingPrerequisiteIds.Append(FailedIds);
				}
				else
				{
					PackResult.LimitedVariantPrerequisiteIds.Append(FailedIds);
				}
			}
		}
		SortUnique(PackResult.BlockingPrerequisiteIds);
		SortUnique(PackResult.LimitedVariantPrerequisiteIds);
		DirectResults.Add(Pack.Id, MoveTemp(PackResult));
	}

	TMap<FString, FHyperAIStudioCapabilityPackResolution> Resolved;
	for (int32 Pass = 0; Pass < Catalog.Packs.Num() && Resolved.Num() < Catalog.Packs.Num(); ++Pass)
	{
		for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
		{
			if (Resolved.Contains(Pack.Id))
			{
				continue;
			}
			bool bDependenciesResolved = true;
			for (const FString& Dependency : Pack.DependsOnPackIds)
			{
				bDependenciesResolved &= Resolved.Contains(Dependency);
			}
			if (!bDependenciesResolved)
			{
				continue;
			}

			FHyperAIStudioCapabilityPackResolution PackResult = DirectResults.FindChecked(Pack.Id);
			bool bDependencyBlocked = false;
			if (PackResult.Status == EHyperAIStudioCapabilityPackStatus::Ready)
			{
				for (const FString& Dependency : Pack.DependsOnPackIds)
				{
					const EHyperAIStudioCapabilityPackStatus DependencyStatus =
						Resolved.FindChecked(Dependency).Status;
					if (DependencyStatus != EHyperAIStudioCapabilityPackStatus::Ready)
					{
						PackResult.Status = WorseStatus(PackResult.Status, DependencyStatus);
						bDependencyBlocked = true;
					}
				}
			}
			if (PackResult.Status == EHyperAIStudioCapabilityPackStatus::Ready
				&& Pack.AdmissionState != EHyperAIStudioCapabilityAdmissionState::Admitted)
			{
				PackResult.Status = EHyperAIStudioCapabilityPackStatus::PendingAdmission;
			}
			PackResult.DiagnosticCode = PackResult.Status == EHyperAIStudioCapabilityPackStatus::PendingAdmission
				? TEXT("pending_admission")
				: DiagnosticForStatus(PackResult.Status, bDependencyBlocked);
			Resolved.Add(Pack.Id, MoveTemp(PackResult));
		}
	}

	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		Resolution.Packs.Add(Resolved.FindChecked(Pack.Id));
	}
	Resolution.ResolutionFingerprint = BuildResolutionFingerprint(Catalog, Input, Resolution);
	return Resolution;
}

const TCHAR* FHyperAIStudioCapabilityPackRegistry::LexToString(EHyperAIStudioCapabilityPackStatus Status)
{
	switch (Status)
	{
	case EHyperAIStudioCapabilityPackStatus::Ready:
		return TEXT("ready");
	case EHyperAIStudioCapabilityPackStatus::Disabled:
		return TEXT("disabled");
	case EHyperAIStudioCapabilityPackStatus::Unavailable:
		return TEXT("unavailable");
	case EHyperAIStudioCapabilityPackStatus::RestartRequired:
		return TEXT("restart_required");
	case EHyperAIStudioCapabilityPackStatus::PendingAdmission:
		return TEXT("pending_admission");
	default:
		return TEXT("unavailable");
	}
}

const TCHAR* FHyperAIStudioCapabilityPackRegistry::LexToString(
	EHyperAIStudioCapabilityAdmissionState State)
{
	switch (State)
	{
	case EHyperAIStudioCapabilityAdmissionState::Planned:
		return TEXT("planned");
	case EHyperAIStudioCapabilityAdmissionState::SourceCandidate:
		return TEXT("source_candidate");
	case EHyperAIStudioCapabilityAdmissionState::Admitted:
		return TEXT("admitted");
	default:
		return TEXT("planned");
	}
}
