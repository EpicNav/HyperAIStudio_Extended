// Games by Hyper 2026.

#include "HyperAIStudioGeometryToolset.h"

#include "AssetRegistry/AssetData.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Geometry::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIStudioGeometryValueSnapshot StructuralSnapshot()
	{
		FHyperAIStudioGeometryValueSnapshot Snapshot;
		Snapshot.bComplete = true;
		Snapshot.Asset.TargetPath = TEXT("/Game/Meshes/SK_Test.SK_Test");
		Snapshot.Asset.ClassPath = TEXT("/Script/Engine.SkeletalMesh");
		Snapshot.Asset.PackageName = TEXT("/Game/Meshes/SK_Test");
		Snapshot.Asset.AssetKind = TEXT("skeletal_mesh");
		Snapshot.Asset.PersistedRevision = Hash(TEXT("geometry-persisted"));
		Snapshot.Asset.VolatileObservationFingerprint = Hash(TEXT("geometry-volatile"));
		Snapshot.Asset.bRevisionComplete = true;
		Snapshot.Asset.bLoaded = true;
		Snapshot.Asset.bWasLoadedFromDisk = true;
		Snapshot.Asset.DiskExistence = TEXT("exists");
		Snapshot.Asset.PackageSavedHash = TEXT("0123456789abcdef");
		Snapshot.Asset.DiskSize = 4096;
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGeometryManifestAuthorityTest,
	"HyperAIStudio.NativeTools.Geometry.ManifestAuthorityAndTypes",
	HyperAIStudio::Geometry::Tests::Flags)

bool FHyperAIStudioGeometryManifestAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("pack"), FString(FHyperAIStudioGeometryContracts::PackId),
		FString(TEXT("geometry_interchange")));
	TestEqual(TEXT("exact three"), FHyperAIStudioGeometryContracts::GetManifest().Num(), 3);
	TestEqual(TEXT("native delegates"),
		FHyperAIStudioGeometryContracts::GetEpicNativeDelegates().Num(), 22);
	TestEqual(TEXT("Python delegates"),
		FHyperAIStudioGeometryContracts::GetEpicPythonDelegates().Num(), 36);
	TestEqual(TEXT("product capability requirements"),
		FHyperAIStudioGeometryContracts::GetCapabilityRequirements().Num(), 10);
	TSet<FString> Reflected;
	for (TFieldIterator<UFunction> It(UHyperAIStudioGeometryToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) Reflected.Add(It->GetName());
	}
	TestEqual(TEXT("exact reflected AICallables"), Reflected.Num(), 3);
	for (const FHyperAIStudioGeometryManifestEntry& Entry :
		FHyperAIStudioGeometryContracts::GetManifest())
	{
		TestTrue(TEXT("manifest reflected"), Reflected.Contains(Entry.Name));
	}
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioGeometryContracts::GetAdapterDescriptor();
	TestEqual(TEXT("descriptor variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking groups absent from nonblocking list"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("request namespace"), Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("result namespace"), Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		TestNotEqual(TEXT("disjoint DTO ids"), Variant.RequestTypeId, Variant.ResultTypeId);
	}
	TestEqual(TEXT("stable blocker"),
		FString(FHyperAIStudioGeometryContracts::NonDryCallableState),
		FString(TEXT("bounded_mesh_build_save_continuation_required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGeometryDetachedAndZeroEffectTest,
	"HyperAIStudio.NativeTools.Geometry.DetachedValidationCloneAndZeroEffect",
	HyperAIStudio::Geometry::Tests::Flags)

bool FHyperAIStudioGeometryDetachedAndZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Geometry::Tests;
	FHyperAIGeometryValidateReport Structural;
	TestTrue(TEXT("detached validator evaluates"),
		FHyperAIStudioGeometryContracts::ValidateDetached(StructuralSnapshot(),
			TEXT("structural"), 32, 65536, Structural));
	TestTrue(TEXT("bounded skeletal projection structurally valid"), Structural.bValid);
	FHyperAIGeometryValidateReport BuildReady;
	TestTrue(TEXT("build readiness evaluates without build"),
		FHyperAIStudioGeometryContracts::ValidateDetached(StructuralSnapshot(),
			TEXT("static_mesh_build_ready"), 32, 65536, BuildReady));
	TestFalse(TEXT("skeletal mesh not static-build-ready"), BuildReady.bValid);

	const TSharedRef<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioGeometryPatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/Meshes/SM_Test.SM_Test");
	Payload->BasePersistedRevision = Hash(TEXT("base"));
	Payload->Patch.LodIndex = 0;
	Payload->Patch.ExpectedLodFingerprint = Hash(TEXT("lod"));
	Payload->Patch.bSetRecomputeNormals = true;
	Payload->Patch.bRecomputeNormals = true;
	Payload->SemanticFingerprint =
		FHyperAIStudioGeometryContracts::ComputePatchSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("clone independent"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone seal"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	FHyperAIStudioGeometryDomainAdapter Adapter;
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioGeometryContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_geometry_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioGeometryContracts::MutationVariantId;
	Context.Binding.ExpectedAdapterFingerprint = Adapter.GetDescriptor().AdapterFingerprint;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *Payload);
	TestEqual(TEXT("edit rejected before effect"), Result.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("exact blocker"), Result.StatusCode,
		FString(FHyperAIStudioGeometryContracts::NonDryCallableState));
	TestFalse(TEXT("no effect result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGeometryCursorAndNoLoadTest,
	"HyperAIStudio.NativeTools.Geometry.CursorTriStateAndNoLoad",
	HyperAIStudio::Geometry::Tests::Flags)

bool FHyperAIStudioGeometryCursorAndNoLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Geometry::Tests;
	const FString Path = TEXT("/Game/Meshes/SM_Test.SM_Test");
	const FString Revision = Hash(TEXT("revision"));
	const FString Cursor = FHyperAIStudioGeometryContracts::BuildCursor(Path, Revision, 8, 3);
	int32 Offset = INDEX_NONE;
	TestTrue(TEXT("cursor round trip"),
		FHyperAIStudioGeometryContracts::ParseCursor(Cursor, Path, Revision, 8, Offset));
	TestEqual(TEXT("offset"), Offset, 3);
	TestEqual(TEXT("Unknown fail closed"),
		FHyperAIStudioGeometryContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	const FString Missing = TEXT("/Game/__HyperAIGeometryNeverLoaded.__HyperAIGeometryNeverLoaded");
	TestNull(TEXT("fixture starts unloaded"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIStudioGeometryValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	TestFalse(TEXT("capture rejects without load"),
		FHyperAIStudioGeometryContracts::CaptureExact(Missing, 50, Snapshot, Status, Diagnostic));
	TestEqual(TEXT("stable status"), Status, FString(TEXT("target_not_loaded")));
	TestNull(TEXT("fixture remains unloaded"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
