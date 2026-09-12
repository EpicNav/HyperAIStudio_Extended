// Games by Hyper 2026.

#include "HyperAIStudioCharacterToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioTrustedExecution.h"
#include "IO/IoHash.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioCharacter, Log, All);

namespace HyperAIStudio::Character::Private
{
	constexpr int64 EstimatedBaseReportBytes = 6144;
	constexpr int64 EstimatedRecordBytes = 1536;
	constexpr int64 EstimatedIssueBytes = 512;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += LexToString(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("|");
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("1") : TEXT("0");
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ')) return true;
		}
		return false;
	}

	bool IsBoundedString(const FString& Value, const int32 MaxCharacters)
	{
		return Value.Len() <= MaxCharacters && !HasControlCharacter(Value);
	}

	void AddIssue(
		TArray<FHyperAICharacterIssue>& Issues,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& StableId,
		const TCHAR* Detail)
	{
		if (Issues.Num() >= FHyperAIStudioCharacterContracts::MaxIssues) return;
		FHyperAICharacterIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(FHyperAIStudioCharacterContracts::MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(512);
	}

	void CopyIssuesWithinBudget(
		const TArray<FHyperAICharacterIssue>& Source,
		const int32 MaxIssues,
		const int32 MaxOutputBytes,
		TArray<FHyperAICharacterIssue>& Out,
		bool& bOutTruncated)
	{
		int64 Used = EstimatedBaseReportBytes;
		for (const FHyperAICharacterIssue& Issue : Source)
		{
			if (Out.Num() >= MaxIssues || Used + EstimatedIssueBytes > MaxOutputBytes)
			{
				bOutTruncated = true;
				continue;
			}
			Out.Add(Issue);
			Used += EstimatedIssueBytes;
		}
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	FString PackageNameForObjectPath(const FString& ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(ObjectPath);
	}

	bool CaptureExactIdentity(
		const FString& TargetPath,
		FHyperAICharacterAssetRecord& OutRecord,
		TArray<FHyperAICharacterIssue>& OutIssues)
	{
		OutRecord = {};
		OutRecord.TargetPath = TargetPath;
		OutRecord.PackageName = PackageNameForObjectPath(TargetPath);

		IAssetRegistry* Registry = IAssetRegistry::Get();
		FAssetPackageData PackageData;
		UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
		if (Registry)
		{
			PackageState = Registry->TryGetAssetPackageData(
				FName(*OutRecord.PackageName), PackageData, /*bFailIfLockHeld=*/true);
		}
		OutRecord.DiskExistence =
			FHyperAIStudioCharacterContracts::ClassifyAssetRegistryExistence(PackageState);
		if (!Registry || PackageState == UE::AssetRegistry::EExists::Unknown)
		{
			AddIssue(OutIssues, TEXT("package_state_unknown"), TEXT("error"), TargetPath,
				TEXT("The fail-fast Asset Registry package query was unavailable or lock-contended; Unknown is never absence."));
		}
		else if (PackageState == UE::AssetRegistry::EExists::Exists)
		{
			OutRecord.DiskSize = PackageData.DiskSize;
			OutRecord.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
			if (OutRecord.DiskSize <= 0 || PackageData.GetPackageSavedHash().IsZero()
				|| OutRecord.PackageSavedHash.IsEmpty())
			{
				AddIssue(OutIssues, TEXT("saved_package_identity_unavailable"), TEXT("error"),
					TargetPath, TEXT("The package has no nonzero saved hash and positive disk size identity."));
			}
		}
		else
		{
			AddIssue(OutIssues, TEXT("package_does_not_exist"), TEXT("error"), TargetPath,
				TEXT("The exact target package does not exist."));
		}

		UObject* Object = FSoftObjectPath(TargetPath).ResolveObject();
		if (!Object)
		{
			AddIssue(OutIssues, TEXT("target_not_loaded"), TEXT("error"), TargetPath,
				TEXT("The exact character object is not already loaded; this adapter never loads, opens, searches, or scans for it."));
			OutRecord.PersistedFingerprint =
				FHyperAIStudioCharacterContracts::ComputeRecordPersistedFingerprint(OutRecord);
			OutRecord.VolatileFingerprint =
				FHyperAIStudioCharacterContracts::ComputeRecordVolatileFingerprint(OutRecord);
			return false;
		}

		OutRecord.bLoaded = true;
		OutRecord.bWasLoadedFromDisk = Object->HasAnyFlags(RF_WasLoaded);
		OutRecord.ClassPath = Object->GetClass()->GetPathName();
		OutRecord.bCharacterClassMatched =
			OutRecord.ClassPath == FHyperAIStudioCharacterContracts::ExpectedCharacterClassPath;
		UPackage* Package = Object->GetOutermost();
		OutRecord.bTopLevelPrimaryObject = Package && Object->GetOuter() == Package
			&& Object->GetPathName() == TargetPath
			&& Package->GetName() == OutRecord.PackageName
			&& Package != GetTransientPackage()
			&& !Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor);
		OutRecord.bPackageDirty = Package && Package->IsDirty();

		if (!OutRecord.bCharacterClassMatched)
		{
			AddIssue(OutIssues, TEXT("character_class_mismatch"), TEXT("error"), TargetPath,
				TEXT("The exact loaded object is not /Script/MetaHumanCharacter.MetaHumanCharacter."));
		}
		if (!OutRecord.bTopLevelPrimaryObject)
		{
			AddIssue(OutIssues, TEXT("top_level_identity_mismatch"), TEXT("error"), TargetPath,
				TEXT("The loaded object is not the exact non-transient top-level primary object asserted by the request."));
		}
		if (!OutRecord.bWasLoadedFromDisk)
		{
			AddIssue(OutIssues, TEXT("disk_origin_unproven"), TEXT("error"), TargetPath,
				TEXT("RF_WasLoaded is absent, so saved package identity cannot prove this loaded object revision."));
		}
		if (OutRecord.bPackageDirty)
		{
			AddIssue(OutIssues, TEXT("dirty_character_asset"), TEXT("error"), TargetPath,
				TEXT("Dirty loaded state cannot be equated with the persisted package revision."));
		}

		OutRecord.bPersistedIdentityComplete =
			PackageState == UE::AssetRegistry::EExists::Exists
			&& OutRecord.DiskSize > 0
			&& !PackageData.GetPackageSavedHash().IsZero()
			&& !OutRecord.PackageSavedHash.IsEmpty()
			&& OutRecord.bLoaded
			&& OutRecord.bWasLoadedFromDisk
			&& !OutRecord.bPackageDirty
			&& OutRecord.bTopLevelPrimaryObject
			&& OutRecord.bCharacterClassMatched;
		// Base intentionally has no hard dependency on MetaHumanCharacter internals.
		OutRecord.bSemanticProjectionComplete = false;
		OutRecord.PersistedFingerprint =
			FHyperAIStudioCharacterContracts::ComputeRecordPersistedFingerprint(OutRecord);
		OutRecord.VolatileFingerprint =
			FHyperAIStudioCharacterContracts::ComputeRecordVolatileFingerprint(OutRecord);
		return OutRecord.bPersistedIdentityComplete;
	}

	bool IsKnownDesiredKey(const FString& Key, double& OutMin, double& OutMax)
	{
		if (Key == TEXT("body.height_cm"))
		{
			OutMin = 135.0;
			OutMax = 220.0;
			return true;
		}
		if (Key == TEXT("body.sex_blend") || Key == TEXT("body.fat")
			|| Key == TEXT("body.muscularity") || Key == TEXT("skin.lightness")
			|| Key == TEXT("skin.redness") || Key == TEXT("eyes.temperature")
			|| Key == TEXT("eyes.brightness"))
		{
			OutMin = 0.0;
			OutMax = 1.0;
			return true;
		}
		return false;
	}

	FString ComputeAuthorityFingerprint(const TArray<FHyperAICharacterAuthorityRow>& Rows)
	{
		FString Canonical(TEXT("hyperai.character.authority.v1|"));
		AppendToken(Canonical, FHyperAIStudioCharacterContracts::PythonAccessReviewId);
		AppendToken(Canonical,
			FHyperAIStudioCharacterContracts::PythonAccessReviewRecordsSha256);
		for (const FHyperAICharacterAuthorityRow& Row : Rows)
		{
			AppendToken(Canonical, Row.Source);
			AppendToken(Canonical, Row.SourceId);
			AppendToken(Canonical, Row.Lifecycle);
			AppendToken(Canonical, Row.Access);
			AppendToken(Canonical, Row.Disposition);
			AppendToken(Canonical, Row.EpicEquivalent);
			AppendToken(Canonical, Row.HyperAIContract);
			AppendToken(Canonical, Row.SourceCoordinate);
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}
}

FString FHyperAIStudioCharacterContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioCharacter.HyperAIStudioCharacterToolset");
}

const TArray<FHyperAIStudioCharacterManifestEntry>&
FHyperAIStudioCharacterContracts::GetManifest()
{
	static const TArray<FHyperAIStudioCharacterManifestEntry> Manifest = {
		{TEXT("hyper_character_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_character_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_character_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

const TArray<FHyperAICharacterAuthorityRow>&
FHyperAIStudioCharacterContracts::GetAuthorityRows()
{
	static const TArray<FHyperAICharacterAuthorityRow> Rows = {
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:135:begin_edit"), TEXT("runtime"), TEXT("external_effect"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.begin_edit"),
			FString(), TEXT("metahuman.py:135")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:149:end_edit"), TEXT("runtime"), TEXT("external_effect"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.end_edit"),
			FString(), TEXT("metahuman.py:149")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:158:get_body_shape"), TEXT("inspect"), TEXT("read"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.get_body_shape"),
			FString(), TEXT("metahuman.py:158")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:190:set_body_shape"), TEXT("edit"), TEXT("edit"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.set_body_shape"),
			FString(), TEXT("metahuman.py:190")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:234:get_skin_tone"), TEXT("inspect"), TEXT("read"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.get_skin_tone"),
			FString(), TEXT("metahuman.py:234")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:250:set_skin_tone"), TEXT("edit"), TEXT("edit"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.set_skin_tone"),
			FString(), TEXT("metahuman.py:250")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:265:get_eye_color"), TEXT("inspect"), TEXT("read"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.get_eye_color"),
			FString(), TEXT("metahuman.py:265")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:281:set_eye_color"), TEXT("edit"), TEXT("edit"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.set_eye_color"),
			FString(), TEXT("metahuman.py:281")},
		{TEXT("epic"), TEXT("MetaHumanGenerator:Content/Python/metahuman_toolset/metahuman.py:307:create"), TEXT("create"), TEXT("edit"),
			TEXT("epic_delegate"), TEXT("MetaHumanGenerator.MetaHumanToolset.create"),
			FString(), TEXT("metahuman.py:307")},
		{TEXT("hyperai_requirement"), TEXT("capability.character.loaded_identity"), TEXT("discover"),
			TEXT("unavailable"), TEXT("capability_gated"), FString(),
			TEXT("hyper_character_inspect"), TEXT("release_requirement_catalog")}};
	return Rows;
}

const TArray<FString>& FHyperAIStudioCharacterContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("MetaHumanGenerator.MetaHumanToolset.begin_edit"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.end_edit"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.get_body_shape"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.set_body_shape"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.get_skin_tone"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.set_skin_tone"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.get_eye_color"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.set_eye_color"),
		TEXT("MetaHumanGenerator.MetaHumanToolset.create")};
	return Delegates;
}

FHyperAICharacterCapabilityStatus FHyperAIStudioCharacterContracts::GetCapabilityStatus()
{
	FHyperAICharacterCapabilityStatus Status;
	Status.DelegatedEpicCallables = GetEpicDelegates();
	Status.DelegatedEpicCallableCount = Status.DelegatedEpicCallables.Num();
	Status.SupportedCases = {
		TEXT("loaded exact clean MetaHuman character package identity"),
		TEXT("persisted and volatile identity seals"),
		TEXT("detached value-only health validation")};
	Status.UnsupportedCases = {
		TEXT("Epic MetaHuman Generator create/edit/session functions are exact delegates"),
		TEXT("unloaded discovery, loading, scanning, and broad reflection"),
		TEXT("cloud generation, accounts, export, files, and NoRedist code"),
		TEXT("mutation until a hard-linked semantic adapter and trusted execution backend exist")};
	Status.AuthorityFingerprint =
		HyperAIStudio::Character::Private::ComputeAuthorityFingerprint(GetAuthorityRows());
	Status.Remediation = TEXT("Enable MetaHumanGenerator and use its nine exact Epic callables for creation and editing. Load and save the exact character asset before identity validation. HyperAI mutation remains zero-effect until a complete semantic projector and trusted backend are admitted.");
	return Status;
}

bool FHyperAIStudioCharacterContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioCharacterManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3 || GetEpicDelegates().Num() != 9
		|| GetAuthorityRows().Num() != 10) return false;
	TArray<FString> Names;
	TSet<FString> UniqueNames;
	for (const FHyperAIStudioCharacterManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| UniqueNames.Contains(Entry.Name)) return false;
		UniqueNames.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioCharacterContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	using HyperAIStudio::Character::Private::HasControlCharacter;
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("\\")) || Path.Contains(TEXT(".."))
		|| Path.Contains(TEXT(":")) || HasControlCharacter(Path)) return false;
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FSoftObjectPath SoftPath(Path);
	if (!SoftPath.IsValid() || !SoftPath.GetSubPathUtf8String().IsEmpty()) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !ObjectName.IsEmpty() && ObjectName.Len() <= MaxNameCharacters
		&& FPackageName::GetShortName(PackageName) == ObjectName;
}

bool FHyperAIStudioCharacterContracts::IsCanonicalSha256(const FString& Value)
{
	if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioCharacterContracts::IsSafeOperationId(const FString& Value)
{
	return !Value.IsEmpty()
		&& Value.Len() <= FHyperAIStudioDomainLimits::MaxOperationIdChars
		&& Value.TrimStartAndEnd() == Value
		&& !HyperAIStudio::Character::Private::HasControlCharacter(Value);
}

FString FHyperAIStudioCharacterContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	if (State == UE::AssetRegistry::EExists::Exists) return TEXT("exists");
	if (State == UE::AssetRegistry::EExists::DoesNotExist) return TEXT("does_not_exist");
	return TEXT("unknown");
}

FString FHyperAIStudioCharacterContracts::ComputeRecordPersistedFingerprint(
	const FHyperAICharacterAssetRecord& Record)
{
	using namespace HyperAIStudio::Character::Private;
	FString Canonical(TEXT("hyperai.character.asset.persisted.v1|"));
	AppendToken(Canonical, Record.TargetPath);
	AppendToken(Canonical, Record.PackageName);
	AppendToken(Canonical, Record.ClassPath);
	AppendToken(Canonical, Record.DiskExistence);
	AppendToken(Canonical, Record.PackageSavedHash);
	AppendToken(Canonical, LexToString(Record.DiskSize));
	AppendToken(Canonical, BoolToken(Record.bTopLevelPrimaryObject));
	AppendToken(Canonical, BoolToken(Record.bCharacterClassMatched));
	AppendToken(Canonical, BoolToken(Record.bPersistedIdentityComplete));
	if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioCharacterContracts::ComputeRecordVolatileFingerprint(
	const FHyperAICharacterAssetRecord& Record)
{
	using namespace HyperAIStudio::Character::Private;
	FString Canonical(TEXT("hyperai.character.asset.volatile.v1|"));
	AppendToken(Canonical, Record.TargetPath);
	AppendToken(Canonical, BoolToken(Record.bLoaded));
	AppendToken(Canonical, BoolToken(Record.bWasLoadedFromDisk));
	AppendToken(Canonical, BoolToken(Record.bPackageDirty));
	AppendToken(Canonical, BoolToken(Record.bSemanticProjectionComplete));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioCharacterContracts::ComputeRequestFingerprint(
	const TArray<FString>& SortedPaths)
{
	FString Canonical(TEXT("hyperai.character.inspect.request.v1|"));
	HyperAIStudio::Character::Private::AppendToken(Canonical, LexToString(SortedPaths.Num()));
	for (const FString& Path : SortedPaths)
	{
		HyperAIStudio::Character::Private::AppendToken(Canonical, Path);
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioCharacterContracts::ComputeSnapshotPersistedFingerprint(
	const TArray<FHyperAICharacterAssetRecord>& Records)
{
	FString Canonical(TEXT("hyperai.character.snapshot.persisted.v1|"));
	HyperAIStudio::Character::Private::AppendToken(Canonical, LexToString(Records.Num()));
	for (const FHyperAICharacterAssetRecord& Record : Records)
	{
		const FString Fingerprint = ComputeRecordPersistedFingerprint(Record);
		if (!IsCanonicalSha256(Fingerprint)) return FString();
		HyperAIStudio::Character::Private::AppendToken(Canonical, Fingerprint);
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioCharacterContracts::ComputeSnapshotVolatileFingerprint(
	const TArray<FHyperAICharacterAssetRecord>& Records)
{
	FString Canonical(TEXT("hyperai.character.snapshot.volatile.v1|"));
	HyperAIStudio::Character::Private::AppendToken(Canonical, LexToString(Records.Num()));
	for (const FHyperAICharacterAssetRecord& Record : Records)
	{
		const FString Fingerprint = ComputeRecordVolatileFingerprint(Record);
		if (!IsCanonicalSha256(Fingerprint)) return FString();
		HyperAIStudio::Character::Private::AppendToken(Canonical, Fingerprint);
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

bool FHyperAIStudioCharacterContracts::ValidateDesiredValues(
	const TArray<FHyperAICharacterDesiredValue>& Values,
	FString& OutFingerprint,
	FString& OutError)
{
	using namespace HyperAIStudio::Character::Private;
	OutFingerprint.Reset();
	OutError.Reset();
	if (Values.IsEmpty() || Values.Num() > MaxDesiredValues
		|| Values.GetAllocatedSize() > MaxContainerAllocatedBytes)
	{
		OutError = TEXT("Desired values must contain between one and eight bounded entries.");
		return false;
	}
	// Bound every nested allocation before making the sortable value copy.
	for (const FHyperAICharacterDesiredValue& Entry : Values)
	{
		double Minimum = 0.0;
		double Maximum = 0.0;
		if (!IsBoundedString(Entry.Key, 64)
			|| !IsKnownDesiredKey(Entry.Key, Minimum, Maximum)
			|| !FMath::IsFinite(Entry.Value) || Entry.Value < Minimum
			|| Entry.Value > Maximum)
		{
			OutError = TEXT("Desired values contain an unbounded, unknown, non-finite, or out-of-range entry.");
			return false;
		}
	}
	TArray<FHyperAICharacterDesiredValue> Sorted = Values;
	Sorted.Sort([](const FHyperAICharacterDesiredValue& A,
		const FHyperAICharacterDesiredValue& B) { return A.Key < B.Key; });
	FString Previous;
	FString Canonical(TEXT("hyperai.character.desired-values.v1|"));
	for (const FHyperAICharacterDesiredValue& Entry : Sorted)
	{
		double Minimum = 0.0;
		double Maximum = 0.0;
		if (!IsBoundedString(Entry.Key, 64) || Entry.Key == Previous
			|| !IsKnownDesiredKey(Entry.Key, Minimum, Maximum)
			|| !FMath::IsFinite(Entry.Value) || Entry.Value < Minimum
			|| Entry.Value > Maximum)
		{
			OutError = TEXT("Desired value keys must be unique allowlisted identifiers with finite in-range values.");
			return false;
		}
		Previous = Entry.Key;
		AppendToken(Canonical, Entry.Key);
		AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Entry.Value));
	}
	if (Canonical.Len() > MaxCanonicalCharacters)
	{
		OutError = TEXT("Desired value canonicalization exceeded its hard bound.");
		return false;
	}
	OutFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	return IsCanonicalSha256(OutFingerprint);
}

FString FHyperAIStudioCharacterContracts::BuildCursor(
	const int32 Offset,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint,
	const FString& VolatileFingerprint)
{
	return FString::Printf(TEXT("v1|%d|%s|%s|%s"), Offset, *RequestFingerprint,
		*PersistedFingerprint, *VolatileFingerprint);
}

bool FHyperAIStudioCharacterContracts::ParseCursor(
	const FString& Cursor,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint,
	const FString& VolatileFingerprint,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.IsEmpty()) return true;
	if (Cursor.Len() > MaxCursorCharacters) return false;
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT("|"), false);
	if (Parts.Num() != 5 || Parts[0] != TEXT("v1")
		|| Parts[2] != RequestFingerprint || Parts[3] != PersistedFingerprint
		|| Parts[4] != VolatileFingerprint
		|| !LexTryParseString(OutOffset, *Parts[1]) || OutOffset < 0) return false;
	return BuildCursor(OutOffset, RequestFingerprint, PersistedFingerprint,
		VolatileFingerprint) == Cursor;
}

FString FHyperAIStudioCharacterContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.inspect-request.v1|target_paths:canonical[0..16]|page_size|cursor|deadline|max_output"));
	return Value;
}

FString FHyperAIStudioCharacterContracts::ApplyPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.edit-intent.v1|target|persisted_cas|desired_values:closed[1..8]|semantic_fingerprint"));
	return Value;
}

FString FHyperAIStudioCharacterContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.validate-request.v1|detached_snapshot|issue_bound|deadline|max_output"));
	return Value;
}

FString FHyperAIStudioCharacterContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.inspect-result.v1|capability|identity_records|persisted_seal|volatile_seal|issues"));
	return Value;
}

FString FHyperAIStudioCharacterContracts::ApplyResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.apply-blocked-result.v1|prepared|zero_effect|status|hashes|effects|issues"));
	return Value;
}

FString FHyperAIStudioCharacterContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("character.validate-result.v1|valid|complete|recomputed_seals|validator|issues"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioCharacterContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.character.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// The generated MetaHumanGenerator requirement group is blocking and core-owned.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_character_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_character_apply_plan"), ApplyVariantId,
			ApplyPayloadTypeId, ApplyPayloadSchemaFingerprint(), ApplyResultTypeId,
			ApplyResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_character_validate"), ValidateVariantId,
			ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(), ValidateResultTypeId,
			ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAICharacterInspectReport FHyperAIStudioCharacterContracts::Inspect(
	const FHyperAICharacterInspectRequest& Request)
{
	using namespace HyperAIStudio::Character::Private;
	FHyperAICharacterInspectReport Report;
	Report.Capability = GetCapabilityStatus();
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Loaded character identity capture is game-thread only."));
	}
	if (Request.TargetPaths.Num() > MaxTargetPaths
		|| Request.TargetPaths.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_envelope"),
			TEXT("Target count/storage, page, cursor, deadline, or output bounds are invalid."));
	}
	if (Request.TargetPaths.IsEmpty())
	{
		if (!Request.Cursor.IsEmpty())
		{
			return Reject(TEXT("cursor_without_scope"),
				TEXT("A cursor cannot be used without exact target paths."));
		}
		Report.bOk = true;
		Report.Status = TEXT("capability_only");
		Report.Diagnostic = TEXT("Returned the frozen nine-call Epic delegation map and bounded Character adapter scope; no UObject or Asset Registry query ran.");
		return Report;
	}
	const int32 PotentialIssues = FMath::Min(MaxIssues, 4 * Request.TargetPaths.Num() + 4);
	const int64 WorstCaseBytes = EstimatedBaseReportBytes
		+ static_cast<int64>(Request.PageSize) * EstimatedRecordBytes
		+ static_cast<int64>(PotentialIssues) * EstimatedIssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The selected page cannot fit its closed worst-case output envelope."));
	}

	TSet<FString> UniquePaths;
	for (const FString& Path : Request.TargetPaths)
	{
		if (!IsCanonicalProjectObjectPath(Path) || UniquePaths.Contains(Path))
		{
			return Reject(TEXT("invalid_target_scope"),
				TEXT("Target paths must be unique canonical top-level /Game primary-object paths."));
		}
		UniquePaths.Add(Path);
	}
	TArray<FString> Paths = Request.TargetPaths;
	Paths.Sort();
	const FString RequestFingerprint = ComputeRequestFingerprint(Paths);
	if (!IsCanonicalSha256(RequestFingerprint))
	{
		return Reject(TEXT("request_seal_unavailable"),
			TEXT("The bounded target scope could not be sealed."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FHyperAICharacterAssetRecord> AllRecords;
	TArray<FHyperAICharacterIssue> CaptureIssues;
	AllRecords.Reserve(Paths.Num());
	bool bAllComplete = true;
	for (const FString& Path : Paths)
	{
		if (DeadlineExceeded(Deadline))
		{
			bAllComplete = false;
			AddIssue(CaptureIssues, TEXT("deadline_exceeded"), TEXT("error"), Path,
				TEXT("Loaded-only capture exceeded its monotonic deadline."));
			FHyperAICharacterAssetRecord& Missing = AllRecords.AddDefaulted_GetRef();
			Missing.TargetPath = Path;
			Missing.PackageName = PackageNameForObjectPath(Path);
			Missing.PersistedFingerprint = ComputeRecordPersistedFingerprint(Missing);
			Missing.VolatileFingerprint = ComputeRecordVolatileFingerprint(Missing);
			continue;
		}
		FHyperAICharacterAssetRecord& Record = AllRecords.AddDefaulted_GetRef();
		if (!CaptureExactIdentity(Path, Record, CaptureIssues)) bAllComplete = false;
	}
	AllRecords.Sort([](const FHyperAICharacterAssetRecord& A,
		const FHyperAICharacterAssetRecord& B) { return A.TargetPath < B.TargetPath; });
	const FString PersistedFingerprint = ComputeSnapshotPersistedFingerprint(AllRecords);
	const FString VolatileFingerprint = ComputeSnapshotVolatileFingerprint(AllRecords);
	if (!IsCanonicalSha256(PersistedFingerprint) || !IsCanonicalSha256(VolatileFingerprint))
	{
		return Reject(TEXT("snapshot_seal_unavailable"),
			TEXT("The bounded identity projection could not be sealed."));
	}

	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, RequestFingerprint, PersistedFingerprint,
		VolatileFingerprint, Offset) || Offset >= AllRecords.Num())
	{
		return Reject(TEXT("cursor_invalid_or_stale"),
			TEXT("The cursor is malformed, stale, or outside the exact snapshot."));
	}
	const int32 End = FMath::Min(AllRecords.Num(), Offset + Request.PageSize);
	Report.Snapshot.RequestFingerprint = RequestFingerprint;
	Report.Snapshot.PersistedFingerprint = PersistedFingerprint;
	Report.Snapshot.VolatileFingerprint = VolatileFingerprint;
	Report.Snapshot.TotalRecords = AllRecords.Num();
	Report.Snapshot.Records.Reserve(End - Offset);
	for (int32 Index = Offset; Index < End; ++Index)
	{
		Report.Snapshot.Records.Add(AllRecords[Index]);
	}
	const bool bFullPage = Offset == 0 && End == AllRecords.Num();
	Report.Snapshot.bIdentityProjectionComplete = bAllComplete && bFullPage;
	Report.Snapshot.bSemanticProjectionComplete = false;
	if (End < AllRecords.Num())
	{
		Report.bTruncated = true;
		Report.NextCursor = BuildCursor(End, RequestFingerprint,
			PersistedFingerprint, VolatileFingerprint);
	}
	CopyIssuesWithinBudget(CaptureIssues, MaxIssues, Request.MaxOutputBytes,
		Report.Issues, Report.bTruncated);
	Report.bOk = true;
	Report.Status = Report.Snapshot.bIdentityProjectionComplete
		? TEXT("identity_complete") : TEXT("identity_partial");
	Report.Diagnostic = Report.Snapshot.bIdentityProjectionComplete
		? TEXT("Exact loaded clean MetaHuman character identity and persisted/volatile seals are complete; semantic generator state remains delegated.")
		: TEXT("Returned bounded identity evidence, but unloaded/wrong-class/dirty/unknown/paged state prevents a complete detached identity assertion.");
	return Report;
}

FHyperAICharacterValidateReport FHyperAIStudioCharacterContracts::Validate(
	const FHyperAICharacterValidateRequest& Request)
{
	using namespace HyperAIStudio::Character::Private;
	FHyperAICharacterValidateReport Report;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Snapshot.TotalRecords < 1
		|| Request.Snapshot.TotalRecords > MaxTargetPaths
		|| Request.Snapshot.Records.Num() != Request.Snapshot.TotalRecords
		|| Request.Snapshot.Records.GetAllocatedSize() > MaxContainerAllocatedBytes)
	{
		return Reject(TEXT("invalid_detached_envelope"),
			TEXT("Validation requires one complete bounded unpaged detached snapshot."));
	}
	const int64 WorstCaseBytes = EstimatedBaseReportBytes
		+ static_cast<int64>(Request.MaxIssues) * EstimatedIssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The validator issue cap cannot fit the selected output envelope."));
	}
	for (const FHyperAICharacterAssetRecord& Record : Request.Snapshot.Records)
	{
		if (!IsBoundedString(Record.TargetPath, MaxPathCharacters)
			|| !IsBoundedString(Record.PackageName, MaxPathCharacters)
			|| !IsBoundedString(Record.ClassPath, MaxPathCharacters)
			|| !IsBoundedString(Record.DiskExistence, 32)
			|| !IsBoundedString(Record.PackageSavedHash, MaxNameCharacters)
			|| Record.PersistedFingerprint.Len() > 71
			|| Record.VolatileFingerprint.Len() > 71)
		{
			return Reject(TEXT("detached_string_bound_exceeded"),
				TEXT("A detached record exceeded a nested string bound before validator materialization."));
		}
	}

	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FString> Paths;
	Paths.Reserve(Request.Snapshot.Records.Num());
	TSet<FString> UniquePaths;
	bool bEnvelopeComplete = Request.Snapshot.bIdentityProjectionComplete
		&& !Request.Snapshot.bSemanticProjectionComplete;
	int64 UsedBytes = EstimatedBaseReportBytes;
	auto ValidationIssue = [&](const TCHAR* Code, const TCHAR* Severity,
		const FString& StableId, const TCHAR* Detail)
	{
		if (FCString::Strcmp(Severity, TEXT("error")) == 0) ++Report.ErrorCount;
		else ++Report.WarningCount;
		if (Report.Issues.Num() >= Request.MaxIssues
			|| UsedBytes + EstimatedIssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			return;
		}
		FHyperAICharacterIssue& Issue = Report.Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(512);
		UsedBytes += EstimatedIssueBytes;
	};

	FString PreviousPath;
	for (const FHyperAICharacterAssetRecord& Record : Request.Snapshot.Records)
	{
		if (DeadlineExceeded(Deadline))
		{
			bEnvelopeComplete = false;
			ValidationIssue(TEXT("deadline_exceeded"), TEXT("error"), Record.TargetPath,
				TEXT("Detached validation exceeded its monotonic deadline."));
			break;
		}
		const FString ExpectedPackage = PackageNameForObjectPath(Record.TargetPath);
		if (!IsCanonicalProjectObjectPath(Record.TargetPath)
			|| Record.TargetPath <= PreviousPath || UniquePaths.Contains(Record.TargetPath)
			|| Record.PackageName != ExpectedPackage
			|| !IsBoundedString(Record.ClassPath, MaxPathCharacters)
			|| !IsBoundedString(Record.PackageSavedHash, MaxNameCharacters))
		{
			bEnvelopeComplete = false;
			ValidationIssue(TEXT("record_identity_invalid"), TEXT("error"), Record.TargetPath,
				TEXT("Record ordering, uniqueness, path, package, class, or saved-hash shape is invalid."));
		}
		PreviousPath = Record.TargetPath;
		UniquePaths.Add(Record.TargetPath);
		Paths.Add(Record.TargetPath);
		if (Record.ClassPath != ExpectedCharacterClassPath
			|| Record.DiskExistence != TEXT("exists")
			|| Record.DiskSize <= 0 || Record.PackageSavedHash.IsEmpty()
			|| !Record.bLoaded || !Record.bWasLoadedFromDisk || Record.bPackageDirty
			|| !Record.bTopLevelPrimaryObject || !Record.bCharacterClassMatched
			|| !Record.bPersistedIdentityComplete || Record.bSemanticProjectionComplete)
		{
			bEnvelopeComplete = false;
			ValidationIssue(TEXT("character_identity_health_invalid"), TEXT("error"),
				Record.TargetPath,
				TEXT("A complete identity requires exact class, saved package evidence, loaded disk origin, clean top-level state, and no semantic-completeness claim."));
		}
		if (Record.PersistedFingerprint != ComputeRecordPersistedFingerprint(Record)
			|| Record.VolatileFingerprint != ComputeRecordVolatileFingerprint(Record))
		{
			bEnvelopeComplete = false;
			ValidationIssue(TEXT("record_seal_mismatch"), TEXT("error"), Record.TargetPath,
				TEXT("A detached per-record persisted or volatile seal drifted."));
		}
	}

	Report.RecomputedRequestFingerprint = ComputeRequestFingerprint(Paths);
	Report.RecomputedPersistedFingerprint =
		ComputeSnapshotPersistedFingerprint(Request.Snapshot.Records);
	Report.RecomputedVolatileFingerprint =
		ComputeSnapshotVolatileFingerprint(Request.Snapshot.Records);
	if (Report.RecomputedRequestFingerprint != Request.Snapshot.RequestFingerprint
		|| Report.RecomputedPersistedFingerprint != Request.Snapshot.PersistedFingerprint
		|| Report.RecomputedVolatileFingerprint != Request.Snapshot.VolatileFingerprint)
	{
		bEnvelopeComplete = false;
		ValidationIssue(TEXT("snapshot_seal_mismatch"), TEXT("error"), FString(),
			TEXT("Detached request, persisted, or volatile snapshot seal drifted."));
	}
	FString ValidatorCanonical(TEXT("hyperai.character.validator.v1|"));
	AppendToken(ValidatorCanonical, Report.RecomputedRequestFingerprint);
	AppendToken(ValidatorCanonical, Report.RecomputedPersistedFingerprint);
	AppendToken(ValidatorCanonical, Report.RecomputedVolatileFingerprint);
	AppendToken(ValidatorCanonical, LexToString(Report.ErrorCount));
	AppendToken(ValidatorCanonical, LexToString(Report.WarningCount));
	Report.ValidatorFingerprint =
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ValidatorCanonical);
	Report.bOk = true;
	Report.bComplete = bEnvelopeComplete && !Report.bTruncated;
	Report.bValid = Report.bComplete && Report.ErrorCount == 0;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("The detached value-only character identity snapshot is internally complete and all seals match.")
		: TEXT("The detached character identity snapshot is partial, inconsistent, unhealthy, tampered, or output-truncated.");
	return Report;
}

FString FHyperAIStudioCharacterContracts::ComputePlanSemanticFingerprint(
	const FHyperAIStudioCharacterPlanPayload& Payload)
{
	using namespace HyperAIStudio::Character::Private;
	FString Canonical(TEXT("hyperai.character.edit-intent.semantic.v1|"));
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BasePersistedFingerprint);
	AppendToken(Canonical, Payload.DesiredValueFingerprint);
	TArray<FHyperAICharacterDesiredValue> Sorted = Payload.DesiredValues;
	Sorted.Sort([](const FHyperAICharacterDesiredValue& A,
		const FHyperAICharacterDesiredValue& B) { return A.Key < B.Key; });
	for (const FHyperAICharacterDesiredValue& Entry : Sorted)
	{
		AppendToken(Canonical, Entry.Key);
		AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Entry.Value));
	}
	if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAICharacterApplyPlanReport FHyperAIStudioCharacterContracts::BuildPlan(
	const FHyperAICharacterApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Character::Private;
	FHyperAICharacterApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	Report.VariantId = ApplyVariantId;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.Effects.PhysicalEffectCount = 0;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Character edit-intent preflight is game-thread only."));
	}
	if (!IsSafeOperationId(Request.OperationId)
		|| !IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedFingerprint)
		|| Request.DesiredValues.Num() > MaxDesiredValues
		|| Request.DesiredValues.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_envelope"),
			TEXT("Operation, target, persisted assertion, desired-value storage, deadline, or output bounds are invalid."));
	}
	if ((Request.bDryRun && !Request.ExpectedPlanHash.IsEmpty())
		|| (!Request.bDryRun && !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_plan_hash_contract"),
			TEXT("Dry-run takes no expected plan hash; non-dry intent must echo one canonical hash."));
	}
	FString DesiredFingerprint;
	FString DesiredError;
	if (!ValidateDesiredValues(Request.DesiredValues, DesiredFingerprint, DesiredError))
	{
		return Reject(TEXT("invalid_desired_value_model"), *DesiredError);
	}
	Report.DesiredValueFingerprint = DesiredFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.DesiredValueCount = Request.DesiredValues.Num();
	Report.Effects.bIntentValueModelValid = true;

	// Stable zero-effect terminal for every non-dry request. Do not even resolve a UObject.
	if (!Request.bDryRun)
	{
		return Reject(NonDryCallableState,
			TEXT("No effect ran. Epic owns character create/edit/session calls; HyperAI has no admitted journaled semantic mutation backend and did not begin a session, mutate, save, stage, submit, export, or invoke cloud behavior."));
	}

	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	FHyperAICharacterAssetRecord Record;
	TArray<FHyperAICharacterIssue> CaptureIssues;
	const bool bIdentityComplete = CaptureExactIdentity(
		Request.TargetPath, Record, CaptureIssues);
	bool bIssuesTruncated = false;
	CopyIssuesWithinBudget(CaptureIssues, MaxIssues, Request.MaxOutputBytes,
		Report.Issues, bIssuesTruncated);
	Report.bStaged = false;
	if (bIssuesTruncated)
	{
		return Reject(TEXT("issue_output_bound"),
			TEXT("Identity preflight diagnostics could not fit the closed output envelope."));
	}
	if (!bIdentityComplete)
	{
		return Reject(TEXT("persisted_identity_incomplete"),
			TEXT("Pure preflight requires one exact already-loaded, clean, disk-backed MetaHuman character identity."));
	}
	Report.BasePersistedFingerprint = Record.PersistedFingerprint;
	if (Record.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("persisted_identity_cas_mismatch"),
			TEXT("The fresh exact character identity seal differs from the caller assertion."));
	}
	if (DeadlineExceeded(Deadline))
	{
		return Reject(TEXT("preflight_deadline_exceeded"),
			TEXT("Loaded-only identity preflight exceeded its monotonic deadline."));
	}
	Report.Effects.bLoadedSemanticProjectionComplete =
		Record.bSemanticProjectionComplete;
	if (!Record.bSemanticProjectionComplete)
	{
		return Reject(SemanticProjectionBlocker,
			TEXT("No effect ran and no valid dry-run was claimed. Clean package identity is not complete body/skin/eye semantic state; a hard-linked MetaHuman semantic projector is required before pure Prepare may seal an edit plan."));
	}

	// Future specialized adapters may reach this pure branch only after projecting a
	// complete semantic base. The base module never sets that completeness bit.
	const TSharedRef<FHyperAIStudioCharacterPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioCharacterPlanPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedFingerprint = Record.PersistedFingerprint;
	Payload->DesiredValues = Request.DesiredValues;
	Payload->DesiredValueFingerprint = DesiredFingerprint;
	Payload->SemanticFingerprint = ComputePlanSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || !IsCanonicalSha256(Payload->SemanticFingerprint)
		|| Clone->GetSemanticFingerprint() != Payload->SemanticFingerprint
		|| Clone->GetBoundedByteSize() != Payload->GetBoundedByteSize())
	{
		return Reject(TEXT("immutable_payload_seal_failed"),
			TEXT("The closed detached character edit intent could not be sealed independently."));
	}

	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_character_apply_plan");
	Binding.VariantId = ApplyVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	Binding.ExpectedAdapterFingerprint = GetAdapterDescriptor().AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = false;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = MoveTemp(Binding);
	Contract.ArtifactTypeId = Clone->GetTypeId();
	Contract.ArtifactSchemaFingerprint = Clone->GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Clone->GetSemanticFingerprint();
	Contract.EffectTarget = Request.TargetPath;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = Request.DesiredValues.Num() + 4;
	Contract.MaxGameThreadMs = FMath::Min(Request.DeadlineMs, 200);
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), *PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.bOk = true;
	Report.Status = TEXT("dry_run_valid_zero_effect");
	Report.Diagnostic = TEXT("Pure Prepare sealed one complete detached semantic edit intent. No Epic session, mutation, save, stage, submission, file, export, or cloud effect ran.");
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.bWouldBeginEpicEditSessionOnce = true;
	Report.Effects.bWouldEndEpicEditSessionOnce = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	return Report;
}

FString FHyperAIStudioCharacterInspectPayload::GetTypeId() const
{
	return FHyperAIStudioCharacterContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioCharacterInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCharacterContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioCharacterInspectPayload::GetBoundedByteSize() const
{
	int64 Size = 128ll + 2ll * Request.Cursor.Len();
	for (const FString& Path : Request.TargetPaths) Size += 32ll + 2ll * Path.Len();
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

FString FHyperAIStudioCharacterValidatePayload::GetTypeId() const
{
	return FHyperAIStudioCharacterContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioCharacterValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCharacterContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioCharacterValidatePayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (Request.Snapshot.RequestFingerprint.Len()
		+ Request.Snapshot.PersistedFingerprint.Len()
		+ Request.Snapshot.VolatileFingerprint.Len());
	for (const FHyperAICharacterAssetRecord& Record : Request.Snapshot.Records)
	{
		Size += 512ll + 2ll * (Record.TargetPath.Len() + Record.PackageName.Len()
			+ Record.ClassPath.Len() + Record.PackageSavedHash.Len());
	}
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

FString FHyperAIStudioCharacterPlanPayload::GetTypeId() const
{
	return FHyperAIStudioCharacterContracts::ApplyPayloadTypeId;
}

FString FHyperAIStudioCharacterPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCharacterContracts::ApplyPayloadSchemaFingerprint();
}

int32 FHyperAIStudioCharacterPlanPayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (TargetPath.Len() + BasePersistedFingerprint.Len()
		+ DesiredValueFingerprint.Len() + SemanticFingerprint.Len());
	for (const FHyperAICharacterDesiredValue& Entry : DesiredValues)
	{
		Size += 64ll + 2ll * Entry.Key.Len();
	}
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioCharacterPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioCharacterPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioCharacterPlanPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->DesiredValues = DesiredValues;
	Clone->DesiredValueFingerprint = DesiredValueFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioCharacterInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioCharacterContracts::InspectResultTypeId;
}

FString FHyperAIStudioCharacterInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCharacterContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioCharacterInspectResultPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 4096
		+ Report.Snapshot.Records.Num() * static_cast<int32>(
			HyperAIStudio::Character::Private::EstimatedRecordBytes)
		+ Report.Issues.Num() * static_cast<int32>(
			HyperAIStudio::Character::Private::EstimatedIssueBytes));
}

FString FHyperAIStudioCharacterValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioCharacterContracts::ValidateResultTypeId;
}

FString FHyperAIStudioCharacterValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCharacterContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioCharacterValidateResultPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 4096
		+ Report.Issues.Num() * static_cast<int32>(
			HyperAIStudio::Character::Private::EstimatedIssueBytes));
}

FHyperAICharacterInspectReport UHyperAIStudioCharacterToolset::hyper_character_inspect(
	const FHyperAICharacterInspectRequest& Request)
{
	return FHyperAIStudioCharacterContracts::Inspect(Request);
}

FHyperAICharacterApplyPlanReport UHyperAIStudioCharacterToolset::hyper_character_apply_plan(
	const FHyperAICharacterApplyPlanRequest& Request)
{
	return FHyperAIStudioCharacterContracts::BuildPlan(Request);
}

FHyperAICharacterValidateReport UHyperAIStudioCharacterToolset::hyper_character_validate(
	const FHyperAICharacterValidateRequest& Request)
{
	return FHyperAIStudioCharacterContracts::Validate(Request);
}

FHyperAIStudioCharacterDomainAdapter::FHyperAIStudioCharacterDomainAdapter()
	: Descriptor(FHyperAIStudioCharacterContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioCharacterDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioCharacterDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&Result](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Character adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_character_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioCharacterContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioCharacterContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCharacterContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioCharacterInspectPayload& Typed =
			static_cast<const FHyperAIStudioCharacterInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioCharacterInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioCharacterInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioCharacterContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_character_validate")
		&& Context.Binding.VariantId == FHyperAIStudioCharacterContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioCharacterContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCharacterContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioCharacterValidatePayload& Typed =
			static_cast<const FHyperAIStudioCharacterValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioCharacterValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioCharacterValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioCharacterContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_character_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioCharacterContracts::ApplyVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioCharacterContracts::ApplyPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCharacterContracts::ApplyPayloadSchemaFingerprint())
	{
		const FHyperAIStudioCharacterPlanPayload& Typed =
			static_cast<const FHyperAIStudioCharacterPlanPayload&>(Payload);
		FString DesiredFingerprint;
		FString DesiredError;
		if (!FHyperAIStudioCharacterContracts::ValidateDesiredValues(
			Typed.DesiredValues, DesiredFingerprint, DesiredError)
			|| DesiredFingerprint != Typed.DesiredValueFingerprint
			|| FHyperAIStudioCharacterContracts::ComputePlanSemanticFingerprint(Typed)
				!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Character intent value model or semantic seal drifted."));
		}
		return Reject(FHyperAIStudioCharacterContracts::NonDryCallableState,
			TEXT("No effect ran. Epic MetaHuman create/edit/session operations remain exact delegates and this synchronous adapter has no admitted mutation backend."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Character adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioCharacterRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioCharacterRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioCharacterRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioCharacterRegistration::IsRegistered() const
{
	return FHyperAIStudioCharacterContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioCharacterToolset::StaticClass(),
			FHyperAIStudioCharacterContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioCharacterRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid();
}

void FHyperAIStudioCharacterRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable()) return;
	if (!FHyperAIStudioCharacterContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioCharacter, Verbose,
			TEXT("Character exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioCharacterDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioCharacter, Error,
			TEXT("Character adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioCharacterToolset::StaticClass(),
		FHyperAIStudioCharacterContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioCharacter, Error,
			TEXT("Character atomic three-tool registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioCharacterRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioCharacterToolset::StaticClass(),
			FHyperAIStudioCharacterContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioCharacter, Error,
				TEXT("Character owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioCharacter, Error,
				TEXT("Character adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
