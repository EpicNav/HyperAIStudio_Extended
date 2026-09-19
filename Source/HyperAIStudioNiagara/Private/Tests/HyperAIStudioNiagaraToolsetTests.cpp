// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioNiagaraToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "FileHelpers.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "Misc/ScopeExit.h"
#include "HyperAIStudioNiagaraAssetsGate.h"
#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "ObjectTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorModule.h"
#include "NiagaraSystem.h"
#include "UObject/SoftObjectPath.h"

#include <type_traits>

namespace HyperAIStudio::Niagara::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter;

	FString HashOf(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FHyperAIStudioDomainDispatchContext MakeMutationContext(const EHyperAIStudioDomainExecutionActionKind Phase)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioNiagaraContracts::GetAdapterDescriptor();
		FHyperAIStudioDomainDispatchContext Context;
		Context.Binding.PackId = FHyperAIStudioNiagaraContracts::PackId;
		Context.Binding.ToolName = TEXT("hyper_niagara_apply_plan");
		Context.Binding.VariantId = FHyperAIStudioNiagaraContracts::MutationVariantId;
		Context.Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Context.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Context.Safety = EHyperAIStudioDomainSafety::Edit;
		Context.ActionKind = Phase;
		return Context;
	}

	FHyperAINiagaraEditOp MakeOp(const FString& Kind)
	{
		FHyperAINiagaraEditOp Op;
		Op.Kind = Kind;
		return Op;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraManifestTest,
	"HyperAIStudio.Niagara.Contracts.ExactThreeToolCohort",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraManifestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioNiagaraManifestEntry>& Manifest =
		FHyperAIStudioNiagaraContracts::GetManifest();
	// The generated catalog pins these three names; a fourth tool would fail admission until it is regenerated.
	TestEqual(TEXT("exactly three Niagara callables"), Manifest.Num(), 3);
	TestEqual(TEXT("exact optional-module toolset identity"),
		FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioNiagara.HyperAIStudioNiagaraToolset")));
	const TSet<FString> Expected = {
		TEXT("hyper_niagara_inspect"),
		TEXT("hyper_niagara_apply_plan"),
		TEXT("hyper_niagara_validate")};
	TSet<FString> Actual;
	for (const FHyperAIStudioNiagaraManifestEntry& Entry : Manifest)
	{
		Actual.Add(Entry.Name);
		TestEqual(TEXT("one exact reflected owner"), Entry.QualifiedToolset,
			FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName());
		const UFunction* Function = UHyperAIStudioNiagaraToolset::StaticClass()->FindFunctionByName(
			FName(*Entry.Name), EIncludeSuperFlag::ExcludeSuper);
		TestNotNull(TEXT("manifest function exists"), Function);
		if (Function)
		{
			TestTrue(TEXT("manifest function is AICallable"),
				Function->HasMetaData(TEXT("AICallable")));
			// The registry turns this tooltip into the MCP tool description agents choose by.
			TestFalse(TEXT("manifest function has a description"),
				Function->GetMetaData(TEXT("ToolTip")).IsEmpty());
		}
	}
	bool bNamesExact = Actual.Num() == Expected.Num();
	for (const FString& Name : Expected) bNamesExact &= Actual.Contains(Name);
	TestTrue(TEXT("manifest names exact"), bNamesExact);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraDescriptorTest,
	"HyperAIStudio.Niagara.Contracts.AdapterDescriptorExact",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraDescriptorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioNiagaraContracts::GetAdapterDescriptor();
	TestEqual(TEXT("pack id"), Descriptor.PackId, FString(TEXT("niagara_vfx")));
	TestEqual(TEXT("optional module type contract"),
		FString(FHyperAIStudioNiagaraContracts::RequiredModuleType), FString(TEXT("Editor")));
	TestEqual(TEXT("explicit loading phase contract"),
		FString(FHyperAIStudioNiagaraContracts::RequiredLoadingPhase), FString(TEXT("None")));
	TestEqual(TEXT("source cohort"),
		FString(FHyperAIStudioNiagaraContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioniagaratoolset.v1")));
	TestEqual(TEXT("exact live probe"),
		FString(FHyperAIStudioNiagaraContracts::LiveProbeId),
		FString(TEXT("probe.niagara_editor")));
	TestEqual(TEXT("exact three variants"), Descriptor.Variants.Num(), 3);
	TestTrue(TEXT("contract fingerprint canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Descriptor.ContractFingerprint));
	TestTrue(TEXT("adapter fingerprint canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Descriptor.AdapterFingerprint));
	TestTrue(TEXT("all Niagara requirement groups are blocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.IsEmpty());
	TSet<FString> Variants;
	TSet<FString> RequestTypes;
	TSet<FString> ResultTypes;
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		Variants.Add(Variant.VariantId);
		RequestTypes.Add(Variant.RequestTypeId);
		ResultTypes.Add(Variant.ResultTypeId);
		TestTrue(TEXT("request schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Variant.RequestSchemaFingerprint));
		TestTrue(TEXT("result schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Variant.ResultSchemaFingerprint));
		TestTrue(TEXT("result type stays in the disjoint result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
	}
	TestEqual(TEXT("unique variants"), Variants.Num(), 3);
	TestEqual(TEXT("unique request DTOs"), RequestTypes.Num(), 3);
	TestEqual(TEXT("unique result DTOs"), ResultTypes.Num(), 3);
	// Safety classes are pinned by the generated catalog; changing one fails admission closed.
	TestEqual(TEXT("inspect read safety"), Descriptor.Variants[0].Safety, EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("apply_plan edit safety"), Descriptor.Variants[1].Safety, EHyperAIStudioDomainSafety::Edit);
	TestEqual(TEXT("apply_plan runs batched edit ops"), Descriptor.Variants[1].VariantId,
		FString(TEXT("external_edit_ops.v1")));
	TestEqual(TEXT("validate read safety"), Descriptor.Variants[2].Safety, EHyperAIStudioDomainSafety::Read);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraDelegationTest,
	"HyperAIStudio.Niagara.Contracts.CapabilityMatrix",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraDelegationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAINiagaraCapabilityStatus> Matrix =
		FHyperAIStudioNiagaraContracts::GetCapabilityMatrix();
	TestEqual(TEXT("one explicit Niagara matrix row"), Matrix.Num(), 1);
	if (Matrix.IsEmpty()) return false;
	TSet<FString> Delegated;
	for (const FString& Name : Matrix[0].DelegatedEpicCallables) Delegated.Add(Name);
	TestEqual(TEXT("all 56 Epic Niagara callables still listed"), Matrix[0].DelegatedEpicCallables.Num(), 56);
	TestEqual(TEXT("no duplicate delegation entries"), Delegated.Num(), 56);
	// Agents read these flags from every Niagara report to decide whether apply_plan can mutate.
	TestTrue(TEXT("independent validation is implemented"), Matrix[0].bIndependentValidationImplemented);
	TestTrue(TEXT("mutation execution is implemented"), Matrix[0].bMutationExecutionImplemented);
	TestTrue(TEXT("batched edits are a supported case"),
		Matrix[0].SupportedCases.Contains(TEXT("batched_edit_ops_one_undo_step")));
	TestTrue(TEXT("open-editor hazard is machine-readable"),
		Matrix[0].UnsupportedCases.Contains(TEXT("target_open_in_niagara_editor")));
	TestFalse(TEXT("retired rename blocker is gone"),
		Matrix[0].UnsupportedCases.Contains(TEXT("full_reference_inventory_incomplete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEditOpValidationTest,
	"HyperAIStudio.Niagara.Mutation.EditOpValidation",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEditOpValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	namespace Gate = HyperAIStudio::Niagara::ExternalEditGate;
	FString Status;
	FString Diagnostic;
	auto Accepts = [&](const FHyperAINiagaraEditOp& Op) { return Gate::ValidateOp(Op, false, Status, Diagnostic); };

	FHyperAINiagaraEditOp Enable = MakeOp(TEXT("set_module_enabled"));
	Enable.EmitterName = TEXT("Sparks");
	Enable.ScriptName = TEXT("ParticleUpdateScript");
	Enable.ModuleName = TEXT("GravityForce");
	TestTrue(TEXT("emitter module toggle accepted"), Accepts(Enable));

	FHyperAINiagaraEditOp SystemScoped = Enable;
	SystemScoped.ScriptName = TEXT("SystemUpdateScript");
	TestFalse(TEXT("system script with an emitter name rejected"), Accepts(SystemScoped));
	SystemScoped.EmitterName.Reset();
	TestTrue(TEXT("system script without an emitter accepted"), Accepts(SystemScoped));

	FHyperAINiagaraEditOp NoEmitter = Enable;
	NoEmitter.EmitterName.Reset();
	TestFalse(TEXT("emitter script without an emitter rejected"), Accepts(NoEmitter));
	TestEqual(TEXT("address failure status"), Status, FString(TEXT("invalid_op_address")));

	FHyperAINiagaraEditOp BadScript = Enable;
	BadScript.ScriptName = TEXT("ParticleSimulationStage");
	TestFalse(TEXT("unknown script stack rejected"), Accepts(BadScript));

	FHyperAINiagaraEditOp ControlChars = Enable;
	ControlChars.ModuleName = TEXT("Gravity\nForce");
	TestFalse(TEXT("control characters rejected"), Accepts(ControlChars));
	TestEqual(TEXT("text failure status"), Status, FString(TEXT("invalid_op_text")));

	TestFalse(TEXT("unknown kind rejected"), Accepts(MakeOp(TEXT("remove_module"))));
	TestEqual(TEXT("removals stay unsupported"), Status, FString(TEXT("unsupported_op_kind")));

	FHyperAINiagaraEditOp SetValue = Enable;
	SetValue.Kind = TEXT("set_input_value");
	SetValue.InputNameStack = { TEXT("Gravity") };
	auto ValueAccepted = [&](const TCHAR* Type, const TCHAR* Value)
	{
		SetValue.ValueType = Type;
		SetValue.Value = Value;
		return Accepts(SetValue);
	};
	TestTrue(TEXT("float"), ValueAccepted(TEXT("float"), TEXT("-9.8")));
	TestTrue(TEXT("int32"), ValueAccepted(TEXT("int32"), TEXT("3")));
	TestTrue(TEXT("bool"), ValueAccepted(TEXT("bool"), TEXT("true")));
	TestTrue(TEXT("vector"), ValueAccepted(TEXT("vector"), TEXT("0, 0, -980")));
	TestTrue(TEXT("color"), ValueAccepted(TEXT("color"), TEXT("1,0.5,0,1")));
	TestFalse(TEXT("int32 rejects a fraction"), ValueAccepted(TEXT("int32"), TEXT("3.5")));
	TestFalse(TEXT("vector rejects the wrong arity"), ValueAccepted(TEXT("vector"), TEXT("1,2")));
	TestFalse(TEXT("float rejects exponent and words"), ValueAccepted(TEXT("float"), TEXT("nan")));
	TestFalse(TEXT("bool rejects anything but true or false"), ValueAccepted(TEXT("bool"), TEXT("1")));
	TestFalse(TEXT("unknown value type rejected"), ValueAccepted(TEXT("quat"), TEXT("0,0,0,1")));
	SetValue.ValueType = TEXT("float");
	SetValue.Value = TEXT("1");
	SetValue.InputNameStack.Reset();
	TestFalse(TEXT("set_input_value needs an input name"), Accepts(SetValue));

	FHyperAINiagaraEditOp Renderer = MakeOp(TEXT("add_renderer"));
	Renderer.EmitterName = TEXT("Sparks");
	Renderer.AssetPath = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
	TestTrue(TEXT("renderer add accepted by shape"), Accepts(Renderer));
	TestTrue(TEXT("sprite renderer class resolves"), Gate::ValidateOp(Renderer, true, Status, Diagnostic));
	Renderer.AssetPath = TEXT("/Script/Niagara.NiagaraRendererProperties");
	TestFalse(TEXT("abstract renderer base rejected"), Gate::ValidateOp(Renderer, true, Status, Diagnostic));

	FHyperAINiagaraEditOp Fix = MakeOp(TEXT("apply_stack_issue_fix"));
	TestFalse(TEXT("fix without ids rejected"), Accepts(Fix));
	Fix.IssueId = TEXT("issue");
	Fix.FixId = TEXT("fix");
	TestTrue(TEXT("fix with ids accepted"), Accepts(Fix));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraCasBoundsTest,
	"HyperAIStudio.Niagara.Contracts.CasTriStatePathsAndBounds",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraCasBoundsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("AssetRegistry Exists"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Exists), FString(TEXT("exists")));
	TestEqual(TEXT("AssetRegistry DoesNotExist"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::DoesNotExist), FString(TEXT("does_not_exist")));
	TestEqual(TEXT("AssetRegistry Unknown stays unknown"),
		FHyperAIStudioNiagaraContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	TestTrue(TEXT("canonical top-level object path"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/NS_Test.NS_Test")));
	TestFalse(TEXT("folder path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX")));
	TestFalse(TEXT("subobject path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/NS_Test.NS_Test:Emitter")));
	TestFalse(TEXT("wildcard rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX/*.NS_Test")));
	TestTrue(TEXT("lowercase sha accepted"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(HyperAIStudio::Niagara::Tests::HashOf(TEXT('a'))));
	TestFalse(TEXT("uppercase sha rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(HyperAIStudio::Niagara::Tests::HashOf(TEXT('A'))));
	TestTrue(TEXT("User namespace accepted"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("User.SpawnRate")));
	TestFalse(TEXT("non-user namespace rejected"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("System.SpawnRate")));
	TestEqual(TEXT("page bound"), FHyperAIStudioNiagaraContracts::MaxPageSize, 64);
	TestEqual(TEXT("op bound"), FHyperAIStudioNiagaraContracts::MaxOpsPerPlan, 16);

	FHyperAINiagaraApplyPlanRequest Request;
	Request.TargetPath = TEXT("/Game/VFX/NS_Test.NS_Test");
	Request.ExpectedRevision = HyperAIStudio::Niagara::Tests::HashOf(TEXT('1'));
	TestEqual(TEXT("an empty plan is rejected before any capture"),
		FHyperAIStudioNiagaraContracts::BuildPlan(Request).Status, FString(TEXT("invalid_op_count")));
	Request.bDryRun = false;
	TestEqual(TEXT("non-dry needs an operation id and plan hash"),
		FHyperAIStudioNiagaraContracts::BuildPlan(Request).Status, FString(TEXT("invalid_submission_fields")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraPayloadAndAdapterTest,
	"HyperAIStudio.Niagara.Mutation.PayloadSealAndAdapterPhases",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraPayloadAndAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	const TSharedRef<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioNiagaraEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/__HyperAIStudioMissing/NS_Missing.NS_Missing");
	Payload->BaseRevision = HashOf(TEXT('1'));
	FHyperAINiagaraEditOp First = MakeOp(TEXT("set_module_enabled"));
	First.EmitterName = TEXT("A");
	First.ScriptName = TEXT("ParticleUpdateScript");
	First.ModuleName = TEXT("M1");
	FHyperAINiagaraEditOp Second = First;
	Second.ModuleName = TEXT("M2");
	Second.bEnabled = false;
	Payload->Ops = { First, Second };
	Payload->SemanticFingerprint = FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(*Payload);
	TestTrue(TEXT("fingerprint is canonical"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(Payload->SemanticFingerprint));
	TestEqual(TEXT("fingerprint is stable"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(*Payload));

	FHyperAIStudioNiagaraEditOpsPayload Reordered = *Payload;
	Reordered.Ops = { Second, First };
	TestNotEqual(TEXT("reordered ops are a different plan"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Reordered));
	FHyperAIStudioNiagaraEditOpsPayload Unsaved = *Payload;
	Unsaved.bSave = false;
	TestNotEqual(TEXT("save choice is sealed"), Payload->SemanticFingerprint,
		FHyperAIStudioNiagaraContracts::ComputeEditOpsSemanticFingerprint(Unsaved));

	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone = Payload->CloneImmutable();
	TestTrue(TEXT("clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone seal preserved"), Clone->GetSemanticFingerprint(), Payload->GetSemanticFingerprint());
	TestEqual(TEXT("clone size preserved"), Clone->GetBoundedByteSize(), Payload->GetBoundedByteSize());

	FHyperAIStudioNiagaraDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Apply = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Clone);
	TestEqual(TEXT("apply on a missing target is rejected before effect"), Apply.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("missing target status"), Apply.StatusCode, FString(TEXT("target_not_loaded")));
	TestFalse(TEXT("rejected apply claims no result"), Apply.Payload.IsValid());

	// After Apply, the edit may already be in memory: a later phase failing must never look effect-free.
	const FHyperAIStudioDomainAdapterResult Save = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Save), *Clone);
	TestEqual(TEXT("post-apply failure reports a known effect"), Save.Outcome,
		EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect);

	Payload->Ops[0].ModuleName = TEXT("Drifted");
	const FHyperAIStudioDomainAdapterResult Drifted = Adapter.Execute(
		MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Payload);
	TestEqual(TEXT("semantic drift rejected"), Drifted.StatusCode, FString(TEXT("typed_payload_drift")));

	FHyperAIStudioNiagaraFreshVerifier Verifier(Adapter.GetDescriptor().AdapterFingerprint);
	FHyperAIStudioNiagaraMutationResultPayload NotCompleted;
	NotCompleted.Phase = TEXT("saved");
	NotCompleted.Revision = HashOf(TEXT('4'));
	NotCompleted.bValid = true;
	FString Postcondition;
	FString VerifyError;
	TestFalse(TEXT("only a completed capture verifies"),
		Verifier.VerifyFreshExact(*Clone, NotCompleted, Postcondition, VerifyError));
	TestTrue(TEXT("failed verification emits no postcondition"), Postcondition.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEndToEndTest,
	"HyperAIStudio.Niagara.Mutation.EndToEndToggleModule",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Planning accepts only a System loaded from disk, so this spans two editor sessions: the first saves a
	// duplicate fixture of a project System, the second loads that fixture fresh and edits it. The fixture lives
	// in Content/__HyperAIStudioTests; delete that folder once done. Nothing here edits an original asset.
	const FString FixtureFolder = TEXT("/Game/__HyperAIStudioTests");
	const FString FixtureName = TEXT("NS_HyperAIEditOpsE2E");
	const FString PackageName = FixtureFolder / FixtureName;
	const FString TargetPath = PackageName + TEXT(".") + FixtureName;

	if (!FPackageName::DoesPackageExist(PackageName))
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Candidates;
		Registry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Candidates);
		const FAssetData* Source = Candidates.FindByPredicate([&](const FAssetData& Asset)
		{
			const FString Package = Asset.PackageName.ToString();
			return Package.StartsWith(TEXT("/Game/")) && !Package.StartsWith(FixtureFolder);
		});
		if (!Source)
		{
			AddInfo(TEXT("Skipped: no Niagara System under /Game to duplicate."));
			return true;
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Fixture = AssetTools.DuplicateAsset(FixtureName, FixtureFolder, Source->GetAsset());
		TestNotNull(TEXT("fixture duplicated"), Fixture);
		if (Fixture)
		{
			TestTrue(TEXT("fixture saved"), UEditorLoadingAndSavingUtils::SavePackages({ Fixture->GetOutermost() }, false));
		}
		AddWarning(FString::Printf(TEXT("Created fixture %s from %s. Run this test again in a new editor session to exercise the edit path."),
			*TargetPath, *Source->PackageName.ToString()));
		return true;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(TargetPath).TryLoad());
	if (!System || !System->HasAnyFlags(RF_WasLoaded))
	{
		AddWarning(TEXT("The fixture was created in this session and is not loaded from disk yet; run again in a new editor session."));
		return true;
	}
	// A freshly loaded System may not have compiled yet; planning requires a finished compile.
	System->RequestCompile(/*bForce=*/false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	FHyperAINiagaraInspectRequest InspectRequest;
	InspectRequest.TargetPath = TargetPath;
	InspectRequest.bIncludeTopology = true;
	InspectRequest.MaxOutputBytes = FHyperAIStudioNiagaraContracts::MaxOutputBytes;
	const FHyperAINiagaraInspectReport Inspect = FHyperAIStudioNiagaraContracts::Inspect(InspectRequest);
	FString CaptureCodes;
	for (const FHyperAINiagaraIssue& Issue : Inspect.Issues)
	{
		CaptureCodes += Issue.Code + TEXT(" ");
	}
	TestTrue(*FString::Printf(TEXT("fixture inspects cleanly (%s: %s)"), *Inspect.Status, *CaptureCodes), Inspect.bOk);
	TestFalse(TEXT("topology returned"), Inspect.Emitters.IsEmpty());

	// Late particle-update modules (colour, scale) can be disabled without breaking the stack; state modules cannot.
	FHyperAINiagaraEditOp Toggle = HyperAIStudio::Niagara::Tests::MakeOp(TEXT("set_module_enabled"));
	for (const FHyperAINiagaraEmitterTopology& Emitter : Inspect.Emitters)
	{
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			if (Stack.ScriptName != TEXT("ParticleUpdateScript") || !Toggle.ModuleName.IsEmpty())
			{
				continue;
			}
			for (int32 Index = Stack.Modules.Num() - 1; Index >= 0; --Index)
			{
				const FHyperAINiagaraModuleTopology& Module = Stack.Modules[Index];
				if (!Module.ModuleName.Contains(TEXT("State")) && !Module.ModuleName.Contains(TEXT("Initialize")))
				{
					Toggle.EmitterName = Emitter.EmitterName;
					Toggle.ScriptName = Stack.ScriptName;
					Toggle.ModuleName = Module.ModuleName;
					Toggle.bEnabled = !Module.bEnabled;
					break;
				}
			}
		}
	}
	if (!Inspect.bOk || Toggle.ModuleName.IsEmpty())
	{
		AddError(TEXT("The fixture has no ParticleUpdateScript module suitable for toggling, or did not inspect cleanly."));
		return false;
	}

	// This test approves nothing by hand, so it runs with the approval gate off and puts it back after.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bPreviousApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bPreviousApproval; };

	FHyperAINiagaraApplyPlanRequest Plan;
	Plan.TargetPath = TargetPath;
	Plan.ExpectedRevision = Inspect.Revision;
	Plan.Ops = { Toggle };
	const FHyperAINiagaraApplyPlanReport DryRun = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic),
		DryRun.bOk && DryRun.bTrustedPrepared);
	TestFalse(TEXT("dry run stages nothing"), DryRun.bStaged);
	if (!DryRun.bOk)
	{
		return false;
	}

	Plan.bDryRun = false;
	Plan.OperationId = TEXT("niagara-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAINiagaraApplyPlanReport Submitted = FHyperAIStudioNiagaraContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submission accepted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic),
		Submitted.bExecutionSubmitted);

	FHyperAIStudioTypedArtifactOperationStatus OperationStatus;
	FString StatusError;
	for (int32 Tick = 0; Tick < 400 && Submitted.bExecutionSubmitted; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.01f);
		if (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, OperationStatus, StatusError)
			&& OperationStatus.bTerminal)
		{
			break;
		}
	}
	TestTrue(*FString::Printf(TEXT("operation reaches a terminal state (%s)"), *OperationStatus.Status), OperationStatus.bTerminal);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *OperationStatus.Diagnostic),
		OperationStatus.Status, FString(TEXT("completed")));

	FHyperAINiagaraInspectRequest After = InspectRequest;
	const FHyperAINiagaraInspectReport Final = FHyperAIStudioNiagaraContracts::Inspect(After);
	bool bFlipped = false;
	for (const FHyperAINiagaraEmitterTopology& Emitter : Final.Emitters)
	{
		for (const FHyperAINiagaraScriptStackTopology& Stack : Emitter.ScriptStacks)
		{
			for (const FHyperAINiagaraModuleTopology& Module : Stack.Modules)
			{
				bFlipped |= Emitter.EmitterName == Toggle.EmitterName && Stack.ScriptName == Toggle.ScriptName
					&& Module.ModuleName == Toggle.ModuleName && Module.bEnabled == Toggle.bEnabled;
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("%s/%s/%s is now %s"), *Toggle.EmitterName, *Toggle.ScriptName, *Toggle.ModuleName,
		Toggle.bEnabled ? TEXT("enabled") : TEXT("disabled")), bFlipped);
	TestFalse(TEXT("the edit was saved"), System->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraPublicApiAuditTest,
	"HyperAIStudio.Niagara.UE58.PublicApiSignatures",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraPublicApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Experimental external-edit signatures are audited at compile time inside the gate's .cpp.
	using FExistingViewModelSignature = TSharedPtr<FNiagaraSystemViewModel>
		(FNiagaraEditorModule::*)(UNiagaraSystem*);
	FExistingViewModelSignature ExistingViewModelApi =
		&FNiagaraEditorModule::GetExistingViewModelForSystem;
	using FRequestCompileReturn = decltype(DeclVal<UNiagaraSystem&>().RequestCompile(
		false, nullptr, nullptr));
	using FPackageExistsReturn = decltype(DeclVal<IAssetRegistry&>().TryGetAssetPackageData(
		FName(), DeclVal<FAssetPackageData&>(), true));
	static_assert(std::is_same_v<FRequestCompileReturn, bool>,
		"UE 5.8 UNiagaraSystem::RequestCompile return type changed");
	static_assert(std::is_same_v<FPackageExistsReturn, UE::AssetRegistry::EExists>,
		"UE 5.8 non-blocking package tri-state changed");
	TestTrue(TEXT("public existing-ViewModel lookup API signature"), ExistingViewModelApi != nullptr);
	TestTrue(TEXT("external edit API present"), HyperAIStudio::Niagara::ExternalEditGate::IsApiAvailable());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraEffectTypeAndDataChannelTest,
	"HyperAIStudio.Niagara.Assets.EffectTypeAndDataChannel",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraEffectTypeAndDataChannelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Assets = HyperAIStudio::Niagara::AssetsGate;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString EffectPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/ET_HyperAI_%s.ET_HyperAI_%s"), *Suffix, *Suffix);
	const FString ChannelPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/DC_HyperAI_%s.DC_HyperAI_%s"), *Suffix, *Suffix);
	const FString SystemPath = TEXT("/Game/__HyperAIStudioTests/NS_Unused.NS_Unused");
	auto Op = [](const TCHAR* Kind, const FString& AssetPath, const TCHAR* Name = TEXT(""), const TCHAR* Value = TEXT(""), const TCHAR* ValueType = TEXT(""))
	{
		FHyperAINiagaraEditOp Result = HyperAIStudio::Niagara::Tests::MakeOp(Kind);
		Result.AssetPath = AssetPath;
		Result.Name = Name;
		Result.Value = Value;
		Result.ValueType = ValueType;
		return Result;
	};
	FString Status;
	FString Diagnostic;
	TSet<FString> Planned;
	TestTrue(TEXT("a new effect type path validates"), Assets::ValidateAssetOp(Op(TEXT("create_effect_type"), EffectPath), SystemPath, Planned, Status, Diagnostic));
	Planned.Add(EffectPath);
	TestTrue(TEXT("settings on an effect type created earlier in the plan validate"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_distance"), TEXT("5000")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("unknown settings are refused"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_fun"), TEXT("1")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("enum settings outside their set are refused"),
		Assets::ValidateAssetOp(Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("update_frequency"), TEXT("sometimes")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("an unknown channel type is refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("Energy:float"), TEXT("sideways")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("an unknown variable type is refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("Energy:double"), TEXT("islands")), SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("duplicate variables are refused"),
		Assets::ValidateAssetOp(Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("A:float,a:int32"), TEXT("global")), SystemPath, Planned, Status, Diagnostic));
	FHyperAINiagaraEditOp Addressed = Op(TEXT("create_effect_type"), ChannelPath);
	Addressed.EmitterName = TEXT("Sparks");
	TestFalse(TEXT("asset ops take no emitter address"), Assets::ValidateAssetOp(Addressed, SystemPath, Planned, Status, Diagnostic));
	TestFalse(TEXT("baking an unknown capture is refused"),
		Assets::ValidateAssetOp(Op(TEXT("bake_sim_cache"), ChannelPath, TEXT("capture-nope")), SystemPath, Planned, Status, Diagnostic));

	// The two private data channel properties are written by name; this fails when UE renames or retypes them.
	const FObjectProperty* ChannelProperty = FindFProperty<FObjectProperty>(UNiagaraDataChannelAsset::StaticClass(), TEXT("DataChannel"));
	const FArrayProperty* VariablesProperty = FindFProperty<FArrayProperty>(UNiagaraDataChannel::StaticClass(), TEXT("ChannelVariables"));
	TestNotNull(TEXT("UNiagaraDataChannelAsset::DataChannel still exists"), ChannelProperty);
	TestTrue(TEXT("UNiagaraDataChannel::ChannelVariables is still an array of FNiagaraDataChannelVariable"),
		VariablesProperty && CastField<FStructProperty>(VariablesProperty->Inner)
		&& CastField<FStructProperty>(VariablesProperty->Inner)->Struct == FNiagaraDataChannelVariable::StaticStruct());

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<UObject*> Created;
	ON_SCOPE_EXIT { if (!Created.IsEmpty()) ObjectTools::ForceDeleteObjects(Created, /*ShowConfirmation=*/false); };
	TestTrue(TEXT("create_effect_type applies: ") + Diagnostic, Assets::ApplyAssetOp(*System, Op(TEXT("create_effect_type"), EffectPath), Status, Diagnostic));
	UNiagaraEffectType* EffectType = Cast<UNiagaraEffectType>(FSoftObjectPath(EffectPath).ResolveObject());
	if (!TestNotNull(TEXT("the effect type exists"), EffectType)) return false;
	Created.Add(EffectType);
	TestTrue(TEXT("max_distance applies"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("max_distance"), TEXT("5000")), Status, Diagnostic));
	TestTrue(TEXT("update_frequency applies"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type_setting"), EffectPath, TEXT("update_frequency"), TEXT("low")), Status, Diagnostic));
	FHyperAINiagaraAssetRecord Record;
	TestTrue(TEXT("the effect type is described"), Assets::DescribeAsset(*EffectType, Record));
	TestEqual(TEXT("as an effect type"), Record.Kind, FString(TEXT("effect_type")));
	TestTrue(TEXT("with its distance budget"), Record.Details.ContainsByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("max_distance") && Row.Value == TEXT("5000.0");
	}));
	TestEqual(TEXT("and update frequency"), EffectType->UpdateFrequency, ENiagaraScalabilityUpdateFrequency::Low);
	TestTrue(TEXT("set_effect_type assigns it"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type"), EffectPath), Status, Diagnostic));
	TestEqual(TEXT("the System reports it"), Assets::GetEffectTypePath(*System), EffectPath);
	TestTrue(TEXT("an empty path clears it"), Assets::ApplyAssetOp(*System, Op(TEXT("set_effect_type"), FString()), Status, Diagnostic));
	TestTrue(TEXT("and it is gone"), Assets::GetEffectTypePath(*System).IsEmpty());

	TestTrue(TEXT("create_data_channel applies: ") + Diagnostic, Assets::ApplyAssetOp(*System,
		Op(TEXT("create_data_channel"), ChannelPath, TEXT(""), TEXT("ImpactPosition:position,Energy:float,Tint:linear_color"), TEXT("islands")), Status, Diagnostic));
	UObject* Channel = FSoftObjectPath(ChannelPath).ResolveObject();
	if (!TestNotNull(TEXT("the data channel exists"), Channel)) return false;
	Created.Add(Channel);
	Record = {};
	TestTrue(TEXT("the data channel is described"), Assets::DescribeAsset(*Channel, Record));
	TestTrue(TEXT("as islands"), Record.Details.ContainsByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("type") && Row.Value == TEXT("islands");
	}));
	TestEqual(TEXT("with its three variables"), Record.Details.FilterByPredicate([](const FHyperAINiagaraKeyValue& Row)
	{
		return Row.Key == TEXT("variable");
	}).Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraSimCacheTest,
	"HyperAIStudio.Niagara.Assets.SimCacheCaptureBakeAndRegression",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraSimCacheTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Assets = HyperAIStudio::Niagara::AssetsGate;
	// Captures need a CPU System under /Game. An unsaved copy of one of Niagara's own CPU templates serves.
	UNiagaraSystem* Template = nullptr;
	for (const TCHAR* Candidate : {TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"),
		TEXT("/Niagara/DefaultAssets/Templates/Systems/RadialBurst.RadialBurst"),
		TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion")})
	{
		UNiagaraSystem* Loaded = Cast<UNiagaraSystem>(FSoftObjectPath(Candidate).TryLoad());
		if (Loaded && !Loaded->HasAnyGPUEmitters())
		{
			Template = Loaded;
			break;
		}
	}
	if (!Template)
	{
		AddInfo(TEXT("Skipped: no CPU Niagara template System is available to capture."));
		return true;
	}
	const FString FixtureName = TEXT("NS_HyperAICapture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	UPackage* FixturePackage = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + FixtureName));
	UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(Template, FixturePackage, FName(*FixtureName));
	if (!TestNotNull(TEXT("the capture fixture duplicates"), System)) return false;
	System->SetFlags(RF_Public | RF_Standalone);
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({System}, /*ShowConfirmation=*/false); };
	const FString SystemPath = System->GetPathName();
	// A golden comparison only means something for a repeatable simulation, so the copy turns determinism on
	// for the System and every emitter.
	if (FBoolProperty* Determinism = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bDeterminism")))
	{
		Determinism->SetPropertyValue_InContainer(System, true);
	}
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData()) Data->bDeterminism = true;
	}
	System->RequestCompile(/*bForce=*/true);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);

	auto Capture = [&](const FString& Golden, const float Delta = 1.f / 60.f)
	{
		FHyperAINiagaraValidateRequest Request;
		Request.TargetPath = SystemPath;
		Request.Policy = TEXT("sim_cache_capture");
		Request.CaptureFrames = 12;
		Request.CaptureDeltaSeconds = Delta;
		FHyperAINiagaraValidateReport Report = FHyperAIStudioNiagaraContracts::Validate(Request);
		TestEqual(*FString::Printf(TEXT("capture starts (%s)"), *Report.Diagnostic), Report.Status, FString(TEXT("capture_started")));
		Request.CaptureId = Report.SimCache.CaptureId;
		Request.GoldenSimCachePath = Golden;
		for (int32 Tick = 0; Tick < 500 && Report.SimCache.State == TEXT("capturing"); ++Tick)
		{
			FPlatformProcess::Sleep(0.01f);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			Report = FHyperAIStudioNiagaraContracts::Validate(Request);
		}
		return Report;
	};

	const FHyperAINiagaraValidateReport First = Capture(FString());
	TestEqual(*FString::Printf(TEXT("the capture completes (%s)"), *First.SimCache.Error), First.SimCache.State, FString(TEXT("complete")));
	TestEqual(TEXT("every frame is recorded"), First.SimCache.FramesCaptured, 12);
	TestFalse(TEXT("emitters are listed"), First.SimCache.EmitterNames.IsEmpty());
	if (First.SimCache.State != TEXT("complete")) return false;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString GoldenPath = FString::Printf(TEXT("/Game/__HyperAIStudioTests/SC_HyperAI_%s.SC_HyperAI_%s"), *Suffix, *Suffix);
	FHyperAINiagaraEditOp Bake = HyperAIStudio::Niagara::Tests::MakeOp(TEXT("bake_sim_cache"));
	Bake.Name = First.SimCache.CaptureId;
	Bake.AssetPath = GoldenPath;
	FString Status;
	FString Diagnostic;
	TestTrue(TEXT("a fresh capture of this System can be baked: ") + Diagnostic,
		Assets::ValidateAssetOp(Bake, SystemPath, {}, Status, Diagnostic));
	TestTrue(TEXT("baking applies: ") + Diagnostic, Assets::ApplyAssetOp(*System, Bake, Status, Diagnostic));
	UObject* Golden = FSoftObjectPath(GoldenPath).ResolveObject();
	if (!TestNotNull(TEXT("the golden cache exists"), Golden)) return false;
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({Golden}, /*ShowConfirmation=*/false); };
	FHyperAINiagaraAssetRecord Record;
	TestTrue(TEXT("the golden cache is described"), Assets::DescribeAsset(*Golden, Record) && Record.Kind == TEXT("sim_cache"));
	TestFalse(TEXT("a baked capture cannot be baked twice"), Assets::ValidateAssetOp(Bake, SystemPath, {}, Status, Diagnostic));

	const FHyperAINiagaraValidateReport Second = Capture(GoldenPath);
	TestTrue(TEXT("the second capture is compared with the golden cache"), Second.SimCache.bCompared);
	TestTrue(TEXT("the copy counts as fully deterministic"), Second.SimCache.bDeterministic);
	TestTrue(TEXT("an unchanged deterministic System matches its golden cache: ") + FString::Join(Second.SimCache.Differences, TEXT(" | ")),
		Second.SimCache.bMatch);
	TestEqual(TEXT("and validate says so"), Second.Status, FString(TEXT("matches_golden")));
	const FHyperAINiagaraValidateReport Changed = Capture(GoldenPath, 1.f / 30.f);
	TestFalse(TEXT("a changed simulation no longer matches"), Changed.SimCache.bMatch);
	TestFalse(TEXT("and the differences are listed"), Changed.SimCache.Differences.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
