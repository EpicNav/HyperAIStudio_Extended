// Games by Hyper 2026.

#include "HyperAIStudioWorldbuildingToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SplineComponent.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "InstancedFoliageActor.h"
#include "Interfaces/IPluginManager.h"
#include "LandscapeProxy.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VT/RuntimeVirtualTextureVolume.h"
#include "WorldPartition/WorldPartition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioWorldbuildingToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioWorldbuilding, Log, All);

namespace HyperAIStudio::Worldbuilding::Private
{
	constexpr int32 MinOutputBytes = 4096;
	constexpr int32 MaxTextCharacters = 512;
	constexpr int32 MaxIssueTextCharacters = 384;
	constexpr double MaxCaptureSeconds = 0.125;

	FString Clip(const FString& Value, const int32 MaxCharacters = MaxTextCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical += TEXT("|");
	}

	void AppendInt(FString& Canonical, const int64 Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Value));
	}

	void AppendBool(FString& Canonical, const bool bValue)
	{
		AppendToken(Canonical, bValue ? TEXT("1") : TEXT("0"));
	}

	void AppendDouble(FString& Canonical, const double Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Value));
	}

	void AppendVector(FString& Canonical, const FVector& Value)
	{
		AppendDouble(Canonical, Value.X);
		AppendDouble(Canonical, Value.Y);
		AppendDouble(Canonical, Value.Z);
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

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsSafeExactObjectPath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.Len() > FHyperAIStudioWorldbuildingContracts::MaxPathCharacters
			|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\"))
			|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")))
		{
			return false;
		}
		for (const TCHAR Character : Path)
		{
			if (Character < TEXT(' ') || Character == TEXT('"'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeName(const FString& Value, const int32 Maximum = 128)
	{
		if (Value.IsEmpty() || Value.Len() > Maximum)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(' ')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsCreateVariant(const FString& Variant)
	{
		return Variant == TEXT("landscape.create") || Variant == TEXT("foliage.add_type")
			|| Variant == TEXT("water.create_body") || Variant == TEXT("spline.create")
			|| Variant == TEXT("level_instance.create") || Variant == TEXT("nav_link.create")
			|| Variant == TEXT("nav_area.create")
			|| Variant == TEXT("optimize.merge") || Variant == TEXT("optimize.proxy")
			|| Variant == TEXT("optimize.instance");
	}

	bool IsRemoveVariant(const FString& Variant)
	{
		return Variant == TEXT("foliage.erase_instances") || Variant == TEXT("foliage.remove_type")
			|| Variant == TEXT("spline.remove_point") || Variant == TEXT("nav_link.remove")
			|| Variant == TEXT("nav_area.remove");
	}

	bool IsWaterVariant(const FString& Variant)
	{
		return Variant.StartsWith(TEXT("water."), ESearchCase::CaseSensitive)
			|| Variant == TEXT("water_body") || Variant == TEXT("water_zone")
			|| Variant == TEXT("water_waves");
	}

	bool LoadedWorldVariantHasBoundedEnumerator(const FString& Variant)
	{
		static const TSet<FString> Captured = {
			TEXT("level"), TEXT("level_instance"), TEXT("world_partition"), TEXT("landscape"),
			TEXT("foliage"), TEXT("spline"), TEXT("water_body"), TEXT("water_zone"),
			TEXT("runtime_virtual_texture"), TEXT("environment")};
		return Captured.Contains(Variant);
	}

	bool LoadedNavigationVariantHasBoundedEnumerator(const FString& Variant)
	{
		return Variant == TEXT("nav_system") || Variant == TEXT("navmesh")
			|| Variant == TEXT("nav_link");
	}

	bool WaterPrerequisiteReady()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Water"));
		return Plugin.IsValid() && Plugin->IsEnabled()
			&& FModuleManager::Get().IsModuleLoaded(TEXT("Water"));
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	void AddIssue(
		TArray<FHyperAIWorldIssue>& Issues,
		const int32 Maximum,
		bool& bOutTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& ObjectPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Maximum)
		{
			bOutTruncated = true;
			return;
		}
		FHyperAIWorldIssue Issue;
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.Variant = Clip(Variant, 96);
		Issue.ObjectPath = Clip(ObjectPath, MaxIssueTextCharacters);
		Issue.StableId = Clip(StableId, MaxIssueTextCharacters);
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message, MaxIssueTextCharacters);
		Issues.Add(MoveTemp(Issue));
	}

	void AddCaptureIssue(
		TArray<FHyperAIWorldIssue>& Issues,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& Message)
	{
		bool bUnused = false;
		AddIssue(Issues, FHyperAIStudioWorldbuildingContracts::MaxIssues, bUnused,
			Code, Severity, Variant, FString(), FString(), -1, Message);
	}

	bool HasError(const TArray<FHyperAIWorldIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIWorldIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	FString WorldRecordCanonical(const FHyperAIWorldRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.Variant);
		AppendToken(Canonical, Record.StableId);
		AppendToken(Canonical, Record.ObjectPath);
		AppendToken(Canonical, Record.PackageName);
		AppendToken(Canonical, Record.ClassPath);
		AppendToken(Canonical, Record.WorldPath);
		AppendToken(Canonical, Record.ParentPath);
		AppendToken(Canonical, Record.DisplayName);
		AppendInt(Canonical, Record.PrimaryCount);
		AppendInt(Canonical, Record.SecondaryCount);
		AppendBool(Canonical, Record.bLoaded);
		AppendBool(Canonical, Record.bDirty);
		AppendBool(Canonical, Record.bEnabled);
		AppendBool(Canonical, Record.bHasBounds);
		if (Record.bHasBounds)
		{
			AppendVector(Canonical, Record.BoundsMin);
			AppendVector(Canonical, Record.BoundsMax);
		}
		TArray<FString> Details = Record.Details;
		Details.Sort();
		for (const FString& Detail : Details)
		{
			AppendToken(Canonical, Detail);
		}
		return Canonical;
	}

	FString NavigationRecordCanonical(const FHyperAINavigationRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.Variant);
		AppendToken(Canonical, Record.StableId);
		AppendToken(Canonical, Record.ObjectPath);
		AppendToken(Canonical, Record.ClassPath);
		AppendInt(Canonical, Record.PrimaryCount);
		AppendInt(Canonical, Record.SecondaryCount);
		AppendBool(Canonical, Record.bLoaded);
		AppendBool(Canonical, Record.bEnabled);
		AppendBool(Canonical, Record.bHasBounds);
		if (Record.bHasBounds)
		{
			AppendVector(Canonical, Record.BoundsMin);
			AppendVector(Canonical, Record.BoundsMax);
		}
		TArray<FString> Details = Record.Details;
		Details.Sort();
		for (const FString& Detail : Details)
		{
			AppendToken(Canonical, Detail);
		}
		return Canonical;
	}

	bool MatchesFilter(const TArray<FString>& Values, const FString& Value)
	{
		return Values.IsEmpty() || Values.Contains(Value);
	}

	bool ValidateStringSet(
		const TArray<FString>& Values,
		const int32 Maximum,
		TFunctionRef<bool(const FString&)> Predicate,
		FString& OutError)
	{
		if (Values.Num() > Maximum)
		{
			OutError = TEXT("A bounded string-list limit was exceeded.");
			return false;
		}
		TSet<FString> Unique;
		for (const FString& Value : Values)
		{
			if (!Predicate(Value) || Unique.Contains(Value))
			{
				OutError = TEXT("A list contains an invalid or duplicate closed value.");
				return false;
			}
			Unique.Add(Value);
		}
		return true;
	}

	bool ValidateProjection(const TArray<FString>& Projection, const bool bNavigation, FString& OutError)
	{
		static const TSet<FString> World = {
			TEXT("identity"), TEXT("package"), TEXT("world"), TEXT("counts"),
			TEXT("bounds"), TEXT("details")};
		static const TSet<FString> Navigation = {
			TEXT("identity"), TEXT("counts"), TEXT("bounds"), TEXT("details")};
		const TSet<FString>& Allowed = bNavigation ? Navigation : World;
		return ValidateStringSet(Projection,
			bNavigation ? FHyperAIStudioNavigationContracts::MaxProjectionFields
				: FHyperAIStudioWorldbuildingContracts::MaxProjectionFields,
			[&](const FString& Value) { return Allowed.Contains(Value); }, OutError);
	}

	void ProjectRecord(FHyperAIWorldRecord& Record, const TArray<FString>& Projection)
	{
		if (Projection.IsEmpty()) return;
		if (!Projection.Contains(TEXT("package"))) Record.PackageName.Reset();
		if (!Projection.Contains(TEXT("world")))
		{
			Record.WorldPath.Reset();
			Record.ParentPath.Reset();
		}
		if (!Projection.Contains(TEXT("counts")))
		{
			Record.PrimaryCount = 0;
			Record.SecondaryCount = 0;
		}
		if (!Projection.Contains(TEXT("bounds")))
		{
			Record.bHasBounds = false;
			Record.BoundsMin = FVector::ZeroVector;
			Record.BoundsMax = FVector::ZeroVector;
		}
		if (!Projection.Contains(TEXT("details"))) Record.Details.Reset();
		if (!Projection.Contains(TEXT("identity")))
		{
			Record.ClassPath.Reset();
			Record.DisplayName.Reset();
		}
	}

	void ProjectRecord(FHyperAINavigationRecord& Record, const TArray<FString>& Projection)
	{
		if (Projection.IsEmpty()) return;
		if (!Projection.Contains(TEXT("counts")))
		{
			Record.PrimaryCount = 0;
			Record.SecondaryCount = 0;
		}
		if (!Projection.Contains(TEXT("bounds")))
		{
			Record.bHasBounds = false;
			Record.BoundsMin = FVector::ZeroVector;
			Record.BoundsMax = FVector::ZeroVector;
		}
		if (!Projection.Contains(TEXT("details"))) Record.Details.Reset();
		if (!Projection.Contains(TEXT("identity"))) Record.ClassPath.Reset();
	}

	int32 SaturatingJsonBytesFromCharacters(const int64 Characters)
	{
		if (Characters < 0 || Characters > MAX_int32 / 6) return MAX_int32;
		return static_cast<int32>(Characters * 6);
	}

	int32 EstimateBytes(const FHyperAIWorldRecord& Record)
	{
		int64 Characters = 320 + Record.Variant.Len() + Record.StableId.Len()
			+ Record.ObjectPath.Len() + Record.PackageName.Len() + Record.ClassPath.Len()
			+ Record.WorldPath.Len() + Record.ParentPath.Len() + Record.DisplayName.Len()
			+ Record.Revision.Len();
		for (const FString& Detail : Record.Details) Characters += Detail.Len() + 16;
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	int32 EstimateBytes(const FHyperAINavigationRecord& Record)
	{
		int64 Characters = 256 + Record.Variant.Len() + Record.StableId.Len()
			+ Record.ObjectPath.Len() + Record.ClassPath.Len() + Record.Revision.Len();
		for (const FString& Detail : Record.Details) Characters += Detail.Len() + 16;
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	int32 EstimateBytes(const FHyperAIWorldIssue& Issue)
	{
		const int64 Characters = 128 + Issue.Code.Len() + Issue.Severity.Len()
			+ Issue.Variant.Len() + Issue.ObjectPath.Len() + Issue.StableId.Len()
			+ Issue.Message.Len();
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	bool ParseCursor(const FString& Cursor, const FString& Revision, int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("|"), false);
		if (Parts.Num() != 3 || Parts[0] != TEXT("v1") || Parts[1] != Revision
			|| !LexTryParseString(OutOffset, *Parts[2]) || OutOffset < 0)
		{
			return false;
		}
		return true;
	}

	FString MakeCursor(const FString& Revision, const int32 Offset)
	{
		return FString::Printf(TEXT("v1|%s|%d"), *Revision, Offset);
	}

	FString ComputeLoadedObjectRevision(const UObject* Object)
	{
		if (!IsValid(Object)) return FString();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.loaded-world-object.v1"));
		AppendToken(Canonical, Object->GetPathName());
		AppendToken(Canonical, Object->GetClass()->GetPathName());
		AppendBool(Canonical, Object->GetPackage() && Object->GetPackage()->IsDirty());
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			AppendVector(Canonical, Actor->GetActorLocation());
			AppendVector(Canonical, Actor->GetActorRotation().Euler());
			AppendVector(Canonical, Actor->GetActorScale3D());
			FVector Origin = FVector::ZeroVector;
			FVector Extent = FVector::ZeroVector;
			Actor->GetActorBounds(false, Origin, Extent);
			AppendVector(Canonical, Origin);
			AppendVector(Canonical, Extent);
		}
		if (const USplineComponent* Spline = Cast<USplineComponent>(Object))
		{
			AppendInt(Canonical, Spline->GetNumberOfSplinePoints());
			AppendBool(Canonical, Spline->IsClosedLoop());
			AppendDouble(Canonical, Spline->GetSplineLength());
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool ValidateLoadedTargetType(const FString& Variant, const UObject* Object, FString& OutError)
	{
		if (!IsValid(Object))
		{
			OutError = TEXT("Loaded target is invalid.");
			return false;
		}
		bool bCompatible = true;
		if (Variant.StartsWith(TEXT("landscape."))) bCompatible = Object->IsA<ALandscapeProxy>();
		else if (Variant.StartsWith(TEXT("foliage.")))
			bCompatible = Object->IsA<AInstancedFoliageActor>() || Object->IsA<UFoliageType>();
		else if (Variant.StartsWith(TEXT("world_partition.")) || Variant == TEXT("hlod.build"))
			bCompatible = Object->IsA<UWorldPartition>();
		else if (Variant == TEXT("data_layer.set_state"))
			bCompatible = Object->GetClass()->GetFName() == TEXT("DataLayerInstance")
				|| Object->GetClass()->GetFName() == TEXT("DataLayerAsset");
		else if (Variant == TEXT("rvt.bind_volume")) bCompatible = Object->IsA<ARuntimeVirtualTextureVolume>();
		else if (Variant.StartsWith(TEXT("water.")))
		{
			const FName Name = Object->GetClass()->GetFName();
			bCompatible = Name == TEXT("WaterBodyRiver") || Name == TEXT("WaterBodyLake")
				|| Name == TEXT("WaterBodyOcean") || Name == TEXT("WaterBodyCustom")
				|| Name == TEXT("WaterZone") || Name == TEXT("WaterWavesAsset")
				|| Name == TEXT("GerstnerWaterWaves");
		}
		else if (Variant.StartsWith(TEXT("spline."))) bCompatible = Object->IsA<USplineComponent>();
		else if (Variant.StartsWith(TEXT("level_instance."))) bCompatible = Object->IsA<ALevelInstance>();
		else if (Variant.StartsWith(TEXT("environment.")))
		{
			const FName Name = Object->GetClass()->GetFName();
			bCompatible = Name == TEXT("SkyAtmosphere") || Name == TEXT("SkyLight")
				|| Name == TEXT("ExponentialHeightFog") || Name == TEXT("PostProcessVolume");
		}
		else if (Variant.StartsWith(TEXT("navmesh."))) bCompatible = Object->IsA<ANavigationData>();
		else if (Variant.StartsWith(TEXT("nav_link.")))
			bCompatible = Object->GetClass()->GetFName() == TEXT("NavLinkProxy");
		else if (Variant.StartsWith(TEXT("nav_area.")))
			bCompatible = Object->GetClass()->GetName().Contains(TEXT("NavArea"), ESearchCase::CaseSensitive);
		if (!bCompatible)
		{
			OutError = TEXT("Loaded target class does not match the closed operation variant.");
			return false;
		}
		return true;
	}

	bool AddWorldRecord(FHyperAIWorldValueSnapshot& Snapshot, FHyperAIWorldRecord&& Record)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioWorldbuildingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return false;
		}
		Record.StableId = Record.Variant + TEXT(":") + Record.ObjectPath;
		Record.Details.Sort();
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.world-record.v1|") + WorldRecordCanonical(Record));
		if (!IsSha256(Record.Revision)) Snapshot.bComplete = false;
		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	bool AddNavigationRecord(FHyperAINavigationValueSnapshot& Snapshot, FHyperAINavigationRecord&& Record)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioNavigationContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return false;
		}
		Record.StableId = Record.Variant + TEXT(":") + Record.ObjectPath;
		Record.Details.Sort();
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.navigation-record.v1|") + NavigationRecordCanonical(Record));
		if (!IsSha256(Record.Revision)) Snapshot.bComplete = false;
		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	bool CaptureOnDiskWorld(
		const FHyperAIWorldInspectRequest& Request,
		FHyperAIWorldValueSnapshot& Snapshot,
		FString& OutError)
	{
		(void)Request;
		(void)OutError;
		// UE 5.8's synchronous Asset Registry enumeration materializes the complete match set
		// before a caller callback can enforce its count/deadline. Object-level exact lookup also
		// takes a blocking registry read lock. Neither can honestly satisfy this tool's hard bounds.
		Snapshot.bComplete = false;
		AddCaptureIssue(Snapshot.Issues, TEXT("async_on_disk_index_required"), TEXT("warning"),
			TEXT("on_disk_index"),
			TEXT("On-disk world class inventory requires a generation-cached asynchronous typed index; no synchronous Asset Registry scan or object lookup was attempted."));
		return true;
	}

	bool CaptureLoadedWorld(
		const FHyperAIWorldInspectRequest& Request,
		FHyperAIWorldValueSnapshot& Snapshot,
		FString& OutError)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("editor_world_unavailable"), TEXT("error"),
				TEXT("loaded_only"), TEXT("No current editor world is available; no world was opened or loaded."));
			return true;
		}
		const FString WorldPath = World->GetPathName();
		TSet<FString> ExactPaths;
		for (const FString& Path : Request.ObjectPaths) ExactPaths.Add(Path);
		const bool bRequestsUnenumeratedVariant = Request.Variants.IsEmpty()
			|| Request.Variants.ContainsByPredicate([](const FString& Variant)
			{
				return !LoadedWorldVariantHasBoundedEnumerator(Variant);
			});
		if (bRequestsUnenumeratedVariant)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("loaded_variant_index_incomplete"), TEXT("warning"),
				TEXT("loaded_only"),
				TEXT("One or more requested world variants have no hard-bounded loaded-object enumerator; their absence is not inferred."));
		}
		auto Wants = [&](const FString& Variant, const FString& Path)
		{
			return MatchesFilter(Request.Variants, Variant)
				&& (ExactPaths.IsEmpty() || ExactPaths.Contains(Path));
		};
		if (UWorldPartition* Partition = World->GetWorldPartition())
		{
			++Snapshot.Scanned;
			const FString Path = Partition->GetPathName();
			if (Wants(TEXT("world_partition"), Path))
			{
				FHyperAIWorldRecord Record;
				Record.Variant = TEXT("world_partition");
				Record.ObjectPath = Path;
				Record.PackageName = Partition->GetPackage()->GetName();
				Record.ClassPath = Partition->GetClass()->GetPathName();
				Record.WorldPath = WorldPath;
				Record.DisplayName = Partition->GetName();
				Record.bLoaded = true;
				Record.bDirty = Partition->GetPackage()->IsDirty();
				Record.bEnabled = true;
				Record.Details = {TEXT("subsystem=WorldPartition")};
				AddWorldRecord(Snapshot, MoveTemp(Record));
			}
		}
		const double LoadedScanStarted = FPlatformTime::Seconds();
		bool bObjectBoundReached = false;
		for (ULevel* Level : World->GetLevels())
		{
			if (!Level) continue;
			if (++Snapshot.Scanned > FHyperAIStudioWorldbuildingContracts::MaxScannedObjects
				|| FPlatformTime::Seconds() - LoadedScanStarted > MaxCaptureSeconds)
			{
				Snapshot.bComplete = false;
				bObjectBoundReached = true;
				break;
			}
			const FString Path = Level->GetPathName();
			if (Wants(TEXT("level"), Path))
			{
				FHyperAIWorldRecord Record;
				Record.Variant = TEXT("level");
				Record.ObjectPath = Path;
				Record.PackageName = Level->GetPackage()->GetName();
				Record.ClassPath = Level->GetClass()->GetPathName();
				Record.WorldPath = WorldPath;
				Record.DisplayName = Level->GetName();
				Record.PrimaryCount = Level->Actors.Num();
				Record.bLoaded = true;
				Record.bDirty = Level->GetPackage()->IsDirty();
				Record.bEnabled = Level->bIsVisible;
				AddWorldRecord(Snapshot, MoveTemp(Record));
			}
		}
		if (bObjectBoundReached)
		{
			AddCaptureIssue(Snapshot.Issues, TEXT("loaded_object_bound_reached"), TEXT("warning"),
				TEXT("loaded_only"), TEXT("Loaded capture stopped at a hard object bound."));
			return true;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor)) continue;
			if (++Snapshot.Scanned > FHyperAIStudioWorldbuildingContracts::MaxScannedObjects
				|| FPlatformTime::Seconds() - LoadedScanStarted > MaxCaptureSeconds)
			{
				Snapshot.bComplete = false;
				bObjectBoundReached = true;
				break;
			}
			FString Variant;
			int32 PrimaryCount = 0;
			int32 SecondaryCount = 0;
			TArray<FString> Details;
			if (const ALandscapeProxy* Landscape = Cast<ALandscapeProxy>(Actor))
			{
				Variant = TEXT("landscape");
				PrimaryCount = Landscape->LandscapeComponents.Num();
				SecondaryCount = Landscape->CollisionComponents.Num();
				Details.Add(FString::Printf(TEXT("landscape_guid=%s"),
					*Landscape->GetLandscapeGuid().ToString(EGuidFormats::DigitsWithHyphensLower)));
			}
			else if (const AInstancedFoliageActor* Foliage = Cast<AInstancedFoliageActor>(Actor))
			{
				Variant = TEXT("foliage");
				PrimaryCount = Foliage->GetFoliageInfos().Num();
			}
			else if (Actor->IsA<ARuntimeVirtualTextureVolume>())
			{
				Variant = TEXT("runtime_virtual_texture");
			}
			else if (Actor->IsA<ALevelInstance>())
			{
				Variant = TEXT("level_instance");
			}
			else
			{
				const FName ClassName = Actor->GetClass()->GetFName();
				if (ClassName == TEXT("WaterBodyRiver") || ClassName == TEXT("WaterBodyLake")
					|| ClassName == TEXT("WaterBodyOcean") || ClassName == TEXT("WaterBodyCustom"))
				{
					Variant = TEXT("water_body");
				}
				else if (ClassName == TEXT("WaterZone")) Variant = TEXT("water_zone");
				else if (ClassName == TEXT("SkyAtmosphere") || ClassName == TEXT("SkyLight")
					|| ClassName == TEXT("ExponentialHeightFog") || ClassName == TEXT("PostProcessVolume"))
				{
					Variant = TEXT("environment");
				}
			}
			if (!Variant.IsEmpty() && Wants(Variant, Actor->GetPathName()))
			{
				FHyperAIWorldRecord Record;
				Record.Variant = Variant;
				Record.ObjectPath = Actor->GetPathName();
				Record.PackageName = Actor->GetPackage()->GetName();
				Record.ClassPath = Actor->GetClass()->GetPathName();
				Record.WorldPath = WorldPath;
				Record.ParentPath = Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString();
				Record.DisplayName = Actor->GetActorLabel();
				Record.PrimaryCount = PrimaryCount;
				Record.SecondaryCount = SecondaryCount;
				Record.bLoaded = true;
				Record.bDirty = Actor->GetPackage()->IsDirty();
				Record.bEnabled = !Actor->IsHiddenEd();
				FVector Origin = FVector::ZeroVector;
				FVector Extent = FVector::ZeroVector;
				Actor->GetActorBounds(false, Origin, Extent);
				Record.bHasBounds = true;
				Record.BoundsMin = Origin - Extent;
				Record.BoundsMax = Origin + Extent;
				Record.Details = MoveTemp(Details);
				AddWorldRecord(Snapshot, MoveTemp(Record));
			}

			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (++Snapshot.Scanned > FHyperAIStudioWorldbuildingContracts::MaxScannedObjects
					|| FPlatformTime::Seconds() - LoadedScanStarted > MaxCaptureSeconds)
				{
					Snapshot.bComplete = false;
					bObjectBoundReached = true;
					break;
				}
				USplineComponent* Spline = Cast<USplineComponent>(Component);
				if (!IsValid(Spline) || !Wants(TEXT("spline"), Spline->GetPathName())) continue;
				FHyperAIWorldRecord Record;
				Record.Variant = TEXT("spline");
				Record.ObjectPath = Spline->GetPathName();
				Record.PackageName = Spline->GetPackage()->GetName();
				Record.ClassPath = Spline->GetClass()->GetPathName();
				Record.WorldPath = WorldPath;
				Record.ParentPath = Actor->GetPathName();
				Record.DisplayName = Spline->GetName();
				Record.PrimaryCount = Spline->GetNumberOfSplinePoints();
				Record.bLoaded = true;
				Record.bDirty = Spline->GetPackage()->IsDirty();
				Record.bEnabled = Spline->IsActive();
				Record.Details = {
					FString::Printf(TEXT("closed=%s"), Spline->IsClosedLoop() ? TEXT("true") : TEXT("false")),
					FString::Printf(TEXT("length=%.9g"), Spline->GetSplineLength())};
				AddWorldRecord(Snapshot, MoveTemp(Record));
			}
			if (bObjectBoundReached) break;
		}
		if (bObjectBoundReached)
		{
			AddCaptureIssue(Snapshot.Issues, TEXT("loaded_object_bound_reached"), TEXT("warning"),
				TEXT("loaded_only"), TEXT("Loaded capture stopped at a hard object or output bound."));
		}
		return true;
	}

	bool CaptureOnDiskNavigation(
		const FHyperAINavigationInspectRequest& Request,
		FHyperAINavigationValueSnapshot& Snapshot,
		FString& OutError)
	{
		(void)Request;
		(void)OutError;
		Snapshot.bComplete = false;
		AddCaptureIssue(Snapshot.Issues, TEXT("async_on_disk_index_required"), TEXT("warning"),
			TEXT("on_disk_index"),
			TEXT("On-disk navigation class inventory requires a generation-cached asynchronous typed index; no synchronous Asset Registry scan or object lookup was attempted."));
		return true;
	}

	bool CaptureLoadedNavigation(
		const FHyperAINavigationInspectRequest& Request,
		FHyperAINavigationValueSnapshot& Snapshot,
		FString& OutError)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("editor_world_unavailable"), TEXT("error"),
				TEXT("loaded_only"), TEXT("No current editor world is available."));
			return true;
		}
		TSet<FString> ExactPaths;
		for (const FString& Path : Request.ObjectPaths) ExactPaths.Add(Path);
		const bool bRequestsUnenumeratedVariant = Request.Variants.IsEmpty()
			|| Request.Variants.ContainsByPredicate([](const FString& Variant)
			{
				return !LoadedNavigationVariantHasBoundedEnumerator(Variant);
			});
		if (bRequestsUnenumeratedVariant)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("loaded_variant_index_incomplete"), TEXT("warning"),
				TEXT("loaded_only"),
				TEXT("Requested nav_area/nav_filter inventory has no hard-bounded loaded-object enumerator; absence is not inferred."));
		}
		auto Wants = [&](const FString& Variant, const FString& Path)
		{
			return MatchesFilter(Request.Variants, Variant)
				&& (ExactPaths.IsEmpty() || ExactPaths.Contains(Path));
		};
		UNavigationSystemV1* NavigationSystem = UNavigationSystemV1::GetCurrent(World);
		if (!NavigationSystem)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("navigation_system_unavailable"), TEXT("error"),
				TEXT("nav_system"), TEXT("The current editor world has no NavigationSystemV1; none was created."));
			return true;
		}
		++Snapshot.Scanned;
		if (Wants(TEXT("nav_system"), NavigationSystem->GetPathName()))
		{
			FHyperAINavigationRecord Record;
			Record.Variant = TEXT("nav_system");
			Record.ObjectPath = NavigationSystem->GetPathName();
			Record.ClassPath = NavigationSystem->GetClass()->GetPathName();
			Record.bLoaded = true;
			Record.bEnabled = true;
			AddNavigationRecord(Snapshot, MoveTemp(Record));
		}
		ANavigationData* MainData = NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
		if (!MainData)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("default_nav_data_unavailable"), TEXT("error"),
				TEXT("navmesh"), TEXT("No default navigation data is loaded; DontCreate prevented an implicit build."));
		}
		else
		{
			++Snapshot.Scanned;
			if (Wants(TEXT("navmesh"), MainData->GetPathName()))
			{
				FHyperAINavigationRecord Record;
				Record.Variant = TEXT("navmesh");
				Record.ObjectPath = MainData->GetPathName();
				Record.ClassPath = MainData->GetClass()->GetPathName();
				Record.bLoaded = true;
				Record.bEnabled = true;
				if (const ARecastNavMesh* Recast = Cast<ARecastNavMesh>(MainData))
				{
					Record.PrimaryCount = Recast->GetNumActiveTiles();
					Record.SecondaryCount = Recast->GetNavMeshTilesCount();
					const FBox Bounds = Recast->GetNavMeshBounds();
					Record.bHasBounds = Bounds.IsValid != 0;
					Record.BoundsMin = Bounds.Min;
					Record.BoundsMax = Bounds.Max;
					Record.Details.Add(TEXT("backend=recast"));
				}
				AddNavigationRecord(Snapshot, MoveTemp(Record));
			}
		}
		const double ActorScanStarted = FPlatformTime::Seconds();
		bool bNavObjectBoundReached = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->GetClass()->GetFName() != TEXT("NavLinkProxy")) continue;
			if (++Snapshot.Scanned > FHyperAIStudioNavigationContracts::MaxScannedObjects
				|| FPlatformTime::Seconds() - ActorScanStarted > MaxCaptureSeconds)
			{
				Snapshot.bComplete = false;
				bNavObjectBoundReached = true;
				break;
			}
			if (!Wants(TEXT("nav_link"), Actor->GetPathName())) continue;
			FHyperAINavigationRecord Record;
			Record.Variant = TEXT("nav_link");
			Record.ObjectPath = Actor->GetPathName();
			Record.ClassPath = Actor->GetClass()->GetPathName();
			Record.bLoaded = true;
			Record.bEnabled = !Actor->IsHiddenEd();
			FVector Origin = FVector::ZeroVector;
			FVector Extent = FVector::ZeroVector;
			Actor->GetActorBounds(false, Origin, Extent);
			Record.bHasBounds = true;
			Record.BoundsMin = Origin - Extent;
			Record.BoundsMax = Origin + Extent;
			AddNavigationRecord(Snapshot, MoveTemp(Record));
		}
		if (bNavObjectBoundReached)
		{
			AddCaptureIssue(Snapshot.Issues, TEXT("loaded_object_bound_reached"), TEXT("warning"),
				TEXT("loaded_only"), TEXT("Loaded navigation capture stopped at a hard object or time bound."));
		}
		if (Request.bQueryPath)
		{
			Snapshot.bPathAttempted = true;
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.Issues, TEXT("async_path_query_backend_required"), TEXT("warning"),
				TEXT("path_query"),
				TEXT("UE's synchronous path helper cannot be deadline-preempted; the query was not executed and requires a bounded asynchronous continuation backend."));
		}
		return true;
	}

	FString WorldOperationCanonical(const FHyperAIWorldBackendOperation& Operation)
	{
		FString Canonical;
		AppendToken(Canonical, Operation.Variant);
		AppendToken(Canonical, FHyperAIStudioWorldbuildingContracts::SafetyToString(Operation.Safety));
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.ReferencePath);
		AppendToken(Canonical, Operation.SourceAssetPath);
		AppendToken(Canonical, Operation.DestinationAssetPath);
		for (const FString& SourcePath : Operation.SourcePaths) AppendToken(Canonical, SourcePath);
		AppendToken(Canonical, Operation.Name);
		AppendToken(Canonical, Operation.LayerName);
		AppendToken(Canonical, Operation.State);
		AppendVector(Canonical, Operation.Location);
		AppendVector(Canonical, Operation.Extent);
		for (const FVector& Point : Operation.Points) AppendVector(Canonical, Point);
		AppendInt(Canonical, Operation.Index);
		AppendInt(Canonical, Operation.Count);
		AppendDouble(Canonical, Operation.Radius);
		AppendDouble(Canonical, Operation.Strength);
		AppendDouble(Canonical, Operation.Value);
		AppendBool(Canonical, Operation.bSetEnabled);
		AppendBool(Canonical, Operation.bEnabled);
		return Canonical;
	}

	FString NavigationOperationCanonical(const FHyperAINavigationBackendOperation& Operation)
	{
		FString Canonical;
		AppendToken(Canonical, Operation.Variant);
		AppendToken(Canonical, FHyperAIStudioWorldbuildingContracts::SafetyToString(Operation.Safety));
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.ReferencePath);
		AppendVector(Canonical, Operation.Location);
		AppendVector(Canonical, Operation.Extent);
		AppendVector(Canonical, Operation.LinkStart);
		AppendVector(Canonical, Operation.LinkEnd);
		AppendDouble(Canonical, Operation.AgentRadius);
		AppendDouble(Canonical, Operation.AgentHeight);
		AppendDouble(Canonical, Operation.Cost);
		AppendBool(Canonical, Operation.bSetEnabled);
		AppendBool(Canonical, Operation.bEnabled);
		return Canonical;
	}

	UE::AssetRegistry::EExists QueryTargetPackageExistenceNonBlocking(const FString& Path)
	{
		FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (!Module) return UE::AssetRegistry::EExists::Unknown;
		const FString PackageName = FSoftObjectPath(Path).GetLongPackageName();
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return UE::AssetRegistry::EExists::Unknown;
		}
		FAssetPackageData PackageData;
		return Module->Get().TryGetAssetPackageData(FName(*PackageName), PackageData, true);
	}

	bool ValidateTargetCas(
		const FString& Variant,
		const FString& TargetPath,
		const FString& ExpectedRevision,
		FString& OutCurrentRevision,
		FString& OutError)
	{
		OutCurrentRevision.Reset();
		if (!IsSafeExactObjectPath(TargetPath))
		{
			OutError = TEXT("TargetPath must be one exact bounded /Game object path.");
			return false;
		}
		UObject* Loaded = FindObject<UObject>(nullptr, *TargetPath);
		if (IsCreateVariant(Variant))
		{
			if (!ExpectedRevision.IsEmpty())
			{
				OutError = TEXT("Create variants prohibit ExpectedRevision.");
				return false;
			}
			if (!FHyperAIStudioWorldbuildingContracts::IsCanonicalProjectObjectPath(TargetPath))
			{
				OutError = TEXT("Actor/component creation requires an exact loaded-container revision; package absence only proves a canonical primary project asset absent.");
				return false;
			}
			const UE::AssetRegistry::EExists PackageState =
				QueryTargetPackageExistenceNonBlocking(TargetPath);
			const FString PackageName = FSoftObjectPath(TargetPath).GetLongPackageName();
			UPackage* LoadedContainerPackage = PackageName.IsEmpty()
				? nullptr : FindPackage(nullptr, *PackageName);
			if (Loaded || LoadedContainerPackage
				|| PackageState == UE::AssetRegistry::EExists::Exists)
			{
				OutError = TEXT("Create target or its containing package already exists; subobject creation requires a separately bound clean-container CAS.");
				return false;
			}
			if (PackageState != UE::AssetRegistry::EExists::DoesNotExist)
			{
				OutError = TEXT("Nonblocking package evidence could not prove the create target absent; existing-package subobject creation requires a separately bound container CAS.");
				return false;
			}
			return true;
		}
		if (!Loaded)
		{
			OutError = TEXT("Existing mutation targets must already be loaded for complete CAS; no implicit load was attempted.");
			return false;
		}
		if (!ValidateLoadedTargetType(Variant, Loaded, OutError)) return false;
		(void)ExpectedRevision;
		OutError = TEXT("Existing world/navigation mutation requires a bounded variant-specific persisted-state revision backend; inspection identity/transform evidence is not mutation CAS.");
		return false;
	}

	EHyperAIStudioDomainSafety ToDomainSafety(const EHyperAIWorldSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIWorldSafety::Destructive: return EHyperAIStudioDomainSafety::Destructive;
		case EHyperAIWorldSafety::ExternalEffect: return EHyperAIStudioDomainSafety::ExternalEffect;
		default: return EHyperAIStudioDomainSafety::Edit;
		}
	}

	bool BuildPreparedArtifact(
		const FString& PackId,
		const FString& ToolName,
		const FString& VariantId,
		const EHyperAIWorldSafety Safety,
		const FString& EffectTarget,
		const TSharedRef<const FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding.PackId = PackId;
		Contract.Binding.ToolName = ToolName;
		Contract.Binding.VariantId = VariantId;
		Contract.Binding.ExpectedSafety = ToDomainSafety(Safety);
		Contract.Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		Contract.Binding.ExpectedAdapterFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.worldbuilding.source-adapter.v1|") + PackId);
		Contract.Binding.ExpectedAdapterGeneration = 1;
		Contract.Binding.ExpectedRegistryEpoch = 1;
		Contract.Binding.Prerequisites.PackId = PackId;
		Contract.Binding.Prerequisites.bPackEnabled = true;
		Contract.Binding.Prerequisites.Revision = 1;
		if (ToolName.StartsWith(TEXT("hyper_navigation"), ESearchCase::CaseSensitive))
		{
			Contract.Binding.Prerequisites.Observations = {
				{TEXT("NavigationSystem"), EHyperAIStudioDomainPrerequisiteState::Available}};
		}
		else
		{
			Contract.Binding.Prerequisites.Observations = {
				{TEXT("Landscape"), EHyperAIStudioDomainPrerequisiteState::Available},
				{TEXT("Foliage"), EHyperAIStudioDomainPrerequisiteState::Available},
				{TEXT("WorldPartition"), EHyperAIStudioDomainPrerequisiteState::Available}};
		}
		Contract.Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Contract.Binding.Prerequisites);
		Contract.Binding.Admission.PackId = PackId;
		Contract.Binding.Admission.bPackAdmitted = false;
		Contract.Binding.Admission.Revision = 1;
		Contract.Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
				Contract.Binding.Admission);
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = EffectTarget;
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 128;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 128 * 1024;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = false;
		Contract.bSaveOnce = Safety != EHyperAIWorldSafety::ExternalEffect;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

int32 FHyperAIWorldTypedArtifactPayload::GetBoundedByteSize() const
{
	int64 Characters = TypeId.Len() + SchemaFingerprint.Len() + PackId.Len()
		+ SafetyClass.Len() + BaseRevision.Len() + 64;
	for (const FString& Operation : CanonicalOperations) Characters += Operation.Len() + 16;
	return Characters > MAX_int32 / static_cast<int32>(sizeof(TCHAR))
		? MAX_int32 : static_cast<int32>(Characters * sizeof(TCHAR));
}

FString FHyperAIWorldTypedArtifactPayload::GetSemanticFingerprint() const
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.world-typed-artifact.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, SafetyClass);
	AppendToken(Canonical, BaseRevision);
	for (const FString& Operation : CanonicalOperations) AppendToken(Canonical, Operation);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIWorldTypedArtifactPayload::CloneImmutable() const
{
	// TArray<FString> performs a detached value copy. The returned object shares no mutable array
	// storage with the caller-owned DTO and contains no UObject, token, delegate, or executable text.
	TSharedRef<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe>();
	Clone->TypeId = TypeId;
	Clone->SchemaFingerprint = SchemaFingerprint;
	Clone->PackId = PackId;
	Clone->SafetyClass = SafetyClass;
	Clone->BaseRevision = BaseRevision;
	Clone->CanonicalOperations = CanonicalOperations;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioWorldbuilding.HyperAIStudioWorldbuildingToolset");
}

const TArray<FHyperAIWorldManifestEntry>& FHyperAIStudioWorldbuildingContracts::GetManifest()
{
	static const TArray<FHyperAIWorldManifestEntry> Manifest = {
		{TEXT("hyper_worldbuilding_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_worldbuilding_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_worldbuilding_validate"), GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_inspect"), FHyperAIStudioNavigationContracts::GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_apply_plan"), FHyperAIStudioNavigationContracts::GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_validate"), FHyperAIStudioNavigationContracts::GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioWorldbuildingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIWorldManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 6) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIWorldManifestEntry& Entry : Manifest)
	{
		if (Entry.QualifiedToolset.IsEmpty() || Entry.Name.IsEmpty()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioWorldbuildingContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (!HyperAIStudio::Worldbuilding::Private::IsSafeExactObjectPath(Path)
		|| !FPackageName::IsValidObjectPath(Path)) return false;
	const FSoftObjectPath Reference(Path);
	const FString PackageName = Reference.GetLongPackageName();
	const FString AssetName = Reference.GetAssetName();
	return Reference.IsValid() && FPackageName::IsValidLongPackageName(PackageName)
		&& !AssetName.IsEmpty() && Path == PackageName + TEXT(".") + AssetName;
}

bool FHyperAIStudioWorldbuildingContracts::IsWorldVariant(const FString& Variant)
{
	static const TSet<FString> Variants = {
		TEXT("level"), TEXT("level_instance"), TEXT("world_partition"), TEXT("data_layer"),
		TEXT("hlod"), TEXT("landscape"), TEXT("landscape_layer"), TEXT("foliage"),
		TEXT("foliage_type"), TEXT("spline"), TEXT("water_body"), TEXT("water_zone"),
		TEXT("water_waves"), TEXT("runtime_virtual_texture"), TEXT("environment")};
	return Variants.Contains(Variant);
}

FString FHyperAIStudioWorldbuildingContracts::ComputeLoadedTargetRevision(
	const FString& ExactObjectPath)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	if (!IsSafeExactObjectPath(ExactObjectPath)) return FString();
	return ComputeLoadedObjectRevision(FindObject<UObject>(nullptr, *ExactObjectPath));
}

const TCHAR* FHyperAIStudioWorldbuildingContracts::SafetyToString(const EHyperAIWorldSafety Safety)
{
	switch (Safety)
	{
	case EHyperAIWorldSafety::Destructive: return TEXT("destructive");
	case EHyperAIWorldSafety::ExternalEffect: return TEXT("external_effect");
	default: return TEXT("edit");
	}
}

bool FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(
	const FHyperAIWorldPlanOperation& In,
	FHyperAIWorldBackendOperation& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	Out = {};
	OutError.Reset();
	static const TSet<FString> EditVariants = {
		TEXT("landscape.create"), TEXT("landscape.sculpt_delta"), TEXT("landscape.paint_layer"),
		TEXT("landscape.import_height_asset"), TEXT("landscape.export_height_asset"),
		TEXT("foliage.add_type"), TEXT("foliage.paint_instances"),
		TEXT("data_layer.set_state"), TEXT("rvt.bind_volume"),
		TEXT("water.create_body"), TEXT("water.set_zone"), TEXT("water.set_wave_profile"),
		TEXT("spline.create"), TEXT("spline.add_point"), TEXT("spline.set_point"),
		TEXT("spline.set_closed"), TEXT("level_instance.create"), TEXT("level_instance.update"),
		TEXT("environment.set_lighting"), TEXT("environment.set_fog"),
		TEXT("environment.set_sky"), TEXT("environment.set_post_process")};
	static const TSet<FString> DestructiveVariants = {
		TEXT("foliage.erase_instances"), TEXT("foliage.remove_type"),
		TEXT("spline.remove_point"), TEXT("optimize.merge"), TEXT("optimize.proxy"),
		TEXT("optimize.instance")};
	static const TSet<FString> ExternalVariants = {
		TEXT("world_partition.load_region"), TEXT("world_partition.unload_region"),
		TEXT("hlod.build")};
	if (!EditVariants.Contains(In.Variant) && !DestructiveVariants.Contains(In.Variant)
		&& !ExternalVariants.Contains(In.Variant))
	{
		OutError = TEXT("Operation is not one of the closed worldbuilding variants; actor/level CRUD delegates to Epic.");
		return false;
	}
	if (!IsSafeExactObjectPath(In.TargetPath) || In.SourcePaths.Num() > MaxPaths
		|| In.Points.Num() > MaxPointsPerOperation || !IsFiniteVector(In.Location)
		|| !IsFiniteVector(In.Extent) || !FMath::IsFinite(In.Radius)
		|| !FMath::IsFinite(In.Strength) || !FMath::IsFinite(In.Value)
		|| In.Radius < 0.0 || In.Radius > 10000000.0
		|| FMath::Abs(In.Strength) > 1000000.0 || FMath::Abs(In.Value) > 1000000000.0)
	{
		OutError = TEXT("Operation path, geometry, scalar, or collection bounds are invalid.");
		return false;
	}
	TSet<FString> UniqueSources;
	for (const FString& SourcePath : In.SourcePaths)
	{
		if (!IsSafeExactObjectPath(SourcePath) || UniqueSources.Contains(SourcePath))
		{
			OutError = TEXT("SourcePaths must contain unique exact /Game object paths.");
			return false;
		}
		UniqueSources.Add(SourcePath);
	}
	for (const FVector& Point : In.Points)
	{
		if (!IsFiniteVector(Point))
		{
			OutError = TEXT("Spline/placement points must be finite.");
			return false;
		}
	}
	if ((!In.SourceAssetPath.IsEmpty() && !IsCanonicalProjectObjectPath(In.SourceAssetPath))
		|| (!In.DestinationAssetPath.IsEmpty() && !IsCanonicalProjectObjectPath(In.DestinationAssetPath))
		|| (!In.ReferencePath.IsEmpty() && !IsSafeExactObjectPath(In.ReferencePath)))
	{
		OutError = TEXT("Asset/reference fields must be exact project-contained object paths; raw files are prohibited.");
		return false;
	}
	if ((!In.Name.IsEmpty() && !IsSafeName(In.Name))
		|| (!In.LayerName.IsEmpty() && !IsSafeName(In.LayerName)))
	{
		OutError = TEXT("Name and LayerName use a bounded identifier allowlist.");
		return false;
	}

	const bool bExtentPositive = In.Extent.X > 0.0 && In.Extent.Y > 0.0 && In.Extent.Z >= 0.0;
	if (In.Variant == TEXT("landscape.create")
		&& (!bExtentPositive || In.Count < 1 || In.Count > 8192))
	{
		OutError = TEXT("landscape.create requires positive extent and bounded component/resolution Count.");
		return false;
	}
	if (In.Variant == TEXT("landscape.sculpt_delta")
		&& (In.Radius <= 0.0 || In.Strength == 0.0))
	{
		OutError = TEXT("landscape.sculpt_delta requires nonzero bounded strength and positive radius.");
		return false;
	}
	if (In.Variant == TEXT("landscape.paint_layer")
		&& (!IsSafeName(In.LayerName) || In.Radius <= 0.0 || In.Strength < 0.0 || In.Strength > 1.0))
	{
		OutError = TEXT("landscape.paint_layer requires layer, positive radius, and strength [0,1].");
		return false;
	}
	if (In.Variant == TEXT("landscape.import_height_asset")
		&& !IsCanonicalProjectObjectPath(In.SourceAssetPath))
	{
		OutError = TEXT("Height import accepts only a canonical /Game source asset, never a file path.");
		return false;
	}
	if (In.Variant == TEXT("landscape.export_height_asset")
		&& !IsCanonicalProjectObjectPath(In.DestinationAssetPath))
	{
		OutError = TEXT("Height export accepts only a canonical /Game destination asset, never a file path.");
		return false;
	}
	if (In.Variant == TEXT("foliage.add_type")
		&& !IsCanonicalProjectObjectPath(In.ReferencePath))
	{
		OutError = TEXT("foliage.add_type requires one canonical FoliageType project asset.");
		return false;
	}
	if ((In.Variant == TEXT("foliage.paint_instances")
		|| In.Variant == TEXT("foliage.erase_instances"))
		&& (In.Radius <= 0.0 || In.Count < 1 || In.Count > 100000))
	{
		OutError = TEXT("Foliage paint/erase requires positive radius and a bounded instance Count.");
		return false;
	}
	if ((In.Variant == TEXT("world_partition.load_region")
		|| In.Variant == TEXT("world_partition.unload_region")) && !bExtentPositive)
	{
		OutError = TEXT("World Partition region operations require positive bounded extent.");
		return false;
	}
	if (In.Variant == TEXT("data_layer.set_state")
		&& In.State != TEXT("unloaded") && In.State != TEXT("loaded")
		&& In.State != TEXT("activated"))
	{
		OutError = TEXT("Data Layer state must be unloaded, loaded, or activated.");
		return false;
	}
	if (In.Variant == TEXT("rvt.bind_volume")
		&& !IsCanonicalProjectObjectPath(In.ReferencePath))
	{
		OutError = TEXT("RVT binding requires one canonical RuntimeVirtualTexture project asset.");
		return false;
	}
	if (IsWaterVariant(In.Variant) && !WaterPrerequisiteReady())
	{
		OutError = TEXT("Water variants require the enabled and already-loaded Water module; no module was loaded implicitly.");
		return false;
	}
	if (In.Variant == TEXT("water.create_body")
		&& In.Name != TEXT("river") && In.Name != TEXT("lake")
		&& In.Name != TEXT("ocean") && In.Name != TEXT("custom"))
	{
		OutError = TEXT("Water body kind must be river, lake, ocean, or custom.");
		return false;
	}
	if ((In.Variant == TEXT("spline.create") && In.Points.Num() < 2)
		|| ((In.Variant == TEXT("spline.add_point") || In.Variant == TEXT("spline.set_point")
			|| In.Variant == TEXT("spline.remove_point")) && In.Index < 0))
	{
		OutError = TEXT("Spline creation requires at least two points; point operations require a nonnegative index.");
		return false;
	}
	if ((In.Variant == TEXT("level_instance.create") || In.Variant == TEXT("level_instance.update"))
		&& !IsCanonicalProjectObjectPath(In.ReferencePath))
	{
		OutError = TEXT("Level Instance variants require a canonical /Game World asset reference.");
		return false;
	}
	if (In.Variant.StartsWith(TEXT("optimize."), ESearchCase::CaseSensitive)
		&& In.SourcePaths.Num() < 2)
	{
		OutError = TEXT("Actor merge/proxy/instance optimization requires at least two exact loaded sources.");
		return false;
	}

	Out.Variant = In.Variant;
	Out.Safety = DestructiveVariants.Contains(In.Variant) ? EHyperAIWorldSafety::Destructive
		: ExternalVariants.Contains(In.Variant) ? EHyperAIWorldSafety::ExternalEffect
		: EHyperAIWorldSafety::Edit;
	Out.TargetPath = In.TargetPath;
	Out.ExpectedRevision = In.ExpectedRevision;
	Out.ReferencePath = In.ReferencePath;
	Out.SourceAssetPath = In.SourceAssetPath;
	Out.DestinationAssetPath = In.DestinationAssetPath;
	Out.SourcePaths = In.SourcePaths;
	Out.Name = In.Name;
	Out.LayerName = In.LayerName;
	Out.State = In.State;
	Out.Location = In.Location;
	Out.Extent = In.Extent;
	Out.Points = In.Points;
	Out.Index = In.Index;
	Out.Count = In.Count;
	Out.Radius = In.Radius;
	Out.Strength = In.Strength;
	Out.Value = In.Value;
	Out.bSetEnabled = In.bSetEnabled;
	Out.bEnabled = In.bEnabled;
	return true;
}

bool FHyperAIStudioWorldbuildingContracts::Capture(
	const FHyperAIWorldInspectRequest& Request,
	FHyperAIWorldValueSnapshot& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	Out = {};
	Out.Scope = Request.Scope;
	OutError.Reset();
	if (Request.Scope != TEXT("loaded_only") && Request.Scope != TEXT("on_disk_index")
		&& Request.Scope != TEXT("loaded_and_on_disk"))
	{
		OutError = TEXT("Scope must be loaded_only, on_disk_index, or loaded_and_on_disk.");
		return false;
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Cursor.Len() > 192)
	{
		OutError = TEXT("Paging, cursor, or output budget is outside hard bounds.");
		return false;
	}
	if (!ValidateStringSet(Request.Variants, MaxVariants,
		[](const FString& Value) { return IsWorldVariant(Value); }, OutError)
		|| !ValidateStringSet(Request.ObjectPaths, MaxPaths,
		[](const FString& Value) { return IsSafeExactObjectPath(Value); }, OutError)
		|| !ValidateProjection(Request.Projection, false, OutError))
	{
		return false;
	}
	if (!Request.Variants.IsEmpty()
		&& Request.Variants.ContainsByPredicate([](const FString& Variant) { return IsWaterVariant(Variant); })
		&& !WaterPrerequisiteReady())
	{
		Out.bComplete = false;
		AddCaptureIssue(Out.Issues, TEXT("water_prerequisite_unavailable"), TEXT("error"),
			TEXT("water"), TEXT("Water was explicitly requested but its plugin/module is not enabled and already loaded."));
	}
	if ((Request.Scope == TEXT("loaded_only") || Request.Scope == TEXT("loaded_and_on_disk"))
		&& !CaptureLoadedWorld(Request, Out, OutError)) return false;
	if ((Request.Scope == TEXT("on_disk_index") || Request.Scope == TEXT("loaded_and_on_disk"))
		&& !CaptureOnDiskWorld(Request, Out, OutError)) return false;

	Out.Records.Sort([](const FHyperAIWorldRecord& Left, const FHyperAIWorldRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		return Left.Revision < Right.Revision;
	});
	for (int32 Index = Out.Records.Num() - 1; Index > 0; --Index)
	{
		if (Out.Records[Index].StableId == Out.Records[Index - 1].StableId
			&& Out.Records[Index].Revision == Out.Records[Index - 1].Revision)
		{
			Out.Records.RemoveAt(Index);
		}
	}
	ComputeSnapshotRevision(Out);
	return IsSha256(Out.Revision);
}

FString FHyperAIStudioWorldbuildingContracts::ComputeSnapshotRevision(FHyperAIWorldValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	TArray<FHyperAIWorldRecord> Records = Snapshot.Records;
	Records.Sort([](const FHyperAIWorldRecord& Left, const FHyperAIWorldRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		return Left.Revision < Right.Revision;
	});
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.worldbuilding-snapshot.v1"));
	AppendToken(Canonical, Snapshot.Scope);
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.Scanned);
	for (const FHyperAIWorldRecord& Record : Records)
	{
		AppendToken(Canonical, Record.Revision);
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsSha256(Snapshot.Revision))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
	}
	return Snapshot.Revision;
}

TArray<FHyperAIWorldIssue> FHyperAIStudioWorldbuildingContracts::ValidateSnapshot(
	const FHyperAIWorldValueSnapshot& Snapshot,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	TArray<FHyperAIWorldIssue> Issues;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	for (const FHyperAIWorldIssue& Existing : Snapshot.Issues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *Existing.Code, *Existing.Severity,
			Existing.Variant, Existing.ObjectPath, Existing.StableId, Existing.OperationIndex,
			Existing.Message);
	}
	if (!Snapshot.bComplete || !IsSha256(Snapshot.Revision))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("revision_incomplete"), TEXT("error"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("Independent validation requires a complete deterministic source revision."));
	}
	if (Snapshot.Records.IsEmpty())
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("no_matching_world_records"), TEXT("warning"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("No loaded/on-disk worldbuilding records matched the bounded request."));
	}
	for (const FHyperAIWorldRecord& Record : Snapshot.Records)
	{
		if (!IsSha256(Record.Revision))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("record_revision_missing"), TEXT("error"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Record cannot participate in revision-CAS."));
		}
		if (Record.Variant == TEXT("landscape") && Record.bLoaded && Record.PrimaryCount == 0)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("landscape_has_no_components"), TEXT("warning"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Loaded Landscape proxy has no LandscapeComponents."));
		}
		if (Record.Variant == TEXT("spline") && Record.PrimaryCount < 2)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("spline_has_too_few_points"), TEXT("error"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("A usable spline requires at least two points."));
		}
		if (Record.bDirty)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("unsaved_loaded_state"), TEXT("info"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Record includes unsaved loaded state; on-disk metadata may differ."));
		}
	}
	if (Snapshot.Scope.Contains(TEXT("on_disk")))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("on_disk_index_deferred"), TEXT("warning"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("On-disk world validation is deferred until a generation-bound asynchronous typed index is available; no metadata inventory was sampled."));
	}
	return Issues;
}

FHyperAIWorldInspectReport UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_inspect(
	const FHyperAIWorldInspectRequest& Request)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	FHyperAIWorldInspectReport Report;
	Report.ObservationScope = Request.Scope;
	FHyperAIWorldValueSnapshot Snapshot;
	if (!FHyperAIStudioWorldbuildingContracts::Capture(Request, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.SourceRecordsScanned = Snapshot.Scanned;
	Report.TotalRecords = Snapshot.Records.Num();
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Snapshot.Revision, Offset) || Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("stale_or_invalid_cursor");
		Report.Diagnostic = TEXT("Cursor is malformed or bound to a different complete revision.");
		return Report;
	}
	int64 EstimatedBytes = 2048;
	for (const FHyperAIWorldIssue& Issue : Snapshot.Issues)
	{
		const int64 IssueBytes = EstimateBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			Report.Status = TEXT("output_budget_too_small");
			Report.Diagnostic = TEXT("Issue evidence cannot fit within MaxOutputBytes.");
			return Report;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	for (int32 Index = Offset; Index < End; ++Index)
	{
		FHyperAIWorldRecord Record = Snapshot.Records[Index];
		ProjectRecord(Record, Request.Projection);
		const int64 RecordBytes = EstimateBytes(Record);
		if (EstimatedBytes + RecordBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += RecordBytes;
		Report.Records.Add(MoveTemp(Record));
	}
	Report.ReturnedRecords = Report.Records.Num();
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	Report.bTruncated |= NextOffset < Snapshot.Records.Num();
	if (Report.bTruncated) Report.NextCursor = MakeCursor(Snapshot.Revision, NextOffset);
	if (Report.Records.IsEmpty() && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("The selected projection cannot fit one record in MaxOutputBytes.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = Snapshot.bComplete ? TEXT("ok") : TEXT("ok_incomplete_revision");
	Report.Diagnostic = TEXT("Bounded records were captured without synchronously loading any object or module.");
	return Report;
}

FHyperAIWorldValidateReport UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_validate(
	const FHyperAIWorldValidateRequest& Request)
{
	FHyperAIWorldValidateReport Report;
	Report.ObservationScope = Request.Scope;
	if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioWorldbuildingContracts::MaxIssues)
	{
		Report.Status = TEXT("invalid_issue_budget");
		Report.Diagnostic = TEXT("MaxIssues is outside hard bounds.");
		return Report;
	}
	FHyperAIWorldInspectRequest Inspect;
	Inspect.Scope = Request.Scope;
	Inspect.Variants = Request.Variants;
	Inspect.ObjectPaths = Request.ObjectPaths;
	Inspect.PageSize = FHyperAIStudioWorldbuildingContracts::MaxPageSize;
	Inspect.MaxOutputBytes = FHyperAIStudioWorldbuildingContracts::MaxOutputBytes;
	FHyperAIWorldValueSnapshot Snapshot;
	if (!FHyperAIStudioWorldbuildingContracts::Capture(Inspect, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.Issues = FHyperAIStudioWorldbuildingContracts::ValidateSnapshot(
		Snapshot, Request.MaxIssues, Report.bTruncated);
	for (const FHyperAIWorldIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	Report.bOk = true;
	Report.bValid = Report.ErrorCount == 0 && Snapshot.bComplete;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = TEXT("Independent loaded/on-disk worldbuilding invariants were evaluated.");
	return Report;
}

FHyperAIWorldApplyPlanReport FHyperAIStudioWorldbuildingContracts::BuildPlan(
	const FHyperAIWorldApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	FHyperAIWorldApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Clip(Request.OperationId, 128);
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations)
	{
		Report.Status = TEXT("invalid_operation_count");
		Report.Diagnostic = TEXT("A plan requires 1..128 closed operations.");
		return Report;
	}
	TArray<FHyperAIWorldBackendOperation> Operations;
	TArray<FString> BaseTokens;
	TSet<FString> Targets;
	EHyperAIWorldSafety Safety = EHyperAIWorldSafety::Edit;
	bool bSafetySet = false;
	int32 TotalPoints = 0;
	int32 TotalSourcePaths = 0;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIWorldBackendOperation Operation;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Operation, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("invalid_operation"), TEXT("error"),
				Request.Operations[Index].Variant, Request.Operations[Index].TargetPath, FString(), Index, Error);
			Report.Status = TEXT("invalid_operation");
			Report.Diagnostic = TEXT("One closed operation failed schema, safety, or prerequisite validation.");
			return Report;
		}
		if (bSafetySet && Operation.Safety != Safety)
		{
			Report.Status = TEXT("mixed_safety_cohort");
			Report.Diagnostic = TEXT("Edit, destructive, and external-effect operations require separate exact plans.");
			return Report;
		}
		Safety = Operation.Safety;
		bSafetySet = true;
		TotalPoints += Operation.Points.Num();
		TotalSourcePaths += Operation.SourcePaths.Num();
		if (TotalPoints > MaxTotalPoints || TotalSourcePaths > MaxTotalSourcePaths)
		{
			Report.Status = TEXT("aggregate_plan_bound_exceeded");
			Report.Diagnostic = TEXT("Aggregate spline/placement points or optimization sources exceed hard plan bounds.");
			return Report;
		}
		if (Targets.Contains(Operation.TargetPath))
		{
			Report.Status = TEXT("duplicate_target");
			Report.Diagnostic = TEXT("One plan cannot target the same exact object more than once.");
			return Report;
		}
		Targets.Add(Operation.TargetPath);
		Operations.Add(MoveTemp(Operation));
	}
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const FHyperAIWorldBackendOperation& Operation = Operations[Index];
		FString Error;
		FString CurrentRevision;
		if (!ValidateTargetCas(Operation.Variant, Operation.TargetPath,
			Operation.ExpectedRevision, CurrentRevision, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("cas_failed"), TEXT("error"),
				Operation.Variant, Operation.TargetPath, FString(), Index, Error);
			Report.Status = TEXT("cas_failed");
			Report.Diagnostic = TEXT("A target existence/revision precondition failed without loading it.");
			return Report;
		}
		BaseTokens.Add(Operation.TargetPath + TEXT("=")
			+ (CurrentRevision.IsEmpty() ? TEXT("absent") : CurrentRevision));
		for (const FString& SourcePath : Operation.SourcePaths)
		{
			(void)SourcePath;
			Report.Status = TEXT("source_revision_backend_required");
			Report.Diagnostic = TEXT("Optimization sources require a bounded type-specific persisted-state projection; path/class/dirty evidence is insufficient.");
			return Report;
		}
	}
	BaseTokens.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.worldbuilding-base.v1"));
	for (const FString& Token : BaseTokens) AppendToken(BaseCanonical, Token);
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	if (!IsSha256(Report.BaseRevision))
	{
		Report.Status = TEXT("base_revision_unavailable");
		Report.Diagnostic = TEXT("A deterministic base revision could not be produced.");
		return Report;
	}
	TSharedRef<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe>();
	Payload->TypeId = TEXT("hyperai.payload.worldbuilding.plan.v1");
	Payload->SchemaFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.schema.worldbuilding.plan.v1:closed-dto-no-files-no-script"));
	Payload->PackId = PackId;
	Payload->SafetyClass = SafetyToString(Safety);
	Payload->BaseRevision = Report.BaseRevision;
	for (const FHyperAIWorldBackendOperation& Operation : Operations)
	{
		Payload->CanonicalOperations.Add(WorldOperationCanonical(Operation));
		++Report.Effects.OperationCount;
		if (Operation.Safety == EHyperAIWorldSafety::Destructive) ++Report.Effects.DestructiveCount;
		else if (Operation.Safety == EHyperAIWorldSafety::ExternalEffect) ++Report.Effects.ExternalEffectCount;
		else ++Report.Effects.EditCount;
		if (IsCreateVariant(Operation.Variant)) ++Report.Effects.CreateCount;
		if (IsRemoveVariant(Operation.Variant)) ++Report.Effects.RemoveCount;
	}
	Report.Effects.bTransactionOnce = Safety != EHyperAIWorldSafety::ExternalEffect;
	Report.Effects.bSaveOnce = Safety != EHyperAIWorldSafety::ExternalEffect;
	Report.SafetyClass = SafetyToString(Safety);
	Report.bRequiresTrustedAuthorization = Safety != EHyperAIWorldSafety::Edit;
	const FString BeforeClone = Payload->GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (!IsSha256(BeforeClone) || &Detached.Get() == &Payload.Get()
		|| Detached->GetSemanticFingerprint() != BeforeClone)
	{
		Report.Status = TEXT("immutable_payload_failed");
		Report.Diagnostic = TEXT("Typed plan could not produce an exact detached immutable snapshot.");
		return Report;
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	const FString EffectTarget = Operations.Num() == 1
		? Operations[0].TargetPath : TEXT("project-worldbuilding-cohort");
	if (!BuildPreparedArtifact(PackId, TEXT("hyper_worldbuilding_apply_plan"),
		FString(TEXT("apply_")) + SafetyToString(Safety), Safety, EffectTarget,
		Payload, Prepared, PrepareError))
	{
		Report.Status = TEXT("shared_prepare_rejected");
		Report.Diagnostic = Clip(PrepareError);
		return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.PreparedContractFingerprint = Prepared.ContractFingerprint;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = Report.bRequiresTrustedAuthorization
			? TEXT("valid_requires_trusted_authorization") : TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("Closed operations, loaded CAS, prerequisites, effects, and shared typed-artifact hashes validated without mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Non-dry-run submission requires a valid shared-journal operation_id.");
		return Report;
	}
	if (!IsSha256(Request.ExpectedPlanHash) || Request.ExpectedPlanHash != Report.PlanHash)
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Non-dry-run submission must echo the exact shared dry-run plan hash.");
		return Report;
	}
	Report.bOk = false;
	Report.bExecutionSubmitted = false;
	Report.Status = TEXT("staged_backend_required");
	Report.Diagnostic = TEXT("No artifact or mutation was submitted: this source candidate awaits an admitted typed mutator, "
		"independent validator, durable journal, and trusted authorization route.");
	return Report;
}

FHyperAIWorldApplyPlanReport UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(
	const FHyperAIWorldApplyPlanRequest& Request)
{
	return FHyperAIStudioWorldbuildingContracts::BuildPlan(Request);
}

FString FHyperAIStudioNavigationContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioWorldbuilding.HyperAIStudioNavigationToolset");
}

const TArray<FHyperAIWorldManifestEntry>& FHyperAIStudioNavigationContracts::GetManifest()
{
	// Both classes are one generated six-tool atomic cohort. Each contract resolver therefore
	// carries the same exact joint manifest instead of independently admitting three names.
	static const TArray<FHyperAIWorldManifestEntry> Manifest = {
		{TEXT("hyper_worldbuilding_inspect"), FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName()},
		{TEXT("hyper_worldbuilding_apply_plan"), FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName()},
		{TEXT("hyper_worldbuilding_validate"), FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_navigation_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioNavigationContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIWorldManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 6) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIWorldManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset.IsEmpty() || Unique.Contains(Entry.Name))
			return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioNavigationContracts::IsNavigationVariant(const FString& Variant)
{
	static const TSet<FString> Variants = {
		TEXT("nav_system"), TEXT("navmesh"), TEXT("nav_link"), TEXT("nav_area"),
		TEXT("nav_filter")};
	return Variants.Contains(Variant);
}

bool FHyperAIStudioNavigationContracts::ValidateOperationShape(
	const FHyperAINavigationPlanOperation& In,
	FHyperAINavigationBackendOperation& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	Out = {};
	OutError.Reset();
	static const TSet<FString> EditVariants = {
		TEXT("navmesh.configure"), TEXT("nav_link.create"), TEXT("nav_link.update"),
		TEXT("nav_area.create"), TEXT("nav_area.update")};
	static const TSet<FString> DestructiveVariants = {
		TEXT("nav_link.remove"), TEXT("nav_area.remove")};
	static const TSet<FString> ExternalVariants = {TEXT("navmesh.build")};
	if (!EditVariants.Contains(In.Variant) && !DestructiveVariants.Contains(In.Variant)
		&& !ExternalVariants.Contains(In.Variant))
	{
		OutError = TEXT("Operation is not a closed navigation authoring variant.");
		return false;
	}
	if (!IsSafeExactObjectPath(In.TargetPath)
		|| (!In.ReferencePath.IsEmpty() && !IsSafeExactObjectPath(In.ReferencePath))
		|| !IsFiniteVector(In.Location) || !IsFiniteVector(In.Extent)
		|| !IsFiniteVector(In.LinkStart) || !IsFiniteVector(In.LinkEnd)
		|| !FMath::IsFinite(In.AgentRadius) || !FMath::IsFinite(In.AgentHeight)
		|| !FMath::IsFinite(In.Cost) || In.AgentRadius < 0.0 || In.AgentRadius > 100000.0
		|| In.AgentHeight < 0.0 || In.AgentHeight > 100000.0
		|| In.Cost < 0.0 || In.Cost > 1000000000.0)
	{
		OutError = TEXT("Navigation path, vector, or scalar fields exceed closed bounds.");
		return false;
	}
	if (In.Variant == TEXT("navmesh.configure")
		&& (In.AgentRadius <= 0.0 || In.AgentHeight <= 0.0))
	{
		OutError = TEXT("navmesh.configure requires positive bounded agent radius and height.");
		return false;
	}
	if ((In.Variant == TEXT("nav_link.create") || In.Variant == TEXT("nav_link.update"))
		&& In.LinkStart.Equals(In.LinkEnd, UE_SMALL_NUMBER))
	{
		OutError = TEXT("A navigation link requires distinct finite endpoints.");
		return false;
	}
	if ((In.Variant == TEXT("nav_area.create") || In.Variant == TEXT("nav_area.update"))
		&& In.Cost <= 0.0)
	{
		OutError = TEXT("Navigation area create/update requires positive traversal cost.");
		return false;
	}
	Out.Variant = In.Variant;
	Out.Safety = DestructiveVariants.Contains(In.Variant) ? EHyperAIWorldSafety::Destructive
		: ExternalVariants.Contains(In.Variant) ? EHyperAIWorldSafety::ExternalEffect
		: EHyperAIWorldSafety::Edit;
	Out.TargetPath = In.TargetPath;
	Out.ExpectedRevision = In.ExpectedRevision;
	Out.ReferencePath = In.ReferencePath;
	Out.Location = In.Location;
	Out.Extent = In.Extent;
	Out.LinkStart = In.LinkStart;
	Out.LinkEnd = In.LinkEnd;
	Out.AgentRadius = In.AgentRadius;
	Out.AgentHeight = In.AgentHeight;
	Out.Cost = In.Cost;
	Out.bSetEnabled = In.bSetEnabled;
	Out.bEnabled = In.bEnabled;
	return true;
}

bool FHyperAIStudioNavigationContracts::Capture(
	const FHyperAINavigationInspectRequest& Request,
	FHyperAINavigationValueSnapshot& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	Out = {};
	Out.Scope = Request.Scope;
	OutError.Reset();
	if (Request.Scope != TEXT("loaded_only") && Request.Scope != TEXT("on_disk_index"))
	{
		OutError = TEXT("Navigation Scope must be loaded_only or on_disk_index.");
		return false;
	}
	if (Request.bQueryPath && Request.Scope != TEXT("loaded_only"))
	{
		OutError = TEXT("Path queries require the loaded editor world and are prohibited for on_disk_index.");
		return false;
	}
	if (Request.bQueryPath && (!IsFiniteVector(Request.PathStart) || !IsFiniteVector(Request.PathEnd)
		|| Request.PathStart.Equals(Request.PathEnd, UE_SMALL_NUMBER)))
	{
		OutError = TEXT("Path query endpoints must be distinct finite vectors.");
		return false;
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Cursor.Len() > 192)
	{
		OutError = TEXT("Paging, cursor, or output budget is outside hard bounds.");
		return false;
	}
	if (!ValidateStringSet(Request.Variants, MaxVariants,
		[](const FString& Value) { return IsNavigationVariant(Value); }, OutError)
		|| !ValidateStringSet(Request.ObjectPaths, MaxPaths,
		[](const FString& Value) { return IsSafeExactObjectPath(Value); }, OutError)
		|| !ValidateProjection(Request.Projection, true, OutError))
	{
		return false;
	}
	if (Request.Scope == TEXT("loaded_only"))
	{
		if (!CaptureLoadedNavigation(Request, Out, OutError)) return false;
	}
	else if (!CaptureOnDiskNavigation(Request, Out, OutError)) return false;
	Out.Records.Sort([](const FHyperAINavigationRecord& Left, const FHyperAINavigationRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		return Left.Revision < Right.Revision;
	});
	ComputeSnapshotRevision(Out);
	return IsSha256(Out.Revision);
}

FString FHyperAIStudioNavigationContracts::ComputeSnapshotRevision(
	FHyperAINavigationValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	TArray<FHyperAINavigationRecord> Records = Snapshot.Records;
	Records.Sort([](const FHyperAINavigationRecord& Left, const FHyperAINavigationRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		return Left.Revision < Right.Revision;
	});
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.navigation-snapshot.v1"));
	AppendToken(Canonical, Snapshot.Scope);
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.Scanned);
	AppendBool(Canonical, Snapshot.bPathAttempted);
	AppendBool(Canonical, Snapshot.bPathValid);
	AppendDouble(Canonical, Snapshot.PathLength);
	for (const FVector& Point : Snapshot.PathPoints) AppendVector(Canonical, Point);
	for (const FHyperAINavigationRecord& Record : Records) AppendToken(Canonical, Record.Revision);
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsSha256(Snapshot.Revision))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
	}
	return Snapshot.Revision;
}

TArray<FHyperAIWorldIssue> FHyperAIStudioNavigationContracts::ValidateSnapshot(
	const FHyperAINavigationValueSnapshot& Snapshot,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	TArray<FHyperAIWorldIssue> Issues;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	for (const FHyperAIWorldIssue& Existing : Snapshot.Issues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *Existing.Code, *Existing.Severity,
			Existing.Variant, Existing.ObjectPath, Existing.StableId, Existing.OperationIndex,
			Existing.Message);
	}
	if (!Snapshot.bComplete || !IsSha256(Snapshot.Revision))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("revision_incomplete"), TEXT("error"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("Navigation validation requires a complete deterministic revision."));
	}
	bool bFoundSystem = false;
	bool bFoundNavMesh = false;
	for (const FHyperAINavigationRecord& Record : Snapshot.Records)
	{
		bFoundSystem |= Record.Variant == TEXT("nav_system");
		bFoundNavMesh |= Record.Variant == TEXT("navmesh");
		if (!IsSha256(Record.Revision))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("record_revision_missing"), TEXT("error"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Navigation record cannot participate in revision-CAS."));
		}
		if (Record.Variant == TEXT("navmesh") && Record.bLoaded && Record.PrimaryCount == 0)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("navmesh_has_no_active_tiles"), TEXT("warning"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Loaded Recast navigation mesh has zero active tiles."));
		}
	}
	if (Snapshot.Scope == TEXT("loaded_only") && !bFoundSystem)
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("navigation_system_not_observed"), TEXT("error"),
			TEXT("nav_system"), FString(), FString(), -1,
			TEXT("No loaded NavigationSystemV1 record was observed."));
	}
	if (Snapshot.Scope == TEXT("loaded_only") && !bFoundNavMesh)
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("navmesh_not_observed"), TEXT("error"),
			TEXT("navmesh"), FString(), FString(), -1,
			TEXT("No loaded default navigation data record was observed."));
	}
	if (Snapshot.bPathAttempted && !Snapshot.bPathValid)
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("navigation_path_unverified"), TEXT("error"),
			TEXT("path_query"), FString(), FString(), -1,
			TEXT("No valid bounded path evidence exists; the synchronous UE helper was not called and a continuation backend is required."));
	}
	if (Snapshot.Scope == TEXT("on_disk_index"))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("on_disk_index_deferred"), TEXT("warning"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("On-disk navigation validation is deferred until a generation-bound asynchronous typed index is available; no metadata inventory was sampled."));
	}
	return Issues;
}

FHyperAINavigationInspectReport UHyperAIStudioNavigationToolset::hyper_navigation_inspect(
	const FHyperAINavigationInspectRequest& Request)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	FHyperAINavigationInspectReport Report;
	Report.ObservationScope = Request.Scope;
	FHyperAINavigationValueSnapshot Snapshot;
	if (!FHyperAIStudioNavigationContracts::Capture(Request, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.SourceRecordsScanned = Snapshot.Scanned;
	Report.TotalRecords = Snapshot.Records.Num();
	Report.bPathQueryAttempted = Snapshot.bPathAttempted;
	Report.bPathValid = Snapshot.bPathValid;
	Report.PathLength = Snapshot.PathLength;
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Snapshot.Revision, Offset) || Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("stale_or_invalid_cursor");
		Report.Diagnostic = TEXT("Cursor is malformed or bound to a different navigation revision.");
		return Report;
	}
	int64 EstimatedBytes = 2048;
	for (const FHyperAIWorldIssue& Issue : Snapshot.Issues)
	{
		const int64 IssueBytes = EstimateBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			Report.Status = TEXT("output_budget_too_small");
			Report.Diagnostic = TEXT("Issue evidence cannot fit within MaxOutputBytes.");
			return Report;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	constexpr int64 JsonBytesPerPathPoint = 128;
	for (const FVector& Point : Snapshot.PathPoints)
	{
		if (EstimatedBytes + JsonBytesPerPathPoint > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += JsonBytesPerPathPoint;
		Report.PathPoints.Add(Point);
	}
	Report.bTruncated |= Report.PathPoints.Num() < Snapshot.PathPoints.Num();
	bool bRecordsTruncated = false;
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	for (int32 Index = Offset; Index < End; ++Index)
	{
		FHyperAINavigationRecord Record = Snapshot.Records[Index];
		ProjectRecord(Record, Request.Projection);
		const int64 RecordBytes = EstimateBytes(Record);
		if (EstimatedBytes + RecordBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			bRecordsTruncated = true;
			break;
		}
		EstimatedBytes += RecordBytes;
		Report.Records.Add(MoveTemp(Record));
	}
	Report.ReturnedRecords = Report.Records.Num();
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	bRecordsTruncated |= NextOffset < Snapshot.Records.Num();
	Report.bTruncated |= bRecordsTruncated;
	if (bRecordsTruncated) Report.NextCursor = MakeCursor(Snapshot.Revision, NextOffset);
	if (Report.Records.IsEmpty() && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("The selected projection/path cannot fit one record in MaxOutputBytes.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = Snapshot.bComplete ? TEXT("ok") : TEXT("ok_incomplete_revision");
	Report.Diagnostic = TEXT("Navigation state was observed without creating nav data or loading assets/modules.");
	return Report;
}

FHyperAINavigationValidateReport UHyperAIStudioNavigationToolset::hyper_navigation_validate(
	const FHyperAINavigationValidateRequest& Request)
{
	FHyperAINavigationValidateReport Report;
	if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioNavigationContracts::MaxIssues)
	{
		Report.Status = TEXT("invalid_issue_budget");
		Report.Diagnostic = TEXT("MaxIssues is outside hard bounds.");
		return Report;
	}
	FHyperAINavigationInspectRequest Inspect;
	Inspect.Scope = Request.Scope;
	Inspect.Variants = Request.Variants;
	Inspect.ObjectPaths = Request.ObjectPaths;
	Inspect.bQueryPath = Request.bQueryPath;
	Inspect.PathStart = Request.PathStart;
	Inspect.PathEnd = Request.PathEnd;
	Inspect.PageSize = FHyperAIStudioNavigationContracts::MaxPageSize;
	Inspect.MaxOutputBytes = FHyperAIStudioNavigationContracts::MaxOutputBytes;
	FHyperAINavigationValueSnapshot Snapshot;
	if (!FHyperAIStudioNavigationContracts::Capture(Inspect, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.Issues = FHyperAIStudioNavigationContracts::ValidateSnapshot(
		Snapshot, Request.MaxIssues, Report.bTruncated);
	for (const FHyperAIWorldIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	Report.bOk = true;
	Report.bValid = Report.ErrorCount == 0 && Snapshot.bComplete;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = TEXT("Independent nav-system, navmesh, link, area, and optional path invariants were evaluated.");
	return Report;
}

FHyperAINavigationApplyPlanReport FHyperAIStudioNavigationContracts::BuildPlan(
	const FHyperAINavigationApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Worldbuilding::Private;
	FHyperAINavigationApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Clip(Request.OperationId, 128);
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations)
	{
		Report.Status = TEXT("invalid_operation_count");
		Report.Diagnostic = TEXT("A navigation plan requires 1..64 closed operations.");
		return Report;
	}
	UWorld* World = GetEditorWorld();
	UNavigationSystemV1* NavSystem = World ? UNavigationSystemV1::GetCurrent(World) : nullptr;
	if (!NavSystem)
	{
		Report.Status = TEXT("navigation_prerequisite_unavailable");
		Report.Diagnostic = TEXT("A loaded editor NavigationSystemV1 is required; none was created implicitly.");
		return Report;
	}
	TArray<FHyperAINavigationBackendOperation> Operations;
	TArray<FString> BaseTokens;
	TSet<FString> Targets;
	EHyperAIWorldSafety Safety = EHyperAIWorldSafety::Edit;
	bool bSafetySet = false;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAINavigationBackendOperation Operation;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Operation, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("invalid_operation"), TEXT("error"),
				Request.Operations[Index].Variant, Request.Operations[Index].TargetPath,
				FString(), Index, Error);
			Report.Status = TEXT("invalid_operation");
			Report.Diagnostic = TEXT("One navigation operation failed its closed schema.");
			return Report;
		}
		if (bSafetySet && Operation.Safety != Safety)
		{
			Report.Status = TEXT("mixed_safety_cohort");
			Report.Diagnostic = TEXT("Edit, destructive, and external navigation effects require separate plans.");
			return Report;
		}
		Safety = Operation.Safety;
		bSafetySet = true;
		if (Targets.Contains(Operation.TargetPath))
		{
			Report.Status = TEXT("duplicate_target");
			Report.Diagnostic = TEXT("One plan cannot target the same exact navigation object twice.");
			return Report;
		}
		Targets.Add(Operation.TargetPath);
		FString CurrentRevision;
		if (!ValidateTargetCas(Operation.Variant, Operation.TargetPath,
			Operation.ExpectedRevision, CurrentRevision, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("cas_failed"), TEXT("error"),
				Operation.Variant, Operation.TargetPath, FString(), Index, Error);
			Report.Status = TEXT("cas_failed");
			Report.Diagnostic = TEXT("A navigation target existence/revision precondition failed.");
			return Report;
		}
		BaseTokens.Add(Operation.TargetPath + TEXT("=")
			+ (CurrentRevision.IsEmpty() ? TEXT("absent") : CurrentRevision));
		Operations.Add(MoveTemp(Operation));
	}
	BaseTokens.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.navigation-base.v1"));
	for (const FString& Token : BaseTokens) AppendToken(BaseCanonical, Token);
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	if (!IsSha256(Report.BaseRevision))
	{
		Report.Status = TEXT("base_revision_unavailable");
		Report.Diagnostic = TEXT("A deterministic navigation base revision could not be produced.");
		return Report;
	}
	TSharedRef<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIWorldTypedArtifactPayload, ESPMode::ThreadSafe>();
	Payload->TypeId = TEXT("hyperai.payload.navigation.plan.v1");
	Payload->SchemaFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.schema.navigation.plan.v1:closed-dto-no-console-no-script"));
	Payload->PackId = PackId;
	Payload->SafetyClass = FHyperAIStudioWorldbuildingContracts::SafetyToString(Safety);
	Payload->BaseRevision = Report.BaseRevision;
	for (const FHyperAINavigationBackendOperation& Operation : Operations)
	{
		Payload->CanonicalOperations.Add(NavigationOperationCanonical(Operation));
		++Report.Effects.OperationCount;
		if (Operation.Safety == EHyperAIWorldSafety::Destructive) ++Report.Effects.DestructiveCount;
		else if (Operation.Safety == EHyperAIWorldSafety::ExternalEffect) ++Report.Effects.ExternalEffectCount;
		else ++Report.Effects.EditCount;
	}
	Report.Effects.bTransactionOnce = Safety != EHyperAIWorldSafety::ExternalEffect;
	Report.Effects.bSaveOnce = Safety != EHyperAIWorldSafety::ExternalEffect;
	Report.SafetyClass = FHyperAIStudioWorldbuildingContracts::SafetyToString(Safety);
	Report.bRequiresTrustedAuthorization = Safety != EHyperAIWorldSafety::Edit;
	const FString BeforeClone = Payload->GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (!IsSha256(BeforeClone) || &Detached.Get() == &Payload.Get()
		|| Detached->GetSemanticFingerprint() != BeforeClone)
	{
		Report.Status = TEXT("immutable_payload_failed");
		Report.Diagnostic = TEXT("Typed navigation plan could not produce an exact detached snapshot.");
		return Report;
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	const FString EffectTarget = Operations.Num() == 1
		? Operations[0].TargetPath : TEXT("project-navigation-cohort");
	if (!BuildPreparedArtifact(PackId, TEXT("hyper_navigation_apply_plan"),
		FString(TEXT("apply_")) + FHyperAIStudioWorldbuildingContracts::SafetyToString(Safety),
		Safety, EffectTarget, Payload, Prepared, PrepareError))
	{
		Report.Status = TEXT("shared_prepare_rejected");
		Report.Diagnostic = Clip(PrepareError);
		return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.PreparedContractFingerprint = Prepared.ContractFingerprint;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = Report.bRequiresTrustedAuthorization
			? TEXT("valid_requires_trusted_authorization") : TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("Closed navigation operations, loaded CAS, effects, and shared hashes validated without mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Non-dry-run submission requires a valid shared-journal operation_id.");
		return Report;
	}
	if (!IsSha256(Request.ExpectedPlanHash) || Request.ExpectedPlanHash != Report.PlanHash)
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Non-dry-run submission must echo the exact shared dry-run plan hash.");
		return Report;
	}
	Report.bOk = false;
	Report.bExecutionSubmitted = false;
	Report.Status = TEXT("staged_backend_required");
	Report.Diagnostic = TEXT("No artifact or navigation effect was submitted: an admitted typed mutator, independent "
		"validator, durable journal, and trusted authorization route are required.");
	return Report;
}

FHyperAINavigationApplyPlanReport UHyperAIStudioNavigationToolset::hyper_navigation_apply_plan(
	const FHyperAINavigationApplyPlanRequest& Request)
{
	return FHyperAIStudioNavigationContracts::BuildPlan(Request);
}

void FHyperAIStudioWorldbuildingRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioWorldbuildingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioWorldbuildingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (IsInGameThread() && UObjectInitialized())
	{
		FString Error;
		if (bOwnsNavigationRegistration
			&& !FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
				UHyperAIStudioNavigationToolset::StaticClass(),
				FHyperAIStudioNavigationContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioWorldbuilding, Error,
				TEXT("Navigation owned-registration shutdown failed closed: %s"), *Error);
		}
		else
		{
			bOwnsNavigationRegistration = false;
		}
		Error.Reset();
		if (bOwnsWorldbuildingRegistration
			&& !FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
				UHyperAIStudioWorldbuildingToolset::StaticClass(),
				FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioWorldbuilding, Error,
				TEXT("Worldbuilding owned-registration shutdown failed closed: %s"), *Error);
		}
		else
		{
			bOwnsWorldbuildingRegistration = false;
		}
	}
	bStarted = false;
}

bool FHyperAIStudioWorldbuildingRegistration::IsWorldbuildingRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioWorldbuildingContracts::IsRegistrationAllowed(bDev)
		&& FHyperAIStudioNavigationContracts::IsRegistrationAllowed(bDev)
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioWorldbuildingToolset::StaticClass(),
			FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioNavigationToolset::StaticClass(),
			FHyperAIStudioNavigationContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioWorldbuildingRegistration::IsNavigationRegistered() const
{
	return IsWorldbuildingRegistered();
}

void FHyperAIStudioWorldbuildingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()) return;
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioWorldbuildingContracts::IsRegistrationAllowed(bDev)
		|| !FHyperAIStudioNavigationContracts::IsRegistrationAllowed(bDev)) return;

	const bool bWorldAlreadyOwned = FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioWorldbuildingToolset::StaticClass(),
		FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName());
	const bool bNavigationAlreadyOwned = FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioNavigationToolset::StaticClass(),
		FHyperAIStudioNavigationContracts::GetQualifiedToolsetName());
	if (bWorldAlreadyOwned != bNavigationAlreadyOwned)
	{
		FString RollbackError;
		if (bNavigationAlreadyOwned)
		{
			FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
				UHyperAIStudioNavigationToolset::StaticClass(),
				FHyperAIStudioNavigationContracts::GetQualifiedToolsetName(), RollbackError);
		}
		else
		{
			FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
				UHyperAIStudioWorldbuildingToolset::StaticClass(),
				FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName(), RollbackError);
		}
		UE_LOG(LogHyperAIStudioWorldbuilding, Error,
			TEXT("Six-tool cohort found partial ownership and rolled back fail closed: %s"),
			*RollbackError);
		return;
	}
	if (bWorldAlreadyOwned && bNavigationAlreadyOwned)
	{
		bOwnsWorldbuildingRegistration = true;
		bOwnsNavigationRegistration = true;
		return;
	}

	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioWorldbuildingToolset::StaticClass(),
		FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioWorldbuilding, Error,
			TEXT("Worldbuilding half of the six-tool cohort failed closed: %s"), *Error);
		return;
	}
	bOwnsWorldbuildingRegistration = true;
	Error.Reset();
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioNavigationToolset::StaticClass(),
		FHyperAIStudioNavigationContracts::GetQualifiedToolsetName(), Error))
	{
		FString RollbackError;
		FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioWorldbuildingToolset::StaticClass(),
			FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName(), RollbackError);
		bOwnsWorldbuildingRegistration = false;
		UE_LOG(LogHyperAIStudioWorldbuilding, Error,
			TEXT("Navigation half failed; worldbuilding registration rolled back: %s | rollback=%s"),
			*Error, *RollbackError);
		return;
	}
	bOwnsNavigationRegistration = true;
}
