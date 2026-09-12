// Games by Hyper 2026.

#include "HyperAIStudioGameplaySystemsToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "GameFeatureAction.h"
#include "GameFeatureData.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WorldConditionSchema.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGameplaySystems, Log, All);

namespace HyperAIStudio::GameplaySystems::Private
{
	constexpr int64 BaseOutputBytes = 8192;
	constexpr int64 RecordOutputBytes = 2048;
	constexpr int64 IssueOutputBytes = 1024;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += LexToString(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("|");
	}

	FString BoolToken(const bool bValue) { return bValue ? TEXT("1") : TEXT("0"); }

	FString HashCanonical(const FString& Canonical)
	{
		if (Canonical.Len() > FHyperAIStudioGameplaySystemsContracts::MaxCanonicalChars)
		{
			return {};
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	int32 RoleRank(const FString& Role)
	{
		if (Role == TEXT("game_feature_data")) return 0;
		if (Role == TEXT("mass_entity_config")) return 1;
		if (Role == TEXT("world_condition_schema")) return 2;
		return MAX_int32;
	}

	const TCHAR* ExpectedClassForRole(const FString& Role)
	{
		if (Role == TEXT("game_feature_data"))
			return FHyperAIStudioGameplaySystemsContracts::GameFeatureDataClassPath;
		if (Role == TEXT("mass_entity_config"))
			return FHyperAIStudioGameplaySystemsContracts::MassEntityConfigClassPath;
		if (Role == TEXT("world_condition_schema"))
			return FHyperAIStudioGameplaySystemsContracts::WorldConditionSchemaClassPath;
		return TEXT("");
	}

	const TCHAR* PluginForRole(const FString& Role)
	{
		if (Role == TEXT("game_feature_data")) return TEXT("GameFeatures");
		if (Role == TEXT("mass_entity_config")) return TEXT("MassAI");
		if (Role == TEXT("world_condition_schema")) return TEXT("WorldConditions");
		return TEXT("");
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	void AddIssue(TArray<FHyperAIGameplaySystemsIssue>& Issues, const TCHAR* Code,
		const TCHAR* Severity, const FString& Role, const FString& Path,
		const TCHAR* Message, const int32 MaxIssues)
	{
		if (Issues.Num() >= MaxIssues) return;
		FHyperAIGameplaySystemsIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.Role = Role;
		Issue.ObjectPath = Path;
		Issue.Message = Message;
	}

	bool ValidateRequestShape(const FString& FeaturePluginName,
		const TArray<FHyperAIGameplaySystemsTarget>& Targets, FString& OutError)
	{
		if (!FHyperAIStudioGameplaySystemsContracts::IsSafeFeaturePluginName(
			FeaturePluginName) || Targets.Num() != FHyperAIStudioGameplaySystemsContracts::MaxTargets)
		{
			OutError = TEXT("Exactly three closed targets and one bounded feature plugin name are required.");
			return false;
		}
		TSet<FString> Roles;
		int64 InputBytes = 2ll * FeaturePluginName.Len();
		for (const FHyperAIGameplaySystemsTarget& Target : Targets)
		{
			if (Target.Role.Len() > 32
				|| Target.ObjectPath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars)
			{
				OutError = TEXT("A target exceeds pre-copy scalar bounds.");
				return false;
			}
			InputBytes += 2ll * (Target.Role.Len() + Target.ObjectPath.Len());
			if (InputBytes > 8192 || RoleRank(Target.Role) == MAX_int32
				|| Roles.Contains(Target.Role)
				|| !FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
					Target.Role, Target.ObjectPath, FeaturePluginName))
			{
				OutError = TEXT("Targets must be unique canonical paths for the exact three roles.");
				return false;
			}
			Roles.Add(Target.Role);
		}
		return Roles.Num() == FHyperAIStudioGameplaySystemsContracts::MaxTargets;
	}

	const FHyperAIGameplaySystemsTarget* FindTarget(
		const TArray<FHyperAIGameplaySystemsTarget>& Targets, const FString& Role)
	{
		return Targets.FindByPredicate([&](const FHyperAIGameplaySystemsTarget& Target)
		{
			return Target.Role == Role;
		});
	}

	bool SnapshotScalarsAreBounded(
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
	{
		if (Snapshot.Records.Num()
			!= FHyperAIStudioGameplaySystemsContracts::MaxTargets
			|| Snapshot.FeaturePluginName.Len()
				> FHyperAIStudioGameplaySystemsContracts::MaxFeaturePluginNameChars
			|| Snapshot.FeatureMountPoint.Len()
				> FHyperAIStudioGameplaySystemsContracts::MaxPathChars
			|| Snapshot.RequestFingerprint.Len() > 71
			|| Snapshot.PersistedFingerprint.Len() > 71
			|| Snapshot.VolatileObservationFingerprint.Len() > 71)
		{
			return false;
		}
		int64 Bytes = 2ll * (Snapshot.FeaturePluginName.Len()
			+ Snapshot.FeatureMountPoint.Len() + Snapshot.RequestFingerprint.Len()
			+ Snapshot.PersistedFingerprint.Len()
			+ Snapshot.VolatileObservationFingerprint.Len());
		for (const FHyperAIGameplaySystemsRecord& Record : Snapshot.Records)
		{
			if (Record.Role.Len() > 32
				|| Record.ObjectPath.Len()
					> FHyperAIStudioGameplaySystemsContracts::MaxPathChars
				|| Record.ExpectedClassPath.Len()
					> FHyperAIStudioGameplaySystemsContracts::MaxPathChars
				|| Record.ActualClassPath.Len()
					> FHyperAIStudioGameplaySystemsContracts::MaxPathChars
				|| Record.PluginName.Len() > 64 || Record.PackageState.Len() > 32
				|| Record.ContentFingerprint.Len() > 71
				|| Record.ElementFingerprint.Len() > 71)
			{
				return false;
			}
			Bytes += 2ll * (Record.Role.Len() + Record.ObjectPath.Len()
				+ Record.ExpectedClassPath.Len() + Record.ActualClassPath.Len()
				+ Record.PluginName.Len() + Record.PackageState.Len()
				+ Record.ContentFingerprint.Len() + Record.ElementFingerprint.Len());
			if (Bytes > 16384) return false;
		}
		return true;
	}

	FString BuildContentFingerprint(const UGameFeatureData* Data, bool& bComplete,
		int32& OutCount, const double DeadlineSeconds)
	{
		const TArray<UGameFeatureAction*>& Actions = Data->GetActions();
		OutCount = Actions.Num();
		if (OutCount > FHyperAIStudioGameplaySystemsContracts::MaxEntriesPerTarget)
		{
			bComplete = false;
			return {};
		}
		FString Canonical(TEXT("hyperai.gameplay-systems.game-feature-content.v1|"));
		for (const UGameFeatureAction* Action : Actions)
		{
			if (!Action || DeadlineExceeded(DeadlineSeconds))
			{
				bComplete = false;
				return {};
			}
			const FString ClassPath = Action->GetClass()->GetPathName();
			const FString ObjectPath = Action->GetPathName(Data);
			if (ClassPath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars
				|| ObjectPath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars)
			{
				bComplete = false;
				return {};
			}
			AppendToken(Canonical, ClassPath);
			AppendToken(Canonical, ObjectPath);
		}
		return HashCanonical(Canonical);
	}

	FString BuildContentFingerprint(const UMassEntityConfigAsset* Data, bool& bComplete,
		int32& OutCount, const double DeadlineSeconds)
	{
		const FMassEntityConfig& Config = Data->GetConfig();
		const TConstArrayView<UMassEntityTraitBase*> Traits = Config.GetTraits();
		OutCount = Traits.Num();
		if (OutCount > FHyperAIStudioGameplaySystemsContracts::MaxEntriesPerTarget)
		{
			bComplete = false;
			return {};
		}
		FString Canonical(TEXT("hyperai.gameplay-systems.mass-config-content.v1|"));
		AppendToken(Canonical, Config.GetParent() ? Config.GetParent()->GetPathName() : FString());
		for (const UMassEntityTraitBase* Trait : Traits)
		{
			if (!Trait || DeadlineExceeded(DeadlineSeconds))
			{
				bComplete = false;
				return {};
			}
			const FString ClassPath = Trait->GetClass()->GetPathName();
			const FString ObjectPath = Trait->GetPathName(Data);
			if (ClassPath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars
				|| ObjectPath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars)
			{
				bComplete = false;
				return {};
			}
			AppendToken(Canonical, ClassPath);
			AppendToken(Canonical, ObjectPath);
		}
		return HashCanonical(Canonical);
	}

	FString BuildContentFingerprint(const UWorldConditionSchema* Schema, bool& bComplete,
		int32& OutCount, const double DeadlineSeconds)
	{
		const TConstArrayView<FWorldConditionContextDataDesc> Contexts =
			Schema->GetContextDataDescs();
		OutCount = Contexts.Num();
		if (OutCount > FHyperAIStudioGameplaySystemsContracts::MaxEntriesPerTarget)
		{
			bComplete = false;
			return {};
		}
		FString Canonical(TEXT("hyperai.gameplay-systems.world-condition-schema.v1|"));
		for (const FWorldConditionContextDataDesc& Context : Contexts)
		{
			if (!Context.Struct || DeadlineExceeded(DeadlineSeconds))
			{
				bComplete = false;
				return {};
			}
			const FString Name = Context.Name.ToString();
			const FString TypePath = Context.Struct->GetPathName();
			if (Name.Len() > 128
				|| TypePath.Len() > FHyperAIStudioGameplaySystemsContracts::MaxPathChars)
			{
				bComplete = false;
				return {};
			}
			AppendToken(Canonical, Name);
			AppendToken(Canonical, TypePath);
			AppendToken(Canonical, LexToString(static_cast<uint8>(Context.Type)));
		}
		return HashCanonical(Canonical);
	}

	bool IsRecordComplete(const FHyperAIGameplaySystemsRecord& Record)
	{
		if (!Record.bLoaded || !Record.bClassCompatible || !Record.bPluginFound
			|| !Record.bPluginEnabled || Record.EntryCount < 0
			|| !FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(
				Record.ContentFingerprint))
		{
			return false;
		}
		if (Record.Role == TEXT("world_condition_schema"))
		{
			return Record.PackageState == TEXT("not_applicable");
		}
		return Record.PackageState == TEXT("exists") && Record.bWasLoaded && !Record.bDirty;
	}

	bool Capture(const FHyperAIGameplaySystemsInspectRequest& Request,
		FHyperAIGameplaySystemsDetachedSnapshot& OutSnapshot,
		TArray<FHyperAIGameplaySystemsIssue>& OutIssues, FString& OutStatus,
		FString& OutDiagnostic)
	{
		const double Started = FPlatformTime::Seconds();
		const double DeadlineSeconds = Started + FMath::Min(Request.DeadlineMs,
			Request.MaxGameThreadMs) / 1000.0;
		OutSnapshot = {};
		OutSnapshot.FeaturePluginName = Request.FeaturePluginName;

		IPluginManager& PluginManager = IPluginManager::Get();
		const TSharedPtr<IPlugin> FeaturePlugin =
			PluginManager.FindPlugin(Request.FeaturePluginName);
		OutSnapshot.bFeaturePluginFound = FeaturePlugin.IsValid();
		if (FeaturePlugin)
		{
			OutSnapshot.bFeaturePluginEnabled = FeaturePlugin->IsEnabled();
			OutSnapshot.bFeaturePluginProjectOwned =
				FeaturePlugin->GetLoadedFrom() == EPluginLoadedFrom::Project;
			OutSnapshot.bFeaturePluginContainsContent = FeaturePlugin->CanContainContent();
			OutSnapshot.bFeaturePluginExplicitlyLoaded =
				FeaturePlugin->GetDescriptor().bExplicitlyLoaded;
			OutSnapshot.FeatureMountPoint = FeaturePlugin->GetMountedAssetPath();
		}
		if (OutSnapshot.FeatureMountPoint.IsEmpty())
		{
			OutSnapshot.FeatureMountPoint = TEXT("/") + Request.FeaturePluginName + TEXT("/");
		}

		static const TArray<FString> Roles = {
			TEXT("game_feature_data"), TEXT("mass_entity_config"),
			TEXT("world_condition_schema")};
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		for (const FString& Role : Roles)
		{
			if (DeadlineExceeded(DeadlineSeconds))
			{
				OutStatus = TEXT("deadline_exceeded_before_complete_capture");
				OutDiagnostic = TEXT("The bounded loaded-only preflight exceeded its game-thread budget.");
				return false;
			}
			const FHyperAIGameplaySystemsTarget* Target = FindTarget(Request.Targets, Role);
			if (!Target) return false;
			FHyperAIGameplaySystemsRecord& Record = OutSnapshot.Records.AddDefaulted_GetRef();
			Record.Role = Role;
			Record.ObjectPath = Target->ObjectPath;
			Record.ExpectedClassPath = ExpectedClassForRole(Role);
			Record.PluginName = PluginForRole(Role);
			const TSharedPtr<IPlugin> RequiredPlugin = PluginManager.FindPlugin(Record.PluginName);
			Record.bPluginFound = RequiredPlugin.IsValid();
			Record.bPluginEnabled = RequiredPlugin && RequiredPlugin->IsEnabled();

			bool bContentComplete = true;
			if (Role == TEXT("world_condition_schema"))
			{
				Record.PackageState = TEXT("not_applicable");
				UClass* SchemaClass = FindObjectSafe<UClass>(nullptr, *Target->ObjectPath);
				Record.bLoaded = SchemaClass != nullptr;
				Record.bWasLoaded = SchemaClass && SchemaClass->HasAnyFlags(RF_WasLoaded);
				Record.bDirty = SchemaClass && SchemaClass->GetPackage()
					&& SchemaClass->GetPackage()->IsDirty();
				Record.ActualClassPath = SchemaClass ? SchemaClass->GetPathName() : FString();
				Record.bClassCompatible = SchemaClass
					&& SchemaClass->IsChildOf(UWorldConditionSchema::StaticClass());
				const UWorldConditionSchema* Schema = SchemaClass
					? Cast<UWorldConditionSchema>(SchemaClass->GetDefaultObject(false)) : nullptr;
				if (Schema)
				{
					Record.ContentFingerprint = BuildContentFingerprint(
						Schema, bContentComplete, Record.EntryCount, DeadlineSeconds);
				}
				else
				{
					bContentComplete = false;
				}
			}
			else
			{
				const FString PackageName = FPackageName::ObjectPathToPackageName(Target->ObjectPath);
				FAssetPackageData PackageData;
				const UE::AssetRegistry::EExists PackageState = AssetRegistry
					? AssetRegistry->TryGetAssetPackageData(
						FName(*PackageName), PackageData, /*bFailIfLockHeld=*/true)
					: UE::AssetRegistry::EExists::Unknown;
				Record.PackageState = FHyperAIStudioGameplaySystemsContracts::
					ClassifyPackageExistence(static_cast<int32>(PackageState));
				UObject* Object = FindObjectSafe<UObject>(nullptr, *Target->ObjectPath);
				Record.bLoaded = Object != nullptr;
				Record.bWasLoaded = Object && Object->HasAnyFlags(RF_WasLoaded);
				Record.bDirty = Object && Object->GetPackage() && Object->GetPackage()->IsDirty();
				Record.ActualClassPath = Object ? Object->GetClass()->GetPathName() : FString();
				if (Role == TEXT("game_feature_data"))
				{
					const UGameFeatureData* Data = Cast<UGameFeatureData>(Object);
					Record.bClassCompatible = Data != nullptr;
					if (Data)
					{
						Record.ContentFingerprint = BuildContentFingerprint(
							Data, bContentComplete, Record.EntryCount, DeadlineSeconds);
					}
					else bContentComplete = false;
				}
				else
				{
					const UMassEntityConfigAsset* Data = Cast<UMassEntityConfigAsset>(Object);
					Record.bClassCompatible = Data != nullptr;
					if (Data)
					{
						Record.ContentFingerprint = BuildContentFingerprint(
							Data, bContentComplete, Record.EntryCount, DeadlineSeconds);
					}
					else bContentComplete = false;
				}
			}

			Record.ElementFingerprint =
				FHyperAIStudioGameplaySystemsContracts::ComputeElementFingerprint(Record);
			if (Record.PackageState == TEXT("unknown"))
			{
				AddIssue(OutIssues, TEXT("asset_registry_unknown"), TEXT("error"), Role,
					Record.ObjectPath,
					TEXT("Fail-fast package evidence is Unknown; no absence, identity, or readiness claim is allowed."),
					FHyperAIStudioGameplaySystemsContracts::MaxIssues);
			}
			if (!bContentComplete || !IsRecordComplete(Record))
			{
				AddIssue(OutIssues, TEXT("target_preflight_incomplete"), TEXT("error"), Role,
					Record.ObjectPath,
					TEXT("The exact target/plugin/type/content projection is not complete, clean, and loaded."),
					FHyperAIStudioGameplaySystemsContracts::MaxIssues);
			}
		}

		OutSnapshot.RequestFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputeRequestFingerprint(
				OutSnapshot.FeaturePluginName, OutSnapshot.Records);
		OutSnapshot.PersistedFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputePersistedFingerprint(OutSnapshot);
		OutSnapshot.VolatileObservationFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputeVolatileFingerprint(OutSnapshot);
		OutSnapshot.bSnapshotComplete = OutSnapshot.bFeaturePluginFound
			&& OutSnapshot.bFeaturePluginEnabled
			&& OutSnapshot.bFeaturePluginProjectOwned
			&& OutSnapshot.bFeaturePluginContainsContent
			&& OutSnapshot.bFeaturePluginExplicitlyLoaded
			&& OutSnapshot.FeatureMountPoint == TEXT("/") + Request.FeaturePluginName + TEXT("/")
			&& OutSnapshot.Records.Num() == 3
			&& OutSnapshot.Records.ContainsByPredicate([](const FHyperAIGameplaySystemsRecord& R)
				{ return R.Role == TEXT("game_feature_data") && IsRecordComplete(R); })
			&& OutSnapshot.Records.ContainsByPredicate([](const FHyperAIGameplaySystemsRecord& R)
				{ return R.Role == TEXT("mass_entity_config") && IsRecordComplete(R); })
			&& OutSnapshot.Records.ContainsByPredicate([](const FHyperAIGameplaySystemsRecord& R)
				{ return R.Role == TEXT("world_condition_schema") && IsRecordComplete(R); });
		if (!OutSnapshot.bSnapshotComplete)
		{
			AddIssue(OutIssues, TEXT("composed_preflight_not_ready"), TEXT("error"), FString(),
				FString(),
				TEXT("The project-owned Game Feature plus MassAI/GameFeatures/WorldConditions composition is not fully ready."),
				FHyperAIStudioGameplaySystemsContracts::MaxIssues);
		}
		return true;
	}

	FString InspectPayloadSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("gameplay-systems.inspect.v1|feature_plugin:string|targets:exact3|deadline:int|game_thread:int|output:int"));
		return Value;
	}

	FString ValidatePayloadSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("gameplay-systems.validate.v1|detached_snapshot:value|max_issues:int|deadline:int|output:int"));
		return Value;
	}

	FString InspectResultSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("gameplay-systems.inspect-result.v1|snapshot:exact3|persisted:sha256|volatile:sha256|issues:bounded"));
		return Value;
	}

	FString ApplyResultSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("gameplay-systems.apply-result.v1|prepared:bool|staged:false|submitted:false|hashes:sealed"));
		return Value;
	}

	FString ValidateResultSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("gameplay-systems.validate-result.v1|valid:bool|recomputed_seals:sha256|issues:bounded"));
		return Value;
	}

	int64 EstimateOutputBytes(const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot,
		const TArray<FHyperAIGameplaySystemsIssue>& Issues)
	{
		return BaseOutputBytes + RecordOutputBytes * Snapshot.Records.Num()
			+ IssueOutputBytes * Issues.Num()
			+ 6ll * (Snapshot.FeaturePluginName.Len() + Snapshot.FeatureMountPoint.Len()
				+ Snapshot.RequestFingerprint.Len() + Snapshot.PersistedFingerprint.Len()
				+ Snapshot.VolatileObservationFingerprint.Len());
	}

	FHyperAIGameplaySystemsApplyPlanReport PrepareFromSnapshot(
		const FHyperAIGameplaySystemsApplyPlanRequest& Request,
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
	{
		FHyperAIGameplaySystemsApplyPlanReport Report;
		Report.bDryRun = Request.bDryRun;
		Report.OperationId = Request.OperationId.Left(
			FHyperAIStudioDomainLimits::MaxOperationIdChars);
		auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.bStaged = false;
			Report.bExecutionSubmitted = false;
			Report.Effects.bProjectAssetMutationAttempted = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};

		FString ShapeError;
		if (!ValidateRequestShape(Request.FeaturePluginName, Request.Targets, ShapeError)
			|| Request.Operation != FHyperAIStudioGameplaySystemsContracts::ClosedOperation
			|| !FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(
				Request.ExpectedPersistedFingerprint)
			|| Request.DeadlineMs < 100
			|| Request.DeadlineMs > FHyperAIStudioGameplaySystemsContracts::MaxDeadlineMs
			|| Request.MaxGameThreadMs < 50
			|| Request.MaxGameThreadMs > FHyperAIStudioGameplaySystemsContracts::MaxGameThreadMs
			|| Request.MaxOutputBytes < 4096
			|| Request.MaxOutputBytes > FHyperAIStudioGameplaySystemsContracts::MaxOutputBytes)
		{
			return Reject(TEXT("invalid_plan_shape_or_bounds"), ShapeError.IsEmpty()
				? TEXT("The closed preflight plan violates identity or budget bounds.") : ShapeError);
		}
		if (Request.bDryRun && (!Request.OperationId.IsEmpty()
			|| !Request.ExpectedPlanHash.IsEmpty()))
		{
			return Reject(TEXT("unexpected_submission_fields"),
				TEXT("Dry-run preparation prohibits operation_id and expected_plan_hash."));
		}
		if (!Request.bDryRun
			&& (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
				|| !FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(
					Request.ExpectedPlanHash)))
		{
			return Reject(TEXT("invalid_submission_identity"),
				TEXT("Non-dry intent requires one valid operation_id and exact dry-run plan hash."));
		}

		if (!SnapshotScalarsAreBounded(Snapshot))
		{
			return Reject(TEXT("preflight_snapshot_bounds_exceeded"),
				TEXT("The detached preflight exceeds fixed scalar or aggregate pre-copy bounds."));
		}
		FHyperAIGameplaySystemsValidateRequest ValidateRequest;
		ValidateRequest.Snapshot = Snapshot;
		ValidateRequest.MaxIssues = FHyperAIStudioGameplaySystemsContracts::MaxIssues;
		ValidateRequest.DeadlineMs = Request.DeadlineMs;
		ValidateRequest.MaxOutputBytes = Request.MaxOutputBytes;
		const FHyperAIGameplaySystemsValidateReport Validation =
			FHyperAIStudioGameplaySystemsContracts::ValidateDetached(ValidateRequest);
		Report.Issues = Validation.Issues;
		if (!Validation.bOk || !Validation.bValid)
		{
			return Reject(TEXT("preflight_snapshot_invalid"), Validation.Diagnostic);
		}
		if (Snapshot.FeaturePluginName != Request.FeaturePluginName
			|| Snapshot.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
		{
			return Reject(TEXT("stale_or_wrong_preflight_snapshot"),
				TEXT("The detached preflight identity no longer matches the exact plan CAS."));
		}
		for (const FHyperAIGameplaySystemsTarget& Target : Request.Targets)
		{
			if (!Snapshot.Records.ContainsByPredicate(
				[&](const FHyperAIGameplaySystemsRecord& Record)
				{
					return Record.Role == Target.Role && Record.ObjectPath == Target.ObjectPath;
				}))
			{
				return Reject(TEXT("target_set_mismatch"),
					TEXT("The exact three plan targets do not match the detached preflight."));
			}
		}

		const TSharedRef<FHyperAIStudioGameplaySystemsPlanPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioGameplaySystemsPlanPayload, ESPMode::ThreadSafe>();
		Payload->FeaturePluginName = Snapshot.FeaturePluginName;
		Payload->FeatureMountPoint = Snapshot.FeatureMountPoint;
		Payload->BasePersistedFingerprint = Snapshot.PersistedFingerprint;
		for (const FHyperAIGameplaySystemsRecord& Record : Snapshot.Records)
		{
			Payload->CanonicalTargets.Add(Record.Role + TEXT("=") + Record.ObjectPath);
		}
		Payload->SemanticFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputePayloadSemanticFingerprint(*Payload);
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
			Payload->CloneImmutable();
		if (&Clone.Get() == &Payload.Get()
			|| Clone->GetTypeId() != Payload->GetTypeId()
			|| Clone->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
			|| Clone->GetSemanticFingerprint() != Payload->GetSemanticFingerprint())
		{
			return Reject(TEXT("immutable_clone_failed"),
				TEXT("The closed preflight payload did not produce an exact detached clone."));
		}

		const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioGameplaySystemsContracts::GetPreparationDescriptor();
		if (ProjectId.IsEmpty()
			|| !FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(
				Descriptor.AdapterFingerprint))
		{
			return Reject(TEXT("preparation_identity_unavailable"),
				TEXT("Canonical project or exact preparation descriptor identity is unavailable."));
		}

		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioGameplaySystemsContracts::PackId;
		Binding.ToolName = TEXT("hyper_gameplay_systems_apply_plan");
		Binding.VariantId = FHyperAIStudioGameplaySystemsContracts::ApplyVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = ProjectId;
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("plugin.MassAI"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("plugin.GameFeatures"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("plugin.WorldConditions"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("probe.gameplay_systems"), EHyperAIStudioDomainPrerequisiteState::Available}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
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
		Contract.EffectTarget = Snapshot.Records[0].ObjectPath;
		Contract.DeadlineMs = Request.DeadlineMs;
		Contract.MaxNativeOperations = 5;
		Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
		Contract.MaxOutputBytes = Request.MaxOutputBytes;
		Contract.MaxResultBytes = 128;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = false;
		Contract.bSaveOnce = false;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		FHyperAIStudioPreparedTypedArtifact Prepared;
		FString PrepareError;
		if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
		{
			return Reject(TEXT("typed_artifact_prepare_failed"), PrepareError);
		}

		Report.bTypedPrepared = true;
		Report.BasePersistedFingerprint = Snapshot.PersistedFingerprint;
		Report.SemanticFingerprint = Payload->SemanticFingerprint;
		Report.PlanHash = Prepared.PlanHash;
		Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
		Report.CapabilityHash = Prepared.CapabilityHash;
		Report.EffectFingerprint = Prepared.EffectFingerprint;
		Report.Effects.TargetCount = 3;
		Report.Effects.bWouldPersistJournalReceiptOnce = true;
		Report.Effects.bWouldValidateOnce = true;
		Report.Effects.bWouldVerifyFreshOnce = true;
		Report.Effects.bProjectAssetMutationAttempted = false;
		if (Request.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("dry_run_preflight_prepared");
			Report.Diagnostic = TEXT("A pure detached preflight artifact was sealed. No project asset, plugin state, registry state, journal, stage, adapter, or external effect changed.");
			return Report;
		}
		if (Request.ExpectedPlanHash != Prepared.PlanHash)
		{
			return Reject(TEXT("plan_hash_mismatch"),
				TEXT("Non-dry intent does not echo the exact fresh dry-run plan hash."));
		}
		return Reject(FHyperAIStudioGameplaySystemsContracts::NonDryCallableState,
			TEXT("No effect ran. A core-owned typed backend is required to journal, validate, and fresh-verify this composed authoring preflight; Game Feature activation remains an exact Epic delegate."));
	}
}

FString FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioGameplaySystems.HyperAIStudioGameplaySystemsToolset");
}

const TArray<FHyperAIStudioGameplaySystemsManifestEntry>&
FHyperAIStudioGameplaySystemsContracts::GetManifest()
{
	static const TArray<FHyperAIStudioGameplaySystemsManifestEntry> Manifest = {
		{TEXT("hyper_gameplay_systems_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_gameplay_systems_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_gameplay_systems_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

const TArray<FString>& FHyperAIStudioGameplaySystemsContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("GameFeaturesToolset.GameFeaturesToolset.RequestDeactivateGameFeature"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.ListEnabledGameFeaturePlugins"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.ListDiscoveredGameFeaturePlugins"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.IsGameFeaturePlugin"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.IsGameFeatureActive"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.GetGameFeatureState"),
		TEXT("GameFeaturesToolset.GameFeaturesToolset.RequestActivateGameFeature"),
		TEXT("WorldConditionsToolset.WorldConditionTools.GetQueryDescription"),
		TEXT("WorldConditionsToolset.WorldConditionTools.GetConditionDescription")};
	return Delegates;
}

TArray<FHyperAIGameplaySystemsCapabilityStatus>
FHyperAIStudioGameplaySystemsContracts::GetCapabilityMatrix()
{
	FHyperAIGameplaySystemsCapabilityStatus Status;
	Status.DelegatedEpicCallables = GetEpicDelegates();
	Status.ClosedCases = {
		TEXT("loaded-only exact GameFeatureData plus bounded action identity seal"),
		TEXT("loaded-only exact MassEntityConfig plus bounded trait identity seal"),
		TEXT("loaded WorldConditionSchema plus bounded context descriptor seal"),
		TEXT("one project-owned feature mount and cross-system readiness preflight")};
	Status.UnsupportedCases = {
		TEXT("Game Feature activation/deactivation/state operations delegated to Epic"),
		TEXT("World Condition descriptions delegated to Epic"),
		TEXT("Smart Object and Gameplay Behavior authoring owned by Gameplay AI"),
		TEXT("asset creation, trait/action mutation, plugin enablement, or arbitrary reflection")};
	Status.Remediation = TEXT("Enable the exact optional plugins and load clean target objects. Use Epic's direct GameFeatures/WorldConditions callables for equivalent lifecycle work.");
	return {MoveTemp(Status)};
}

bool FHyperAIStudioGameplaySystemsContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioGameplaySystemsManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioGameplaySystemsManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioGameplaySystemsContracts::IsSafeFeaturePluginName(
	const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > MaxFeaturePluginNameChars
		|| !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_'))) return false;
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('_'))) return false;
	}
	return true;
}

bool FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
	const FString& Role, const FString& Path, const FString& FeaturePluginName)
{
	if (!IsSafeFeaturePluginName(FeaturePluginName) || Path.IsEmpty()
		|| Path.Len() > MaxPathChars || Path.Contains(TEXT("*"))
		|| Path.Contains(TEXT("?")) || Path.Contains(TEXT(":"))) return false;
	for (const TCHAR Character : Path) if (FChar::IsControl(Character)) return false;
	int32 ObjectDelimiter = INDEX_NONE;
	if (!Path.FindChar(TEXT('.'), ObjectDelimiter) || ObjectDelimiter <= 0
		|| ObjectDelimiter == Path.Len() - 1
		|| Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			!= ObjectDelimiter) return false;
	const FString PackageName = Path.Left(ObjectDelimiter);
	const FString ObjectName = Path.Mid(ObjectDelimiter + 1);
	FText Reason;
	if (!FPackageName::IsValidTextForLongPackageName(PackageName, &Reason)
		|| !FName::IsValidXName(ObjectName, INVALID_OBJECTPATH_CHARACTERS, &Reason)
		|| ObjectName.Contains(TEXT("/"))) return false;
	if (Role == TEXT("world_condition_schema")) return Path.StartsWith(TEXT("/Script/"));
	if (FPackageName::GetLongPackageAssetName(PackageName) != ObjectName) return false;
	const FString FeatureRoot = TEXT("/") + FeaturePluginName + TEXT("/");
	if (Role == TEXT("game_feature_data")) return Path.StartsWith(FeatureRoot);
	if (Role == TEXT("mass_entity_config"))
		return Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(FeatureRoot);
	return false;
}

FString FHyperAIStudioGameplaySystemsContracts::ClassifyPackageExistence(
	const int32 StateValue)
{
	const UE::AssetRegistry::EExists State =
		static_cast<UE::AssetRegistry::EExists>(StateValue);
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists: return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist: return TEXT("does_not_exist");
	default: return TEXT("unknown");
	}
}

FString FHyperAIStudioGameplaySystemsContracts::ComputeRequestFingerprint(
	const FString& FeaturePluginName,
	const TArray<FHyperAIGameplaySystemsRecord>& Records)
{
	if (!IsSafeFeaturePluginName(FeaturePluginName) || Records.Num() != 3) return {};
	FString Canonical(TEXT("hyperai.gameplay-systems.request.v1|"));
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, FeaturePluginName);
	for (int32 Index = 0; Index < Records.Num(); ++Index)
	{
		if (HyperAIStudio::GameplaySystems::Private::RoleRank(Records[Index].Role) != Index
			|| !IsCanonicalTargetPath(Records[Index].Role, Records[Index].ObjectPath,
				FeaturePluginName)) return {};
		HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Records[Index].Role);
		HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Records[Index].ObjectPath);
	}
	return HyperAIStudio::GameplaySystems::Private::HashCanonical(Canonical);
}

FString FHyperAIStudioGameplaySystemsContracts::ComputeElementFingerprint(
	const FHyperAIGameplaySystemsRecord& Record)
{
	FString Canonical(TEXT("hyperai.gameplay-systems.element.v1|"));
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Record.Role);
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Record.ObjectPath);
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Record.ExpectedClassPath);
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Record.ActualClassPath);
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical,
		HyperAIStudio::GameplaySystems::Private::BoolToken(Record.bClassCompatible));
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical,
		FString::FromInt(Record.EntryCount));
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical, Record.ContentFingerprint);
	return HyperAIStudio::GameplaySystems::Private::HashCanonical(Canonical);
}

FString FHyperAIStudioGameplaySystemsContracts::ComputePersistedFingerprint(
	const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
{
	if (Snapshot.Records.Num() != 3) return {};
	FString Canonical(TEXT("hyperai.gameplay-systems.persisted.v1|"));
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical,
		Snapshot.FeaturePluginName);
	HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical,
		Snapshot.FeatureMountPoint);
	for (const FHyperAIGameplaySystemsRecord& Record : Snapshot.Records)
	{
		HyperAIStudio::GameplaySystems::Private::AppendToken(Canonical,
			Record.ElementFingerprint);
	}
	return HyperAIStudio::GameplaySystems::Private::HashCanonical(Canonical);
}

FString FHyperAIStudioGameplaySystemsContracts::ComputeVolatileFingerprint(
	const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
{
	if (Snapshot.Records.Num() != 3) return {};
	using namespace HyperAIStudio::GameplaySystems::Private;
	FString Canonical(TEXT("hyperai.gameplay-systems.volatile.v1|"));
	AppendToken(Canonical, BoolToken(Snapshot.bFeaturePluginFound));
	AppendToken(Canonical, BoolToken(Snapshot.bFeaturePluginEnabled));
	AppendToken(Canonical, BoolToken(Snapshot.bFeaturePluginProjectOwned));
	AppendToken(Canonical, BoolToken(Snapshot.bFeaturePluginContainsContent));
	AppendToken(Canonical, BoolToken(Snapshot.bFeaturePluginExplicitlyLoaded));
	for (const FHyperAIGameplaySystemsRecord& Record : Snapshot.Records)
	{
		AppendToken(Canonical, Record.Role);
		AppendToken(Canonical, Record.PackageState);
		AppendToken(Canonical, BoolToken(Record.bLoaded));
		AppendToken(Canonical, BoolToken(Record.bWasLoaded));
		AppendToken(Canonical, BoolToken(Record.bDirty));
		AppendToken(Canonical, BoolToken(Record.bPluginFound));
		AppendToken(Canonical, BoolToken(Record.bPluginEnabled));
	}
	return HashCanonical(Canonical);
}

FHyperAIGameplaySystemsValidateReport
FHyperAIStudioGameplaySystemsContracts::ValidateDetached(
	const FHyperAIGameplaySystemsValidateRequest& Request)
{
	using namespace HyperAIStudio::GameplaySystems::Private;
	FHyperAIGameplaySystemsValidateReport Report;
	auto AddError = [&](const TCHAR* Code, const FString& Role,
		const FString& Path, const TCHAR* Message)
	{
		AddIssue(Report.Issues, Code, TEXT("error"), Role, Path, Message,
			FMath::Clamp(Request.MaxIssues, 1, MaxIssues));
	};
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < 4096 || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_validation_bounds");
		Report.Diagnostic = TEXT("Detached validation bounds are outside the closed envelope.");
		return Report;
	}
	const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot = Request.Snapshot;
	if (!SnapshotScalarsAreBounded(Snapshot)
		|| !IsSafeFeaturePluginName(Snapshot.FeaturePluginName)
		|| Snapshot.FeatureMountPoint != TEXT("/") + Snapshot.FeaturePluginName + TEXT("/")
		|| Snapshot.Records.Num() != 3)
	{
		Report.Status = TEXT("invalid_snapshot_shape");
		Report.Diagnostic = TEXT("Detached snapshot does not contain one canonical feature mount and exactly three records.");
		return Report;
	}
	bool bAllRecordsComplete = true;
	for (int32 Index = 0; Index < Snapshot.Records.Num(); ++Index)
	{
		const FHyperAIGameplaySystemsRecord& Record = Snapshot.Records[Index];
		if (RoleRank(Record.Role) != Index
			|| Record.ExpectedClassPath != ExpectedClassForRole(Record.Role)
			|| Record.PluginName != PluginForRole(Record.Role)
			|| !IsCanonicalTargetPath(Record.Role, Record.ObjectPath,
				Snapshot.FeaturePluginName)
			|| Record.ActualClassPath.Len() > MaxPathChars
			|| (Record.PackageState != TEXT("exists")
				&& Record.PackageState != TEXT("does_not_exist")
				&& Record.PackageState != TEXT("unknown")
				&& Record.PackageState != TEXT("not_applicable"))
			|| Record.EntryCount < 0 || Record.EntryCount > MaxEntriesPerTarget)
		{
			AddError(TEXT("invalid_record_shape"), Record.Role, Record.ObjectPath,
				TEXT("Record role, type, path, ordering, or count is outside the closed schema."));
		}
		if (Record.bPluginEnabled && !Record.bPluginFound)
		{
			AddError(TEXT("plugin_state_contradiction"), Record.Role, Record.ObjectPath,
				TEXT("An unavailable plugin cannot be enabled."));
		}
		if (Record.ElementFingerprint != ComputeElementFingerprint(Record))
		{
			AddError(TEXT("element_fingerprint_mismatch"), Record.Role, Record.ObjectPath,
				TEXT("Persisted element identity does not match its detached fields."));
		}
		if (!IsRecordComplete(Record))
		{
			bAllRecordsComplete = false;
			AddError(Record.PackageState == TEXT("unknown")
				? TEXT("asset_registry_unknown_incomplete")
				: TEXT("record_not_ready"), Record.Role, Record.ObjectPath,
				TEXT("The detached record is not complete, clean, loaded, typed, and plugin-ready."));
		}
	}
	if (Snapshot.bFeaturePluginEnabled && !Snapshot.bFeaturePluginFound)
	{
		AddError(TEXT("feature_plugin_state_contradiction"), FString(), FString(),
			TEXT("An unavailable feature plugin cannot be enabled."));
	}
	Report.RecomputedRequestFingerprint = ComputeRequestFingerprint(
		Snapshot.FeaturePluginName, Snapshot.Records);
	Report.RecomputedPersistedFingerprint = ComputePersistedFingerprint(Snapshot);
	Report.RecomputedVolatileObservationFingerprint = ComputeVolatileFingerprint(Snapshot);
	if (Snapshot.RequestFingerprint != Report.RecomputedRequestFingerprint
		|| Snapshot.PersistedFingerprint != Report.RecomputedPersistedFingerprint
		|| Snapshot.VolatileObservationFingerprint
			!= Report.RecomputedVolatileObservationFingerprint)
	{
		AddError(TEXT("snapshot_fingerprint_mismatch"), FString(), FString(),
			TEXT("One or more detached snapshot seals do not match their value fields."));
	}
	const bool bFeatureReady = Snapshot.bFeaturePluginFound
		&& Snapshot.bFeaturePluginEnabled && Snapshot.bFeaturePluginProjectOwned
		&& Snapshot.bFeaturePluginContainsContent
		&& Snapshot.bFeaturePluginExplicitlyLoaded;
	if (Snapshot.bSnapshotComplete != (bFeatureReady && bAllRecordsComplete))
	{
		AddError(TEXT("snapshot_completeness_mismatch"), FString(), FString(),
			TEXT("The completeness claim does not match detached prerequisite evidence."));
	}
	if (EstimateOutputBytes(Snapshot, Report.Issues) > Request.MaxOutputBytes)
	{
		Report.Status = TEXT("output_budget_exceeded");
		Report.Diagnostic = TEXT("Worst-case detached validation output exceeds the caller budget.");
		return Report;
	}
	Report.bOk = true;
	Report.bValid = Report.Issues.IsEmpty() && Snapshot.bSnapshotComplete;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("The exact three-role detached preflight and both revision seals validate independently.")
		: TEXT("The detached composed preflight is incomplete or internally inconsistent.");
	return Report;
}

FString FHyperAIStudioGameplaySystemsContracts::ApplyPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.payload.gameplay-systems.preflight-plan.v1|feature_plugin|mount|base_persisted|targets:exact3|semantic"));
	return Value;
}

FString FHyperAIStudioGameplaySystemsContracts::ComputePayloadSemanticFingerprint(
	const FHyperAIStudioGameplaySystemsPlanPayload& Payload)
{
	using namespace HyperAIStudio::GameplaySystems::Private;
	FString Canonical(TEXT("hyperai.gameplay-systems.plan-semantic.v1|"));
	AppendToken(Canonical, Payload.FeaturePluginName);
	AppendToken(Canonical, Payload.FeatureMountPoint);
	AppendToken(Canonical, Payload.BasePersistedFingerprint);
	AppendToken(Canonical, ClosedOperation);
	for (const FString& Target : Payload.CanonicalTargets) AppendToken(Canonical, Target);
	return HashCanonical(Canonical);
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioGameplaySystemsContracts::GetPreparationDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.gameplay-systems.preparation-only.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both generated gameplay_systems groups are blocking and remain core-owned.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_gameplay_systems_inspect"), InspectVariantId,
			InspectPayloadTypeId, HyperAIStudio::GameplaySystems::Private::InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, HyperAIStudio::GameplaySystems::Private::InspectResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_gameplay_systems_apply_plan"), ApplyVariantId,
			ApplyPayloadTypeId, ApplyPayloadSchemaFingerprint(), ApplyResultTypeId,
			HyperAIStudio::GameplaySystems::Private::ApplyResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_gameplay_systems_validate"), ValidateVariantId,
			ValidatePayloadTypeId, HyperAIStudio::GameplaySystems::Private::ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, HyperAIStudio::GameplaySystems::Private::ValidateResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FString FHyperAIStudioGameplaySystemsPlanPayload::GetTypeId() const
{
	return FHyperAIStudioGameplaySystemsContracts::ApplyPayloadTypeId;
}

FString FHyperAIStudioGameplaySystemsPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGameplaySystemsContracts::ApplyPayloadSchemaFingerprint();
}

int32 FHyperAIStudioGameplaySystemsPlanPayload::GetBoundedByteSize() const
{
	int64 Bytes = 256ll + 2ll * (FeaturePluginName.Len() + FeatureMountPoint.Len()
		+ BasePersistedFingerprint.Len() + SemanticFingerprint.Len());
	for (const FString& Target : CanonicalTargets) Bytes += 32ll + 2ll * Target.Len();
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioGameplaySystemsPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioGameplaySystemsPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioGameplaySystemsPlanPayload, ESPMode::ThreadSafe>();
	Clone->FeaturePluginName = FeaturePluginName;
	Clone->FeatureMountPoint = FeatureMountPoint;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	Clone->CanonicalTargets = CanonicalTargets;
	return Clone;
}

FHyperAIGameplaySystemsInspectReport FHyperAIStudioGameplaySystemsContracts::Inspect(
	const FHyperAIGameplaySystemsInspectRequest& Request)
{
	using namespace HyperAIStudio::GameplaySystems::Private;
	FHyperAIGameplaySystemsInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Gameplay Systems source cohort is fail-closed outside evidence mode until admission.");
		return Report;
	}
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Loaded UObject capture must run once on the Unreal game thread.");
		return Report;
	}
	FString ShapeError;
	if (!ValidateRequestShape(Request.FeaturePluginName, Request.Targets, ShapeError)
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxGameThreadMs < 10 || Request.MaxGameThreadMs > MaxGameThreadMs
		|| Request.MaxOutputBytes < 4096 || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_shape_or_bounds");
		Report.Diagnostic = ShapeError.IsEmpty()
			? TEXT("Inspect budget is outside the closed envelope.") : ShapeError;
		return Report;
	}
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!Capture(Request, Report.Snapshot, Report.Issues,
		CaptureStatus, CaptureDiagnostic))
	{
		Report.Status = CaptureStatus.IsEmpty() ? TEXT("capture_failed") : CaptureStatus;
		Report.Diagnostic = CaptureDiagnostic.IsEmpty()
			? TEXT("The bounded loaded-only preflight could not complete.") : CaptureDiagnostic;
		return Report;
	}
	if (EstimateOutputBytes(Report.Snapshot, Report.Issues) > Request.MaxOutputBytes)
	{
		Report.Status = TEXT("output_budget_exceeded");
		Report.Diagnostic = TEXT("Worst-case inspect output exceeds the caller budget.");
		Report.Snapshot = {};
		Report.Issues.Reset();
		return Report;
	}
	Report.bOk = true;
	Report.Status = Report.Snapshot.bSnapshotComplete
		? TEXT("complete") : TEXT("incomplete");
	Report.Diagnostic = Report.Snapshot.bSnapshotComplete
		? TEXT("The project-owned Game Feature, Mass config, and World Condition schema passed one exact loaded-only preflight.")
		: TEXT("The preflight is bounded and truthful but one or more exact prerequisites are incomplete.");
	return Report;
}

FHyperAIGameplaySystemsApplyPlanReport
FHyperAIStudioGameplaySystemsContracts::BuildPlan(
	const FHyperAIGameplaySystemsApplyPlanRequest& Request)
{
	FHyperAIGameplaySystemsApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Gameplay Systems source cohort is fail-closed outside evidence mode until admission.");
		return Report;
	}
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Plan preparation requires one fresh loaded-only game-thread capture.");
		return Report;
	}
	FString ShapeError;
	if (!HyperAIStudio::GameplaySystems::Private::ValidateRequestShape(
		Request.FeaturePluginName, Request.Targets, ShapeError))
	{
		Report.Status = TEXT("invalid_plan_shape_or_bounds");
		Report.Diagnostic = ShapeError;
		return Report;
	}
	FHyperAIGameplaySystemsInspectRequest InspectRequest;
	InspectRequest.FeaturePluginName = Request.FeaturePluginName;
	InspectRequest.Targets = Request.Targets;
	InspectRequest.DeadlineMs = Request.DeadlineMs;
	InspectRequest.MaxGameThreadMs = Request.MaxGameThreadMs;
	InspectRequest.MaxOutputBytes = Request.MaxOutputBytes;
	FHyperAIGameplaySystemsDetachedSnapshot Snapshot;
	TArray<FHyperAIGameplaySystemsIssue> Issues;
	FString Status;
	FString Diagnostic;
	if (!HyperAIStudio::GameplaySystems::Private::Capture(
		InspectRequest, Snapshot, Issues, Status, Diagnostic))
	{
		Report.Status = Status.IsEmpty() ? TEXT("capture_failed") : Status;
		Report.Diagnostic = Diagnostic;
		Report.Issues = MoveTemp(Issues);
		return Report;
	}
	return HyperAIStudio::GameplaySystems::Private::PrepareFromSnapshot(Request, Snapshot);
}

FHyperAIGameplaySystemsApplyPlanReport
FHyperAIStudioGameplaySystemsContracts::PrepareFromSnapshotForTest(
	const FHyperAIGameplaySystemsApplyPlanRequest& Request,
	const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
{
	return HyperAIStudio::GameplaySystems::Private::PrepareFromSnapshot(Request, Snapshot);
}

FHyperAIGameplaySystemsInspectReport
UHyperAIStudioGameplaySystemsToolset::hyper_gameplay_systems_inspect(
	const FHyperAIGameplaySystemsInspectRequest& Request)
{
	return FHyperAIStudioGameplaySystemsContracts::Inspect(Request);
}

FHyperAIGameplaySystemsApplyPlanReport
UHyperAIStudioGameplaySystemsToolset::hyper_gameplay_systems_apply_plan(
	const FHyperAIGameplaySystemsApplyPlanRequest& Request)
{
	return FHyperAIStudioGameplaySystemsContracts::BuildPlan(Request);
}

FHyperAIGameplaySystemsValidateReport
UHyperAIStudioGameplaySystemsToolset::hyper_gameplay_systems_validate(
	const FHyperAIGameplaySystemsValidateRequest& Request)
{
	return FHyperAIStudioGameplaySystemsContracts::ValidateDetached(Request);
}

void FHyperAIStudioGameplaySystemsRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioGameplaySystemsRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioGameplaySystemsRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioGameplaySystemsRegistration::IsRegistered() const
{
	return FHyperAIStudioGameplaySystemsContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGameplaySystemsToolset::StaticClass(),
			FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioGameplaySystemsRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()) return;
	if (!FHyperAIStudioGameplaySystemsContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())) return;
	if (!IAssetRegistry::Get() || !UGameFeatureData::StaticClass()
		|| !UMassEntityConfigAsset::StaticClass()
		|| !UWorldConditionSchema::StaticClass()) return;
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioGameplaySystemsToolset::StaticClass(),
		FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioGameplaySystems, Error,
			TEXT("Gameplay Systems atomic registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioGameplaySystemsRegistration::RollBackRegistration()
{
	if (!bOwnsToolset || !IsInGameThread() || !UObjectInitialized()) return;
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
		UHyperAIStudioGameplaySystemsToolset::StaticClass(),
		FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioGameplaySystems, Error,
			TEXT("Gameplay Systems owned rollback failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = false;
}
