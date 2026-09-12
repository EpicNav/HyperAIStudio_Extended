// Games by Hyper 2026.

#include "HyperAIStudioDataDelegationMatrix.h"
#include "HyperAIStudioDataToolset.h"

#include "AssetRegistry/AssetData.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Data::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	void Seal(TArray<FHyperAIDataRecord>& Records)
	{
		for (FHyperAIDataRecord& Record : Records)
		{
			Record.ElementFingerprint =
				FHyperAIStudioDataContracts::ComputeElementFingerprint(Record);
		}
	}

	FHyperAIDataDetachedSnapshot AbsentSnapshot(const FString& State = TEXT("does_not_exist"))
	{
		FHyperAIDataDetachedSnapshot Snapshot;
		Snapshot.RequestFingerprint = Hash(TEXT("request"));
		Snapshot.bSnapshotComplete = true;
		FHyperAIDataRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
		Record.Kind = TEXT("asset_state");
		Record.ObjectPath = TEXT("/Game/Data/DA_New.DA_New");
		Record.StableId = TEXT("asset_state");
		Record.PackageState = State;
		Seal(Snapshot.Records);
		Snapshot.TotalRecords = Snapshot.Records.Num();
		Snapshot.PersistedFingerprint =
			FHyperAIStudioDataContracts::ComputePersistedFingerprint(Snapshot.Records);
		Snapshot.VolatileObservationFingerprint =
			FHyperAIStudioDataContracts::ComputeVolatileFingerprint(Snapshot.Records);
		return Snapshot;
	}

	TArray<FHyperAIDataRecord> ExistingEnum()
	{
		TArray<FHyperAIDataRecord> Records;
		const FString ObjectPath = TEXT("/Game/Data/E_State.E_State");
		FHyperAIDataRecord& State = Records.AddDefaulted_GetRef();
		State.Kind = TEXT("asset_state");
		State.ObjectPath = ObjectPath;
		State.StableId = TEXT("asset_state");
		State.PackageState = TEXT("exists");
		State.bLoaded = true;
		FHyperAIDataRecord& Header = Records.AddDefaulted_GetRef();
		Header.Kind = TEXT("user_enum");
		Header.ObjectPath = ObjectPath;
		Header.StableId = TEXT("enum");
		Header.Name = TEXT("E_State");
		Header.TypeId = TEXT("user_defined_enum");
		Header.Count = 1;
		FHyperAIDataRecord& Value = Records.AddDefaulted_GetRef();
		Value.Kind = TEXT("user_enum_value");
		Value.ObjectPath = ObjectPath;
		Value.StableId = TEXT("Idle");
		Value.Name = TEXT("Idle");
		Value.TypeId = TEXT("enum_value");
		Value.Index = 0;
		Value.IntegerValue = 0;
		Seal(Records);
		return Records;
	}

	bool PreparePayload(
		const TSharedRef<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioDataContracts::GetPreparationDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioDataContracts::PackId;
		Binding.ToolName = TEXT("hyper_data_apply_plan");
		Binding.VariantId = FHyperAIStudioDataContracts::EditVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
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
		Contract.EffectTarget = TEXT("data:test");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 5;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 8192;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataManifestMatrixTest,
	"HyperAIStudio.NativeTools.Data.ManifestDelegationAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataManifestMatrixTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Exact pack"), FString(FHyperAIStudioDataContracts::PackId),
		FString(TEXT("data_config_localization")));
	TestEqual(TEXT("Exact cohort"), FString(FHyperAIStudioDataContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiodatatoolset.v1")));
	TestEqual(TEXT("Exact qualifier"), FHyperAIStudioDataContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioData.HyperAIStudioDataToolset")));
	TestEqual(TEXT("Exact non-dry blocker"),
		FString(FHyperAIStudioDataContracts::NonDryCallableState),
		FString(TEXT("staged_backend_required")));
	const TArray<FHyperAIStudioDataManifestEntry>& Manifest =
		FHyperAIStudioDataContracts::GetManifest();
	TestEqual(TEXT("Exactly three callables"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect callable"), Manifest[0].Name, FString(TEXT("hyper_data_inspect")));
	TestEqual(TEXT("Apply callable"), Manifest[1].Name, FString(TEXT("hyper_data_apply_plan")));
	TestEqual(TEXT("Validate callable"), Manifest[2].Name, FString(TEXT("hyper_data_validate")));
	const TConstArrayView<FHyperAIStudioDataEpicDelegation> Epic =
		FHyperAIStudioDataDelegationMatrix::GetEpic();
	const TConstArrayView<FHyperAIStudioDataRequirementDisposition> Requirements =
		FHyperAIStudioDataDelegationMatrix::GetRequirements();
	TestEqual(TEXT("Exact Epic delegation matrix"), Epic.Num(), 56);
	TestEqual(TEXT("Exact product requirement matrix"), Requirements.Num(), 18);
	TSet<FString> EpicCoordinates;
	for (const FHyperAIStudioDataEpicDelegation& Row : Epic)
	{
		const FString Coordinate = FString(Row.SourceToolset) + TEXT(":") + Row.CallableName;
		TestFalse(TEXT("No duplicated Epic coordinate"), EpicCoordinates.Contains(Coordinate));
		EpicCoordinates.Add(Coordinate);
	}
	TSet<FString> RequirementCoordinates;
	for (const FHyperAIStudioDataRequirementDisposition& Row : Requirements)
	{
		const FString Coordinate = FString(Row.Source) + TEXT(":") + Row.SourceId;
		TestFalse(TEXT("No duplicated product requirement coordinate"),
			RequirementCoordinates.Contains(Coordinate));
		RequirementCoordinates.Add(Coordinate);
	}
	TestEqual(TEXT("Two typed safety variants"),
		FHyperAIStudioDataContracts::GetPreparationDescriptor().Variants.Num(), 2);
	TestEqual(TEXT("No non-blocking authority groups"),
		FHyperAIStudioDataContracts::GetPreparationDescriptor()
			.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataFingerprintCursorTest,
	"HyperAIStudio.NativeTools.Data.PersistedVolatileAndRevisionCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataFingerprintCursorTest::RunTest(const FString& Parameters)
{
	TArray<FHyperAIDataRecord> Records = HyperAIStudio::Data::Tests::AbsentSnapshot().Records;
	const FString Persisted = FHyperAIStudioDataContracts::ComputePersistedFingerprint(Records);
	const FString Volatile = FHyperAIStudioDataContracts::ComputeVolatileFingerprint(Records);
	Records[0].bDirty = true;
	Records[0].PackageState = TEXT("unknown");
	TestEqual(TEXT("Volatile observations do not drift persisted identity"),
		FHyperAIStudioDataContracts::ComputePersistedFingerprint(Records), Persisted);
	const FString DriftedVolatile =
		FHyperAIStudioDataContracts::ComputeVolatileFingerprint(Records);
	TestNotEqual(TEXT("Volatile identity changes independently"), DriftedVolatile, Volatile);
	const FString Request = HyperAIStudio::Data::Tests::Hash(TEXT("request"));
	const FString Cursor = FHyperAIStudioDataContracts::EncodeCursor(
		7, Request, Persisted, DriftedVolatile);
	int32 Offset = -1;
	TestTrue(TEXT("Exact revision cursor opens"), FHyperAIStudioDataContracts::DecodeCursor(
		Cursor, Request, Persisted, DriftedVolatile, Offset));
	TestEqual(TEXT("Exact offset"), Offset, 7);
	TestFalse(TEXT("Volatile drift closes cursor"), FHyperAIStudioDataContracts::DecodeCursor(
		Cursor, Request, Persisted, Volatile, Offset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataUnknownDetachedValidationTest,
	"HyperAIStudio.NativeTools.Data.UnknownIsIncompleteDetachedValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataUnknownDetachedValidationTest::RunTest(const FString& Parameters)
{
	FHyperAIDataValidateRequest Request;
	Request.Snapshot = HyperAIStudio::Data::Tests::AbsentSnapshot();
	const FHyperAIDataValidateReport Valid = UHyperAIStudioDataToolset::hyper_data_validate(Request);
	TestTrue(TEXT("Proven absence validates as a full detached value"), Valid.bValid);
	Request.Snapshot = HyperAIStudio::Data::Tests::AbsentSnapshot(TEXT("unknown"));
	const FHyperAIDataValidateReport Unknown = UHyperAIStudioDataToolset::hyper_data_validate(Request);
	TestFalse(TEXT("Unknown never validates"), Unknown.bValid);
	TestTrue(TEXT("Unknown emits independent issue"), Unknown.Issues.ContainsByPredicate(
		[](const FHyperAIDataIssue& Issue)
		{
			return Issue.Code == TEXT("package_state_unknown_incomplete");
		}));
	TestEqual(TEXT("Unknown tri-state token"),
		FHyperAIStudioDataContracts::ClassifyPackageExistence(
			static_cast<int32>(UE::AssetRegistry::EExists::Unknown)), FString(TEXT("unknown")));
	TestFalse(TEXT("Unknown cannot admit create"),
		FHyperAIStudioDataContracts::IsCreateExistenceAdmitted(
			static_cast<int32>(UE::AssetRegistry::EExists::Unknown), false));
	TestTrue(TEXT("Only DoesNotExist with no loaded package admits create"),
		FHyperAIStudioDataContracts::IsCreateExistenceAdmitted(
			static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataClosedOperationCASTest,
	"HyperAIStudio.NativeTools.Data.ClosedShapesCASAndShadowReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataClosedOperationCASTest::RunTest(const FString& Parameters)
{
	FHyperAIDataPlanOperation Rename;
	Rename.Kind = TEXT("user_enum.rename_value");
	Rename.SubjectId = TEXT("Idle");
	Rename.ExpectedElementFingerprint = HyperAIStudio::Data::Tests::Hash(TEXT("wrong"));
	Rename.Name = TEXT("Waiting");
	FHyperAIStudioDataBackendOperation Parsed;
	FString Error;
	TestTrue(TEXT("Closed rename shape parses"),
		FHyperAIStudioDataContracts::ValidateOperationShape(Rename, Parsed, Error));
	TArray<FHyperAIDataRecord> Base = HyperAIStudio::Data::Tests::ExistingEnum();
	TArray<FHyperAIDataRecord> Desired;
	FHyperAIDataPlanEffects Effects;
	TestFalse(TEXT("Wrong element CAS fails shadow"),
		FHyperAIStudioDataContracts::ReplayShadowForTest(
			TEXT("/Game/Data/E_State.E_State"), Base, {Parsed}, Desired, Effects, Error));
	Rename.ExpectedElementFingerprint = Base[2].ElementFingerprint;
	TestTrue(TEXT("Fresh element CAS shape parses"),
		FHyperAIStudioDataContracts::ValidateOperationShape(Rename, Parsed, Error));
	TestTrue(TEXT("Fresh element CAS replays"),
		FHyperAIStudioDataContracts::ReplayShadowForTest(
			TEXT("/Game/Data/E_State.E_State"), Base, {Parsed}, Desired, Effects, Error));
	TestEqual(TEXT("One rename effect"), Effects.RenameCount, 1);
	TestTrue(TEXT("Desired enum name changed"), Desired.ContainsByPredicate(
		[](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == TEXT("user_enum_value") && Record.Name == TEXT("Waiting");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataFastEditRouteTest,
	"HyperAIStudio.NativeTools.Data.FastEditRouteIsNarrowAndPublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataFastEditRouteTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDataBackendOperation Rename;
	Rename.Kind = EHyperAIStudioDataOperationKind::UserStructRenameField;
	Rename.Safety = EHyperAIStudioDataPlanSafety::Edit;
	TestTrue(TEXT("One reversible struct rename uses the fast route"),
		FHyperAIStudioDataContracts::IsFastReversibleEditPlan({Rename}));

	FHyperAIStudioDataBackendOperation Add = Rename;
	Add.Kind = EHyperAIStudioDataOperationKind::UserStructAddField;
	TestFalse(TEXT("Add remains gated"),
		FHyperAIStudioDataContracts::IsFastReversibleEditPlan({Add}));
	FHyperAIStudioDataBackendOperation Remove = Rename;
	Remove.Kind = EHyperAIStudioDataOperationKind::UserStructRemoveField;
	Remove.Safety = EHyperAIStudioDataPlanSafety::Destructive;
	TestFalse(TEXT("Remove remains gated"),
		FHyperAIStudioDataContracts::IsFastReversibleEditPlan({Remove}));
	TestFalse(TEXT("Compound plans remain gated"),
		FHyperAIStudioDataContracts::IsFastReversibleEditPlan({Rename, Rename}));

	using FRenameVariable = bool (*)(UUserDefinedStruct*, FGuid, const FString&);
	const FRenameVariable PublicRename = static_cast<FRenameVariable>(
		&FStructureEditorUtils::RenameVariable);
	TestTrue(TEXT("UE 5.8 public typed rename API is linked"), PublicRename != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataCreateCloneBoundsTest,
	"HyperAIStudio.NativeTools.Data.CreateShadowDeepCloneAndBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataCreateCloneBoundsTest::RunTest(const FString& Parameters)
{
	TArray<FHyperAIDataRecord> Base = HyperAIStudio::Data::Tests::AbsentSnapshot().Records;
	FHyperAIStudioDataBackendOperation Create;
	Create.Kind = EHyperAIStudioDataOperationKind::UserStructCreate;
	FHyperAIStudioDataBackendOperation Add;
	Add.Kind = EHyperAIStudioDataOperationKind::UserStructAddField;
	Add.Name = TEXT("Health");
	Add.ValueType = TEXT("float");
	TArray<FHyperAIDataRecord> Desired;
	FHyperAIDataPlanEffects Effects;
	FString Error;
	TestTrue(TEXT("Proven absence supports pure create shadow"),
		FHyperAIStudioDataContracts::ReplayShadowForTest(
			TEXT("/Game/Data/DA_New.DA_New"), Base, {Create, Add}, Desired, Effects, Error));
	TestEqual(TEXT("One create"), Effects.CreateCount, 1);
	TestEqual(TEXT("One add"), Effects.AddCount, 1);
	const TSharedRef<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = TEXT("/Game/Data/DA_New.DA_New");
	Payload->BasePersistedFingerprint =
		FHyperAIStudioDataContracts::ComputePersistedFingerprint(Base);
	Payload->DesiredPersistedFingerprint =
		FHyperAIStudioDataContracts::ComputePersistedFingerprint(Desired);
	Payload->CanonicalOperations = {TEXT("create"), TEXT("add")};
	Payload->SemanticFingerprint =
		FHyperAIStudioDataContracts::ComputePayloadSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	const int32 CloneBytes = Clone->GetBoundedByteSize();
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TestTrue(TEXT("Public typed-artifact seam performs pure preparation"),
		HyperAIStudio::Data::Tests::PreparePayload(Payload, Prepared, Error));
	TestTrue(TEXT("Pure preparation seals a canonical plan hash"),
		FHyperAIStudioDataContracts::IsCanonicalSha256(Prepared.PlanHash));
	Payload->CanonicalOperations[0] = FString::ChrN(200, TEXT('x'));
	TestNotEqual(TEXT("Clone owns a detached operations array"),
		Payload->GetBoundedByteSize(), CloneBytes);
	TestTrue(TEXT("Worst-case JSON estimator accounts for six-byte escapes"),
		FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(TEXT("abc")) >= 20);
	TestTrue(TEXT("Canonical primary path admitted"),
		FHyperAIStudioDataContracts::IsCanonicalProjectObjectPath(
			TEXT("/Game/Data/DA_New.DA_New")));
	TestFalse(TEXT("Subobject path rejected"),
		FHyperAIStudioDataContracts::IsCanonicalProjectObjectPath(
			TEXT("/Game/Data/DA_New.DA_New:Sub")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDataInvalidZeroEffectTest,
	"HyperAIStudio.NativeTools.Data.InvalidNonDryRejectedBeforeEveryEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDataInvalidZeroEffectTest::RunTest(const FString& Parameters)
{
	FHyperAIDataApplyPlanRequest Request;
	Request.bDryRun = false;
	Request.OperationId = TEXT("zero-effect-test");
	Request.TargetPath = TEXT("/Engine/Rejected.Rejected");
	Request.ExpectedPersistedFingerprint = HyperAIStudio::Data::Tests::Hash(TEXT("base"));
	FHyperAIDataPlanOperation& Operation = Request.Operations.AddDefaulted_GetRef();
	Operation.Kind = TEXT("user_enum.create");
	const FHyperAIDataApplyPlanReport Report =
		UHyperAIStudioDataToolset::hyper_data_apply_plan(Request);
	TestFalse(TEXT("Invalid non-dry request is not accepted"), Report.bOk);
	TestFalse(TEXT("Never staged"), Report.bStaged);
	TestFalse(TEXT("Never submitted"), Report.bExecutionSubmitted);
	TestFalse(TEXT("No compile effect"), Report.Effects.bWouldCompileOnce);
	TestFalse(TEXT("No save effect"), Report.Effects.bWouldSaveOnce);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
