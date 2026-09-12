// Games by Hyper 2026.

#include "HyperAIStudioPaper2DToolset.h"

#include "AssetRegistry/AssetData.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Paper2D::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIStudioPaper2DValueSnapshot ValidSnapshot(const FString& Kind)
	{
		FHyperAIStudioPaper2DValueSnapshot Snapshot;
		Snapshot.bComplete = true;
		Snapshot.Asset.TargetPath = TEXT("/Game/Paper2D/P_Test.P_Test");
		Snapshot.Asset.PackageName = TEXT("/Game/Paper2D/P_Test");
		Snapshot.Asset.AssetKind = Kind;
		Snapshot.Asset.PersistedRevision = Hash(TEXT("persisted"));
		Snapshot.Asset.VolatileObservationFingerprint = Hash(TEXT("volatile"));
		Snapshot.Asset.DiskExistence = TEXT("exists");
		Snapshot.Asset.PackageSavedHash = TEXT("0123456789abcdef");
		Snapshot.Asset.DiskSize = 4096;
		Snapshot.Asset.bLoaded = true;
		Snapshot.Asset.bWasLoadedFromDisk = true;
		Snapshot.Asset.bRevisionComplete = true;
		Snapshot.Asset.bProjectionComplete = true;
		Snapshot.Asset.CollisionMode = 0;
		if (Kind == TEXT("sprite"))
		{
			Snapshot.Asset.ClassPath = TEXT("/Script/Paper2D.PaperSprite");
			Snapshot.Asset.PixelsPerUnrealUnit = 1.0;
			Snapshot.Asset.SourceWidth = 32.0;
			Snapshot.Asset.SourceHeight = 32.0;
			Snapshot.Asset.RenderVertexCount = 6;
			Snapshot.Asset.DefaultMaterialPath = TEXT("/Paper2D/MaskedUnlitSpriteMaterial.MaskedUnlitSpriteMaterial");
		}
		else if (Kind == TEXT("flipbook"))
		{
			Snapshot.Asset.ClassPath = TEXT("/Script/Paper2D.PaperFlipbook");
			Snapshot.Asset.FramesPerSecond = 15.0;
			Snapshot.Asset.NumKeyFrames = 1;
			Snapshot.Asset.NumFrames = 2;
			Snapshot.Asset.DefaultMaterialPath = TEXT("/Paper2D/MaskedUnlitSpriteMaterial.MaskedUnlitSpriteMaterial");
			FHyperAIPaper2DKeyFrameRecord Frame;
			Frame.Index = 0;
			Frame.FrameRun = 2;
			Frame.SpritePath = TEXT("/Game/Paper2D/S_Test.S_Test");
			Snapshot.KeyFrames.Add(MoveTemp(Frame));
		}
		else if (Kind == TEXT("tile_set"))
		{
			Snapshot.Asset.ClassPath = TEXT("/Script/Paper2D.PaperTileSet");
			Snapshot.Asset.TileSheetPath = TEXT("/Game/Paper2D/T_Tiles.T_Tiles");
			Snapshot.Asset.TileSheetWidth = 256;
			Snapshot.Asset.TileSheetHeight = 256;
			Snapshot.Asset.TileWidth = 32;
			Snapshot.Asset.TileHeight = 32;
			Snapshot.Asset.TileCountX = 8;
			Snapshot.Asset.TileCountY = 8;
			Snapshot.Asset.TileCount = 64;
		}
		else
		{
			Snapshot.Asset.ClassPath = TEXT("/Script/Paper2D.PaperTileMap");
			Snapshot.Asset.MapWidth = 8;
			Snapshot.Asset.MapHeight = 8;
			Snapshot.Asset.TileWidth = 32;
			Snapshot.Asset.TileHeight = 32;
			Snapshot.Asset.PixelsPerUnrealUnit = 1.0;
			Snapshot.Asset.ProjectionMode = 0;
			Snapshot.Asset.LayerCount = 1;
			Snapshot.Asset.DefaultMaterialPath = TEXT("/Paper2D/MaskedUnlitSpriteMaterial.MaskedUnlitSpriteMaterial");
			FHyperAIPaper2DLayerRecord Layer;
			Layer.Index = 0;
			Layer.LayerPath = TEXT("/Game/Paper2D/P_Test.P_Test:Layer_0");
			Layer.LayerName = TEXT("Layer 0");
			Layer.Width = 8;
			Layer.Height = 8;
			Layer.bVisibleInEditor = true;
			Layer.bVisibleInGame = true;
			Snapshot.Layers.Add(MoveTemp(Layer));
		}
		return Snapshot;
	}

	TSharedRef<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe> PatchPayload()
	{
		const TSharedRef<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe>();
		Payload->TargetPath = TEXT("/Game/Paper2D/TS_Test.TS_Test");
		Payload->BasePersistedRevision = Hash(TEXT("base"));
		Payload->Patch.ExpectedKind = TEXT("tile_set");
		Payload->Patch.bSetTileSize = true;
		Payload->Patch.TileWidth = 32;
		Payload->Patch.TileHeight = 32;
		Payload->SemanticFingerprint =
			FHyperAIStudioPaper2DContracts::ComputePatchSemanticFingerprint(*Payload);
		return Payload;
	}

	bool PreparePure(const TSharedRef<FHyperAIStudioPaper2DPatchPayload,
		ESPMode::ThreadSafe>& Payload, FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioPaper2DContracts::GetAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioPaper2DContracts::PackId;
		Binding.ToolName = TEXT("hyper_paper2d_apply_plan");
		Binding.VariantId = FHyperAIStudioPaper2DContracts::MutationVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("plugin.Paper2D"), EHyperAIStudioDomainPrerequisiteState::Available},
			{FHyperAIStudioPaper2DContracts::LiveProbeId,
				EHyperAIStudioDomainPrerequisiteState::Available}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = MoveTemp(Binding);
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = Payload->TargetPath;
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 150;
		Contract.MaxOutputBytes = 32768;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(
			Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPaper2DManifestTest,
	"HyperAIStudio.NativeTools.Paper2D.ManifestDelegationAndBlockingGroup",
	HyperAIStudio::Paper2D::Tests::Flags)

bool FHyperAIStudioPaper2DManifestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioPaper2DContracts::PackId),
		FString(TEXT("paper2d")));
	TestEqual(TEXT("cohort id"), FString(FHyperAIStudioPaper2DContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiopaper2dtoolset.v1")));
	TestEqual(TEXT("qualified toolset"),
		FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioPaper2D.HyperAIStudioPaper2DToolset")));
	const TArray<FHyperAIStudioPaper2DManifestEntry>& Manifest =
		FHyperAIStudioPaper2DContracts::GetManifest();
	TestEqual(TEXT("exact three callables"), Manifest.Num(), 3);
	TestEqual(TEXT("inspect first"), Manifest[0].Name,
		FString(TEXT("hyper_paper2d_inspect")));
	TestEqual(TEXT("apply second"), Manifest[1].Name,
		FString(TEXT("hyper_paper2d_apply_plan")));
	TestEqual(TEXT("validate third"), Manifest[2].Name,
		FString(TEXT("hyper_paper2d_validate")));
	TestFalse(TEXT("SourceCandidate unavailable without dev gate"),
		FHyperAIStudioPaper2DContracts::IsRegistrationAllowed(false));
	TestEqual(TEXT("exact generic Epic delegates"),
		FHyperAIStudioPaper2DContracts::GetEpicDelegates().Num(), 16);
	TestEqual(TEXT("Python review id"),
		FString(FHyperAIStudioPaper2DContracts::PythonAccessReviewId),
		FString(TEXT("epic-ue58-python-access-2026-08-15")));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioPaper2DContracts::GetAdapterDescriptor();
	TestEqual(TEXT("three descriptor variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking group absent from nonblocking descriptor list"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	TestTrue(TEXT("request ids use payload namespace"),
		Descriptor.Variants[0].RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
	TestTrue(TEXT("result ids use result namespace"),
		Descriptor.Variants[0].ResultTypeId.StartsWith(TEXT("hyperai.result.")));
	TestEqual(TEXT("honest non-dry blocker"),
		FString(FHyperAIStudioPaper2DContracts::NonDryCallableState),
		FString(TEXT("bounded_rebuild_save_backend_required")));
	FString Reason;
	FHyperAIPaper2DMetadataPatch Flipbook;
	Flipbook.ExpectedKind = TEXT("flipbook");
	Flipbook.bSetFramesPerSecond = true;
	TestTrue(TEXT("public flipbook FPS mutator is fast-executable"),
		FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(Flipbook, Reason));
	FHyperAIPaper2DMetadataPatch Sprite;
	Sprite.ExpectedKind = TEXT("sprite");
	Sprite.bSetPixelsPerUnrealUnit = true;
	TestFalse(TEXT("sprite metadata remains gated without a public setter"),
		FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(Sprite, Reason));
	TestTrue(TEXT("gated field explains the concrete API gap"),
		Reason.Contains(TEXT("no public UE 5.8 metadata setters")));
	FHyperAIPaper2DMetadataPatch TileSet;
	TileSet.ExpectedKind = TEXT("tile_set");
	TileSet.bSetTileSize = true;
	TileSet.bSetTileSpacing = true;
	TileSet.bSetTileDrawingOffset = true;
	TestTrue(TEXT("all closed TileSet fields use public setters"),
		FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(TileSet, Reason));
	FHyperAIPaper2DMetadataPatch TileMap;
	TileMap.ExpectedKind = TEXT("tile_map");
	TileMap.bSetCollisionMode = true;
	TileMap.bSetCollisionThickness = true;
	TestTrue(TEXT("TileMap collision setters are fast-executable"),
		FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(TileMap, Reason));
	TileMap.bSetPixelsPerUnrealUnit = true;
	TestFalse(TEXT("mixed TileMap patch cannot partially mutate"),
		FHyperAIStudioPaper2DContracts::CanExecuteFastPatch(TileMap, Reason));
	TestTrue(TEXT("TileMap gate names the missing setter"),
		Reason.Contains(TEXT("pixels-per-unit setter")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPaper2DDetachedValidationTest,
	"HyperAIStudio.NativeTools.Paper2D.DetachedFourKindValidation",
	HyperAIStudio::Paper2D::Tests::Flags)

bool FHyperAIStudioPaper2DDetachedValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Paper2D::Tests;
	for (const FString Kind : {FString(TEXT("sprite")), FString(TEXT("flipbook")),
		FString(TEXT("tile_set")), FString(TEXT("tile_map"))})
	{
		FHyperAIPaper2DValidateReport Report;
		TestTrue(*FString::Printf(TEXT("%s validator executes"), *Kind),
			FHyperAIStudioPaper2DContracts::ValidateDetached(
				ValidSnapshot(Kind), TEXT("authoring_ready"), 32, 65536, Report));
		TestTrue(*FString::Printf(TEXT("%s fixture valid"), *Kind), Report.bValid);
		TestTrue(*FString::Printf(TEXT("%s validator sealed"), *Kind),
			FHyperAIStudioPaper2DContracts::IsCanonicalSha256(
				Report.ValidatorFingerprint));
	}
	FHyperAIStudioPaper2DValueSnapshot Broken = ValidSnapshot(TEXT("flipbook"));
	Broken.KeyFrames[0].FrameRun = 0;
	FHyperAIPaper2DValidateReport Invalid;
	TestTrue(TEXT("invalid detached fixture still evaluates"),
		FHyperAIStudioPaper2DContracts::ValidateDetached(
			Broken, TEXT("structural"), 32, 65536, Invalid));
	TestFalse(TEXT("broken frame run rejected"), Invalid.bValid);
	TestTrue(TEXT("broken frame run produces error"), Invalid.ErrorCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPaper2DPrepareZeroEffectTest,
	"HyperAIStudio.NativeTools.Paper2D.PurePrepareCloneAndZeroEffectAdapter",
	HyperAIStudio::Paper2D::Tests::Flags)

bool FHyperAIStudioPaper2DPrepareZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Paper2D::Tests;
	const TSharedRef<FHyperAIStudioPaper2DPatchPayload, ESPMode::ThreadSafe> Payload =
		PatchPayload();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("clone has independent address"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone semantic seal"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("public pure Prepare accepts exact detached contract"),
		PreparePure(Payload, Prepared, Error));
	TestTrue(TEXT("pure plan hash canonical"),
		FHyperAIStudioPaper2DContracts::IsCanonicalSha256(Prepared.PlanHash));

	FHyperAIStudioPaper2DDomainAdapter Adapter;
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioPaper2DContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_paper2d_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioPaper2DContracts::MutationVariantId;
	Context.Binding.ExpectedAdapterFingerprint = Adapter.GetDescriptor().AdapterFingerprint;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *Payload);
	TestEqual(TEXT("concrete edit rejects before effect"), Result.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("stable backend blocker"), Result.StatusCode,
		FString(FHyperAIStudioPaper2DContracts::NonDryCallableState));
	TestFalse(TEXT("zero-effect adapter returns no result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPaper2DNoLoadTest,
	"HyperAIStudio.NativeTools.Paper2D.TriStateAndNoLoad",
	HyperAIStudio::Paper2D::Tests::Flags)

bool FHyperAIStudioPaper2DNoLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("Exists classification"),
		FHyperAIStudioPaper2DContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Exists), FString(TEXT("exists")));
	TestEqual(TEXT("Unknown remains unknown"),
		FHyperAIStudioPaper2DContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	const FString Missing =
		TEXT("/Game/__HyperAIStudioPaper2DNeverLoaded.__HyperAIStudioPaper2DNeverLoaded");
	TestNull(TEXT("fixture starts unloaded"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIStudioPaper2DValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	TestFalse(TEXT("capture rejects unloaded identity without loading"),
		FHyperAIStudioPaper2DContracts::CaptureExact(
			Missing, 50, Snapshot, Status, Diagnostic));
	TestEqual(TEXT("stable no-load status"), Status, FString(TEXT("target_not_loaded")));
	TestNull(TEXT("capture leaves fixture unloaded"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
