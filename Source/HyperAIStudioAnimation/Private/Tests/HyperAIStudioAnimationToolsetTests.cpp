// Games by Hyper 2026.

#include "HyperAIStudioAnimationToolset.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Animation::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	TSharedRef<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe> MakePayload(
		const FString& BaseRevision,
		const FString& TargetPath,
		const FString& ExpectedRevision,
		const FString& LayerName)
	{
		const TSharedRef<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe>();
		FHyperAIStudioAnimationBackendOperation Operation;
		Operation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddLayer;
		Operation.Type = TEXT("animbp.add_layer");
		Operation.TargetPath = TargetPath;
		Operation.ExpectedRevision = ExpectedRevision;
		Operation.Name = LayerName;
		Payload->Operations.Add(MoveTemp(Operation));
		Payload->BaseRevision = BaseRevision;
		Payload->SemanticFingerprint =
			FHyperAIStudioAnimationContracts::ComputePayloadSemanticFingerprint(
				Payload->Operations, Payload->BaseRevision);
		return Payload;
	}

	bool PrepareArtifact(
		const TSharedRef<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const auto& Descriptor = FHyperAIStudioAnimationContracts::GetBaseAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioAnimationContracts::PackId;
		Binding.ToolName = TEXT("hyper_animation_apply_plan");
		Binding.VariantId = FHyperAIStudioAnimationContracts::MutationVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.Engine"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.AnimGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.BlueprintGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.Kismet"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.UnrealEd"), EHyperAIStudioDomainPrerequisiteState::Available}};
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
		Contract.EffectTarget = TEXT("animation:test-fixture");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 8192;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = FHyperAIStudioAnimationContracts::StageLifetimeMs;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimationManifestAndPrerequisiteTest,
	"HyperAIStudio.NativeTools.AnimationRigging.ManifestAndPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAnimationManifestAndPrerequisiteTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Exact generated pack id"),
		FString(FHyperAIStudioAnimationContracts::PackId), FString(TEXT("animation_rigging")));
	TestEqual(TEXT("Exact atomic cohort id"),
		FString(FHyperAIStudioAnimationContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioanimationtoolset.v1")));
	const auto& Manifest = FHyperAIStudioAnimationContracts::GetManifest();
	TestEqual(TEXT("Exactly three Animation/Rigging tools"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect name"), Manifest[0].Name, FString(TEXT("hyper_animation_inspect")));
	TestEqual(TEXT("Apply name"), Manifest[1].Name, FString(TEXT("hyper_animation_apply_plan")));
	TestEqual(TEXT("Validate name"), Manifest[2].Name, FString(TEXT("hyper_animation_validate")));
	for (const auto& Entry : Manifest)
	{
		TestEqual(TEXT("One exact qualified Toolset"), Entry.QualifiedToolset,
			FString(TEXT("HyperAIStudioAnimation.HyperAIStudioAnimationToolset")));
		const UFunction* Function = UHyperAIStudioAnimationToolset::StaticClass()->FindFunctionByName(
			FName(*Entry.Name));
		TestNotNull(TEXT("Manifest function is reflected"), Function);
		if (Function) TestTrue(TEXT("Manifest function is AICallable"), Function->HasMetaData(TEXT("AICallable")));
	}
	const auto& Variants = FHyperAIStudioAnimationFacade::GetVariantDescriptors();
	TestEqual(TEXT("Exact typed variant matrix"), Variants.Num(), 12);
	TestTrue(TEXT("Animation Blueprint lifecycle is a staged base variant"),
		Variants[0].Variant == TEXT("animation_blueprint")
		&& Variants[0].bInspectImplemented && Variants[0].bPlanSchemaImplemented
		&& !Variants[0].bApplyBackendExecutable
		&& Variants[0].SupportedCases.Contains(TEXT("state_machine_state_transition_inspection")));
	TestTrue(TEXT("Control Rig exposes only additive gap ownership"),
		Variants[7].Variant == TEXT("control_rig")
		&& Variants[7].RequiredPlugins.Contains(TEXT("ControlRig"))
		&& Variants[7].RequiredPlugins.Contains(TEXT("RigVM"))
		&& Variants[7].DelegatedEpicCases.Contains(
			TEXT("AnimationAssistantToolset_control_rig_hierarchy_and_graph_primitives"))
		&& Variants[7].UnsupportedCases.Contains(TEXT("reflection_fallback_prohibited")));
	TestTrue(TEXT("IK Rig and Retargeter declare exact plugin/module prerequisites"),
		Variants[8].RequiredPlugins == TArray<FString>{TEXT("IKRig")}
		&& Variants[8].RequiredModules.Contains(TEXT("IKRigEditor"))
		&& Variants[9].RequiredPlugins == TArray<FString>{TEXT("IKRig")});
	TestTrue(TEXT("Pose Search schema/database stay independently declared"),
		Variants[10].Variant == TEXT("pose_search_schema")
		&& Variants[11].Variant == TEXT("pose_search_database")
		&& Variants[10].RequiredModules.Contains(TEXT("PoseSearchEditor")));
	const auto Statuses = FHyperAIStudioAnimationFacade::ResolveVariantStatuses();
	TestEqual(TEXT("Base variants truthfully remain staged-only"), Statuses[0].State,
		FString(TEXT("staged_backend_required")));
	TestEqual(TEXT("Optional adapter stays unavailable regardless installation"), Statuses[7].State,
		FString(TEXT("adapter_unavailable")));
	FHyperAIAnimationInspectRequest OptionalInspect;
	OptionalInspect.Variant = TEXT("pose_search_schema");
	const FHyperAIAnimationInspectReport OptionalReport =
		UHyperAIStudioAnimationToolset::hyper_animation_inspect(OptionalInspect);
	TestFalse(TEXT("Optional inspect never reflects into an unloaded facade"), OptionalReport.bOk);
	TestEqual(TEXT("Optional inspect failure is exact"), OptionalReport.Status,
		FString(TEXT("variant_adapter_unavailable")));
	TestFalse(TEXT("Source candidate never production-registers without admission"),
		FHyperAIStudioAnimationContracts::IsRegistrationAllowed(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimationClosedSchemaTest,
	"HyperAIStudio.NativeTools.AnimationRigging.ClosedSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAnimationClosedSchemaTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Animation::Tests;
	FHyperAIAnimationPlanOperation Create;
	Create.Type = TEXT("animbp.create");
	Create.TargetPath = TEXT("/Game/HyperAIStudioTests/ABP_AnimationLifecycle.ABP_AnimationLifecycle");
	Create.ReferencePath = TEXT("/Game/Animation/SK_Test.SK_Test");
	FHyperAIStudioAnimationBackendOperation Backend;
	FString Code;
	FString Error;
	TestTrue(TEXT("Closed Animation Blueprint creation shape is accepted"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Create, Backend, Code, Error));
	TestTrue(TEXT("Creation is typed and marked absent-target"),
		Backend.Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate
		&& Backend.bCreatesTarget);
	FHyperAIAnimationPlanOperation NonPrimaryCreate = Create;
	NonPrimaryCreate.TargetPath =
		TEXT("/Game/HyperAIStudioTests/ABP_AnimationLifecycle.DifferentObjectName");
	TestFalse(TEXT("Create absence is queried only for a package-primary object name"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(
			NonPrimaryCreate, Backend, Code, Error));
	TestEqual(TEXT("Non-primary create rejection is stable"), Code,
		FString(TEXT("invalid_create_target_name")));

	FHyperAIAnimationPlanOperation Rule;
	Rule.Type = TEXT("animbp.set_transition_rule");
	Rule.TargetPath = Create.TargetPath;
	Rule.ExpectedRevision = Hash(TEXT("target"));
	Rule.Name = TEXT("Locomotion");
	FHyperAIAnimationNameMapping TransitionEndpoints;
	TransitionEndpoints.Source = TEXT("Grounded");
	TransitionEndpoints.Target = TEXT("InAir");
	Rule.Mappings.Add(TransitionEndpoints);
	Rule.TransitionRule.Kind = TEXT("time_remaining_ratio");
	Rule.TransitionRule.Threshold = 0.15;
	TestTrue(TEXT("Closed transition rule avoids free-form expressions"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Rule, Backend, Code, Error));

	FHyperAIAnimationPlanOperation Segment;
	Segment.Type = TEXT("composite.set_segments");
	Segment.TargetPath = TEXT("/Game/Animation/AC_Test.AC_Test");
	Segment.ExpectedRevision = Hash(TEXT("composite"));
	FHyperAIAnimationSegmentSpec SegmentSpec;
	SegmentSpec.SequencePath = TEXT("/Game/Animation/A_Test.A_Test");
	SegmentSpec.StartSeconds = 0.0;
	SegmentSpec.EndSeconds = 1.0;
	Segment.Segments.Add(SegmentSpec);
	TestTrue(TEXT("Bounded typed composite segment is accepted"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Segment, Backend, Code, Error));
	Segment.Samples.Add(FHyperAIAnimationBlendSampleSpec{});
	TestFalse(TEXT("Unused cross-variant fields are rejected"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Segment, Backend, Code, Error));
	TestEqual(TEXT("Strict shape error is stable"), Code,
		FString(TEXT("invalid_composite_segments_shape")));

	FHyperAIAnimationPlanOperation RemoveNotify;
	RemoveNotify.Type = TEXT("sequence.remove_notify");
	RemoveNotify.TargetPath = TEXT("/Game/Animation/A_Test.A_Test");
	RemoveNotify.ExpectedRevision = Hash(TEXT("sequence"));
	RemoveNotify.StableId = TEXT("notify:/Game/Animation/A_Test.A_Test:01234567-89ab-cdef-0123-456789abcdef");
	TestTrue(TEXT("Notify removal consumes the exact inspector-issued stable id"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(
			RemoveNotify, Backend, Code, Error));
	RemoveNotify.StableId = TEXT("notify:/Game/Animation/Other.Other:1");
	TestFalse(TEXT("Cross-target notify identity is rejected"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(
			RemoveNotify, Backend, Code, Error));
	TestEqual(TEXT("Notify identity failure is stable"), Code,
		FString(TEXT("invalid_sequence_remove_notify_shape")));

	FHyperAIAnimationPlanOperation PostCreateLayer;
	PostCreateLayer.Type = TEXT("animbp.add_layer");
	PostCreateLayer.TargetPath = Create.TargetPath;
	PostCreateLayer.Name = TEXT("UpperBody");
	TestTrue(TEXT("Post-create configuration shape permits symbolic absent-target binding"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(
			PostCreateLayer, Backend, Code, Error));
	UPackage* SkeletonPackage = CreatePackage(
		TEXT("/Game/HyperAIStudioTests/SK_AnimationLifecycle"));
	USkeleton* Skeleton = FindObject<USkeleton>(SkeletonPackage, TEXT("SK_AnimationLifecycle"));
	if (!Skeleton)
	{
		Skeleton = NewObject<USkeleton>(SkeletonPackage,
			TEXT("SK_AnimationLifecycle"), RF_Transient);
	}
	TestNotNull(TEXT("Loaded-only lifecycle fixture exists"), Skeleton);
	if (Skeleton)
	{
		Create.ReferencePath = Skeleton->GetPathName();
		FHyperAIAnimationPlanOperation AddMachine;
		AddMachine.Type = TEXT("animbp.add_state_machine");
		AddMachine.TargetPath = Create.TargetPath;
		AddMachine.Name = TEXT("Locomotion");
		FHyperAIAnimationPlanOperation AddGrounded;
		AddGrounded.Type = TEXT("animbp.add_state");
		AddGrounded.TargetPath = Create.TargetPath;
		AddGrounded.Name = TEXT("Locomotion");
		AddGrounded.SecondaryName = TEXT("Grounded");
		FHyperAIAnimationPlanOperation AddInAir = AddGrounded;
		AddInAir.SecondaryName = TEXT("InAir");
		FHyperAIAnimationPlanOperation AddTransition;
		AddTransition.Type = TEXT("animbp.add_transition");
		AddTransition.TargetPath = Create.TargetPath;
		AddTransition.Name = TEXT("Locomotion");
		AddTransition.Mappings.Add(TransitionEndpoints);
		FHyperAIAnimationPlanOperation LifecycleRule = Rule;
		LifecycleRule.ExpectedRevision.Reset();
		FHyperAIAnimationApplyPlanRequest Lifecycle;
		Lifecycle.Operations = {Create, AddMachine, AddGrounded, AddInAir,
			AddTransition, LifecycleRule, PostCreateLayer};
		const FHyperAIAnimationApplyPlanReport LifecycleReport =
			FHyperAIStudioAnimationContracts::BuildPlan(Lifecycle);
		TestTrue(TEXT("One dry-run supports create then configure as one compound lifecycle"),
			LifecycleReport.bOk && LifecycleReport.Status == TEXT("valid_dry_run"));
		TestEqual(TEXT("Compound lifecycle counts one created asset"),
			LifecycleReport.Effects.AssetsCreated, 1);
		TestEqual(TEXT("Compound lifecycle does not miscount a same-plan update"),
			LifecycleReport.Effects.AssetsUpdated, 0);
	}
	UPackage* SequencePackage = CreatePackage(
		TEXT("/Game/HyperAIStudioTests/A_AnimationSelector"));
	UAnimSequence* SelectorSequence = FindObject<UAnimSequence>(
		SequencePackage, TEXT("A_AnimationSelector"));
	if (!SelectorSequence)
	{
		SelectorSequence = NewObject<UAnimSequence>(SequencePackage,
			TEXT("A_AnimationSelector"), RF_Transient);
	}
	TestNotNull(TEXT("Loaded-only selector fixture exists"), SelectorSequence);
	if (SelectorSequence)
	{
		FSoftObjectProperty* PreviewProperty = FindFProperty<FSoftObjectProperty>(
			UAnimationAsset::StaticClass(), TEXT("PreviewSkeletalMesh"));
		TestNotNull(TEXT("UE 5.8 preview soft-reference audit property exists"), PreviewProperty);
		FSoftObjectPtr* PreviewValue = PreviewProperty
			? PreviewProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(SelectorSequence) : nullptr;
		if (PreviewValue)
		{
			*PreviewValue = FSoftObjectPath(
				TEXT("/Game/HyperAIStudioTests/DefinitelyMissingPreview.DefinitelyMissingPreview"));
			TestTrue(TEXT("Preview fixture starts unresolved"),
				!PreviewValue->IsNull() && PreviewValue->Get() == nullptr);
		}
		FHyperAIStudioAnimationValueSnapshot SelectorSnapshot;
		FString CaptureStatus;
		FString CaptureDiagnostic;
		const FString SelectorPath = SelectorSequence->GetPathName();
		const bool bCapturedSelector = FHyperAIStudioAnimationFacade::CaptureLoaded(
			{SelectorPath}, TEXT("animation_sequence"), true, SelectorSnapshot,
			CaptureStatus, CaptureDiagnostic);
		TestTrue(TEXT("Selector fixture captures without implicit load"),
			bCapturedSelector && SelectorSnapshot.Records.Num() == 1);
		if (PreviewValue)
		{
			TestTrue(TEXT("Loaded-only capture never resolves preview soft references"),
				PreviewValue->Get() == nullptr);
		}
		if (!bCapturedSelector || SelectorSnapshot.Records.Num() != 1) return false;
		FHyperAIAnimationPlanOperation MissingNotify;
		MissingNotify.Type = TEXT("sequence.remove_notify");
		MissingNotify.TargetPath = SelectorPath;
		MissingNotify.ExpectedRevision = SelectorSnapshot.Records[0].Revision;
		MissingNotify.StableId = TEXT("notify:") + SelectorPath + TEXT(":missing");
		FHyperAIAnimationApplyPlanRequest MissingSelectorPlan;
		MissingSelectorPlan.Operations.Add(MissingNotify);
		const FHyperAIAnimationApplyPlanReport MissingSelectorReport =
			FHyperAIStudioAnimationContracts::BuildPlan(MissingSelectorPlan);
		TestFalse(TEXT("Dry-run rejects a well-shaped but absent element selector"),
			MissingSelectorReport.bOk);
		TestTrue(TEXT("Missing selector evidence is explicit"),
			MissingSelectorReport.Issues.ContainsByPredicate([](const auto& Issue)
			{
				return Issue.Code == TEXT("notify_selector_missing");
			}));
		UPackage* PagePackage = CreatePackage(
			TEXT("/Game/HyperAIStudioTests/A_AnimationPageTwo"));
		UAnimSequence* PageSequence = FindObject<UAnimSequence>(
			PagePackage, TEXT("A_AnimationPageTwo"));
		if (!PageSequence)
		{
			PageSequence = NewObject<UAnimSequence>(PagePackage,
				TEXT("A_AnimationPageTwo"), RF_Transient);
		}
		if (!PageSequence) return false;
		FHyperAIAnimationInspectRequest FirstPage;
		FirstPage.AssetPaths = {SelectorPath, PageSequence->GetPathName()};
		FirstPage.Variant = TEXT("animation_sequence");
		FirstPage.PageSize = 1;
		FirstPage.bIncludeDetails = false;
		const FHyperAIAnimationInspectReport FirstPageReport =
			UHyperAIStudioAnimationToolset::hyper_animation_inspect(FirstPage);
		TestTrue(TEXT("First deterministic page emits a revision-bound cursor"),
			FirstPageReport.bOk && !FirstPageReport.NextCursor.IsEmpty());
		SelectorSequence->Notifies.AddDefaulted();
		FHyperAIAnimationInspectRequest StalePage = FirstPage;
		StalePage.Cursor = FirstPageReport.NextCursor;
		const FHyperAIAnimationInspectReport StalePageReport =
			UHyperAIStudioAnimationToolset::hyper_animation_inspect(StalePage);
		TestEqual(TEXT("Mutation between pages rejects the stale cursor"),
			StalePageReport.Status, FString(TEXT("stale_cursor_revision")));
		SelectorSequence->Notifies.Reset();
		const FString LongName = FString::ChrN(300, TEXT('L'));
		UPackage* LongPackage = CreatePackage(
			*(TEXT("/Game/HyperAIStudioTests/") + LongName));
		UAnimSequence* LongSequence = NewObject<UAnimSequence>(
			LongPackage, FName(*LongName), RF_Transient);
		if (!LongSequence) return false;
		FHyperAIAnimationInspectRequest SmallBudget;
		SmallBudget.AssetPaths = {LongSequence->GetPathName()};
		SmallBudget.Variant = TEXT("animation_sequence");
		SmallBudget.bIncludeDetails = false;
		SmallBudget.bIncludePrerequisites = false;
		SmallBudget.MaxOutputBytes = 4096;
		const FHyperAIAnimationInspectReport SmallBudgetReport =
			UHyperAIStudioAnimationToolset::hyper_animation_inspect(SmallBudget);
		TestTrue(TEXT("Oversized base record fails without a non-progress cursor"),
			SmallBudgetReport.Status == TEXT("record_exceeds_output_budget")
			&& SmallBudgetReport.NextCursor.IsEmpty());
		SelectorSequence->Notifies.SetNum(
			FHyperAIStudioAnimationContracts::MaxElementsPerAsset + 128);
		FHyperAIStudioAnimationValueSnapshot BoundedSnapshot;
		const bool bCapturedBounded = FHyperAIStudioAnimationFacade::CaptureLoaded(
			{SelectorPath}, TEXT("animation_sequence"), true, BoundedSnapshot,
			CaptureStatus, CaptureDiagnostic);
		TestTrue(TEXT("Pathological nested traversal stops exactly at the per-asset cap"),
			bCapturedBounded && BoundedSnapshot.Records.Num() == 1
			&& BoundedSnapshot.Records[0].Elements.Num()
				== FHyperAIStudioAnimationContracts::MaxElementsPerAsset
			&& !BoundedSnapshot.bComplete);
		SelectorSequence->Notifies.Reset();
		if (PreviewValue) PreviewValue->Reset();
	}

	FHyperAIAnimationPlanOperation Optional = Rule;
	Optional.Type = TEXT("ik_retargeter.set_chain_mapping");
	TestFalse(TEXT("Optional variant never receives a reflection fallback"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Optional, Backend, Code, Error));
	TestEqual(TEXT("Optional failure is explicit"), Code,
		FString(TEXT("variant_adapter_unavailable")));
	FHyperAIAnimationPlanOperation Script = Rule;
	Script.Type = TEXT("animbp.execute_python");
	TestFalse(TEXT("Script operation is outside the closed schema"),
		FHyperAIStudioAnimationContracts::ValidateOperationShape(Script, Backend, Code, Error));
	TestEqual(TEXT("No raw execution vocabulary"), Code, FString(TEXT("unknown_operation_type")));
	TestNull(TEXT("Edit request exposes no client authorization token"),
		FHyperAIAnimationApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestTrue(TEXT("Payload schema is canonical"),
		FHyperAIStudioAnimationContracts::IsCanonicalSha256(
			FHyperAIStudioAnimationContracts::PayloadSchemaFingerprint()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimationRevisionAndValidationTest,
	"HyperAIStudio.NativeTools.AnimationRigging.RevisionAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAnimationRevisionAndValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Animation::Tests;
	FHyperAIAnimationAssetRecord Sequence;
	Sequence.Variant = TEXT("animation_sequence");
	Sequence.StableId = TEXT("animation_sequence:/Game/Animation/A.A");
	Sequence.AssetPath = TEXT("/Game/Animation/A.A");
	Sequence.SkeletonPath = TEXT("/Game/Animation/SK.SK");
	Sequence.PlayLengthSeconds = 1.0;
	Sequence.bRevisionComplete = true;
	FHyperAIAnimationElementView CurveA;
	CurveA.Kind = TEXT("curve.float");
	CurveA.Name = TEXT("Speed");
	CurveA.StableId = TEXT("curve.float:/Game/Animation/A.A:Speed");
	FHyperAIAnimationElementView Notify;
	Notify.Kind = TEXT("notify");
	Notify.Name = TEXT("Footstep");
	Notify.StableId = TEXT("notify:/Game/Animation/A.A:1");
	Notify.TimeSeconds = 0.5;
	Notify.DurationSeconds = 0.0;
	Sequence.Elements = {Notify, CurveA};

	FHyperAIStudioAnimationValueSnapshot A;
	A.ScopeFingerprint = Hash(TEXT("scope"));
	A.Records = {Sequence};
	FHyperAIStudioAnimationValueSnapshot B = A;
	B.Records[0].Elements.Swap(0, 1);
	const FString RevisionA = FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(A);
	const FString RevisionB = FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(B);
	TestEqual(TEXT("Revision ignores nested ordering"), RevisionA, RevisionB);
	TestTrue(TEXT("Revision is canonical"),
		FHyperAIStudioAnimationContracts::IsCanonicalSha256(RevisionA));
	FHyperAIStudioAnimationValueSnapshot Oversized;
	Oversized.ScopeFingerprint = Hash(TEXT("oversized-scope"));
	for (int32 Index = 0; Index < 1300; ++Index)
	{
		FHyperAIAnimationAssetRecord Record;
		Record.Variant = TEXT("animation_sequence");
		Record.AssetPath = TEXT("/Game/") + FString::ChrN(900, TEXT('A'))
			+ FString::FromInt(Index);
		Record.StableId = TEXT("record:") + FString::FromInt(Index);
		Record.bRevisionComplete = true;
		Oversized.Records.Add(MoveTemp(Record));
	}
	const FString OversizedRevision =
		FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(Oversized);
	TestTrue(TEXT("Aggregate hash overflow fails revision completeness closed"),
		OversizedRevision.IsEmpty() && !Oversized.bComplete);
	FHyperAIStudioAnimationValueSnapshot NonFinite;
	NonFinite.ScopeFingerprint = Hash(TEXT("non-finite-scope"));
	FHyperAIAnimationAssetRecord NonFiniteRecord;
	NonFiniteRecord.Variant = TEXT("animation_sequence");
	NonFiniteRecord.AssetPath = TEXT("/Game/Animation/NonFinite.NonFinite");
	NonFiniteRecord.StableId = TEXT("animation_sequence:/Game/Animation/NonFinite.NonFinite");
	NonFiniteRecord.SkeletonPath = TEXT("/Game/Animation/SK.SK");
	NonFiniteRecord.PlayLengthSeconds = std::numeric_limits<double>::quiet_NaN();
	NonFiniteRecord.bRevisionComplete = true;
	NonFinite.Records.Add(NonFiniteRecord);
	FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(NonFinite);
	TestFalse(TEXT("Non-finite captured state fails CAS completeness closed"), NonFinite.bComplete);
	bool bNonFiniteTruncated = false;
	const auto NonFiniteIssues = FHyperAIStudioAnimationContracts::ValidateValueSnapshot(
		NonFinite, false, 16, bNonFiniteTruncated);
	TestTrue(TEXT("Independent validator reports non-finite state"),
		NonFiniteIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("non_finite_asset_numeric");
		}));
	FHyperAIStudioAnimationValueSnapshot PathBoundSnapshot;
	FString PathBoundStatus;
	FString PathBoundDiagnostic;
	TestFalse(TEXT("Oversized paths fail before the capture copies or sorts them"),
		FHyperAIStudioAnimationFacade::CaptureLoaded(
			{FString::ChrN(FHyperAIStudioAnimationContracts::MaxPathCharacters + 1, TEXT('A'))},
			TEXT("all"), false, PathBoundSnapshot, PathBoundStatus, PathBoundDiagnostic, 100));
	TestEqual(TEXT("Oversized Animation path has a stable rejection"), PathBoundStatus,
		FString(TEXT("invalid_asset_path")));

	FHyperAIStudioAnimationValueSnapshot Invalid;
	Invalid.ScopeFingerprint = Hash(TEXT("invalid-scope"));
	FHyperAIAnimationAssetRecord Blueprint;
	Blueprint.Variant = TEXT("animation_blueprint");
	Blueprint.StableId = TEXT("animation_blueprint:/Game/Animation/ABP.ABP");
	Blueprint.AssetPath = TEXT("/Game/Animation/ABP.ABP");
	Blueprint.bRevisionComplete = true;
	Blueprint.BlueprintStatus = TEXT("error");
	FHyperAIAnimationElementView BrokenTransition;
	BrokenTransition.Kind = TEXT("transition");
	BrokenTransition.StableId = TEXT("transition:broken");
	Blueprint.Elements.Add(BrokenTransition);
	Invalid.Records.Add(Blueprint);
	FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(Invalid);
	bool bTruncated = false;
	const auto Issues = FHyperAIStudioAnimationContracts::ValidateValueSnapshot(
		Invalid, false, 128, bTruncated);
	TestTrue(TEXT("Validator finds missing non-template target Skeleton"),
		Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("missing_target_skeleton");
		}));
	TestTrue(TEXT("Validator finds Blueprint compile failure"),
		Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("blueprint_compile_error");
		}));
	TestTrue(TEXT("Validator independently finds broken transition endpoints"),
		Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("broken_transition_endpoint");
		}));
	TestFalse(TEXT("Bounded fixture did not truncate"), bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimationAdapterAndStageTest,
	"HyperAIStudio.NativeTools.AnimationRigging.AdapterAndIdempotentStage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAnimationAdapterAndStageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Animation::Tests;
	FHyperAIStudioAnimationStagingService::Reset();
	const FString Base = Hash(TEXT("base"));
	const FString TargetRevision = Hash(TEXT("target"));
	const auto Payload = MakePayload(Base, TEXT("/Game/Animation/ABP_Test.ABP_Test"),
		TargetRevision, TEXT("UpperBody"));
	const auto Detached = Payload->CloneImmutable();
	TestTrue(TEXT("Immutable clone is detached"), &Detached.Get() != &Payload.Get());
	TestEqual(TEXT("Clone preserves semantic fingerprint"), Detached->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	const int32 DetachedSize = Detached->GetBoundedByteSize();
	Payload->Operations[0].Name = TEXT("A_Much_Longer_Mutable_Source_Layer_Name");
	TestEqual(TEXT("Deep clone does not alias nested operation strings"),
		Detached->GetBoundedByteSize(), DetachedSize);
	Payload->Operations[0].Name = TEXT("UpperBody");

	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioAnimationContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_animation_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioAnimationContracts::MutationVariantId;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FHyperAIStudioAnimationDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Blocked = Adapter.Execute(Context, Payload.Get());
	TestEqual(TEXT("Adapter fails with exact staged-only status"), Blocked.StatusCode,
		FString(TEXT("staged_backend_required")));
	TestEqual(TEXT("Blocked adapter reports no known effect"), static_cast<uint8>(Blocked.Outcome),
		static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect));
	TestFalse(TEXT("Blocked adapter returns no false result payload"), Blocked.Payload.IsValid());
	Context.Binding.PackId = TEXT("animation");
	const FHyperAIStudioDomainAdapterResult Mismatch = Adapter.Execute(Context, Payload.Get());
	TestEqual(TEXT("Legacy/wrong pack id fails exact binding"), Mismatch.StatusCode,
		FString(TEXT("typed_binding_mismatch")));

	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("Shared typed executor seals the Animation artifact"),
		PrepareArtifact(Payload, Prepared, Error));
	FHyperAIStudioAnimationStagedArtifact Artifact;
	Artifact.Prepared = Prepared;
	Artifact.Payload = Payload;
	Artifact.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Artifact.OperationId = TEXT("animation-stage-001");
	Artifact.StageId = FHyperAIStudioAnimationContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	Artifact.ExpiresMonotonicMs = static_cast<int64>(FPlatformTime::Seconds() * 1000.0)
		+ FHyperAIStudioAnimationContracts::StageLifetimeMs;
	bool bReplay = false;
	TestTrue(TEXT("First exact stage succeeds"),
		FHyperAIStudioAnimationStagingService::Stage(Artifact, bReplay, Error));
	TestFalse(TEXT("First stage is not replay"), bReplay);
	TestTrue(TEXT("Exact replay is idempotent"),
		FHyperAIStudioAnimationStagingService::Stage(Artifact, bReplay, Error));
	TestTrue(TEXT("Replay is identified"), bReplay);
	TestEqual(TEXT("One exact artifact remains staged"),
		FHyperAIStudioAnimationStagingService::NumStaged(), 1);
	FHyperAIStudioAnimationStagedArtifact KindForged = Artifact;
	KindForged.OperationId = TEXT("animation-stage-kind-forged-001");
	const auto KindForgedPayload = MakePayload(Base,
		TEXT("/Game/Animation/ABP_Test.ABP_Test"), TargetRevision, TEXT("UpperBody"));
	KindForgedPayload->Operations[0].Kind =
		EHyperAIStudioAnimationOperationKind::MontageAddSection;
	KindForged.Payload = KindForgedPayload;
	KindForged.StageId = FHyperAIStudioAnimationContracts::ComputeStageId(
		KindForged.CanonicalProjectId, KindForged.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	TestFalse(TEXT("Concrete stage rejects Type-to-Kind semantic drift"),
		FHyperAIStudioAnimationStagingService::Stage(KindForged, bReplay, Error));

	FHyperAIStudioAnimationStagedArtifact Forged = Artifact;
	Forged.OperationId = TEXT("animation-stage-forged-001");
	const auto ForgedPayload = MakePayload(Base, TEXT("/Game/Animation/ABP_Test.ABP_Test"),
		TargetRevision, TEXT("Forged"));
	ForgedPayload->SemanticFingerprint = Hash(TEXT("caller-claimed-semantic"));
	Forged.Payload = ForgedPayload;
	Forged.StageId = FHyperAIStudioAnimationContracts::ComputeStageId(
		Forged.CanonicalProjectId, Forged.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	TestFalse(TEXT("Concrete stage rejects a forged semantic fingerprint"),
		FHyperAIStudioAnimationStagingService::Stage(Forged, bReplay, Error));
	FHyperAIStudioAnimationStagingService::Reset();
	return true;
}

#endif
