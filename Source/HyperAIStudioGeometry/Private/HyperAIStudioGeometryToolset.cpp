// Games by Hyper 2026.

#include "HyperAIStudioGeometryToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "GeometryScript/MeshQueryFunctions.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGeometry, Log, All);

namespace HyperAIStudio::Geometry::Private
{
	constexpr int32 EstimatedBaseReportBytes = 32768;
	constexpr int32 EstimatedLodBytes = 1536;
	constexpr int32 EstimatedMaterialBytes = 1536;
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

	int64 EstimateIssueBytes(const FHyperAIGeometryIssue& Issue)
	{
		int64 Bytes = EstimatedIssueBytes;
		for (const FString* Value : {&Issue.Code, &Issue.Severity, &Issue.StableId,
			&Issue.Subject, &Issue.Detail})
		{
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(*Value));
		}
		return Bytes;
	}

	int64 EstimateLodBytes(const FHyperAIGeometryLodRecord& Record)
	{
		return SaturatingAdd(EstimatedLodBytes, EstimateStringBytes(Record.Fingerprint));
	}

	int64 EstimateMaterialBytes(const FHyperAIGeometryMaterialRecord& Record)
	{
		int64 Bytes = EstimatedMaterialBytes;
		for (const FString* Value : {&Record.SlotName, &Record.ImportedSlotName,
			&Record.MaterialPath, &Record.Fingerprint})
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

	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	bool IsFinite(const FVector& Value)
	{
		return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z);
	}

	FString CanonicalFloat(const double Value)
	{
		return IsFinite(Value) ? FString::Printf(TEXT("%.17g"), Value) : FString();
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	void AddIssue(TArray<FHyperAIGeometryIssue>& Issues, const FString& Code,
		const FString& Severity, const FString& Subject, const FString& Detail)
	{
		if (Issues.Num() >= FHyperAIStudioGeometryContracts::MaxIssues)
		{
			return;
		}
		FHyperAIGeometryIssue Issue;
		Issue.Code = Code.Left(96);
		Issue.Severity = Severity.Left(16);
		Issue.Subject = Subject.Left(FHyperAIStudioGeometryContracts::MaxPathCharacters);
		Issue.Detail = Detail.Left(1024);
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.geometry.issue.v1"));
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
		AppendToken(Canonical, TEXT("hyperai.geometry.inspect-cursor.v1"));
		AppendToken(Canonical, TargetPath);
		AppendToken(Canonical, Revision);
		AppendToken(Canonical, FString::FromInt(PageSize));
		return HashCanonical(Canonical);
	}

	FString BuildMaterialFingerprint(const FHyperAIGeometryMaterialRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.geometry.material.v1"));
		AppendToken(Canonical, FString::FromInt(Record.SlotIndex));
		AppendToken(Canonical, Record.SlotName);
		AppendToken(Canonical, Record.ImportedSlotName);
		AppendToken(Canonical, Record.MaterialPath);
		return HashCanonical(Canonical);
	}

	FString BuildLodFingerprint(const FHyperAIGeometryLodRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.geometry.lod-build-settings.v1"));
		AppendToken(Canonical, FString::FromInt(Record.LodIndex));
		AppendToken(Canonical, Record.bBuildSettingsProjected ? TEXT("projected") : TEXT("not_projected"));
		AppendToken(Canonical, Record.bRecomputeNormals ? TEXT("recompute_normals") : TEXT("keep_normals"));
		AppendToken(Canonical, Record.bRecomputeTangents ? TEXT("recompute_tangents") : TEXT("keep_tangents"));
		AppendToken(Canonical, Record.bUseMikkTSpace ? TEXT("mikk") : TEXT("non_mikk"));
		AppendToken(Canonical, Record.bGenerateLightmapUVs ? TEXT("lightmap_uv") : TEXT("no_lightmap_uv"));
		AppendToken(Canonical, Record.bBuildReversedIndexBuffer ? TEXT("reversed_index") : TEXT("single_index"));
		AppendToken(Canonical, Record.bUseHighPrecisionTangentBasis ? TEXT("high_precision_tangent") : TEXT("normal_precision_tangent"));
		AppendToken(Canonical, Record.bUseFullPrecisionUVs ? TEXT("full_precision_uv") : TEXT("half_precision_uv"));
		AppendToken(Canonical, FString::FromInt(Record.MinLightmapResolution));
		AppendToken(Canonical, FString::FromInt(Record.SrcLightmapIndex));
		AppendToken(Canonical, FString::FromInt(Record.DstLightmapIndex));
		AppendToken(Canonical, CanonicalFloat(Record.BuildScale3D.X));
		AppendToken(Canonical, CanonicalFloat(Record.BuildScale3D.Y));
		AppendToken(Canonical, CanonicalFloat(Record.BuildScale3D.Z));
		AppendToken(Canonical, CanonicalFloat(Record.DistanceFieldResolutionScale));
		return HashCanonical(Canonical);
	}

	FString ObjectPtrPath(const TObjectPtr<UMaterialInterface>& Object)
	{
		if (!Object)
		{
			return {};
		}
		const FString Path = Object.GetPathName();
		return Path == TEXT("None") ? FString() : Path;
	}

	bool IsLoadedGeometryBackendReady()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("GeometryScriptingCore"))
			&& UGeometryScriptLibrary_MeshQueryFunctions::StaticClass() != nullptr
			&& IAssetRegistry::Get() != nullptr;
	}

}

FString FHyperAIStudioGeometryContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioGeometry.HyperAIStudioGeometryToolset");
}

const TArray<FHyperAIStudioGeometryManifestEntry>&
FHyperAIStudioGeometryContracts::GetManifest()
{
	static const TArray<FHyperAIStudioGeometryManifestEntry> Manifest = {
		{TEXT("hyper_geometry_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_geometry_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_geometry_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioGeometryContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioGeometryManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TSet<FString> Unique;
	TArray<FString> Names;
	for (const FHyperAIStudioGeometryManifestEntry& Entry : Manifest)
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

const TArray<FString>& FHyperAIStudioGeometryContracts::GetEpicNativeDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("DataflowAgent.DataflowAgentToolset.AddCommentBox"),
		TEXT("DataflowAgent.DataflowAgentToolset.AddNode"),
		TEXT("DataflowAgent.DataflowAgentToolset.AddVariable"),
		TEXT("DataflowAgent.DataflowAgentToolset.AssignDataflowTemplate"),
		TEXT("DataflowAgent.DataflowAgentToolset.ConnectNodePins"),
		TEXT("DataflowAgent.DataflowAgentToolset.CreateDataflowCompatibleAsset"),
		TEXT("DataflowAgent.DataflowAgentToolset.CreateDataflowCompatibleAssetFromTemplate"),
		TEXT("DataflowAgent.DataflowAgentToolset.CreateGraph"),
		TEXT("DataflowAgent.DataflowAgentToolset.DisconnectNodePins"),
		TEXT("DataflowAgent.DataflowAgentToolset.GetGraphStructure"),
		TEXT("DataflowAgent.DataflowAgentToolset.GetNodeInfo"),
		TEXT("DataflowAgent.DataflowAgentToolset.GetNodeTypeSchema"),
		TEXT("DataflowAgent.DataflowAgentToolset.ListDataflowCompatibleAssetTypes"),
		TEXT("DataflowAgent.DataflowAgentToolset.ListDataflowTemplatesForAssetClass"),
		TEXT("DataflowAgent.DataflowAgentToolset.ListNodeTypes"),
		TEXT("DataflowAgent.DataflowAgentToolset.ListVariables"),
		TEXT("DataflowAgent.DataflowAgentToolset.RemoveCommentBox"),
		TEXT("DataflowAgent.DataflowAgentToolset.RemoveNode"),
		TEXT("DataflowAgent.DataflowAgentToolset.RemoveVariable"),
		TEXT("DataflowAgent.DataflowAgentToolset.RepositionNode"),
		TEXT("DataflowAgent.DataflowAgentToolset.SetVariable"),
		TEXT("DataflowAgent.DataflowAgentToolset.UpdateNode")};
	return Delegates;
}

const TArray<FString>& FHyperAIStudioGeometryContracts::GetEpicPythonDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("EditorToolset.SkeletalMeshTools.add_socket"),
		TEXT("EditorToolset.SkeletalMeshTools.assign_physics_asset"),
		TEXT("EditorToolset.SkeletalMeshTools.get_bone_children"),
		TEXT("EditorToolset.SkeletalMeshTools.get_bone_names"),
		TEXT("EditorToolset.SkeletalMeshTools.get_bone_parent"),
		TEXT("EditorToolset.SkeletalMeshTools.get_bounds"),
		TEXT("EditorToolset.SkeletalMeshTools.get_lod_count"),
		TEXT("EditorToolset.SkeletalMeshTools.get_material"),
		TEXT("EditorToolset.SkeletalMeshTools.get_material_slots"),
		TEXT("EditorToolset.SkeletalMeshTools.get_morph_target_names"),
		TEXT("EditorToolset.SkeletalMeshTools.get_physics_asset"),
		TEXT("EditorToolset.SkeletalMeshTools.get_section_count"),
		TEXT("EditorToolset.SkeletalMeshTools.get_skeleton"),
		TEXT("EditorToolset.SkeletalMeshTools.get_socket_bone"),
		TEXT("EditorToolset.SkeletalMeshTools.get_socket_names"),
		TEXT("EditorToolset.SkeletalMeshTools.get_socket_transform"),
		TEXT("EditorToolset.SkeletalMeshTools.get_vertex_count"),
		TEXT("EditorToolset.SkeletalMeshTools.remove_socket"),
		TEXT("EditorToolset.SkeletalMeshTools.rename_socket"),
		TEXT("EditorToolset.SkeletalMeshTools.set_material"),
		TEXT("EditorToolset.SkeletalMeshTools.set_socket_transform"),
		TEXT("EditorToolset.StaticMeshTools.generate_convex_collisions"),
		TEXT("EditorToolset.StaticMeshTools.generate_lods"),
		TEXT("EditorToolset.StaticMeshTools.get_bounds"),
		TEXT("EditorToolset.StaticMeshTools.get_lod_count"),
		TEXT("EditorToolset.StaticMeshTools.get_lod_thresholds"),
		TEXT("EditorToolset.StaticMeshTools.get_material"),
		TEXT("EditorToolset.StaticMeshTools.get_material_slots"),
		TEXT("EditorToolset.StaticMeshTools.get_triangle_count"),
		TEXT("EditorToolset.StaticMeshTools.get_vertex_count"),
		TEXT("EditorToolset.StaticMeshTools.is_nanite_enabled"),
		TEXT("EditorToolset.StaticMeshTools.remove_collisions"),
		TEXT("EditorToolset.StaticMeshTools.remove_lods"),
		TEXT("EditorToolset.StaticMeshTools.set_lod_thresholds"),
		TEXT("EditorToolset.StaticMeshTools.set_material"),
		TEXT("EditorToolset.StaticMeshTools.set_nanite_enabled")};
	return Delegates;
}

const TArray<FHyperAIStudioGeometryAuthorityRecord>&
FHyperAIStudioGeometryContracts::GetCapabilityRequirements()
{
	static const TArray<FHyperAIStudioGeometryAuthorityRecord> Records = {
		{TEXT("capability.geometry.bsp_brush"), TEXT("capability_gated"), TEXT("hyper_geometry_apply_plan"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.mesh_metadata"), TEXT("capability_gated"), TEXT("hyper_geometry_inspect"), TEXT("read"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.boolean_analysis"), TEXT("capability_gated"), TEXT("hyper_geometry_inspect"), TEXT("read"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.repair"), TEXT("capability_gated"), TEXT("hyper_geometry_apply_plan"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.lod_screen_policy"), TEXT("capability_gated"), TEXT("hyper_geometry_apply_plan"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.nanite_delegate"), TEXT("epic_delegate"), TEXT("EditorToolset.StaticMeshTools.set_nanite_enabled"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.static_mesh_binding"), TEXT("capability_gated"), TEXT("hyper_geometry_apply_plan"), TEXT("edit"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.simplification"), TEXT("capability_gated"), TEXT("hyper_geometry_inspect"), TEXT("read"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.dataflow_gap"), TEXT("capability_gated"), TEXT("hyper_geometry_inspect"), TEXT("unavailable"), CapabilityRequirementsSha256},
		{TEXT("capability.geometry.scripting_gap"), TEXT("capability_gated"), TEXT("hyper_geometry_inspect"), TEXT("unavailable"), CapabilityRequirementsSha256}};
	return Records;
}

TArray<FHyperAIGeometryCapabilityStatus>
FHyperAIStudioGeometryContracts::GetCapabilityMatrix()
{
	FHyperAIGeometryCapabilityStatus Status;
	Status.DelegatedNativeCallables = GetEpicNativeDelegates();
	Status.DelegatedPythonCallables = GetEpicPythonDelegates();
	for (const FHyperAIStudioGeometryAuthorityRecord& Record : GetCapabilityRequirements())
	{
		Status.CapabilityRequirementCoordinates.Add(Record.SourceCoordinate);
	}
	Status.AuthorityFingerprints = {
		FString::Printf(TEXT("%s|%s"), NativeAccessReviewId, NativeAccessReviewRecordsSha256),
		FString::Printf(TEXT("%s|%s"), PythonAccessReviewId, PythonAccessReviewRecordsSha256),
		CapabilityRequirementsSha256};
	Status.ClosedCases = {
		TEXT("loaded-only exact UStaticMesh/USkeletalMesh metadata snapshot"),
		TEXT("clean persisted CAS with fail-closed Asset Registry tri-state"),
		TEXT("closed static-mesh build-policy preflight"),
		TEXT("detached structural/build-readiness value validation")};
	Status.UnsupportedCases = {
		TEXT("Epic-equivalent Dataflow graph operations"),
		TEXT("Epic-equivalent material, Nanite, socket, LOD generation, and collision operations"),
		TEXT("dynamic-mesh materialization, mesh-description extraction, compile/build, save, or fresh post-effect proof"),
		TEXT("skeletal-mesh build-setting mutation")};
	Status.Remediation = TEXT("Use the exact Epic native/Python delegates for covered work. Concrete HyperAI mesh edits require a hard-bounded build/save/validation continuation backend.");
	return {MoveTemp(Status)};
}

bool FHyperAIStudioGeometryContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (FChar::IsControl(Character))
		{
			return false;
		}
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

bool FHyperAIStudioGeometryContracts::IsCanonicalSha256(const FString& Value)
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

FString FHyperAIStudioGeometryContracts::ClassifyAssetRegistryExistence(
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

FString FHyperAIStudioGeometryContracts::BuildCursor(const FString& TargetPath,
	const FString& Revision, const int32 PageSize, const int32 Offset)
{
	if (!IsCanonicalProjectObjectPath(TargetPath) || !IsCanonicalSha256(Revision)
		|| PageSize < 1 || PageSize > MaxPageSize || Offset < 0
		|| Offset > MaxSourceModels + MaxMaterialSlots)
	{
		return {};
	}
	return HyperAIStudio::Geometry::Private::CursorSeal(TargetPath, Revision, PageSize)
		+ TEXT(".") + FString::FromInt(Offset);
}

bool FHyperAIStudioGeometryContracts::ParseCursor(const FString& Cursor,
	const FString& TargetPath, const FString& Revision, const int32 PageSize,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.Len() > MaxCursorCharacters || !IsCanonicalProjectObjectPath(TargetPath)
		|| !IsCanonicalSha256(Revision) || PageSize < 1 || PageSize > MaxPageSize)
	{
		return false;
	}
	if (Cursor.IsEmpty())
	{
		return true;
	}
	int32 Separator = INDEX_NONE;
	if (!Cursor.FindLastChar(TEXT('.'), Separator) || Separator <= 0
		|| Separator >= Cursor.Len() - 1
		|| Cursor.Left(Separator) != HyperAIStudio::Geometry::Private::CursorSeal(
			TargetPath, Revision, PageSize))
	{
		return false;
	}
	const FString OffsetText = Cursor.Mid(Separator + 1);
	if (OffsetText.IsEmpty())
	{
		return false;
	}
	int64 Offset = 0;
	for (const TCHAR Character : OffsetText)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		Offset = Offset * 10 + Character - TEXT('0');
		if (Offset > MaxSourceModels + MaxMaterialSlots)
		{
			return false;
		}
	}
	OutOffset = static_cast<int32>(Offset);
	return true;
}

FString FHyperAIStudioGeometryContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.inspect.v1|target:string|page:int|cursor:string|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioGeometryContracts::PatchPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.build-policy-patch.v1|target:string|base:sha256|lod:int|expected_lod:sha256|closed_patch:typed|semantic:sha256"));
	return Value;
}

FString FHyperAIStudioGeometryContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.validate.v1|target:string|expected:sha256?|policy:enum|max_issues:int|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioGeometryContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.inspect-result.v1|asset:record|lods:bounded|materials:bounded|issues:bounded"));
	return Value;
}

FString FHyperAIStudioGeometryContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.mutation-blocked-result.v1|terminal:false|effect_count:zero"));
	return Value;
}

FString FHyperAIStudioGeometryContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("geometry.validate-result.v1|valid:bool|complete:bool|validator:sha256|issues:bounded"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioGeometryContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.geometry.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both generated geometry_interchange groups are blocking any_of groups.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_geometry_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_geometry_apply_plan"), MutationVariantId,
			PatchPayloadTypeId, PatchPayloadSchemaFingerprint(), MutationResultTypeId,
			MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_geometry_validate"), ValidateVariantId,
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

FString FHyperAIStudioGeometryInspectPayload::GetTypeId() const
{
	return FHyperAIStudioGeometryContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioGeometryInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGeometryContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioGeometryInspectPayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		128ll + 2ll * (Request.TargetPath.Len() + Request.Cursor.Len())));
}

FString FHyperAIStudioGeometryValidatePayload::GetTypeId() const
{
	return FHyperAIStudioGeometryContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioGeometryValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGeometryContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioGeometryValidatePayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		160ll + 2ll * (Request.TargetPath.Len()
			+ Request.ExpectedPersistedRevision.Len() + Request.Policy.Len())));
}

FString FHyperAIStudioGeometryPatchPayload::GetTypeId() const
{
	return FHyperAIStudioGeometryContracts::PatchPayloadTypeId;
}

FString FHyperAIStudioGeometryPatchPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGeometryContracts::PatchPayloadSchemaFingerprint();
}

int32 FHyperAIStudioGeometryPatchPayload::GetBoundedByteSize() const
{
	return static_cast<int32>(FMath::Min<int64>(MAX_int32,
		512ll + 2ll * (TargetPath.Len() + BasePersistedRevision.Len()
			+ Patch.ExpectedLodFingerprint.Len() + SemanticFingerprint.Len())));
}

FString FHyperAIStudioGeometryPatchPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioGeometryPatchPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedRevision = BasePersistedRevision;
	Clone->Patch = Patch;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioGeometryInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioGeometryContracts::InspectResultTypeId;
}

FString FHyperAIStudioGeometryInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGeometryContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioGeometryInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Geometry::Private;
	int64 Bytes = EstimatedBaseReportBytes;
	for (const FHyperAIGeometryLodRecord& Record : Report.Lods)
	{
		Bytes = SaturatingAdd(Bytes, EstimateLodBytes(Record));
	}
	for (const FHyperAIGeometryMaterialRecord& Record : Report.Materials)
	{
		Bytes = SaturatingAdd(Bytes, EstimateMaterialBytes(Record));
	}
	for (const FHyperAIGeometryIssue& Issue : Report.Issues)
	{
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Issue));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

FString FHyperAIStudioGeometryValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioGeometryContracts::ValidateResultTypeId;
}

FString FHyperAIStudioGeometryValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGeometryContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioGeometryValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Geometry::Private;
	int64 Bytes = EstimatedBaseReportBytes;
	for (const FHyperAIGeometryIssue& Issue : Report.Issues)
	{
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Issue));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

bool FHyperAIStudioGeometryContracts::CaptureExact(const FString& TargetPath,
	const int32 MaxWorkMs, FHyperAIStudioGeometryValueSnapshot& OutSnapshot,
	FString& OutStatus, FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Geometry::Private;
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
			TEXT("Exact loaded mesh capture is game-thread only."));
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
			TEXT("The exact mesh is not already loaded; HyperAI never loads, opens, searches, scans, or compiles it."));
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
			TEXT("Mesh getters can wait on async-property locks, so capture fails before any getter while compilation is active."));
	}
	if (DeadlineExceeded(DeadlineSeconds))
	{
		return Fail(TEXT("capture_deadline_exceeded"),
			TEXT("The closed capture deadline expired before mesh value access."));
	}

	UPackage* Package = Resolved->GetOutermost();
	if (!Package || Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor)
		|| Package->GetName() != FPackageName::ObjectPathToPackageName(TargetPath))
	{
		return Fail(TEXT("unsupported_target_package"),
			TEXT("Transient, PIE, or non-exact packages are outside the persisted geometry CAS contract."));
	}

	int32 SourceModelCount = 0;
	int32 MaterialCount = 0;
	if (bExactStatic)
	{
		SourceModelCount = StaticMesh->GetNumSourceModels();
		const TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();
		MaterialCount = Materials.Num();
	}
	else
	{
		SourceModelCount = SkeletalMesh->GetNumSourceModels();
		const TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
		MaterialCount = Materials.Num();
	}
	if (SourceModelCount < 0 || SourceModelCount > MaxSourceModels
		|| MaterialCount < 0 || MaterialCount > MaxMaterialSlots)
	{
		return Fail(TEXT("asset_container_bound_exceeded"),
			TEXT("Source-model or material-slot count exceeded its hard cap before reserve or record materialization."));
	}
	if (DeadlineExceeded(DeadlineSeconds))
	{
		return Fail(TEXT("capture_deadline_exceeded"),
			TEXT("The closed capture deadline expired after bounded mesh-count access."));
	}

	FHyperAIGeometryAssetRecord& Record = OutSnapshot.Asset;
	Record.TargetPath = TargetPath;
	Record.ClassPath = Resolved->GetClass()->GetPathName();
	Record.PackageName = Package->GetName();
	Record.AssetKind = bExactStatic ? TEXT("static_mesh") : TEXT("skeletal_mesh");
	Record.bLoaded = true;
	Record.bWasLoadedFromDisk = Resolved->HasAnyFlags(RF_WasLoaded);
	Record.bPackageDirty = Package->IsDirty();
	Record.bAsyncCompilationActive = false;
	Record.SourceModelCount = SourceModelCount;
	Record.MaterialSlotCount = MaterialCount;

	bool bComplete = true;
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
		|| !Record.bWasLoadedFromDisk)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("asset_registry_presence_unproven"),
			TEXT("error"), TargetPath,
			TEXT("TryGetAssetPackageData(..., true) must return Exists and the exact mesh must carry RF_WasLoaded; Unknown is never absence."));
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
			TEXT("Dirty loaded mesh values cannot claim a persisted CAS revision."));
	}
	if (Record.ClassPath.Len() > MaxPathCharacters
		|| Record.PackageName.Len() > MaxPathCharacters)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("mesh_identity_projection_exceeded"),
			TEXT("error"), TargetPath,
			TEXT("Class or package identity exceeded the closed projection bound."));
	}

	OutSnapshot.Lods.Reserve(SourceModelCount);
	for (int32 Index = 0; Index < SourceModelCount; ++Index)
	{
		if (DeadlineExceeded(DeadlineSeconds))
		{
			return Fail(TEXT("capture_deadline_exceeded"),
				TEXT("Mesh LOD projection exceeded its closed game-thread deadline."));
		}
		FHyperAIGeometryLodRecord Lod;
		Lod.LodIndex = Index;
		if (bExactStatic)
		{
			const FStaticMeshSourceModel& SourceModel = StaticMesh->GetSourceModel(Index);
			const FMeshBuildSettings& Settings = SourceModel.BuildSettings;
			Lod.bBuildSettingsProjected = true;
			Lod.bRecomputeNormals = Settings.bRecomputeNormals;
			Lod.bRecomputeTangents = Settings.bRecomputeTangents;
			Lod.bUseMikkTSpace = Settings.bUseMikkTSpace;
			Lod.bGenerateLightmapUVs = Settings.bGenerateLightmapUVs;
			Lod.bBuildReversedIndexBuffer = Settings.bBuildReversedIndexBuffer;
			Lod.bUseHighPrecisionTangentBasis = Settings.bUseHighPrecisionTangentBasis;
			Lod.bUseFullPrecisionUVs = Settings.bUseFullPrecisionUVs;
			Lod.MinLightmapResolution = Settings.MinLightmapResolution;
			Lod.SrcLightmapIndex = Settings.SrcLightmapIndex;
			Lod.DstLightmapIndex = Settings.DstLightmapIndex;
			Lod.BuildScale3D = Settings.BuildScale3D;
			Lod.DistanceFieldResolutionScale = Settings.DistanceFieldResolutionScale;
			if (!IsFinite(Lod.BuildScale3D)
				|| !IsFinite(Lod.DistanceFieldResolutionScale))
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("non_finite_build_settings"),
					TEXT("error"), FString::FromInt(Index),
					TEXT("A static-mesh build setting contains a non-finite value."));
			}
		}
		// Skeletal source models are counted and identified only. Their bulk/model
		// contents and build settings are deliberately not materialized by this cohort.
		Lod.Fingerprint = BuildLodFingerprint(Lod);
		if (!IsCanonicalSha256(Lod.Fingerprint))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("lod_fingerprint_failed"),
				TEXT("error"), FString::FromInt(Index),
				TEXT("The bounded LOD value record could not be sealed."));
		}
		OutSnapshot.Lods.Add(MoveTemp(Lod));
	}

	OutSnapshot.Materials.Reserve(MaterialCount);
	if (bExactStatic)
	{
		const TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();
		for (int32 Index = 0; Index < MaterialCount; ++Index)
		{
			if (DeadlineExceeded(DeadlineSeconds))
			{
				return Fail(TEXT("capture_deadline_exceeded"),
					TEXT("Static-mesh material projection exceeded its closed game-thread deadline."));
			}
			const FStaticMaterial& Source = Materials[Index];
			FHyperAIGeometryMaterialRecord Material;
			Material.SlotIndex = Index;
			Material.SlotName = Source.MaterialSlotName.ToString();
			Material.ImportedSlotName = Source.ImportedMaterialSlotName.ToString();
			Material.MaterialPath = ObjectPtrPath(Source.MaterialInterface);
			if (Material.SlotName.Len() > MaxNameCharacters
				|| Material.ImportedSlotName.Len() > MaxNameCharacters
				|| Material.MaterialPath.Len() > MaxPathCharacters)
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("material_identity_bound_exceeded"),
					TEXT("error"), FString::FromInt(Index),
					TEXT("A static-mesh material identity exceeded a closed string bound."));
				Material.SlotName = Material.SlotName.Left(MaxNameCharacters);
				Material.ImportedSlotName = Material.ImportedSlotName.Left(MaxNameCharacters);
				Material.MaterialPath = Material.MaterialPath.Left(MaxPathCharacters);
			}
			Material.Fingerprint = BuildMaterialFingerprint(Material);
			if (!IsCanonicalSha256(Material.Fingerprint))
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("material_fingerprint_failed"),
					TEXT("error"), FString::FromInt(Index),
					TEXT("The bounded material record could not be sealed."));
			}
			OutSnapshot.Materials.Add(MoveTemp(Material));
		}
	}
	else
	{
		const TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
		for (int32 Index = 0; Index < MaterialCount; ++Index)
		{
			if (DeadlineExceeded(DeadlineSeconds))
			{
				return Fail(TEXT("capture_deadline_exceeded"),
					TEXT("Skeletal-mesh material projection exceeded its closed game-thread deadline."));
			}
			const FSkeletalMaterial& Source = Materials[Index];
			FHyperAIGeometryMaterialRecord Material;
			Material.SlotIndex = Index;
			Material.SlotName = Source.MaterialSlotName.ToString();
			Material.ImportedSlotName = Source.ImportedMaterialSlotName.ToString();
			Material.MaterialPath = ObjectPtrPath(Source.MaterialInterface);
			if (Material.SlotName.Len() > MaxNameCharacters
				|| Material.ImportedSlotName.Len() > MaxNameCharacters
				|| Material.MaterialPath.Len() > MaxPathCharacters)
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("material_identity_bound_exceeded"),
					TEXT("error"), FString::FromInt(Index),
					TEXT("A skeletal-mesh material identity exceeded a closed string bound."));
				Material.SlotName = Material.SlotName.Left(MaxNameCharacters);
				Material.ImportedSlotName = Material.ImportedSlotName.Left(MaxNameCharacters);
				Material.MaterialPath = Material.MaterialPath.Left(MaxPathCharacters);
			}
			Material.Fingerprint = BuildMaterialFingerprint(Material);
			if (!IsCanonicalSha256(Material.Fingerprint))
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("material_fingerprint_failed"),
					TEXT("error"), FString::FromInt(Index),
					TEXT("The bounded material record could not be sealed."));
			}
			OutSnapshot.Materials.Add(MoveTemp(Material));
		}
	}

	FString ValueCanonical;
	AppendToken(ValueCanonical, TEXT("hyperai.geometry.persisted-value-projection.v1"));
	AppendToken(ValueCanonical, Record.TargetPath);
	AppendToken(ValueCanonical, Record.ClassPath);
	AppendToken(ValueCanonical, Record.PackageName);
	AppendToken(ValueCanonical, Record.AssetKind);
	AppendToken(ValueCanonical, Record.PackageSavedHash);
	AppendToken(ValueCanonical, FString::Printf(TEXT("%lld"), Record.DiskSize));
	AppendToken(ValueCanonical, FString::FromInt(Record.SourceModelCount));
	AppendToken(ValueCanonical, FString::FromInt(Record.MaterialSlotCount));
	for (const FHyperAIGeometryLodRecord& Lod : OutSnapshot.Lods)
	{
		AppendToken(ValueCanonical, Lod.Fingerprint);
	}
	for (const FHyperAIGeometryMaterialRecord& Material : OutSnapshot.Materials)
	{
		AppendToken(ValueCanonical, Material.Fingerprint);
	}
	const FString ProjectedValueFingerprint = HashCanonical(ValueCanonical);
	if (bComplete && IsCanonicalSha256(ProjectedValueFingerprint)
		&& OutSnapshot.Lods.Num() == SourceModelCount
		&& OutSnapshot.Materials.Num() == MaterialCount)
	{
		Record.PersistedRevision = ProjectedValueFingerprint;
		Record.bRevisionComplete = true;
	}
	else
	{
		bComplete = false;
	}

	FString VolatileCanonical;
	AppendToken(VolatileCanonical, TEXT("hyperai.geometry.volatile-observation.v1"));
	AppendToken(VolatileCanonical, ProjectedValueFingerprint);
	AppendToken(VolatileCanonical, Record.bLoaded ? TEXT("loaded") : TEXT("unloaded"));
	AppendToken(VolatileCanonical,
		Record.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("not_disk_loaded"));
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
		? TEXT("exact_loaded_geometry_snapshot") : TEXT("snapshot_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured one bounded, clean, already-loaded exact mesh with non-blocking Asset Registry CAS evidence; no object, mesh description, bulk payload, or material was loaded or opened.")
		: TEXT("Loaded public values were inspected, but dirty, unknown, unsupported, invalid, or incomplete evidence prevents persisted revision admission.");
	return true;
}

bool FHyperAIStudioGeometryContracts::ValidateDetached(
	const FHyperAIStudioGeometryValueSnapshot& Snapshot, const FString& Policy,
	const int32 MaxIssueCount, const int32 OutputByteLimit,
	FHyperAIGeometryValidateReport& OutReport)
{
	using namespace HyperAIStudio::Geometry::Private;
	OutReport = {};
	OutReport.Policy = Policy.Left(32);
	OutReport.PersistedRevision = Snapshot.Asset.PersistedRevision;
	OutReport.Capabilities = GetCapabilityMatrix();
	if ((Policy != TEXT("structural") && Policy != TEXT("static_mesh_build_ready"))
		|| MaxIssueCount < 1 || MaxIssueCount > MaxIssues
		|| OutputByteLimit < MinOutputBytes || OutputByteLimit > MaxOutputBytes)
	{
		OutReport.Status = TEXT("invalid_validation_bounds_or_policy");
		OutReport.Diagnostic = TEXT("Detached geometry validation accepts only structural/static_mesh_build_ready and closed issue/output bounds.");
		return false;
	}

	int64 EstimatedBytes = EstimatedBaseReportBytes;
	bool bBudgetComplete = true;
	auto Emit = [&](const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail)
	{
		TArray<FHyperAIGeometryIssue> One;
		AddIssue(One, Code, Severity, Subject, Detail);
		if (One.IsEmpty())
		{
			bBudgetComplete = false;
			return false;
		}
		FHyperAIGeometryIssue Issue = MoveTemp(One[0]);
		const int64 IssueBytes = EstimateIssueBytes(Issue);
		if (OutReport.Issues.Num() >= MaxIssueCount
			|| EstimatedBytes > OutputByteLimit - IssueBytes)
		{
			bBudgetComplete = false;
			OutReport.bTruncated = true;
			return false;
		}
		EstimatedBytes += IssueBytes;
		if (Severity == TEXT("error"))
		{
			++OutReport.ErrorCount;
		}
		else if (Severity == TEXT("warning"))
		{
			++OutReport.WarningCount;
		}
		OutReport.Issues.Add(MoveTemp(Issue));
		return true;
	};

	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete
		|| !IsCanonicalSha256(Snapshot.Asset.PersistedRevision))
	{
		Emit(TEXT("persisted_revision_incomplete"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Independent validation cannot admit a mesh snapshot without complete clean persisted CAS evidence."));
	}
	for (const FHyperAIGeometryIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (!Emit(CaptureIssue.Code, CaptureIssue.Severity,
			CaptureIssue.Subject, CaptureIssue.Detail))
		{
			break;
		}
	}
	if (!IsCanonicalProjectObjectPath(Snapshot.Asset.TargetPath)
		|| (Snapshot.Asset.AssetKind != TEXT("static_mesh")
			&& Snapshot.Asset.AssetKind != TEXT("skeletal_mesh"))
		|| Snapshot.Asset.DiskExistence != TEXT("exists")
		|| !Snapshot.Asset.bLoaded || !Snapshot.Asset.bWasLoadedFromDisk
		|| Snapshot.Asset.bPackageDirty || Snapshot.Asset.bAsyncCompilationActive
		|| Snapshot.Asset.DiskSize <= 0 || Snapshot.Asset.PackageSavedHash.IsEmpty())
	{
		Emit(TEXT("asset_identity_or_cas_invalid"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Detached asset identity, loaded state, clean state, or saved package evidence is invalid."));
	}
	if (Snapshot.Asset.SourceModelCount != Snapshot.Lods.Num()
		|| Snapshot.Asset.MaterialSlotCount != Snapshot.Materials.Num()
		|| Snapshot.Lods.Num() > MaxSourceModels
		|| Snapshot.Materials.Num() > MaxMaterialSlots)
	{
		Emit(TEXT("record_count_mismatch"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Detached LOD/material arrays must exactly match the bounded top-level counts."));
	}

	for (int32 Index = 0; Index < Snapshot.Lods.Num(); ++Index)
	{
		const FHyperAIGeometryLodRecord& Lod = Snapshot.Lods[Index];
		if (Lod.LodIndex != Index || !IsCanonicalSha256(Lod.Fingerprint)
			|| Lod.Fingerprint != BuildLodFingerprint(Lod)
			|| !IsFinite(Lod.BuildScale3D)
			|| !IsFinite(Lod.DistanceFieldResolutionScale))
		{
			if (!Emit(TEXT("lod_record_invalid"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("LOD order, finite values, or independently recomputed fingerprint is invalid.")))
			{
				break;
			}
		}
		if (Snapshot.Asset.AssetKind == TEXT("static_mesh")
			&& !Lod.bBuildSettingsProjected)
		{
			if (!Emit(TEXT("static_mesh_build_projection_missing"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("Every static-mesh source model requires one direct public build-settings projection.")))
			{
				break;
			}
		}
		if (Snapshot.Asset.AssetKind == TEXT("skeletal_mesh")
			&& Lod.bBuildSettingsProjected)
		{
			if (!Emit(TEXT("skeletal_build_projection_out_of_contract"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("This v1 cohort deliberately does not project skeletal source-model build settings.")))
			{
				break;
			}
		}
		if (Lod.bBuildSettingsProjected
			&& (Lod.MinLightmapResolution < 0 || Lod.MinLightmapResolution > 4096
				|| Lod.SrcLightmapIndex < -1 || Lod.SrcLightmapIndex > 15
				|| Lod.DstLightmapIndex < -1 || Lod.DstLightmapIndex > 15
				|| Lod.DistanceFieldResolutionScale <= 0.0
				|| Lod.DistanceFieldResolutionScale > 1000.0
				|| FMath::Abs(Lod.BuildScale3D.X) < UE_SMALL_NUMBER
				|| FMath::Abs(Lod.BuildScale3D.Y) < UE_SMALL_NUMBER
				|| FMath::Abs(Lod.BuildScale3D.Z) < UE_SMALL_NUMBER))
		{
			if (!Emit(TEXT("static_mesh_build_settings_out_of_bounds"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("Projected build scale, lightmap layout, or distance-field scale is outside the closed validation envelope.")))
			{
				break;
			}
		}
	}

	TSet<FString> MaterialFingerprints;
	for (int32 Index = 0; Index < Snapshot.Materials.Num(); ++Index)
	{
		const FHyperAIGeometryMaterialRecord& Material = Snapshot.Materials[Index];
		if (Material.SlotIndex != Index
			|| Material.SlotName.Len() > MaxNameCharacters
			|| Material.ImportedSlotName.Len() > MaxNameCharacters
			|| Material.MaterialPath.Len() > MaxPathCharacters
			|| !IsCanonicalSha256(Material.Fingerprint)
			|| Material.Fingerprint != BuildMaterialFingerprint(Material)
			|| MaterialFingerprints.Contains(Material.Fingerprint))
		{
			if (!Emit(TEXT("material_record_invalid"), TEXT("error"),
				FString::FromInt(Index),
				TEXT("Material order, bounded identity, uniqueness, or independently recomputed fingerprint is invalid.")))
			{
				break;
			}
		}
		MaterialFingerprints.Add(Material.Fingerprint);
	}

	if (Policy == TEXT("static_mesh_build_ready"))
	{
		if (Snapshot.Asset.AssetKind != TEXT("static_mesh"))
		{
			Emit(TEXT("static_mesh_required"), TEXT("error"),
				Snapshot.Asset.TargetPath,
				TEXT("The static_mesh_build_ready policy never admits a skeletal mesh."));
		}
		if (Snapshot.Lods.IsEmpty())
		{
			Emit(TEXT("source_model_required"), TEXT("error"),
				Snapshot.Asset.TargetPath,
				TEXT("A static-mesh build-policy preflight requires at least one source model."));
		}
	}

	FString ValidatorCanonical;
	AppendToken(ValidatorCanonical, TEXT("hyperai.geometry.detached-validator.v1"));
	AppendToken(ValidatorCanonical, Policy);
	AppendToken(ValidatorCanonical, Snapshot.Asset.PersistedRevision);
	AppendToken(ValidatorCanonical, FString::FromInt(OutReport.ErrorCount));
	AppendToken(ValidatorCanonical, FString::FromInt(OutReport.WarningCount));
	for (const FHyperAIGeometryLodRecord& Lod : Snapshot.Lods)
	{
		AppendToken(ValidatorCanonical, BuildLodFingerprint(Lod));
	}
	for (const FHyperAIGeometryMaterialRecord& Material : Snapshot.Materials)
	{
		AppendToken(ValidatorCanonical, BuildMaterialFingerprint(Material));
	}
	for (const FHyperAIGeometryIssue& Issue : OutReport.Issues)
	{
		AppendToken(ValidatorCanonical, Issue.StableId);
	}
	OutReport.ValidatorFingerprint = HashCanonical(ValidatorCanonical);
	OutReport.bComplete = Snapshot.bComplete && bBudgetComplete
		&& IsCanonicalSha256(OutReport.ValidatorFingerprint);
	OutReport.bValid = OutReport.bComplete && OutReport.ErrorCount == 0;
	OutReport.bOk = true;
	OutReport.Status = OutReport.bValid ? TEXT("validation_passed")
		: (OutReport.bComplete ? TEXT("validation_failed") : TEXT("validation_incomplete"));
	OutReport.Diagnostic = OutReport.bValid
		? TEXT("Detached UObject-free geometry values passed independent validation; no load, build, save, or mutation occurred.")
		: TEXT("Detached geometry validation found errors or incomplete evidence; no runtime object was consulted by the validator.");
	return true;
}

FHyperAIGeometryInspectReport FHyperAIStudioGeometryContracts::Inspect(
	const FHyperAIGeometryInspectRequest& Request)
{
	using namespace HyperAIStudio::Geometry::Private;
	FHyperAIGeometryInspectReport Report;
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
			TEXT("The exact Geometry source cohort is SourceCandidate-only and remains unavailable outside its generated development gate."));
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
	FHyperAIStudioGeometryValueSnapshot Snapshot;
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
			TEXT("Paging cursors are unavailable when the persisted geometry revision is incomplete."));
	}
	int32 Offset = 0;
	if (Report.bCursorEligible && !ParseCursor(Request.Cursor, Request.TargetPath,
		Snapshot.Asset.PersistedRevision, Request.PageSize, Offset))
	{
		return Reject(TEXT("invalid_or_stale_cursor"),
			TEXT("The inspect cursor is malformed or sealed to different identity, revision, or bounds."));
	}
	const int32 Total = Snapshot.Lods.Num() + Snapshot.Materials.Num();
	if (Offset > Total)
	{
		return Reject(TEXT("cursor_offset_out_of_range"),
			TEXT("The sealed cursor offset exceeds the exact bounded geometry snapshot."));
	}
	int64 EstimatedBytes = EstimatedBaseReportBytes;
	for (const FHyperAIGeometryIssue& Issue : Report.Issues)
	{
		EstimatedBytes = SaturatingAdd(EstimatedBytes, EstimateIssueBytes(Issue));
	}
	if (EstimatedBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("Capture diagnostics exceed the caller's closed output bound."));
	}
	int32 GlobalIndex = 0;
	int32 Added = 0;
	bool bStopped = false;
	auto Admit = [&](const int64 ItemBytes)
	{
		if (GlobalIndex < Offset)
		{
			++GlobalIndex;
			return false;
		}
		if (Added >= Request.PageSize
			|| EstimatedBytes > Request.MaxOutputBytes - ItemBytes)
		{
			bStopped = true;
			return false;
		}
		EstimatedBytes += ItemBytes;
		++GlobalIndex;
		++Added;
		return true;
	};
	for (const FHyperAIGeometryLodRecord& Lod : Snapshot.Lods)
	{
		if (bStopped) break;
		if (Admit(EstimateLodBytes(Lod))) Report.Lods.Add(Lod);
	}
	for (const FHyperAIGeometryMaterialRecord& Material : Snapshot.Materials)
	{
		if (bStopped) break;
		if (Admit(EstimateMaterialBytes(Material))) Report.Materials.Add(Material);
	}
	if (bStopped && Added == 0 && Offset < Total)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("The next exact geometry record cannot fit the output budget; no non-advancing cursor is emitted."));
	}
	Report.bTruncated = GlobalIndex < Total;
	if (Report.bTruncated && Report.bCursorEligible)
	{
		Report.NextCursor = BuildCursor(Request.TargetPath,
			Snapshot.Asset.PersistedRevision, Request.PageSize, GlobalIndex);
	}
	Report.bOk = true;
	Report.Status = CaptureStatus;
	Report.Diagnostic = CaptureDiagnostic;
	return Report;
}

FHyperAIGeometryValidateReport FHyperAIStudioGeometryContracts::Validate(
	const FHyperAIGeometryValidateRequest& Request)
{
	FHyperAIGeometryValidateReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Geometry source cohort remains fail-closed outside development admission.");
		return Report;
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (!Request.ExpectedPersistedRevision.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPersistedRevision))
		|| (Request.Policy != TEXT("structural")
			&& Request.Policy != TEXT("static_mesh_build_ready"))
		|| Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_validation_request");
		Report.Diagnostic = TEXT("Validation requires canonical identity, policy, revision assertion, and closed bounds.");
		return Report;
	}
	FHyperAIStudioGeometryValueSnapshot Snapshot;
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

FString FHyperAIStudioGeometryContracts::ComputePatchSemanticFingerprint(
	const FHyperAIStudioGeometryPatchPayload& Payload)
{
	using namespace HyperAIStudio::Geometry::Private;
	const FHyperAIGeometryBuildPolicyPatch& Patch = Payload.Patch;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.geometry.build-policy-patch.v1"));
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BasePersistedRevision);
	AppendToken(Canonical, FString::FromInt(Patch.LodIndex));
	AppendToken(Canonical, Patch.ExpectedLodFingerprint);
	AppendToken(Canonical, Patch.bSetRecomputeNormals ? TEXT("set_normals") : TEXT("keep_normals"));
	AppendToken(Canonical, Patch.bRecomputeNormals ? TEXT("true") : TEXT("false"));
	AppendToken(Canonical, Patch.bSetRecomputeTangents ? TEXT("set_tangents") : TEXT("keep_tangents"));
	AppendToken(Canonical, Patch.bRecomputeTangents ? TEXT("true") : TEXT("false"));
	AppendToken(Canonical, Patch.bSetUseMikkTSpace ? TEXT("set_mikk") : TEXT("keep_mikk"));
	AppendToken(Canonical, Patch.bUseMikkTSpace ? TEXT("true") : TEXT("false"));
	AppendToken(Canonical, Patch.bSetGenerateLightmapUVs ? TEXT("set_lightmap_uv") : TEXT("keep_lightmap_uv"));
	AppendToken(Canonical, Patch.bGenerateLightmapUVs ? TEXT("true") : TEXT("false"));
	AppendToken(Canonical, Patch.bSetLightmapLayout ? TEXT("set_layout") : TEXT("keep_layout"));
	AppendToken(Canonical, FString::FromInt(Patch.MinLightmapResolution));
	AppendToken(Canonical, FString::FromInt(Patch.SrcLightmapIndex));
	AppendToken(Canonical, FString::FromInt(Patch.DstLightmapIndex));
	AppendToken(Canonical, Patch.bSetBuildScale ? TEXT("set_scale") : TEXT("keep_scale"));
	AppendToken(Canonical, CanonicalFloat(Patch.BuildScale3D.X));
	AppendToken(Canonical, CanonicalFloat(Patch.BuildScale3D.Y));
	AppendToken(Canonical, CanonicalFloat(Patch.BuildScale3D.Z));
	AppendToken(Canonical, Patch.bSetDistanceFieldResolutionScale ? TEXT("set_distance_scale") : TEXT("keep_distance_scale"));
	AppendToken(Canonical, CanonicalFloat(Patch.DistanceFieldResolutionScale));
	return HashCanonical(Canonical);
}

FHyperAIGeometryApplyPlanReport FHyperAIStudioGeometryContracts::BuildPlan(
	const FHyperAIGeometryApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Geometry::Private;
	FHyperAIGeometryApplyPlanReport Report;
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
			TEXT("The exact Geometry source cohort remains fail-closed outside development admission."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Geometry build-policy planning requires the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| Request.Patch.LodIndex < 0 || Request.Patch.LodIndex >= MaxSourceModels
		|| !IsCanonicalSha256(Request.Patch.ExpectedLodFingerprint)
		|| Request.DeadlineMs < MinPrepareDeadlineMs
		|| Request.DeadlineMs > MaxPrepareDeadlineMs
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_bounds"),
			TEXT("Planning requires exact identity/CAS/LOD assertions and closed deadline/work/output bounds."));
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

	const FHyperAIGeometryBuildPolicyPatch& InputPatch = Request.Patch;
	const int32 PatchFieldCount = static_cast<int32>(InputPatch.bSetRecomputeNormals)
		+ static_cast<int32>(InputPatch.bSetRecomputeTangents)
		+ static_cast<int32>(InputPatch.bSetUseMikkTSpace)
		+ static_cast<int32>(InputPatch.bSetGenerateLightmapUVs)
		+ static_cast<int32>(InputPatch.bSetLightmapLayout)
		+ static_cast<int32>(InputPatch.bSetBuildScale)
		+ static_cast<int32>(InputPatch.bSetDistanceFieldResolutionScale);
	if (PatchFieldCount < 1)
	{
		return Reject(TEXT("empty_patch"),
			TEXT("At least one closed static-mesh build-policy field must be selected."));
	}
	if ((InputPatch.bSetLightmapLayout
			&& (InputPatch.MinLightmapResolution < 1
				|| InputPatch.MinLightmapResolution > 4096
				|| InputPatch.SrcLightmapIndex < 0 || InputPatch.SrcLightmapIndex > 15
				|| InputPatch.DstLightmapIndex < 0 || InputPatch.DstLightmapIndex > 15
				|| InputPatch.SrcLightmapIndex == InputPatch.DstLightmapIndex))
		|| (InputPatch.bSetBuildScale
			&& (!IsFinite(InputPatch.BuildScale3D)
				|| InputPatch.BuildScale3D.X <= UE_SMALL_NUMBER
				|| InputPatch.BuildScale3D.Y <= UE_SMALL_NUMBER
				|| InputPatch.BuildScale3D.Z <= UE_SMALL_NUMBER
				|| InputPatch.BuildScale3D.GetAbsMax() > 10000.0))
		|| (InputPatch.bSetDistanceFieldResolutionScale
			&& (!IsFinite(InputPatch.DistanceFieldResolutionScale)
				|| InputPatch.DistanceFieldResolutionScale < 0.01
				|| InputPatch.DistanceFieldResolutionScale > 100.0)))
	{
		return Reject(TEXT("patch_value_out_of_bounds"),
			TEXT("Lightmap layout, positive build scale, or distance-field scale is outside its closed preflight envelope."));
	}

	FHyperAIStudioGeometryValueSnapshot Snapshot;
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
			TEXT("A build-policy plan requires one complete clean persisted mesh revision."));
	}
	if (Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The fresh loaded-only persisted mesh revision does not match the assertion."));
	}
	if (Snapshot.Asset.AssetKind != TEXT("static_mesh"))
	{
		return Reject(TEXT("static_mesh_required"),
			TEXT("This missing-gap build-policy variant admits exact UStaticMesh only."));
	}
	const FHyperAIGeometryLodRecord* Current = Snapshot.Lods.FindByPredicate(
		[&](const FHyperAIGeometryLodRecord& Candidate)
		{
			return Candidate.LodIndex == InputPatch.LodIndex;
		});
	if (!Current || !Current->bBuildSettingsProjected
		|| Current->Fingerprint != InputPatch.ExpectedLodFingerprint)
	{
		return Reject(TEXT("stale_or_missing_lod"),
			TEXT("The exact LOD index/fingerprint assertion no longer matches the loaded static mesh."));
	}

	FHyperAIGeometryBuildPolicyPatch Patch = InputPatch;
	if (!Patch.bSetRecomputeNormals) Patch.bRecomputeNormals = Current->bRecomputeNormals;
	if (!Patch.bSetRecomputeTangents) Patch.bRecomputeTangents = Current->bRecomputeTangents;
	if (!Patch.bSetUseMikkTSpace) Patch.bUseMikkTSpace = Current->bUseMikkTSpace;
	if (!Patch.bSetGenerateLightmapUVs) Patch.bGenerateLightmapUVs = Current->bGenerateLightmapUVs;
	if (!Patch.bSetLightmapLayout)
	{
		Patch.MinLightmapResolution = Current->MinLightmapResolution;
		Patch.SrcLightmapIndex = Current->SrcLightmapIndex;
		Patch.DstLightmapIndex = Current->DstLightmapIndex;
	}
	if (!Patch.bSetBuildScale) Patch.BuildScale3D = Current->BuildScale3D;
	if (!Patch.bSetDistanceFieldResolutionScale)
	{
		Patch.DistanceFieldResolutionScale = Current->DistanceFieldResolutionScale;
	}
	bool bNoOp = true;
	if (Patch.bSetRecomputeNormals) bNoOp &= Patch.bRecomputeNormals == Current->bRecomputeNormals;
	if (Patch.bSetRecomputeTangents) bNoOp &= Patch.bRecomputeTangents == Current->bRecomputeTangents;
	if (Patch.bSetUseMikkTSpace) bNoOp &= Patch.bUseMikkTSpace == Current->bUseMikkTSpace;
	if (Patch.bSetGenerateLightmapUVs) bNoOp &= Patch.bGenerateLightmapUVs == Current->bGenerateLightmapUVs;
	if (Patch.bSetLightmapLayout)
	{
		bNoOp &= Patch.MinLightmapResolution == Current->MinLightmapResolution
			&& Patch.SrcLightmapIndex == Current->SrcLightmapIndex
			&& Patch.DstLightmapIndex == Current->DstLightmapIndex;
	}
	if (Patch.bSetBuildScale) bNoOp &= Patch.BuildScale3D == Current->BuildScale3D;
	if (Patch.bSetDistanceFieldResolutionScale)
	{
		bNoOp &= Patch.DistanceFieldResolutionScale == Current->DistanceFieldResolutionScale;
	}
	if (bNoOp)
	{
		return Reject(TEXT("no_op_patch"),
			TEXT("Every selected build-policy field already equals the requested value."));
	}

	const TSharedRef<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Payload->Patch = Patch;
	Payload->SemanticFingerprint = ComputePatchSemanticFingerprint(*Payload);
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_fingerprint_failed"),
			TEXT("The closed typed Geometry patch could not be sealed."));
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
	Report.Effects.TargetCount = 1;
	Report.Effects.PatchFieldCount = PatchFieldCount;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bDetachedImmutableClone = true;
	Report.Effects.bWouldTransactionOnce = true;
	Report.Effects.bWouldBuildOnce = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	Report.Effects.bWouldFreshVerifyOnce = true;

	// Prepare is a pure dry-run sealer. Non-dry intent never calls it and cannot
	// enter any mutation lifecycle from this source cohort.
	if (!Request.bDryRun)
	{
		return Reject(NonDryCallableState,
			TEXT("No Geometry effect ran. A bounded continuation host must transaction once, build/compile to terminal evidence, save once, independently validate, and fresh-verify persisted CAS."));
	}

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetAdapterDescriptor();
	if (ProjectId.IsEmpty() || !IsCanonicalSha256(Descriptor.AdapterFingerprint))
	{
		return Reject(TEXT("preparation_identity_unavailable"),
			TEXT("Canonical project or exact adapter identity is unavailable."));
	}
	const bool bDataflowLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("DataflowEditor"))
		|| FModuleManager::Get().IsModuleLoaded(TEXT("DataflowEnginePlugin"));
	const bool bInterchangeLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("InterchangeEngine"));
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_geometry_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("plugin.GeometryScripting"), IsLoadedGeometryBackendReady()
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("plugin.Dataflow"), bDataflowLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("plugin.Interchange"), bInterchangeLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{LiveProbeId, EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("probe.dataflow"), bDataflowLoaded
			? EHyperAIStudioDomainPrerequisiteState::Available
			: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("probe.interchange_manager"), EHyperAIStudioDomainPrerequisiteState::Missing}};
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
	Contract.MaxNativeOperations = PatchFieldCount + 4;
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
	Report.Diagnostic = TEXT("Pure TypedArtifactExecutor::Prepare sealed a detached immutable Geometry plan. No transaction, mutation, build, compile, save, registration mutation, or effect occurred.");
	return Report;
}

FHyperAIGeometryInspectReport UHyperAIStudioGeometryToolset::hyper_geometry_inspect(
	const FHyperAIGeometryInspectRequest& Request)
{
	return FHyperAIStudioGeometryContracts::Inspect(Request);
}

FHyperAIGeometryApplyPlanReport UHyperAIStudioGeometryToolset::hyper_geometry_apply_plan(
	const FHyperAIGeometryApplyPlanRequest& Request)
{
	return FHyperAIStudioGeometryContracts::BuildPlan(Request);
}

FHyperAIGeometryValidateReport UHyperAIStudioGeometryToolset::hyper_geometry_validate(
	const FHyperAIGeometryValidateRequest& Request)
{
	return FHyperAIStudioGeometryContracts::Validate(Request);
}

FHyperAIStudioGeometryDomainAdapter::FHyperAIStudioGeometryDomainAdapter()
	: Descriptor(FHyperAIStudioGeometryContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioGeometryDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioGeometryDomainAdapter::Execute(
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
			TEXT("Geometry adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_geometry_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioGeometryContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioGeometryContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioGeometryContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioGeometryInspectPayload& Typed =
			static_cast<const FHyperAIStudioGeometryInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioGeometryInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioGeometryInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioGeometryContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_geometry_validate")
		&& Context.Binding.VariantId == FHyperAIStudioGeometryContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioGeometryContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioGeometryContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioGeometryValidatePayload& Typed =
			static_cast<const FHyperAIStudioGeometryValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioGeometryValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioGeometryValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioGeometryContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_geometry_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioGeometryContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioGeometryContracts::PatchPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioGeometryContracts::PatchPayloadSchemaFingerprint())
	{
		const FHyperAIStudioGeometryPatchPayload& Typed =
			static_cast<const FHyperAIStudioGeometryPatchPayload&>(Payload);
		if (FHyperAIStudioGeometryContracts::ComputePatchSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Geometry patch semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioGeometryContracts::NonDryCallableState,
			TEXT("No Geometry effect ran. This synchronous adapter cannot transaction, mutate, build/compile, save, independently validate, or fresh-verify."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Geometry adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioGeometryRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioGeometryRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioGeometryRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioGeometryRegistration::IsRegistered() const
{
	return FHyperAIStudioGeometryContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGeometryToolset::StaticClass(),
			FHyperAIStudioGeometryContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioGeometryRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioGeometryRegistration::RegisterAfterEngineInit()
{
	using namespace HyperAIStudio::Geometry::Private;
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioGeometryContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioGeometry, Verbose,
			TEXT("Geometry exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!IsLoadedGeometryBackendReady())
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioGeometryDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioGeometry, Error,
			TEXT("Geometry adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioGeometryContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioGeometry, Error,
			TEXT("Geometry live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = IsLoadedGeometryBackendReady();
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("GeometryScriptingCore public class and Asset Registry are already available; the probe loaded, opened, and scanned nothing. Core separately owns both blocking any_of gates.")
		: TEXT("The loaded-only Geometry Script backend interface is unavailable.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioGeometry, Error,
			TEXT("Geometry live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioGeometryToolset::StaticClass(),
		FHyperAIStudioGeometryContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioGeometry, Error,
			TEXT("Geometry atomic three-tool owner registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioGeometryRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioGeometryToolset::StaticClass(),
			FHyperAIStudioGeometryContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioGeometry, Error,
				TEXT("Geometry owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioGeometry, Error,
				TEXT("Geometry probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioGeometry, Error,
				TEXT("Geometry adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
