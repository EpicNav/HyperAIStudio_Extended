// Games by Hyper 2026.

#include "HyperAIStudioDiagnoseToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioDiagnoseToolset)

namespace HyperAIStudio::Diagnose::Private
{
	const TSet<FString>& AllowedChecks()
	{
		static const TSet<FString> Values = {
			TEXT("asset_context"),
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
		return Values;
	}

	const TArray<FString>& DefaultChecks()
	{
		static const TArray<FString> Values = {
			TEXT("asset_registry_readiness"),
			TEXT("current_map_state"),
			TEXT("dirty_loaded_packages"),
			TEXT("loaded_blueprint_health"),
			TEXT("plugin_readiness")
		};
		return Values;
	}

	const TArray<FString>& RequiredPlugins()
	{
		static const TArray<FString> Values = {
			TEXT("EditorToolset"),
			TEXT("MCPClientToolset"),
			TEXT("ModelContextProtocol"),
			TEXT("Terminal"),
			TEXT("ToolsetRegistry")
		};
		return Values;
	}

	FString BlueprintStatusString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown: return TEXT("unknown");
		case BS_Dirty: return TEXT("dirty");
		case BS_Error: return TEXT("error");
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_BeingCreated: return TEXT("being_created");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		default: return TEXT("invalid");
		}
	}

	bool IsCanonicalGameObjectPath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.Len() > FHyperAIStudioDiagnoseContracts::MaxPathCharacters
			|| FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(Path)
			|| !FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(Path)
			|| !Path.StartsWith(TEXT("/Game/")) || Path.Contains(TEXT(":")))
		{
			return false;
		}
		FText Reason;
		if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
		const FSoftObjectPath ObjectPath(Path);
		const FString PackageName = ObjectPath.GetLongPackageName();
		const FString AssetName = ObjectPath.GetAssetName();
		return ObjectPath.IsValid() && FPackageName::IsValidLongPackageName(PackageName)
			&& !AssetName.IsEmpty() && Path == PackageName + TEXT(".") + AssetName;
	}

	FHyperAIStudioDiagnosticsFinding MakeFinding(
		const FString& Kind,
		const FString& Severity,
		const FString& Code,
		const FString& Subject,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsFinding Finding;
		Finding.Kind = Kind;
		Finding.Severity = Severity;
		Finding.Code = Code;
		Finding.Subject = FHyperAIStudioDiagnosticsCommon::Clip(
			Subject, FHyperAIStudioDiagnosticsCommon::MaxSubjectCharacters);
		Finding.Message = FHyperAIStudioDiagnosticsCommon::Clip(
			Message, FHyperAIStudioDiagnosticsCommon::MaxFindingMessageCharacters);
		Finding.RecordId = FHyperAIStudioDiagnosticsCommon::MakeRecordId(
			Finding.Kind, Finding.Subject, Finding.Code);
		return Finding;
	}

	void SortAndDeduplicateFindings(TArray<FHyperAIStudioDiagnosticsFinding>& Findings)
	{
		for (FHyperAIStudioDiagnosticsFinding& Finding : Findings)
		{
			Finding.Fields.Sort([](
				const FHyperAIStudioDiagnosticsField& Left,
				const FHyperAIStudioDiagnosticsField& Right)
			{
				if (Left.Name != Right.Name)
				{
					return Left.Name < Right.Name;
				}
				return Left.Value < Right.Value;
			});
		}
		Findings.Sort([](
			const FHyperAIStudioDiagnosticsFinding& Left,
			const FHyperAIStudioDiagnosticsFinding& Right)
		{
			return Left.RecordId < Right.RecordId;
		});
		for (int32 Index = Findings.Num() - 1; Index > 0; --Index)
		{
			if (Findings[Index].RecordId == Findings[Index - 1].RecordId)
			{
				Findings.RemoveAt(Index, 1, EAllowShrinking::No);
			}
		}
	}

	FHyperAIStudioDiagnosticsDiagnoseResult InvalidResult(
		const FString& Code,
		const FString& Field,
		const FString& Message,
		const FString& Cursor)
	{
		FHyperAIStudioDiagnosticsDiagnoseResult Result;
		Result.Status = TEXT("invalid_request");
		Result.bIncomplete = false;
		Result.CursorStatus = Cursor.IsEmpty() ? TEXT("none") : TEXT("rejected");
		Result.CursorDiagnosticCode = Cursor.IsEmpty() ? FString() : Code;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, Code, TEXT("error"), Field, Message);
		return Result;
	}
}

bool FHyperAIStudioDiagnoseContracts::NormalizeRequest(
	const FHyperAIStudioDiagnosticsDiagnoseRequest& Request,
	FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Diagnose::Private;
	OutRequest = FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest();
	if (!FHyperAIStudioDiagnosticsCommon::NormalizeAllowlist(
		Request.Checks,
		AllowedChecks(),
		DefaultChecks(),
		MaxChecks,
		OutRequest.Checks,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	if (Request.MaxObjectsScanned < MinObjectsScanned || Request.MaxObjectsScanned > MaxObjectsScanned)
	{
		OutErrorCode = TEXT("invalid_object_scan_bound");
		OutError = TEXT("MaxObjectsScanned is outside the fixed supported range.");
		return false;
	}
	if (!FHyperAIStudioDiagnosticsCommon::ValidatePageBounds(
		Request.PageSize, Request.Cursor, Request.MaxOutputBytes, OutErrorCode, OutError))
	{
		return false;
	}
	if (Request.AssetPaths.Num() > MaxAssetPaths)
	{
		OutErrorCode = TEXT("too_many_asset_paths");
		OutError = TEXT("AssetPaths exceeds the hard input bound.");
		return false;
	}
	TSet<FString> UniquePaths;
	for (const FString& Path : Request.AssetPaths)
	{
		if (!IsCanonicalGameObjectPath(Path))
		{
			OutErrorCode = TEXT("invalid_asset_path");
			OutError = TEXT("Every asset path must be an exact canonical /Game object path without a subobject suffix.");
			return false;
		}
		UniquePaths.Add(Path);
	}
	OutRequest.AssetPaths = UniquePaths.Array();
	OutRequest.AssetPaths.Sort();
	OutRequest.MaxObjectsScanned = Request.MaxObjectsScanned;
	OutRequest.PageSize = Request.PageSize;
	OutRequest.Cursor = Request.Cursor;
	OutRequest.MaxOutputBytes = Request.MaxOutputBytes;
	TArray<FString> Tokens = {
		TEXT("hyperai.diagnose.request.v1"),
		FString::Join(OutRequest.Checks, TEXT(",")),
		FString::Join(OutRequest.AssetPaths, TEXT(",")),
		FString::FromInt(OutRequest.MaxObjectsScanned)
	};
	OutRequest.RequestFingerprint = FHyperAIStudioDiagnosticsCommon::HashTokens(Tokens);
	if (OutRequest.RequestFingerprint.IsEmpty())
	{
		OutErrorCode = TEXT("request_fingerprint_failed");
		OutError = TEXT("Normalized request could not be fingerprinted within the fixed hash budget.");
		return false;
	}
	return true;
}

FHyperAIStudioDiagnosticsValueSnapshot FHyperAIStudioDiagnoseContracts::Capture(
	const FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest& Request)
{
	using namespace HyperAIStudio::Diagnose::Private;
	FHyperAIStudioDiagnosticsValueSnapshot Snapshot;
	Snapshot.RequestFingerprint = Request.RequestFingerprint;
	Snapshot.CapturedUtc = FDateTime::UtcNow().ToIso8601();

	if (Request.Checks.Contains(TEXT("plugin_readiness")))
	{
		for (const FString& PluginName : RequiredPlugins())
		{
			FHyperAIStudioDiagnosticsPluginSnapshot& Plugin = Snapshot.Plugins.AddDefaulted_GetRef();
			Plugin.Name = PluginName;
			const TSharedPtr<IPlugin> Found = IPluginManager::Get().FindPlugin(PluginName);
			Plugin.bInstalled = Found.IsValid();
			Plugin.bEnabled = Found.IsValid() && Found->IsEnabled();
		}
	}

	const bool bNeedsRegistry = Request.Checks.Contains(TEXT("asset_registry_readiness"))
		|| Request.Checks.Contains(TEXT("asset_context"));
	if (bNeedsRegistry)
	{
		if (FAssetRegistryModule* AssetRegistryModule =
			FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
				IAssetRegistry& Registry = AssetRegistryModule->Get();
				Snapshot.bAssetRegistryAvailable = true;
				Snapshot.bAssetRegistryGathering = Registry.IsGathering();
				// UE 5.8's IsSearchAllAssets takes the blocking registry interface lock.
				// This bounded tool does not call it; a core-owned cached completion observer
				// is required before the exact SearchAllAssets claim can become true.
				Snapshot.bAssetRegistrySearchAllObserved = false;
				Snapshot.bIncomplete = true;
				FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
					Snapshot.Diagnostics,
					TEXT("cached_asset_registry_completion_probe_required"),
					TEXT("warning"),
					TEXT("asset_registry"),
					TEXT("Exact SearchAllAssets readiness requires a core-owned cached completion observer; the blocking readiness getter was intentionally not called."));
			if (Request.Checks.Contains(TEXT("asset_context")))
			{
				for (const FString& RequestedPath : Request.AssetPaths)
				{
					FHyperAIStudioDiagnosticsAssetSnapshot& Asset = Snapshot.Assets.AddDefaulted_GetRef();
					Asset.RequestedPath = RequestedPath;
					const FSoftObjectPath SoftPath(RequestedPath);
					const FString PackageName = SoftPath.GetLongPackageName();
					FAssetPackageData PackageData;
					const UE::AssetRegistry::EExists PackageState = PackageName.IsEmpty()
						? UE::AssetRegistry::EExists::Unknown
						: Registry.TryGetAssetPackageData(
							FName(*PackageName), PackageData, /*bFailIfLockHeld=*/true);
					Asset.bPackageStateKnown = PackageState != UE::AssetRegistry::EExists::Unknown;
					Asset.bPackageExists = PackageState == UE::AssetRegistry::EExists::Exists;
					Asset.bPackageAbsent = PackageState == UE::AssetRegistry::EExists::DoesNotExist;
					UObject* Loaded = FindObject<UObject>(nullptr, *RequestedPath);
					Asset.bFound = IsValid(Loaded) && Loaded->GetPathName() == RequestedPath;
					if (Asset.bFound)
					{
						Asset.ObjectPath = Loaded->GetPathName();
						Asset.PackageName = Loaded->GetOutermost()->GetName();
						Asset.AssetName = Loaded->GetName();
						Asset.ClassPath = Loaded->GetClass()->GetPathName();
					}
					if (PackageState == UE::AssetRegistry::EExists::Unknown)
					{
						Snapshot.bIncomplete = true;
						FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
							Snapshot.Diagnostics,
							TEXT("asset_package_state_unavailable"),
							TEXT("warning"),
							TEXT("asset_context"),
							TEXT("A nonblocking package query could not acquire the registry lock or the full search is incomplete; disk absence is not inferred."));
					}
					else if (!Asset.bFound && PackageState == UE::AssetRegistry::EExists::Exists)
					{
						Snapshot.bIncomplete = true;
						FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
							Snapshot.Diagnostics,
							TEXT("async_asset_identity_index_required"),
							TEXT("warning"),
							TEXT("asset_context"),
							TEXT("The package exists, but exact unloaded object/class identity requires a bounded asynchronous typed index; the blocking object lookup was not called."));
					}
				}
			}
		}
		else
		{
			Snapshot.bIncomplete = true;
			FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
				Snapshot.Diagnostics,
				TEXT("asset_registry_module_unavailable"),
				TEXT("error"),
				TEXT("asset_registry"),
				TEXT("The already-loaded Asset Registry module is unavailable; no synchronous module load was attempted."));
		}
	}

	if (Request.Checks.Contains(TEXT("current_map_state")))
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		Snapshot.bEditorWorldAvailable = World != nullptr;
		if (World && World->GetOutermost())
		{
			Snapshot.CurrentMapPackage = World->GetOutermost()->GetName();
			Snapshot.bCurrentMapDirty = World->GetOutermost()->IsDirty();
		}
		else
		{
			Snapshot.bIncomplete = true;
		}
	}

	if (Request.Checks.Contains(TEXT("loaded_blueprint_health")))
	{
		const double BlueprintCaptureStart = FPlatformTime::Seconds();
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (Snapshot.BlueprintsScanned >= Request.MaxObjectsScanned
				|| FPlatformTime::Seconds() - BlueprintCaptureStart > MaxCaptureSeconds)
			{
				Snapshot.bCaptureWorkBoundReached = true;
				Snapshot.bIncomplete = true;
				break;
			}
			UBlueprint* Blueprint = *It;
			if (!Blueprint || Blueprint->HasAnyFlags(RF_Transient))
			{
				continue;
			}
			const FString ObjectPath = Blueprint->GetPathName();
			if (!ObjectPath.StartsWith(TEXT("/Game/")))
			{
				continue;
			}
			++Snapshot.BlueprintsScanned;
			FHyperAIStudioDiagnosticsBlueprintSnapshot& Entry = Snapshot.Blueprints.AddDefaulted_GetRef();
			Entry.ObjectPath = FHyperAIStudioDiagnosticsCommon::Clip(ObjectPath, MaxPathCharacters);
			Entry.Status = BlueprintStatusString(Blueprint->Status);
			Entry.bPackageDirty = Blueprint->GetOutermost() && Blueprint->GetOutermost()->IsDirty();
		}
	}

	if (Request.Checks.Contains(TEXT("dirty_loaded_packages")))
	{
		const double PackageCaptureStart = FPlatformTime::Seconds();
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (Snapshot.PackagesScanned >= Request.MaxObjectsScanned
				|| FPlatformTime::Seconds() - PackageCaptureStart > MaxCaptureSeconds)
			{
				Snapshot.bCaptureWorkBoundReached = true;
				Snapshot.bIncomplete = true;
				break;
			}
			UPackage* Package = *It;
			if (!Package || Package->HasAnyPackageFlags(PKG_CompiledIn))
			{
				continue;
			}
			const FString PackageName = Package->GetName();
			if (!PackageName.StartsWith(TEXT("/Game/")))
			{
				continue;
			}
			++Snapshot.PackagesScanned;
			if (Package->IsDirty())
			{
				Snapshot.DirtyLoadedPackages.Add(
					FHyperAIStudioDiagnosticsCommon::Clip(PackageName, MaxPathCharacters));
			}
		}
	}

	Snapshot.Plugins.Sort([](const auto& Left, const auto& Right) { return Left.Name < Right.Name; });
	Snapshot.Assets.Sort([](const auto& Left, const auto& Right) { return Left.RequestedPath < Right.RequestedPath; });
	Snapshot.Blueprints.Sort([](const auto& Left, const auto& Right) { return Left.ObjectPath < Right.ObjectPath; });
	Snapshot.DirtyLoadedPackages.Sort();
	if (Snapshot.bCaptureWorkBoundReached)
	{
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Snapshot.Diagnostics,
			TEXT("capture_work_bound_reached"),
			TEXT("warning"),
			TEXT("max_objects_scanned"),
			TEXT("Loaded-object capture reached its fixed object or elapsed-time bound; omitted state is explicit."));
	}
	return Snapshot;
}

FHyperAIStudioDiagnosticsDiagnoseResult FHyperAIStudioDiagnoseContracts::Analyze(
	const FHyperAIStudioDiagnosticsValueSnapshot& Snapshot,
	const FHyperAIStudioDiagnosticsDiagnoseRequest& Request,
	const FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest* PreparedRequest)
{
	using namespace HyperAIStudio::Diagnose::Private;
	FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest NormalizedStorage;
	FString ErrorCode;
	FString Error;
	if (!PreparedRequest
		&& !NormalizeRequest(Request, NormalizedStorage, ErrorCode, Error))
	{
		return InvalidResult(ErrorCode, TEXT("request"), Error, Request.Cursor);
	}
	const FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest& Normalized =
		PreparedRequest ? *PreparedRequest : NormalizedStorage;
	if (Snapshot.RequestFingerprint != Normalized.RequestFingerprint)
	{
		return InvalidResult(
			TEXT("snapshot_request_mismatch"),
			TEXT("request"),
			TEXT("The immutable diagnostic snapshot belongs to a different normalized request."),
			Request.Cursor);
	}

	FHyperAIStudioDiagnosticsDiagnoseResult Result;
	Result.RequestFingerprint = Normalized.RequestFingerprint;
	Result.bIncomplete = Snapshot.bIncomplete;
	Result.bCaptureWorkBoundReached = Snapshot.bCaptureWorkBoundReached;
	Result.BlueprintsScanned = Snapshot.BlueprintsScanned;
	Result.PackagesScanned = Snapshot.PackagesScanned;
	Result.DirtyLoadedPackageCount = Snapshot.DirtyLoadedPackages.Num();
	Result.Diagnostics = Snapshot.Diagnostics;
	FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
		Result.Diagnostics,
		TEXT("no_delete_inference"),
		TEXT("info"),
		TEXT("deletion_assessment"),
		Result.SafetyNotice);

	TArray<FHyperAIStudioDiagnosticsFinding> Findings;
	Findings.Reserve(MaxFindings);
	bool bFindingBoundReached = false;
	const auto AddFindingBounded = [&Findings, &bFindingBoundReached](
		FHyperAIStudioDiagnosticsFinding&& Finding)
	{
		if (Findings.Num() >= FHyperAIStudioDiagnoseContracts::MaxFindings)
		{
			bFindingBoundReached = true;
			return;
		}
		Findings.Add(MoveTemp(Finding));
	};
	if (Normalized.Checks.Contains(TEXT("plugin_readiness")))
	{
		for (const FHyperAIStudioDiagnosticsPluginSnapshot& Plugin : Snapshot.Plugins)
		{
			const FString Code = !Plugin.bInstalled
				? TEXT("required_plugin_missing")
				: (!Plugin.bEnabled ? TEXT("required_plugin_disabled") : TEXT("required_plugin_ready"));
			const FString Severity = Plugin.bEnabled ? TEXT("info") : (Plugin.bInstalled ? TEXT("warning") : TEXT("error"));
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("plugin_readiness"), Severity, Code, Plugin.Name,
				Plugin.bEnabled
					? TEXT("Required Unreal plugin is installed and enabled.")
					: (Plugin.bInstalled
						? TEXT("Required Unreal plugin is installed but disabled or awaiting restart.")
						: TEXT("Required Unreal plugin was not found in this engine installation.")));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("installed"), Plugin.bInstalled ? TEXT("true") : TEXT("false"));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("enabled"), Plugin.bEnabled ? TEXT("true") : TEXT("false"));
			AddFindingBounded(MoveTemp(Finding));
		}
	}

	if (Normalized.Checks.Contains(TEXT("asset_registry_readiness")))
	{
		const bool bReady = Snapshot.bAssetRegistryAvailable
			&& !Snapshot.bAssetRegistryGathering
			&& Snapshot.bAssetRegistrySearchAllObserved;
		FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
			TEXT("asset_registry_readiness"),
			bReady ? TEXT("info") : TEXT("warning"),
			bReady ? TEXT("asset_registry_ready") : TEXT("asset_registry_incomplete"),
			TEXT("AssetRegistry"),
			bReady
				? TEXT("Asset Registry reports a completed all-assets search at capture time.")
				: TEXT("Asset Registry is unavailable, gathering, or has not observed a completed all-assets search."));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("available"), Snapshot.bAssetRegistryAvailable ? TEXT("true") : TEXT("false"));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("gathering"), Snapshot.bAssetRegistryGathering ? TEXT("true") : TEXT("false"));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("search_all_observed"), Snapshot.bAssetRegistrySearchAllObserved ? TEXT("true") : TEXT("false"));
		AddFindingBounded(MoveTemp(Finding));
		if (!bReady)
		{
			Result.bIncomplete = true;
		}
	}

	if (Normalized.Checks.Contains(TEXT("asset_context")))
	{
		for (const FHyperAIStudioDiagnosticsAssetSnapshot& Asset : Snapshot.Assets)
		{
			const TCHAR* Severity = Asset.bFound || Asset.bPackageAbsent
				? TEXT("info") : TEXT("warning");
			const TCHAR* Code = Asset.bFound
				? TEXT("asset_loaded_exact")
				: Asset.bPackageAbsent
					? TEXT("asset_package_absent")
					: Asset.bPackageExists
						? TEXT("asset_package_exists_identity_unverified")
						: TEXT("asset_identity_unavailable");
			const TCHAR* Message = Asset.bFound
				? TEXT("The exact canonical object is already loaded; package-state evidence is reported separately and no on-disk object lookup was attempted.")
				: Asset.bPackageAbsent
					? TEXT("The nonblocking registry query proves the entire target package absent; no exact object/class lookup was attempted.")
					: Asset.bPackageExists
						? TEXT("The package exists, but exact unloaded object/class identity requires the bounded asynchronous typed index.")
						: TEXT("Package/object identity is unavailable without blocking; disk absence is not inferred.");
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("asset_context"),
				Severity,
				Code,
				Asset.RequestedPath,
				Message);
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("package_state"),
				Asset.bPackageExists ? TEXT("exists")
					: Asset.bPackageAbsent ? TEXT("does_not_exist") : TEXT("unknown"));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("package_state_known"),
				Asset.bPackageStateKnown ? TEXT("true") : TEXT("false"));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("loaded_exact"),
				Asset.bFound ? TEXT("true") : TEXT("false"));
			if (Asset.bFound)
			{
				FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("asset_name"), Asset.AssetName);
				FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("class_path"), Asset.ClassPath);
				FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("object_path"), Asset.ObjectPath);
				FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("package_name"), Asset.PackageName);
			}
			AddFindingBounded(MoveTemp(Finding));
		}
		if (Normalized.AssetPaths.IsEmpty())
		{
			FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
				Result.Diagnostics,
				TEXT("asset_context_empty"),
				TEXT("info"),
				TEXT("asset_paths"),
				TEXT("asset_context was selected with no exact asset paths; no broad asset enumeration was attempted."));
		}
	}

	if (Normalized.Checks.Contains(TEXT("current_map_state")))
	{
		FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
			TEXT("current_map_state"),
			Snapshot.bEditorWorldAvailable ? (Snapshot.bCurrentMapDirty ? TEXT("warning") : TEXT("info")) : TEXT("error"),
			!Snapshot.bEditorWorldAvailable ? TEXT("editor_world_unavailable")
				: (Snapshot.bCurrentMapDirty ? TEXT("current_map_dirty") : TEXT("current_map_clean")),
			Snapshot.bEditorWorldAvailable ? Snapshot.CurrentMapPackage : TEXT("EditorWorld"),
			!Snapshot.bEditorWorldAvailable
				? TEXT("No current editor world was available at capture time.")
				: (Snapshot.bCurrentMapDirty
					? TEXT("Current map package has unsaved changes.")
					: TEXT("Current map package is not dirty.")));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("loaded"), Snapshot.bEditorWorldAvailable ? TEXT("true") : TEXT("false"));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("dirty"), Snapshot.bCurrentMapDirty ? TEXT("true") : TEXT("false"));
		AddFindingBounded(MoveTemp(Finding));
	}

	if (Normalized.Checks.Contains(TEXT("loaded_blueprint_health")))
	{
		int32 NonHealthy = 0;
		for (const FHyperAIStudioDiagnosticsBlueprintSnapshot& Blueprint : Snapshot.Blueprints)
		{
			if (Blueprint.Status == TEXT("up_to_date") && !Blueprint.bPackageDirty)
			{
				continue;
			}
			++NonHealthy;
			const bool bError = Blueprint.Status == TEXT("error") || Blueprint.Status == TEXT("invalid");
			FHyperAIStudioDiagnosticsFinding Finding = MakeFinding(
				TEXT("loaded_blueprint_health"),
				bError ? TEXT("error") : TEXT("warning"),
				bError ? TEXT("loaded_blueprint_compile_error") : TEXT("loaded_blueprint_needs_attention"),
				Blueprint.ObjectPath,
				bError
					? TEXT("Loaded Blueprint reports a compile error state.")
					: TEXT("Loaded Blueprint is dirty, compiling, unknown, or compiled with warnings."));
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("blueprint_status"), Blueprint.Status);
			FHyperAIStudioDiagnosticsCommon::AddFindingField(Finding, TEXT("package_dirty"), Blueprint.bPackageDirty ? TEXT("true") : TEXT("false"));
			AddFindingBounded(MoveTemp(Finding));
		}
		FHyperAIStudioDiagnosticsFinding Summary = MakeFinding(
			TEXT("loaded_blueprint_health"),
			NonHealthy == 0 ? TEXT("info") : TEXT("warning"),
			TEXT("loaded_blueprint_health_summary"),
			TEXT("LoadedBlueprints"),
			TEXT("Bounded health summary for already-loaded project Blueprints; unloaded assets were not loaded or assessed."));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Summary, TEXT("scanned"), FString::FromInt(Snapshot.BlueprintsScanned));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Summary, TEXT("needs_attention"), FString::FromInt(NonHealthy));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Summary, TEXT("scope"), TEXT("loaded_only"));
		AddFindingBounded(MoveTemp(Summary));
	}

	if (Normalized.Checks.Contains(TEXT("dirty_loaded_packages")))
	{
		for (const FString& Package : Snapshot.DirtyLoadedPackages)
		{
			AddFindingBounded(MakeFinding(
				TEXT("dirty_loaded_packages"),
				TEXT("warning"),
				TEXT("loaded_package_dirty"),
				Package,
				TEXT("Loaded project package has unsaved changes.")));
		}
		FHyperAIStudioDiagnosticsFinding Summary = MakeFinding(
			TEXT("dirty_loaded_packages"),
			Snapshot.DirtyLoadedPackages.IsEmpty() ? TEXT("info") : TEXT("warning"),
			TEXT("dirty_loaded_package_summary"),
			TEXT("LoadedProjectPackages"),
			TEXT("Bounded summary for already-loaded /Game packages."));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Summary, TEXT("scanned"), FString::FromInt(Snapshot.PackagesScanned));
		FHyperAIStudioDiagnosticsCommon::AddFindingField(Summary, TEXT("dirty"), FString::FromInt(Snapshot.DirtyLoadedPackages.Num()));
		AddFindingBounded(MoveTemp(Summary));
	}

	SortAndDeduplicateFindings(Findings);
	if (bFindingBoundReached)
	{
		Result.bIncomplete = true;
		Result.bCaptureWorkBoundReached = true;
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("diagnose_finding_bound_reached"),
			TEXT("warning"),
			TEXT("findings"),
			TEXT("Diagnostic findings reached the hard retained-record bound; omitted findings are explicit."));
	}
	Result.SnapshotFingerprint = FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint(
		Findings,
		{
			Snapshot.bIncomplete ? TEXT("incomplete") : TEXT("complete"),
			Snapshot.bCaptureWorkBoundReached ? TEXT("work_bound") : TEXT("within_bound"),
			FString::FromInt(Snapshot.BlueprintsScanned),
			FString::FromInt(Snapshot.PackagesScanned)
		});
	if (Result.SnapshotFingerprint.IsEmpty())
	{
		return InvalidResult(
			TEXT("snapshot_fingerprint_failed"),
			TEXT("snapshot"),
			TEXT("Diagnostic snapshot exceeded the fixed fingerprint budget or contained malformed text."),
			Request.Cursor);
	}

	FHyperAIStudioDiagnosticsProjectedPage Page;
	if (!FHyperAIStudioDiagnosticsCommon::ProjectFindings(
		Findings,
		Result.RequestFingerprint,
		Result.SnapshotFingerprint,
		Normalized.PageSize,
		Normalized.Cursor,
		Normalized.MaxOutputBytes,
		Page,
		ErrorCode,
		Error))
	{
		Result.Status = TEXT("invalid_request");
		Result.CursorStatus = Page.CursorStatus;
		Result.CursorDiagnosticCode = Page.CursorDiagnosticCode;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, ErrorCode, TEXT("error"), TEXT("cursor"), Error);
		return Result;
	}
	Result.TotalRecords = Page.TotalRecords;
	Result.PageOffset = Page.PageOffset;
	Result.ReturnedRecords = Page.ReturnedRecords;
	Result.bHasMore = Page.bHasMore;
	Result.CursorStatus = Page.CursorStatus;
	Result.CursorDiagnosticCode = Page.CursorDiagnosticCode;
	Result.NextCursor = Page.NextCursor;
	Result.bOutputBudgetReached = Page.bOutputBudgetReached;
	Result.Findings = MoveTemp(Page.Findings);
	Result.bTruncated = Result.bIncomplete || Result.bHasMore || Result.bOutputBudgetReached;
	Result.Status = Result.bIncomplete ? TEXT("partial") : TEXT("complete");
	if (Result.bOutputBudgetReached)
	{
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("output_budget_reached"),
			TEXT("warning"),
			TEXT("max_output_bytes"),
			TEXT("Finding projection reached the fixed approximate serialized output budget."));
	}
	return Result;
}

FHyperAIStudioDiagnosticsDiagnoseResult UHyperAIStudioDiagnoseToolset::hyper_diagnose(
	const FHyperAIStudioDiagnosticsDiagnoseRequest& Request)
{
	FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest Normalized;
	FString ErrorCode;
	FString Error;
	if (!FHyperAIStudioDiagnoseContracts::NormalizeRequest(Request, Normalized, ErrorCode, Error))
	{
		return HyperAIStudio::Diagnose::Private::InvalidResult(
			ErrorCode, TEXT("request"), Error, Request.Cursor);
	}
	const FHyperAIStudioDiagnosticsValueSnapshot Snapshot =
		FHyperAIStudioDiagnoseContracts::Capture(Normalized);
	return FHyperAIStudioDiagnoseContracts::Analyze(Snapshot, Request, &Normalized);
}
