// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioGameplaySystemsToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::GameplaySystems::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIGameplaySystemsDetachedSnapshot ValidSnapshot()
	{
		FHyperAIGameplaySystemsDetachedSnapshot Snapshot;
		Snapshot.FeaturePluginName = TEXT("FeatureAI");
		Snapshot.FeatureMountPoint = TEXT("/FeatureAI/");
		Snapshot.bFeaturePluginFound = true;
		Snapshot.bFeaturePluginEnabled = true;
		Snapshot.bFeaturePluginProjectOwned = true;
		Snapshot.bFeaturePluginContainsContent = true;
		Snapshot.bFeaturePluginExplicitlyLoaded = true;

		auto Add = [&](const TCHAR* Role, const TCHAR* Path, const TCHAR* ClassPath,
			const TCHAR* Plugin, const TCHAR* PackageState, const int32 Count)
		{
			FHyperAIGameplaySystemsRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
			Record.Role = Role;
			Record.ObjectPath = Path;
			Record.ExpectedClassPath = ClassPath;
			Record.ActualClassPath = ClassPath;
			Record.PluginName = Plugin;
			Record.PackageState = PackageState;
			Record.ContentFingerprint = Hash(FString(Role) + TEXT("-content"));
			Record.EntryCount = Count;
			Record.bLoaded = true;
			Record.bWasLoaded = FCString::Strcmp(Role, TEXT("world_condition_schema")) != 0;
			Record.bDirty = false;
			Record.bClassCompatible = true;
			Record.bPluginFound = true;
			Record.bPluginEnabled = true;
			Record.ElementFingerprint =
				FHyperAIStudioGameplaySystemsContracts::ComputeElementFingerprint(Record);
		};
		Add(TEXT("game_feature_data"),
			TEXT("/FeatureAI/GameFeatureData.GameFeatureData"),
			FHyperAIStudioGameplaySystemsContracts::GameFeatureDataClassPath,
			TEXT("GameFeatures"), TEXT("exists"), 2);
		Add(TEXT("mass_entity_config"), TEXT("/Game/AI/MassConfig.MassConfig"),
			FHyperAIStudioGameplaySystemsContracts::MassEntityConfigClassPath,
			TEXT("MassAI"), TEXT("exists"), 3);
		Add(TEXT("world_condition_schema"),
			TEXT("/Script/WorldConditions.WorldConditionSchema"),
			FHyperAIStudioGameplaySystemsContracts::WorldConditionSchemaClassPath,
			TEXT("WorldConditions"), TEXT("not_applicable"), 1);
		Snapshot.RequestFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputeRequestFingerprint(
				Snapshot.FeaturePluginName, Snapshot.Records);
		Snapshot.PersistedFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputePersistedFingerprint(Snapshot);
		Snapshot.VolatileObservationFingerprint =
			FHyperAIStudioGameplaySystemsContracts::ComputeVolatileFingerprint(Snapshot);
		Snapshot.bSnapshotComplete = true;
		return Snapshot;
	}

	FHyperAIGameplaySystemsApplyPlanRequest PlanRequest(
		const FHyperAIGameplaySystemsDetachedSnapshot& Snapshot)
	{
		FHyperAIGameplaySystemsApplyPlanRequest Request;
		Request.FeaturePluginName = Snapshot.FeaturePluginName;
		Request.ExpectedPersistedFingerprint = Snapshot.PersistedFingerprint;
		for (const FHyperAIGameplaySystemsRecord& Record : Snapshot.Records)
		{
			FHyperAIGameplaySystemsTarget& Target = Request.Targets.AddDefaulted_GetRef();
			Target.Role = Record.Role;
			Target.ObjectPath = Record.ObjectPath;
		}
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplaySystemsManifestTest,
	"HyperAIStudio.NativeTools.GameplaySystems.ExactManifestAndDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplaySystemsManifestTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIStudioGameplaySystemsManifestEntry>& Manifest =
		FHyperAIStudioGameplaySystemsContracts::GetManifest();
	TestEqual(TEXT("Exactly three tools"), Manifest.Num(), 3);
	TestEqual(TEXT("Exactly nine Epic delegates remain delegates"),
		FHyperAIStudioGameplaySystemsContracts::GetEpicDelegates().Num(), 9);
	TSet<FString> Names;
	for (const FHyperAIStudioGameplaySystemsManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		TestEqual(TEXT("Qualified owner"), Entry.QualifiedToolset,
			FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName());
	}
	TestEqual(TEXT("Manifest names unique"), Names.Num(), 3);
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioGameplaySystemsContracts::GetPreparationDescriptor();
	TestEqual(TEXT("Descriptor pack"), Descriptor.PackId,
		FString(FHyperAIStudioGameplaySystemsContracts::PackId));
	TestEqual(TEXT("Exactly three typed variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("Blocking groups are not self-declared nonblocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("Request namespace"),
			Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("Result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		TestNotEqual(TEXT("Namespaces disjoint"), Variant.RequestTypeId, Variant.ResultTypeId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplaySystemsDetachedTest,
	"HyperAIStudio.NativeTools.GameplaySystems.DetachedValidationAndSeals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplaySystemsDetachedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GameplaySystems::Tests;
	FHyperAIGameplaySystemsDetachedSnapshot Snapshot = ValidSnapshot();
	FHyperAIGameplaySystemsValidateRequest Request;
	Request.Snapshot = Snapshot;
	const FHyperAIGameplaySystemsValidateReport Valid =
		FHyperAIStudioGameplaySystemsContracts::ValidateDetached(Request);
	TestTrue(TEXT("Complete detached snapshot validates"), Valid.bValid);
	const FString Persisted = Snapshot.PersistedFingerprint;
	const FString Volatile = Snapshot.VolatileObservationFingerprint;
	Snapshot.Records[0].bDirty = true;
	Snapshot.VolatileObservationFingerprint =
		FHyperAIStudioGameplaySystemsContracts::ComputeVolatileFingerprint(Snapshot);
	TestEqual(TEXT("Dirty state excluded from persisted seal"),
		FHyperAIStudioGameplaySystemsContracts::ComputePersistedFingerprint(Snapshot), Persisted);
	TestNotEqual(TEXT("Dirty state changes volatile seal"),
		Snapshot.VolatileObservationFingerprint, Volatile);
	Snapshot.bSnapshotComplete = false;
	Request.Snapshot = Snapshot;
	const FHyperAIGameplaySystemsValidateReport Dirty =
		FHyperAIStudioGameplaySystemsContracts::ValidateDetached(Request);
	TestFalse(TEXT("Dirty base fails readiness validation"), Dirty.bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplaySystemsUnknownTest,
	"HyperAIStudio.NativeTools.GameplaySystems.AssetRegistryUnknownFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplaySystemsUnknownTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GameplaySystems::Tests;
	FHyperAIGameplaySystemsDetachedSnapshot Snapshot = ValidSnapshot();
	Snapshot.Records[1].PackageState = TEXT("unknown");
	Snapshot.VolatileObservationFingerprint =
		FHyperAIStudioGameplaySystemsContracts::ComputeVolatileFingerprint(Snapshot);
	Snapshot.bSnapshotComplete = false;
	FHyperAIGameplaySystemsValidateRequest Request;
	Request.Snapshot = Snapshot;
	const FHyperAIGameplaySystemsValidateReport Report =
		FHyperAIStudioGameplaySystemsContracts::ValidateDetached(Request);
	TestFalse(TEXT("Unknown is never ready"), Report.bValid);
	TestTrue(TEXT("Unknown has explicit issue"), Report.Issues.ContainsByPredicate(
		[](const FHyperAIGameplaySystemsIssue& Issue)
		{
			return Issue.Code == TEXT("asset_registry_unknown_incomplete");
		}));
	TestEqual(TEXT("Unknown enum maps to unknown"),
		FHyperAIStudioGameplaySystemsContracts::ClassifyPackageExistence(
			static_cast<int32>(UE::AssetRegistry::EExists::Unknown)), FString(TEXT("unknown")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplaySystemsPrepareTest,
	"HyperAIStudio.NativeTools.GameplaySystems.PurePrepareAndZeroEffectNonDry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplaySystemsPrepareTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GameplaySystems::Tests;
	const FHyperAIGameplaySystemsDetachedSnapshot Snapshot = ValidSnapshot();
	FHyperAIGameplaySystemsApplyPlanRequest Request = PlanRequest(Snapshot);
	const FHyperAIGameplaySystemsApplyPlanReport Dry =
		FHyperAIStudioGameplaySystemsContracts::PrepareFromSnapshotForTest(Request, Snapshot);
	TestTrue(TEXT("Pure dry-run prepares"), Dry.bOk && Dry.bTypedPrepared);
	TestTrue(TEXT("Pure dry-run seals plan"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalSha256(Dry.PlanHash));
	TestFalse(TEXT("Dry-run never stages"), Dry.bStaged);
	TestFalse(TEXT("Dry-run never submits"), Dry.bExecutionSubmitted);
	TestFalse(TEXT("Dry-run never mutates a project asset"),
		Dry.Effects.bProjectAssetMutationAttempted);

	Request.bDryRun = false;
	Request.OperationId = TEXT("gameplay-systems-zero-effect-0001");
	Request.ExpectedPlanHash = Dry.PlanHash;
	const FHyperAIGameplaySystemsApplyPlanReport NonDry =
		FHyperAIStudioGameplaySystemsContracts::PrepareFromSnapshotForTest(Request, Snapshot);
	TestFalse(TEXT("Non-dry remains blocked"), NonDry.bOk);
	TestEqual(TEXT("Truthful blocker"), NonDry.Status,
		FString(FHyperAIStudioGameplaySystemsContracts::NonDryCallableState));
	TestFalse(TEXT("Non-dry never stages"), NonDry.bStaged);
	TestFalse(TEXT("Non-dry never submits"), NonDry.bExecutionSubmitted);
	TestFalse(TEXT("Non-dry performs zero project effects"),
		NonDry.Effects.bProjectAssetMutationAttempted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplaySystemsPathBoundsTest,
	"HyperAIStudio.NativeTools.GameplaySystems.ClosedPathsAndBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplaySystemsPathBoundsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Feature asset in exact feature mount"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
			TEXT("game_feature_data"),
			TEXT("/FeatureAI/GameFeatureData.GameFeatureData"), TEXT("FeatureAI")));
	TestTrue(TEXT("Mass config in project content"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
			TEXT("mass_entity_config"), TEXT("/Game/AI/MassConfig.MassConfig"),
			TEXT("FeatureAI")));
	TestTrue(TEXT("Schema class uses script path"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
			TEXT("world_condition_schema"),
			TEXT("/Script/WorldConditions.WorldConditionSchema"), TEXT("FeatureAI")));
	TestFalse(TEXT("Feature path cannot escape its mount"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
			TEXT("game_feature_data"), TEXT("/Game/Wrong.Wrong"), TEXT("FeatureAI")));
	TestFalse(TEXT("Subobject rejected"),
		FHyperAIStudioGameplaySystemsContracts::IsCanonicalTargetPath(
			TEXT("mass_entity_config"), TEXT("/Game/AI/Mass.Mass:Inner"),
			TEXT("FeatureAI")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
