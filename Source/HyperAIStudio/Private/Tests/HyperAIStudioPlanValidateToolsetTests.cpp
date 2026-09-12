// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPlanValidateToolset.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioTypedPlan.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::PlanValidate::Tests
{
	FString Sha(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FString CanonicalProjectId()
	{
		return Sha(TEXT('a'));
	}

	FString CapabilityHash()
	{
		return FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry().ComputeCapabilityHash();
	}

	FString ReadPlan(
		const bool bDryRun = true,
		const FString& OperationId = FString(),
		const FString& Subject = TEXT("/Game/Test"),
		const FString& Capability = FString())
	{
		const FString OperationField = OperationId.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\"operation_id\":\"%s\""), *OperationId);
		const FString EffectiveCapability = Capability.IsEmpty() ? CapabilityHash() : Capability;
		return FString::Printf(TEXT(R"json({
  "schema":"hyperai.plan.v1",
  "dry_run":%s,
  "capability_hash":"%s"%s,
  "budget":{"deadline_ms":10000,"max_steps":4,"max_mutations":0,"max_native_operations":8,"max_game_thread_ms":100,"max_output_bytes":8192},
  "steps":[{
    "id":"inspect",
    "operation":"foundation.inspect",
    "depends_on":[],
    "arguments":{"subject":"%s","fields":["name","class"]},
    "preconditions":[{"kind":"object_exists","target":"%s"}],
    "effects":[],
    "budget":{"max_native_operations":1,"max_game_thread_ms":2,"max_output_bytes":1024}
  }]
})json"),
			bDryRun ? TEXT("true") : TEXT("false"),
			*EffectiveCapability,
			*OperationField,
			*Subject,
			*Subject);
	}

	FString DestructiveDryRunPlan()
	{
		return FString::Printf(TEXT(R"json({
  "schema":"hyperai.plan.v1",
  "dry_run":true,
  "capability_hash":"%s",
  "operation_id":"operation-delete-001",
  "budget":{"deadline_ms":10000,"max_steps":4,"max_mutations":1,"max_native_operations":16,"max_game_thread_ms":100,"max_output_bytes":8192},
  "steps":[{
    "id":"delete_asset",
    "operation":"asset.delete",
    "depends_on":[],
    "arguments":{"asset_path":"/Game/Old"},
    "preconditions":[{"kind":"object_exists","target":"/Game/Old"}],
    "effects":[{"kind":"object_deleted","target":"/Game/Old","validator_id":"asset.absence"}],
    "budget":{"max_native_operations":2,"max_game_thread_ms":10,"max_output_bytes":1024}
  }]
})json"), *CapabilityHash());
	}

	FString FirstCode(const FHyperAIPlanValidateReport& Report)
	{
		return Report.Diagnostics.IsEmpty() ? FString() : Report.Diagnostics[0].Code;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanValidateStrictContractTest,
	"HyperAIStudio.NativeTools.PlanValidate.StrictContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanValidateStrictContractTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanValidate::Tests;
	const FString ProjectId = CanonicalProjectId();
	const FString ValidJson = ReadPlan();

	const FHyperAIPlanValidateReport Valid =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(ValidJson, FString(), ProjectId);
	TestTrue(TEXT("A strict bounded plan validates"), Valid.bOk);
	TestEqual(TEXT("Valid status"), Valid.Status, TEXT("valid"));
	TestEqual(TEXT("Registry fingerprint is exact"), Valid.CapabilityFingerprint, CapabilityHash());
	TestEqual(TEXT("One step is reported"), Valid.StepCount, 1);
	TestEqual(TEXT("One precondition is reported"), Valid.Preconditions.Num(), 1);
	TestEqual(TEXT("No effect is invented"), Valid.Effects.Num(), 0);
	const FHyperAIPlanValidateReport Sealed =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(
			ValidJson, Valid.CanonicalPlanHash, ProjectId);
	TestTrue(TEXT("A matching optional expected hash is accepted"), Sealed.bOk);
	TestTrue(TEXT("Expected-hash presence is explicit"), Sealed.bExpectedPlanHashProvided);
	TestTrue(TEXT("Expected-hash match is explicit"), Sealed.bExpectedPlanHashMatched);
	TestEqual(TEXT("Sealed validation preserves the canonical hash"),
		Sealed.CanonicalPlanHash, Valid.CanonicalPlanHash);

	const FString DuplicateJson = ValidJson.Replace(
		TEXT("\"schema\":\"hyperai.plan.v1\""),
		TEXT("\"schema\":\"hyperai.plan.v1\",\"schema\":\"hyperai.plan.v1\""));
	const FHyperAIPlanValidateReport Duplicate =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(DuplicateJson, FString(), ProjectId);
	TestFalse(TEXT("Duplicate JSON keys fail closed"), Duplicate.bOk);
	TestEqual(TEXT("Duplicate-key diagnostic"), FirstCode(Duplicate), TEXT("duplicate_json_key"));

	const FString UnknownFieldJson = ValidJson.Replace(
		TEXT("\"dry_run\":true"),
		TEXT("\"dry_run\":true,\"raw_tool\":\"execute_script\""));
	const FHyperAIPlanValidateReport UnknownField =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(UnknownFieldJson, FString(), ProjectId);
	TestFalse(TEXT("Unknown schema fields fail closed"), UnknownField.bOk);
	TestEqual(TEXT("Unknown-field diagnostic"), FirstCode(UnknownField), TEXT("unknown_field"));

	const FString UnknownOperationJson = ValidJson.Replace(
		TEXT("foundation.inspect"), TEXT("raw.execute_tool"));
	const FHyperAIPlanValidateReport UnknownOperation =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(UnknownOperationJson, FString(), ProjectId);
	TestFalse(TEXT("Unknown operations cannot dispatch"), UnknownOperation.bOk);
	TestEqual(TEXT("Allowlist diagnostic"), FirstCode(UnknownOperation), TEXT("operation_not_allowlisted"));

	const FHyperAIPlanValidateReport CapabilityDrift =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(
			ReadPlan(true, FString(), TEXT("/Game/Test"), Sha(TEXT('b'))), FString(), ProjectId);
	TestFalse(TEXT("Capability drift fails closed"), CapabilityDrift.bOk);
	TestEqual(TEXT("Capability drift diagnostic"), FirstCode(CapabilityDrift), TEXT("capability_mismatch"));

	const FString Oversized = FString::ChrN(FHyperAIStudioPlanLimits::MaxPlanJsonBytes + 1, TEXT('x'));
	const FHyperAIPlanValidateReport Bounds =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(Oversized, FString(), ProjectId);
	TestFalse(TEXT("Oversized plans fail before JSON parsing"), Bounds.bOk);
	TestEqual(TEXT("Size diagnostic"), FirstCode(Bounds), TEXT("plan_size"));

	const FHyperAIPlanValidateReport MissingProject =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(ValidJson, FString(), FString());
	TestFalse(TEXT("Validation is unavailable without a strong project identity"), MissingProject.bOk);
	TestEqual(TEXT("Project binding diagnostic"), FirstCode(MissingProject), TEXT("project_identity_unavailable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanValidateHashSafetyTest,
	"HyperAIStudio.NativeTools.PlanValidate.HashSafetyNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanValidateHashSafetyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanValidate::Tests;
	TestEqual(TEXT("Local SHA-256 matches the NIST abc vector"),
		FHyperAIStudioPlanValidateContracts::ComputeBoundedSha256(TEXT("abc")),
		TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	TestTrue(TEXT("Local SHA-256 rejects inputs beyond its hard UTF-8 bound"),
		FHyperAIStudioPlanValidateContracts::ComputeBoundedSha256(
			FString::ChrN(FHyperAIStudioPlanValidateContracts::MaxHashInputUtf8Bytes + 1, TEXT('a'))).IsEmpty());
	FString MalformedUtf16;
	MalformedUtf16.AppendChar(static_cast<TCHAR>(0xd800));
	TestTrue(TEXT("Local SHA-256 rejects malformed UTF-16"),
		FHyperAIStudioPlanValidateContracts::ComputeBoundedSha256(MalformedUtf16).IsEmpty());
	const FString ProjectId = CanonicalProjectId();
	const FHyperAIPlanValidateReport Preview =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(ReadPlan(true), FString(), ProjectId);
	const FString ReorderedJson = ReadPlan(true).Replace(
		TEXT("\"arguments\":{\"subject\":\"/Game/Test\",\"fields\":[\"name\",\"class\"]}"),
		TEXT("\"arguments\":{\"fields\":[\"name\",\"class\"],\"subject\":\"/Game/Test\"}"));
	const FHyperAIPlanValidateReport Reordered =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(ReorderedJson, FString(), ProjectId);
	const FHyperAIPlanValidateReport ExecuteModeRead =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(
			ReadPlan(false, TEXT("operation-read-001")), FString(), ProjectId);
	TestTrue(TEXT("Preview plan validates"), Preview.bOk);
	TestTrue(TEXT("Reordered JSON object fields validate"), Reordered.bOk);
	TestEqual(TEXT("Canonical plan hash ignores JSON object field order"),
		Preview.CanonicalPlanHash, Reordered.CanonicalPlanHash);
	TestTrue(TEXT("Execute-mode read artifact can be inspected without execution"), ExecuteModeRead.bOk);
	TestEqual(TEXT("Plan hash ignores dry-run mode and operation id"),
		Preview.CanonicalPlanHash, ExecuteModeRead.CanonicalPlanHash);
	TestNotEqual(TEXT("Project binding also seals the operation id"),
		Preview.ProjectBindingFingerprint, ExecuteModeRead.ProjectBindingFingerprint);
	TestTrue(TEXT("Validation callable remains dry-run"), ExecuteModeRead.bDryRun);
	TestTrue(TEXT("Validation callable declares no mutation"), ExecuteModeRead.bNoMutation);
	TestFalse(TEXT("No execution is performed"), ExecuteModeRead.bExecutionPerformed);
	TestTrue(TEXT("Selected backend is explicitly validation-only"),
		ExecuteModeRead.bSelectedBackendValidationOnly);
	TestFalse(TEXT("No authorization is issued"), ExecuteModeRead.bAuthorizationIssued);
	TestTrue(TEXT("Input dry-run mode was parsed"), ExecuteModeRead.bInputPlanDryRunKnown);
	TestFalse(TEXT("Input execute-mode is reported honestly"), ExecuteModeRead.bInputPlanDryRun);

	const FString TamperedJson = ReadPlan(true, FString(), TEXT("/Game/Tampered"));
	const FHyperAIPlanValidateReport Tampered =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(
			TamperedJson, Preview.CanonicalPlanHash, ProjectId);
	TestFalse(TEXT("A separately sealed hash detects semantic tampering"), Tampered.bOk);
	TestFalse(TEXT("Mismatched expected hash is explicit"), Tampered.bExpectedPlanHashMatched);
	TestEqual(TEXT("Tamper diagnostic"), FirstCode(Tampered), TEXT("expected_plan_hash_mismatch"));
	TestNotEqual(TEXT("Tampered semantics produce a different canonical hash"),
		Tampered.CanonicalPlanHash, Preview.CanonicalPlanHash);
	TestTrue(TEXT("A failed expected-hash seal does not issue a project binding"),
		Tampered.ProjectBindingFingerprint.IsEmpty());

	const FHyperAIPlanValidateReport InvalidExpected =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(ReadPlan(), TEXT("SHA256:BAD"), ProjectId);
	TestFalse(TEXT("Non-canonical expected hashes fail closed"), InvalidExpected.bOk);
	TestEqual(TEXT("Expected-hash schema diagnostic"), FirstCode(InvalidExpected), TEXT("invalid_expected_plan_hash"));

	const FHyperAIPlanValidateReport Destructive =
		FHyperAIStudioPlanValidateContracts::ValidateForProject(
			DestructiveDryRunPlan(), FString(), ProjectId);
	TestTrue(TEXT("A destructive dry-run can be validated"), Destructive.bOk);
	TestEqual(TEXT("Derived safety is destructive"), Destructive.MaximumSafety, TEXT("destructive"));
	TestTrue(TEXT("Destructive plan requires authorization for later execution"), Destructive.bRequiresAuthorization);
	TestTrue(TEXT("Mutation operation id requirement is explicit"), Destructive.bRequiresOperationId);
	TestTrue(TEXT("Destructive authorization reason is explicit"), Destructive.bRequiresDestructiveAuthorization);
	TestFalse(TEXT("External-effect authorization is not invented"), Destructive.bRequiresExternalEffectAuthorization);
	TestTrue(TEXT("Destructive validation still performs no mutation"), Destructive.bNoMutation);
	TestFalse(TEXT("Destructive validation does not execute"), Destructive.bExecutionPerformed);
	TestFalse(TEXT("Destructive validation is not a token issuer"), Destructive.bAuthorizationIssued);
	TestEqual(TEXT("Declared effect is returned"), Destructive.Effects.Num(), 1);
	TestTrue(TEXT("Lifecycle schedule is derived"), Destructive.OrderedSchedule.Num() > Destructive.StepCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanValidateRegistrationPolicyTest,
	"HyperAIStudio.NativeTools.PlanValidate.RegistrationCohort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanValidateRegistrationPolicyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlanValidate::Tests;
	const FHyperAIStudioPlanValidateAdmissionEvidence Compiled =
		FHyperAIStudioPlanValidateContracts::GetCompiledAdmissionEvidence();
	TestEqual(TEXT("Checked-in evidence is SourceCandidate"),
		static_cast<uint8>(Compiled.AdmissionState),
		static_cast<uint8>(EHyperAIStudioPlanValidateAdmissionState::SourceCandidate));
	TestFalse(TEXT("SourceCandidate is fail-closed in production"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(Compiled, false));
	const FHyperAIStudioCapabilityToolDefinition* BuiltInTool = nullptr;
	for (const FHyperAIStudioCapabilityToolDefinition& Tool :
		FHyperAIStudioCapabilityPackRegistry::GetCatalog().Tools)
	{
		if (Tool.Name == TEXT("hyper_plan_validate"))
		{
			BuiltInTool = &Tool;
			break;
		}
	}
	TestNotNull(TEXT("Built-in catalog contains hyper_plan_validate"), BuiltInTool);
	const bool bCatalogAllowsDev = BuiltInTool
		&& BuiltInTool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
	TestEqual(TEXT("Dev flag respects the catalog state instead of bypassing it"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(Compiled, true),
		bCatalogAllowsDev);

	FHyperAIStudioCapabilityToolDefinition SourceCandidateTool;
	SourceCandidateTool.Name = TEXT("hyper_plan_validate");
	SourceCandidateTool.PackId = TEXT("shared_foundation");
	SourceCandidateTool.AdmissionState = EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
	SourceCandidateTool.bMayCauseExternalEffects = false;
	TestTrue(TEXT("Explicit dev flag exposes an exact catalog-bound SourceCandidate"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationEvidenceAllowed(
			Compiled,
			SourceCandidateTool,
			Compiled.CatalogFingerprint,
			Compiled.CapabilityFingerprint,
			true));

	FHyperAIStudioCapabilityToolDefinition AdmittedTool;
	AdmittedTool.Name = TEXT("hyper_plan_validate");
	AdmittedTool.PackId = TEXT("shared_foundation");
	AdmittedTool.AdmissionState = EHyperAIStudioCapabilityAdmissionState::Admitted;
	AdmittedTool.bMayCauseExternalEffects = false;
	AdmittedTool.SourceArtifactCount = 1;
	AdmittedTool.SourceArtifactFingerprint = Sha(TEXT('c'));

	FHyperAIStudioPlanValidateAdmissionEvidence Exact = Compiled;
	Exact.AdmissionState = EHyperAIStudioPlanValidateAdmissionState::Admitted;
	Exact.SourceArtifactCount = AdmittedTool.SourceArtifactCount;
	Exact.SourceArtifactFingerprint = AdmittedTool.SourceArtifactFingerprint;
	TestTrue(TEXT("Exact generated-catalog admission passes the pure one-tool policy"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationEvidenceAllowed(
			Exact,
			AdmittedTool,
			Exact.CatalogFingerprint,
			Exact.CapabilityFingerprint,
			false));
	FHyperAIStudioPlanValidateAdmissionEvidence StaleState = Exact;
	StaleState.AdmissionState = EHyperAIStudioPlanValidateAdmissionState::SourceCandidate;
	TestFalse(TEXT("A hand-written state cannot override generated-catalog admission"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationEvidenceAllowed(
			StaleState,
			AdmittedTool,
			Exact.CatalogFingerprint,
			Exact.CapabilityFingerprint,
			false));

	FHyperAIStudioPlanValidateAdmissionEvidence MissingSourceBinding = Exact;
	MissingSourceBinding.SourceArtifactFingerprint.Reset();
	TestFalse(TEXT("Missing generated source binding blocks production registration"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationEvidenceAllowed(
			MissingSourceBinding,
			AdmittedTool,
			Exact.CatalogFingerprint,
			Exact.CapabilityFingerprint,
			false));

	FHyperAIStudioPlanValidateAdmissionEvidence TamperedIdentity = Compiled;
	TamperedIdentity.QualifiedToolsetName = TEXT("HyperAIStudio.OtherToolset");
	TestFalse(TEXT("Dev mode does not bypass exact toolset identity"),
		FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(TamperedIdentity, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlanValidateUhtSchemaTest,
	"HyperAIStudio.NativeTools.PlanValidate.UhtSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlanValidateUhtSchemaTest::RunTest(const FString& Parameters)
{
	UClass* ToolsetClass = UHyperAIStudioPlanValidateToolset::StaticClass();
	TestNotNull(TEXT("Toolset UClass exists"), ToolsetClass);
	UFunction* Function = ToolsetClass
		? ToolsetClass->FindFunctionByName(TEXT("hyper_plan_validate"))
		: nullptr;
	TestNotNull(TEXT("hyper_plan_validate is reflected"), Function);
	if (!Function)
	{
		return false;
	}
	TestTrue(TEXT("Callable carries Epic AICallable metadata"), Function->HasMetaData(TEXT("AICallable")));
	TestTrue(TEXT("Callable is static"), Function->HasAllFunctionFlags(FUNC_Static));
	TObjectPtr<UFunction> FunctionObject = Function;
	const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(FunctionObject);
	TestTrue(TEXT("Epic ToolsetRegistry accepts the reflected contract"),
		Callability.HasValue() && Callability.GetValue());
	TestNotNull(TEXT("plan_json is a reflected string"), FindFProperty<FStrProperty>(Function, TEXT("PlanJson")));
	TestNotNull(TEXT("expected_plan_hash is a reflected string"), FindFProperty<FStrProperty>(Function, TEXT("ExpectedPlanHash")));
	const FStructProperty* ReturnProperty = CastField<FStructProperty>(Function->GetReturnProperty());
	TestNotNull(TEXT("Return value is a reflected USTRUCT"), ReturnProperty);
	if (ReturnProperty)
	{
		TestEqual(TEXT("Return USTRUCT is exact"), ReturnProperty->Struct.Get(), FHyperAIPlanValidateReport::StaticStruct());
	}

	int32 AICallableCount = 0;
	for (TFieldIterator<UFunction> It(ToolsetClass, EFieldIterationFlags::None); It; ++It)
	{
		AICallableCount += It->HasMetaData(TEXT("AICallable")) ? 1 : 0;
	}
	TestEqual(TEXT("Registration cohort contains exactly one callable"), AICallableCount, 1);
	TestNotNull(TEXT("Canonical hash field is reflected"),
		FindFProperty<FStrProperty>(FHyperAIPlanValidateReport::StaticStruct(), TEXT("CanonicalPlanHash")));
	TestNotNull(TEXT("No-mutation field is reflected"),
		FindFProperty<FBoolProperty>(FHyperAIPlanValidateReport::StaticStruct(), TEXT("bNoMutation")));
	TestNotNull(TEXT("Schedule array is reflected"),
		FindFProperty<FArrayProperty>(FHyperAIPlanValidateReport::StaticStruct(), TEXT("OrderedSchedule")));
	TestNull(TEXT("No authorization token can be returned"),
		FindFProperty<FProperty>(FHyperAIPlanValidateReport::StaticStruct(), TEXT("AuthorizationToken")));
	TestNull(TEXT("No raw fallback field exists"),
		FindFProperty<FProperty>(FHyperAIPlanValidateReport::StaticStruct(), TEXT("FallbackTool")));

	const FString JsonSchema = UToolsetRegistry::GetToolsetJsonSchema(ToolsetClass);
	TSharedPtr<FJsonObject> SchemaObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonSchema);
	TestTrue(TEXT("Epic ToolsetRegistry emits valid JSON schema"),
		FJsonSerializer::Deserialize(Reader, SchemaObject));
	if (SchemaObject.IsValid())
	{
		TestEqual(TEXT("Toolset schema name is exact"),
			SchemaObject->GetStringField(TEXT("name")),
			FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName());
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		TestTrue(TEXT("Toolset schema contains a tools array"),
			SchemaObject->TryGetArrayField(TEXT("tools"), Tools));
		if (Tools)
		{
			TestEqual(TEXT("Toolset schema exposes one-tool cohort"), Tools->Num(), 1);
			if (Tools->Num() == 1 && (*Tools)[0].IsValid() && (*Tools)[0]->AsObject().IsValid())
			{
				TestEqual(TEXT("Schema tool name is fully qualified"),
					(*Tools)[0]->AsObject()->GetStringField(TEXT("name")),
					FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName()
						+ TEXT(".hyper_plan_validate"));
			}
		}
	}
	return true;
}

#endif
