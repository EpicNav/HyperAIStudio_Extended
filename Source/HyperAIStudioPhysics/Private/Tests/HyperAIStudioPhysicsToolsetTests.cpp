// Games by Hyper 2026.

#include "HyperAIStudioPhysicsToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Physics::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIStudioPhysicsValueSnapshot ValidSnapshot()
	{
		FHyperAIStudioPhysicsValueSnapshot Snapshot;
		Snapshot.bComplete = true;
		Snapshot.Asset.TargetPath = TEXT("/Game/Physics/PA_Test.PA_Test");
		Snapshot.Asset.ClassPath = UPhysicsAsset::StaticClass()->GetPathName();
		Snapshot.Asset.PackageName = TEXT("/Game/Physics/PA_Test");
		Snapshot.Asset.PersistedRevision = Hash(TEXT("persisted"));
		Snapshot.Asset.VolatileObservationFingerprint = Hash(TEXT("volatile"));
		Snapshot.Asset.bRevisionComplete = true;
		Snapshot.Asset.bLoaded = true;
		Snapshot.Asset.bWasLoadedFromDisk = true;
		Snapshot.Asset.DiskExistence = TEXT("exists");
		Snapshot.Asset.PackageSavedHash = TEXT("0123456789abcdef");
		Snapshot.Asset.DiskSize = 4096;
		Snapshot.Asset.BodyCount = 1;
		Snapshot.Asset.Solver.PositionIterations = 8;
		Snapshot.Asset.Solver.VelocityIterations = 1;
		Snapshot.Asset.Solver.ProjectionIterations = 1;
		Snapshot.Asset.Solver.CullDistance = 5.0;
		Snapshot.Asset.Solver.MaxDepenetrationVelocity = 100.0;
		Snapshot.Asset.Solver.FixedTimeStep = 0.0;
		Snapshot.Asset.Solver.Fingerprint = Hash(TEXT("solver"));
		FHyperAIPhysicsBodyRecord Body;
		Body.StableId = Hash(TEXT("body-id"));
		Body.BoneName = TEXT("pelvis");
		Body.BodyObjectPath = TEXT("/Game/Physics/PA_Test.PA_Test:SkeletalBodySetup_0");
		Body.BodyFingerprint = Hash(TEXT("body"));
		Body.DefaultInstanceFingerprint = Hash(TEXT("instance"));
		Body.BodySetupGuid = TEXT("11111111-1111-1111-1111-111111111111");
		Body.CollisionProfileName = TEXT("PhysicsActor");
		Body.MassScale = 1.0;
		Body.bShapeProjectionComplete = true;
		Body.bShapeGeometryValid = true;
		Body.SphereCount = 1;
		Body.TotalShapeCount = 1;
		Snapshot.Bodies.Add(MoveTemp(Body));
		return Snapshot;
	}

	TSharedRef<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe> PatchPayload()
	{
		const TSharedRef<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe>();
		Payload->TargetPath = TEXT("/Game/Physics/PA_Test.PA_Test");
		Payload->BasePersistedRevision = Hash(TEXT("base"));
		Payload->Patch.BodyName = TEXT("pelvis");
		Payload->Patch.ExpectedBodyFingerprint = Hash(TEXT("body"));
		Payload->Patch.bSetCollisionProfile = true;
		Payload->Patch.CollisionProfileName = TEXT("Ragdoll");
		Payload->SemanticFingerprint =
			FHyperAIStudioPhysicsContracts::ComputePatchSemanticFingerprint(*Payload);
		return Payload;
	}

	bool PreparePure(const TSharedRef<FHyperAIStudioPhysicsPatchPayload,
		ESPMode::ThreadSafe>& Payload, FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioPhysicsContracts::GetAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioPhysicsContracts::PackId;
		Binding.ToolName = TEXT("hyper_physics_apply_plan");
		Binding.VariantId = FHyperAIStudioPhysicsContracts::MutationVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.PhysicsAssetEditor"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("plugin.ChaosClothAssetEditor"), EHyperAIStudioDomainPrerequisiteState::Missing},
			{FHyperAIStudioPhysicsContracts::LiveProbeId,
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
		Contract.MaxGameThreadMs = 200;
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
	FHyperAIStudioPhysicsManifestDelegationTest,
	"HyperAIStudio.NativeTools.Physics.ManifestDelegationAndBlockingGroups",
	HyperAIStudio::Physics::Tests::Flags)

bool FHyperAIStudioPhysicsManifestDelegationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioPhysicsContracts::PackId),
		FString(TEXT("physics_chaos")));
	TestEqual(TEXT("cohort id"), FString(FHyperAIStudioPhysicsContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiophysicstoolset.v1")));
	TestEqual(TEXT("qualified toolset"),
		FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioPhysics.HyperAIStudioPhysicsToolset")));
	const TArray<FHyperAIStudioPhysicsManifestEntry>& Manifest =
		FHyperAIStudioPhysicsContracts::GetManifest();
	TestEqual(TEXT("exact three callables"), Manifest.Num(), 3);
	TestEqual(TEXT("inspect first"), Manifest[0].Name,
		FString(TEXT("hyper_physics_inspect")));
	TestEqual(TEXT("apply second"), Manifest[1].Name,
		FString(TEXT("hyper_physics_apply_plan")));
	TestEqual(TEXT("validate third"), Manifest[2].Name,
		FString(TEXT("hyper_physics_validate")));
	TestFalse(TEXT("SourceCandidate is unavailable without dev gate"),
		FHyperAIStudioPhysicsContracts::IsRegistrationAllowed(false));
	TestEqual(TEXT("exact 23 native delegates"),
		FHyperAIStudioPhysicsContracts::GetEpicDelegates().Num(), 23);
	TestEqual(TEXT("exact five Python delegates"),
		FHyperAIStudioPhysicsContracts::GetPythonDelegates().Num(), 5);
	TestEqual(TEXT("native review id"),
		FString(FHyperAIStudioPhysicsContracts::NativeAccessReviewId),
		FString(TEXT("epic-ue5.8-native-aicallable-access-2026-08-15")));
	TestEqual(TEXT("native review records hash"),
		FString(FHyperAIStudioPhysicsContracts::NativeAccessReviewRecordsSha256),
		FString(TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88")));
	TestEqual(TEXT("Python review id"),
		FString(FHyperAIStudioPhysicsContracts::PythonAccessReviewId),
		FString(TEXT("epic-ue58-python-access-2026-08-15")));
	TestEqual(TEXT("Python review records hash"),
		FString(FHyperAIStudioPhysicsContracts::PythonAccessReviewRecordsSha256),
		FString(TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10")));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioPhysicsContracts::GetAdapterDescriptor();
	TestEqual(TEXT("three descriptor variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking groups are never listed as non-blocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	TestEqual(TEXT("stable non-dry blocker"),
		FString(FHyperAIStudioPhysicsContracts::NonDryCallableState),
		FString(TEXT("bounded_compile_or_simulation_backend_required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhysicsDetachedValidationTest,
	"HyperAIStudio.NativeTools.Physics.DetachedIndependentValidation",
	HyperAIStudio::Physics::Tests::Flags)

bool FHyperAIStudioPhysicsDetachedValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Physics::Tests;
	FHyperAIStudioPhysicsValueSnapshot Snapshot = ValidSnapshot();
	FHyperAIPhysicsValidateReport Valid;
	TestTrue(TEXT("pure structural validator executes"),
		FHyperAIStudioPhysicsContracts::ValidateDetached(Snapshot, TEXT("structural"),
			32, 65536, Valid));
	TestTrue(TEXT("complete structural fixture is valid"), Valid.bValid);
	TestTrue(TEXT("validator seal is canonical"),
		FHyperAIStudioPhysicsContracts::IsCanonicalSha256(Valid.ValidatorFingerprint));

	Snapshot.Asset.ConstraintCount = 1;
	FHyperAIPhysicsConstraintRecord Constraint;
	Constraint.StableId = Hash(TEXT("constraint-id"));
	Constraint.ConstraintFingerprint = Hash(TEXT("constraint"));
	Constraint.ChildBoneName = TEXT("pelvis");
	Constraint.ParentBoneName = TEXT("missing_parent");
	Snapshot.Constraints.Add(MoveTemp(Constraint));
	FHyperAIPhysicsValidateReport Invalid;
	TestTrue(TEXT("invalid detached fixture still evaluates"),
		FHyperAIStudioPhysicsContracts::ValidateDetached(Snapshot, TEXT("structural"),
			32, 65536, Invalid));
	TestFalse(TEXT("broken constraint closure is rejected"), Invalid.bValid);
	TestTrue(TEXT("closure produces an error"), Invalid.ErrorCount > 0);

	FHyperAIPhysicsValidateReport Simulation;
	TestTrue(TEXT("simulation policy evaluates without simulating"),
		FHyperAIStudioPhysicsContracts::ValidateDetached(ValidSnapshot(),
			TEXT("simulation_ready"), 32, 65536, Simulation));
	TestFalse(TEXT("simulation readiness is never falsely claimed"), Simulation.bValid);
	TestFalse(TEXT("simulation validation remains incomplete"), Simulation.bComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhysicsPrepareAndZeroEffectTest,
	"HyperAIStudio.NativeTools.Physics.PurePrepareDeepCloneAndZeroEffectAdapter",
	HyperAIStudio::Physics::Tests::Flags)

bool FHyperAIStudioPhysicsPrepareAndZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Physics::Tests;
	const TSharedRef<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe> Payload =
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
	TestTrue(TEXT("pure plan hash is canonical"),
		FHyperAIStudioPhysicsContracts::IsCanonicalSha256(Prepared.PlanHash));

	FHyperAIStudioPhysicsDomainAdapter Adapter;
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioPhysicsContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_physics_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioPhysicsContracts::MutationVariantId;
	Context.Binding.ExpectedAdapterFingerprint = Adapter.GetDescriptor().AdapterFingerprint;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *Payload);
	TestEqual(TEXT("concrete edit rejects before effect"), Result.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("stable backend blocker"), Result.StatusCode,
		FString(FHyperAIStudioPhysicsContracts::NonDryCallableState));
	TestFalse(TEXT("zero-effect adapter returns no result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhysicsCursorAndNoLoadTest,
	"HyperAIStudio.NativeTools.Physics.CursorTriStateAndNoLoad",
	HyperAIStudio::Physics::Tests::Flags)

bool FHyperAIStudioPhysicsCursorAndNoLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Physics::Tests;
	const FString Path = TEXT("/Game/Physics/PA_Test.PA_Test");
	const FString Revision = Hash(TEXT("revision"));
	const FString Cursor = FHyperAIStudioPhysicsContracts::BuildCursor(
		Path, Revision, 16, 7);
	int32 Offset = INDEX_NONE;
	TestTrue(TEXT("sealed cursor round trips"),
		FHyperAIStudioPhysicsContracts::ParseCursor(Cursor, Path, Revision, 16, Offset));
	TestEqual(TEXT("cursor offset"), Offset, 7);
	TestFalse(TEXT("revision drift invalidates cursor"),
		FHyperAIStudioPhysicsContracts::ParseCursor(Cursor, Path, Hash(TEXT("other")),
			16, Offset));
	TestEqual(TEXT("Exists classification"),
		FHyperAIStudioPhysicsContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Exists), FString(TEXT("exists")));
	TestEqual(TEXT("Unknown stays fail-closed"),
		FHyperAIStudioPhysicsContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));

	const FString Missing = TEXT("/Game/__HyperAIStudioPhysicsNeverLoaded.__HyperAIStudioPhysicsNeverLoaded");
	TestNull(TEXT("fixture starts unloaded"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIStudioPhysicsValueSnapshot Snapshot;
	FString Status;
	FString Diagnostic;
	TestFalse(TEXT("capture rejects unloaded identity without loading"),
		FHyperAIStudioPhysicsContracts::CaptureExact(Missing, 50, Snapshot,
			Status, Diagnostic));
	TestEqual(TEXT("stable no-load status"), Status, FString(TEXT("target_not_loaded")));
	TestNull(TEXT("capture leaves fixture unloaded"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
