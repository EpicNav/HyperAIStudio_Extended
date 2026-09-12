// Games by Hyper 2026.

#include "HyperAIStudioGameplayAIToolset.h"

#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::GameplayAI::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	TSharedRef<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe> MakePayload(
		const FString& BaseRevision,
		const FString& TargetPath,
		const FString& ExpectedRevision,
		const FString& KeyName)
	{
		TSharedRef<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe>();
		FHyperAIStudioGameplayAIBackendOperation Operation;
		Operation.Kind = EHyperAIStudioGameplayAIOperationKind::AddBlackboardKey;
		Operation.TargetPath = TargetPath;
		Operation.ExpectedRevision = ExpectedRevision;
		Operation.Name = KeyName;
		Operation.KeyType = TEXT("bool");
		Payload->Operations.Add(Operation);
		Payload->BaseRevision = BaseRevision;
		Payload->SemanticFingerprint =
			FHyperAIStudioGameplayAIContracts::ComputePayloadSemanticFingerprint(
				Payload->Operations, Payload->BaseRevision);
		return Payload;
	}

	bool PrepareArtifact(
		const TSharedRef<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const auto& Descriptor = FHyperAIStudioGameplayAIContracts::GetBaseAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioGameplayAIContracts::PackId;
		Binding.ToolName = TEXT("hyper_gameplay_ai_apply_plan");
		Binding.VariantId = FHyperAIStudioGameplayAIContracts::MutationVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.AIModule"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.AIGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.BehaviorTreeEditor"), EHyperAIStudioDomainPrerequisiteState::Available}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = TEXT("gameplay_ai:test-fixture");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 8192;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplayAIManifestAndPrerequisiteTest,
	"HyperAIStudio.NativeTools.GameplayAI.ManifestAndPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplayAIManifestAndPrerequisiteTest::RunTest(const FString& Parameters)
{
	const auto& Manifest = FHyperAIStudioGameplayAIContracts::GetManifest();
	TestEqual(TEXT("Exactly three Gameplay AI contracts"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect name"), Manifest[0].Name, FString(TEXT("hyper_gameplay_ai_inspect")));
	TestEqual(TEXT("Apply name"), Manifest[1].Name, FString(TEXT("hyper_gameplay_ai_apply_plan")));
	TestEqual(TEXT("Validate name"), Manifest[2].Name, FString(TEXT("hyper_gameplay_ai_validate")));
	for (const auto& Entry : Manifest)
	{
		TestEqual(TEXT("One exact qualified Toolset"), Entry.QualifiedToolset,
			FString(TEXT("HyperAIStudioGameplayAI.HyperAIStudioGameplayAIToolset")));
	}
	const auto& Variants = FHyperAIStudioGameplayAIFacade::GetVariantDescriptors();
	TestEqual(TEXT("Exact variant matrix size"), Variants.Num(), 9);
	TestTrue(TEXT("Behavior Tree base inspect/apply exists"),
		Variants[0].Variant == TEXT("behavior_tree")
		&& Variants[0].bInspectImplemented && Variants[0].bApplyImplemented
		&& !Variants[0].bApplyBackendExecutable
		&& Variants[0].SupportedCases.Contains(TEXT("set_blackboard_reference_dry_run_stage"))
		&& Variants[0].UnsupportedCases.Contains(TEXT("mutation_execution_until_async_host")));
	TestTrue(TEXT("AI Controller is inspect-only in this slice"),
		Variants[2].Variant == TEXT("ai_controller_perception")
		&& Variants[2].bInspectImplemented && !Variants[2].bApplyImplemented);
	TestTrue(TEXT("StateTree declares exact plugin/modules"),
		Variants[3].RequiredPlugins == TArray<FString>{TEXT("StateTree")}
		&& Variants[3].RequiredModules == TArray<FString>{TEXT("StateTreeModule"), TEXT("StateTreeEditorModule")});
	TestTrue(TEXT("Behavior-SmartObject bridge declares all prerequisites"),
		Variants[7].RequiredPlugins.Contains(TEXT("GameplayBehaviorSmartObjects"))
		&& Variants[7].RequiredPlugins.Contains(TEXT("GameplayBehaviors"))
		&& Variants[7].RequiredPlugins.Contains(TEXT("SmartObjects"))
		&& Variants[7].RequiredPlugins.Contains(TEXT("GameplayAbilities")));
	TestTrue(TEXT("Interaction bridge declares StateTree, SmartObjects, GAS"),
		Variants[8].RequiredPlugins.Contains(TEXT("StateTree"))
		&& Variants[8].RequiredPlugins.Contains(TEXT("SmartObjects"))
		&& Variants[8].RequiredPlugins.Contains(TEXT("GameplayAbilities"))
		&& Variants[8].RequiredPlugins.Contains(TEXT("ContextualAnimation"))
		&& Variants[8].RequiredPlugins.Contains(TEXT("NavCorridor"))
		&& Variants[8].RequiredModules.Contains(TEXT("SmartObjectsEditorModule"))
		&& Variants[8].RequiredModules.Contains(TEXT("ContextualAnimationEditor"))
		&& Variants[8].UnsupportedCases.Contains(TEXT("typed_adapter_not_implemented")));
	const auto Statuses = FHyperAIStudioGameplayAIFacade::ResolveVariantStatuses();
	TestEqual(TEXT("Base mutation variants report stage-only truthfully"),
		Statuses[0].State, FString(TEXT("staged_backend_required")));
	TestEqual(TEXT("Perception remains inspect-only"), Statuses[2].State,
		FString(TEXT("inspect_ready_apply_unavailable")));
	TestEqual(TEXT("Optional adapters stay unavailable regardless installation"),
		Statuses[3].State, FString(TEXT("adapter_unavailable")));
	FHyperAIGameplayAIInspectRequest OptionalInspect;
	OptionalInspect.Variant = TEXT("state_tree");
	const FHyperAIGameplayAIInspectReport OptionalReport =
		UHyperAIStudioGameplayAIToolset::hyper_gameplay_ai_inspect(OptionalInspect);
	TestFalse(TEXT("Optional inspect does not claim an unavailable adapter"), OptionalReport.bOk);
	TestEqual(TEXT("Optional inspect has an exact unavailable status"), OptionalReport.Status,
		FString(TEXT("variant_adapter_unavailable")));
	TestEqual(TEXT("Unavailable response still exposes the prerequisite matrix"),
		OptionalReport.Variants.Num(), 9);
	TestFalse(TEXT("Source candidate is never production-registered without admission"),
		FHyperAIStudioGameplayAIContracts::IsRegistrationAllowed(false));
	TestEqual(TEXT("Loaded Gameplay AI registration follows generated owner policy"),
		FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGameplayAIToolset::StaticClass(),
			FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName()),
		FHyperAIStudioGameplayAIContracts::IsRegistrationAllowed(
			FHyperAIStudioGameplayAIContracts::IsPendingTestRegistrationEnabled()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplayAISchemaTest,
	"HyperAIStudio.NativeTools.GameplayAI.ClosedSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplayAISchemaTest::RunTest(const FString& Parameters)
{
	const FString Revision = HyperAIStudio::GameplayAI::Tests::Hash(TEXT("revision"));
	FHyperAIGameplayAIPlanOperation Add;
	Add.Type = TEXT("add_blackboard_key");
	Add.TargetPath = TEXT("/Game/AI/BB_Test.BB_Test");
	Add.ExpectedRevision = Revision;
	Add.Name = TEXT("TargetActor");
	Add.KeyType = TEXT("bool");
	Add.bInstanceSynced = true;
	FHyperAIStudioGameplayAIBackendOperation Backend;
	FString Code;
	FString Error;
	TestTrue(TEXT("Closed Blackboard add-key shape is accepted"),
		FHyperAIStudioGameplayAIContracts::ValidateOperationShape(Add, Backend, Code, Error));
	TestEqual(TEXT("Typed operation kind"), static_cast<uint8>(Backend.Kind),
		static_cast<uint8>(EHyperAIStudioGameplayAIOperationKind::AddBlackboardKey));

	FHyperAIGameplayAIPlanOperation Optional = Add;
	Optional.Type = TEXT("state_tree_add_state");
	TestFalse(TEXT("Optional operation fails closed"),
		FHyperAIStudioGameplayAIContracts::ValidateOperationShape(Optional, Backend, Code, Error));
	TestEqual(TEXT("Optional failure is explicit"), Code, FString(TEXT("variant_adapter_unavailable")));

	FHyperAIGameplayAIPlanOperation Bad = Add;
	Bad.ReferencePath = TEXT("/Game/Unexpected.Unexpected");
	TestFalse(TEXT("Unrelated fields are rejected"),
		FHyperAIStudioGameplayAIContracts::ValidateOperationShape(Bad, Backend, Code, Error));
	TestEqual(TEXT("Strict shape error"), Code, FString(TEXT("invalid_add_key_shape")));

	TestNull(TEXT("Edit DTO exposes no client authorization token"),
		FHyperAIGameplayAIApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestTrue(TEXT("Payload schema is canonical"),
		FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(
			FHyperAIStudioGameplayAIContracts::PayloadSchemaFingerprint()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplayAIAdapterFailClosedTest,
	"HyperAIStudio.NativeTools.GameplayAI.AdapterFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplayAIAdapterFailClosedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GameplayAI::Tests;
	const auto Payload = MakePayload(Hash(TEXT("base")), TEXT("/Game/AI/BB_Test.BB_Test"),
		Hash(TEXT("target")), TEXT("Target"));
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioGameplayAIContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_gameplay_ai_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioGameplayAIContracts::MutationVariantId;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FHyperAIStudioGameplayAIDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Blocked = Adapter.Execute(Context, Payload.Get());
	TestEqual(TEXT("Exact apply remains blocked until async hosting"), Blocked.StatusCode,
		FString(TEXT("async_host_required")));
	TestEqual(TEXT("Blocked adapter reports no known effect"), static_cast<uint8>(Blocked.Outcome),
		static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect));
	TestFalse(TEXT("Blocked adapter returns no result payload"), Blocked.Payload.IsValid());

	Context.Binding.PackId = TEXT("not_gameplay_ai");
	const FHyperAIStudioDomainAdapterResult Mismatch = Adapter.Execute(Context, Payload.Get());
	TestEqual(TEXT("Cross-pack dispatch fails exact binding"), Mismatch.StatusCode,
		FString(TEXT("typed_binding_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplayAIRevisionAndValidationTest,
	"HyperAIStudio.NativeTools.GameplayAI.RevisionAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplayAIRevisionAndValidationTest::RunTest(const FString& Parameters)
{
	FHyperAIGameplayAIAssetRecord Blackboard;
	Blackboard.Variant = TEXT("blackboard");
	Blackboard.AssetPath = TEXT("/Game/AI/BB_A.BB_A");
	Blackboard.StableId = TEXT("blackboard:/Game/AI/BB_A.BB_A");
	Blackboard.bRevisionComplete = true;
	FHyperAIGameplayAIKeyView KeyA;
	KeyA.Name = TEXT("A");
	KeyA.StableId = TEXT("key:A");
	KeyA.KeyType = TEXT("/Script/AIModule.BlackboardKeyType_Bool");
	FHyperAIGameplayAIKeyView KeyB = KeyA;
	KeyB.Name = TEXT("B");
	KeyB.StableId = TEXT("key:B");
	Blackboard.Keys = {KeyB, KeyA};

	FHyperAIGameplayAIAssetRecord Tree;
	Tree.Variant = TEXT("behavior_tree");
	Tree.AssetPath = TEXT("/Game/AI/BT_A.BT_A");
	Tree.StableId = TEXT("behavior-tree:/Game/AI/BT_A.BT_A");
	Tree.bRevisionComplete = true;
	Tree.BlackboardPath = Blackboard.AssetPath;

	FHyperAIStudioGameplayAIValueSnapshot A;
	A.ScopeFingerprint = HyperAIStudio::GameplayAI::Tests::Hash(TEXT("scope"));
	A.Records = {Tree, Blackboard};
	FHyperAIStudioGameplayAIValueSnapshot B;
	B.ScopeFingerprint = A.ScopeFingerprint;
	Blackboard.Keys = {KeyA, KeyB};
	B.Records = {Blackboard, Tree};
	const FString RevisionA = FHyperAIStudioGameplayAIContracts::ComputeSnapshotRevision(A);
	const FString RevisionB = FHyperAIStudioGameplayAIContracts::ComputeSnapshotRevision(B);
	TestEqual(TEXT("Revision ignores record/nested ordering"), RevisionA, RevisionB);
	TestTrue(TEXT("Revision is canonical"),
		FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(RevisionA));

	FHyperAIStudioGameplayAIValueSnapshot Invalid = A;
	Invalid.Records[0].ParentAssetPath = Invalid.Records[0].AssetPath;
	Invalid.Records[0].Variant = TEXT("blackboard");
	Invalid.Records[0].Keys.Add(Invalid.Records[0].Keys.IsEmpty() ? KeyA : Invalid.Records[0].Keys[0]);
	FHyperAIStudioGameplayAIContracts::ComputeSnapshotRevision(Invalid);
	bool bTruncated = false;
	const auto Issues = FHyperAIStudioGameplayAIContracts::ValidateValueSnapshot(
		Invalid, false, 128, bTruncated);
	TestTrue(TEXT("Independent validator finds bad parent"),
		Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("invalid_blackboard_parent")
				|| Issue.Code == TEXT("blackboard_parent_cycle");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGameplayAIStagingTest,
	"HyperAIStudio.NativeTools.GameplayAI.IdempotentStage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGameplayAIStagingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GameplayAI::Tests;
	FHyperAIStudioGameplayAIStagingService::Reset();
	const FString Base = Hash(TEXT("base"));
	const FString TargetRevision = Hash(TEXT("target"));
	const auto Payload = MakePayload(Base, TEXT("/Game/AI/BB_Test.BB_Test"), TargetRevision, TEXT("A"));
	const auto Clone = Payload->CloneImmutable();
	TestTrue(TEXT("Immutable clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("Clone preserves semantic fingerprint"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	const auto CloneSource = MakePayload(Base, TEXT("/Game/AI/BB_Clone.BB_Clone"),
		TargetRevision, TEXT("Before"));
	const auto DetachedClone = CloneSource->CloneImmutable();
	const int32 DetachedSize = DetachedClone->GetBoundedByteSize();
	CloneSource->Operations[0].Name = TEXT("A_much_longer_name_after_the_detached_copy");
	TestEqual(TEXT("Detached clone does not alias mutable source fields"),
		DetachedClone->GetBoundedByteSize(), DetachedSize);
	TestTrue(TEXT("Source mutation changes only the source projection"),
		CloneSource->GetBoundedByteSize() != DetachedSize);

	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("Shared typed executor seals the stage artifact"),
		PrepareArtifact(Payload, Prepared, Error));
	FHyperAIStudioGameplayAIStagedArtifact Artifact;
	Artifact.Prepared = Prepared;
	Artifact.Payload = Payload;
	Artifact.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Artifact.OperationId = TEXT("gameplay-ai-stage-001");
	Artifact.StageId = FHyperAIStudioGameplayAIContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	Artifact.ExpiresMonotonicMs = static_cast<int64>(FPlatformTime::Seconds() * 1000.0) + 15000;
	bool bReplay = false;
	TestTrue(TEXT("First exact stage succeeds"),
		FHyperAIStudioGameplayAIStagingService::Stage(Artifact, bReplay, Error));
	TestFalse(TEXT("First stage is not replay"), bReplay);
	TestTrue(TEXT("Exact replay is idempotent"),
		FHyperAIStudioGameplayAIStagingService::Stage(Artifact, bReplay, Error));
	TestTrue(TEXT("Replay is identified"), bReplay);

	const auto ForgedPayload = MakePayload(Base, TEXT("/Game/AI/BB_Test.BB_Test"),
		TargetRevision, TEXT("Forged"));
	ForgedPayload->SemanticFingerprint = Hash(TEXT("caller-claimed-semantic-value"));
	FHyperAIStudioPreparedTypedArtifact ForgedPrepared;
	TestTrue(TEXT("Shared prepare seals only the declared contract"),
		PrepareArtifact(ForgedPayload, ForgedPrepared, Error));
	FHyperAIStudioGameplayAIStagedArtifact Forged = Artifact;
	Forged.Prepared = ForgedPrepared;
	Forged.Payload = ForgedPayload;
	Forged.OperationId = TEXT("gameplay-ai-stage-forged-001");
	Forged.StageId = FHyperAIStudioGameplayAIContracts::ComputeStageId(
		Forged.CanonicalProjectId, Forged.OperationId,
		ForgedPrepared.PlanHash, ForgedPrepared.EffectFingerprint);
	TestFalse(TEXT("Concrete stage recomputes and rejects a forged semantic fingerprint"),
		FHyperAIStudioGameplayAIStagingService::Stage(Forged, bReplay, Error));

	const auto ConflictingPayload = MakePayload(Base, TEXT("/Game/AI/BB_Test.BB_Test"),
		TargetRevision, TEXT("B"));
	FHyperAIStudioPreparedTypedArtifact ConflictingPrepared;
	TestTrue(TEXT("Conflicting fixture seals"),
		PrepareArtifact(ConflictingPayload, ConflictingPrepared, Error));
	FHyperAIStudioGameplayAIStagedArtifact Conflict = Artifact;
	Conflict.Prepared = ConflictingPrepared;
	Conflict.Payload = ConflictingPayload;
	Conflict.StageId = FHyperAIStudioGameplayAIContracts::ComputeStageId(
		Conflict.CanonicalProjectId, Conflict.OperationId,
		ConflictingPrepared.PlanHash, ConflictingPrepared.EffectFingerprint);
	TestFalse(TEXT("Same operation id cannot bind a different plan"),
		FHyperAIStudioGameplayAIStagingService::Stage(Conflict, bReplay, Error));
	TestEqual(TEXT("Only one exact artifact remains staged"),
		FHyperAIStudioGameplayAIStagingService::NumStaged(), 1);
	FHyperAIStudioGameplayAIStagingService::Reset();
	return true;
}

#endif
