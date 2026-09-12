// Games by Hyper 2026.

#include "HyperAIStudioLiveProductionToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::LiveProduction::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAILiveProductionNDisplayTopology Topology(const int32 Width)
	{
		FHyperAILiveProductionNDisplayTopology Value;
		FHyperAILiveProductionNDisplayNode Primary;
		Primary.NodeId = TEXT("node_a");
		Primary.HostAddress = TEXT("127.0.0.1");
		Primary.bPrimary = true;
		Value.Nodes.Add(MoveTemp(Primary));
		FHyperAILiveProductionNDisplayNode Secondary;
		Secondary.NodeId = TEXT("node_b");
		Secondary.HostAddress = TEXT("render-b.local");
		Value.Nodes.Add(MoveTemp(Secondary));
		FHyperAILiveProductionNDisplayViewport Viewport;
		Viewport.NodeId = TEXT("node_a");
		Viewport.ViewportId = TEXT("viewport_main");
		Viewport.Width = Width;
		Viewport.Height = 1080;
		Value.Viewports.Add(MoveTemp(Viewport));
		return Value;
	}

	FHyperAILiveProductionDetachedSnapshot IdentitySnapshot()
	{
		FHyperAILiveProductionObjectRecord Record;
		Record.FamilyId = TEXT("ndisplay");
		Record.TargetPath = TEXT("/Game/VP/Cluster.Cluster");
		Record.PackageName = TEXT("/Game/VP/Cluster");
		Record.ClassPath = TEXT("/Script/DisplayCluster.DisplayClusterBlueprint");
		Record.DiskExistence = TEXT("exists");
		Record.PackageSavedHash = TEXT("0123456789abcdef");
		Record.DiskSize = 4096;
		Record.bLoaded = true;
		Record.bWasLoadedFromDisk = true;
		Record.bFamilyClassMatched = true;
		Record.bPersistedIdentityComplete = true;
		Record.PersistedFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeObjectPersistedFingerprint(Record);
		Record.VolatileFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeObjectVolatileFingerprint(Record);

		FHyperAILiveProductionDetachedSnapshot Snapshot;
		Snapshot.FamilyId = Record.FamilyId;
		Snapshot.RequestFingerprint = Hash(TEXT("detached-request"));
		Snapshot.bIdentityProjectionComplete = true;
		Snapshot.bSemanticProjectionComplete = false;
		Snapshot.TotalRecords = 1;
		Snapshot.Records.Add(MoveTemp(Record));
		Snapshot.PersistedFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeSnapshotPersistedFingerprint(
				Snapshot.Records);
		Snapshot.VolatileFingerprint =
			FHyperAIStudioLiveProductionContracts::ComputeSnapshotVolatileFingerprint(
				Snapshot.Records);
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLiveProductionManifestTest,
	"HyperAIStudio.NativeTools.LiveProduction.ManifestDescriptorAndReflection",
	HyperAIStudio::LiveProduction::Tests::Flags)

bool FHyperAIStudioLiveProductionManifestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioLiveProductionContracts::PackId),
		FString(TEXT("live_production")));
	TestEqual(TEXT("atomic source cohort"),
		FString(FHyperAIStudioLiveProductionContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioliveproductiontoolset.v1")));
	TestEqual(TEXT("plugin blocking group"),
		FString(FHyperAIStudioLiveProductionContracts::PluginRequirementGroupId),
		FString(TEXT("live_production_plugin")));
	TestEqual(TEXT("backend blocking group"),
		FString(FHyperAIStudioLiveProductionContracts::BackendRequirementGroupId),
		FString(TEXT("live_production_backend")));

	const TArray<FHyperAIStudioLiveProductionManifestEntry>& Manifest =
		FHyperAIStudioLiveProductionContracts::GetManifest();
	const TArray<FString> ExpectedNames = {
		TEXT("hyper_live_production_inspect"),
		TEXT("hyper_live_production_apply_plan"),
		TEXT("hyper_live_production_validate")};
	TestEqual(TEXT("exact manifest callable count"), Manifest.Num(), ExpectedNames.Num());
	for (int32 Index = 0; Index < FMath::Min(Manifest.Num(), ExpectedNames.Num()); ++Index)
	{
		TestEqual(TEXT("manifest callable order"), Manifest[Index].Name,
			ExpectedNames[Index]);
		TestEqual(TEXT("single qualified owner"), Manifest[Index].QualifiedToolset,
			FString(TEXT("HyperAIStudioLiveProduction.HyperAIStudioLiveProductionToolset")));
	}

	TSet<FString> ReflectedCallables;
	for (TFieldIterator<UFunction> It(UHyperAIStudioLiveProductionToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) ReflectedCallables.Add(It->GetName());
	}
	TestEqual(TEXT("exact reflected AICallable count"), ReflectedCallables.Num(), 3);
	for (const FString& ExpectedName : ExpectedNames)
	{
		TestTrue(TEXT("expected reflected callable"), ReflectedCallables.Contains(ExpectedName));
	}

	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioLiveProductionContracts::GetAdapterDescriptor();
	TestEqual(TEXT("exact descriptor variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking groups excluded from nonblocking list"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	const TArray<FString> ExpectedRequestTypes = {
		TEXT("hyperai.payload.live-production.inspect.v1"),
		TEXT("hyperai.payload.live-production.ndisplay-intent.v1"),
		TEXT("hyperai.payload.live-production.validate.v1")};
	const TArray<FString> ExpectedResultTypes = {
		TEXT("hyperai.result.live-production.inspect.v1"),
		TEXT("hyperai.result.live-production.apply-blocked.v1"),
		TEXT("hyperai.result.live-production.validate.v1")};
	for (int32 Index = 0; Index < Descriptor.Variants.Num(); ++Index)
	{
		const FHyperAIStudioDomainVariantDescriptor& Variant = Descriptor.Variants[Index];
		TestTrue(TEXT("request id uses central payload namespace"),
			Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("result id uses central result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		if (ExpectedRequestTypes.IsValidIndex(Index))
		{
			TestEqual(TEXT("exact request type id"), Variant.RequestTypeId,
				ExpectedRequestTypes[Index]);
			TestEqual(TEXT("exact result type id"), Variant.ResultTypeId,
				ExpectedResultTypes[Index]);
		}
	}
	TestTrue(TEXT("adapter fingerprint canonical"),
		FHyperAIStudioLiveProductionContracts::IsCanonicalSha256(
			Descriptor.AdapterFingerprint));
	if (Descriptor.Variants.Num() == 3)
	{
		TestEqual(TEXT("inspect is Read"),
			static_cast<int32>(Descriptor.Variants[0].Safety),
			static_cast<int32>(EHyperAIStudioDomainSafety::Read));
		TestEqual(TEXT("apply is ExternalEffect"),
			static_cast<int32>(Descriptor.Variants[1].Safety),
			static_cast<int32>(EHyperAIStudioDomainSafety::ExternalEffect));
		TestEqual(TEXT("validate is Read"),
			static_cast<int32>(Descriptor.Variants[2].Safety),
			static_cast<int32>(EHyperAIStudioDomainSafety::Read));
	}
	TestFalse(TEXT("production registration remains source-candidate gated"),
		FHyperAIStudioLiveProductionContracts::IsRegistrationAllowed(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLiveProductionAuthorityTest,
	"HyperAIStudio.NativeTools.LiveProduction.ExactAuthorityAndDelegationMatrix",
	HyperAIStudio::LiveProduction::Tests::Flags)

bool FHyperAIStudioLiveProductionAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAILiveProductionAuthorityRow>& Epic =
		FHyperAIStudioLiveProductionContracts::GetEpicReviewAuthority();
	TestEqual(TEXT("exact Epic review rows"), Epic.Num(), 2);
	if (Epic.Num() == 2)
	{
		TestEqual(TEXT("native review count"), Epic[0].ReviewedCallableCount, 278);
		TestEqual(TEXT("Python review count"), Epic[1].ReviewedCallableCount, 598);
		TestEqual(TEXT("native live-production matches"), Epic[0].MatchingCallableCount, 0);
		TestEqual(TEXT("Python live-production matches"), Epic[1].MatchingCallableCount, 0);
		TestEqual(TEXT("native review id"), Epic[0].SourceId,
			FString(FHyperAIStudioLiveProductionContracts::NativeAccessReviewId));
		TestEqual(TEXT("Python review id"), Epic[1].SourceId,
			FString(FHyperAIStudioLiveProductionContracts::PythonAccessReviewId));
		TestEqual(TEXT("native review hash"), Epic[0].ReviewedFingerprint,
			FString(FHyperAIStudioLiveProductionContracts::NativeAccessReviewRecordsSha256));
		TestEqual(TEXT("Python review hash"), Epic[1].ReviewedFingerprint,
			FString(FHyperAIStudioLiveProductionContracts::PythonAccessReviewRecordsSha256));
	}

	const TArray<FHyperAILiveProductionAuthorityRow>& Plugins =
		FHyperAIStudioLiveProductionContracts::GetPluginDescriptorAuthority();
	const TArray<FString> PluginIds = {TEXT("plugin.LiveLink"), TEXT("plugin.RemoteControl"),
		TEXT("plugin.DMXEngine"), TEXT("plugin.Avalanche"), TEXT("plugin.NDisplay")};
	const TArray<FString> PluginHashes = {
		TEXT("sha256:33fd6909681bdbcbca55f03bd29ad39e88f0be74885ddf02d64402a91f8f4fe4"),
		TEXT("sha256:17c2d9744d8e269e5d0d7762251bab4475096fca486bf049f5e997136e60bf74"),
		TEXT("sha256:95ebbe4235d91c8caddb600ebd5501fea42d953461ec84cb227cb264674b5a62"),
		TEXT("sha256:3453fea9378af8cfdd5e82c050e0e252da10eec8dc299e63a221af9566cb3a76"),
		TEXT("sha256:62f63a8f0bf515841fce92f12207e540fb511ce0b694c2e00f7c7b71a0c7e7e0")};
	TestEqual(TEXT("five exact plugin descriptor authorities"), Plugins.Num(), 5);
	for (int32 Index = 0; Index < FMath::Min(Plugins.Num(), PluginIds.Num()); ++Index)
	{
		TestEqual(TEXT("plugin id"), Plugins[Index].SourceId, PluginIds[Index]);
		TestEqual(TEXT("plugin descriptor hash"), Plugins[Index].ReviewedFingerprint,
			PluginHashes[Index]);
		TestEqual(TEXT("plugin is capability gated"), Plugins[Index].Disposition,
			FString(TEXT("capability_gated")));
	}

	const TArray<FHyperAILiveProductionAuthorityRow>& Requirements =
		FHyperAIStudioLiveProductionContracts::GetCapabilityRequirements();
	const TArray<FString> ExpectedIds = {
		TEXT("capability.live_production.ndisplay_configuration"),
		TEXT("capability.live_production.avalanche"),
		TEXT("capability.live_production.avalanche_datalink"),
		TEXT("capability.live_production.avalanche_scene_state"),
		TEXT("capability.live_production.avalanche_transition"),
		TEXT("capability.live_production.dmx_control_console"),
		TEXT("capability.live_production.dmx_engine"),
		TEXT("capability.live_production.dmx_fixtures"),
		TEXT("capability.live_production.dmx_protocol"),
		TEXT("capability.live_production.livelink"),
		TEXT("capability.live_production.ndisplay"),
		TEXT("capability.live_production.pixel_streaming2"),
		TEXT("capability.live_production.remote_control"),
		TEXT("capability.live_production.remote_control_components"),
		TEXT("capability.live_production.remote_control_protocol_dmx"),
		TEXT("capability.live_production.remote_control_protocol_midi"),
		TEXT("capability.live_production.remote_control_protocol_osc"),
		TEXT("capability.live_production.scene_state_datalink"),
		TEXT("capability.live_production.svg_importer"),
		TEXT("capability.live_production.text3d")};
	TestEqual(TEXT("exact product requirement row count"), Requirements.Num(), ExpectedIds.Num());
	int32 Gated = 0;
	int32 Dropped = 0;
	for (int32 Index = 0; Index < FMath::Min(Requirements.Num(), ExpectedIds.Num()); ++Index)
	{
		TestEqual(TEXT("exact product requirement id/order"), Requirements[Index].SourceId,
			ExpectedIds[Index]);
		Gated += Requirements[Index].Disposition == TEXT("capability_gated") ? 1 : 0;
		Dropped += Requirements[Index].Disposition == TEXT("drop") ? 1 : 0;
	}
	TestEqual(TEXT("19 capability-gated rows"), Gated, 19);
	TestEqual(TEXT("one explicit drop"), Dropped, 1);
	if (Requirements.Num() > 11)
	{
		TestEqual(TEXT("PixelStreaming2 is the exact drop"), Requirements[11].SourceId,
			FString(TEXT("capability.live_production.pixel_streaming2")));
		TestTrue(TEXT("dropped row delegates no HyperAI contract"),
			Requirements[11].HyperAIContract.IsEmpty());
	}
	const FHyperAILiveProductionCapabilityStatus Status =
		FHyperAIStudioLiveProductionContracts::GetCapabilityStatus();
	TestTrue(TEXT("authority fingerprint canonical"),
		FHyperAIStudioLiveProductionContracts::IsCanonicalSha256(Status.AuthorityFingerprint));
	TestFalse(TEXT("optional families are not hard linked"), Status.bOptionalFamilyHardLinked);
	TestFalse(TEXT("external effect execution absent"), Status.bExternalEffectExecutionImplemented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLiveProductionDetachedValidationTest,
	"HyperAIStudio.NativeTools.LiveProduction.DetachedValueValidationAndSeals",
	HyperAIStudio::LiveProduction::Tests::Flags)

bool FHyperAIStudioLiveProductionDetachedValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::LiveProduction::Tests;
	FHyperAILiveProductionNDisplayTopology Value = Topology(1920);
	FString TopologyFingerprint;
	TArray<FHyperAILiveProductionIssue> Issues;
	TestTrue(TEXT("closed topology validates independently"),
		FHyperAIStudioLiveProductionContracts::ValidateTopologyValueModel(
			Value, TopologyFingerprint, Issues));
	TestTrue(TEXT("topology fingerprint canonical"),
		FHyperAIStudioLiveProductionContracts::IsCanonicalSha256(TopologyFingerprint));

	FHyperAILiveProductionNDisplayTopology Invalid = Value;
	Invalid.Nodes[1].NodeId = TEXT("node_a");
	FString InvalidFingerprint;
	TestFalse(TEXT("duplicate/unsorted node identity fails closed"),
		FHyperAIStudioLiveProductionContracts::ValidateTopologyValueModel(
			Invalid, InvalidFingerprint, Issues));

	FHyperAILiveProductionValidateRequest TopologyRequest;
	TopologyRequest.Policy = TEXT("ndisplay_topology");
	TopologyRequest.Topology = Value;
	const FHyperAILiveProductionValidateReport TopologyReport =
		FHyperAIStudioLiveProductionContracts::Validate(TopologyRequest);
	TestTrue(TEXT("detached topology value report valid"), TopologyReport.bValid);
	TestTrue(TEXT("detached topology is complete only as a value model"),
		TopologyReport.bComplete);
	TestTrue(TEXT("detached topology warns that it is not live evidence"),
		TopologyReport.WarningCount > 0);

	FHyperAILiveProductionDetachedSnapshot Snapshot = IdentitySnapshot();
	TestNotEqual(TEXT("persisted and volatile seals are independent"),
		Snapshot.PersistedFingerprint, Snapshot.VolatileFingerprint);
	FHyperAILiveProductionValidateRequest IdentityRequest;
	IdentityRequest.Policy = TEXT("identity_snapshot");
	IdentityRequest.Snapshot = Snapshot;
	const FHyperAILiveProductionValidateReport IdentityReport =
		FHyperAIStudioLiveProductionContracts::Validate(IdentityRequest);
	TestTrue(TEXT("detached identity verifies without UObject access"), IdentityReport.bValid);
	TestTrue(TEXT("complete detached identity projection"), IdentityReport.bComplete);
	TestTrue(TEXT("identity-only evidence warning"), IdentityReport.WarningCount > 0);

	IdentityRequest.Snapshot.Records[0].bPackageDirty = true;
	const FHyperAILiveProductionValidateReport Drifted =
		FHyperAIStudioLiveProductionContracts::Validate(IdentityRequest);
	TestFalse(TEXT("detached value drift invalidates its independent seal"), Drifted.bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLiveProductionZeroEffectTest,
	"HyperAIStudio.NativeTools.LiveProduction.NoLoadCursorAndStableZeroEffect",
	HyperAIStudio::LiveProduction::Tests::Flags)

bool FHyperAIStudioLiveProductionZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::LiveProduction::Tests;
	const FString RequestSeal = Hash(TEXT("request"));
	const FString PersistedSeal = Hash(TEXT("persisted"));
	const FString VolatileSeal = Hash(TEXT("volatile"));
	const FString Cursor = FHyperAIStudioLiveProductionContracts::BuildCursor(
		7, RequestSeal, PersistedSeal, VolatileSeal);
	int32 Offset = INDEX_NONE;
	TestTrue(TEXT("sealed cursor round trips"),
		FHyperAIStudioLiveProductionContracts::ParseCursor(Cursor, RequestSeal,
			PersistedSeal, VolatileSeal, Offset));
	TestEqual(TEXT("cursor offset"), Offset, 7);
	TestFalse(TEXT("volatile drift invalidates cursor"),
		FHyperAIStudioLiveProductionContracts::ParseCursor(Cursor, RequestSeal,
			PersistedSeal, Hash(TEXT("other")), Offset));
	TestEqual(TEXT("Unknown is fail-closed"),
		FHyperAIStudioLiveProductionContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));

	const FString Missing =
		TEXT("/Game/__HyperAIStudioLiveProductionNeverLoaded.__HyperAIStudioLiveProductionNeverLoaded");
	TestNull(TEXT("fixture begins unloaded"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAILiveProductionApplyPlanRequest Request;
	Request.bDryRun = false;
	Request.OperationId = TEXT("op:cluster:0001");
	Request.ExpectedPlanHash = Hash(TEXT("prior-plan"));
	Request.TargetPath = Missing;
	Request.ExpectedPersistedFingerprint = PersistedSeal;
	Request.BaseTopology = Topology(1920);
	Request.DesiredTopology = Topology(1280);
	const FHyperAILiveProductionApplyPlanReport Blocked =
		FHyperAIStudioLiveProductionContracts::BuildPlan(Request);
	TestEqual(TEXT("stable non-dry continuation blocker"), Blocked.Status,
		FString(FHyperAIStudioLiveProductionContracts::NonDryCallableState));
	TestFalse(TEXT("no physical effect began"), Blocked.bEffectStarted);
	TestFalse(TEXT("non-dry never calls pure Prepare"), Blocked.bTypedPrepared);
	TestEqual(TEXT("physical effect count remains zero"),
		Blocked.Effects.PhysicalEffectCount, 0);
	TestNull(TEXT("non-dry blocker did not resolve/load target"),
		FSoftObjectPath(Missing).ResolveObject());

	Request.bDryRun = true;
	Request.ExpectedPlanHash.Reset();
	const FHyperAILiveProductionApplyPlanReport DryRun =
		FHyperAIStudioLiveProductionContracts::BuildPlan(Request);
	TestEqual(TEXT("dry-run refuses unloaded target"), DryRun.Status,
		FString(TEXT("target_not_loaded")));
	TestFalse(TEXT("failed dry-run is not prepared"), DryRun.bTypedPrepared);
	TestNull(TEXT("dry-run remains loaded-only"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
