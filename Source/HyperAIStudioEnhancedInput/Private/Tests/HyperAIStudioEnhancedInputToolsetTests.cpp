// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioEnhancedInputToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Internationalization/Text.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::EnhancedInput::Tests
{
	FString Hash(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	bool HasIssue(const TArray<FHyperAIInputIssue>& Issues, const TCHAR* Code)
	{
		return Issues.ContainsByPredicate([Code](const FHyperAIInputIssue& Issue)
		{
			return Issue.Code == Code;
		});
	}

	FHyperAIStudioEnhancedInputMappingValue Mapping(
		const FString& Context,
		const int32 Index,
		const FString& Action,
		const FString& Key,
		const FString& ValueType)
	{
		FHyperAIStudioEnhancedInputMappingValue Value;
		Value.ContextPath = Context;
		Value.MappingIndex = Index;
		Value.ActionPath = Action;
		Value.Key = Key;
		Value.ActionValueType = ValueType;
		Value.MappingFingerprint = Hash(TEXT('a') + (Index % 20));
		Value.bKeyValid = true;
		Value.bRevisionComplete = true;
		return Value;
	}

	int64 FutureUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000ll + Now.GetMillisecond() + 60000ll;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputManifestTest,
	"HyperAIStudio.NativeTools.EnhancedInput.ManifestAndAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputManifestTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EnhancedInput::Tests;
	const TArray<FHyperAIStudioEnhancedInputManifestEntry>& Manifest =
		FHyperAIStudioEnhancedInputContracts::GetManifest();
	TestEqual(TEXT("The pack exposes exactly three callable contracts"), Manifest.Num(), 3);
	const TArray<FString> Expected = {
		TEXT("hyper_input_inspect"), TEXT("hyper_input_apply_plan"), TEXT("hyper_input_validate")};
	TestEqual(TEXT("Optional-module toolset uses Epic's real script-module qualifier"),
		FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioEnhancedInput.HyperAIStudioEnhancedInputToolset")));
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		TestEqual(TEXT("Manifest wire name is exact"), Manifest[Index].Name, Expected[Index]);
		TestEqual(TEXT("Every entry belongs to one toolset"), Manifest[Index].QualifiedToolset,
			FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName());
	}

	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("Generated registry is the sole exact cohort admission authority"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			FHyperAIStudioEnhancedInputContracts::PackId,
			FHyperAIStudioEnhancedInputContracts::AtomicCohortId,
			Expected,
			Admission));
	TestTrue(TEXT("Generated catalog itself validated"), Admission.bCatalogValid);
	TestTrue(TEXT("All and only the three declared names form the atomic cohort"),
		Admission.bExactCohortMatch);
	TestTrue(TEXT("Enhanced Input remains an optional pack"), Admission.bOptionalPack);
	TestEqual(TEXT("The implementation remains source-candidate only"), Admission.State,
		EHyperAIStudioExtensionAdmissionState::SourceCandidate);
	const bool bProductionExpected =
		Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted;
	TestEqual(TEXT("Production registration follows generated admitted state"),
		FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(false),
		bProductionExpected);
	const bool bDevExpected = bProductionExpected
		|| (Admission.State == EHyperAIStudioExtensionAdmissionState::SourceCandidate
			&& FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled());
	TestEqual(TEXT("Source candidates additionally require the explicit dev command-line gate"),
		FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(true), bDevExpected);
	TestTrue(TEXT("Automation exists only after the LoadingPhase=None module was explicitly loaded"),
		FModuleManager::Get().IsModuleLoaded(TEXT("HyperAIStudioEnhancedInput")));
	if (UToolsetRegistry::IsAvailable())
	{
		TestEqual(TEXT("Explicit module startup delegates registration to generated cohort policy"),
			FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				UHyperAIStudioEnhancedInputToolset::StaticClass(),
				FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName()),
			FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(
				FHyperAIStudioEnhancedInputContracts::IsPendingTestRegistrationEnabled()));
	}

	TSet<FString> Reflected;
	for (TFieldIterator<UFunction> It(UHyperAIStudioEnhancedInputToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable")))
		{
			Reflected.Add(It->GetName());
		}
	}
	TestEqual(TEXT("Reflection exposes no hidden fourth callable"), Reflected.Num(), 3);
	for (const FString& Name : Expected)
	{
		TestTrue(TEXT("Expected callable is reflected"), Reflected.Contains(Name));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputStrictSchemaTest,
	"HyperAIStudio.NativeTools.EnhancedInput.StrictTypedSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputStrictSchemaTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioEnhancedInputBackendComponent Parsed;
	FString Error;
	FHyperAIInputComponentSpec Hold;
	Hold.Kind = TEXT("trigger.hold");
	Hold.ActuationThreshold = 0.5;
	Hold.DurationSeconds = 0.25;
	Hold.bHasOption = true;
	Hold.Mode = TEXT("real_time");
	TestTrue(TEXT("Allowlisted hold trigger accepts its exact fields"),
		FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(Hold, true, Parsed, Error));
	TestFalse(TEXT("Trigger cannot be smuggled through the modifier array"),
		FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(Hold, false, Parsed, Error));

	FHyperAIInputComponentSpec Scalar;
	Scalar.Kind = TEXT("modifier.scalar");
	Scalar.bHasVector = true;
	Scalar.Vector = FVector(1.0, 2.0, 3.0);
	TestTrue(TEXT("Allowlisted scalar accepts a bounded explicit vector"),
		FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(Scalar, false, Parsed, Error));
	Scalar.Mode = TEXT("/Script/Untrusted.RawClass");
	TestFalse(TEXT("Unused/raw class-like fields fail closed"),
		FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(Scalar, false, Parsed, Error));

	FHyperAIInputComponentSpec DeadZone;
	DeadZone.Kind = TEXT("modifier.dead_zone");
	DeadZone.LowerThreshold = 0.5;
	DeadZone.UpperThreshold = 0.5;
	DeadZone.Mode = TEXT("radial");
	TestFalse(TEXT("Dead-zone bounds must be ordered"),
		FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(DeadZone, false, Parsed, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputOperationAndSafetyTest,
	"HyperAIStudio.NativeTools.EnhancedInput.OperationsAndSafetyRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputOperationAndSafetyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EnhancedInput::Tests;
	FString Error;
	FHyperAIStudioEnhancedInputBackendOperation Backend;
	FHyperAIInputPlanOperation Create;
	Create.Type = TEXT("create_action");
	Create.TargetPath = TEXT("/Game/Input/IA_Jump.IA_Jump");
	Create.ValueType = TEXT("boolean");
	TestTrue(TEXT("Canonical project-contained action creation is typed"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(Create, Backend, Error));
	TestEqual(TEXT("Creation routes to edit safety"), Backend.Safety,
		EHyperAIStudioEnhancedInputSafety::Edit);
	FHyperAIInputPlanOperation NonPrimaryCreate = Create;
	NonPrimaryCreate.TargetPath = TEXT("/Game/Input/IA_Jump.DifferentObjectName");
	TestFalse(TEXT("Create absence is queried only for a package-primary object name"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(
			NonPrimaryCreate, Backend, Error));
	Create.Key = TEXT("SpaceBar");
	TestFalse(TEXT("Unused discriminant fields are rejected"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(Create, Backend, Error));
	Create.Key.Reset();
	Create.ProfileId = TEXT("keyboard");
	TestFalse(TEXT("Public read-only profile overrides cannot be edited"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(Create, Backend, Error));

	FHyperAIInputPlanOperation Remove;
	Remove.Type = TEXT("remove_mapping");
	Remove.TargetPath = TEXT("/Game/Input/IMC_Default.IMC_Default");
	Remove.ExpectedRevision = Hash(TEXT('b'));
	Remove.MappingIndex = 0;
	Remove.ExpectedMappingFingerprint = Hash(TEXT('c'));
	TestTrue(TEXT("Exact mapping removal has revision and mapping CAS"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(Remove, Backend, Error));
	TestEqual(TEXT("Mapping removal routes to destructive safety"), Backend.Safety,
		EHyperAIStudioEnhancedInputSafety::Destructive);

	FHyperAIInputPlanOperation Runtime;
	Runtime.Type = TEXT("set_runtime_priority");
	Runtime.TargetPath = TEXT("/Game/Input/IMC_Default.IMC_Default");
	Runtime.ExpectedRevision = Hash(TEXT('d'));
	Runtime.RuntimeOwnerPath = TEXT("/Engine/Transient.EnhancedPlayerInput_0");
	Runtime.Priority = 20;
	TestTrue(TEXT("Runtime priority is an explicit typed external operation"),
		FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(Runtime, Backend, Error));
	TestEqual(TEXT("Runtime priority routes to external-effect safety"), Backend.Safety,
		EHyperAIStudioEnhancedInputSafety::ExternalEffect);

	TestTrue(TEXT("Edit dry-run needs no grant"),
		FHyperAIStudioEnhancedInputContracts::ValidateAuthorizationEnvelope(
			EHyperAIStudioEnhancedInputSafety::Edit, true, FString(), Error));
	TestFalse(TEXT("Dry-run never transports bearer authority"),
		FHyperAIStudioEnhancedInputContracts::ValidateAuthorizationEnvelope(
			EHyperAIStudioEnhancedInputSafety::Destructive, true, TEXT("opaque"), Error));
	TestFalse(TEXT("Destructive execution never trusts an absent/client boolean grant"),
		FHyperAIStudioEnhancedInputContracts::ValidateAuthorizationEnvelope(
			EHyperAIStudioEnhancedInputSafety::Destructive, false, FString(), Error));
	TestTrue(TEXT("Destructive staging requires an opaque grant for later trusted verification"),
		FHyperAIStudioEnhancedInputContracts::ValidateAuthorizationEnvelope(
			EHyperAIStudioEnhancedInputSafety::Destructive, false, TEXT("server-opaque-grant"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputValidationTest,
	"HyperAIStudio.NativeTools.EnhancedInput.ConflictValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EnhancedInput::Tests;
	const FString Context = TEXT("/Game/Input/IMC_Test.IMC_Test");
	const FString Jump = TEXT("/Game/Input/IA_Jump.IA_Jump");
	const FString Fire = TEXT("/Game/Input/IA_Fire.IA_Fire");
	FHyperAIStudioEnhancedInputValueSnapshot Snapshot;
	Snapshot.ActionPaths = {Jump, Fire};
	Snapshot.ContextPaths.Add(Context);
	Snapshot.ActionValueTypes.Add(Jump, TEXT("boolean"));
	Snapshot.ActionValueTypes.Add(Fire, TEXT("boolean"));
	Snapshot.Mappings.Add(Mapping(Context, 0, Jump, TEXT("SpaceBar"), TEXT("boolean")));
	Snapshot.Mappings.Add(Mapping(Context, 1, Fire, TEXT("SpaceBar"), TEXT("boolean")));
	Snapshot.Mappings.Add(Mapping(Context, 2, Jump, TEXT("SpaceBar"), TEXT("boolean")));
	FHyperAIStudioEnhancedInputMappingValue Invalid = Mapping(Context, 3, FString(), TEXT("Invalid"), FString());
	Invalid.bKeyValid = false;
	Snapshot.Mappings.Add(Invalid);
	FHyperAIInputComponentSpec Scalar;
	Scalar.Kind = TEXT("modifier.scalar");
	Scalar.bHasVector = true;
	Scalar.Vector = FVector(1.0);
	Snapshot.Mappings[0].Modifiers.Add(Scalar);
	FHyperAIInputRecord EmptyContext;
	EmptyContext.Kind = TEXT("mapping_context");
	EmptyContext.StableId = TEXT("context:") + Context;
	EmptyContext.AssetPath = Context;
	Snapshot.Records.Add(EmptyContext);
	FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(Snapshot);
	bool bTruncated = false;
	const TArray<FHyperAIInputIssue> Issues =
		FHyperAIStudioEnhancedInputContracts::ValidateValueSnapshot(Snapshot, 64, bTruncated);
	TestFalse(TEXT("Synthetic issue set fits its hard bound"), bTruncated);
	TestTrue(TEXT("Conflicting keys are diagnosed"), HasIssue(Issues, TEXT("conflicting_key")));
	TestTrue(TEXT("Duplicate mappings are diagnosed"), HasIssue(Issues, TEXT("duplicate_mapping")));
	TestTrue(TEXT("Invalid keys are diagnosed"), HasIssue(Issues, TEXT("invalid_key")));
	TestTrue(TEXT("Missing actions are diagnosed"), HasIssue(Issues, TEXT("missing_action")));
	TestTrue(TEXT("Boolean/axis modifier incompatibility is diagnosed"),
		HasIssue(Issues, TEXT("modifier_value_type_incompatible")));
	TestTrue(TEXT("Empty contexts are diagnosed"), HasIssue(Issues, TEXT("empty_mapping_context")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputRevisionTest,
	"HyperAIStudio.NativeTools.EnhancedInput.DeterministicRevisionCAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputRevisionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EnhancedInput::Tests;
	FHyperAIStudioEnhancedInputValueSnapshot First;
	First.bComplete = true;
	FHyperAIInputRecord A;
	A.Kind = TEXT("action"); A.StableId = TEXT("action:a"); A.AssetPath = TEXT("/Game/A.A");
	A.ValueType = TEXT("boolean");
	FHyperAIInputRecord B;
	B.Kind = TEXT("mapping_context"); B.StableId = TEXT("context:b"); B.AssetPath = TEXT("/Game/B.B");
	First.Records = {A, B};
	const FString FirstHash = FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(First);
	FHyperAIStudioEnhancedInputValueSnapshot Reordered = First;
	Reordered.Records = {B, A};
	const FString ReorderedHash = FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(Reordered);
	TestEqual(TEXT("Record insertion order cannot change snapshot CAS"), FirstHash, ReorderedHash);
	Reordered.Records[0].Description = TEXT("changed");
	const FString ChangedHash = FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(Reordered);
	TestNotEqual(TEXT("Semantic asset state changes snapshot CAS"), FirstHash, ChangedHash);
	TestTrue(TEXT("CAS uses a canonical bounded SHA-256"), FirstHash.StartsWith(TEXT("sha256:"))
		&& FirstHash.Len() == 71);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputNoLoadTest,
	"HyperAIStudio.NativeTools.EnhancedInput.LoadedOnlyNoSyncLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputNoLoadTest::RunTest(const FString& Parameters)
{
	UE::AssetRegistry::EExists (IAssetRegistry::*TryPackageNonBlocking)(
		FName, FAssetPackageData&, bool) const = &IAssetRegistry::TryGetAssetPackageData;
	TArray<FString> (UInputMappingContext::*GetProfiles)() const =
		&UInputMappingContext::GetProfilesWithOverridenMappings;
	const TArray<FEnhancedActionKeyMapping>& (UInputMappingContext::*GetProfileMappings)(
		const FString&) const = &UInputMappingContext::GetMappingsForProfile;
	const FString& (*TextDisplayView)(const FText&) = &FTextInspector::GetDisplayString;
	TestTrue(TEXT("Create absence binds UE 5.8's nonblocking package query"),
		TryPackageNonBlocking != nullptr);
	TestTrue(TEXT("Profile capture binds UE 5.8's public profile enumeration"),
		GetProfiles != nullptr);
	TestTrue(TEXT("Profile capture binds UE 5.8's public profile mapping view"),
		GetProfileMappings != nullptr);
	TestTrue(TEXT("Loaded FText capture binds a reference view before clipping"),
		TextDisplayView != nullptr);
	const FString Path = TEXT("/Game/__HyperAI_NoLoad_Evidence__/IA_NeverLoaded.IA_NeverLoaded");
	const FSoftObjectPath Reference(Path);
	TestNull(TEXT("Fixture starts unloaded"), Reference.ResolveObject());
	FHyperAIInputInspectRequest Request;
	Request.AssetPaths.Add(Path);
	Request.bIncludeRuntimeBindings = false;
	const FHyperAIInputInspectReport Report = UHyperAIStudioEnhancedInputToolset::hyper_input_inspect(Request);
	TestTrue(TEXT("Loaded-only miss returns bounded evidence rather than loading"), Report.bOk);
	TestEqual(TEXT("Loaded-only miss is explicitly partial"), Report.Status, FString(TEXT("partial")));
	TestTrue(TEXT("Loaded-only miss is machine-readable"),
		HyperAIStudio::EnhancedInput::Tests::HasIssue(Report.Issues, TEXT("asset_not_loaded")));
	TestNull(TEXT("Inspection never synchronously loads the asset"), Reference.ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputStagingTest,
	"HyperAIStudio.NativeTools.EnhancedInput.IdempotentTypedStagingOneFinalize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputStagingTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EnhancedInput::Tests;
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	FHyperAIStudioStagedEnhancedInputPlanArtifact Artifact;
	Artifact.CanonicalProjectId = FString::ChrN(40, TEXT('a'));
	Artifact.OperationId = TEXT("enhanced_input_test_001");
	Artifact.BaseRevision = Hash(TEXT('b'));
	Artifact.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
	Artifact.ExpiresUtcMs = FutureUtcMs();
	Artifact.bRequiresSaveOnce = true;
	Artifact.bRequiresValidateOnce = true;
	Artifact.bRequiresFreshVerifyOnce = true;
	FHyperAIStudioEnhancedInputBackendOperation& Operation = Artifact.Operations.AddDefaulted_GetRef();
	Operation.Kind = EHyperAIStudioEnhancedInputOperationKind::CreateAction;
	Operation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
	Operation.TargetPath = TEXT("/Game/Input/IA_Staged.IA_Staged");
	Operation.ValueType = EHyperAIStudioEnhancedInputValueType::Boolean;
	Operation.bHasValueType = true;
	FString Error;
	TestTrue(TEXT("Exact typed artifact receives deterministic plan/effect bindings"),
		FHyperAIStudioEnhancedInputContracts::FinalizeArtifactFingerprints(Artifact, Error));
	TestEqual(TEXT("Edit artifact routes through the exact central operation type"),
		Artifact.TypedOperationType, FString(FHyperAIStudioEnhancedInputContracts::EditOperationType));
	TestTrue(TEXT("One-finalize lifecycle binds one save, validate, and fresh verify"),
		Artifact.bRequiresSaveOnce && Artifact.bRequiresValidateOnce && Artifact.bRequiresFreshVerifyOnce);

	bool bReplay = false;
	TestTrue(TEXT("First exact stage succeeds without mutation"),
		FHyperAIStudioEnhancedInputPlanStagingService::Stage(Artifact, bReplay, Error));
	TestFalse(TEXT("First stage is not a replay"), bReplay);
	TestTrue(TEXT("Same operation and exact hashes replay idempotently"),
		FHyperAIStudioEnhancedInputPlanStagingService::Stage(Artifact, bReplay, Error));
	TestTrue(TEXT("Second exact stage is marked replay"), bReplay);
	FHyperAIStudioStagedEnhancedInputPlanArtifact Claimed;
	TestTrue(TEXT("Central executor can claim only the exact project/operation/plan tuple"),
		FHyperAIStudioEnhancedInputPlanStagingService::ClaimExact(
			Artifact.CanonicalProjectId, Artifact.OperationId, Artifact.PlanHash, Claimed, Error));
	TestEqual(TEXT("Claim preserves exact effect binding"), Claimed.EffectFingerprint,
		Artifact.EffectFingerprint);
	TestFalse(TEXT("Claim is one-shot"),
		FHyperAIStudioEnhancedInputPlanStagingService::ClaimExact(
			Artifact.CanonicalProjectId, Artifact.OperationId, Artifact.PlanHash, Claimed, Error));
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputApplyPlanBackendGateTest,
	"HyperAIStudio.NativeTools.EnhancedInput.ApplyPlanFailsClosedWithoutBackend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputApplyPlanBackendGateTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	FHyperAIInputApplyPlanRequest Preview;
	Preview.bDryRun = true;
	FHyperAIInputPlanOperation& Create = Preview.Operations.AddDefaulted_GetRef();
	Create.Type = TEXT("create_action");
	Create.TargetPath = TEXT("/Game/__HyperAI_EnhancedInput_Evidence__/IA_BackendGate.IA_BackendGate");
	Create.ValueType = TEXT("boolean");
	const FHyperAIInputApplyPlanReport PreviewReport =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(Preview);
	TestTrue(TEXT("Closed create plan can be previewed without mutation"), PreviewReport.bOk);
	if (!PreviewReport.bOk)
	{
		AddError(FString::Printf(TEXT("Preview failed: %s (%s)"),
			*PreviewReport.Status, *PreviewReport.Diagnostic));
		return false;
	}

	FHyperAIInputApplyPlanRequest Execute = Preview;
	Execute.bDryRun = false;
	Execute.OperationId = TEXT("enhanced_input_backend_gate_001");
	Execute.ExpectedPlanHash = PreviewReport.PlanHash;
	const FHyperAIInputApplyPlanReport ExecuteReport =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(Execute);
	TestFalse(TEXT("Staging is not reported as successful execution"), ExecuteReport.bOk);
	TestTrue(TEXT("Exact typed artifact is retained for a future admitted backend"),
		ExecuteReport.bStaged);
	TestFalse(TEXT("No mutation backend was submitted"), ExecuteReport.bExecutionSubmitted);
	TestEqual(TEXT("Missing executor backend has one machine-readable fail-closed status"),
		ExecuteReport.Status, FString(TEXT("staged_backend_required")));
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIEnhancedInputFastReversibleEditTest,
	"HyperAIStudio.NativeTools.EnhancedInput.FastReversibleEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIEnhancedInputFastReversibleEditTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const EHyperAIStudioNativeExecutionMode PreviousMode = Settings->NativeExecutionMode;
	Settings->NativeExecutionMode = EHyperAIStudioNativeExecutionMode::Fast;

	const FString PackagePath = TEXT("/Game/__HyperAI_EnhancedInput_Evidence__/IA_FastEdit");
	UPackage* Package = CreatePackage(*PackagePath);
	Package->SetPackageFlags(PKG_InMemoryOnly);
	UInputAction* Action = NewObject<UInputAction>(Package, TEXT("IA_FastEdit"),
		RF_Public | RF_Standalone | RF_Transactional);
	TestNotNull(TEXT("Fast edit fixture action exists"), Action);
	if (!Action)
	{
		Settings->NativeExecutionMode = PreviousMode;
		return false;
	}
	const FString ObjectPath = Action->GetPathName();

	FHyperAIInputInspectRequest InspectRequest;
	InspectRequest.AssetPaths.Add(ObjectPath);
	InspectRequest.bIncludeRuntimeBindings = false;
	const FHyperAIInputInspectReport Before =
		UHyperAIStudioEnhancedInputToolset::hyper_input_inspect(InspectRequest);
	TestTrue(TEXT("Loaded action has a complete fast-edit base revision"),
		Before.bOk && Before.bRevisionComplete && !Before.Revision.IsEmpty());
	if (!Before.bOk || !Before.bRevisionComplete)
	{
		Settings->NativeExecutionMode = PreviousMode;
		Package->SetDirtyFlag(false);
		return false;
	}

	FHyperAIInputApplyPlanRequest Preview;
	Preview.bDryRun = true;
	FHyperAIInputPlanOperation& SetConfig = Preview.Operations.AddDefaulted_GetRef();
	SetConfig.Type = TEXT("set_action_config");
	SetConfig.TargetPath = ObjectPath;
	SetConfig.ExpectedRevision = Before.Revision;
	SetConfig.bSetTriggerWhenPaused = true;
	SetConfig.bTriggerWhenPaused = true;
	SetConfig.bSetConsumeInput = true;
	SetConfig.bConsumeInput = false;
	const FHyperAIInputApplyPlanReport PreviewReport =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(Preview);
	TestTrue(TEXT("Fast reversible edit keeps typed dry-run"), PreviewReport.bOk);

	FHyperAIInputApplyPlanRequest Execute = Preview;
	Execute.bDryRun = false;
	Execute.OperationId = TEXT("enhanced_input_fast_edit_001");
	Execute.ExpectedPlanHash = PreviewReport.PlanHash;
	const FHyperAIInputApplyPlanReport Applied =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(Execute);
	TestTrue(TEXT("Fast edit executes instead of staging"), Applied.bOk);
	TestEqual(TEXT("Fast edit status is explicit"), Applied.Status,
		FString(TEXT("applied_fast_transaction")));
	TestFalse(TEXT("Fast edit does not stage"), Applied.bStaged);
	TestTrue(TEXT("Fast edit reports execution"), Applied.bExecutionSubmitted);
	TestFalse(TEXT("Fast edit bypasses durable journal"), Applied.bRequiresJournalExecution);
	TestTrue(TEXT("Input Action config changed"), Action->bTriggerWhenPaused);
	TestFalse(TEXT("Input Action consume flag changed"), Action->bConsumeInput);

	const FHyperAIInputApplyPlanReport Replay =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(Execute);
	TestTrue(TEXT("Exact fast operation replay is idempotent"), Replay.bOk && Replay.bReplay);
	TestEqual(TEXT("Replay avoids a second capture/mutation"), Replay.Status,
		FString(TEXT("already_applied")));

	const FString ContextPackagePath =
		TEXT("/Game/__HyperAI_EnhancedInput_Evidence__/IMC_FastEdit");
	UPackage* ContextPackage = CreatePackage(*ContextPackagePath);
	ContextPackage->SetPackageFlags(PKG_InMemoryOnly);
	UInputMappingContext* Context = NewObject<UInputMappingContext>(ContextPackage,
		TEXT("IMC_FastEdit"), RF_Public | RF_Standalone | RF_Transactional);
	FHyperAIInputInspectRequest ContextInspectRequest;
	ContextInspectRequest.AssetPaths.Add(Context->GetPathName());
	ContextInspectRequest.bIncludeRuntimeBindings = false;
	const FHyperAIInputInspectReport ContextBefore =
		UHyperAIStudioEnhancedInputToolset::hyper_input_inspect(ContextInspectRequest);
	TestTrue(TEXT("Loaded mapping context has an exact base revision"),
		ContextBefore.bOk && ContextBefore.bRevisionComplete);

	FHyperAIInputApplyPlanRequest MappingPreview;
	MappingPreview.bDryRun = true;
	FHyperAIInputPlanOperation& AddMapping = MappingPreview.Operations.AddDefaulted_GetRef();
	AddMapping.Type = TEXT("add_mapping");
	AddMapping.TargetPath = Context->GetPathName();
	AddMapping.ExpectedRevision = ContextBefore.Revision;
	AddMapping.ActionPath = ObjectPath;
	AddMapping.Key = EKeys::SpaceBar.GetFName().ToString();
	const FHyperAIInputApplyPlanReport MappingPreviewReport =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(MappingPreview);
	TestTrue(TEXT("Simple mapping has a valid typed preview"), MappingPreviewReport.bOk);
	FHyperAIInputApplyPlanRequest MappingExecute = MappingPreview;
	MappingExecute.bDryRun = false;
	MappingExecute.OperationId = TEXT("enhanced_input_fast_edit_002");
	MappingExecute.ExpectedPlanHash = MappingPreviewReport.PlanHash;
	const FHyperAIInputApplyPlanReport MappingApplied =
		UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(MappingExecute);
	TestTrue(TEXT("Simple mapping executes in fast mode"), MappingApplied.bOk);
	TestEqual(TEXT("Exactly one mapping is added"), Context->GetMappings().Num(), 1);
	if (Context->GetMappings().Num() == 1)
	{
		TestTrue(TEXT("Mapping binds the exact action"),
			Context->GetMappings()[0].Action == Action);
		TestTrue(TEXT("Mapping binds the exact key"),
			Context->GetMappings()[0].Key == EKeys::SpaceBar);
	}

	Settings->NativeExecutionMode = PreviousMode;
	Package->SetDirtyFlag(false);
	ContextPackage->SetDirtyFlag(false);
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
