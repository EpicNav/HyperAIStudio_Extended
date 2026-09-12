// Games by Hyper 2026.

#include "HyperAIStudioLiveProductionToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLiveProduction, Log, All);

namespace HyperAIStudio::LiveProduction::Private
{
	constexpr int64 EstimatedBaseReportBytes = 12288;
	// Closed serialized-output upper estimates, including field/JSON overhead and
	// pessimistic multi-byte text expansion for every bounded string field.
	constexpr int64 EstimatedRecordBytes = 18432;
	constexpr int64 EstimatedIssueBytes = 6144;

	int64 SaturatingAdd(const int64 A, const int64 B)
	{
		return A > MAX_int64 - FMath::Max<int64>(0, B)
			? MAX_int64 : A + FMath::Max<int64>(0, B);
	}

	int64 EstimateStringBytes(const FString& Value)
	{
		return SaturatingAdd(64, static_cast<int64>(Value.Len()) * 6);
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (FChar::IsControl(Character)) return true;
		}
		return false;
	}

	bool AppendToken(FString& Canonical, const FString& Token)
	{
		const int64 Added = static_cast<int64>(Token.Len()) + 24;
		if (Canonical.Len() > FHyperAIStudioLiveProductionContracts::MaxCanonicalCharacters
			- Added)
		{
			return false;
		}
		Canonical += FString::FromInt(Token.Len());
		Canonical += TEXT(":");
		Canonical += Token;
		Canonical += TEXT("|");
		return true;
	}

	FString HashCanonical(const FString& Canonical)
	{
		if (Canonical.IsEmpty()
			|| Canonical.Len() > FHyperAIStudioLiveProductionContracts::MaxCanonicalCharacters)
		{
			return {};
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool IsSafeName(const FString& Value)
	{
		if (Value.IsEmpty()
			|| Value.Len() > FHyperAIStudioLiveProductionContracts::MaxNameCharacters
			|| HasControlCharacter(Value))
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

	bool IsSafeHostAddress(const FString& Value)
	{
		if (Value.IsEmpty()
			|| Value.Len() > FHyperAIStudioLiveProductionContracts::MaxHostCharacters
			|| HasControlCharacter(Value))
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('.')
				|| Character == TEXT('-') || Character == TEXT(':')
				|| Character == TEXT('[') || Character == TEXT(']')))
			{
				return false;
			}
		}
		return true;
	}

	void AddIssue(TArray<FHyperAILiveProductionIssue>& Issues,
		const TCHAR* Code, const TCHAR* Severity, const FString& StableId,
		const FString& Detail)
	{
		if (Issues.Num() >= FHyperAIStudioLiveProductionContracts::MaxIssues) return;
		FHyperAILiveProductionIssue Issue;
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(
			FHyperAIStudioLiveProductionContracts::MaxPathCharacters);
		Issue.Detail = Detail.Left(1024);
		Issues.Add(MoveTemp(Issue));
	}

	int32 CountIssuesBySeverity(const TArray<FHyperAILiveProductionIssue>& Issues,
		const TCHAR* Severity)
	{
		int32 Count = 0;
		for (const FHyperAILiveProductionIssue& Issue : Issues)
		{
			if (Issue.Severity == Severity) ++Count;
		}
		return Count;
	}

	int32 CountErrors(const TArray<FHyperAILiveProductionIssue>& Issues)
	{
		return CountIssuesBySeverity(Issues, TEXT("error"));
	}

	FString BuildRequestFingerprint(const FString& FamilyId,
		const TArray<FString>& SortedPaths, const int32 PageSize)
	{
		FString Canonical;
		if (!AppendToken(Canonical, TEXT("hyperai.live-production.inspect-request.v1"))
			|| !AppendToken(Canonical, FamilyId)
			|| !AppendToken(Canonical, FString::FromInt(PageSize)))
		{
			return {};
		}
		for (const FString& Path : SortedPaths)
		{
			if (!AppendToken(Canonical, Path)) return {};
		}
		return HashCanonical(Canonical);
	}

	bool CaptureExactIdentity(const FString& FamilyId, const FString& TargetPath,
		FHyperAILiveProductionObjectRecord& OutRecord, FString& OutStatus,
		FString& OutDiagnostic)
	{
		OutRecord = {};
		OutStatus.Reset();
		OutDiagnostic.Reset();
		auto Fail = [&](const TCHAR* Status, const TCHAR* Diagnostic)
		{
			OutStatus = Status;
			OutDiagnostic = Diagnostic;
			return false;
		};
		if (!IsInGameThread())
		{
			return Fail(TEXT("game_thread_required"),
				TEXT("Exact loaded-object capture is game-thread only."));
		}
		if (!FHyperAIStudioLiveProductionContracts::IsKnownFamilyId(FamilyId)
			|| !FHyperAIStudioLiveProductionContracts::IsCanonicalProjectObjectPath(TargetPath))
		{
			return Fail(TEXT("invalid_capture_identity"),
				TEXT("Capture requires one known family and one canonical top-level /Game object path."));
		}

		UObject* Object = FSoftObjectPath(TargetPath).ResolveObject();
		if (!Object)
		{
			return Fail(TEXT("target_not_loaded"),
				TEXT("The exact target is not already loaded; this pack never loads, searches, opens, or scans for it."));
		}
		if (Object->GetPathName() != TargetPath)
		{
			return Fail(TEXT("resolved_identity_mismatch"),
				TEXT("The loaded object did not resolve to the exact asserted primary-object path."));
		}
		UPackage* Package = Object->GetOutermost();
		if (!Package || Package == GetTransientPackage()
			|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor))
		{
			return Fail(TEXT("unsupported_target_package"),
				TEXT("Transient and PIE objects are outside persisted live-production identity evidence."));
		}

		OutRecord.FamilyId = FamilyId;
		OutRecord.TargetPath = TargetPath;
		OutRecord.PackageName = Package->GetName();
		OutRecord.ClassPath = Object->GetClass()->GetPathName();
		OutRecord.bLoaded = true;
		OutRecord.bWasLoadedFromDisk = Object->HasAnyFlags(RF_WasLoaded);
		OutRecord.bPackageDirty = Package->IsDirty();
		OutRecord.bFamilyClassMatched =
			FHyperAIStudioLiveProductionContracts::DoesClassMatchFamily(
				OutRecord.ClassPath, FamilyId);

		IAssetRegistry* Registry = IAssetRegistry::Get();
		FAssetPackageData PackageData;
		UE::AssetRegistry::EExists State = UE::AssetRegistry::EExists::Unknown;
		if (Registry)
		{
			State = Registry->TryGetAssetPackageData(
				Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
		}
		OutRecord.DiskExistence =
			FHyperAIStudioLiveProductionContracts::ClassifyAssetRegistryExistence(State);
		if (State == UE::AssetRegistry::EExists::Exists)
		{
			OutRecord.DiskSize = PackageData.DiskSize;
				OutRecord.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
		}
		OutRecord.bPersistedIdentityComplete = Registry
			&& State == UE::AssetRegistry::EExists::Exists
			&& OutRecord.bWasLoadedFromDisk && !OutRecord.bPackageDirty
			&& OutRecord.DiskSize > 0 && !PackageData.GetPackageSavedHash().IsZero()
			&& !OutRecord.PackageSavedHash.IsEmpty();
		// A class namespace and package identity are deliberately not a complete
		// LiveLink/DMX/RemoteControl/Avalanche/nDisplay semantic projection.
		OutRecord.bSemanticProjectionComplete = false;
		OutRecord.PersistedFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeObjectPersistedFingerprint(
				OutRecord);
		OutRecord.VolatileFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeObjectVolatileFingerprint(
				OutRecord);
		return true;
	}

	void CopyIssuesWithinBudget(const TArray<FHyperAILiveProductionIssue>& Source,
		const int32 MaxIssues, const int32 MaxOutputBytes,
		TArray<FHyperAILiveProductionIssue>& OutIssues, bool& bOutTruncated)
	{
		int64 Bytes = EstimatedBaseReportBytes;
		for (const FHyperAILiveProductionIssue& Issue : Source)
		{
			const int64 Cost = EstimatedIssueBytes
				+ EstimateStringBytes(Issue.Code) + EstimateStringBytes(Issue.Severity)
				+ EstimateStringBytes(Issue.StableId) + EstimateStringBytes(Issue.Detail);
			if (OutIssues.Num() >= MaxIssues || SaturatingAdd(Bytes, Cost) > MaxOutputBytes)
			{
				bOutTruncated = true;
				break;
			}
			Bytes = SaturatingAdd(Bytes, Cost);
			OutIssues.Add(Issue);
		}
	}
}

FString FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioLiveProduction.HyperAIStudioLiveProductionToolset");
}

const TArray<FHyperAIStudioLiveProductionManifestEntry>&
FHyperAIStudioLiveProductionContracts::GetManifest()
{
	static const TArray<FHyperAIStudioLiveProductionManifestEntry> Manifest = {
		{TEXT("hyper_live_production_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_live_production_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_live_production_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioLiveProductionContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioLiveProductionManifestEntry& Entry : GetManifest())
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name))
		{
			return false;
		}
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return Names.Num() == 3
		&& FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
			PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

const TArray<FHyperAILiveProductionAuthorityRow>&
FHyperAIStudioLiveProductionContracts::GetEpicReviewAuthority()
{
	static const TArray<FHyperAILiveProductionAuthorityRow> Rows = {
		{TEXT("epic_native_access_review"), TEXT("epic"), NativeAccessReviewId,
			TEXT("review"), TEXT("reviewed"), TEXT("epic_delegate"), TEXT(""),
			TEXT(""), 278, 0, NativeAccessReviewRecordsSha256,
			TEXT("Plan/CapabilityUnion/epic_native_access_review.json"),
			TEXT("no_dedicated_live_production_callable")},
		{TEXT("epic_python_access_review"), TEXT("epic"), PythonAccessReviewId,
			TEXT("review"), TEXT("reviewed"), TEXT("epic_delegate"), TEXT(""),
			TEXT(""), 598, 0, PythonAccessReviewRecordsSha256,
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"),
			TEXT("no_dedicated_live_production_callable")}};
	return Rows;
}

const TArray<FHyperAILiveProductionAuthorityRow>&
FHyperAIStudioLiveProductionContracts::GetPluginDescriptorAuthority()
{
	static const TArray<FHyperAILiveProductionAuthorityRow> Rows = {
		{TEXT("ue58_plugin_descriptor"), TEXT("epic"), TEXT("plugin.LiveLink"),
			TEXT("prerequisite"), TEXT("read"), TEXT("capability_gated"), TEXT(""),
			TEXT("LiveLink"), 0, 0,
			TEXT("sha256:33fd6909681bdbcbca55f03bd29ad39e88f0be74885ddf02d64402a91f8f4fe4"),
			TEXT("Engine/Plugins/Animation/LiveLink/LiveLink.uplugin"), TEXT("descriptor_authority")},
		{TEXT("ue58_plugin_descriptor"), TEXT("epic"), TEXT("plugin.RemoteControl"),
			TEXT("prerequisite"), TEXT("read"), TEXT("capability_gated"), TEXT(""),
			TEXT("RemoteControl"), 0, 0,
			TEXT("sha256:17c2d9744d8e269e5d0d7762251bab4475096fca486bf049f5e997136e60bf74"),
			TEXT("Engine/Plugins/VirtualProduction/RemoteControl/RemoteControl.uplugin"), TEXT("descriptor_authority")},
		{TEXT("ue58_plugin_descriptor"), TEXT("epic"), TEXT("plugin.DMXEngine"),
			TEXT("prerequisite"), TEXT("read"), TEXT("capability_gated"), TEXT(""),
			TEXT("DMXEngine"), 0, 0,
			TEXT("sha256:95ebbe4235d91c8caddb600ebd5501fea42d953461ec84cb227cb264674b5a62"),
			TEXT("Engine/Plugins/VirtualProduction/DMX/DMXEngine/DMXEngine.uplugin"), TEXT("descriptor_authority")},
		{TEXT("ue58_plugin_descriptor"), TEXT("epic"), TEXT("plugin.Avalanche"),
			TEXT("prerequisite"), TEXT("read"), TEXT("capability_gated"), TEXT(""),
			TEXT("Avalanche"), 0, 0,
			TEXT("sha256:3453fea9378af8cfdd5e82c050e0e252da10eec8dc299e63a221af9566cb3a76"),
			TEXT("Engine/Plugins/VirtualProduction/Avalanche/Avalanche.uplugin"), TEXT("descriptor_authority")},
		{TEXT("ue58_plugin_descriptor"), TEXT("epic"), TEXT("plugin.NDisplay"),
			TEXT("prerequisite"), TEXT("read"), TEXT("capability_gated"), TEXT(""),
			TEXT("nDisplay"), 0, 0,
			TEXT("sha256:62f63a8f0bf515841fce92f12207e540fb511ce0b694c2e00f7c7b71a0c7e7e0"),
			TEXT("Engine/Plugins/Runtime/nDisplay/nDisplay.uplugin"), TEXT("descriptor_authority")}};
	return Rows;
}

const TArray<FHyperAILiveProductionAuthorityRow>&
FHyperAIStudioLiveProductionContracts::GetCapabilityRequirements()
{
	#define REQUIREMENT_ROW(IdValue, LifeValue, AccessValue, DispositionValue, ContractValue, PluginsValue, CoverageValue) \
		{TEXT("release_requirement"), TEXT("hyperai_requirement"), TEXT(IdValue), TEXT(LifeValue), \
		 TEXT(AccessValue), TEXT(DispositionValue), TEXT(ContractValue), TEXT(PluginsValue), \
		 0, 0, TEXT(""), TEXT("release_requirement_catalog"), TEXT(CoverageValue)}
	static const TArray<FHyperAILiveProductionAuthorityRow> Rows = {
		REQUIREMENT_ROW("capability.live_production.ndisplay_configuration", "edit", "edit",
			"capability_gated", "hyper_live_production_apply_plan",
			"LiveLink,RemoteControl,DMXEngine,Avalanche,NDisplay",
			"intent_only_semantic_projection_backend_required"),
		REQUIREMENT_ROW("capability.live_production.avalanche", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "Avalanche,RemoteControl,StructUtils", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.avalanche_datalink", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "Avalanche,AvalancheDataLink,DataLink", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.avalanche_scene_state", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "Avalanche,AvalancheSceneState,SceneState", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.avalanche_transition", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "Avalanche,StateTree", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.dmx_control_console", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "DMXControlConsole", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.dmx_engine", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "DMXEngine,DMXProtocol", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.dmx_fixtures", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "DMXFixtures", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.dmx_protocol", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "DMXProtocol", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.livelink", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "LiveLink", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.ndisplay", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "nDisplay", "loaded_identity_only_specialized_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.pixel_streaming2", "discover", "unavailable",
			"drop", "", "PixelStreaming2", "excluded_remote_pixel_streaming"),
		REQUIREMENT_ROW("capability.live_production.remote_control", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "RemoteControl,StructUtils", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.remote_control_components", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "RemoteControl,RemoteControlComponents", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.remote_control_protocol_dmx", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "DMXEngine,DMXProtocol,RemoteControl,RemoteControlProtocolDMX", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.remote_control_protocol_midi", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "MIDIDevice,RemoteControl,RemoteControlProtocolMIDI", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.remote_control_protocol_osc", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "OSC,RemoteControl,RemoteControlProtocolOSC", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.scene_state_datalink", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "AvalancheSceneState,DataLink,SceneState,SceneStateDataLink", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.svg_importer", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "ActorModifier,ActorModifierCore,Avalanche,SVGImporter", "specialized_optional_adapter_required"),
		REQUIREMENT_ROW("capability.live_production.text3d", "discover", "unavailable",
			"capability_gated", "hyper_live_production_inspect", "GeometryMask,GeometryProcessing,GeometryScripting,Text3D", "specialized_optional_adapter_required")};
	#undef REQUIREMENT_ROW
	return Rows;
}

FHyperAILiveProductionCapabilityStatus
FHyperAIStudioLiveProductionContracts::GetCapabilityStatus()
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FHyperAILiveProductionCapabilityStatus Status;
	Status.SupportedCases = {
		TEXT("frozen 278-native/598-Python Epic review boundary with zero dedicated live-production callables"),
		TEXT("exact 20-row product capability requirement matrix"),
		TEXT("bounded exact already-loaded primary-object and clean package identity evidence"),
		TEXT("separate persisted and volatile fingerprints without asset loading or registry scans"),
		TEXT("detached identity and closed nDisplay topology value validation")};
	Status.UnsupportedCases = {
		TEXT("LiveLink source or subject enumeration, enablement, removal, streaming, or evaluation"),
		TEXT("media open, close, seek, playback, capture, transcode, or device interaction"),
		TEXT("Remote Control HTTP/WebSocket, DMX/MIDI/OSC protocol binding, transmission, or reception"),
		TEXT("Avalanche, SceneState, DataLink, SVG, Text3D, Cloner/Effector authoring or playback"),
		TEXT("nDisplay semantic topology projection, edit, compile, cluster launch, or synchronized render"),
		TEXT("PixelStreaming2 remote control, which is an explicit ledger drop")};
	Status.Remediation = TEXT("Load a disjoint specialized optional adapter only after its exact generated cohort and plugin requirements exist. External effects additionally require explicit confirmation plus one pinned begin call, asynchronous terminal observation/cancellation, and fresh verification.");

	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.live-production.authority.v1"));
	for (const TArray<FHyperAILiveProductionAuthorityRow>* Rows : {
		&GetEpicReviewAuthority(), &GetPluginDescriptorAuthority(),
		&GetCapabilityRequirements()})
	{
		for (const FHyperAILiveProductionAuthorityRow& Row : *Rows)
		{
			AppendToken(Canonical, Row.AuthorityKind);
			AppendToken(Canonical, Row.Source);
			AppendToken(Canonical, Row.SourceId);
			AppendToken(Canonical, Row.Lifecycle);
			AppendToken(Canonical, Row.Access);
			AppendToken(Canonical, Row.Disposition);
			AppendToken(Canonical, Row.HyperAIContract);
			AppendToken(Canonical, Row.RequiredPlugins);
			AppendToken(Canonical, FString::FromInt(Row.ReviewedCallableCount));
			AppendToken(Canonical, FString::FromInt(Row.MatchingCallableCount));
			AppendToken(Canonical, Row.ReviewedFingerprint);
			AppendToken(Canonical, Row.SourceCoordinate);
			AppendToken(Canonical, Row.FrozenCoverage);
		}
	}
	Status.AuthorityFingerprint = HashCanonical(Canonical);
	return Status;
}

bool FHyperAIStudioLiveProductionContracts::IsCanonicalProjectObjectPath(
	const FString& Path)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters
		|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| HasControlCharacter(Path) || Path.Contains(TEXT("\\"))
		|| Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))
		|| Path.EndsWith(TEXT(".")))
	{
		return false;
	}
	const int32 Dot = Path.Find(TEXT("."), ESearchCase::CaseSensitive,
		ESearchDir::FromEnd);
	const int32 Slash = Path.Find(TEXT("/"), ESearchCase::CaseSensitive,
		ESearchDir::FromEnd);
	if (Dot <= Slash + 1 || Dot >= Path.Len() - 1 || Path.Find(TEXT(".")) != Dot)
	{
		return false;
	}
	const FString PackageLeaf = Path.Mid(Slash + 1, Dot - Slash - 1);
	const FString ObjectName = Path.Mid(Dot + 1);
	return PackageLeaf == ObjectName && PackageLeaf.Len() <= MaxNameCharacters
		&& FPackageName::IsValidObjectPath(Path);
}

bool FHyperAIStudioLiveProductionContracts::IsCanonicalSha256(
	const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"),
		ESearchCase::CaseSensitive))
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

bool FHyperAIStudioLiveProductionContracts::IsSafeOperationId(
	const FString& Value)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	if (Value.Len() < 8
		|| Value.Len() > FHyperAIStudioDomainLimits::MaxOperationIdChars
		|| HasControlCharacter(Value))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('-')
			|| Character == TEXT('_') || Character == TEXT('.')
			|| Character == TEXT(':')))
		{
			return false;
		}
	}
	return true;
}

bool FHyperAIStudioLiveProductionContracts::IsKnownFamilyId(
	const FString& Value)
{
	static const TSet<FString> Families = {
		TEXT("livelink"), TEXT("media"), TEXT("remote_control"), TEXT("dmx"),
		TEXT("avalanche"), TEXT("scene_state"), TEXT("ndisplay"),
		TEXT("text3d"), TEXT("svg")};
	return Families.Contains(Value);
}

bool FHyperAIStudioLiveProductionContracts::DoesClassMatchFamily(
	const FString& ClassPath, const FString& FamilyId)
{
	if (ClassPath.IsEmpty() || ClassPath.Len() > MaxPathCharacters
		|| !IsKnownFamilyId(FamilyId))
	{
		return false;
	}
	if (FamilyId == TEXT("livelink"))
	{
		return ClassPath.StartsWith(TEXT("/Script/LiveLink."), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("media"))
	{
		return ClassPath.StartsWith(TEXT("/Script/MediaAssets."), ESearchCase::CaseSensitive)
			|| ClassPath.StartsWith(TEXT("/Script/MediaStream."), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("remote_control"))
	{
		return ClassPath.StartsWith(TEXT("/Script/RemoteControl."), ESearchCase::CaseSensitive)
			|| ClassPath.StartsWith(TEXT("/Script/RemoteControlComponents."), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("dmx"))
	{
		return ClassPath.StartsWith(TEXT("/Script/DMX"), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("avalanche"))
	{
		return ClassPath.StartsWith(TEXT("/Script/Avalanche"), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("scene_state"))
	{
		return ClassPath.StartsWith(TEXT("/Script/SceneState."), ESearchCase::CaseSensitive)
			|| ClassPath.StartsWith(TEXT("/Script/DataLink."), ESearchCase::CaseSensitive);
	}
	if (FamilyId == TEXT("ndisplay"))
	{
		return ClassPath == TEXT("/Script/DisplayCluster.DisplayClusterBlueprint");
	}
	if (FamilyId == TEXT("text3d"))
	{
		return ClassPath.StartsWith(TEXT("/Script/Text3D."), ESearchCase::CaseSensitive);
	}
	return ClassPath.StartsWith(TEXT("/Script/SVGImporter."), ESearchCase::CaseSensitive);
}

FString FHyperAIStudioLiveProductionContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists: return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist: return TEXT("does_not_exist");
	default: return TEXT("unknown");
	}
}

FString FHyperAIStudioLiveProductionContracts::ComputeObjectPersistedFingerprint(
	const FHyperAILiveProductionObjectRecord& Record)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.object-persisted.v1"))
		|| !AppendToken(Canonical, Record.FamilyId)
		|| !AppendToken(Canonical, Record.TargetPath)
		|| !AppendToken(Canonical, Record.PackageName)
		|| !AppendToken(Canonical, Record.ClassPath)
		|| !AppendToken(Canonical, Record.DiskExistence)
		|| !AppendToken(Canonical, Record.PackageSavedHash)
		|| !AppendToken(Canonical, FString::Printf(TEXT("%lld"), Record.DiskSize))
		|| !AppendToken(Canonical, Record.bFamilyClassMatched ? TEXT("family") : TEXT("mismatch"))
		|| !AppendToken(Canonical, Record.bPersistedIdentityComplete ? TEXT("complete") : TEXT("partial")))
	{
		return {};
	}
	return HashCanonical(Canonical);
}

FString FHyperAIStudioLiveProductionContracts::ComputeObjectVolatileFingerprint(
	const FHyperAILiveProductionObjectRecord& Record)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.object-volatile.v1"))
		|| !AppendToken(Canonical, Record.TargetPath)
		|| !AppendToken(Canonical, Record.bLoaded ? TEXT("loaded") : TEXT("not_loaded"))
		|| !AppendToken(Canonical, Record.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("not_disk_loaded"))
		|| !AppendToken(Canonical, Record.bPackageDirty ? TEXT("dirty") : TEXT("clean"))
		|| !AppendToken(Canonical, Record.bSemanticProjectionComplete
			? TEXT("semantic_complete") : TEXT("identity_only")))
	{
		return {};
	}
	return HashCanonical(Canonical);
}

FString FHyperAIStudioLiveProductionContracts::ComputeSnapshotPersistedFingerprint(
	const TArray<FHyperAILiveProductionObjectRecord>& Records)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.snapshot-persisted.v1")))
	{
		return {};
	}
	for (const FHyperAILiveProductionObjectRecord& Record : Records)
	{
		if (!AppendToken(Canonical, Record.TargetPath)
			|| !AppendToken(Canonical, Record.PersistedFingerprint)) return {};
	}
	return HashCanonical(Canonical);
}

FString FHyperAIStudioLiveProductionContracts::ComputeSnapshotVolatileFingerprint(
	const TArray<FHyperAILiveProductionObjectRecord>& Records)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.snapshot-volatile.v1")))
	{
		return {};
	}
	for (const FHyperAILiveProductionObjectRecord& Record : Records)
	{
		if (!AppendToken(Canonical, Record.TargetPath)
			|| !AppendToken(Canonical, Record.VolatileFingerprint)) return {};
	}
	return HashCanonical(Canonical);
}

bool FHyperAIStudioLiveProductionContracts::ValidateTopologyValueModel(
	const FHyperAILiveProductionNDisplayTopology& Topology,
	FString& OutFingerprint, TArray<FHyperAILiveProductionIssue>& OutIssues)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	OutFingerprint.Reset();
	OutIssues.Reset();
	if ((!Topology.Fingerprint.IsEmpty() && !IsCanonicalSha256(Topology.Fingerprint))
		|| Topology.Nodes.Num() < 1 || Topology.Nodes.Num() > MaxNodes
		|| Topology.Viewports.Num() < 1 || Topology.Viewports.Num() > MaxViewports
		|| Topology.Nodes.GetAllocatedSize() + Topology.Viewports.GetAllocatedSize()
			> MaxContainerAllocatedBytes)
	{
		AddIssue(OutIssues, TEXT("topology_container_bound"), TEXT("error"),
			TEXT("topology"), TEXT("Fingerprint, node/viewport counts, or allocated storage exceed the closed pre-projection bound."));
		return false;
	}

	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.ndisplay-topology.v1")))
	{
		AddIssue(OutIssues, TEXT("topology_canonical_bound"), TEXT("error"),
			TEXT("topology"), TEXT("Topology canonical storage exceeded its hard bound."));
		return false;
	}
	TSet<FString> NodeIds;
	FString PreviousNode;
	int32 PrimaryCount = 0;
	for (const FHyperAILiveProductionNDisplayNode& Node : Topology.Nodes)
	{
		if (!IsSafeName(Node.NodeId) || !IsSafeHostAddress(Node.HostAddress))
		{
			AddIssue(OutIssues, TEXT("invalid_node_value"), TEXT("error"), Node.NodeId,
				TEXT("Each node needs one bounded identifier and explicit bounded host address."));
			continue;
		}
		if (NodeIds.Contains(Node.NodeId) || (!PreviousNode.IsEmpty()
			&& Node.NodeId.Compare(PreviousNode, ESearchCase::CaseSensitive) <= 0))
		{
			AddIssue(OutIssues, TEXT("node_order_or_identity"), TEXT("error"), Node.NodeId,
				TEXT("Nodes must be unique and strictly sorted by case-sensitive NodeId."));
			continue;
		}
		NodeIds.Add(Node.NodeId);
		PreviousNode = Node.NodeId;
		PrimaryCount += Node.bPrimary ? 1 : 0;
		AppendToken(Canonical, Node.NodeId);
		AppendToken(Canonical, Node.HostAddress);
		AppendToken(Canonical, Node.bPrimary ? TEXT("primary") : TEXT("secondary"));
	}
	if (PrimaryCount != 1)
	{
		AddIssue(OutIssues, TEXT("primary_node_count"), TEXT("error"), TEXT("topology"),
			TEXT("The closed nDisplay value model requires exactly one primary node."));
	}

	TSet<FString> ViewportIds;
	FString PreviousViewport;
	for (const FHyperAILiveProductionNDisplayViewport& Viewport : Topology.Viewports)
	{
		const FString StableId = Viewport.NodeId + TEXT("/") + Viewport.ViewportId;
		const bool bRegionValid = Viewport.X >= 0 && Viewport.Y >= 0
			&& Viewport.Width > 0 && Viewport.Height > 0
			&& Viewport.X <= 32768 && Viewport.Y <= 32768
			&& Viewport.Width <= 32768 && Viewport.Height <= 32768
			&& static_cast<int64>(Viewport.X) + Viewport.Width <= 65536
			&& static_cast<int64>(Viewport.Y) + Viewport.Height <= 65536;
		if (!IsSafeName(Viewport.NodeId) || !IsSafeName(Viewport.ViewportId)
			|| !NodeIds.Contains(Viewport.NodeId) || !bRegionValid)
		{
			AddIssue(OutIssues, TEXT("invalid_viewport_value"), TEXT("error"), StableId,
				TEXT("Each viewport needs a known node, bounded identity, and finite positive pixel region."));
			continue;
		}
		if (ViewportIds.Contains(StableId) || (!PreviousViewport.IsEmpty()
			&& StableId.Compare(PreviousViewport, ESearchCase::CaseSensitive) <= 0))
		{
			AddIssue(OutIssues, TEXT("viewport_order_or_identity"), TEXT("error"), StableId,
				TEXT("Viewports must be unique and strictly sorted by case-sensitive node/id."));
			continue;
		}
		ViewportIds.Add(StableId);
		PreviousViewport = StableId;
		AppendToken(Canonical, StableId);
		AppendToken(Canonical, FString::FromInt(Viewport.X));
		AppendToken(Canonical, FString::FromInt(Viewport.Y));
		AppendToken(Canonical, FString::FromInt(Viewport.Width));
		AppendToken(Canonical, FString::FromInt(Viewport.Height));
	}
	if (CountErrors(OutIssues) != 0) return false;
	OutFingerprint = HashCanonical(Canonical);
	if (!IsCanonicalSha256(OutFingerprint))
	{
		AddIssue(OutIssues, TEXT("topology_fingerprint_failed"), TEXT("error"),
			TEXT("topology"), TEXT("The bounded topology fingerprint could not be computed."));
		return false;
	}
	if (!Topology.Fingerprint.IsEmpty() && Topology.Fingerprint != OutFingerprint)
	{
		AddIssue(OutIssues, TEXT("topology_fingerprint_mismatch"), TEXT("error"),
			TEXT("topology"), TEXT("The supplied detached topology fingerprint does not match its values."));
		return false;
	}
	return true;
}

FString FHyperAIStudioLiveProductionContracts::BuildCursor(const int32 Offset,
	const FString& RequestFingerprint, const FString& PersistedFingerprint,
	const FString& VolatileFingerprint)
{
	if (Offset < 0 || !IsCanonicalSha256(RequestFingerprint)
		|| !IsCanonicalSha256(PersistedFingerprint)
		|| !IsCanonicalSha256(VolatileFingerprint)) return {};
	const FString Cursor = FString::Printf(TEXT("v1:%d:%s:%s:%s"), Offset,
		*RequestFingerprint, *PersistedFingerprint, *VolatileFingerprint);
	return Cursor.Len() <= MaxCursorCharacters ? Cursor : FString();
}

bool FHyperAIStudioLiveProductionContracts::ParseCursor(const FString& Cursor,
	const FString& RequestFingerprint, const FString& PersistedFingerprint,
	const FString& VolatileFingerprint, int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.IsEmpty()) return true;
	if (Cursor.Len() > MaxCursorCharacters) return false;
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT(":"), false);
	// Each sha256 value itself contributes one colon, so the exact token count is eight.
	if (Parts.Num() != 8 || Parts[0] != TEXT("v1")
		|| Parts[2] != TEXT("sha256") || Parts[4] != TEXT("sha256")
		|| Parts[6] != TEXT("sha256") || !LexTryParseString(OutOffset, *Parts[1])
		|| OutOffset < 0)
	{
		return false;
	}
	return FString(TEXT("sha256:")) + Parts[3] == RequestFingerprint
		&& FString(TEXT("sha256:")) + Parts[5] == PersistedFingerprint
		&& FString(TEXT("sha256:")) + Parts[7] == VolatileFingerprint;
}

FString FHyperAIStudioLiveProductionContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.inspect.v1|family:enum|targets:bounded-primary-paths|page:int|cursor:sealed|deadline:int|output:int"));
	return Value;
}

FString FHyperAIStudioLiveProductionContracts::ApplyPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.ndisplay-intent.v1|operation:string|target:primary-path|persisted:sha256|base:closed-topology|desired:closed-topology|dry:bool"));
	return Value;
}

FString FHyperAIStudioLiveProductionContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.validate.v1|policy:enum|snapshot:detached-values|topology:detached-values|issues:int|deadline:int|output:int"));
	return Value;
}

FString FHyperAIStudioLiveProductionContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.inspect-result.v1|capability:frozen|snapshot:loaded-identity|persisted:sha256|volatile:sha256|issues:bounded"));
	return Value;
}

FString FHyperAIStudioLiveProductionContracts::ApplyResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.apply-blocked-result.v1|external-effect:zero|semantic-complete:bool|prepared:dry-only|status:stable"));
	return Value;
}

FString FHyperAIStudioLiveProductionContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("live-production.validate-result.v1|valid:bool|complete:bool|recomputed:sha256|issues:bounded|value-only:true"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioLiveProductionContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.live-production.loaded-identity.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both generated section-20 requirement groups are blocking. Optional
		// adapters may enumerate only non-blocking groups here, so this is empty.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_live_production_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_live_production_apply_plan"), ApplyVariantId,
			ApplyPayloadTypeId, ApplyPayloadSchemaFingerprint(), ApplyResultTypeId,
			ApplyResultSchemaFingerprint(), EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_live_production_validate"), ValidateVariantId,
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

FString FHyperAIStudioLiveProductionPlanPayload::GetTypeId() const
{
	return FHyperAIStudioLiveProductionContracts::ApplyPayloadTypeId;
}

FString FHyperAIStudioLiveProductionPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioLiveProductionContracts::ApplyPayloadSchemaFingerprint();
}

int32 FHyperAIStudioLiveProductionPlanPayload::GetBoundedByteSize() const
{
	const int64 Bytes = 512ll + 2ll * (TargetPath.Len()
		+ BasePersistedFingerprint.Len() + BaseTopologyFingerprint.Len()
		+ DesiredTopologyFingerprint.Len() + SemanticFingerprint.Len());
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioLiveProductionPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioLiveProductionPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioLiveProductionPlanPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->BaseTopologyFingerprint = BaseTopologyFingerprint;
	Clone->DesiredTopologyFingerprint = DesiredTopologyFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioLiveProductionContracts::ComputePlanSemanticFingerprint(
	const FHyperAIStudioLiveProductionPlanPayload& Payload)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FString Canonical;
	if (!AppendToken(Canonical, TEXT("hyperai.live-production.ndisplay-intent.v1"))
		|| !AppendToken(Canonical, Payload.TargetPath)
		|| !AppendToken(Canonical, Payload.BasePersistedFingerprint)
		|| !AppendToken(Canonical, Payload.BaseTopologyFingerprint)
		|| !AppendToken(Canonical, Payload.DesiredTopologyFingerprint))
	{
		return {};
	}
	return HashCanonical(Canonical);
}

FHyperAILiveProductionInspectReport
FHyperAIStudioLiveProductionContracts::Inspect(
	const FHyperAILiveProductionInspectRequest& Request)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FHyperAILiveProductionInspectReport Report;
	Report.Capability = GetCapabilityStatus();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Loaded exact live-production identity capture is game-thread only."));
	}
	if (Request.TargetPaths.Num() > MaxTargetPaths
		|| Request.TargetPaths.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.DeadlineMs < 1 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Target, page, cursor, deadline, or output bounds are invalid."));
	}
	if (!Request.FamilyId.IsEmpty() && !IsKnownFamilyId(Request.FamilyId))
	{
		return Reject(TEXT("invalid_family"),
			TEXT("Family must be empty for the general capability matrix or one exact allowlisted live-production family."));
	}
	const int64 WorstCaseBytes = EstimatedBaseReportBytes
		+ static_cast<int64>(FMath::Min(Request.PageSize, Request.TargetPaths.Num()))
			* EstimatedRecordBytes
		+ static_cast<int64>(FMath::Min(MaxIssues, 2 * Request.TargetPaths.Num() + 4))
			* EstimatedIssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The requested page cannot fit the closed worst-case output envelope."));
	}

	// Validate every nested string before copying or sorting the input container.
	for (const FString& Path : Request.TargetPaths)
	{
		if (!IsCanonicalProjectObjectPath(Path))
		{
			return Reject(TEXT("invalid_target"),
				TEXT("Every target must be one bounded canonical top-level /Game object path."));
		}
	}
	TArray<FString> Paths = Request.TargetPaths;
	Paths.Sort();
	for (int32 Index = 1; Index < Paths.Num(); ++Index)
	{
		if (Paths[Index] == Paths[Index - 1])
		{
			return Reject(TEXT("duplicate_target"),
				TEXT("Every target must be unique after canonical case-sensitive ordering."));
		}
	}
	const FString RequestFingerprint =
		BuildRequestFingerprint(Request.FamilyId, Paths, Request.PageSize);
	if (!IsCanonicalSha256(RequestFingerprint))
	{
		return Reject(TEXT("request_fingerprint_failed"),
			TEXT("The bounded inspect request could not be fingerprinted."));
	}

	if (Paths.IsEmpty())
	{
		const TArray<FHyperAILiveProductionObjectRecord> EmptyRecords;
		Report.bOk = true;
		Report.Status = TEXT("capability_matrix_only");
		Report.Diagnostic = TEXT("Returned the frozen authority/status matrix. No plugin, module, asset, object, registry catalog, device, network, or playback discovery ran.");
		Report.Snapshot.FamilyId = Request.FamilyId;
		Report.Snapshot.RequestFingerprint = RequestFingerprint;
		Report.Snapshot.PersistedFingerprint =
			ComputeSnapshotPersistedFingerprint(EmptyRecords);
		Report.Snapshot.VolatileFingerprint =
			ComputeSnapshotVolatileFingerprint(EmptyRecords);
		Report.Snapshot.bIdentityProjectionComplete = true;
		Report.Snapshot.bSemanticProjectionComplete = false;
		return Report;
	}

	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FHyperAILiveProductionObjectRecord> AllRecords;
	AllRecords.Reserve(Paths.Num());
	bool bIdentityComplete = true;
	for (const FString& Path : Paths)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			return Reject(TEXT("capture_deadline_exceeded"),
				TEXT("Loaded identity capture exceeded its monotonic game-thread deadline."));
		}
		FHyperAILiveProductionObjectRecord Record;
		FString Status;
		FString Diagnostic;
		if (!CaptureExactIdentity(Request.FamilyId, Path, Record, Status, Diagnostic))
		{
			bIdentityComplete = false;
			AddIssue(Report.Issues, *Status, TEXT("error"), Path, Diagnostic);
			continue;
		}
		if (!Record.bPersistedIdentityComplete)
		{
			bIdentityComplete = false;
			AddIssue(Report.Issues, TEXT("persisted_identity_incomplete"), TEXT("warning"),
				Path, TEXT("TryGetAssetPackageData(..., true) did not prove one clean saved package identity; Unknown is not absence."));
		}
		if (!Record.bFamilyClassMatched)
		{
			bIdentityComplete = false;
			AddIssue(Report.Issues, TEXT("family_class_mismatch"), TEXT("error"),
				Path, TEXT("The already-loaded exact class does not match the requested family namespace."));
		}
		AllRecords.Add(MoveTemp(Record));
	}
	const FString Persisted = ComputeSnapshotPersistedFingerprint(AllRecords);
	const FString Volatile = ComputeSnapshotVolatileFingerprint(AllRecords);
	if (!IsCanonicalSha256(Persisted) || !IsCanonicalSha256(Volatile))
	{
		return Reject(TEXT("snapshot_fingerprint_failed"),
			TEXT("The bounded identity snapshot could not be sealed."));
	}
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, RequestFingerprint, Persisted, Volatile, Offset)
		|| Offset > AllRecords.Num())
	{
		return Reject(TEXT("cursor_mismatch"),
			TEXT("The cursor is malformed or bound to different request/persisted/volatile evidence."));
	}

	const int32 End = FMath::Min(AllRecords.Num(), Offset + Request.PageSize);
	Report.Snapshot.FamilyId = Request.FamilyId;
	Report.Snapshot.RequestFingerprint = RequestFingerprint;
	Report.Snapshot.PersistedFingerprint = Persisted;
	Report.Snapshot.VolatileFingerprint = Volatile;
	Report.Snapshot.bIdentityProjectionComplete = bIdentityComplete
		&& AllRecords.Num() == Paths.Num();
	Report.Snapshot.bSemanticProjectionComplete = false;
	Report.Snapshot.TotalRecords = AllRecords.Num();
	Report.Snapshot.Records.Reserve(End - Offset);
	for (int32 Index = Offset; Index < End; ++Index)
	{
		Report.Snapshot.Records.Add(AllRecords[Index]);
	}
	if (End < AllRecords.Num())
	{
		Report.bTruncated = true;
		Report.NextCursor = BuildCursor(End, RequestFingerprint, Persisted, Volatile);
		if (Report.NextCursor.IsEmpty())
		{
			return Reject(TEXT("cursor_generation_failed"),
				TEXT("The bounded continuation cursor could not be generated."));
		}
	}
	Report.bOk = true;
	Report.Status = Report.Snapshot.bIdentityProjectionComplete
		? TEXT("ok_identity_only") : TEXT("ok_partial_identity_only");
	Report.Diagnostic = TEXT("Returned already-loaded object/package identity only. No family internals or readiness were inferred, and no optional module, asset, device, network endpoint, playback, or render path was touched.");
	return Report;
}

FHyperAILiveProductionApplyPlanReport
FHyperAIStudioLiveProductionContracts::BuildPlan(
	const FHyperAILiveProductionApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FHyperAILiveProductionApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	Report.VariantId = ApplyVariantId;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bEffectStarted = false;
		Report.Effects.PhysicalEffectCount = 0;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Live-production preflight is game-thread only."));
	}
	if (!IsSafeOperationId(Request.OperationId)
		|| !IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedFingerprint)
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_envelope"),
			TEXT("Operation, target, persisted assertion, deadline, or output bounds are invalid."));
	}
	if ((Request.bDryRun && !Request.ExpectedPlanHash.IsEmpty())
		|| (!Request.bDryRun && !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_plan_hash_contract"),
			TEXT("Dry-run takes no expected plan hash; non-dry intent must echo one canonical hash."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	if (Request.BaseTopology.Nodes.Num() > MaxNodes
		|| Request.BaseTopology.Viewports.Num() > MaxViewports
		|| Request.DesiredTopology.Nodes.Num() > MaxNodes
		|| Request.DesiredTopology.Viewports.Num() > MaxViewports
		|| Request.BaseTopology.Nodes.GetAllocatedSize()
			+ Request.BaseTopology.Viewports.GetAllocatedSize()
			+ Request.DesiredTopology.Nodes.GetAllocatedSize()
			+ Request.DesiredTopology.Viewports.GetAllocatedSize()
			> MaxContainerAllocatedBytes)
	{
		return Reject(TEXT("plan_container_bound"),
			TEXT("Topology storage exceeded its hard bound before value projection."));
	}
	const int32 PotentialIssueCount = FMath::Min(MaxIssues,
		8 + Request.BaseTopology.Nodes.Num() + Request.BaseTopology.Viewports.Num()
		+ Request.DesiredTopology.Nodes.Num()
		+ Request.DesiredTopology.Viewports.Num());
	const int64 WorstCaseBytes = EstimatedBaseReportBytes
		+ static_cast<int64>(PotentialIssueCount) * EstimatedIssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The requested report cannot fit the closed worst-case output envelope."));
	}

	TArray<FHyperAILiveProductionIssue> BaseIssues;
	TArray<FHyperAILiveProductionIssue> DesiredIssues;
	FString BaseTopologyFingerprint;
	FString DesiredTopologyFingerprint;
	const bool bBaseValid = ValidateTopologyValueModel(
		Request.BaseTopology, BaseTopologyFingerprint, BaseIssues);
	const bool bDesiredValid = ValidateTopologyValueModel(
		Request.DesiredTopology, DesiredTopologyFingerprint, DesiredIssues);
	TArray<FHyperAILiveProductionIssue> TopologyIssues = MoveTemp(BaseIssues);
	TopologyIssues.Append(MoveTemp(DesiredIssues));
	bool bIssuesTruncated = false;
	CopyIssuesWithinBudget(TopologyIssues, MaxIssues, Request.MaxOutputBytes,
		Report.Issues, bIssuesTruncated);
	if (!bBaseValid || !bDesiredValid)
	{
		return Reject(TEXT("invalid_topology_value_model"),
			TEXT("Base or desired nDisplay intent failed independent closed value validation."));
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("preflight_deadline_exceeded"),
			TEXT("Topology validation exceeded its monotonic deadline."));
	}
	if (BaseTopologyFingerprint == DesiredTopologyFingerprint)
	{
		return Reject(TEXT("no_op_intent"),
			TEXT("Base and desired topology value fingerprints are identical."));
	}
	Report.BaseTopologyFingerprint = BaseTopologyFingerprint;
	Report.DesiredTopologyFingerprint = DesiredTopologyFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.NodeCount = Request.DesiredTopology.Nodes.Num();
	Report.Effects.ViewportCount = Request.DesiredTopology.Viewports.Num();
	Report.Effects.bIntentValueModelValid = true;

	// A non-dry request is a stable zero-effect blocker in this base module. Stop
	// before even resolving a UObject: no live-state preflight can be mistaken for
	// an effect continuation, and the result is independent of target residency.
	if (!Request.bDryRun)
	{
		return Reject(NonDryCallableState,
			TEXT("No effect ran. A specialized nDisplay adapter plus explicit external confirmation and a core continuation host must pin one begin call, observe/cancel terminal state asynchronously, and fresh-verify results."));
	}

	FHyperAILiveProductionObjectRecord Record;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExactIdentity(TEXT("ndisplay"), Request.TargetPath, Record,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.BasePersistedFingerprint = Record.PersistedFingerprint;
	Report.Effects.bLoadedSemanticProjectionComplete =
		Record.bSemanticProjectionComplete;
	if (!Record.bFamilyClassMatched)
	{
		return Reject(TEXT("ndisplay_class_mismatch"),
			TEXT("The exact already-loaded target is not a UDisplayClusterBlueprint identity."));
	}
	if (!Record.bPersistedIdentityComplete
		|| Record.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("persisted_identity_cas_mismatch"),
			TEXT("Clean saved package identity is incomplete or no longer matches the exact assertion."));
	}

	// The identity-only base adapter cannot reproduce the complete loaded nDisplay
	// topology. It therefore refuses to label client-authored topology as a valid
	// dry-run or persisted configuration CAS.
	if (!Record.bSemanticProjectionComplete)
	{
		return Reject(SemanticProjectionBlocker,
			TEXT("No effect ran and no valid dry-run was claimed. A hard-linked specialized adapter must project the complete loaded base topology before TypedArtifactExecutor::Prepare may seal a plan."));
	}

	// This branch is intentionally reachable only after a future domain-owned,
	// complete loaded semantic projection. Prepare is pure and dry-run only.
	const TSharedRef<FHyperAIStudioLiveProductionPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioLiveProductionPlanPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedFingerprint = Record.PersistedFingerprint;
	Payload->BaseTopologyFingerprint = BaseTopologyFingerprint;
	Payload->DesiredTopologyFingerprint = DesiredTopologyFingerprint;
	Payload->SemanticFingerprint = ComputePlanSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || !IsCanonicalSha256(Payload->SemanticFingerprint)
		|| Clone->GetSemanticFingerprint() != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("immutable_payload_seal_failed"),
			TEXT("The closed detached topology payload could not be sealed independently."));
	}

	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_live_production_apply_plan");
	Binding.VariantId = ApplyVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	Binding.ExpectedAdapterFingerprint = GetAdapterDescriptor().AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = false;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("plugin.NDisplay"), EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("probe.live_production_backend"), EHyperAIStudioDomainPrerequisiteState::Missing}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bExternalEffectAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
			Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = MoveTemp(Binding);
	Contract.ArtifactTypeId = Clone->GetTypeId();
	Contract.ArtifactSchemaFingerprint = Clone->GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Clone->GetSemanticFingerprint();
	Contract.EffectTarget = Request.TargetPath;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = FMath::Clamp(
		5 + Request.DesiredTopology.Nodes.Num()
		+ Request.DesiredTopology.Viewports.Num(), 5, 192);
	Contract.MaxGameThreadMs = 200;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = true;
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
	Report.Status = TEXT("dry_run_valid");
	Report.Diagnostic = TEXT("Pure dry-run sealed one complete detached semantic plan. No live-production or external effect ran.");
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	return Report;
}

FHyperAILiveProductionValidateReport
FHyperAIStudioLiveProductionContracts::Validate(
	const FHyperAILiveProductionValidateRequest& Request)
{
	using namespace HyperAIStudio::LiveProduction::Private;
	FHyperAILiveProductionValidateReport Report;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 1 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Snapshot.Records.Num() > MaxTargetPaths
		|| Request.Snapshot.Records.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.Topology.Nodes.Num() > MaxNodes
		|| Request.Topology.Viewports.Num() > MaxViewports)
	{
		return Reject(TEXT("invalid_validation_bounds"),
			TEXT("Policy input, container, issue, deadline, or output bounds are invalid."));
	}
	if (Request.Policy != TEXT("identity_snapshot")
		&& Request.Policy != TEXT("ndisplay_topology"))
	{
		return Reject(TEXT("unknown_validation_policy"),
			TEXT("Policy must be identity_snapshot or ndisplay_topology."));
	}
	const int32 PotentialIssueCount = Request.Policy == TEXT("ndisplay_topology")
		? FMath::Min(Request.MaxIssues,
			4 + Request.Topology.Nodes.Num() + Request.Topology.Viewports.Num())
		: FMath::Min(Request.MaxIssues, 4 + 2 * Request.Snapshot.Records.Num());
	const int64 WorstCaseBytes = EstimatedBaseReportBytes
		+ static_cast<int64>(PotentialIssueCount) * EstimatedIssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The detached validation report cannot fit its closed worst-case output envelope."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FHyperAILiveProductionIssue> Issues;

	if (Request.Policy == TEXT("ndisplay_topology"))
	{
		Report.bOk = true;
		Report.bValid = ValidateTopologyValueModel(
			Request.Topology, Report.RecomputedTopologyFingerprint, Issues);
		Report.bComplete = Report.bValid;
		if (Report.bValid)
		{
			AddIssue(Issues, TEXT("value_model_not_live_state_evidence"), TEXT("warning"),
				TEXT("topology"), TEXT("The detached topology is structurally valid but is not proof of loaded or persisted nDisplay state."));
		}
	}
	else if (Request.Policy == TEXT("identity_snapshot"))
	{
		Report.bOk = true;
		const bool bEnvelopeValid =
			Request.Snapshot.TotalRecords == Request.Snapshot.Records.Num()
			&& Request.Snapshot.TotalRecords >= 0
			&& Request.Snapshot.TotalRecords <= MaxTargetPaths
			&& IsKnownFamilyId(Request.Snapshot.FamilyId)
			&& IsCanonicalSha256(Request.Snapshot.RequestFingerprint)
			&& IsCanonicalSha256(Request.Snapshot.PersistedFingerprint)
			&& IsCanonicalSha256(Request.Snapshot.VolatileFingerprint);
		bool bValid = bEnvelopeValid && !Request.Snapshot.bSemanticProjectionComplete;
		if (!bEnvelopeValid)
		{
			AddIssue(Issues, TEXT("invalid_snapshot_envelope"), TEXT("error"),
				TEXT("snapshot"), TEXT("Detached count, family, request seal, or aggregate seals are invalid."));
		}
		if (Request.Snapshot.bSemanticProjectionComplete)
		{
			AddIssue(Issues, TEXT("unsubstantiated_semantic_projection"), TEXT("error"),
				TEXT("snapshot"), TEXT("The identity-only DTO has no fields that can substantiate complete live-production semantics."));
		}
		FString PreviousPath;
		for (const FHyperAILiveProductionObjectRecord& Record : Request.Snapshot.Records)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return Reject(TEXT("validation_deadline_exceeded"),
					TEXT("Detached validation exceeded its monotonic deadline."));
			}
			const bool bPersistedEvidence = Record.DiskExistence == TEXT("exists")
				&& Record.bWasLoadedFromDisk && !Record.bPackageDirty
				&& Record.DiskSize > 0 && !Record.PackageSavedHash.IsEmpty();
			const bool bShape = Record.FamilyId == Request.Snapshot.FamilyId
				&& IsCanonicalProjectObjectPath(Record.TargetPath)
				&& Record.TargetPath.Len() <= MaxPathCharacters
				&& Record.PackageName
					== FPackageName::ObjectPathToPackageName(Record.TargetPath)
				&& Record.PackageName.Len() <= MaxPathCharacters
				&& Record.ClassPath.Len() <= MaxPathCharacters
				&& Record.PackageSavedHash.Len() <= MaxPathCharacters
				&& (Record.DiskExistence == TEXT("exists")
					|| Record.DiskExistence == TEXT("does_not_exist")
					|| Record.DiskExistence == TEXT("unknown"))
				&& Record.bLoaded && !Record.bSemanticProjectionComplete
				&& Record.bPersistedIdentityComplete == bPersistedEvidence
				&& IsCanonicalSha256(Record.PersistedFingerprint)
				&& IsCanonicalSha256(Record.VolatileFingerprint)
				&& (PreviousPath.IsEmpty()
					|| Record.TargetPath.Compare(PreviousPath, ESearchCase::CaseSensitive) > 0)
				&& Record.bFamilyClassMatched
					== DoesClassMatchFamily(Record.ClassPath, Record.FamilyId);
			if (!bShape)
			{
				bValid = false;
				AddIssue(Issues, TEXT("invalid_identity_record"), TEXT("error"),
					Record.TargetPath, TEXT("Detached identity fields, ordering, family match, or bounds are invalid."));
				continue;
			}
			PreviousPath = Record.TargetPath;
			if (ComputeObjectPersistedFingerprint(Record) != Record.PersistedFingerprint
				|| ComputeObjectVolatileFingerprint(Record) != Record.VolatileFingerprint)
			{
				bValid = false;
				AddIssue(Issues, TEXT("identity_record_fingerprint_mismatch"), TEXT("error"),
					Record.TargetPath, TEXT("A detached record fingerprint does not match its independent value projection."));
			}
		}
		Report.RecomputedPersistedFingerprint =
			ComputeSnapshotPersistedFingerprint(Request.Snapshot.Records);
		Report.RecomputedVolatileFingerprint =
			ComputeSnapshotVolatileFingerprint(Request.Snapshot.Records);
		if (Report.RecomputedPersistedFingerprint != Request.Snapshot.PersistedFingerprint
			|| Report.RecomputedVolatileFingerprint != Request.Snapshot.VolatileFingerprint)
		{
			bValid = false;
			AddIssue(Issues, TEXT("snapshot_fingerprint_mismatch"), TEXT("error"),
				TEXT("snapshot"), TEXT("Aggregate persisted or volatile fingerprint does not match detached records."));
		}
		if (!Request.Snapshot.bSemanticProjectionComplete)
		{
			AddIssue(Issues, TEXT("identity_only_not_semantic_cas"), TEXT("warning"),
				TEXT("snapshot"), TEXT("Valid loaded identity evidence is not complete live-production semantic or readiness evidence."));
		}
		Report.bValid = bValid;
		Report.bComplete = bValid && Request.Snapshot.bIdentityProjectionComplete;
	}
	Report.ErrorCount = CountErrors(Issues);
	Report.WarningCount = HyperAIStudio::LiveProduction::Private::CountIssuesBySeverity(
		Issues, TEXT("warning"));
	Report.bValid = Report.bValid && Report.ErrorCount == 0;
	FString ValidatorCanonical;
	AppendToken(ValidatorCanonical, TEXT("hyperai.live-production.detached-validator.v1"));
	AppendToken(ValidatorCanonical, Request.Policy);
	AppendToken(ValidatorCanonical, Report.RecomputedPersistedFingerprint);
	AppendToken(ValidatorCanonical, Report.RecomputedVolatileFingerprint);
	AppendToken(ValidatorCanonical, Report.RecomputedTopologyFingerprint);
	AppendToken(ValidatorCanonical, FString::FromInt(Report.ErrorCount));
	AppendToken(ValidatorCanonical, FString::FromInt(Report.WarningCount));
	Report.ValidatorFingerprint = HashCanonical(ValidatorCanonical);
	CopyIssuesWithinBudget(Issues, Request.MaxIssues, Request.MaxOutputBytes,
		Report.Issues, Report.bTruncated);
	Report.Status = Report.bValid
		? (Report.bComplete ? TEXT("valid") : TEXT("valid_partial"))
		: TEXT("invalid");
	Report.Diagnostic = Request.Policy == TEXT("ndisplay_topology")
		? TEXT("Validated only the detached closed topology value model; no UObject, plugin, device, network, playback, cluster, or render state was read or changed.")
		: TEXT("Independently recomputed detached identity fingerprints; no UObject, Asset Registry, plugin, module, device, network, playback, cluster, or render state was accessed.");
	return Report;
}

FHyperAILiveProductionInspectReport
UHyperAIStudioLiveProductionToolset::hyper_live_production_inspect(
	const FHyperAILiveProductionInspectRequest& Request)
{
	return FHyperAIStudioLiveProductionContracts::Inspect(Request);
}

FHyperAILiveProductionApplyPlanReport
UHyperAIStudioLiveProductionToolset::hyper_live_production_apply_plan(
	const FHyperAILiveProductionApplyPlanRequest& Request)
{
	return FHyperAIStudioLiveProductionContracts::BuildPlan(Request);
}

FHyperAILiveProductionValidateReport
UHyperAIStudioLiveProductionToolset::hyper_live_production_validate(
	const FHyperAILiveProductionValidateRequest& Request)
{
	return FHyperAIStudioLiveProductionContracts::Validate(Request);
}

void FHyperAIStudioLiveProductionRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioLiveProductionRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioLiveProductionRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioLiveProductionRegistration::IsRegistered() const
{
	return FHyperAIStudioLiveProductionContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioLiveProductionToolset::StaticClass(),
			FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioLiveProductionRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioLiveProductionContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioLiveProduction, Verbose,
			TEXT("Live-production exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioLiveProductionToolset::StaticClass(),
		FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioLiveProduction, Error,
			TEXT("Live-production atomic three-tool registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioLiveProductionRegistration::RollBackRegistration()
{
	if (!IsInGameThread() || !bOwnsToolset || !UObjectInitialized()) return;
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
		UHyperAIStudioLiveProductionToolset::StaticClass(),
		FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioLiveProduction, Error,
			TEXT("Live-production exact registration rollback failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = false;
}
