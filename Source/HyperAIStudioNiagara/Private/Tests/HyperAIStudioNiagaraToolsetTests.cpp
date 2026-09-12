// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioNiagaraToolset.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraSystem.h"
#include "NiagaraValidationRuleSet.h"
#include "NiagaraValidationRules.h"

#include <type_traits>

namespace HyperAIStudio::Niagara::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter;

	FString HashOf(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FHyperAIStudioDomainDispatchContext MakeMutationContext()
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
		Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
		return Context;
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
	TestEqual(TEXT("blocking plugin requirement group"),
		FString(FHyperAIStudioNiagaraContracts::PluginRequirementGroupId),
		FString(TEXT("niagara_plugin")));
	TestEqual(TEXT("blocking backend requirement group"),
		FString(FHyperAIStudioNiagaraContracts::BackendRequirementGroupId),
		FString(TEXT("niagara_backend")));
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
	TestFalse(TEXT("unlisted exact cohort is not production-admitted"),
		FHyperAIStudioNiagaraContracts::IsRegistrationAllowed(false));
	TSet<FString> Tools;
	TSet<FString> Variants;
	TSet<FString> RequestTypes;
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		Tools.Add(Variant.ToolName);
		Variants.Add(Variant.VariantId);
		RequestTypes.Add(Variant.RequestTypeId);
		TestTrue(TEXT("request schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(
				Variant.RequestSchemaFingerprint));
		TestTrue(TEXT("result schema canonical"),
			FHyperAIStudioNiagaraContracts::IsCanonicalSha256(
				Variant.ResultSchemaFingerprint));
	}
	TestEqual(TEXT("unique tools"), Tools.Num(), 3);
	TestEqual(TEXT("unique variants"), Variants.Num(), 3);
	TestEqual(TEXT("unique request DTOs"), RequestTypes.Num(), 3);
	TestEqual(TEXT("inspect read safety"), Descriptor.Variants[0].Safety,
		EHyperAIStudioDomainSafety::Read);
	TestEqual(TEXT("rename edit safety"), Descriptor.Variants[1].Safety,
		EHyperAIStudioDomainSafety::Edit);
	TestEqual(TEXT("rename variant makes no full-reference claim"),
		Descriptor.Variants[1].VariantId,
		FString(TEXT("user_parameter_rename_preflight.v1")));
	TestEqual(TEXT("validate read safety"), Descriptor.Variants[2].Safety,
		EHyperAIStudioDomainSafety::Read);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraDelegationTest,
	"HyperAIStudio.Niagara.Contracts.AllEpicCallablesDelegated",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraDelegationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAINiagaraCapabilityStatus> Matrix =
		FHyperAIStudioNiagaraContracts::GetCapabilityMatrix();
	TestEqual(TEXT("one explicit Niagara matrix row"), Matrix.Num(), 1);
	if (Matrix.IsEmpty()) return false;
	const TArray<FString>& Delegated = Matrix[0].DelegatedEpicCallables;
	TestEqual(TEXT("all 56 Epic Niagara callables delegated"), Delegated.Num(), 56);
	TSet<FString> Unique;
	for (const FString& Name : Delegated) Unique.Add(Name);
	TestEqual(TEXT("no duplicate delegation entries"), Unique.Num(), 56);
	TestTrue(TEXT("Epic compile-state read delegated"),
		Unique.Contains(TEXT("GetSystemCompileState")));
	TestTrue(TEXT("Epic mutation primitive delegated"),
		Unique.Contains(TEXT("AddUserVariables")));
	TestTrue(TEXT("Epic fix execution delegated"),
		Unique.Contains(TEXT("ApplyStackIssueFix")));
	TestFalse(TEXT("full-reference rename is absent from Epic 56"),
		Unique.Contains(TEXT("RenameUserParameterForSystem")));
	TestTrue(TEXT("native virtual rule blocker is machine-readable"),
		Matrix[0].UnsupportedCases.Contains(TEXT("native_rule_execution_not_hard_bounded")));
	TestTrue(TEXT("full-reference inventory blocker is machine-readable"),
		Matrix[0].UnsupportedCases.Contains(TEXT("full_reference_inventory_incomplete")));
	TestFalse(TEXT("old reference-safe claim is removed"),
		Matrix[0].SupportedCases.Contains(
			TEXT("single_reference_safe_user_parameter_rename_dry_run")));
	TestFalse(TEXT("independent validation is not admitted as implemented"),
		Matrix[0].bIndependentValidationImplemented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraNoLoadValidationAuthorityTest,
	"HyperAIStudio.Niagara.Validation.LoadedOnlyNativeAuthority",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraNoLoadValidationAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
	{
		FHyperAINiagaraValidateRequest Request;
		Request.TargetPath = TEXT("/Game/VFX/NS_Unloaded.NS_Unloaded");
		const FHyperAINiagaraValidateReport Disabled =
			FHyperAIStudioNiagaraContracts::Validate(Request);
		TestEqual(TEXT("environment-unset gate"), Disabled.Status,
			FString(TEXT("source_candidate_dev_mode_required")));
		TestEqual(TEXT("environment-unset executes no native rule"),
			Disabled.ExecutedRuleCount, 0);
	}
	const TSoftObjectPtr<UNiagaraValidationRuleSet> UnloadedRuleSet{
		FSoftObjectPath(TEXT("/Game/__HyperAIStudioNiagara_UnloadedRuleSet.__HyperAIStudioNiagara_UnloadedRuleSet"))};
	TestNull(TEXT("unloaded soft rule set fails closed without loading"),
		FHyperAIStudioNiagaraContracts::ResolveAlreadyLoadedRuleSet(UnloadedRuleSet));
	const TArray<const UClass*> NativeClasses = {
		UNiagaraValidationRule_NoWarmupTime::StaticClass(),
		UNiagaraValidationRule_NoEvents::StaticClass(),
		UNiagaraValidationRule_FixedGPUBoundsSet::StaticClass(),
		UNiagaraValidationRule_EmitterCount::StaticClass(),
		UNiagaraValidationRule_RendererCount::StaticClass(),
		UNiagaraValidationRule_BannedRenderers::StaticClass(),
		UNiagaraValidationRule_Lightweight::StaticClass(),
		UNiagaraValidationRule_BannedModules::StaticClass(),
		UNiagaraValidationRule_BannedDataInterfaces::StaticClass(),
		UNiagaraValidationRule_RendererSortingEnabled::StaticClass(),
		UNiagaraValidationRule_GpuUsage::StaticClass(),
		UNiagaraValidationRule_RibbonRenderer::StaticClass(),
		UNiagaraValidationRule_InvalidEffectType::StaticClass(),
		UNiagaraValidationRule_HasEffectType::StaticClass(),
		UNiagaraValidationRule_LWC::StaticClass(),
		UNiagaraValidationRule_NoOpaqueRenderMaterial::StaticClass(),
		UNiagaraValidationRule_NoFixedDeltaTime::StaticClass(),
		UNiagaraValidationRule_SimulationStageBudget::StaticClass(),
		UNiagaraValidationRule_TickDependencyCheck::StaticClass(),
		UNiagaraValidationRule_UserDataInterfaces::StaticClass(),
		UNiagaraValidationRule_SingletonModule::StaticClass(),
		UNiagaraValidationRule_NoMapForOnCpu::StaticClass(),
		UNiagaraValidationRule_ModuleSimTargetRestriction::StaticClass(),
		UNiagaraValidationRule_MaterialUsage::StaticClass(),
		UNiagaraValidationRule_RequireLatestParentEmitterVersion::StaticClass(),
		UNiagaraValidationRule_RequireParentEmitter::StaticClass()};
	TestEqual(TEXT("sealed UE 5.8 class count"), NativeClasses.Num(), 26);
	TSet<const UClass*> Unique;
	for (const UClass* NativeClass : NativeClasses)
	{
		Unique.Add(NativeClass);
		TestTrue(TEXT("native class is admitted exactly"),
			FHyperAIStudioNiagaraContracts::IsSealedNativeValidationRuleClass(NativeClass));
	}
	TestEqual(TEXT("native class list has no duplicates"), Unique.Num(), 26);
	TestFalse(TEXT("abstract base is not executable authority"),
		FHyperAIStudioNiagaraContracts::IsSealedNativeValidationRuleClass(
			UNiagaraValidationRule::StaticClass()));
	TestFalse(TEXT("non-rule class is rejected"),
		FHyperAIStudioNiagaraContracts::IsSealedNativeValidationRuleClass(
			UNiagaraValidationRuleSet::StaticClass()));
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
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(
			TEXT("/Game/VFX/NS_Test.NS_Test")));
	TestFalse(TEXT("folder path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(TEXT("/Game/VFX")));
	TestFalse(TEXT("subobject path rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(
			TEXT("/Game/VFX/NS_Test.NS_Test:Emitter")));
	TestFalse(TEXT("wildcard rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalProjectObjectPath(
			TEXT("/Game/VFX/*.NS_Test")));
	TestTrue(TEXT("lowercase sha accepted"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(
			HyperAIStudio::Niagara::Tests::HashOf(TEXT('a'))));
	TestFalse(TEXT("uppercase sha rejected"),
		FHyperAIStudioNiagaraContracts::IsCanonicalSha256(
			HyperAIStudio::Niagara::Tests::HashOf(TEXT('A'))));
	TestTrue(TEXT("User namespace accepted"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("User.SpawnRate")));
	TestFalse(TEXT("non-user namespace rejected"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("System.SpawnRate")));
	TestFalse(TEXT("empty segment rejected"),
		FHyperAIStudioNiagaraContracts::IsUserParameterName(TEXT("User..SpawnRate")));
	FHyperAINiagaraUserParameterIdentity Source;
	Source.VariableGuid = TEXT("12345678-1234-1234-1234-1234567890ab");
	Source.Name = TEXT("User.Old");
	Source.TypeFingerprint = HyperAIStudio::Niagara::Tests::HashOf(TEXT('2'));
	FHyperAINiagaraUserParameterRename Rename;
	Rename.ExpectedVariableGuid = Source.VariableGuid;
	Rename.OldName = Source.Name;
	Rename.NewName = TEXT("User.New");
	Rename.ExpectedTypeFingerprint = Source.TypeFingerprint;
	const FHyperAINiagaraUserParameterIdentity* Matched = nullptr;
	FString RenameError;
	TArray<FHyperAINiagaraUserParameterIdentity> Snapshot{Source};
	TestTrue(TEXT("exact rename identity accepted"),
		FHyperAIStudioNiagaraContracts::ValidateRenameIdentityAgainstSnapshot(
			Snapshot, Rename, Matched, RenameError));
	TestTrue(TEXT("source identity returned"), Matched == &Snapshot[0]);
	FHyperAINiagaraUserParameterIdentity Duplicate = Source;
	Duplicate.VariableGuid = TEXT("87654321-4321-4321-4321-ba0987654321");
	Duplicate.Name = Rename.NewName;
	Snapshot.Add(Duplicate);
	TestFalse(TEXT("duplicate destination rejected"),
		FHyperAIStudioNiagaraContracts::ValidateRenameIdentityAgainstSnapshot(
			Snapshot, Rename, Matched, RenameError));
	TestEqual(TEXT("duplicate destination status"), RenameError,
		FString(TEXT("destination_parameter_exists")));
	Snapshot.Pop();
	Rename.ExpectedTypeFingerprint = HyperAIStudio::Niagara::Tests::HashOf(TEXT('3'));
	TestFalse(TEXT("type mismatch rejected"),
		FHyperAIStudioNiagaraContracts::ValidateRenameIdentityAgainstSnapshot(
			Snapshot, Rename, Matched, RenameError));
	TestEqual(TEXT("type mismatch status"), RenameError,
		FString(TEXT("variable_identity_mismatch")));
	Rename.ExpectedTypeFingerprint = Source.TypeFingerprint;
	Rename.NewName = TEXT("System.Invalid");
	TestFalse(TEXT("invalid destination namespace rejected"),
		FHyperAIStudioNiagaraContracts::ValidateRenameIdentityAgainstSnapshot(
			Snapshot, Rename, Matched, RenameError));
	TestEqual(TEXT("invalid name status"), RenameError,
		FString(TEXT("invalid_rename_identity")));
	TestEqual(TEXT("page bound"), FHyperAIStudioNiagaraContracts::MaxPageSize, 64);
	TestEqual(TEXT("parameter bound"), FHyperAIStudioNiagaraContracts::MaxParameters, 256);
	TestEqual(TEXT("graph-node bound"), FHyperAIStudioNiagaraContracts::MaxGraphNodes, 4096);
	TestEqual(TEXT("native-rule bound"), FHyperAIStudioNiagaraContracts::MaxValidationRules, 128);
	TestFalse(TEXT("store plus graphs cannot prove UE 5.8 full rename closure"),
		FHyperAIStudioNiagaraContracts::AreFullReferenceDomainsProven(
			true, true, false, false, false, false, false));
	TestFalse(TEXT("all domains still fail when reflection materializes before bounds"),
		FHyperAIStudioNiagaraContracts::AreFullReferenceDomainsProven(
			true, true, true, true, true, true, false));
	TestTrue(TEXT("future proof requires every domain and pre-materialization bound"),
		FHyperAIStudioNiagaraContracts::AreFullReferenceDomainsProven(
			true, true, true, true, true, true, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraCloneAndAdapterTest,
	"HyperAIStudio.Niagara.Mutation.DeepCloneAndNoEffectAdapter",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraCloneAndAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Niagara::Tests;
	const TSharedRef<FHyperAIStudioNiagaraRenamePayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioNiagaraRenamePayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/VFX/NS_Test.NS_Test");
	Payload->BaseRevision = HashOf(TEXT('1'));
	Payload->ExpectedVariableGuid = TEXT("12345678-1234-1234-1234-1234567890ab");
	Payload->OldName = TEXT("User.Old");
	Payload->NewName = TEXT("User.New");
	Payload->TypeFingerprint = HashOf(TEXT('2'));
	Payload->DefaultFingerprint = HashOf(TEXT('3'));
	Payload->SemanticFingerprint =
		FHyperAIStudioNiagaraContracts::ComputeRenameSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone type preserved"), Clone->GetTypeId(), Payload->GetTypeId());
	TestEqual(TEXT("clone schema preserved"), Clone->GetSchemaFingerprint(),
		Payload->GetSchemaFingerprint());
	TestEqual(TEXT("clone semantic seal preserved"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	TestEqual(TEXT("clone bounded size preserved"), Clone->GetBoundedByteSize(),
		Payload->GetBoundedByteSize());

	FHyperAIStudioNiagaraDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(
		MakeMutationContext(), *Payload);
	TestEqual(TEXT("edit rejected before effect"), Result.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("exact async host status"), Result.StatusCode,
		FString(FHyperAIStudioNiagaraContracts::NonDryCallableState));
	TestFalse(TEXT("no mutation result claimed"), Result.Payload.IsValid());

	Payload->NewName = TEXT("User.Drifted");
	const FHyperAIStudioDomainAdapterResult Drifted = Adapter.Execute(
		MakeMutationContext(), *Payload);
	TestEqual(TEXT("semantic drift rejected"), Drifted.StatusCode,
		FString(TEXT("typed_payload_drift")));
	TestEqual(TEXT("drift rejected before effect"), Drifted.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	FHyperAIStudioNiagaraFreshVerifier Verifier(
		Adapter.GetDescriptor().AdapterFingerprint);
	FHyperAIStudioNiagaraMutationResultPayload MutationResult;
	MutationResult.Phase = TEXT("completed");
	MutationResult.Revision = HashOf(TEXT('4'));
	MutationResult.bValid = true;
	FString PostconditionHash;
	FString VerifyError;
	TestFalse(TEXT("fresh PASS blocked until independent bounded validator exists"),
		Verifier.VerifyFreshExact(*Clone, MutationResult, PostconditionHash, VerifyError));
	TestEqual(TEXT("fresh verifier machine blocker"), VerifyError,
		FString(TEXT("native_rule_execution_not_hard_bounded")));
	TestTrue(TEXT("blocked verifier emits no postcondition hash"), PostconditionHash.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNiagaraPublicApiAuditTest,
	"HyperAIStudio.Niagara.UE58.PublicApiSignatures",
	HyperAIStudio::Niagara::Tests::Flags)

bool FHyperAIStudioNiagaraPublicApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using FRenameSignature = void (*)(UNiagaraSystem&, const FNiagaraVariableBase&,
		const FNiagaraVariableBase&);
	FRenameSignature RenameApi =
		&FNiagaraEditorUtilities::UserParameters::RenameUserParameterForSystem;
	using FExistingViewModelSignature = TSharedPtr<FNiagaraSystemViewModel>
		(FNiagaraEditorModule::*)(UNiagaraSystem*);
	FExistingViewModelSignature ExistingViewModelApi =
		&FNiagaraEditorModule::GetExistingViewModelForSystem;
	using FCompileStateSignature = void (*)(UNiagaraSystem*,
		FNiagaraExt_SystemCompileState&, FNiagaraExternalEditContext&);
	FCompileStateSignature CompileStateApi =
		&UNiagaraExternalEditUtilities::GetSystemCompileState;
	using FRequestCompileReturn = decltype(DeclVal<UNiagaraSystem&>().RequestCompile(
		false, nullptr, nullptr));
	using FPackageExistsReturn = decltype(DeclVal<IAssetRegistry&>().TryGetAssetPackageData(
		FName(), DeclVal<FAssetPackageData&>(), true));
	static_assert(std::is_same_v<FRequestCompileReturn, bool>,
		"UE 5.8 UNiagaraSystem::RequestCompile return type changed");
	static_assert(std::is_same_v<FPackageExistsReturn, UE::AssetRegistry::EExists>,
		"UE 5.8 non-blocking package tri-state changed");
	TestTrue(TEXT("public full-reference rename API signature"), RenameApi != nullptr);
	TestTrue(TEXT("public existing-ViewModel lookup API signature"), ExistingViewModelApi != nullptr);
	TestTrue(TEXT("public nonblocking compile-state API signature"), CompileStateApi != nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
