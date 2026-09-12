// Games by Hyper 2026.

#include "HyperAIStudioPaper2DToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPaper2D, Log, All);

namespace HyperAIStudio::Paper2D::Private
{
	constexpr int32 EstimatedBaseReportBytes = 12288;
	constexpr int32 EstimatedReferenceBytes = 1536;
	constexpr int32 EstimatedKeyFrameBytes = 1024;
	constexpr int32 EstimatedLayerBytes = 1536;
	constexpr int32 EstimatedIssueBytes = 2048;

	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	int64 SaturatingAdd(const int64 A, const int64 B)
	{
		return A > MAX_int64 - FMath::Max<int64>(0, B)
			? MAX_int64 : A + FMath::Max<int64>(0, B);
	}

	int64 EstimateStringBytes(const FString& Value)
	{
		return SaturatingAdd(64, static_cast<int64>(Value.Len()) * 6);
	}

	int64 EstimateSnapshotBytes(const FHyperAIStudioPaper2DValueSnapshot& Snapshot)
	{
		int64 Bytes = EstimatedBaseReportBytes;
		for (const FHyperAIPaper2DReferenceRecord& Reference : Snapshot.References)
		{
			Bytes = SaturatingAdd(Bytes, EstimatedReferenceBytes);
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.Role));
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.ObjectPath));
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.ClassPath));
		}
		for (const FHyperAIPaper2DKeyFrameRecord& Frame : Snapshot.KeyFrames)
		{
			Bytes = SaturatingAdd(Bytes, EstimatedKeyFrameBytes);
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Frame.SpritePath));
		}
		for (const FHyperAIPaper2DLayerRecord& Layer : Snapshot.Layers)
		{
			Bytes = SaturatingAdd(Bytes, EstimatedLayerBytes);
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Layer.LayerPath));
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Layer.LayerName));
		}
		for (const FHyperAIPaper2DIssue& Issue : Snapshot.CaptureIssues)
		{
			Bytes = SaturatingAdd(Bytes, EstimatedIssueBytes);
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Code));
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Subject));
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Detail));
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

	FString DoubleToken(const double Value)
	{
		uint64 Bits = 0;
		static_assert(sizeof(Bits) == sizeof(Value));
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(Bits));
	}

	FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	bool IsBoundedReferencePath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.Len() > FHyperAIStudioPaper2DContracts::MaxPathCharacters
			|| !Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("*"))
			|| Path.Contains(TEXT("?")) || Path.Contains(TEXT("\\")))
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
		return true;
	}

	void AddIssue(TArray<FHyperAIPaper2DIssue>& Issues, const TCHAR* Code,
		const TCHAR* Severity, const FString& Subject, const TCHAR* Detail)
	{
		if (Issues.Num() >= FHyperAIStudioPaper2DContracts::MaxIssues)
		{
			return;
		}
		FHyperAIPaper2DIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.Subject = Subject.Left(FHyperAIStudioPaper2DContracts::MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
	}

	bool AddReference(FHyperAIStudioPaper2DValueSnapshot& Snapshot,
		const TCHAR* Role, const UObject* Object)
	{
		if (!Object)
		{
			return true;
		}
		if (Snapshot.References.Num() >= FHyperAIStudioPaper2DContracts::MaxReferences)
		{
			AddIssue(Snapshot.CaptureIssues, TEXT("reference_bound_exceeded"), TEXT("error"),
				Snapshot.Asset.TargetPath, TEXT("Paper2D reference projection exceeded its hard bound."));
			return false;
		}
		const FString Path = ObjectPath(Object);
		const FString ClassPath = Object->GetClass()->GetPathName();
		if (!IsBoundedReferencePath(Path)
			|| !IsBoundedReferencePath(ClassPath))
		{
			AddIssue(Snapshot.CaptureIssues, TEXT("unsafe_reference_identity"), TEXT("error"),
				Snapshot.Asset.TargetPath, TEXT("A loaded Paper2D reference has an unsafe or oversized identity."));
			return false;
		}
		FHyperAIPaper2DReferenceRecord& Record = Snapshot.References.AddDefaulted_GetRef();
		Record.Role = Role;
		Record.ObjectPath = Path;
		Record.ClassPath = ClassPath;
		return true;
	}

	FString BuildObservationFingerprint(const FHyperAIStudioPaper2DValueSnapshot& Snapshot)
	{
		const FHyperAIPaper2DAssetRecord& Asset = Snapshot.Asset;
		FString Canonical;
		Canonical.Reserve(32768);
		for (const FString* Value : {&Asset.TargetPath, &Asset.ClassPath, &Asset.PackageName,
			&Asset.AssetKind, &Asset.DefaultMaterialPath, &Asset.AlternateMaterialPath,
			&Asset.BodySetupPath, &Asset.TileSheetPath})
		{
			AppendToken(Canonical, *Value);
		}
		for (const double Value : {Asset.PixelsPerUnrealUnit, Asset.CollisionThickness,
			Asset.FramesPerSecond, Asset.SourceWidth, Asset.SourceHeight})
		{
			AppendToken(Canonical, DoubleToken(Value));
		}
		for (const int32 Value : {Asset.CollisionMode, Asset.NumFrames, Asset.NumKeyFrames,
			Asset.TileSheetWidth, Asset.TileSheetHeight, Asset.TileWidth, Asset.TileHeight,
			Asset.TileCountX, Asset.TileCountY, Asset.TileCount, Asset.MarginLeft,
			Asset.MarginTop, Asset.MarginRight, Asset.MarginBottom, Asset.SpacingX,
			Asset.SpacingY, Asset.DrawingOffsetX, Asset.DrawingOffsetY, Asset.MapWidth,
			Asset.MapHeight, Asset.ProjectionMode, Asset.LayerCount,
			Asset.CollidingLayerCount, Asset.RenderVertexCount})
		{
			AppendToken(Canonical, FString::FromInt(Value));
		}
		for (const FHyperAIPaper2DReferenceRecord& Reference : Snapshot.References)
		{
			AppendToken(Canonical, Reference.Role);
			AppendToken(Canonical, Reference.ObjectPath);
			AppendToken(Canonical, Reference.ClassPath);
		}
		for (const FHyperAIPaper2DKeyFrameRecord& Frame : Snapshot.KeyFrames)
		{
			AppendToken(Canonical, FString::FromInt(Frame.Index));
			AppendToken(Canonical, FString::FromInt(Frame.FrameRun));
			AppendToken(Canonical, Frame.SpritePath);
		}
		for (const FHyperAIPaper2DLayerRecord& Layer : Snapshot.Layers)
		{
			AppendToken(Canonical, FString::FromInt(Layer.Index));
			AppendToken(Canonical, Layer.LayerPath);
			AppendToken(Canonical, Layer.LayerName);
			AppendToken(Canonical, FString::FromInt(Layer.Width));
			AppendToken(Canonical, FString::FromInt(Layer.Height));
			AppendToken(Canonical, Layer.bCollides ? TEXT("1") : TEXT("0"));
			AppendToken(Canonical, Layer.bVisibleInEditor ? TEXT("1") : TEXT("0"));
			AppendToken(Canonical, Layer.bVisibleInGame ? TEXT("1") : TEXT("0"));
		}
		return HashCanonical(Canonical);
	}

	FString BuildPersistedRevision(const FHyperAIPaper2DAssetRecord& Asset)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.paper2d.persisted-revision.v1"));
		AppendToken(Canonical, Asset.TargetPath);
		AppendToken(Canonical, Asset.PackageName);
		AppendToken(Canonical, Asset.PackageSavedHash);
		AppendToken(Canonical, FString::Printf(TEXT("%lld"),
			static_cast<long long>(Asset.DiskSize)));
		AppendToken(Canonical, Asset.VolatileObservationFingerprint);
		return HashCanonical(Canonical);
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	bool IsKnownKind(const FString& Kind)
	{
		return Kind == TEXT("sprite") || Kind == TEXT("flipbook")
			|| Kind == TEXT("tile_set") || Kind == TEXT("tile_map");
	}

	FString ExpectedClassForKind(const FString& Kind)
	{
		if (Kind == TEXT("sprite")) return TEXT("/Script/Paper2D.PaperSprite");
		if (Kind == TEXT("flipbook")) return TEXT("/Script/Paper2D.PaperFlipbook");
		if (Kind == TEXT("tile_set")) return TEXT("/Script/Paper2D.PaperTileSet");
		if (Kind == TEXT("tile_map")) return TEXT("/Script/Paper2D.PaperTileMap");
		return {};
	}

	bool VerifyFastPatch(UObject* Target, const FHyperAIPaper2DMetadataPatch& Patch)
	{
		if (Patch.ExpectedKind == TEXT("flipbook"))
		{
			const UPaperFlipbook* Flipbook = Cast<UPaperFlipbook>(Target);
			return Flipbook && (!Patch.bSetFramesPerSecond
				|| FMath::IsNearlyEqual(Flipbook->GetFramesPerSecond(),
					static_cast<float>(Patch.FramesPerSecond)));
		}
		if (Patch.ExpectedKind == TEXT("tile_set"))
		{
			const UPaperTileSet* TileSet = Cast<UPaperTileSet>(Target);
			return TileSet
				&& (!Patch.bSetTileSize
					|| TileSet->GetTileSize() == FIntPoint(Patch.TileWidth, Patch.TileHeight))
				&& (!Patch.bSetTileSpacing
					|| TileSet->GetPerTileSpacing() == FIntPoint(Patch.SpacingX, Patch.SpacingY))
				&& (!Patch.bSetTileDrawingOffset
					|| TileSet->GetDrawingOffset() == FIntPoint(
						Patch.DrawingOffsetX, Patch.DrawingOffsetY));
		}
		if (Patch.ExpectedKind == TEXT("tile_map"))
		{
			const UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
			return TileMap
				&& (!Patch.bSetCollisionMode
					|| static_cast<int32>(TileMap->GetSpriteCollisionDomain()) == Patch.CollisionMode)
				&& (!Patch.bSetCollisionThickness
					|| FMath::IsNearlyEqual(TileMap->GetCollisionThickness(),
						static_cast<float>(Patch.CollisionThickness)));
		}
		return false;
	}

	void ApplyFastPatch(UObject* Target, const FHyperAIPaper2DMetadataPatch& Patch)
	{
		if (Patch.ExpectedKind == TEXT("flipbook"))
		{
			FScopedFlipbookMutator Mutator(CastChecked<UPaperFlipbook>(Target));
			if (Patch.bSetFramesPerSecond)
			{
				Mutator.FramesPerSecond = static_cast<float>(Patch.FramesPerSecond);
			}
		}
		else if (Patch.ExpectedKind == TEXT("tile_set"))
		{
			UPaperTileSet* TileSet = CastChecked<UPaperTileSet>(Target);
			if (Patch.bSetTileSize)
			{
				TileSet->SetTileSize(FIntPoint(Patch.TileWidth, Patch.TileHeight));
			}
			if (Patch.bSetTileSpacing)
			{
				TileSet->SetPerTileSpacing(FIntPoint(Patch.SpacingX, Patch.SpacingY));
			}
			if (Patch.bSetTileDrawingOffset)
			{
				TileSet->SetDrawingOffset(FIntPoint(
					Patch.DrawingOffsetX, Patch.DrawingOffsetY));
			}
		}
		else
		{
			UPaperTileMap* TileMap = CastChecked<UPaperTileMap>(Target);
			if (Patch.bSetCollisionThickness)
			{
				TileMap->SetCollisionThickness(static_cast<float>(Patch.CollisionThickness));
			}
			if (Patch.bSetCollisionMode)
			{
				TileMap->SetCollisionDomain(
					static_cast<ESpriteCollisionMode::Type>(Patch.CollisionMode));
			}
		}
	}
}

FString FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPaper2D.HyperAIStudioPaper2DToolset");
}

const TArray<FHyperAIStudioPaper2DManifestEntry>&
FHyperAIStudioPaper2DContracts::GetManifest()
{
	static const TArray<FHyperAIStudioPaper2DManifestEntry> Manifest = {
		{TEXT("hyper_paper2d_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_paper2d_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_paper2d_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioPaper2DContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioPaper2DManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TSet<FString> Unique;
	TArray<FString> Names;
	for (const FHyperAIStudioPaper2DManifestEntry& Entry : Manifest)
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

const TArray<FString>& FHyperAIStudioPaper2DContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("EditorToolset.AssetTools.can_edit_asset"),
		TEXT("EditorToolset.AssetTools.delete"),
		TEXT("EditorToolset.AssetTools.duplicate"),
		TEXT("EditorToolset.AssetTools.exists"),
		TEXT("EditorToolset.AssetTools.find_assets"),
		TEXT("EditorToolset.AssetTools.get_asset_class"),
		TEXT("EditorToolset.AssetTools.get_asset_tags"),
		TEXT("EditorToolset.AssetTools.get_dependencies"),
		TEXT("EditorToolset.AssetTools.get_metadata_tags"),
		TEXT("EditorToolset.AssetTools.get_referencers"),
		TEXT("EditorToolset.AssetTools.is_checked_out"),
		TEXT("EditorToolset.AssetTools.is_dirty"),
		TEXT("EditorToolset.AssetTools.load_asset"),
		TEXT("EditorToolset.AssetTools.move"),
		TEXT("EditorToolset.AssetTools.save_assets"),
		TEXT("EditorToolset.AssetTools.update_metadata_tags")};
	return Delegates;
}

TArray<FHyperAIPaper2DCapabilityStatus>
FHyperAIStudioPaper2DContracts::GetCapabilityMatrix()
{
	FHyperAIPaper2DCapabilityStatus Paper2D;
	const bool bFastEdits = !FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
		EHyperAIStudioDomainSafety::Edit);
	Paper2D.bMutationExecutionImplemented = bFastEdits;
	Paper2D.State = bFastEdits ? TEXT("fast_reversible_edits_ready")
		: TEXT("strict_safety_backend_required");
	Paper2D.DelegatedEpicCallables = GetEpicDelegates();
	Paper2D.ClosedCases = {
		TEXT("loaded-only exact Sprite, Flipbook, TileSet, or TileMap metadata snapshot"),
		TEXT("clean persisted CAS with fail-closed non-blocking Asset Registry tri-state"),
		TEXT("fast reversible Flipbook FPS, TileSet geometry, and TileMap collision edits"),
		TEXT("detached structural and authoring-readiness validation")};
	Paper2D.UnsupportedCases = {
		TEXT("Epic-equivalent generic asset find/load/duplicate/move/delete/save and metadata operations"),
		TEXT("asset creation, source-texture loading, atlas slicing, tile-cell edits, or map resize"),
		TEXT("Sprite metadata, Flipbook collision source, and TileMap pixels-per-unit lack public UE 5.8 setters")};
	Paper2D.Remediation = bFastEdits
		? TEXT("Use the fast reversible edit subset or Epic AssetTools for generic lifecycle work.")
		: TEXT("Switch Native Execution Mode to Fast for reversible edits; Strict Safety keeps the staged-backend gate.");
	return {MoveTemp(Paper2D)};
}

bool FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(
	const FHyperAIPaper2DMetadataPatch& Patch, FString& OutReason)
{
	OutReason.Reset();
	if (Patch.ExpectedKind == TEXT("flipbook"))
	{
		if (Patch.bSetCollisionMode)
		{
			OutReason = TEXT("UPaperFlipbook exposes no public UE 5.8 collision-source setter.");
			return false;
		}
		return Patch.bSetFramesPerSecond;
	}
	if (Patch.ExpectedKind == TEXT("tile_set"))
	{
		return Patch.bSetTileSize || Patch.bSetTileSpacing
			|| Patch.bSetTileDrawingOffset;
	}
	if (Patch.ExpectedKind == TEXT("tile_map"))
	{
		if (Patch.bSetPixelsPerUnrealUnit)
		{
			OutReason = TEXT("UPaperTileMap exposes no public UE 5.8 pixels-per-unit setter.");
			return false;
		}
		return Patch.bSetCollisionMode || Patch.bSetCollisionThickness;
	}
	OutReason = TEXT("UPaperSprite exposes getters but no public UE 5.8 metadata setters.");
	return false;
}

bool FHyperAIStudioPaper2DContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?"))
		|| Path.Contains(TEXT(":")) || Path.Contains(TEXT("\\")))
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

bool FHyperAIStudioPaper2DContracts::IsCanonicalSha256(const FString& Value)
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

FString FHyperAIStudioPaper2DContracts::ClassifyAssetRegistryExistence(
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

FString FHyperAIStudioPaper2DContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.inspect.v1|target:string|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioPaper2DContracts::PatchPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.metadata-patch.v1|target:string|base:sha256|kind:enum|closed_patch:typed|semantic:sha256"));
	return Value;
}

FString FHyperAIStudioPaper2DContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.validate.v1|target:string|expected:sha256?|policy:enum|max_issues:int|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioPaper2DContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.inspect-result.v1|asset:record|references:bounded|keyframes:bounded|layers:bounded|issues:bounded"));
	return Value;
}

FString FHyperAIStudioPaper2DContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.mutation-blocked-result.v1|terminal:false|effect_count:zero"));
	return Value;
}

FString FHyperAIStudioPaper2DContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("paper2d.validate-result.v1|valid:bool|complete:bool|validator:sha256|issues:bounded"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPaper2DContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.paper2d.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// The generated paper2d all_of group is blocking and remains core-owned.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_paper2d_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_paper2d_apply_plan"), MutationVariantId,
			PatchPayloadTypeId, PatchPayloadSchemaFingerprint(), MutationResultTypeId,
			MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_paper2d_validate"), ValidateVariantId,
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

FString FHyperAIStudioPaper2DInspectPayload::GetTypeId() const
{
	return FHyperAIStudioPaper2DContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioPaper2DInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPaper2DContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPaper2DInspectPayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 128ll + 2ll * Request.TargetPath.Len());
}

FString FHyperAIStudioPaper2DValidatePayload::GetTypeId() const
{
	return FHyperAIStudioPaper2DContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioPaper2DValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPaper2DContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioPaper2DValidatePayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 160ll + 2ll * (Request.TargetPath.Len()
		+ Request.ExpectedPersistedRevision.Len() + Request.Policy.Len()));
}

FString FHyperAIStudioPaper2DPatchPayload::GetTypeId() const
{
	return FHyperAIStudioPaper2DContracts::PatchPayloadTypeId;
}

FString FHyperAIStudioPaper2DPatchPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPaper2DContracts::PatchPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPaper2DPatchPayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 512ll + 2ll * (TargetPath.Len()
		+ BasePersistedRevision.Len() + Patch.ExpectedKind.Len()
		+ SemanticFingerprint.Len()));
}

FString FHyperAIStudioPaper2DPatchPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioPaper2DPatchPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedRevision = BasePersistedRevision;
	Clone->Patch = Patch;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioPaper2DInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioPaper2DContracts::InspectResultTypeId;
}

FString FHyperAIStudioPaper2DInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPaper2DContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioPaper2DInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Paper2D::Private;
	if (Report.References.Num() > FHyperAIStudioPaper2DContracts::MaxReferences
		|| Report.KeyFrames.Num() > FHyperAIStudioPaper2DContracts::MaxKeyFrames
		|| Report.Layers.Num() > FHyperAIStudioPaper2DContracts::MaxLayers
		|| Report.Issues.Num() > FHyperAIStudioPaper2DContracts::MaxIssues)
	{
		return MAX_int32;
	}
	int64 Bytes = EstimatedBaseReportBytes;
	for (const FHyperAIPaper2DReferenceRecord& Reference : Report.References)
	{
		Bytes = SaturatingAdd(Bytes, EstimatedReferenceBytes);
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.Role));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.ObjectPath));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Reference.ClassPath));
	}
	for (const FHyperAIPaper2DKeyFrameRecord& Frame : Report.KeyFrames)
	{
		Bytes = SaturatingAdd(Bytes, EstimatedKeyFrameBytes);
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Frame.SpritePath));
	}
	for (const FHyperAIPaper2DLayerRecord& Layer : Report.Layers)
	{
		Bytes = SaturatingAdd(Bytes, EstimatedLayerBytes);
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Layer.LayerPath));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Layer.LayerName));
	}
	for (const FHyperAIPaper2DIssue& Issue : Report.Issues)
	{
		Bytes = SaturatingAdd(Bytes, EstimatedIssueBytes);
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Detail));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

FString FHyperAIStudioPaper2DValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioPaper2DContracts::ValidateResultTypeId;
}

FString FHyperAIStudioPaper2DValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPaper2DContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioPaper2DValidateResultPayload::GetBoundedByteSize() const
{
	int64 Bytes = 4096;
	for (const FHyperAIPaper2DIssue& Issue : Report.Issues)
	{
		Bytes = HyperAIStudio::Paper2D::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Paper2D::Private::EstimatedIssueBytes);
		Bytes = HyperAIStudio::Paper2D::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Paper2D::Private::EstimateStringBytes(Issue.Detail));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

bool FHyperAIStudioPaper2DContracts::CaptureExact(const FString& TargetPath,
	const int32 MaxWorkMs, FHyperAIStudioPaper2DValueSnapshot& OutSnapshot,
	FString& OutStatus, FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Paper2D::Private;
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
			TEXT("Exact loaded Paper2D capture is game-thread only."));
	}
	if (!IsCanonicalProjectObjectPath(TargetPath) || MaxWorkMs < 1
		|| MaxWorkMs > MaxReadGameThreadMs)
	{
		return Fail(TEXT("invalid_capture_bounds"),
			TEXT("Capture requires one canonical top-level /Game object path and a closed work budget."));
	}
	const double DeadlineSeconds = FPlatformTime::Seconds() + MaxWorkMs / 1000.0;
	UObject* Resolved = FSoftObjectPath(TargetPath).ResolveObject();
	if (!Resolved)
	{
		return Fail(TEXT("target_not_loaded"),
			TEXT("The exact Paper2D asset is not already loaded; HyperAI never loads, opens, searches, or scans for it."));
	}
	UPackage* Package = Resolved->GetOutermost();
	if (!Package || Package == GetTransientPackage() || Resolved->GetOuter() != Package
		|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor))
	{
		return Fail(TEXT("unsupported_target_package"),
			TEXT("Only exact top-level persisted Paper2D assets are admitted."));
	}
	if (Resolved->GetPathName() != TargetPath)
	{
		return Fail(TEXT("target_identity_mismatch"),
			TEXT("The resolved loaded object does not match the exact requested identity."));
	}

	FHyperAIPaper2DAssetRecord& Asset = OutSnapshot.Asset;
	Asset.TargetPath = TargetPath;
	Asset.ClassPath = Resolved->GetClass()->GetPathName();
	Asset.PackageName = Package->GetName();
	Asset.bLoaded = true;
	Asset.bWasLoadedFromDisk = Resolved->HasAnyFlags(RF_WasLoaded);
	Asset.bPackageDirty = Package->IsDirty();
	bool bComplete = true;

	if (Resolved->GetClass() == UPaperSprite::StaticClass())
	{
		UPaperSprite* Sprite = CastChecked<UPaperSprite>(Resolved);
		Asset.AssetKind = TEXT("sprite");
		if (Sprite->BakedRenderData.Num() < 0
			|| Sprite->BakedRenderData.Num() > MaxRenderVertices)
		{
			return Fail(TEXT("sprite_render_data_bound_exceeded"),
				TEXT("Sprite baked render vertex count exceeded its hard bound before projection."));
		}
		Asset.RenderVertexCount = Sprite->BakedRenderData.Num();
		Asset.PixelsPerUnrealUnit = Sprite->GetPixelsPerUnrealUnit();
#if WITH_EDITOR
		const FVector2D SourceSize = Sprite->GetSourceSize();
		Asset.SourceWidth = SourceSize.X;
		Asset.SourceHeight = SourceSize.Y;
		Asset.CollisionMode = static_cast<int32>(Sprite->GetSpriteCollisionDomain());
		Asset.CollisionThickness = Sprite->GetCollisionThickness();
#endif
		Asset.DefaultMaterialPath = ObjectPath(Sprite->GetDefaultMaterial());
		Asset.AlternateMaterialPath = ObjectPath(Sprite->GetAlternateMaterial());
		Asset.BodySetupPath = ObjectPath(Sprite->BodySetup);
		bComplete &= AddReference(OutSnapshot, TEXT("default_material"),
			Sprite->GetDefaultMaterial());
		bComplete &= AddReference(OutSnapshot, TEXT("alternate_material"),
			Sprite->GetAlternateMaterial());
		bComplete &= AddReference(OutSnapshot, TEXT("body_setup"), Sprite->BodySetup);
	}
	else if (Resolved->GetClass() == UPaperFlipbook::StaticClass())
	{
		UPaperFlipbook* Flipbook = CastChecked<UPaperFlipbook>(Resolved);
		Asset.AssetKind = TEXT("flipbook");
		const int32 KeyFrameCount = Flipbook->GetNumKeyFrames();
		if (KeyFrameCount < 0 || KeyFrameCount > MaxKeyFrames)
		{
			return Fail(TEXT("flipbook_keyframe_bound_exceeded"),
				TEXT("Flipbook keyframe count exceeded its hard bound before materialization."));
		}
		Asset.NumKeyFrames = KeyFrameCount;
		Asset.FramesPerSecond = Flipbook->GetFramesPerSecond();
		Asset.CollisionMode = static_cast<int32>(Flipbook->GetCollisionSource());
		Asset.DefaultMaterialPath = ObjectPath(Flipbook->GetDefaultMaterial());
		OutSnapshot.KeyFrames.Reserve(KeyFrameCount);
		OutSnapshot.References.Reserve(KeyFrameCount + 1);
		int64 TotalFrames = 0;
		for (int32 Index = 0; Index < KeyFrameCount; ++Index)
		{
			if (DeadlineExceeded(DeadlineSeconds))
			{
				return Fail(TEXT("capture_deadline_exceeded"),
					TEXT("Flipbook metadata capture exceeded its closed deadline."));
			}
			const FPaperFlipbookKeyFrame& Source = Flipbook->GetKeyFrameChecked(Index);
			FHyperAIPaper2DKeyFrameRecord& Frame = OutSnapshot.KeyFrames.AddDefaulted_GetRef();
			Frame.Index = Index;
			Frame.FrameRun = Source.FrameRun;
			Frame.SpritePath = ObjectPath(Source.Sprite);
			TotalFrames += Source.FrameRun;
			if (TotalFrames < 0 || TotalFrames > MAX_int32)
			{
				bComplete = false;
				AddIssue(OutSnapshot.CaptureIssues, TEXT("flipbook_frame_total_overflow"),
					TEXT("error"), TargetPath,
					TEXT("Flipbook frame-run total cannot be represented by the closed v1 value model."));
			}
			bComplete &= AddReference(OutSnapshot, TEXT("keyframe_sprite"), Source.Sprite);
		}
		Asset.NumFrames = TotalFrames >= 0 && TotalFrames <= MAX_int32
			? static_cast<int32>(TotalFrames) : INDEX_NONE;
		bComplete &= AddReference(OutSnapshot, TEXT("default_material"),
			Flipbook->GetDefaultMaterial());
	}
	else if (Resolved->GetClass() == UPaperTileSet::StaticClass())
	{
		UPaperTileSet* TileSet = CastChecked<UPaperTileSet>(Resolved);
		Asset.AssetKind = TEXT("tile_set");
		const FIntPoint TileSize = TileSet->GetTileSize();
		const FIntMargin Margin = TileSet->GetMargin();
		const FIntPoint Spacing = TileSet->GetPerTileSpacing();
		const FIntPoint Offset = TileSet->GetDrawingOffset();
		UTexture2D* TileSheet = TileSet->GetTileSheetTexture();
		const FIntPoint SheetSize = TileSheet ? TileSheet->GetImportedSize() : FIntPoint::ZeroValue;
		Asset.TileSheetPath = ObjectPath(TileSheet);
		Asset.TileSheetWidth = SheetSize.X;
		Asset.TileSheetHeight = SheetSize.Y;
		Asset.TileWidth = TileSize.X;
		Asset.TileHeight = TileSize.Y;
		Asset.MarginLeft = Margin.Left;
		Asset.MarginTop = Margin.Top;
		Asset.MarginRight = Margin.Right;
		Asset.MarginBottom = Margin.Bottom;
		Asset.SpacingX = Spacing.X;
		Asset.SpacingY = Spacing.Y;
		Asset.DrawingOffsetX = Offset.X;
		Asset.DrawingOffsetY = Offset.Y;
		Asset.TileCountX = INDEX_NONE;
		Asset.TileCountY = INDEX_NONE;
		Asset.TileCount = INDEX_NONE;
		const int64 DenominatorX = static_cast<int64>(TileSize.X) + Spacing.X;
		const int64 DenominatorY = static_cast<int64>(TileSize.Y) + Spacing.Y;
		const int64 AvailableX = static_cast<int64>(SheetSize.X) - Margin.Left
			- Margin.Right + Spacing.X;
		const int64 AvailableY = static_cast<int64>(SheetSize.Y) - Margin.Top
			- Margin.Bottom + Spacing.Y;
		if (TileSheet && TileSize.X > 0 && TileSize.Y > 0 && Spacing.X >= 0
			&& Spacing.Y >= 0 && Margin.Left >= 0 && Margin.Top >= 0
			&& Margin.Right >= 0 && Margin.Bottom >= 0 && DenominatorX > 0
			&& DenominatorY > 0 && AvailableX >= 0 && AvailableY >= 0)
		{
			const int64 CountX = AvailableX / DenominatorX;
			const int64 CountY = AvailableY / DenominatorY;
			const int64 Count = CountX * CountY;
			if (CountX > MAX_int32 || CountY > MAX_int32 || Count > MaxTiles)
			{
				return Fail(TEXT("tile_count_bound_exceeded"),
					TEXT("TileSet computed tile count exceeded its hard bound before projection."));
			}
			Asset.TileCountX = static_cast<int32>(CountX);
			Asset.TileCountY = static_cast<int32>(CountY);
			Asset.TileCount = static_cast<int32>(Count);
		}
		bComplete &= AddReference(OutSnapshot, TEXT("tile_sheet"), TileSheet);
	}
	else if (Resolved->GetClass() == UPaperTileMap::StaticClass())
	{
		UPaperTileMap* TileMap = CastChecked<UPaperTileMap>(Resolved);
		Asset.AssetKind = TEXT("tile_map");
		const int32 LayerCount = TileMap->TileLayers.Num();
		if (LayerCount < 0 || LayerCount > MaxLayers)
		{
			return Fail(TEXT("tile_map_layer_bound_exceeded"),
				TEXT("TileMap layer count exceeded its hard bound before materialization."));
		}
		Asset.MapWidth = TileMap->MapWidth;
		Asset.MapHeight = TileMap->MapHeight;
		Asset.TileWidth = TileMap->TileWidth;
		Asset.TileHeight = TileMap->TileHeight;
		Asset.PixelsPerUnrealUnit = TileMap->GetPixelsPerUnrealUnit();
		Asset.ProjectionMode = static_cast<int32>(TileMap->ProjectionMode.GetValue());
		Asset.CollisionMode = static_cast<int32>(TileMap->GetSpriteCollisionDomain());
		Asset.CollisionThickness = TileMap->GetCollisionThickness();
		Asset.DefaultMaterialPath = ObjectPath(TileMap->Material);
		Asset.BodySetupPath = ObjectPath(TileMap->BodySetup);
		Asset.LayerCount = LayerCount;
		OutSnapshot.Layers.Reserve(LayerCount);
		OutSnapshot.References.Reserve(LayerCount + 2);
		bComplete &= AddReference(OutSnapshot, TEXT("material"), TileMap->Material);
		bComplete &= AddReference(OutSnapshot, TEXT("body_setup"), TileMap->BodySetup);
		for (int32 Index = 0; Index < LayerCount; ++Index)
		{
			if (DeadlineExceeded(DeadlineSeconds))
			{
				return Fail(TEXT("capture_deadline_exceeded"),
					TEXT("TileMap layer capture exceeded its closed deadline."));
			}
			UPaperTileLayer* Source = TileMap->TileLayers[Index];
			FHyperAIPaper2DLayerRecord& Layer = OutSnapshot.Layers.AddDefaulted_GetRef();
			Layer.Index = Index;
			if (!Source)
			{
				AddIssue(OutSnapshot.CaptureIssues, TEXT("null_tile_layer"), TEXT("error"),
					FString::FromInt(Index), TEXT("The bounded TileMap layer array contains a null layer."));
				continue;
			}
			Layer.LayerPath = Source->GetPathName();
			Layer.LayerName = Source->LayerName.ToString();
			if (!IsBoundedReferencePath(Layer.LayerPath)
				|| Layer.LayerName.Len() > MaxNameCharacters)
			{
				bComplete = false;
				Layer.LayerName.Reset();
				AddIssue(OutSnapshot.CaptureIssues, TEXT("unsafe_layer_identity"), TEXT("error"),
					FString::FromInt(Index), TEXT("TileMap layer identity exceeded the closed value bound."));
			}
			Layer.Width = Source->GetLayerWidth();
			Layer.Height = Source->GetLayerHeight();
			Layer.bCollides = Source->GetLayerCollides();
#if WITH_EDITORONLY_DATA
			Layer.bVisibleInEditor = Source->ShouldRenderInEditor();
#else
			Layer.bVisibleInEditor = false;
#endif
			Layer.bVisibleInGame = Source->ShouldRenderInGame();
			Asset.CollidingLayerCount += Layer.bCollides ? 1 : 0;
			bComplete &= AddReference(OutSnapshot, TEXT("tile_layer"), Source);
		}
	}
	else
	{
		return Fail(TEXT("target_class_mismatch"),
			TEXT("The exact loaded top-level object is not a supported PaperSprite, PaperFlipbook, PaperTileSet, or PaperTileMap."));
	}

	if (DeadlineExceeded(DeadlineSeconds))
	{
		return Fail(TEXT("capture_deadline_exceeded"),
			TEXT("Paper2D metadata capture exceeded its closed deadline."));
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	FAssetPackageData PackageData;
	UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
	if (AssetRegistry)
	{
		PackageState = AssetRegistry->TryGetAssetPackageData(
			Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
	}
	Asset.DiskExistence = ClassifyAssetRegistryExistence(PackageState);
	if (!AssetRegistry || PackageState != UE::AssetRegistry::EExists::Exists
		|| !Asset.bWasLoadedFromDisk)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("asset_registry_presence_unproven"),
			TEXT("error"), TargetPath,
			TEXT("TryGetAssetPackageData(..., true) must return Exists and the exact object must carry RF_WasLoaded; Unknown is never absence."));
	}
	else
	{
		Asset.DiskSize = PackageData.DiskSize;
		Asset.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
		if (Asset.DiskSize <= 0 || PackageData.GetPackageSavedHash().IsZero()
			|| Asset.PackageSavedHash.IsEmpty())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("saved_package_identity_unavailable"),
				TEXT("error"), TargetPath,
				TEXT("Asset Registry presence lacks a nonzero package saved hash or disk size."));
		}
	}
	if (Asset.bPackageDirty)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("dirty_loaded_state_not_cas_complete"),
			TEXT("error"), TargetPath,
			TEXT("Dirty loaded Paper2D values cannot claim a persisted CAS revision."));
	}
	Asset.VolatileObservationFingerprint = BuildObservationFingerprint(OutSnapshot);
	if (!IsCanonicalSha256(Asset.VolatileObservationFingerprint))
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("volatile_seal_failed"), TEXT("error"),
			TargetPath, TEXT("The bounded Paper2D value projection could not be sealed."));
	}
	Asset.bProjectionComplete = bComplete;
	if (bComplete)
	{
		Asset.PersistedRevision = BuildPersistedRevision(Asset);
		Asset.bRevisionComplete = IsCanonicalSha256(Asset.PersistedRevision);
		bComplete &= Asset.bRevisionComplete;
	}
	OutSnapshot.bComplete = bComplete;
	Asset.bProjectionComplete = bComplete;
	OutStatus = bComplete ? TEXT("ok") : TEXT("snapshot_incomplete");
	OutDiagnostic = bComplete
		? TEXT("Captured one exact loaded Paper2D asset without loading or scanning.")
		: TEXT("Captured bounded volatile Paper2D values, but persisted identity or projection evidence is incomplete.");
	return true;
}

bool FHyperAIStudioPaper2DContracts::ValidateDetached(
	const FHyperAIStudioPaper2DValueSnapshot& Snapshot, const FString& Policy,
	const int32 MaxIssueCount, const int32 OutputByteLimit,
	FHyperAIPaper2DValidateReport& OutReport)
{
	using namespace HyperAIStudio::Paper2D::Private;
	OutReport = {};
	OutReport.Policy = Policy;
	OutReport.PersistedRevision = Snapshot.Asset.PersistedRevision;
	if ((Policy != TEXT("structural") && Policy != TEXT("authoring_ready"))
		|| MaxIssueCount < 1 || MaxIssueCount > MaxIssues
		|| OutputByteLimit < MinOutputBytes || OutputByteLimit > MaxOutputBytes)
	{
		OutReport.Status = TEXT("invalid_validator_bounds_or_policy");
		OutReport.Diagnostic = TEXT("Validator requires a closed policy, issue bound, and output budget.");
		return false;
	}
	auto Add = [&](const TCHAR* Code, const TCHAR* Severity, const FString& Subject,
		const TCHAR* Detail)
	{
		if (FCString::Stricmp(Severity, TEXT("error")) == 0)
		{
			++OutReport.ErrorCount;
		}
		else
		{
			++OutReport.WarningCount;
		}
		if (OutReport.Issues.Num() >= MaxIssueCount)
		{
			OutReport.bTruncated = true;
			return;
		}
		FHyperAIPaper2DIssue& Issue = OutReport.Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.Subject = Subject.Left(MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
	};
	for (const FHyperAIPaper2DIssue& Issue : Snapshot.CaptureIssues)
	{
		Add(*Issue.Code, *Issue.Severity, Issue.Subject, *Issue.Detail);
	}
	const FHyperAIPaper2DAssetRecord& Asset = Snapshot.Asset;
	if (!IsCanonicalProjectObjectPath(Asset.TargetPath)
		|| !IsKnownKind(Asset.AssetKind)
		|| Asset.ClassPath != ExpectedClassForKind(Asset.AssetKind))
	{
		Add(TEXT("invalid_asset_identity"), TEXT("error"), Asset.TargetPath,
			TEXT("Detached asset path, kind, and exact Paper2D class must agree."));
	}
	if (!IsCanonicalSha256(Asset.VolatileObservationFingerprint))
	{
		Add(TEXT("invalid_volatile_seal"), TEXT("error"), Asset.TargetPath,
			TEXT("Detached Paper2D values lack a canonical volatile observation seal."));
	}
	if (Asset.CollisionMode < 0 || Asset.CollisionMode > 2)
	{
		Add(TEXT("invalid_collision_mode"), TEXT("error"), Asset.TargetPath,
			TEXT("Paper2D collision mode is outside the exact UE 5.8 enum range."));
	}
	if (Asset.AssetKind == TEXT("sprite"))
	{
		if (!IsFinite(Asset.PixelsPerUnrealUnit) || Asset.PixelsPerUnrealUnit <= 0.0
			|| !IsFinite(Asset.SourceWidth) || !IsFinite(Asset.SourceHeight)
			|| Asset.SourceWidth <= 0.0 || Asset.SourceHeight <= 0.0
			|| Asset.RenderVertexCount < 0 || Asset.RenderVertexCount > MaxRenderVertices
			|| Asset.RenderVertexCount % 3 != 0)
		{
			Add(TEXT("invalid_sprite_geometry_metadata"), TEXT("error"), Asset.TargetPath,
				TEXT("Sprite scale, source extent, or triangle vertex metadata is invalid."));
		}
		if (Asset.CollisionMode == 2
			&& (!IsFinite(Asset.CollisionThickness) || Asset.CollisionThickness <= 0.0))
		{
			Add(TEXT("invalid_sprite_collision_thickness"), TEXT("error"), Asset.TargetPath,
				TEXT("A 3D-collision sprite requires positive finite extrusion thickness."));
		}
		if (Policy == TEXT("authoring_ready") && Asset.DefaultMaterialPath.IsEmpty())
		{
			Add(TEXT("sprite_material_missing"), TEXT("error"), Asset.TargetPath,
				TEXT("Authoring-ready sprite validation requires a default material reference."));
		}
	}
	else if (Asset.AssetKind == TEXT("flipbook"))
	{
		if (!IsFinite(Asset.FramesPerSecond) || Asset.FramesPerSecond <= 0.0
			|| Asset.FramesPerSecond > 1000.0 || Asset.NumKeyFrames < 1
			|| Asset.NumKeyFrames > MaxKeyFrames
			|| Asset.NumKeyFrames != Snapshot.KeyFrames.Num() || Asset.NumFrames < 1)
		{
			Add(TEXT("invalid_flipbook_timing"), TEXT("error"), Asset.TargetPath,
				TEXT("Flipbook frame rate, frame total, or bounded keyframe count is invalid."));
		}
		int64 Total = 0;
		for (int32 Index = 0; Index < Snapshot.KeyFrames.Num(); ++Index)
		{
			const FHyperAIPaper2DKeyFrameRecord& Frame = Snapshot.KeyFrames[Index];
			if (Frame.Index != Index || Frame.FrameRun < 1
				|| !IsBoundedReferencePath(Frame.SpritePath))
			{
				Add(TEXT("invalid_flipbook_keyframe"), TEXT("error"),
					FString::FromInt(Index),
					TEXT("Every keyframe needs a stable index, positive frame run, and loaded sprite reference."));
			}
			Total += Frame.FrameRun;
		}
		if (Total != Asset.NumFrames)
		{
			Add(TEXT("flipbook_frame_total_mismatch"), TEXT("error"), Asset.TargetPath,
				TEXT("Detached keyframe runs do not equal the captured frame total."));
		}
		if (Policy == TEXT("authoring_ready") && Asset.DefaultMaterialPath.IsEmpty())
		{
			Add(TEXT("flipbook_material_missing"), TEXT("error"), Asset.TargetPath,
				TEXT("Authoring-ready flipbook validation requires a default material reference."));
		}
	}
	else if (Asset.AssetKind == TEXT("tile_set"))
	{
		const int64 Product = static_cast<int64>(Asset.TileCountX) * Asset.TileCountY;
		if (!IsBoundedReferencePath(Asset.TileSheetPath) || Asset.TileSheetWidth < 1
			|| Asset.TileSheetHeight < 1 || Asset.TileWidth < 1 || Asset.TileHeight < 1
			|| Asset.SpacingX < 0 || Asset.SpacingY < 0 || Asset.MarginLeft < 0
			|| Asset.MarginTop < 0 || Asset.MarginRight < 0 || Asset.MarginBottom < 0
			|| Asset.TileCountX < 1 || Asset.TileCountY < 1 || Asset.TileCount < 1
			|| Asset.TileCount > MaxTiles || Product != Asset.TileCount)
		{
			Add(TEXT("invalid_tile_set_layout"), TEXT("error"), Asset.TargetPath,
				TEXT("TileSet sheet, tile size, spacing, margin, or computed tile counts are invalid."));
		}
	}
	else if (Asset.AssetKind == TEXT("tile_map"))
	{
		if (Asset.MapWidth < 1 || Asset.MapWidth > 1024
			|| Asset.MapHeight < 1 || Asset.MapHeight > 1024
			|| Asset.TileWidth < 1 || Asset.TileHeight < 1
			|| !IsFinite(Asset.PixelsPerUnrealUnit)
			|| Asset.PixelsPerUnrealUnit <= 0.0 || Asset.ProjectionMode < 0
			|| Asset.ProjectionMode > 3 || Asset.LayerCount < 1
			|| Asset.LayerCount > MaxLayers || Asset.LayerCount != Snapshot.Layers.Num())
		{
			Add(TEXT("invalid_tile_map_layout"), TEXT("error"), Asset.TargetPath,
				TEXT("TileMap dimensions, scale, projection, or bounded layer count are invalid."));
		}
		int32 CollidingLayers = 0;
		for (int32 Index = 0; Index < Snapshot.Layers.Num(); ++Index)
		{
			const FHyperAIPaper2DLayerRecord& Layer = Snapshot.Layers[Index];
			if (Layer.Index != Index || !IsBoundedReferencePath(Layer.LayerPath)
				|| Layer.LayerName.IsEmpty() || Layer.LayerName.Len() > MaxNameCharacters
				|| Layer.Width != Asset.MapWidth || Layer.Height != Asset.MapHeight)
			{
				Add(TEXT("invalid_tile_map_layer"), TEXT("error"),
					FString::FromInt(Index),
					TEXT("Each TileMap layer must have a bounded identity and match map dimensions."));
			}
			CollidingLayers += Layer.bCollides ? 1 : 0;
		}
		if (CollidingLayers != Asset.CollidingLayerCount)
		{
			Add(TEXT("colliding_layer_count_mismatch"), TEXT("error"), Asset.TargetPath,
				TEXT("Detached colliding-layer count does not match the layer projection."));
		}
		if (Asset.CollisionMode == 2 && Asset.CollidingLayerCount > 0
			&& (!IsFinite(Asset.CollisionThickness) || Asset.CollisionThickness <= 0.0))
		{
			Add(TEXT("invalid_tile_map_collision_thickness"), TEXT("error"), Asset.TargetPath,
				TEXT("A colliding 3D TileMap requires positive finite extrusion thickness."));
		}
		if (Policy == TEXT("authoring_ready") && Asset.DefaultMaterialPath.IsEmpty())
		{
			Add(TEXT("tile_map_material_missing"), TEXT("error"), Asset.TargetPath,
				TEXT("Authoring-ready TileMap validation requires a material reference."));
		}
	}
	for (const FHyperAIPaper2DReferenceRecord& Reference : Snapshot.References)
	{
		if (Reference.Role.IsEmpty() || Reference.Role.Len() > MaxNameCharacters
			|| !IsBoundedReferencePath(Reference.ObjectPath)
			|| !IsBoundedReferencePath(Reference.ClassPath))
		{
			Add(TEXT("invalid_reference_identity"), TEXT("error"), Reference.Role,
				TEXT("A detached Paper2D dependency has an unsafe or oversized identity."));
		}
	}

	OutReport.bComplete = Snapshot.bComplete && Asset.bProjectionComplete
		&& !OutReport.bTruncated;
	if (Asset.bRevisionComplete && !IsCanonicalSha256(Asset.PersistedRevision))
	{
		Add(TEXT("invalid_persisted_revision"), TEXT("error"), Asset.TargetPath,
			TEXT("Complete persisted Paper2D evidence requires a canonical revision seal."));
		OutReport.bComplete = false;
	}
	if (OutReport.bTruncated)
	{
		OutReport.bComplete = false;
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.paper2d.validator.v1"));
	AppendToken(Canonical, Policy);
	AppendToken(Canonical, Asset.VolatileObservationFingerprint);
	AppendToken(Canonical, Asset.PersistedRevision);
	AppendToken(Canonical, FString::FromInt(OutReport.ErrorCount));
	AppendToken(Canonical, FString::FromInt(OutReport.WarningCount));
	for (const FHyperAIPaper2DIssue& Issue : OutReport.Issues)
	{
		AppendToken(Canonical, Issue.Code);
		AppendToken(Canonical, Issue.Severity);
		AppendToken(Canonical, Issue.Subject);
	}
	OutReport.ValidatorFingerprint = HashCanonical(Canonical);
	OutReport.bOk = IsCanonicalSha256(OutReport.ValidatorFingerprint);
	OutReport.bValid = OutReport.bOk && OutReport.bComplete
		&& OutReport.ErrorCount == 0;
	OutReport.Status = OutReport.bValid ? TEXT("valid")
		: (OutReport.bComplete ? TEXT("invalid") : TEXT("validation_incomplete"));
	OutReport.Diagnostic = OutReport.bValid
		? TEXT("Detached Paper2D value validation passed.")
		: TEXT("Paper2D validation found structural errors or incomplete evidence.");
	return OutReport.bOk;
}

FHyperAIPaper2DInspectReport FHyperAIStudioPaper2DContracts::Inspect(
	const FHyperAIPaper2DInspectRequest& Request)
{
	using namespace HyperAIStudio::Paper2D::Private;
	FHyperAIPaper2DInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Paper2D source cohort remains fail-closed outside admitted or development evidence mode.");
		return Report;
	}
	if (Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_read_bounds");
		Report.Diagnostic = TEXT("Paper2D inspection requires closed work and output bounds.");
		return Report;
	}
	FHyperAIStudioPaper2DValueSnapshot Snapshot;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		Report.Status, Report.Diagnostic))
	{
		return Report;
	}
	if (EstimateSnapshotBytes(Snapshot) > Request.MaxOutputBytes)
	{
		Report.Status = TEXT("output_budget_exceeded");
		Report.Diagnostic = TEXT("The complete bounded Paper2D snapshot exceeds the caller's output budget; no partial snapshot is returned.");
		return Report;
	}
	Report.Asset = MoveTemp(Snapshot.Asset);
	Report.References = MoveTemp(Snapshot.References);
	Report.KeyFrames = MoveTemp(Snapshot.KeyFrames);
	Report.Layers = MoveTemp(Snapshot.Layers);
	Report.Issues = MoveTemp(Snapshot.CaptureIssues);
	Report.bFreshCapture = true;
	Report.bComplete = Snapshot.bComplete;
	Report.bOk = true;
	return Report;
}

FHyperAIPaper2DValidateReport FHyperAIStudioPaper2DContracts::Validate(
	const FHyperAIPaper2DValidateRequest& Request)
{
	FHyperAIPaper2DValidateReport Report;
	Report.Policy = Request.Policy;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Paper2D source cohort remains fail-closed outside admitted or development evidence mode.");
		return Report;
	}
	if (!Request.ExpectedPersistedRevision.IsEmpty()
		&& !IsCanonicalSha256(Request.ExpectedPersistedRevision))
	{
		Report.Status = TEXT("invalid_expected_revision");
		Report.Diagnostic = TEXT("Expected persisted revision must be empty or canonical sha256.");
		return Report;
	}
	FHyperAIStudioPaper2DValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = CaptureDiagnostic;
		return Report;
	}
	if (!Request.ExpectedPersistedRevision.IsEmpty()
		&& Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		Report.Status = TEXT("stale_revision");
		Report.Diagnostic = TEXT("The exact loaded Paper2D asset changed after inspection.");
		return Report;
	}
	if (!ValidateDetached(Snapshot, Request.Policy, Request.MaxIssues,
		Request.MaxOutputBytes, Report))
	{
		return Report;
	}
	Report.bFreshCapture = true;
	Report.Capabilities = GetCapabilityMatrix();
	return Report;
}

FString FHyperAIStudioPaper2DContracts::ComputePatchSemanticFingerprint(
	const FHyperAIStudioPaper2DPatchPayload& Payload)
{
	using namespace HyperAIStudio::Paper2D::Private;
	const FHyperAIPaper2DMetadataPatch& Patch = Payload.Patch;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.paper2d.metadata-patch.semantic.v1"));
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BasePersistedRevision);
	AppendToken(Canonical, Patch.ExpectedKind);
	AppendToken(Canonical, Patch.bSetPixelsPerUnrealUnit ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, DoubleToken(Patch.PixelsPerUnrealUnit));
	AppendToken(Canonical, Patch.bSetCollisionMode ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, FString::FromInt(Patch.CollisionMode));
	AppendToken(Canonical, Patch.bSetCollisionThickness ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, DoubleToken(Patch.CollisionThickness));
	AppendToken(Canonical, Patch.bSetFramesPerSecond ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, DoubleToken(Patch.FramesPerSecond));
	AppendToken(Canonical, Patch.bSetTileSize ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, FString::FromInt(Patch.TileWidth));
	AppendToken(Canonical, FString::FromInt(Patch.TileHeight));
	AppendToken(Canonical, Patch.bSetTileSpacing ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, FString::FromInt(Patch.SpacingX));
	AppendToken(Canonical, FString::FromInt(Patch.SpacingY));
	AppendToken(Canonical, Patch.bSetTileDrawingOffset ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, FString::FromInt(Patch.DrawingOffsetX));
	AppendToken(Canonical, FString::FromInt(Patch.DrawingOffsetY));
	return HashCanonical(Canonical);
}

FHyperAIPaper2DApplyPlanReport FHyperAIStudioPaper2DContracts::BuildPlan(
	const FHyperAIPaper2DApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Paper2D::Private;
	FHyperAIPaper2DApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("The exact Paper2D source cohort remains fail-closed outside admitted or development evidence mode."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Paper2D plan preparation requires one bounded exact game-thread capture."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty()
		|| !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("Dry-run preparation prohibits operation_id and expected_plan_hash."));
	}
	if (!Request.bDryRun
		&& (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
			|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_identity"),
			TEXT("Non-dry intent requires one valid operation_id and exact dry-run plan hash."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| Request.DeadlineMs < MinPrepareDeadlineMs
		|| Request.DeadlineMs > MaxPrepareDeadlineMs
		|| Request.MaxGameThreadMs < 25
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_bounds_or_identity"),
			TEXT("Plan preparation requires canonical target/CAS identity and closed deadline/work/output bounds."));
	}
	const FHyperAIPaper2DMetadataPatch& Patch = Request.Patch;
	const int32 PatchFieldCount = static_cast<int32>(Patch.bSetPixelsPerUnrealUnit)
		+ static_cast<int32>(Patch.bSetCollisionMode)
		+ static_cast<int32>(Patch.bSetCollisionThickness)
		+ static_cast<int32>(Patch.bSetFramesPerSecond)
		+ static_cast<int32>(Patch.bSetTileSize)
		+ static_cast<int32>(Patch.bSetTileSpacing)
		+ static_cast<int32>(Patch.bSetTileDrawingOffset);
	if (!IsKnownKind(Patch.ExpectedKind) || PatchFieldCount < 1)
	{
		return Reject(TEXT("invalid_or_empty_patch"),
			TEXT("The closed Paper2D patch requires one exact asset kind and at least one metadata field."));
	}
	if ((Patch.bSetPixelsPerUnrealUnit
			&& (!IsFinite(Patch.PixelsPerUnrealUnit)
				|| Patch.PixelsPerUnrealUnit <= 0.0 || Patch.PixelsPerUnrealUnit > 10000.0))
		|| (Patch.bSetCollisionMode
			&& (Patch.CollisionMode < 0 || Patch.CollisionMode > 2))
		|| (Patch.bSetCollisionThickness
			&& (!IsFinite(Patch.CollisionThickness)
				|| Patch.CollisionThickness < 0.0 || Patch.CollisionThickness > 100000.0))
		|| (Patch.bSetFramesPerSecond
			&& (!IsFinite(Patch.FramesPerSecond)
				|| Patch.FramesPerSecond <= 0.0 || Patch.FramesPerSecond > 1000.0))
		|| (Patch.bSetTileSize && (Patch.TileWidth < 1 || Patch.TileWidth > 8192
			|| Patch.TileHeight < 1 || Patch.TileHeight > 8192))
		|| (Patch.bSetTileSpacing && (Patch.SpacingX < 0 || Patch.SpacingX > 8192
			|| Patch.SpacingY < 0 || Patch.SpacingY > 8192))
		|| (Patch.bSetTileDrawingOffset
			&& (Patch.DrawingOffsetX < -8192 || Patch.DrawingOffsetX > 8192
				|| Patch.DrawingOffsetY < -8192 || Patch.DrawingOffsetY > 8192)))
	{
		return Reject(TEXT("patch_value_out_of_bounds"),
			TEXT("A selected Paper2D metadata value is outside its closed finite envelope."));
	}
	const bool bKindApplicable =
		(Patch.ExpectedKind == TEXT("sprite")
			&& !Patch.bSetFramesPerSecond && !Patch.bSetTileSize
			&& !Patch.bSetTileSpacing && !Patch.bSetTileDrawingOffset)
		|| (Patch.ExpectedKind == TEXT("flipbook")
			&& !Patch.bSetPixelsPerUnrealUnit && !Patch.bSetCollisionThickness
			&& !Patch.bSetTileSize && !Patch.bSetTileSpacing
			&& !Patch.bSetTileDrawingOffset)
		|| (Patch.ExpectedKind == TEXT("tile_set")
			&& !Patch.bSetPixelsPerUnrealUnit && !Patch.bSetCollisionMode
			&& !Patch.bSetCollisionThickness && !Patch.bSetFramesPerSecond)
		|| (Patch.ExpectedKind == TEXT("tile_map")
			&& !Patch.bSetFramesPerSecond && !Patch.bSetTileSize
			&& !Patch.bSetTileSpacing && !Patch.bSetTileDrawingOffset);
	if (!bKindApplicable)
	{
		return Reject(TEXT("patch_not_applicable_to_kind"),
			TEXT("The selected closed metadata fields do not apply to the asserted Paper2D asset kind."));
	}

	FHyperAIStudioPaper2DValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath,
		FMath::Min(Request.MaxGameThreadMs, MaxReadGameThreadMs), Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Report.Issues = Snapshot.CaptureIssues;
	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete
		|| Snapshot.Asset.bPackageDirty || Snapshot.Asset.DiskExistence != TEXT("exists"))
	{
		return Reject(TEXT("persisted_clean_base_required"),
			TEXT("Paper2D planning requires one complete clean loaded CAS snapshot with proven non-blocking Asset Registry presence."));
	}
	if (Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The exact loaded Paper2D asset changed after inspection."));
	}
	if (Snapshot.Asset.AssetKind != Patch.ExpectedKind)
	{
		return Reject(TEXT("asset_kind_mismatch"),
			TEXT("The exact loaded Paper2D kind no longer matches the preflight assertion."));
	}
	const FHyperAIPaper2DAssetRecord& Asset = Snapshot.Asset;
	bool bNoOp = true;
	if (Patch.bSetPixelsPerUnrealUnit)
		bNoOp &= Asset.PixelsPerUnrealUnit == Patch.PixelsPerUnrealUnit;
	if (Patch.bSetCollisionMode)
		bNoOp &= Asset.CollisionMode == Patch.CollisionMode;
	if (Patch.bSetCollisionThickness)
		bNoOp &= Asset.CollisionThickness == Patch.CollisionThickness;
	if (Patch.bSetFramesPerSecond)
		bNoOp &= Asset.FramesPerSecond == Patch.FramesPerSecond;
	if (Patch.bSetTileSize)
		bNoOp &= Asset.TileWidth == Patch.TileWidth && Asset.TileHeight == Patch.TileHeight;
	if (Patch.bSetTileSpacing)
		bNoOp &= Asset.SpacingX == Patch.SpacingX && Asset.SpacingY == Patch.SpacingY;
	if (Patch.bSetTileDrawingOffset)
		bNoOp &= Asset.DrawingOffsetX == Patch.DrawingOffsetX
			&& Asset.DrawingOffsetY == Patch.DrawingOffsetY;
	if (bNoOp)
	{
		return Reject(TEXT("no_op_patch"),
			TEXT("Every selected Paper2D metadata field already equals the requested value."));
	}

	const TSharedRef<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Payload->Patch = Patch;
	Payload->SemanticFingerprint = ComputePatchSemanticFingerprint(*Payload);
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_fingerprint_failed"),
			TEXT("The closed typed Paper2D metadata patch could not be sealed."));
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || Clone->GetTypeId() != Payload->GetTypeId()
		|| Clone->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
		|| Clone->GetSemanticFingerprint() != Payload->GetSemanticFingerprint())
	{
		return Reject(TEXT("immutable_clone_failed"),
			TEXT("Typed execution requires a detached deep immutable payload clone."));
	}

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetAdapterDescriptor();
	if (ProjectId.IsEmpty() || !IsCanonicalSha256(Descriptor.AdapterFingerprint))
	{
		return Reject(TEXT("preparation_identity_unavailable"),
			TEXT("Canonical project or exact Paper2D adapter identity is unavailable."));
	}
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_paper2d_apply_plan");
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
		{TEXT("plugin.Paper2D"), EHyperAIStudioDomainPrerequisiteState::Available},
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
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.PatchFieldCount = PatchFieldCount;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bDetachedImmutableClone = true;
	Report.Effects.bWouldTransactionOnce = true;
	Report.Effects.bWouldRebuildOnce = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	Report.Effects.bWouldFreshVerifyOnce = true;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_execution_blocked");
		Report.Diagnostic = TEXT("Pure TypedArtifactExecutor::Prepare sealed a detached immutable Paper2D plan. No transaction, mutation, rebuild, compile, save, stage, submission, or trusted execution occurred.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("Non-dry intent does not echo the exact pure dry-run plan hash."));
	}
	if (FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
		EHyperAIStudioDomainSafety::Edit))
	{
		return Reject(NonDryCallableState,
			TEXT("Strict Safety keeps reversible Paper2D edits behind the staged-backend gate."));
	}
	FString UnsupportedReason;
	if (!CanExecuteFastPatch(Patch, UnsupportedReason))
	{
		return Reject(TEXT("public_setter_unavailable"), *UnsupportedReason);
	}

	UObject* Target = FSoftObjectPath(Request.TargetPath).ResolveObject();
	UPackage* Package = Target ? Target->GetOutermost() : nullptr;
	if (!Target || Target->GetPathName() != Request.TargetPath || !Package
		|| Target->GetOuter() != Package
		|| !Package->GetName().StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !Package->IsFullyLoaded())
	{
		return Reject(TEXT("fast_target_unavailable"),
			TEXT("The exact loaded top-level /Game asset must remain fully loaded before mutation."));
	}
	const FString ExpectedClass = ExpectedClassForKind(Patch.ExpectedKind);
	if (Target->GetClass()->GetPathName() != ExpectedClass)
	{
		return Reject(TEXT("fast_target_class_changed"),
			TEXT("The exact loaded target class changed after preflight."));
	}

	{
		const FScopedTransaction Transaction(NSLOCTEXT(
			"HyperAIStudioPaper2D", "FastReversibleEdit",
			"HyperAIStudio Paper2D Edit"));
		Target->Modify();
		ApplyFastPatch(Target, Patch);
		// One editor notification after all public setters; no per-field post-edit work.
		Target->PostEditChange();
		Package->MarkPackageDirty();
	}
	Report.bExecutionSubmitted = true;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GWarn;
	if (!UPackage::SavePackage(Package, Target, *Filename, SaveArgs)
		|| Package->IsDirty())
	{
		Report.Status = TEXT("save_failed_asset_dirty");
		Report.Diagnostic = TEXT("The reversible edit ran, but the one package save failed. Undo or source-control revert is available.");
		return Report;
	}
	if (!VerifyFastPatch(Target, Patch))
	{
		Report.Status = TEXT("saved_postcondition_failed");
		Report.Diagnostic = TEXT("The package saved, but the requested public-setter values failed final verification; source-control revert is available.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = TEXT("executed_fast_reversible_edit");
	Report.Diagnostic = TEXT("Applied one reversible Paper2D transaction through public UE 5.8 setters, notified once, saved once, and verified the requested values once.");
	return Report;
}

FHyperAIPaper2DInspectReport UHyperAIStudioPaper2DToolset::hyper_paper2d_inspect(
	const FHyperAIPaper2DInspectRequest& Request)
{
	return FHyperAIStudioPaper2DContracts::Inspect(Request);
}

FHyperAIPaper2DApplyPlanReport UHyperAIStudioPaper2DToolset::hyper_paper2d_apply_plan(
	const FHyperAIPaper2DApplyPlanRequest& Request)
{
	return FHyperAIStudioPaper2DContracts::BuildPlan(Request);
}

FHyperAIPaper2DValidateReport UHyperAIStudioPaper2DToolset::hyper_paper2d_validate(
	const FHyperAIPaper2DValidateRequest& Request)
{
	return FHyperAIStudioPaper2DContracts::Validate(Request);
}

FHyperAIStudioPaper2DDomainAdapter::FHyperAIStudioPaper2DDomainAdapter()
	: Descriptor(FHyperAIStudioPaper2DContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPaper2DDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioPaper2DDomainAdapter::Execute(
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
			TEXT("Paper2D adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_paper2d_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioPaper2DContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPaper2DContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPaper2DContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPaper2DInspectPayload& Typed =
			static_cast<const FHyperAIStudioPaper2DInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioPaper2DInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPaper2DInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPaper2DContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_paper2d_validate")
		&& Context.Binding.VariantId == FHyperAIStudioPaper2DContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPaper2DContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPaper2DContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioPaper2DValidatePayload& Typed =
			static_cast<const FHyperAIStudioPaper2DValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioPaper2DValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPaper2DValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPaper2DContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_paper2d_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioPaper2DContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioPaper2DContracts::PatchPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPaper2DContracts::PatchPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPaper2DPatchPayload& Typed =
			static_cast<const FHyperAIStudioPaper2DPatchPayload&>(Payload);
		if (FHyperAIStudioPaper2DContracts::ComputePatchSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Paper2D metadata patch semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioPaper2DContracts::NonDryCallableState,
			TEXT("No Paper2D effect ran. This synchronous adapter cannot transaction, mutate, rebuild/compile, save, validate, or fresh-verify."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Paper2D adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioPaper2DRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPaper2DRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPaper2DRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioPaper2DRegistration::IsRegistered() const
{
	return FHyperAIStudioPaper2DContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPaper2DToolset::StaticClass(),
			FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioPaper2DRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioPaper2DRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioPaper2DContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioPaper2D, Verbose,
			TEXT("Paper2D exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (UPaperSprite::StaticClass() == nullptr || UPaperFlipbook::StaticClass() == nullptr
		|| UPaperTileSet::StaticClass() == nullptr || UPaperTileMap::StaticClass() == nullptr
		|| IAssetRegistry::Get() == nullptr)
	{
		return;
	}

	Adapter = MakeShared<FHyperAIStudioPaper2DDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPaper2D, Error,
			TEXT("Paper2D adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioPaper2DContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPaper2D, Error,
			TEXT("Paper2D live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = UPaperSprite::StaticClass() != nullptr
		&& UPaperFlipbook::StaticClass() != nullptr
		&& UPaperTileSet::StaticClass() != nullptr
		&& UPaperTileMap::StaticClass() != nullptr
		&& IAssetRegistry::Get() != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("paper2d_module_unavailable");
	Observation.Diagnostic = Observation.bReady
		? TEXT("Paper2D asset classes and Asset Registry interface are already available; the probe loaded and scanned nothing. Core separately owns the blocking plugin.Paper2D gate.")
		: TEXT("The loaded-only Paper2D backend interface is unavailable.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioPaper2D, Error,
			TEXT("Paper2D live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioPaper2DToolset::StaticClass(),
		FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPaper2D, Error,
			TEXT("Paper2D atomic three-tool owner registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioPaper2DRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPaper2DToolset::StaticClass(),
			FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPaper2D, Error,
				TEXT("Paper2D owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioPaper2D, Error,
				TEXT("Paper2D probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioPaper2D, Error,
				TEXT("Paper2D adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
