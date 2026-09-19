// Games by Hyper 2026.

#include "HyperAIStudioInterchangeToolset.h"
#include "HyperAIStudioAgentActivity.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "InterchangeAssetImportData.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioInterchange, Log, All);

namespace HyperAIStudio::Interchange::Private
{
	constexpr int32 EstimatedBaseReportBytes = 16384;
	constexpr int32 EstimatedSourceBytes = 2048;
	constexpr int32 EstimatedIssueBytes = 1536;

	int64 SaturatingAdd(const int64 A, const int64 B)
	{
		return A > MAX_int64 - FMath::Max<int64>(0, B)
			? MAX_int64 : A + FMath::Max<int64>(0, B);
	}

	int64 EstimateStringBytes(const FString& Value)
	{
		return SaturatingAdd(64, static_cast<int64>(Value.Len()) * 6);
	}

	int64 EstimateIssueBytes(const FHyperAIInterchangeIssue& Issue)
	{
		int64 Bytes = EstimatedIssueBytes;
		for (const FString* Value : {&Issue.Code, &Issue.Severity, &Issue.StableId,
			&Issue.Subject, &Issue.Detail})
		{
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(*Value));
		}
		return Bytes;
	}

	int64 EstimateSourceBytes(const FHyperAIInterchangeSourceRecord& Source)
	{
		int64 Bytes = EstimatedSourceBytes;
		for (const FString* Value : {&Source.PathClassification,
			&Source.ProjectRelativePath, &Source.PathFingerprint, &Source.Extension,
			&Source.ImportedFileHash, &Source.DisplayLabel, &Source.Fingerprint})
		{
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(*Value));
		}
		return Bytes;
	}

	void AppendToken(FString& Canonical, const FString& Token)
	{
		Canonical += FString::FromInt(Token.Len());
		Canonical += TEXT(":");
		Canonical += Token;
		Canonical += TEXT("|");
	}

	FString HashCanonical(const FString& Canonical)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	void AddIssue(TArray<FHyperAIInterchangeIssue>& Issues, const FString& Code,
		const FString& Severity, const FString& Subject, const FString& Detail)
	{
		if (Issues.Num() >= FHyperAIStudioInterchangeContracts::MaxIssues)
		{
			return;
		}
		FHyperAIInterchangeIssue Issue;
		Issue.Code = Code.Left(96);
		Issue.Severity = Severity.Left(16);
		Issue.Subject = Subject.Left(FHyperAIStudioInterchangeContracts::MaxObjectPathCharacters);
		Issue.Detail = Detail.Left(1024);
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.interchange.issue.v1"));
		AppendToken(Canonical, Issue.Code);
		AppendToken(Canonical, Issue.Subject);
		AppendToken(Canonical, Issue.Detail);
		Issue.StableId = HashCanonical(Canonical);
		Issues.Add(MoveTemp(Issue));
	}

	FString CursorSeal(const FString& TargetPath, const FString& Revision,
		const int32 PageSize)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.interchange.inspect-cursor.v1"));
		AppendToken(Canonical, TargetPath);
		AppendToken(Canonical, Revision);
		AppendToken(Canonical, FString::FromInt(PageSize));
		return HashCanonical(Canonical);
	}

	FString BuildSourceFingerprint(const FHyperAIInterchangeSourceRecord& Source)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.interchange.source.v1"));
		AppendToken(Canonical, FString::FromInt(Source.SourceIndex));
		AppendToken(Canonical, Source.PathClassification);
		AppendToken(Canonical, Source.ProjectRelativePath);
		AppendToken(Canonical, Source.PathFingerprint);
		AppendToken(Canonical, Source.Extension);
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Source.ImportedTimestampUtcTicks));
		AppendToken(Canonical, Source.ImportedFileHash);
		AppendToken(Canonical, Source.DisplayLabel);
		return HashCanonical(Canonical);
	}

	bool ContainsControl(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (FChar::IsControl(Character))
			{
				return true;
			}
		}
		return false;
	}

	bool IsAllowedSourceExtension(const FString& Extension)
	{
		return Extension == TEXT("fbx") || Extension == TEXT("gltf")
			|| Extension == TEXT("glb") || Extension == TEXT("usd")
			|| Extension == TEXT("usda") || Extension == TEXT("usdc")
			|| Extension == TEXT("obj");
	}

	FString PathSeal(const FString& Value)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.interchange.source-path.v1"));
		AppendToken(Canonical, Value);
		return HashCanonical(Canonical);
	}

	bool IsLoadedInterchangeBackendReady()
	{
		return UInterchangeAssetImportData::StaticClass() != nullptr
			&& IAssetRegistry::Get() != nullptr;
	}
}

FString FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioInterchange.HyperAIStudioInterchangeToolset");
}

const TArray<FHyperAIStudioInterchangeManifestEntry>&
FHyperAIStudioInterchangeContracts::GetManifest()
{
	static const TArray<FHyperAIStudioInterchangeManifestEntry> Manifest = {
		{TEXT("hyper_interchange_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_interchange_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_interchange_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioInterchangeContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioInterchangeManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TSet<FString> Unique;
	TArray<FString> Names;
	for (const FHyperAIStudioInterchangeManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name))
		{
			return false;
		}
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

const TArray<FString>& FHyperAIStudioInterchangeContracts::GetEpicPythonDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("EditorToolset.SkeletalMeshTools.import_file"),
		TEXT("EditorToolset.StaticMeshTools.import_file")};
	return Delegates;
}

const TArray<FHyperAIStudioInterchangeAuthorityRecord>&
FHyperAIStudioInterchangeContracts::GetCapabilityRequirements()
{
	static const TArray<FHyperAIStudioInterchangeAuthorityRecord> Records = {
		{TEXT("capability.interchange.configure"), TEXT("capability_gated"), TEXT("hyper_interchange_apply_plan"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.interchange.import"), TEXT("capability_gated"), TEXT("hyper_interchange_inspect"), TEXT("read"), CapabilityRequirementsSha256},
		{TEXT("capability.interchange.pipeline_gap"), TEXT("capability_gated"), TEXT("hyper_interchange_inspect"), TEXT("unavailable"), CapabilityRequirementsSha256}};
	return Records;
}

TArray<FHyperAIInterchangeCapabilityStatus>
FHyperAIStudioInterchangeContracts::GetCapabilityMatrix()
{
	FHyperAIInterchangeCapabilityStatus Status;
	Status.DelegatedPythonCallables = GetEpicPythonDelegates();
	for (const FHyperAIStudioInterchangeAuthorityRecord& Record : GetCapabilityRequirements())
	{
		Status.CapabilityRequirementCoordinates.Add(Record.SourceCoordinate);
	}
	Status.AuthorityFingerprints = {
		FString::Printf(TEXT("%s|%s"), NativeAccessReviewId, NativeAccessReviewRecordsSha256),
		FString::Printf(TEXT("%s|%s"), PythonAccessReviewId, PythonAccessReviewRecordsSha256),
		CapabilityRequirementsSha256};
	Status.ClosedCases = {
		TEXT("loaded-only exact mesh Interchange provenance snapshot"),
		TEXT("clean persisted CAS with fail-closed Asset Registry tri-state"),
		TEXT("project-contained FBX/glTF/USD/OBJ reimport preflight"),
		TEXT("detached provenance value validation")};
	Status.UnsupportedCases = {
		TEXT("pipeline object materialization or configuration"),
		TEXT("arbitrary/external source paths"),
		TEXT("new asset import, translator selection, import execution, build, save, or fresh post-effect proof")};
	Status.Remediation = TEXT("Use Epic's exact import_file delegates for covered imports. Concrete HyperAI reimport requires a hard-bounded import/build/save continuation backend.");
	return {MoveTemp(Status)};
}

bool FHyperAIStudioInterchangeContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxObjectPathCharacters
		|| !Path.StartsWith(TEXT("/Game/")) || Path.Contains(TEXT("*"))
		|| Path.Contains(TEXT("?")) || Path.Contains(TEXT(":"))
		|| HyperAIStudio::Interchange::Private::ContainsControl(Path))
	{
		return false;
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason))
	{
		return false;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioInterchangeContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
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

FString FHyperAIStudioInterchangeContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists:
		return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist:
		return TEXT("does_not_exist");
	default:
		return TEXT("unknown");
	}
}

bool FHyperAIStudioInterchangeContracts::ResolveProjectRelativeSourcePath(
	const FString& RelativePath, FString& OutNormalizedRelativePath,
	FString& OutExtension)
{
	using namespace HyperAIStudio::Interchange::Private;
	OutNormalizedRelativePath.Reset();
	OutExtension.Reset();
	if (RelativePath.IsEmpty() || RelativePath.Len() > MaxPathCharacters
		|| !FPaths::IsRelative(RelativePath) || RelativePath.Contains(TEXT(":"))
		|| RelativePath.StartsWith(TEXT("/")) || RelativePath.StartsWith(TEXT("\\"))
		|| ContainsControl(RelativePath))
	{
		return false;
	}
	FString Normalized = RelativePath;
	FPaths::NormalizeFilename(Normalized);
	TArray<FString> Segments;
	Normalized.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/false);
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
	const FString Extension = FPaths::GetExtension(Normalized, false).ToLower();
	if (!IsAllowedSourceExtension(Extension))
	{
		return false;
	}
	FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDirectory);
	FString Absolute = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(ProjectDirectory, Normalized));
	FPaths::NormalizeFilename(Absolute);
	if (!FPaths::IsUnderDirectory(Absolute, ProjectDirectory))
	{
		return false;
	}
	OutNormalizedRelativePath = Normalized;
	OutExtension = Extension;
	return true;
}

bool FHyperAIStudioInterchangeContracts::ClassifySourcePath(const FString& RawPath,
	FString& OutClassification, FString& OutProjectRelativePath,
	FString& OutExtension, FString& OutPathFingerprint)
{
	using namespace HyperAIStudio::Interchange::Private;
	OutClassification = TEXT("invalid");
	OutProjectRelativePath.Reset();
	OutExtension.Reset();
	OutPathFingerprint.Reset();
	if (RawPath.IsEmpty() || RawPath.Len() > MaxPathCharacters || ContainsControl(RawPath))
	{
		OutPathFingerprint = PathSeal(TEXT("invalid"));
		return false;
	}
	FString Normalized = RawPath;
	FPaths::NormalizeFilename(Normalized);
	OutExtension = FPaths::GetExtension(Normalized, false).ToLower();
	if (FPaths::IsRelative(Normalized))
	{
		OutClassification = TEXT("relative_unresolved");
		OutPathFingerprint = PathSeal(Normalized);
		return IsCanonicalSha256(OutPathFingerprint);
	}
	Normalized = FPaths::ConvertRelativePathToFull(Normalized);
	FPaths::NormalizeFilename(Normalized);
	OutPathFingerprint = PathSeal(Normalized.ToLower());
	FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDirectory);
	if (FPaths::IsUnderDirectory(Normalized, ProjectDirectory))
	{
		FString Relative = Normalized;
		if (!FPaths::MakePathRelativeTo(Relative, *(ProjectDirectory + TEXT("/"))))
		{
			OutClassification = TEXT("invalid");
			return false;
		}
		FPaths::NormalizeFilename(Relative);
		OutClassification = TEXT("project_contained_absolute");
		OutProjectRelativePath = Relative;
	}
	else
	{
		OutClassification = TEXT("external_absolute");
	}
	return IsCanonicalSha256(OutPathFingerprint);
}

FString FHyperAIStudioInterchangeContracts::BuildCursor(const FString& TargetPath,
	const FString& Revision, const int32 PageSize, const int32 Offset)
{
	if (!IsCanonicalProjectObjectPath(TargetPath) || !IsCanonicalSha256(Revision)
		|| PageSize < 1 || PageSize > MaxPageSize || Offset < 0
		|| Offset > MaxSourceFiles)
	{
		return {};
	}
	return HyperAIStudio::Interchange::Private::CursorSeal(TargetPath, Revision, PageSize)
		+ TEXT(".") + FString::FromInt(Offset);
}

bool FHyperAIStudioInterchangeContracts::ParseCursor(const FString& Cursor,
	const FString& TargetPath, const FString& Revision, const int32 PageSize,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.Len() > MaxCursorCharacters || !IsCanonicalProjectObjectPath(TargetPath)
		|| !IsCanonicalSha256(Revision) || PageSize < 1 || PageSize > MaxPageSize)
	{
		return false;
	}
	if (Cursor.IsEmpty()) return true;
	int32 Separator = INDEX_NONE;
	if (!Cursor.FindLastChar(TEXT('.'), Separator) || Separator <= 0
		|| Separator >= Cursor.Len() - 1
		|| Cursor.Left(Separator) != HyperAIStudio::Interchange::Private::CursorSeal(
			TargetPath, Revision, PageSize))
	{
		return false;
	}
	const FString OffsetText = Cursor.Mid(Separator + 1);
	if (OffsetText.IsEmpty()) return false;
	int64 Offset = 0;
	for (const TCHAR Character : OffsetText)
	{
		if (Character < TEXT('0') || Character > TEXT('9')) return false;
		Offset = Offset * 10 + Character - TEXT('0');
		if (Offset > MaxSourceFiles) return false;
	}
	OutOffset = static_cast<int32>(Offset);
	return true;
}

FString FHyperAIStudioInterchangeContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.inspect.v1|target:string|page:int|cursor:string|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioInterchangeContracts::PatchPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.reimport-patch.v1|target:string|base:sha256|source_index:int|expected_source:sha256|project_relative:string|preserve_pipelines:true|semantic:sha256"));
	return Value;
}

FString FHyperAIStudioInterchangeContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.validate.v1|target:string|expected:sha256?|policy:enum|max_issues:int|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioInterchangeContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.inspect-result.v1|asset:record|sources:bounded|issues:bounded"));
	return Value;
}

FString FHyperAIStudioInterchangeContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.mutation-blocked-result.v1|terminal:false|effect_count:zero"));
	return Value;
}

FString FHyperAIStudioInterchangeContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("interchange.validate-result.v1|valid:bool|complete:bool|validator:sha256|issues:bounded"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioInterchangeContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.interchange.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both generated geometry_interchange groups are blocking any_of groups.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_interchange_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_interchange_apply_plan"), MutationVariantId,
			PatchPayloadTypeId, PatchPayloadSchemaFingerprint(), MutationResultTypeId,
			MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_interchange_validate"), ValidateVariantId,
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

FString FHyperAIStudioInterchangeInspectPayload::GetTypeId() const
{
	return FHyperAIStudioInterchangeContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioInterchangeInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioInterchangeContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioInterchangeInspectPayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		128ll + 2ll * (Request.TargetPath.Len() + Request.Cursor.Len())));
}

FString FHyperAIStudioInterchangeValidatePayload::GetTypeId() const
{
	return FHyperAIStudioInterchangeContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioInterchangeValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioInterchangeContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioInterchangeValidatePayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		160ll + 2ll * (Request.TargetPath.Len()
			+ Request.ExpectedPersistedRevision.Len() + Request.Policy.Len())));
}

FString FHyperAIStudioInterchangePatchPayload::GetTypeId() const
{
	return FHyperAIStudioInterchangeContracts::PatchPayloadTypeId;
}

FString FHyperAIStudioInterchangePatchPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioInterchangeContracts::PatchPayloadSchemaFingerprint();
}

int32 FHyperAIStudioInterchangePatchPayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		512ll + 2ll * (TargetPath.Len() + BasePersistedRevision.Len()
			+ Patch.ExpectedSourceFingerprint.Len()
			+ Patch.ProjectRelativeSourcePath.Len()
			+ ResolvedProjectRelativeSourcePath.Len() + SemanticFingerprint.Len())));
}

FString FHyperAIStudioInterchangePatchPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioInterchangePatchPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedRevision = BasePersistedRevision;
	Clone->Patch = Patch;
	Clone->ResolvedProjectRelativeSourcePath = ResolvedProjectRelativeSourcePath;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioInterchangeInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioInterchangeContracts::InspectResultTypeId;
}

FString FHyperAIStudioInterchangeInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioInterchangeContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioInterchangeInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Interchange::Private;
	int64 Bytes = EstimatedBaseReportBytes;
	for (const FHyperAIInterchangeSourceRecord& Source : Report.Sources)
	{
		Bytes = SaturatingAdd(Bytes, EstimateSourceBytes(Source));
	}
	for (const FHyperAIInterchangeIssue& Issue : Report.Issues)
	{
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Issue));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

FString FHyperAIStudioInterchangeValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioInterchangeContracts::ValidateResultTypeId;
}

FString FHyperAIStudioInterchangeValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioInterchangeContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioInterchangeValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Interchange::Private;
	int64 Bytes = EstimatedBaseReportBytes;
	for (const FHyperAIInterchangeIssue& Issue : Report.Issues)
	{
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Issue));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

bool FHyperAIStudioInterchangeContracts::CaptureExact(const FString& TargetPath,
	const int32 MaxWorkMs, FHyperAIStudioInterchangeValueSnapshot& OutSnapshot,
	FString& OutStatus, FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Interchange::Private;
	OutSnapshot = {};
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
			TEXT("Exact loaded Interchange capture is game-thread only."));
	}
	if (!IsCanonicalProjectObjectPath(TargetPath) || MaxWorkMs < 1
		|| MaxWorkMs > MaxReadGameThreadMs)
	{
		return Fail(TEXT("invalid_capture_bounds"),
			TEXT("Capture requires one canonical top-level /Game mesh identity and a closed game-thread budget."));
	}
	const double DeadlineSeconds = FPlatformTime::Seconds() + MaxWorkMs / 1000.0;
	UObject* Resolved = FSoftObjectPath(TargetPath).ResolveObject();
	if (!Resolved)
	{
		return Fail(TEXT("target_not_loaded"),
			TEXT("The exact mesh is not already loaded; HyperAI never loads, opens, searches, scans, or imports it."));
	}
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(Resolved);
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Resolved);
	const bool bExactStatic = StaticMesh && StaticMesh->GetClass() == UStaticMesh::StaticClass();
	const bool bExactSkeletal = SkeletalMesh
		&& SkeletalMesh->GetClass() == USkeletalMesh::StaticClass();
	if ((!bExactStatic && !bExactSkeletal) || Resolved->GetPathName() != TargetPath)
	{
		return Fail(TEXT("target_class_mismatch"),
			TEXT("The exact loaded top-level object must be an exact UStaticMesh or USkeletalMesh."));
	}
	if ((bExactStatic && StaticMesh->IsCompiling())
		|| (bExactSkeletal && SkeletalMesh->IsCompiling()))
	{
		return Fail(TEXT("async_compilation_active"),
			TEXT("Import-data getters can wait on async-property locks, so capture fails before any getter while compilation is active."));
	}
	if (DeadlineExceeded(DeadlineSeconds))
	{
		return Fail(TEXT("capture_deadline_exceeded"),
			TEXT("The closed capture deadline expired before import-data access."));
	}
	UPackage* Package = Resolved->GetOutermost();
	if (!Package || Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor)
		|| Package->GetName() != FPackageName::ObjectPathToPackageName(TargetPath))
	{
		return Fail(TEXT("unsupported_target_package"),
			TEXT("Transient, PIE, or non-exact packages are outside the persisted Interchange CAS contract."));
	}

#if WITH_EDITORONLY_DATA
	UAssetImportData* RawImportData = bExactStatic
		? StaticMesh->GetAssetImportData() : SkeletalMesh->GetAssetImportData();
#else
	UAssetImportData* RawImportData = nullptr;
#endif
	UInterchangeAssetImportData* ImportData = Cast<UInterchangeAssetImportData>(RawImportData);
	if (!ImportData || ImportData->GetClass() != UInterchangeAssetImportData::StaticClass()
		|| ImportData->GetOutermost() != Package)
	{
		return Fail(TEXT("interchange_import_data_required"),
			TEXT("The exact loaded mesh does not carry one exact already-loaded UInterchangeAssetImportData projection."));
	}
	const FAssetImportInfo& SourceData = ImportData->GetSourceData();
	const int32 SourceCount = SourceData.SourceFiles.Num();
	if (SourceCount < 0 || SourceCount > MaxSourceFiles)
	{
		return Fail(TEXT("source_file_bound_exceeded"),
			TEXT("Interchange source-file count exceeded its hard cap before reserve or record materialization."));
	}
	if (DeadlineExceeded(DeadlineSeconds))
	{
		return Fail(TEXT("capture_deadline_exceeded"),
			TEXT("The closed capture deadline expired after bounded source-count access."));
	}

	FHyperAIInterchangeAssetRecord& Record = OutSnapshot.Asset;
	Record.TargetPath = TargetPath;
	Record.ClassPath = Resolved->GetClass()->GetPathName();
	Record.PackageName = Package->GetName();
	Record.AssetKind = bExactStatic ? TEXT("static_mesh") : TEXT("skeletal_mesh");
	Record.ImportDataClassPath = ImportData->GetClass()->GetPathName();
	Record.ImportDataObjectPath = ImportData->GetPathName();
	Record.NodeUniqueId = ImportData->NodeUniqueID;
	Record.SceneImportAssetPath = ImportData->SceneImportAsset.ToString();
	Record.bLoaded = true;
	Record.bWasLoadedFromDisk = Resolved->HasAnyFlags(RF_WasLoaded);
	Record.bImportDataWasLoadedFromDisk = ImportData->HasAnyFlags(RF_WasLoaded);
	Record.bPackageDirty = Package->IsDirty();
	Record.bAsyncCompilationActive = false;
	Record.bPipelineObjectsMaterialized = false;
	Record.PipelineProjectionStatus = TEXT("deliberately_not_materialized");
	Record.SourceFileCount = SourceCount;

	bool bComplete = true;
	if (Record.ClassPath.Len() > MaxObjectPathCharacters
		|| Record.PackageName.Len() > MaxObjectPathCharacters
		|| Record.ImportDataClassPath.Len() > MaxObjectPathCharacters
		|| Record.ImportDataObjectPath.Len() > MaxObjectPathCharacters
		|| Record.NodeUniqueId.Len() > MaxPathCharacters
		|| Record.SceneImportAssetPath.Len() > MaxObjectPathCharacters)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("import_identity_bound_exceeded"),
			TEXT("error"), TargetPath,
			TEXT("Mesh/import-data/node/scene identity exceeded a closed projection bound."));
		Record.NodeUniqueId = Record.NodeUniqueId.Left(MaxPathCharacters);
		Record.SceneImportAssetPath = Record.SceneImportAssetPath.Left(MaxObjectPathCharacters);
	}
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	FAssetPackageData PackageData;
	UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
	if (AssetRegistry)
	{
		PackageState = AssetRegistry->TryGetAssetPackageData(
			Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
	}
	Record.DiskExistence = ClassifyAssetRegistryExistence(PackageState);
	if (!AssetRegistry || PackageState != UE::AssetRegistry::EExists::Exists
		|| !Record.bWasLoadedFromDisk || !Record.bImportDataWasLoadedFromDisk)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("asset_registry_presence_unproven"),
			TEXT("error"), TargetPath,
			TEXT("TryGetAssetPackageData(..., true) must return Exists and both exact mesh/import-data objects must carry RF_WasLoaded; Unknown is never absence."));
	}
	else
	{
		Record.DiskSize = PackageData.DiskSize;
#if WITH_EDITORONLY_DATA
		const FIoHash SavedHash = PackageData.GetPackageSavedHash();
		Record.PackageSavedHash = LexToString(SavedHash);
		if (Record.DiskSize <= 0 || SavedHash.IsZero()
			|| Record.PackageSavedHash.IsEmpty())
#else
		if (Record.DiskSize <= 0)
#endif
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("saved_package_identity_unavailable"),
				TEXT("error"), TargetPath,
				TEXT("Asset Registry presence lacks a nonzero package saved hash or disk size."));
		}
	}
	if (Record.bPackageDirty)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("dirty_loaded_state_not_cas_complete"),
			TEXT("error"), TargetPath,
			TEXT("Dirty loaded import provenance cannot claim a persisted CAS revision."));
	}

	OutSnapshot.Sources.Reserve(SourceCount);
	for (int32 Index = 0; Index < SourceCount; ++Index)
	{
		if (DeadlineExceeded(DeadlineSeconds))
		{
			return Fail(TEXT("capture_deadline_exceeded"),
				TEXT("Interchange source projection exceeded its closed game-thread deadline."));
		}
		const FAssetImportInfo::FSourceFile& Source = SourceData.SourceFiles[Index];
		FHyperAIInterchangeSourceRecord Projected;
		Projected.SourceIndex = Index;
		const bool bPathProjected = ClassifySourcePath(Source.RelativeFilename,
			Projected.PathClassification, Projected.ProjectRelativePath,
			Projected.Extension, Projected.PathFingerprint);
		Projected.ImportedTimestampUtcTicks = Source.Timestamp.GetTicks();
		Projected.ImportedFileHash = Source.FileHash.IsValid()
			? LexToString(Source.FileHash) : FString();
		const bool bSafeDisplayLabel = Source.DisplayLabelName.Len() <= MaxNameCharacters
			&& !ContainsControl(Source.DisplayLabelName)
			&& !Source.DisplayLabelName.Contains(TEXT("/"))
			&& !Source.DisplayLabelName.Contains(TEXT("\\"))
			&& !Source.DisplayLabelName.Contains(TEXT(":"));
		Projected.DisplayLabel = bSafeDisplayLabel
			? Source.DisplayLabelName : TEXT("[redacted]");
		if (!bPathProjected || !bSafeDisplayLabel
			|| Projected.ImportedFileHash.Len() > 64)
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("source_record_projection_incomplete"),
				TEXT("error"), FString::FromInt(Index),
				TEXT("A raw source path, display label, or imported hash exceeded the closed provenance projection."));
			Projected.DisplayLabel = Projected.DisplayLabel.Left(MaxNameCharacters);
			Projected.ImportedFileHash = Projected.ImportedFileHash.Left(64);
		}
		if (Projected.PathClassification != TEXT("project_contained_absolute"))
		{
			AddIssue(OutSnapshot.CaptureIssues, TEXT("source_not_project_contained"),
				TEXT("warning"), FString::FromInt(Index),
				TEXT("The source path is sealed but not exposed; concrete reimport requires a proven project-contained path."));
		}
		Projected.Fingerprint = BuildSourceFingerprint(Projected);
		if (!IsCanonicalSha256(Projected.Fingerprint))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("source_fingerprint_failed"),
				TEXT("error"), FString::FromInt(Index),
				TEXT("The bounded source provenance record could not be sealed."));
		}
		OutSnapshot.Sources.Add(MoveTemp(Projected));
	}

	FString PersistedCanonical;
	AppendToken(PersistedCanonical, TEXT("hyperai.interchange.persisted-provenance.v1"));
	AppendToken(PersistedCanonical, Record.TargetPath);
	AppendToken(PersistedCanonical, Record.ClassPath);
	AppendToken(PersistedCanonical, Record.PackageName);
	AppendToken(PersistedCanonical, Record.AssetKind);
	AppendToken(PersistedCanonical, Record.ImportDataClassPath);
	AppendToken(PersistedCanonical, Record.ImportDataObjectPath);
	AppendToken(PersistedCanonical, Record.NodeUniqueId);
	AppendToken(PersistedCanonical, Record.SceneImportAssetPath);
	AppendToken(PersistedCanonical, Record.PipelineProjectionStatus);
	AppendToken(PersistedCanonical, Record.PackageSavedHash);
	AppendToken(PersistedCanonical, FString::Printf(TEXT("%lld"), Record.DiskSize));
	AppendToken(PersistedCanonical, FString::FromInt(Record.SourceFileCount));
	for (const FHyperAIInterchangeSourceRecord& Source : OutSnapshot.Sources)
	{
		AppendToken(PersistedCanonical, Source.Fingerprint);
	}
	const FString ProjectedProvenanceFingerprint = HashCanonical(PersistedCanonical);
	if (bComplete && IsCanonicalSha256(ProjectedProvenanceFingerprint)
		&& OutSnapshot.Sources.Num() == SourceCount)
	{
		Record.PersistedRevision = ProjectedProvenanceFingerprint;
		Record.bRevisionComplete = true;
	}
	else
	{
		bComplete = false;
	}

	FString VolatileCanonical;
	AppendToken(VolatileCanonical, TEXT("hyperai.interchange.volatile-observation.v1"));
	AppendToken(VolatileCanonical, ProjectedProvenanceFingerprint);
	AppendToken(VolatileCanonical, Record.bLoaded ? TEXT("loaded") : TEXT("unloaded"));
	AppendToken(VolatileCanonical,
		Record.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("not_disk_loaded"));
	AppendToken(VolatileCanonical,
		Record.bImportDataWasLoadedFromDisk ? TEXT("import_data_was_loaded") : TEXT("import_data_not_disk_loaded"));
	AppendToken(VolatileCanonical, Record.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
	AppendToken(VolatileCanonical, Record.bAsyncCompilationActive ? TEXT("compiling") : TEXT("not_compiling"));
	AppendToken(VolatileCanonical, Record.DiskExistence);
	Record.VolatileObservationFingerprint = HashCanonical(VolatileCanonical);
	OutSnapshot.bComplete = bComplete && Record.bRevisionComplete;
	if (!OutSnapshot.bComplete)
	{
		Record.bRevisionComplete = false;
		Record.PersistedRevision.Reset();
	}
	OutStatus = OutSnapshot.bComplete
		? TEXT("exact_loaded_interchange_snapshot") : TEXT("snapshot_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured bounded, clean, already-loaded exact Interchange provenance with non-blocking Asset Registry CAS evidence; no source file, pipeline object, translator, or manager was opened or materialized.")
		: TEXT("Loaded public provenance was inspected, but dirty, unknown, invalid, or incomplete evidence prevents persisted revision admission.");
	return true;
}

bool FHyperAIStudioInterchangeContracts::ValidateDetached(
	const FHyperAIStudioInterchangeValueSnapshot& Snapshot, const FString& Policy,
	const int32 MaxIssueCount, const int32 OutputByteLimit,
	FHyperAIInterchangeValidateReport& OutReport)
{
	using namespace HyperAIStudio::Interchange::Private;
	OutReport = {};
	OutReport.Policy = Policy.Left(32);
	OutReport.PersistedRevision = Snapshot.Asset.PersistedRevision;
	OutReport.Capabilities = GetCapabilityMatrix();
	if ((Policy != TEXT("provenance") && Policy != TEXT("reimport_ready"))
		|| MaxIssueCount < 1 || MaxIssueCount > MaxIssues
		|| OutputByteLimit < MinOutputBytes || OutputByteLimit > MaxOutputBytes)
	{
		OutReport.Status = TEXT("invalid_validation_bounds_or_policy");
		OutReport.Diagnostic = TEXT("Detached Interchange validation accepts only provenance/reimport_ready and closed issue/output bounds.");
		return false;
	}
	int64 EstimatedBytes = EstimatedBaseReportBytes;
	bool bBudgetComplete = true;
	auto Emit = [&](const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail)
	{
		TArray<FHyperAIInterchangeIssue> One;
		AddIssue(One, Code, Severity, Subject, Detail);
		if (One.IsEmpty())
		{
			bBudgetComplete = false;
			return false;
		}
		FHyperAIInterchangeIssue Issue = MoveTemp(One[0]);
		const int64 IssueBytes = EstimateIssueBytes(Issue);
		if (OutReport.Issues.Num() >= MaxIssueCount
			|| EstimatedBytes > OutputByteLimit - IssueBytes)
		{
			bBudgetComplete = false;
			OutReport.bTruncated = true;
			return false;
		}
		EstimatedBytes += IssueBytes;
		if (Severity == TEXT("error")) ++OutReport.ErrorCount;
		else if (Severity == TEXT("warning")) ++OutReport.WarningCount;
		OutReport.Issues.Add(MoveTemp(Issue));
		return true;
	};
	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete
		|| !IsCanonicalSha256(Snapshot.Asset.PersistedRevision))
	{
		Emit(TEXT("persisted_revision_incomplete"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Independent validation cannot admit provenance without complete clean persisted CAS evidence."));
	}
	for (const FHyperAIInterchangeIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (!Emit(CaptureIssue.Code, CaptureIssue.Severity,
			CaptureIssue.Subject, CaptureIssue.Detail)) break;
	}
	if (!IsCanonicalProjectObjectPath(Snapshot.Asset.TargetPath)
		|| Snapshot.Asset.DiskExistence != TEXT("exists")
		|| !Snapshot.Asset.bLoaded || !Snapshot.Asset.bWasLoadedFromDisk
		|| !Snapshot.Asset.bImportDataWasLoadedFromDisk
		|| Snapshot.Asset.bPackageDirty || Snapshot.Asset.bAsyncCompilationActive
		|| Snapshot.Asset.bPipelineObjectsMaterialized
		|| Snapshot.Asset.PipelineProjectionStatus != TEXT("deliberately_not_materialized")
		|| Snapshot.Asset.DiskSize <= 0 || Snapshot.Asset.PackageSavedHash.IsEmpty())
	{
		Emit(TEXT("asset_identity_or_cas_invalid"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Detached mesh/import-data identity, clean loaded state, pipeline non-materialization, or saved package evidence is invalid."));
	}
	if (Snapshot.Asset.SourceFileCount != Snapshot.Sources.Num()
		|| Snapshot.Sources.Num() > MaxSourceFiles)
	{
		Emit(TEXT("source_record_count_mismatch"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Detached source records must exactly match the bounded top-level count."));
	}
	for (int32 Index = 0; Index < Snapshot.Sources.Num(); ++Index)
	{
		const FHyperAIInterchangeSourceRecord& Source = Snapshot.Sources[Index];
		const bool bKnownClassification =
			Source.PathClassification == TEXT("project_contained_absolute")
			|| Source.PathClassification == TEXT("external_absolute")
			|| Source.PathClassification == TEXT("relative_unresolved");
		if (Source.SourceIndex != Index || !bKnownClassification
			|| !IsCanonicalSha256(Source.PathFingerprint)
			|| !IsCanonicalSha256(Source.Fingerprint)
			|| Source.Fingerprint != BuildSourceFingerprint(Source)
			|| Source.DisplayLabel.Len() > MaxNameCharacters
			|| Source.ImportedFileHash.Len() > 64
			|| (Source.PathClassification != TEXT("project_contained_absolute")
				&& !Source.ProjectRelativePath.IsEmpty()))
		{
			if (!Emit(TEXT("source_record_invalid"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("Source order, classification, redaction, bounded identity, or independently recomputed fingerprint is invalid."))) break;
		}
		if (Source.PathClassification == TEXT("project_contained_absolute"))
		{
			FString Normalized;
			FString Extension;
			if (!ResolveProjectRelativeSourcePath(Source.ProjectRelativePath,
				Normalized, Extension) || Normalized != Source.ProjectRelativePath
				|| Extension != Source.Extension)
			{
				if (!Emit(TEXT("contained_source_path_invalid"), TEXT("error"),
					FString::FromInt(Index),
					TEXT("A project-contained source must retain canonical relative grammar and an allowed extension."))) break;
			}
		}
		else
		{
			if (!Emit(TEXT("source_not_project_contained"), TEXT("warning"),
				FString::FromInt(Index),
				TEXT("Provenance is sealed and redacted, but this source is not eligible for concrete reimport."))) break;
		}
	}

	bool bPolicyCompletable = true;
	if (Policy == TEXT("reimport_ready"))
	{
		bPolicyCompletable = false;
		Emit(TEXT("pipeline_projection_deliberately_unavailable"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Reimport readiness cannot be complete because pipeline objects are deliberately never materialized by this loaded-only cohort."));
		if (Snapshot.Sources.IsEmpty())
		{
			Emit(TEXT("source_file_required"), TEXT("error"),
				Snapshot.Asset.TargetPath,
				TEXT("Reimport also requires at least one bounded source record."));
		}
	}

	FString ValidatorCanonical;
	AppendToken(ValidatorCanonical, TEXT("hyperai.interchange.detached-validator.v1"));
	AppendToken(ValidatorCanonical, Policy);
	AppendToken(ValidatorCanonical, Snapshot.Asset.PersistedRevision);
	AppendToken(ValidatorCanonical, FString::FromInt(OutReport.ErrorCount));
	AppendToken(ValidatorCanonical, FString::FromInt(OutReport.WarningCount));
	for (const FHyperAIInterchangeSourceRecord& Source : Snapshot.Sources)
	{
		AppendToken(ValidatorCanonical, BuildSourceFingerprint(Source));
	}
	for (const FHyperAIInterchangeIssue& Issue : OutReport.Issues)
	{
		AppendToken(ValidatorCanonical, Issue.StableId);
	}
	OutReport.ValidatorFingerprint = HashCanonical(ValidatorCanonical);
	OutReport.bComplete = Snapshot.bComplete && bBudgetComplete && bPolicyCompletable
		&& IsCanonicalSha256(OutReport.ValidatorFingerprint);
	OutReport.bValid = OutReport.bComplete && OutReport.ErrorCount == 0;
	OutReport.bOk = true;
	OutReport.Status = OutReport.bValid ? TEXT("validation_passed")
		: (OutReport.bComplete ? TEXT("validation_failed") : TEXT("validation_incomplete"));
	OutReport.Diagnostic = OutReport.bValid
		? TEXT("Detached UObject-free Interchange provenance passed independent validation; no load, file access, pipeline materialization, import, or mutation occurred.")
		: TEXT("Detached provenance validation found errors or deliberately incomplete reimport evidence; no runtime object was consulted by the validator.");
	return true;
}

FHyperAIInterchangeInspectReport FHyperAIStudioInterchangeContracts::Inspect(
	const FHyperAIInterchangeInspectRequest& Request)
{
	using namespace HyperAIStudio::Interchange::Private;
	FHyperAIInterchangeInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("The exact Interchange source cohort remains unavailable outside its generated development gate."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_bounds"),
			TEXT("Inspect requires canonical exact identity and closed page/cursor/work/output bounds."));
	}
	FHyperAIStudioInterchangeValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.Asset = Snapshot.Asset;
	Report.bFreshCapture = true;
	Report.Issues = Snapshot.CaptureIssues;
	Report.bCursorEligible = Snapshot.bComplete && Snapshot.Asset.bRevisionComplete;
	if (!Request.Cursor.IsEmpty() && !Report.bCursorEligible)
	{
		return Reject(TEXT("cursor_ineligible"),
			TEXT("Paging cursors are unavailable when the persisted provenance revision is incomplete."));
	}
	int32 Offset = 0;
	if (Report.bCursorEligible && !ParseCursor(Request.Cursor, Request.TargetPath,
		Snapshot.Asset.PersistedRevision, Request.PageSize, Offset))
	{
		return Reject(TEXT("invalid_or_stale_cursor"),
			TEXT("The inspect cursor is malformed or sealed to different identity, revision, or bounds."));
	}
	if (Offset > Snapshot.Sources.Num())
	{
		return Reject(TEXT("cursor_offset_out_of_range"),
			TEXT("The sealed cursor offset exceeds the exact bounded provenance snapshot."));
	}
	int64 EstimatedBytes = EstimatedBaseReportBytes;
	for (const FHyperAIInterchangeIssue& Issue : Report.Issues)
	{
		EstimatedBytes = SaturatingAdd(EstimatedBytes, EstimateIssueBytes(Issue));
	}
	if (EstimatedBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("Capture diagnostics exceed the caller's closed output bound."));
	}
	int32 Index = Offset;
	for (; Index < Snapshot.Sources.Num() && Report.Sources.Num() < Request.PageSize; ++Index)
	{
		const int64 ItemBytes = EstimateSourceBytes(Snapshot.Sources[Index]);
		if (EstimatedBytes > Request.MaxOutputBytes - ItemBytes) break;
		EstimatedBytes += ItemBytes;
		Report.Sources.Add(Snapshot.Sources[Index]);
	}
	if (Index == Offset && Offset < Snapshot.Sources.Num())
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("The next exact source record cannot fit the output budget; no non-advancing cursor is emitted."));
	}
	Report.bTruncated = Index < Snapshot.Sources.Num();
	if (Report.bTruncated && Report.bCursorEligible)
	{
		Report.NextCursor = BuildCursor(Request.TargetPath,
			Snapshot.Asset.PersistedRevision, Request.PageSize, Index);
	}
	Report.bOk = true;
	Report.Status = CaptureStatus;
	Report.Diagnostic = CaptureDiagnostic;
	return Report;
}

FHyperAIInterchangeValidateReport FHyperAIStudioInterchangeContracts::Validate(
	const FHyperAIInterchangeValidateRequest& Request)
{
	FHyperAIInterchangeValidateReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Interchange source cohort remains fail-closed outside development admission.");
		return Report;
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (!Request.ExpectedPersistedRevision.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPersistedRevision))
		|| (Request.Policy != TEXT("provenance") && Request.Policy != TEXT("reimport_ready"))
		|| Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_validation_request");
		Report.Diagnostic = TEXT("Validation requires canonical identity, policy, revision assertion, and closed bounds.");
		return Report;
	}
	FHyperAIStudioInterchangeValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = CaptureDiagnostic;
		return Report;
	}
	Report.bFreshCapture = true;
	if (!Request.ExpectedPersistedRevision.IsEmpty()
		&& Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		Report.Status = Snapshot.Asset.bRevisionComplete
			? TEXT("stale_revision") : TEXT("revision_incomplete");
		Report.Diagnostic = TEXT("The fresh loaded-only capture does not match the asserted complete persisted revision.");
		Report.PersistedRevision = Snapshot.Asset.PersistedRevision;
		return Report;
	}
	ValidateDetached(Snapshot, Request.Policy, Request.MaxIssues,
		Request.MaxOutputBytes, Report);
	Report.bFreshCapture = true;
	return Report;
}

FString FHyperAIStudioInterchangeContracts::ComputePatchSemanticFingerprint(
	const FHyperAIStudioInterchangePatchPayload& Payload)
{
	using namespace HyperAIStudio::Interchange::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.interchange.reimport-patch.v1"));
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BasePersistedRevision);
	AppendToken(Canonical, FString::FromInt(Payload.Patch.SourceIndex));
	AppendToken(Canonical, Payload.Patch.ExpectedSourceFingerprint);
	AppendToken(Canonical, Payload.Patch.ProjectRelativeSourcePath);
	AppendToken(Canonical, Payload.ResolvedProjectRelativeSourcePath);
	AppendToken(Canonical, Payload.Patch.bPreserveExistingPipelines
		? TEXT("preserve_pipelines") : TEXT("replace_pipelines"));
	return HashCanonical(Canonical);
}

FHyperAIInterchangeApplyPlanReport FHyperAIStudioInterchangeContracts::BuildPlan(
	const FHyperAIInterchangeApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Interchange::Private;
	FHyperAIInterchangeApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("The exact Interchange source cohort remains fail-closed outside development admission."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Interchange reimport planning requires the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| Request.Patch.SourceIndex < 0 || Request.Patch.SourceIndex >= MaxSourceFiles
		|| !IsCanonicalSha256(Request.Patch.ExpectedSourceFingerprint)
		|| Request.Patch.ProjectRelativeSourcePath.Len() > MaxPathCharacters
		|| !Request.Patch.bPreserveExistingPipelines
		|| Request.DeadlineMs < MinPrepareDeadlineMs
		|| Request.DeadlineMs > MaxPrepareDeadlineMs
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_bounds"),
			TEXT("Planning requires exact identity/CAS/source assertions, pipeline preservation, and closed deadline/work/output bounds."));
	}
	if (Request.bDryRun)
	{
		if (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty())
		{
			return Reject(TEXT("dry_run_identity_forbidden"),
				TEXT("Pure dry-run accepts neither operation id nor expected plan hash."));
		}
	}
	else if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
		|| !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		return Reject(TEXT("non_dry_identity_required"),
			TEXT("Non-dry intent requires one journal-safe operation id and canonical dry-run plan hash."));
	}

	FHyperAIStudioInterchangeValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath,
		FMath::Min(Request.MaxGameThreadMs, MaxReadGameThreadMs), Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete)
	{
		return Reject(TEXT("revision_incomplete"),
			TEXT("A reimport plan requires one complete clean persisted provenance revision."));
	}
	if (Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(TEXT("stale_revision"),
			*(TEXT("The fresh loaded-only persisted provenance revision does not match the assertion.") + FHyperAIStudioAgentActivityLog::DescribeLastChange(Request.TargetPath)));
	}
	const FHyperAIInterchangeSourceRecord* Current = Snapshot.Sources.FindByPredicate(
		[&](const FHyperAIInterchangeSourceRecord& Candidate)
		{
			return Candidate.SourceIndex == Request.Patch.SourceIndex;
		});
	if (!Current || Current->Fingerprint != Request.Patch.ExpectedSourceFingerprint)
	{
		return Reject(TEXT("stale_or_missing_source"),
			TEXT("The exact source index/fingerprint assertion no longer matches loaded provenance."));
	}
	FString NormalizedRelativePath;
	FString Extension;
	if (Request.Patch.ProjectRelativeSourcePath.IsEmpty())
	{
		if (Current->PathClassification != TEXT("project_contained_absolute")
			|| !ResolveProjectRelativeSourcePath(Current->ProjectRelativePath,
				NormalizedRelativePath, Extension))
		{
			return Reject(TEXT("source_not_project_contained"),
				TEXT("Preserving the recorded source requires one proven project-contained path with an allowed extension."));
		}
	}
	else if (!ResolveProjectRelativeSourcePath(Request.Patch.ProjectRelativeSourcePath,
		NormalizedRelativePath, Extension))
	{
		return Reject(TEXT("invalid_project_relative_source"),
			TEXT("Replacement source must be canonical project-relative FBX/glTF/USD/OBJ with no traversal or absolute component."));
	}

	const TSharedRef<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Payload->Patch = Request.Patch;
	if (!Payload->Patch.ProjectRelativeSourcePath.IsEmpty())
	{
		Payload->Patch.ProjectRelativeSourcePath = NormalizedRelativePath;
	}
	Payload->ResolvedProjectRelativeSourcePath = NormalizedRelativePath;
	Payload->SemanticFingerprint = ComputePatchSemanticFingerprint(*Payload);
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_fingerprint_failed"),
			TEXT("The closed typed Interchange patch could not be sealed."));
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || Clone->GetTypeId() != Payload->GetTypeId()
		|| Clone->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
		|| Clone->GetSemanticFingerprint() != Payload->GetSemanticFingerprint())
	{
		return Reject(TEXT("immutable_clone_failed"),
			TEXT("Typed planning requires one detached deep immutable payload clone."));
	}
	Report.BasePersistedRevision = Payload->BasePersistedRevision;
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.ResolvedProjectRelativeSourcePath = NormalizedRelativePath;
	Report.Effects.TargetCount = 1;
	Report.Effects.SourceCount = 1;
	Report.Effects.bSourceProjectContained = true;
	Report.Effects.bDestinationProjectContained = true;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bDetachedImmutableClone = true;
	Report.Effects.bWouldImportOnce = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	Report.Effects.bWouldFreshVerifyOnce = true;

	// Prepare is a pure dry-run sealer. Non-dry intent never calls it and cannot
	// enter any import or mutation lifecycle from this source cohort.
	if (!Request.bDryRun)
	{
		return Reject(NonDryCallableState,
			TEXT("No Interchange effect ran. A bounded continuation host must import/build to terminal evidence, save once, independently validate, and fresh-verify persisted CAS."));
	}

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetAdapterDescriptor();
	if (ProjectId.IsEmpty() || !IsCanonicalSha256(Descriptor.AdapterFingerprint))
	{
		return Reject(TEXT("preparation_identity_unavailable"),
			TEXT("Canonical project or exact adapter identity is unavailable."));
	}
	const bool bGeometryLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("GeometryScriptingCore"));
	const bool bDataflowLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("DataflowEditor"))
		|| FModuleManager::Get().IsModuleLoaded(TEXT("DataflowEnginePlugin"));
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_interchange_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("plugin.GeometryScripting"), bGeometryLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("plugin.Dataflow"), bDataflowLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("plugin.Interchange"), IsLoadedInterchangeBackendReady()
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("probe.geometry_script"), bGeometryLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("probe.dataflow"), bDataflowLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{LiveProbeId, EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
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
	Contract.MaxNativeOperations = 5;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
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
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.bOk = true;
	Report.Status = TEXT("dry_run_valid_execution_blocked");
	Report.Diagnostic = TEXT("Pure TypedArtifactExecutor::Prepare sealed a detached immutable Interchange plan. No source file access, pipeline materialization, import, build, save, registration mutation, or effect occurred.");
	return Report;
}

FHyperAIInterchangeInspectReport UHyperAIStudioInterchangeToolset::hyper_interchange_inspect(
	const FHyperAIInterchangeInspectRequest& Request)
{
	return FHyperAIStudioInterchangeContracts::Inspect(Request);
}

FHyperAIInterchangeApplyPlanReport UHyperAIStudioInterchangeToolset::hyper_interchange_apply_plan(
	const FHyperAIInterchangeApplyPlanRequest& Request)
{
	return FHyperAIStudioInterchangeContracts::BuildPlan(Request);
}

FHyperAIInterchangeValidateReport UHyperAIStudioInterchangeToolset::hyper_interchange_validate(
	const FHyperAIInterchangeValidateRequest& Request)
{
	return FHyperAIStudioInterchangeContracts::Validate(Request);
}

FHyperAIStudioInterchangeDomainAdapter::FHyperAIStudioInterchangeDomainAdapter()
	: Descriptor(FHyperAIStudioInterchangeContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioInterchangeDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioInterchangeDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
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
			TEXT("Interchange adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_interchange_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioInterchangeContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioInterchangeContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioInterchangeContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioInterchangeInspectPayload& Typed =
			static_cast<const FHyperAIStudioInterchangeInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioInterchangeInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioInterchangeInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioInterchangeContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_interchange_validate")
		&& Context.Binding.VariantId == FHyperAIStudioInterchangeContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioInterchangeContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioInterchangeContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioInterchangeValidatePayload& Typed =
			static_cast<const FHyperAIStudioInterchangeValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioInterchangeValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioInterchangeValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioInterchangeContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_interchange_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioInterchangeContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioInterchangeContracts::PatchPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioInterchangeContracts::PatchPayloadSchemaFingerprint())
	{
		const FHyperAIStudioInterchangePatchPayload& Typed =
			static_cast<const FHyperAIStudioInterchangePatchPayload&>(Payload);
		if (FHyperAIStudioInterchangeContracts::ComputePatchSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Interchange patch semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioInterchangeContracts::NonDryCallableState,
			TEXT("No Interchange effect ran. This synchronous adapter cannot access source files, materialize pipelines, import/build, save, independently validate, or fresh-verify."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Interchange adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioInterchangeRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioInterchangeRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioInterchangeRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioInterchangeRegistration::IsRegistered() const
{
	return FHyperAIStudioInterchangeContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioInterchangeToolset::StaticClass(),
			FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioInterchangeRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioInterchangeRegistration::RegisterAfterEngineInit()
{
	using namespace HyperAIStudio::Interchange::Private;
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioInterchangeContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioInterchange, Verbose,
			TEXT("Interchange exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!IsLoadedInterchangeBackendReady()) return;
	Adapter = MakeShared<FHyperAIStudioInterchangeDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioInterchange, Error,
			TEXT("Interchange adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioInterchangeContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioInterchange, Error,
			TEXT("Interchange live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = IsLoadedInterchangeBackendReady();
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("InterchangeEngine public import-data class and Asset Registry are already available; the probe created no manager and loaded, opened, and scanned nothing. Core separately owns both blocking any_of gates.")
		: TEXT("The loaded-only Interchange backend interface is unavailable.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioInterchange, Error,
			TEXT("Interchange live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioInterchangeToolset::StaticClass(),
		FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioInterchange, Error,
			TEXT("Interchange atomic three-tool owner registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioInterchangeRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioInterchangeToolset::StaticClass(),
			FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioInterchange, Error,
				TEXT("Interchange owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioInterchange, Error,
				TEXT("Interchange probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioInterchange, Error,
				TEXT("Interchange adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
