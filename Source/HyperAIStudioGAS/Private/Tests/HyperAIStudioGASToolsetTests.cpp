// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioGASToolset.h"

#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::GAS::Tests
{
	FString UniqueName(const TCHAR* Prefix)
	{
		return FString(Prefix) + TEXT("_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	UBlueprint* CreateBlueprintFixture(
		UClass* ParentClass,
		const TCHAR* Prefix,
		FString& OutPath)
	{
		const FString Name = UniqueName(Prefix);
		UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioAutomation/") + Name));
		if (!Package) return nullptr;
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*Name), BPTYPE_Normal,
			FName(TEXT("HyperAIStudioGASAutomation")));
		if (Blueprint)
		{
			OutPath = Blueprint->GetPathName();
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		return Blueprint;
	}

	UBlueprint* CreateEffectBlueprint(FString& OutPath)
	{
		return CreateBlueprintFixture(
			UGameplayEffect::StaticClass(), TEXT("GE_HyperAIGASTest"), OutPath);
	}

	void DiscardFixture(UBlueprint* Blueprint)
	{
		if (!Blueprint) return;
		if (UPackage* Package = Blueprint->GetOutermost()) Package->SetDirtyFlag(false);
	}

	bool CaptureOne(
		const FString& Path,
		FHyperAIGASAssetRecord& OutRecord,
		FString* OutSnapshotRevision = nullptr)
	{
		FHyperAIGASInspectRequest Request;
		Request.AssetPaths.Add(Path);
		Request.PageSize = 1;
		Request.MaxOutputBytes = FHyperAIStudioGASContracts::MaxOutputBytes;
		const FHyperAIGASInspectReport Report =
			UHyperAIStudioGASToolset::hyper_gas_inspect(Request);
		if (!Report.bOk || Report.Records.Num() != 1) return false;
		OutRecord = Report.Records[0];
		if (OutSnapshotRevision) *OutSnapshotRevision = Report.Revision;
		return true;
	}

	FHyperAIGASPlanOperation DurationOperation(
		const FString& Path,
		const FString& Revision,
		const double Seconds)
	{
		FHyperAIGASPlanOperation Operation;
		Operation.Type = TEXT("set_effect_duration");
		Operation.TargetPath = Path;
		Operation.ExpectedRevision = Revision;
		Operation.DurationPolicy = TEXT("duration");
		Operation.DurationSeconds = Seconds;
		Operation.MaxDurationSeconds = 0.0;
		Operation.PeriodSeconds = 0.0;
		return Operation;
	}

	FHyperAIGASApplyPlanRequest DurationRequest(
		const FString& Path,
		const FString& Revision,
		const double Seconds)
	{
		FHyperAIGASApplyPlanRequest Request;
		Request.bDryRun = true;
		Request.DeadlineMs = 1000;
		Request.MaxGameThreadMs = 250;
		Request.Operations.Add(DurationOperation(Path, Revision, Seconds));
		return Request;
	}

	bool MakeTypedPayload(
		const FHyperAIGASApplyPlanRequest& Request,
		FHyperAIStudioGASTypedPayload& OutPayload,
		FHyperAIGASApplyPlanReport& OutDryRun)
	{
		OutPayload.Operations.Reset();
		OutPayload.BaseRevision.Reset();
		OutPayload.SemanticFingerprint.Reset();
		OutDryRun = UHyperAIStudioGASToolset::hyper_gas_apply_plan(Request);
	if (!OutDryRun.bOk || OutDryRun.Status != TEXT("valid_dry_run_execution_blocked")) return false;
		for (const FHyperAIGASPlanOperation& Operation : Request.Operations)
		{
			FHyperAIStudioGASBackendOperation Backend;
			FString Code;
			FString Error;
			if (!FHyperAIStudioGASContracts::ValidateOperationShape(
				Operation, Backend, Code, Error)) return false;
			OutPayload.Operations.Add(MoveTemp(Backend));
		}
		OutPayload.BaseRevision = OutDryRun.BaseRevision;
		OutPayload.SemanticFingerprint =
			FHyperAIStudioGASContracts::ComputePayloadSemanticFingerprint(
				OutPayload.Operations, OutPayload.BaseRevision);
		return FHyperAIStudioGASContracts::IsCanonicalSha256(OutPayload.SemanticFingerprint);
	}

	FHyperAIStudioDomainDispatchContext ValidContext(
		const FHyperAIStudioGASDomainAdapter& Adapter,
		const FString& PlanHash)
	{
		FHyperAIStudioDomainDispatchContext Context;
		Context.Binding.PackId = FHyperAIStudioGASContracts::PackId;
		Context.Binding.ToolName = TEXT("hyper_gas_apply_plan");
		Context.Binding.VariantId = FHyperAIStudioGASContracts::MutationVariantId;
		Context.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Context.Binding.CanonicalProjectId = TEXT("hyperai-gas-automation-project");
		Context.Binding.ExpectedAdapterFingerprint = Adapter.GetDescriptor().AdapterFingerprint;
		Context.Binding.ExpectedAdapterGeneration = 1;
		Context.Binding.ExpectedRegistryEpoch = 1;
		Context.Safety = EHyperAIStudioDomainSafety::Edit;
		Context.OperationId = TEXT("gas-automation-operation-001");
		Context.PlanHash = PlanHash;
		Context.VerifiedAuthorizationNonce = TEXT("journal-edit-0123456789abcdef");
		Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
		Context.ActionNonce = TEXT("gas-action-0123456789abcdef");
		Context.AdapterGeneration = 1;
		Context.RegistryEpoch = 1;
		return Context;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASManifestAndReflectionTest,
	"HyperAIStudio.NativeTools.GAS.ManifestAndReflection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASManifestAndReflectionTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIStudioGASManifestEntry>& Manifest = FHyperAIStudioGASContracts::GetManifest();
	TestEqual(TEXT("atomic GAS cohort owns exactly three names"), Manifest.Num(), 3);
	TSet<FString> ManifestNames;
	for (const FHyperAIStudioGASManifestEntry& Entry : Manifest)
	{
		ManifestNames.Add(Entry.Name);
		TestEqual(TEXT("every name uses exact script-module qualifier"),
			Entry.QualifiedToolset, FHyperAIStudioGASContracts::GetQualifiedToolsetName());
	}

	TSet<FString> ReflectedNames;
	for (TFieldIterator<UFunction> It(
		UHyperAIStudioGASToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) ReflectedNames.Add(It->GetName());
	}
	TestEqual(TEXT("UHT exposes exactly the owned cohort"), ReflectedNames.Num(), 3);
	TestTrue(TEXT("inspect reflected"), ReflectedNames.Contains(TEXT("hyper_gas_inspect")));
	TestTrue(TEXT("apply reflected"), ReflectedNames.Contains(TEXT("hyper_gas_apply_plan")));
	TestTrue(TEXT("validate reflected"), ReflectedNames.Contains(TEXT("hyper_gas_validate")));
	TestEqual(TEXT("manifest and reflection agree"), ManifestNames.Num(), ReflectedNames.Num());
	TArray<FString> ExactNames = ManifestNames.Array();
	ExactNames.Sort();
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("generated catalog is the sole exact cohort authority"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			FHyperAIStudioGASContracts::PackId,
			FHyperAIStudioGASContracts::AtomicCohortId,
			ExactNames,
			Admission));
	TestTrue(TEXT("generated catalog validated"), Admission.bCatalogValid);
	TestTrue(TEXT("all and only the three GAS names are atomic"), Admission.bExactCohortMatch);
	TestTrue(TEXT("GAS remains optional"), Admission.bOptionalPack);
	TestEqual(TEXT("new implementation remains source-candidate only"), Admission.State,
		EHyperAIStudioExtensionAdmissionState::SourceCandidate);
	TestFalse(TEXT("source candidate is never production-registered"),
		FHyperAIStudioGASContracts::IsRegistrationAllowed(false));
	TestEqual(TEXT("dev registration also requires the explicit command-line flag"),
		FHyperAIStudioGASContracts::IsRegistrationAllowed(true),
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled());
	TestEqual(TEXT("Loaded GAS registration follows generated owner policy"),
		FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGASToolset::StaticClass(),
			FHyperAIStudioGASContracts::GetQualifiedToolsetName()),
		FHyperAIStudioGASContracts::IsRegistrationAllowed(
			FHyperAIStudioGASContracts::IsPendingTestRegistrationEnabled()));

	TestNull(TEXT("apply DTO has no client authorization token"),
		FHyperAIGASApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("apply DTO has no arbitrary script body"),
		FHyperAIGASPlanOperation::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNotNull(TEXT("inspect exposes a hard game-thread budget"),
		FHyperAIGASInspectRequest::StaticStruct()->FindPropertyByName(TEXT("MaxGameThreadMs")));
	TestNotNull(TEXT("validate exposes a hard game-thread budget"),
		FHyperAIGASValidateRequest::StaticStruct()->FindPropertyByName(TEXT("MaxGameThreadMs")));

	FHyperAIStudioGASDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = Adapter.GetDescriptor();
	TestEqual(TEXT("one closed mutation variant"), Descriptor.Variants.Num(), 1);
	TestEqual(TEXT("variant is edit safety"), Descriptor.Variants[0].Safety,
		EHyperAIStudioDomainSafety::Edit);
	TestTrue(TEXT("adapter fingerprint is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(Descriptor.AdapterFingerprint));
	TestEqual(TEXT("payload schema hash is reproducible from canonical fields"),
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			FHyperAIStudioGASContracts::PayloadSchemaCanonical),
		FString(FHyperAIStudioGASContracts::PayloadSchemaFingerprint));
	TestEqual(TEXT("result schema hash is reproducible from canonical fields"),
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			FHyperAIStudioGASContracts::ResultSchemaCanonical),
		FString(FHyperAIStudioGASContracts::ResultSchemaFingerprint));
	FHyperAIStudioGASTypedPayload CloneSource;
	CloneSource.BaseRevision = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	FHyperAIStudioGASBackendOperation CloneOperation;
	CloneOperation.Tags.Add(TEXT("Clone.Source"));
	FHyperAIGASModifierSpec CloneModifier;
	CloneModifier.AttributeName = TEXT("SourceAttribute");
	CloneOperation.Modifiers.Add(CloneModifier);
	FHyperAIGASCueSpec CloneCue;
	CloneCue.CueTags.Add(TEXT("GameplayCue.Clone.Source"));
	CloneOperation.Cues.Add(CloneCue);
	CloneSource.Operations.Add(CloneOperation);
	CloneSource.SemanticFingerprint =
		FHyperAIStudioGASContracts::ComputePayloadSemanticFingerprint(
			CloneSource.Operations, CloneSource.BaseRevision);
	const FString OriginalSemanticFingerprint = CloneSource.SemanticFingerprint;
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		CloneSource.CloneImmutable();
	TestTrue(TEXT("staging clone is a detached object"),
		&Clone.Get() != static_cast<const IHyperAIStudioTypedArtifactPayload*>(&CloneSource));
	CloneSource.Operations[0].Tags[0] = TEXT("Clone.Mutated");
	CloneSource.Operations[0].Modifiers[0].AttributeName = TEXT("MutatedAttribute");
	CloneSource.Operations[0].Cues[0].CueTags[0] = TEXT("GameplayCue.Clone.Mutated");
	TestEqual(TEXT("detached clone retains immutable semantic value"),
		Clone->GetSemanticFingerprint(), OriginalSemanticFingerprint);
	TestTrue(TEXT("mutated source fails recomputed semantic identity closed"),
		CloneSource.GetSemanticFingerprint().IsEmpty());
	const FHyperAIStudioGASTypedPayload& ConcreteClone =
		static_cast<const FHyperAIStudioGASTypedPayload&>(Clone.Get());
	TestEqual(TEXT("operation tag array is deeply detached"), ConcreteClone.Operations[0].Tags[0],
		FString(TEXT("Clone.Source")));
	TestEqual(TEXT("modifier array is deeply detached"),
		ConcreteClone.Operations[0].Modifiers[0].AttributeName,
		FString(TEXT("SourceAttribute")));
	TestEqual(TEXT("cue tag array is deeply detached"),
		ConcreteClone.Operations[0].Cues[0].CueTags[0],
		FString(TEXT("GameplayCue.Clone.Source")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASEpicDelegationTest,
	"HyperAIStudio.NativeTools.GAS.EpicDelegation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASEpicDelegationTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIGASEpicDelegate>& Delegates = FHyperAIStudioGASContracts::GetEpicDelegates();
	TestTrue(TEXT("cue creation remains delegated"), Delegates.ContainsByPredicate(
		[](const FHyperAIGASEpicDelegate& Item) { return Item.Capability == TEXT("create_cue_notify_asset"); }));
	TestTrue(TEXT("cue tag creation remains delegated"), Delegates.ContainsByPredicate(
		[](const FHyperAIGASEpicDelegate& Item) { return Item.Capability == TEXT("add_cue_tag"); }));
	TestTrue(TEXT("runtime cue execution is external-effect delegation"), Delegates.ContainsByPredicate(
		[](const FHyperAIGASEpicDelegate& Item)
		{
			return Item.Capability == TEXT("execute_cue_on_selected_actor")
				&& Item.Safety == TEXT("external_effect");
		}));

	FHyperAIGASPlanOperation RuntimeCue;
	RuntimeCue.Type = TEXT("execute_cue_on_selected_actor");
	RuntimeCue.TargetPath = TEXT("/Game/Test/Test.Test");
	FHyperAIStudioGASBackendOperation Backend;
	FString Code;
	FString Error;
	TestFalse(TEXT("delegated runtime cue cannot enter the editor plan"),
		FHyperAIStudioGASContracts::ValidateOperationShape(RuntimeCue, Backend, Code, Error));
	TestEqual(TEXT("closed variants identify unknown operation"), Code,
		FString(TEXT("unknown_operation_type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASOperationShapeTest,
	"HyperAIStudio.NativeTools.GAS.OperationShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASOperationShapeTest::RunTest(const FString& Parameters)
{
	const FString Revision = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	FHyperAIGASPlanOperation Duration = HyperAIStudio::GAS::Tests::DurationOperation(
		TEXT("/Game/Test/GE_Test.GE_Test"), Revision, 3.0);
	FHyperAIStudioGASBackendOperation Backend;
	FString Code;
	FString Error;
	TestTrue(TEXT("closed duration slice is supported"),
		FHyperAIStudioGASContracts::ValidateOperationShape(Duration, Backend, Code, Error));
	TestTrue(TEXT("duration has a trusted backend"), Backend.bBackendSupported);
	FHyperAIGASPlanOperation OverlongDiscriminant = Duration;
	OverlongDiscriminant.Type.Reset(65);
	for (int32 Index = 0; Index < 65; ++Index)
	{
		OverlongDiscriminant.Type.AppendChar(TEXT('x'));
	}
	TestFalse(TEXT("operation discriminant is bounded before backend DTO copying"),
		FHyperAIStudioGASContracts::ValidateOperationShape(
			OverlongDiscriminant, Backend, Code, Error));
	TestEqual(TEXT("operation envelope reports a stable bounds code"), Code,
		FString(TEXT("invalid_operation_bounds")));

	Duration.Tags.Add(TEXT("State.InvalidUnusedField"));
	TestFalse(TEXT("unused union fields fail closed"),
		FHyperAIStudioGASContracts::ValidateOperationShape(Duration, Backend, Code, Error));

	FHyperAIGASPlanOperation Division;
	Division.Type = TEXT("replace_effect_modifiers");
	Division.TargetPath = TEXT("/Game/Test/GE_Modifier.GE_Modifier");
	Division.ExpectedRevision = Revision;
	FHyperAIGASModifierSpec DivisionSpec;
	DivisionSpec.AttributeOwnerClassPath = TEXT("/Script/GameplayAbilities.AttributeSet");
	DivisionSpec.AttributeName = TEXT("Example");
	DivisionSpec.Operation = TEXT("division");
	DivisionSpec.Magnitude = 0.0;
	Division.Modifiers.Add(DivisionSpec);
	TestFalse(TEXT("division by zero fails before attribute dispatch"),
		FHyperAIStudioGASContracts::ValidateOperationShape(Division, Backend, Code, Error));

	FHyperAIGASPlanOperation Cue;
	Cue.Type = TEXT("replace_effect_cues");
	Cue.TargetPath = TEXT("/Game/Test/GE_Cue.GE_Cue");
	Cue.ExpectedRevision = Revision;
	FHyperAIGASCueSpec CueSpec;
	CueSpec.CueTags.Add(TEXT("GameplayCue.HyperAIStudio.")
		+ HyperAIStudio::GAS::Tests::UniqueName(TEXT("Unregistered")));
	Cue.Cues.Add(CueSpec);
	TestFalse(TEXT("cue references never create an unregistered tag"),
		FHyperAIStudioGASContracts::ValidateOperationShape(Cue, Backend, Code, Error));

	FHyperAIGASPlanOperation Create;
	Create.Type = TEXT("create_gameplay_effect_blueprint");
	Create.TargetPath = TEXT("/Game/Test/GE_Create.GE_Create");
	Create.ParentClassPath = TEXT("/Script/GameplayAbilities.GameplayEffect");
	TestTrue(TEXT("creation is recognized for explicit capability reporting"),
		FHyperAIStudioGASContracts::ValidateOperationShape(Create, Backend, Code, Error));
	TestFalse(TEXT("creation stays backend unavailable"), Backend.bBackendSupported);

	FHyperAIGASPlanOperation EffectTags;
	EffectTags.Type = TEXT("replace_effect_asset_tags");
	EffectTags.TargetPath = TEXT("/Game/Test/GE_Tags.GE_Tags");
	EffectTags.ExpectedRevision = Revision;
	TestTrue(TEXT("effect-tag replacement is recognized"),
		FHyperAIStudioGASContracts::ValidateOperationShape(EffectTags, Backend, Code, Error));
	TestFalse(TEXT("component inheritance semantics fail closed"), Backend.bBackendSupported);

	const FString EmbeddedNullPath = FString::ConstructFromPtrSize(
		TEXT("/Game/Test/GE.GE\0tail"), 21);
	TestFalse(TEXT("embedded null path is rejected"),
		FHyperAIStudioGASContracts::IsCanonicalProjectObjectPath(EmbeddedNullPath));

	FHyperAIGASApplyPlanRequest Oversized;
	Oversized.Operations.SetNum(FHyperAIStudioGASContracts::MaxOperations + 1);
	TestEqual(TEXT("operation bound fails before any target lookup"),
		UHyperAIStudioGASToolset::hyper_gas_apply_plan(Oversized).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGASInspectRequest OversizedInspect;
	OversizedInspect.AssetPaths.SetNum(FHyperAIStudioGASContracts::MaxAssetPaths + 1);
	TestEqual(TEXT("inspect path bound fails before resolution"),
		UHyperAIStudioGASToolset::hyper_gas_inspect(OversizedInspect).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGASInspectRequest InvalidInspectBudget;
	InvalidInspectBudget.MaxGameThreadMs = 0;
	TestEqual(TEXT("inspect time bound fails before resolution"),
		UHyperAIStudioGASToolset::hyper_gas_inspect(InvalidInspectBudget).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGASValidateRequest OversizedValidate;
	OversizedValidate.MaxIssues = FHyperAIStudioGASContracts::MaxIssues + 1;
	TestEqual(TEXT("validate issue bound fails before capture"),
		UHyperAIStudioGASToolset::hyper_gas_validate(OversizedValidate).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGASValidateRequest InvalidValidateBudget;
	InvalidValidateBudget.MaxGameThreadMs =
		FHyperAIStudioGASContracts::MaxReadGameThreadMs + 1;
	TestEqual(TEXT("validate time bound fails before capture"),
		UHyperAIStudioGASToolset::hyper_gas_validate(InvalidValidateBudget).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGASValidateRequest StaleValidation;
	StaleValidation.bRequireFreshCapture = false;
	TestEqual(TEXT("validator never accepts stale planner evidence"),
		UHyperAIStudioGASToolset::hyper_gas_validate(StaleValidation).Status,
		FString(TEXT("fresh_capture_required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASSnapshotAndValidatorTest,
	"HyperAIStudio.NativeTools.GAS.SnapshotAndValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASSnapshotAndValidatorTest::RunTest(const FString& Parameters)
{
	FHyperAIGASAssetRecord A;
	A.Variant = TEXT("gameplay_effect_blueprint");
	A.AssetPath = TEXT("/Game/Test/GE_A.GE_A");
	A.StableId = TEXT("gas:gameplay_effect_blueprint:/Game/Test/GE_A.GE_A");
	A.bRevisionComplete = true;
	A.DurationPolicy = TEXT("duration");
	A.bHasStaticDuration = true;
	A.DurationSeconds = 2.0;
	A.bHasStaticMaxDuration = true;
	A.MaxDurationSeconds = 0.0;
	A.PeriodSeconds = 0.0;
	FHyperAIGASAssetRecord B = A;
	B.AssetPath = TEXT("/Game/Test/GE_B.GE_B");
	B.StableId = TEXT("gas:gameplay_effect_blueprint:/Game/Test/GE_B.GE_B");

	FHyperAIStudioGASValueSnapshot First;
	First.bComplete = true;
	First.Records = {B, A};
	const FString FirstRevision = FHyperAIStudioGASContracts::ComputeSnapshotRevision(First);
	FHyperAIStudioGASValueSnapshot Second;
	Second.bComplete = true;
	Second.Records = {A, B};
	const FString SecondRevision = FHyperAIStudioGASContracts::ComputeSnapshotRevision(Second);
	TestEqual(TEXT("snapshot revision ignores enumeration order"), FirstRevision, SecondRevision);
	TestTrue(TEXT("snapshot revision is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(FirstRevision));
	FHyperAIStudioGASValueSnapshot Incomplete = First;
	Incomplete.bComplete = false;
	TestNotEqual(TEXT("snapshot completeness participates in canonical identity"),
		FHyperAIStudioGASContracts::ComputeSnapshotRevision(Incomplete), FirstRevision);
	FHyperAIStudioGASValueSnapshot UnsupportedComponent;
	UnsupportedComponent.bComplete = true;
	FHyperAIGASAssetRecord ComponentRecord = A;
	ComponentRecord.ComponentCount = 1;
	UnsupportedComponent.Records = {ComponentRecord};
	FHyperAIStudioGASContracts::ComputeSnapshotRevision(UnsupportedComponent);
	bool bUnsupportedTruncated = false;
	const TArray<FHyperAIGASIssue> UnsupportedIssues =
		FHyperAIStudioGASContracts::ValidateValueSnapshot(
			UnsupportedComponent, false, FHyperAIStudioGASContracts::MaxIssues,
			bUnsupportedTruncated);
	TestFalse(TEXT("unsupported component validation is not truncated"), bUnsupportedTruncated);
	TestTrue(TEXT("component count alone never certifies component semantics"),
		UnsupportedIssues.ContainsByPredicate([](const FHyperAIGASIssue& Issue)
		{
			return Issue.Code == TEXT("component_projection_unsupported");
		}));
	FHyperAIGASModifierView Additive;
	Additive.AttributeOwnerClassPath = TEXT("/Script/Test.Attributes");
	Additive.AttributeName = TEXT("Health");
	Additive.Operation = TEXT("additive");
	Additive.MagnitudeKind = TEXT("scalable_float");
	Additive.bHasStaticMagnitude = true;
	Additive.StaticMagnitude = 1.0;
	FHyperAIGASModifierView Override = Additive;
	Override.Operation = TEXT("override");
	Override.StaticMagnitude = 5.0;
	FHyperAIStudioGASValueSnapshot ModifierOrderA;
	ModifierOrderA.bComplete = true;
	A.Modifiers = {Additive, Override};
	ModifierOrderA.Records = {A};
	const FString ModifierOrderARevision =
		FHyperAIStudioGASContracts::ComputeSnapshotRevision(ModifierOrderA);
	FHyperAIStudioGASValueSnapshot ModifierOrderB;
	ModifierOrderB.bComplete = true;
	A.Modifiers = {Override, Additive};
	ModifierOrderB.Records = {A};
	TestNotEqual(TEXT("semantically significant modifier order changes revision"),
		ModifierOrderARevision,
		FHyperAIStudioGASContracts::ComputeSnapshotRevision(ModifierOrderB));

	Second.Records[0].DurationSeconds = -1.0;
	FHyperAIStudioGASContracts::ComputeSnapshotRevision(Second);
	bool bTruncated = false;
	const TArray<FHyperAIGASIssue> Issues = FHyperAIStudioGASContracts::ValidateValueSnapshot(
		Second, false, FHyperAIStudioGASContracts::MaxIssues, bTruncated);
	TestTrue(TEXT("independent validator catches invalid duration"), Issues.ContainsByPredicate(
		[](const FHyperAIGASIssue& Issue) { return Issue.Code == TEXT("duration_invalid"); }));
	TestFalse(TEXT("small evidence does not truncate"), bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASLoadedOnlyInspectTest,
	"HyperAIStudio.NativeTools.GAS.LoadedOnlyInspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASLoadedOnlyInspectTest::RunTest(const FString& Parameters)
{
	const FString Name = HyperAIStudio::GAS::Tests::UniqueName(TEXT("GE_NeverLoaded"));
	const FString Path = TEXT("/Game/__HyperAIStudioAutomation/") + Name + TEXT(".") + Name;
	FHyperAIGASInspectRequest Request;
	Request.AssetPaths.Add(Path);
	const FHyperAIGASInspectReport Report = UHyperAIStudioGASToolset::hyper_gas_inspect(Request);
	TestTrue(TEXT("loaded-only partial evidence still has a stable report"), Report.bOk);
	TestEqual(TEXT("missing asset is not loaded"), Report.Records.Num(), 0);
	TestFalse(TEXT("missing requested evidence is incomplete"), Report.bRevisionComplete);
	TestTrue(TEXT("explicit no-load issue is returned"), Report.Issues.ContainsByPredicate(
		[](const FHyperAIGASIssue& Issue) { return Issue.Code == TEXT("asset_not_loaded"); }));
	TestNull(TEXT("inspect did not synchronously load the object"),
		FSoftObjectPath(Path).ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASLoadedVariantsTest,
	"HyperAIStudio.NativeTools.GAS.LoadedVariants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASLoadedVariantsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GAS::Tests;
	TArray<UBlueprint*> Fixtures;
	TArray<FString> Paths;
	FString Path;
	Fixtures.Add(CreateBlueprintFixture(
		UGameplayAbility::StaticClass(), TEXT("GA_HyperAIGASTest"), Path));
	Paths.Add(Path);
	Path.Reset();
	Fixtures.Add(CreateEffectBlueprint(Path));
	Paths.Add(Path);
	Path.Reset();
	Fixtures.Add(CreateBlueprintFixture(
		UAttributeSet::StaticClass(), TEXT("AS_HyperAIGASTest"), Path));
	Paths.Add(Path);
	ON_SCOPE_EXIT
	{
		for (UBlueprint* Fixture : Fixtures) DiscardFixture(Fixture);
	};
	for (UBlueprint* Fixture : Fixtures)
	{
		TestNotNull(TEXT("variant fixture created"), Fixture);
		if (!Fixture) return false;
	}

	FHyperAIGASInspectRequest Request;
	Request.AssetPaths = Paths;
	Request.PageSize = 3;
	Request.MaxOutputBytes = FHyperAIStudioGASContracts::MaxOutputBytes;
	const FHyperAIGASInspectReport Report =
		UHyperAIStudioGASToolset::hyper_gas_inspect(Request);
	TestTrue(TEXT("all loaded GAS asset variants inspect"), Report.bOk);
	TestTrue(TEXT("exact loaded variant snapshot is complete"), Report.bRevisionComplete);
	TestEqual(TEXT("one Ability Blueprint"), Report.AbilityCount, 1);
	TestEqual(TEXT("one GameplayEffect Blueprint"), Report.EffectCount, 1);
	TestEqual(TEXT("one AttributeSet Blueprint"), Report.AttributeSetCount, 1);
	TestTrue(TEXT("ability policies are projected"), Report.Records.ContainsByPredicate(
		[](const FHyperAIGASAssetRecord& Record)
		{
			return Record.Variant == TEXT("gameplay_ability_blueprint")
				&& !Record.InstancingPolicy.IsEmpty() && !Record.NetExecutionPolicy.IsEmpty()
				&& Record.bMutationCasEligible;
		}));
	TestTrue(TEXT("effect defaults are projected"), Report.Records.ContainsByPredicate(
		[](const FHyperAIGASAssetRecord& Record)
		{
			return Record.Variant == TEXT("gameplay_effect_blueprint")
				&& !Record.DurationPolicy.IsEmpty() && Record.bMutationCasEligible;
		}));
	TestTrue(TEXT("AttributeSet class identity is projected"), Report.Records.ContainsByPredicate(
		[](const FHyperAIGASAssetRecord& Record)
		{
			return Record.Variant == TEXT("attribute_set_blueprint")
				&& !Record.GeneratedClassPath.IsEmpty();
		}));

	Request.PageSize = 1;
	Request.Cursor.Reset();
	const FHyperAIGASInspectReport FirstPage =
		UHyperAIStudioGASToolset::hyper_gas_inspect(Request);
	TestEqual(TEXT("bounded projection returns exactly one first-page record"),
		FirstPage.ReturnedRecords, 1);
	TestTrue(TEXT("first page returns one revision-bound cursor"),
		!FirstPage.NextCursor.IsEmpty());
	Request.Cursor = FirstPage.NextCursor;
	const FHyperAIGASInspectReport SecondPage =
		UHyperAIStudioGASToolset::hyper_gas_inspect(Request);
	TestTrue(TEXT("canonical cursor continues the exact projection"), SecondPage.bOk);
	TestEqual(TEXT("second page remains bounded"), SecondPage.ReturnedRecords, 1);
	if (FirstPage.Records.Num() == 1 && SecondPage.Records.Num() == 1)
	{
		TestNotEqual(TEXT("cursor does not repeat a prior stable record"),
			FirstPage.Records[0].StableId, SecondPage.Records[0].StableId);
	}
	Request.Cursor = TEXT("gas-v1|") + FirstPage.Revision + TEXT("|01");
	TestEqual(TEXT("non-canonical numeric cursor is rejected"),
		UHyperAIStudioGASToolset::hyper_gas_inspect(Request).Status,
		FString(TEXT("stale_or_invalid_cursor")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASApplyCallableGateTest,
	"HyperAIStudio.NativeTools.GAS.ApplyCallableGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASApplyCallableGateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GAS::Tests;
	FString Path;
	UBlueprint* Blueprint = CreateEffectBlueprint(Path);
	TestNotNull(TEXT("effect fixture created"), Blueprint);
	if (!Blueprint) return false;
	ON_SCOPE_EXIT { DiscardFixture(Blueprint); };
	FHyperAIGASAssetRecord Before;
	TestTrue(TEXT("fixture is independently captured"), CaptureOne(Path, Before));
	UGameplayEffect* Effect = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject(false));
	TestNotNull(TEXT("fixture has GameplayEffect CDO"), Effect);
	if (!Effect) return false;
	const EGameplayEffectDurationType OriginalPolicy = Effect->DurationPolicy;

	FHyperAIGASApplyPlanRequest Request = DurationRequest(Path, Before.Revision, 7.0);
	const FHyperAIGASApplyPlanReport DryRun =
		UHyperAIStudioGASToolset::hyper_gas_apply_plan(Request);
	TestTrue(TEXT("dry run is useful and valid"), DryRun.bOk);
	TestEqual(TEXT("dry run status"), DryRun.Status,
		FString(TEXT("valid_dry_run_execution_blocked")));
	TestFalse(TEXT("no transaction backend is claimed"), DryRun.Effects.bTransactionOnce);
	TestFalse(TEXT("no bounded compile backend is claimed"), DryRun.Effects.bCompileOnce);
	TestFalse(TEXT("no bounded save backend is claimed"), DryRun.Effects.bSaveOnce);
	TestFalse(TEXT("no executable validation backend is claimed"), DryRun.Effects.bValidateOnce);
	TestFalse(TEXT("no executable fresh verifier is claimed"), DryRun.Effects.bFreshVerifyOnce);
	TestEqual(TEXT("dry run advertises zero executable native operations"),
		DryRun.NativeOperationCount, 0);
	TestTrue(TEXT("shared typed plan hash is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(DryRun.PlanHash));
	TestTrue(TEXT("shared edit authorization hash is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(DryRun.AuthorizationPlanHash));
	TestTrue(TEXT("shared capability hash is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(DryRun.CapabilityHash));
	TestTrue(TEXT("shared effect fingerprint is canonical"),
		FHyperAIStudioGASContracts::IsCanonicalSha256(DryRun.EffectFingerprint));

	Request.bDryRun = false;
	Request.OperationId = TEXT("gas-callable-gate-001");
	Request.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAIGASApplyPlanReport ExecuteAttempt =
		UHyperAIStudioGASToolset::hyper_gas_apply_plan(Request);
	TestFalse(TEXT("callable cannot mutate synchronously"), ExecuteAttempt.bOk);
	TestFalse(TEXT("no artifact was falsely staged"), ExecuteAttempt.bStaged);
	TestFalse(TEXT("no execution was falsely submitted"), ExecuteAttempt.bExecutionSubmitted);
	TestFalse(TEXT("failure never permits fallback"), ExecuteAttempt.bFallbackPermitted);
	TestEqual(TEXT("bounded backend requirement is explicit"), ExecuteAttempt.Status,
		FString(FHyperAIStudioGASContracts::ExecutionBlocker));
	TestEqual(TEXT("execution attempt reseals the identical shared plan"),
		ExecuteAttempt.PlanHash, DryRun.PlanHash);
	TestEqual(TEXT("execution attempt retains the shared capability identity"),
		ExecuteAttempt.CapabilityHash, DryRun.CapabilityHash);
	TestEqual(TEXT("CDO remains unchanged"), Effect->DurationPolicy, OriginalPolicy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIGASTrustedAdapterSafetyTest,
	"HyperAIStudio.NativeTools.GAS.TrustedAdapterSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIGASTrustedAdapterSafetyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::GAS::Tests;
	FString Path;
	UBlueprint* Blueprint = CreateEffectBlueprint(Path);
	TestNotNull(TEXT("effect fixture created"), Blueprint);
	if (!Blueprint) return false;
	ON_SCOPE_EXIT { DiscardFixture(Blueprint); };
	FHyperAIGASAssetRecord Before;
	FString BeforeSnapshotRevision;
	TestTrue(TEXT("fixture captured"), CaptureOne(Path, Before, &BeforeSnapshotRevision));
	UGameplayEffect* Effect = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject(false));
	TestNotNull(TEXT("effect CDO exists"), Effect);
	if (!Effect) return false;

	const EGameplayEffectDurationType OriginalPolicy = Effect->DurationPolicy;
	FHyperAIGASApplyPlanRequest Request = DurationRequest(Path, Before.Revision, 5.0);
	FHyperAIStudioGASTypedPayload Payload;
	FHyperAIGASApplyPlanReport DryRun;
	TestTrue(TEXT("typed payload sealed from strict plan"), MakeTypedPayload(Request, Payload, DryRun));
	FHyperAIStudioGASDomainAdapter Adapter;
	FHyperAIStudioDomainDispatchContext MissingNonce = ValidContext(Adapter, DryRun.PlanHash);
	MissingNonce.VerifiedAuthorizationNonce.Reset();
	const FHyperAIStudioDomainAdapterResult Rejected = Adapter.Execute(MissingNonce, Payload);
	TestEqual(TEXT("adapter is zero-effect even with malformed context"), Rejected.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("stable bounded-backend blocker"), Rejected.StatusCode,
		FString(FHyperAIStudioGASContracts::ExecutionBlocker));
	TestEqual(TEXT("rejected adapter call did not mutate"), Effect->DurationPolicy, OriginalPolicy);

	FHyperAIStudioDomainDispatchContext Stale = ValidContext(Adapter, DryRun.PlanHash);
	Payload.Operations[0].ExpectedRevision =
		TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	Payload.SemanticFingerprint = FHyperAIStudioGASContracts::ComputePayloadSemanticFingerprint(
		Payload.Operations, Payload.BaseRevision);
	const FHyperAIStudioDomainAdapterResult StaleResult = Adapter.Execute(Stale, Payload);
	TestEqual(TEXT("stale payload cannot reach a mutator"), StaleResult.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("stale call has the same zero-effect blocker"), StaleResult.StatusCode,
		FString(FHyperAIStudioGASContracts::ExecutionBlocker));
	TestEqual(TEXT("stale CAS did not mutate"), Effect->DurationPolicy, OriginalPolicy);

	TestTrue(TEXT("restore exact payload"), MakeTypedPayload(Request, Payload, DryRun));
	const FHyperAIStudioDomainAdapterResult Applied =
		Adapter.Execute(ValidContext(Adapter, DryRun.PlanHash), Payload);
	TestEqual(TEXT("even exact trusted context is zero-effect"), Applied.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("exact call reports bounded backend requirement"), Applied.StatusCode,
		FString(FHyperAIStudioGASContracts::ExecutionBlocker));
	TestEqual(TEXT("duration policy remains unchanged"), Effect->DurationPolicy, OriginalPolicy);
	for (const EHyperAIStudioDomainExecutionActionKind Action : {
		EHyperAIStudioDomainExecutionActionKind::Compile,
		EHyperAIStudioDomainExecutionActionKind::Validate,
		EHyperAIStudioDomainExecutionActionKind::Save,
		EHyperAIStudioDomainExecutionActionKind::VerifyFresh})
	{
		FHyperAIStudioDomainDispatchContext PhaseContext = ValidContext(Adapter, DryRun.PlanHash);
		PhaseContext.ActionKind = Action;
		const FHyperAIStudioDomainAdapterResult PhaseResult =
			Adapter.Execute(PhaseContext, Payload);
		TestEqual(TEXT("every legacy mutation/finalizer phase rejects before effect"),
			PhaseResult.Outcome, EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
		TestEqual(TEXT("every phase reports the stable bounded-backend blocker"),
			PhaseResult.StatusCode, FString(FHyperAIStudioGASContracts::ExecutionBlocker));
		TestEqual(TEXT("every rejected phase preserves the CDO"),
			Effect->DurationPolicy, OriginalPolicy);
	}
	FHyperAIGASValidateRequest ValidateRequest;
	ValidateRequest.AssetPaths.Add(Path);
	ValidateRequest.ExpectedRevision = BeforeSnapshotRevision;
	const FHyperAIGASValidateReport Validation =
		UHyperAIStudioGASToolset::hyper_gas_validate(ValidateRequest);
	TestTrue(TEXT("validator always performs a fresh loaded-state read"), Validation.bFreshCapture);
	TestTrue(TEXT("unchanged expected revision remains independently valid"), Validation.bValid);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
