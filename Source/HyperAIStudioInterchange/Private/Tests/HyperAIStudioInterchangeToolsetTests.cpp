// Games by Hyper 2026.

#include "HyperAIStudioInterchangeToolset.h"

#include "AssetRegistry/AssetData.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Interchange::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIStudioInterchangeValueSnapshot ProvenanceSnapshot()
	{
		FHyperAIStudioInterchangeValueSnapshot Snapshot;
		Snapshot.bComplete = true;
		Snapshot.Asset.TargetPath = TEXT("/Game/Meshes/SM_Test.SM_Test");
		Snapshot.Asset.ClassPath = TEXT("/Script/Engine.StaticMesh");
		Snapshot.Asset.PackageName = TEXT("/Game/Meshes/SM_Test");
		Snapshot.Asset.AssetKind = TEXT("static_mesh");
		Snapshot.Asset.ImportDataClassPath = TEXT("/Script/InterchangeEngine.InterchangeAssetImportData");
		Snapshot.Asset.ImportDataObjectPath = TEXT("/Game/Meshes/SM_Test.SM_Test:InterchangeAssetImportData_0");
		Snapshot.Asset.PersistedRevision = Hash(TEXT("interchange-persisted"));
		Snapshot.Asset.VolatileObservationFingerprint = Hash(TEXT("interchange-volatile"));
		Snapshot.Asset.bRevisionComplete = true;
		Snapshot.Asset.bLoaded = true;
		Snapshot.Asset.bWasLoadedFromDisk = true;
		Snapshot.Asset.bImportDataWasLoadedFromDisk = true;
		Snapshot.Asset.PipelineProjectionStatus = TEXT("deliberately_not_materialized");
		Snapshot.Asset.DiskExistence = TEXT("exists");
		Snapshot.Asset.PackageSavedHash = TEXT("0123456789abcdef");
		Snapshot.Asset.DiskSize = 4096;
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioInterchangeManifestAuthorityTest,
	"HyperAIStudio.NativeTools.Interchange.ManifestAuthorityTypesAndPaths",
	HyperAIStudio::Interchange::Tests::Flags)

bool FHyperAIStudioInterchangeManifestAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("pack"), FString(FHyperAIStudioInterchangeContracts::PackId),
		FString(TEXT("geometry_interchange")));
	TestEqual(TEXT("exact three"), FHyperAIStudioInterchangeContracts::GetManifest().Num(), 3);
	TestEqual(TEXT("Python delegates"),
		FHyperAIStudioInterchangeContracts::GetEpicPythonDelegates().Num(), 2);
	TestEqual(TEXT("product capability requirements"),
		FHyperAIStudioInterchangeContracts::GetCapabilityRequirements().Num(), 3);
	TSet<FString> Reflected;
	for (TFieldIterator<UFunction> It(UHyperAIStudioInterchangeToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) Reflected.Add(It->GetName());
	}
	TestEqual(TEXT("exact reflected AICallables"), Reflected.Num(), 3);
	for (const FHyperAIStudioInterchangeManifestEntry& Entry :
		FHyperAIStudioInterchangeContracts::GetManifest())
	{
		TestTrue(TEXT("manifest reflected"), Reflected.Contains(Entry.Name));
	}
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioInterchangeContracts::GetAdapterDescriptor();
	TestEqual(TEXT("descriptor variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking groups absent from nonblocking list"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	TestEqual(TEXT("apply is external effect"), Descriptor.Variants[1].Safety,
		EHyperAIStudioDomainSafety::ExternalEffect);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("request namespace"), Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("result namespace"), Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		TestNotEqual(TEXT("disjoint DTO ids"), Variant.RequestTypeId, Variant.ResultTypeId);
	}
	FString Normalized;
	FString Extension;
	TestTrue(TEXT("contained relative FBX accepted"),
		FHyperAIStudioInterchangeContracts::ResolveProjectRelativeSourcePath(
			TEXT("Source/Meshes/test.fbx"), Normalized, Extension));
	TestEqual(TEXT("extension normalized"), Extension, FString(TEXT("fbx")));
	TestFalse(TEXT("traversal rejected"),
		FHyperAIStudioInterchangeContracts::ResolveProjectRelativeSourcePath(
			TEXT("../outside.fbx"), Normalized, Extension));
	TestFalse(TEXT("script extension rejected"),
		FHyperAIStudioInterchangeContracts::ResolveProjectRelativeSourcePath(
			TEXT("Source/run.py"), Normalized, Extension));
	TestEqual(TEXT("stable blocker"),
		FString(FHyperAIStudioInterchangeContracts::NonDryCallableState),
		FString(TEXT("bounded_import_continuation_required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioInterchangeDetachedAndZeroEffectTest,
	"HyperAIStudio.NativeTools.Interchange.DetachedValidationCloneAndZeroEffect",
	HyperAIStudio::Interchange::Tests::Flags)

bool FHyperAIStudioInterchangeDetachedAndZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Interchange::Tests;
	FHyperAIInterchangeValidateReport Provenance;
	TestTrue(TEXT("detached validator evaluates"),
		FHyperAIStudioInterchangeContracts::ValidateDetached(ProvenanceSnapshot(),
			TEXT("provenance"), 32, 65536, Provenance));
	TestTrue(TEXT("bounded provenance valid"), Provenance.bValid);
	FHyperAIInterchangeValidateReport Reimport;
	TestTrue(TEXT("reimport readiness evaluates without materialization"),
		FHyperAIStudioInterchangeContracts::ValidateDetached(ProvenanceSnapshot(),
			TEXT("reimport_ready"), 32, 65536, Reimport));
	TestFalse(TEXT("pipeline readiness not falsely claimed"), Reimport.bValid);
	TestFalse(TEXT("reimport readiness deliberately incomplete"), Reimport.bComplete);

	const TSharedRef<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioInterchangePatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/Meshes/SM_Test.SM_Test");
	Payload->BasePersistedRevision = Hash(TEXT("base"));
	Payload->Patch.SourceIndex = 0;
	Payload->Patch.ExpectedSourceFingerprint = Hash(TEXT("source"));
	Payload->Patch.ProjectRelativeSourcePath = TEXT("Source/Meshes/test.fbx");
	Payload->ResolvedProjectRelativeSourcePath = TEXT("Source/Meshes/test.fbx");
	Payload->SemanticFingerprint =
		FHyperAIStudioInterchangeContracts::ComputePatchSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("clone independent"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone seal"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	FHyperAIStudioInterchangeDomainAdapter Adapter;
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioInterchangeContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_interchange_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioInterchangeContracts::MutationVariantId;
	Context.Binding.ExpectedAdapterFingerprint = Adapter.GetDescriptor().AdapterFingerprint;
	Context.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *Payload);
	TestEqual(TEXT("external effect rejected before effect"), Result.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("exact blocker"), Result.StatusCode,
		FString(FHyperAIStudioInterchangeContracts::NonDryCallableState));
	TestFalse(TEXT("no effect result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioInterchangeCursorAndNoLoadTest,
	"HyperAIStudio.NativeTools.Interchange.CursorTriStateAndNoLoad",
	HyperAIStudio::Interchange::Tests::Flags)

bool FHyperAIStudioInterchangeCursorAndNoLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Interchange::Tests;
	const FString Path = TEXT("/Game/Meshes/SM_Test.SM_Test");
	const FString Revision = Hash(TEXT("revision"));
	const FString Cursor = FHyperAIStudioInterchangeContracts::BuildCursor(Path, Revision, 8, 2);
	int32 Offset = INDEX_NONE;
	TestTrue(TEXT("cursor round trip"),
		FHyperAIStudioInterchangeContracts::ParseCursor(Cursor, Path, Revision, 8, Offset));
	TestEqual(TEXT("offset"), Offset, 2);
	TestEqual(TEXT("Unknown fail closed"),
		FHyperAIStudioInterchangeContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	const FString Missing = TEXT("/Game/__HyperAIInterchangeNeverLoaded.__HyperAIInterchangeNeverLoaded");
	TestNull(TEXT("fixture starts unloaded"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIStudioInterchangeValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	TestFalse(TEXT("capture rejects without load"),
		FHyperAIStudioInterchangeContracts::CaptureExact(Missing, 50, Snapshot, Status, Diagnostic));
	TestEqual(TEXT("stable status"), Status, FString(TEXT("target_not_loaded")));
	TestNull(TEXT("fixture remains unloaded"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
