// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioWorldbuildingToolset.h"

#include "Components/SplineComponent.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Worldbuilding::Tests
{
	FString Sha(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FHyperAIWorldPlanOperation SplineCreate(const FString& Target)
	{
		FHyperAIWorldPlanOperation Operation;
		Operation.Variant = TEXT("spline.create");
		Operation.TargetPath = Target;
		Operation.Points = {FVector(0, 0, 0), FVector(100, 0, 0)};
		return Operation;
	}

	FHyperAIWorldApplyPlanRequest SplineCreateRequest(const FString& Target)
	{
		FHyperAIWorldApplyPlanRequest Request;
		Request.bDryRun = true;
		Request.Operations.Add(SplineCreate(Target));
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIWorldbuildingManifestTest,
	"HyperAIStudio.NativeTools.WorldbuildingNavigation.ManifestAndAtomicOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIWorldbuildingManifestTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIWorldManifestEntry>& WorldManifest =
		FHyperAIStudioWorldbuildingContracts::GetManifest();
	const TArray<FHyperAIWorldManifestEntry>& NavigationManifest =
		FHyperAIStudioNavigationContracts::GetManifest();
	TestEqual(TEXT("worldbuilding resolver binds exact joint cohort"), WorldManifest.Num(), 6);
	TestEqual(TEXT("navigation resolver binds exact joint cohort"), NavigationManifest.Num(), 6);
	TestEqual(TEXT("single exact pack id"), FString(FHyperAIStudioWorldbuildingContracts::PackId),
		FString(FHyperAIStudioNavigationContracts::PackId));
	TestEqual(TEXT("single exact cohort id"), FString(FHyperAIStudioWorldbuildingContracts::AtomicCohortId),
		FString(FHyperAIStudioNavigationContracts::AtomicCohortId));
	TestEqual(TEXT("pack id is catalog-owned combined pack"),
		FString(FHyperAIStudioWorldbuildingContracts::PackId), FString(TEXT("worldbuilding_navigation")));

	TSet<FString> ManifestNames;
	for (const FHyperAIWorldManifestEntry& Entry : WorldManifest) ManifestNames.Add(Entry.Name);
	TSet<FString> ReflectedNames;
	for (TFieldIterator<UFunction> It(UHyperAIStudioWorldbuildingToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) ReflectedNames.Add(It->GetName());
	}
	for (TFieldIterator<UFunction> It(UHyperAIStudioNavigationToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) ReflectedNames.Add(It->GetName());
	}
	TestEqual(TEXT("exactly six reflected AICallables"), ReflectedNames.Num(), 6);
	TestEqual(TEXT("manifest/reflection exact union"), ManifestNames.Num(), ReflectedNames.Num());
	for (const FString& Name : ManifestNames)
	{
		const FString Label = TEXT("reflected: ") + Name;
		TestTrue(*Label, ReflectedNames.Contains(Name));
	}

	TArray<FString> ExactNames = ManifestNames.Array();
	ExactNames.Sort();
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("generated catalog is sole exact cohort authority"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			FHyperAIStudioWorldbuildingContracts::PackId,
			FHyperAIStudioWorldbuildingContracts::AtomicCohortId,
			ExactNames, Admission));
	TestTrue(TEXT("catalog valid"), Admission.bCatalogValid);
	TestTrue(TEXT("all and only six tools atomic"), Admission.bExactCohortMatch);
	TestFalse(TEXT("generated pack tier remains core"), Admission.bOptionalPack);
	TestEqual(TEXT("implementation remains source candidate"), Admission.State,
		EHyperAIStudioExtensionAdmissionState::SourceCandidate);
	TestFalse(TEXT("never production-register source candidate"),
		FHyperAIStudioWorldbuildingContracts::IsRegistrationAllowed(false));
	TestEqual(TEXT("both resolvers have identical dev admission"),
		FHyperAIStudioWorldbuildingContracts::IsRegistrationAllowed(true),
		FHyperAIStudioNavigationContracts::IsRegistrationAllowed(true));

	const bool bWorldOwned = FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioWorldbuildingToolset::StaticClass(),
		FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName());
	const bool bNavigationOwned = FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioNavigationToolset::StaticClass(),
		FHyperAIStudioNavigationContracts::GetQualifiedToolsetName());
	TestEqual(TEXT("registration ownership is atomically all-or-none"), bWorldOwned, bNavigationOwned);
	TestNull(TEXT("world apply has no client bearer token"),
		FHyperAIWorldApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("navigation apply has no client bearer token"),
		FHyperAINavigationApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("world operation has no script"),
		FHyperAIWorldPlanOperation::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("world operation has no raw file path"),
		FHyperAIWorldPlanOperation::StaticStruct()->FindPropertyByName(TEXT("FilePath")));
	TestNull(TEXT("navigation operation has no console command"),
		FHyperAINavigationPlanOperation::StaticStruct()->FindPropertyByName(TEXT("ConsoleCommand")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIWorldbuildingOperationSchemaTest,
	"HyperAIStudio.NativeTools.WorldbuildingNavigation.ClosedOperationSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIWorldbuildingOperationSchemaTest::RunTest(const FString& Parameters)
{
	FString Error;
	FHyperAIWorldBackendOperation WorldBackend;
	FHyperAIWorldPlanOperation Landscape;
	Landscape.Variant = TEXT("landscape.create");
	Landscape.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.Landscape");
	Landscape.Extent = FVector(1000, 1000, 100);
	Landscape.Count = 64;
	TestTrue(TEXT("typed landscape creation accepted"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(Landscape, WorldBackend, Error));
	TestEqual(TEXT("landscape create is edit"), WorldBackend.Safety, EHyperAIWorldSafety::Edit);

	FHyperAIWorldPlanOperation HeightImport;
	HeightImport.Variant = TEXT("landscape.import_height_asset");
	HeightImport.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.Landscape");
	HeightImport.ExpectedRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('a'));
	HeightImport.SourceAssetPath = TEXT("C:\\temp\\height.png");
	TestFalse(TEXT("raw heightmap file rejected"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(HeightImport, WorldBackend, Error));
	HeightImport.SourceAssetPath = TEXT("/Game/Height/T_Height.T_Height");
	TestTrue(TEXT("project-contained height asset accepted"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(HeightImport, WorldBackend, Error));

	FHyperAIWorldPlanOperation ActorCrud;
	ActorCrud.Variant = TEXT("actor.create");
	ActorCrud.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.Actor");
	TestFalse(TEXT("ordinary actor CRUD delegates to Epic"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(ActorCrud, WorldBackend, Error));

	FHyperAIWorldPlanOperation Erase;
	Erase.Variant = TEXT("foliage.erase_instances");
	Erase.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.Foliage");
	Erase.ExpectedRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('b'));
	Erase.Radius = 500;
	Erase.Count = 10;
	TestTrue(TEXT("bounded foliage erase schema accepted"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(Erase, WorldBackend, Error));
	TestEqual(TEXT("foliage erase separately destructive"), WorldBackend.Safety,
		EHyperAIWorldSafety::Destructive);

	FHyperAIWorldPlanOperation Partition;
	Partition.Variant = TEXT("world_partition.load_region");
	Partition.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.WorldPartition");
	Partition.ExpectedRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('c'));
	Partition.Extent = FVector(1000, 1000, 500);
	TestTrue(TEXT("bounded partition region schema accepted"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(Partition, WorldBackend, Error));
	TestEqual(TEXT("region load separately external-effect"), WorldBackend.Safety,
		EHyperAIWorldSafety::ExternalEffect);

	FHyperAIWorldPlanOperation Optimize;
	Optimize.Variant = TEXT("optimize.proxy");
	Optimize.TargetPath = TEXT("/Game/Optimized/SM_Proxy.SM_Proxy");
	Optimize.SourcePaths = {
		TEXT("/Game/Maps/Test.Test:PersistentLevel.SourceA"),
		TEXT("/Game/Maps/Test.Test:PersistentLevel.SourceB")};
	TestTrue(TEXT("actor proxy optimization has typed source set"),
		FHyperAIStudioWorldbuildingContracts::ValidateOperationShape(Optimize, WorldBackend, Error));
	TestEqual(TEXT("replacement optimization separately destructive"), WorldBackend.Safety,
		EHyperAIWorldSafety::Destructive);

	FHyperAINavigationBackendOperation NavigationBackend;
	FHyperAINavigationPlanOperation Link;
	Link.Variant = TEXT("nav_link.create");
	Link.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.NavLink");
	Link.LinkStart = FVector(0, 0, 0);
	Link.LinkEnd = FVector(100, 0, 0);
	TestTrue(TEXT("typed nav-link creation accepted"),
		FHyperAIStudioNavigationContracts::ValidateOperationShape(Link, NavigationBackend, Error));
	TestEqual(TEXT("nav-link create is edit"), NavigationBackend.Safety, EHyperAIWorldSafety::Edit);
	FHyperAINavigationPlanOperation Build;
	Build.Variant = TEXT("navmesh.build");
	Build.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.RecastNavMesh");
	Build.ExpectedRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('d'));
	TestTrue(TEXT("typed navmesh build accepted"),
		FHyperAIStudioNavigationContracts::ValidateOperationShape(Build, NavigationBackend, Error));
	TestEqual(TEXT("navmesh build separately external-effect"), NavigationBackend.Safety,
		EHyperAIWorldSafety::ExternalEffect);
	FHyperAINavigationPlanOperation Remove = Link;
	Remove.Variant = TEXT("nav_link.remove");
	Remove.ExpectedRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('e'));
	TestTrue(TEXT("typed nav-link removal accepted"),
		FHyperAIStudioNavigationContracts::ValidateOperationShape(Remove, NavigationBackend, Error));
	TestEqual(TEXT("nav-link removal separately destructive"), NavigationBackend.Safety,
		EHyperAIWorldSafety::Destructive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIWorldbuildingPayloadCloneTest,
	"HyperAIStudio.NativeTools.WorldbuildingNavigation.ImmutablePayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIWorldbuildingPayloadCloneTest::RunTest(const FString& Parameters)
{
	FHyperAIWorldTypedArtifactPayload Source;
	Source.TypeId = TEXT("hyperai.payload.worldbuilding.plan.v1");
	Source.SchemaFingerprint = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('a'));
	Source.PackId = TEXT("worldbuilding_navigation");
	Source.SafetyClass = TEXT("edit");
	Source.BaseRevision = HyperAIStudio::Worldbuilding::Tests::Sha(TEXT('b'));
	Source.CanonicalOperations = {TEXT("operation-one"), TEXT("operation-two")};
	const FString Original = Source.GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Source.CloneImmutable();
	TestTrue(TEXT("clone is a detached object"),
		&Clone.Get() != static_cast<const IHyperAIStudioTypedArtifactPayload*>(&Source));
	TestEqual(TEXT("clone initially exact"), Clone->GetSemanticFingerprint(), Original);
	Source.CanonicalOperations[0] = TEXT("mutated-after-clone");
	Source.CanonicalOperations.Add(TEXT("new-operation"));
	TestNotEqual(TEXT("source semantic identity changes"), Source.GetSemanticFingerprint(), Original);
	TestEqual(TEXT("deep clone retains immutable array values"),
		Clone->GetSemanticFingerprint(), Original);
	const FHyperAIWorldTypedArtifactPayload& Concrete =
		static_cast<const FHyperAIWorldTypedArtifactPayload&>(Clone.Get());
	TestEqual(TEXT("first canonical operation deeply detached"),
		Concrete.CanonicalOperations[0], FString(TEXT("operation-one")));
	TestEqual(TEXT("bounded size remains finite"), Concrete.GetBoundedByteSize() > 0, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIWorldbuildingPlanTest,
	"HyperAIStudio.NativeTools.WorldbuildingNavigation.PlanCASAndFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIWorldbuildingPlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Worldbuilding::Tests;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString CreateTarget = TEXT("/Game/__HyperAIStudioAutomation/Test.Test:PersistentLevel.Spline_") + Suffix;
	FHyperAIWorldApplyPlanRequest Request = SplineCreateRequest(CreateTarget);
	const FHyperAIWorldApplyPlanReport First =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(Request);
	const FHyperAIWorldApplyPlanReport Second =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(Request);
		TestFalse(TEXT("subobject create without container CAS fails closed"), First.bOk);
		TestEqual(TEXT("dry-run status"), First.Status, FString(TEXT("cas_failed")));
		TestFalse(TEXT("dry-run does not submit"), First.bExecutionSubmitted);
		TestTrue(TEXT("failed create emits no plan hash"), First.PlanHash.IsEmpty());
		TestEqual(TEXT("same closed plan hash deterministic"), First.PlanHash, Second.PlanHash);
		TestEqual(TEXT("same effect fingerprint deterministic"),
			First.EffectFingerprint, Second.EffectFingerprint);
		TestTrue(TEXT("failed create emits no prepared contract"),
			First.PreparedContractFingerprint.IsEmpty());

	Request.bDryRun = false;
	Request.OperationId = TEXT("worldbuilding-test-operation-001");
	Request.ExpectedPlanHash = First.PlanHash;
	const FHyperAIWorldApplyPlanReport NonDry =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(Request);
	TestFalse(TEXT("source candidate never reports mutation success"), NonDry.bOk);
	TestFalse(TEXT("source candidate never submits execution"), NonDry.bExecutionSubmitted);
		TestEqual(TEXT("container CAS fail-closed status"),
			NonDry.Status, FString(TEXT("cas_failed")));

		Request.ExpectedPlanHash = Sha(TEXT('f'));
		const FHyperAIWorldApplyPlanReport WrongHash =
			UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(Request);
		TestEqual(TEXT("CAS rejection precedes any echoed plan-hash check"), WrongHash.Status,
			FString(TEXT("cas_failed")));

	FHyperAIWorldApplyPlanRequest Mixed = SplineCreateRequest(
		TEXT("/Game/__HyperAIStudioAutomation/Test.Test:PersistentLevel.Mixed_") + Suffix);
	FHyperAIWorldPlanOperation External;
	External.Variant = TEXT("world_partition.load_region");
	External.TargetPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.WorldPartition");
	External.ExpectedRevision = Sha(TEXT('a'));
	External.Extent = FVector(100, 100, 100);
	Mixed.Operations.Add(External);
	const FHyperAIWorldApplyPlanReport MixedReport =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(Mixed);
	TestEqual(TEXT("mixed safety cohorts rejected before target dispatch"),
		MixedReport.Status, FString(TEXT("mixed_safety_cohort")));

	const FString PackageName = TEXT("/Game/__HyperAIStudioAutomation/SplineCAS_") + Suffix;
	UPackage* Package = CreatePackage(*PackageName);
	if (Package)
	{
		FHyperAIWorldApplyPlanRequest ExistingContainerCreate = SplineCreateRequest(
			PackageName + TEXT(".SplineCAS:PersistentLevel.NewSpline"));
		const FHyperAIWorldApplyPlanReport ExistingContainerReport =
			UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(
				ExistingContainerCreate);
		TestEqual(TEXT("create in a loaded container requires separate container CAS"),
			ExistingContainerReport.Status, FString(TEXT("cas_failed")));
	}
	USplineComponent* Spline = Package
		? NewObject<USplineComponent>(Package, FName(TEXT("SplineFixture")), RF_Transient) : nullptr;
	TestNotNull(TEXT("loaded spline CAS fixture"), Spline);
	if (Spline)
	{
		Spline->ClearSplinePoints(false);
		Spline->AddSplinePoint(FVector(0, 0, 0), ESplineCoordinateSpace::Local, false);
		Spline->AddSplinePoint(FVector(100, 0, 0), ESplineCoordinateSpace::Local, false);
		const FString Revision = FHyperAIStudioWorldbuildingContracts::ComputeLoadedTargetRevision(
			Spline->GetPathName());
		TestTrue(TEXT("loaded spline revision canonical"), Revision.StartsWith(TEXT("sha256:")));
		FHyperAIWorldApplyPlanRequest CasRequest;
		FHyperAIWorldPlanOperation SetClosed;
		SetClosed.Variant = TEXT("spline.set_closed");
		SetClosed.TargetPath = Spline->GetPathName();
		SetClosed.ExpectedRevision = Revision;
		SetClosed.bSetEnabled = true;
		SetClosed.bEnabled = true;
		CasRequest.Operations.Add(SetClosed);
		const FHyperAIWorldApplyPlanReport CasOk =
			UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(CasRequest);
		TestEqual(TEXT("inspection digest is never accepted as mutation CAS"),
			CasOk.Status, FString(TEXT("cas_failed")));
		TestTrue(TEXT("missing persisted-state backend is disclosed"),
			CasOk.Issues.ContainsByPredicate([](const FHyperAIWorldIssue& Issue)
			{
				return Issue.Code == TEXT("cas_failed")
					&& Issue.Message.Contains(TEXT("persisted-state revision backend"));
			}));
		CasRequest.Operations[0].ExpectedRevision = Sha(TEXT('0'));
		const FHyperAIWorldApplyPlanReport CasBad =
			UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_apply_plan(CasRequest);
		TestEqual(TEXT("stale loaded CAS rejected"), CasBad.Status, FString(TEXT("cas_failed")));
		Spline->MarkAsGarbage();
	}
	if (Package) Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIWorldbuildingSnapshotTest,
	"HyperAIStudio.NativeTools.WorldbuildingNavigation.SnapshotPagingValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIWorldbuildingSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Worldbuilding::Tests;
	FHyperAIWorldValueSnapshot Snapshot;
	Snapshot.Scope = TEXT("loaded_only");
	Snapshot.bComplete = true;
	FHyperAIWorldRecord Spline;
	Spline.Variant = TEXT("spline");
	Spline.StableId = TEXT("spline:/Game/Test.Test:Spline");
	Spline.ObjectPath = TEXT("/Game/Test.Test:Spline");
	Spline.ClassPath = TEXT("/Script/Engine.SplineComponent");
	Spline.Revision = Sha(TEXT('a'));
	Spline.PrimaryCount = 1;
	Spline.bLoaded = true;
	Snapshot.Records.Add(Spline);
	const FString FirstRevision =
		FHyperAIStudioWorldbuildingContracts::ComputeSnapshotRevision(Snapshot);
	const FString SecondRevision =
		FHyperAIStudioWorldbuildingContracts::ComputeSnapshotRevision(Snapshot);
	TestEqual(TEXT("snapshot hash deterministic"), FirstRevision, SecondRevision);
	bool bTruncated = false;
	const TArray<FHyperAIWorldIssue> Issues =
		FHyperAIStudioWorldbuildingContracts::ValidateSnapshot(Snapshot, 16, bTruncated);
	TestTrue(TEXT("invalid one-point spline independently detected"), Issues.ContainsByPredicate(
		[](const FHyperAIWorldIssue& Issue)
		{
			return Issue.Code == TEXT("spline_has_too_few_points") && Issue.Severity == TEXT("error");
		}));

	FHyperAINavigationValueSnapshot Navigation;
	Navigation.Scope = TEXT("loaded_only");
	Navigation.bComplete = true;
	FHyperAINavigationRecord NavMesh;
	NavMesh.Variant = TEXT("navmesh");
	NavMesh.StableId = TEXT("navmesh:/Game/Test.Test:NavMesh");
	NavMesh.ObjectPath = TEXT("/Game/Test.Test:NavMesh");
	NavMesh.ClassPath = TEXT("/Script/NavigationSystem.RecastNavMesh");
	NavMesh.Revision = Sha(TEXT('b'));
	NavMesh.bLoaded = true;
	Navigation.Records.Add(NavMesh);
	FHyperAINavigationRecord NavSystem;
	NavSystem.Variant = TEXT("nav_system");
	NavSystem.StableId = TEXT("nav_system:/Game/Test.Test:NavigationSystem");
	NavSystem.ObjectPath = TEXT("/Game/Test.Test:NavigationSystem");
	NavSystem.ClassPath = TEXT("/Script/NavigationSystem.NavigationSystemV1");
	NavSystem.Revision = Sha(TEXT('c'));
	NavSystem.bLoaded = true;
	Navigation.Records.Add(NavSystem);
	FHyperAIStudioNavigationContracts::ComputeSnapshotRevision(Navigation);
	const TArray<FHyperAIWorldIssue> NavigationIssues =
		FHyperAIStudioNavigationContracts::ValidateSnapshot(Navigation, 16, bTruncated);
	TestTrue(TEXT("zero-active-tile warning independently detected"), NavigationIssues.ContainsByPredicate(
		[](const FHyperAIWorldIssue& Issue)
		{
			return Issue.Code == TEXT("navmesh_has_no_active_tiles")
				&& Issue.Severity == TEXT("warning");
		}));

	FHyperAIWorldInspectRequest InvalidProjection;
	InvalidProjection.Projection = {TEXT("identity"), TEXT("identity")};
	const FHyperAIWorldInspectReport ProjectionReport =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_inspect(InvalidProjection);
	TestFalse(TEXT("duplicate projection rejected"), ProjectionReport.bOk);
	FHyperAIWorldInspectRequest InvalidCursor;
	InvalidCursor.Cursor = TEXT("v1|sha256:bad|0");
	const FHyperAIWorldInspectReport CursorReport =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_inspect(InvalidCursor);
	TestFalse(TEXT("stale/malformed cursor rejected"), CursorReport.bOk);
	TestEqual(TEXT("cursor status stable"), CursorReport.Status,
		FString(TEXT("stale_or_invalid_cursor")));

	FHyperAIWorldInspectRequest OnDiskWorld;
	OnDiskWorld.Scope = TEXT("on_disk_index");
	const FHyperAIWorldInspectReport OnDiskWorldReport =
		UHyperAIStudioWorldbuildingToolset::hyper_worldbuilding_inspect(OnDiskWorld);
	TestTrue(TEXT("on-disk world request returns bounded disclosure"), OnDiskWorldReport.bOk);
	TestFalse(TEXT("synchronous on-disk world inventory is never certified complete"),
		OnDiskWorldReport.bRevisionComplete);
	TestTrue(TEXT("world inventory requires async cache"),
		OnDiskWorldReport.Issues.ContainsByPredicate([](const FHyperAIWorldIssue& Issue)
		{
			return Issue.Code == TEXT("async_on_disk_index_required");
		}));

	FHyperAINavigationInspectRequest OnDiskNavigation;
	OnDiskNavigation.Scope = TEXT("on_disk_index");
	const FHyperAINavigationInspectReport OnDiskNavigationReport =
		UHyperAIStudioNavigationToolset::hyper_navigation_inspect(OnDiskNavigation);
	TestTrue(TEXT("on-disk navigation request returns bounded disclosure"),
		OnDiskNavigationReport.bOk);
	TestFalse(TEXT("synchronous on-disk navigation inventory is never certified complete"),
		OnDiskNavigationReport.bRevisionComplete);
	TestTrue(TEXT("navigation inventory requires async cache"),
		OnDiskNavigationReport.Issues.ContainsByPredicate([](const FHyperAIWorldIssue& Issue)
		{
			return Issue.Code == TEXT("async_on_disk_index_required");
		}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
