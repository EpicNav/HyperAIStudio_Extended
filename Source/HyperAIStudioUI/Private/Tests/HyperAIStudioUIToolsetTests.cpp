// Games by Hyper 2026.

#include "HyperAIStudioUIDelegationMatrix.h"
#include "HyperAIStudioUIToolset.h"
#include "HyperAIStudioUIValueModel.h"

#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::UI::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIUIRecord BlueprintRecord()
	{
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("blueprint");
		Record.RecordKey = TEXT("blueprint:/Game/UI/W_Test.W_Test");
		Record.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
		Record.StableId = Record.BlueprintPath;
		Record.Name = TEXT("W_Test");
		Record.ClassPath = TEXT("/Script/UMGEditor.WidgetBlueprint");
		return Record;
	}

	FHyperAIUIRecord RootWidgetRecord()
	{
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("widget");
		Record.RecordKey = TEXT("widget:/Game/UI/W_Test.W_Test:name:Title");
		Record.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
		Record.StableId = TEXT("name:Title");
		Record.Name = TEXT("Title");
		Record.ClassPath = TEXT("/Script/UMG.TextBlock");
		FHyperAIUIFieldValue& Text = Record.Fields.AddDefaulted_GetRef();
		Text.Id = TEXT("text"); Text.Type = TEXT("string"); Text.StringValue = TEXT("Old");
		FHyperAIUIFieldValue& Enabled = Record.Fields.AddDefaulted_GetRef();
		Enabled.Id = TEXT("is_enabled"); Enabled.Type = TEXT("bool"); Enabled.bBoolValue = true;
		FHyperAIUIFieldValue& Visibility = Record.Fields.AddDefaulted_GetRef();
		Visibility.Id = TEXT("visibility"); Visibility.Type = TEXT("enum");
		Visibility.StringValue = TEXT("visible");
		return Record;
	}

	FHyperAIStudioUIValueSnapshot BaseSnapshot()
	{
		FHyperAIStudioUIValueSnapshot Snapshot;
		Snapshot.Records = {BlueprintRecord(), RootWidgetRecord()};
		FString Error;
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error);
		return Snapshot;
	}

	TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Payload(
		const EHyperAIStudioUIPlanSafety Safety)
	{
		const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Value =
			MakeShared<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>();
		Value->TargetPath = TEXT("/Game/UI/W_Test.W_Test");
		Value->BasePersistedFingerprint = Hash(TEXT("base"));
		Value->DesiredPersistedFingerprint = Hash(TEXT("desired"));
		Value->ValidationPolicy = TEXT("structural");
		Value->Safety = Safety;
		FHyperAIStudioUIBackendOperation& Operation = Value->Operations.AddDefaulted_GetRef();
		Operation.Kind = Safety == EHyperAIStudioUIPlanSafety::Destructive
			? EHyperAIStudioUIOperationKind::AnimationDelete
			: EHyperAIStudioUIOperationKind::WidgetSetProperty;
		Operation.Safety = Safety;
		Operation.SubjectId = TEXT("subject");
		Operation.ExpectedElementFingerprint = Hash(TEXT("element"));
		if (Safety == EHyperAIStudioUIPlanSafety::Edit)
		{
			Operation.Name = TEXT("text");
			Operation.StringValue = TEXT("value");
		}
		Value->SemanticFingerprint =
			FHyperAIStudioUIContracts::ComputePayloadSemanticFingerprint(*Value);
		return Value;
	}

	bool Prepare(
		const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>& PayloadValue,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
			FHyperAIStudioUIContracts::GetPreparationDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioUIContracts::PackId;
		Binding.ToolName = TEXT("hyper_ui_apply_plan");
		Binding.VariantId = PayloadValue->Safety == EHyperAIStudioUIPlanSafety::Destructive
			? FHyperAIStudioUIContracts::DestructiveVariantId
			: FHyperAIStudioUIContracts::EditVariantId;
		Binding.ExpectedSafety = PayloadValue->Safety == EHyperAIStudioUIPlanSafety::Destructive
			? EHyperAIStudioDomainSafety::Destructive : EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.UMG"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("plugin.ModelViewViewModel"), EHyperAIStudioDomainPrerequisiteState::Available},
			{FHyperAIStudioUIContracts::LiveProbeId,
				EHyperAIStudioDomainPrerequisiteState::Missing}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = PayloadValue->GetTypeId();
		Contract.ArtifactSchemaFingerprint = PayloadValue->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = PayloadValue->GetSemanticFingerprint();
		Contract.EffectTarget = TEXT("widget_blueprint:test");
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
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
	FHyperAIStudioUIManifestAndDelegationTest,
	"HyperAIStudio.NativeTools.UI.ManifestAndEpicDelegation46",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIManifestAndDelegationTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Exact pack id"), FString(FHyperAIStudioUIContracts::PackId),
		FString(TEXT("ui_slate_mvvm")));
	TestEqual(TEXT("Exact plugin requirement group"),
		FString(FHyperAIStudioUIContracts::PluginRequirementGroupId), FString(TEXT("ui_plugin")));
	TestEqual(TEXT("Exact backend requirement group"),
		FString(FHyperAIStudioUIContracts::BackendRequirementGroupId), FString(TEXT("ui_backend")));
	TestEqual(TEXT("Exact probe id"), FString(FHyperAIStudioUIContracts::LiveProbeId),
		FString(TEXT("probe.widget_blueprint_editor")));
	TestEqual(TEXT("Exact zero-effect non-dry blocker"),
		FString(FHyperAIStudioUIContracts::NonDryCallableState),
		FString(TEXT("staged_backend_required")));
	TestEqual(TEXT("Exact cohort id"), FString(FHyperAIStudioUIContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiouitoolset.v1")));
	TestEqual(TEXT("Central module type handoff"),
		FString(FHyperAIStudioUIContracts::RequiredModuleType), FString(TEXT("Editor")));
	TestEqual(TEXT("Central loading phase handoff"),
		FString(FHyperAIStudioUIContracts::RequiredLoadingPhase), FString(TEXT("None")));
	const TArray<FHyperAIStudioUIManifestEntry>& Manifest = FHyperAIStudioUIContracts::GetManifest();
	TestEqual(TEXT("Exactly three UI callables"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect name"), Manifest[0].Name, FString(TEXT("hyper_ui_inspect")));
	TestEqual(TEXT("Apply name"), Manifest[1].Name, FString(TEXT("hyper_ui_apply_plan")));
	TestEqual(TEXT("Validate name"), Manifest[2].Name, FString(TEXT("hyper_ui_validate")));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioUIContracts::GetPreparationDescriptor();
	TestEqual(TEXT("Two separately typed apply safety variants"), Descriptor.Variants.Num(), 2);
	TestEqual(TEXT("Edit descriptor safety"), static_cast<uint8>(Descriptor.Variants[0].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::Edit));
	TestEqual(TEXT("Destructive descriptor safety"),
		static_cast<uint8>(Descriptor.Variants[1].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::Destructive));
	TestEqual(TEXT("Edit variant id is exact"), Descriptor.Variants[0].VariantId,
		FString(FHyperAIStudioUIContracts::EditVariantId));
	TestEqual(TEXT("Destructive variant id is exact"), Descriptor.Variants[1].VariantId,
		FString(FHyperAIStudioUIContracts::DestructiveVariantId));
	TestEqual(TEXT("Blocking UI groups are absent from non-blocking authority"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	const FString RequestHash = HyperAIStudio::UI::Tests::Hash(TEXT("request"));
	const FString PersistedHash = HyperAIStudio::UI::Tests::Hash(TEXT("persisted"));
	const FString Cursor = FHyperAIStudioUIContracts::EncodeCursor(
		23, RequestHash, PersistedHash);
	int32 CursorOffset = -1;
	TestTrue(TEXT("Cursor opens only for exact request/persisted identity"),
		FHyperAIStudioUIContracts::DecodeCursor(
			Cursor, RequestHash, PersistedHash, CursorOffset));
	TestEqual(TEXT("Cursor offset is exact"), CursorOffset, 23);
	TestFalse(TEXT("Cursor rejects persisted drift"),
		FHyperAIStudioUIContracts::DecodeCursor(
			Cursor, RequestHash, HyperAIStudio::UI::Tests::Hash(TEXT("drift")), CursorOffset));

	const TConstArrayView<FHyperAIStudioUIEpicDelegation> Matrix =
		FHyperAIStudioUIEpicDelegationMatrix::Get();
	TestEqual(TEXT("Exact 46/46 matrix"), Matrix.Num(), 46);
	int32 UMG = 0;
	int32 MVVM = 0;
	int32 Slate = 0;
	TSet<FString> Coordinates;
	for (const FHyperAIStudioUIEpicDelegation& Entry : Matrix)
	{
		const FString Source = Entry.SourceToolset;
		if (Source == TEXT("UMGToolSet")) ++UMG;
		else if (Source == TEXT("MVVMToolset")) ++MVVM;
		else if (Source == TEXT("SlateInspectorToolset")) ++Slate;
		const FString Coordinate = Source + TEXT(":") + Entry.CallableName;
		TestFalse(TEXT("No duplicate Epic coordinate"), Coordinates.Contains(Coordinate));
		Coordinates.Add(Coordinate);
		if (FString(Entry.CallableName) == TEXT("ListWidgetViewModels")
			|| FString(Entry.CallableName) == TEXT("ListWidgetViewBindings"))
		{
			TestEqual(TEXT("MVVM ListWidget lifecycle is edit"),
				FString(Entry.Lifecycle), FString(TEXT("edit")));
			TestEqual(TEXT("MVVM ListWidget access is edit"),
				FString(Entry.Access), FString(TEXT("edit")));
		}
	}
	TestEqual(TEXT("23 UMG delegates"), UMG, 23);
	TestEqual(TEXT("9 MVVM delegates"), MVVM, 9);
	TestEqual(TEXT("14 Slate delegates"), Slate, 14);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUISafetyShapeTest,
	"HyperAIStudio.NativeTools.UI.ClosedShapesAndSafetyAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUISafetyShapeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	const TArray<FString> EditKinds = {TEXT("animation.create"), TEXT("animation.rename"),
		TEXT("widget.set_property"), TEXT("widget.set_style"), TEXT("binding.create_legacy"),
		TEXT("binding.update_legacy"), TEXT("binding.create_mvvm"), TEXT("binding.update_mvvm")};
	const TArray<FString> DestructiveKinds = {TEXT("animation.delete"),
		TEXT("binding.replace_legacy"), TEXT("binding.delete_legacy"),
		TEXT("binding.replace_mvvm"), TEXT("binding.delete_mvvm")};
	for (const FString& Kind : EditKinds)
	{
		EHyperAIStudioUIOperationKind Parsed;
		EHyperAIStudioUIPlanSafety Safety;
		TestTrue(TEXT("Edit kind parses"), FHyperAIStudioUIContracts::ClassifyOperation(Kind, Parsed, Safety));
		TestEqual(TEXT("Edit kind cannot become read/destructive"),
			static_cast<uint8>(Safety), static_cast<uint8>(EHyperAIStudioUIPlanSafety::Edit));
	}
	for (const FString& Kind : DestructiveKinds)
	{
		EHyperAIStudioUIOperationKind Parsed;
		EHyperAIStudioUIPlanSafety Safety;
		TestTrue(TEXT("Destructive kind parses"), FHyperAIStudioUIContracts::ClassifyOperation(Kind, Parsed, Safety));
		TestEqual(TEXT("Delete/replace derives destructive"),
			static_cast<uint8>(Safety), static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	}
	FHyperAIUIPlanOperation Delete;
	Delete.Kind = TEXT("animation.delete");
	Delete.SubjectId = TEXT("Intro");
	Delete.ExpectedElementFingerprint = Hash(TEXT("animation"));
	FHyperAIStudioUIBackendOperation Backend;
	FString Error;
	TestTrue(TEXT("Exact delete shape accepted"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Delete, Backend, Error));
	TestEqual(TEXT("Delete backend remains destructive"), static_cast<uint8>(Backend.Safety),
		static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	Delete.ExpectedElementFingerprint.Reset();
	TestFalse(TEXT("Delete cannot omit per-element CAS"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Delete, Backend, Error));

	FHyperAIUIPlanOperation Replace;
	Replace.Kind = TEXT("binding.replace_legacy");
	Replace.SubjectId = TEXT("binding_id");
	Replace.ExpectedElementFingerprint = Hash(TEXT("binding"));
	Replace.Name = TEXT("Title");
	Replace.SecondaryName = TEXT("Text");
	Replace.SourcePath = TEXT("GetTitle");
	TestTrue(TEXT("Exact replacement shape accepted"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Replace, Backend, Error));
	TestEqual(TEXT("Replacement backend remains destructive"), static_cast<uint8>(Backend.Safety),
		static_cast<uint8>(EHyperAIStudioUIPlanSafety::Destructive));
	Replace.bHasBoolValue = true;
	TestFalse(TEXT("Replacement rejects unused scalar fields"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Replace, Backend, Error));
	FHyperAIUIPlanOperation ArbitraryWidget;
	ArbitraryWidget.Kind = TEXT("widget.set_property");
	ArbitraryWidget.SubjectId = TEXT("arbitrary/path");
	ArbitraryWidget.ExpectedElementFingerprint = Hash(TEXT("widget"));
	ArbitraryWidget.Name = TEXT("text");
	ArbitraryWidget.StringValue = TEXT("value");
	TestFalse(TEXT("Widget edits require exact name:/guid: stable ids"),
		FHyperAIStudioUIContracts::ValidateOperationShape(ArbitraryWidget, Backend, Error));
	FHyperAIUIPlanOperation HiddenScalar = Delete;
	HiddenScalar.ExpectedElementFingerprint = Hash(TEXT("animation"));
	HiddenScalar.NumberValue = 0.5;
	TestFalse(TEXT("Inactive union members must retain exact defaults"),
		FHyperAIStudioUIContracts::ValidateOperationShape(HiddenScalar, Backend, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIFingerprintsAndValidatorTest,
	"HyperAIStudio.NativeTools.UI.PersistedVolatileFingerprintsAndIndependentValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIFingerprintsAndValidatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	FHyperAIStudioUIValueSnapshot Snapshot = BaseSnapshot();
	FHyperAIUIRecord Volatile;
	Volatile.Kind = TEXT("blueprint");
	Volatile.RecordKey = TEXT("volatile_blueprint:/Game/UI/W_Test.W_Test");
	Volatile.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
	Volatile.StableId = TEXT("volatile");
	Volatile.Name = TEXT("W_Test");
	Volatile.bPersisted = false;
	FHyperAIUIFieldValue& Dirty = Volatile.Fields.AddDefaulted_GetRef();
	Dirty.Id = TEXT("package_dirty"); Dirty.Type = TEXT("bool"); Dirty.bBoolValue = false;
	Snapshot.Records.Add(Volatile);
	FString Error;
	TestTrue(TEXT("Fingerprints seal"),
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error));
	const FString Persisted = Snapshot.PersistedFingerprint;
	const FString VolatileBefore = Snapshot.VolatileObservationFingerprint;
	Snapshot.Records.FindByPredicate([](const FHyperAIUIRecord& Record)
	{
		return !Record.bPersisted;
	})->Fields[0].bBoolValue = true;
	TestTrue(TEXT("Drifted fingerprints seal"),
		FHyperAIStudioUIValueContracts::ComputeFingerprints(Snapshot, Error));
	TestEqual(TEXT("Volatile drift does not poison persisted CAS"),
		Snapshot.PersistedFingerprint, Persisted);
	TestNotEqual(TEXT("Volatile drift changes observation identity"),
		Snapshot.VolatileObservationFingerprint, VolatileBefore);

	FHyperAIStudioUIValueSnapshot Cycle;
	Cycle.Records.Add(BlueprintRecord());
	FHyperAIUIRecord A = RootWidgetRecord();
	A.StableId = TEXT("a"); A.Name = TEXT("A"); A.RecordKey = TEXT("widget:a"); A.ParentStableId = TEXT("b");
	FHyperAIUIRecord B = RootWidgetRecord();
	B.StableId = TEXT("b"); B.Name = TEXT("B"); B.RecordKey = TEXT("widget:b"); B.ParentStableId = TEXT("a");
	Cycle.Records.Add(A); Cycle.Records.Add(B);
	FHyperAIStudioUIValidationOptions Options;
	bool bTruncated = false;
	const TArray<FHyperAIUIIssue> Issues =
		FHyperAIStudioUIValueValidator::Validate(Cycle, Options, bTruncated);
	TestFalse(TEXT("Cycle validation is bounded"), bTruncated);
	TestTrue(TEXT("Cycle is found from detached values"), Issues.ContainsByPredicate(
		[](const FHyperAIUIIssue& Issue) { return Issue.Code == TEXT("widget_parent_cycle"); }));

	FHyperAIStudioUIValueSnapshot OrphanMVVM = BaseSnapshot();
	FHyperAIUIRecord& Binding = OrphanMVVM.Records.AddDefaulted_GetRef();
	Binding.Kind = TEXT("mvvm_binding");
	Binding.RecordKey = TEXT("mvvm_binding:/Game/UI/W_Test.W_Test:11111111111111111111111111111111");
	Binding.BlueprintPath = TEXT("/Game/UI/W_Test.W_Test");
	Binding.StableId = TEXT("11111111111111111111111111111111");
	Binding.Name = Binding.StableId;
	auto AddString = [&](const TCHAR* Id, const TCHAR* Value)
	{
		FHyperAIUIFieldValue& Field = Binding.Fields.AddDefaulted_GetRef();
		Field.Id = Id; Field.Type = TEXT("string"); Field.StringValue = Value;
	};
	AddString(TEXT("source_endpoint"), TEXT("self"));
	AddString(TEXT("source_path"), TEXT("Title"));
	AddString(TEXT("destination_endpoint"), TEXT("widget:Title"));
	AddString(TEXT("destination_path"), TEXT("Text"));
	bTruncated = false;
	const TArray<FHyperAIUIIssue> OrphanIssues =
		FHyperAIStudioUIValueValidator::Validate(OrphanMVVM, Options, bTruncated);
	TestTrue(TEXT("MVVM values cannot fabricate an absent extension/view"),
		OrphanIssues.ContainsByPredicate([](const FHyperAIUIIssue& Issue)
		{
			return Issue.Code == TEXT("mvvm_view_inventory_missing");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIShadowClonePrepareTest,
	"HyperAIStudio.NativeTools.UI.ShadowReplayImmutableCloneAndPurePrepare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIShadowClonePrepareTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::Tests;
	FHyperAIStudioUIValueSnapshot Base = BaseSnapshot();
	FHyperAIUIPlanOperation Operation;
	Operation.Kind = TEXT("widget.set_property");
	Operation.SubjectId = TEXT("name:Title");
	Operation.ExpectedElementFingerprint =
		FHyperAIStudioUIValueContracts::ComputeRecordFingerprint(Base.Records[1]);
	Operation.Name = TEXT("text");
	Operation.StringValue = TEXT("New");
	FHyperAIStudioUIBackendOperation Backend;
	FString Error;
	TestTrue(TEXT("Text operation has a closed shape"),
		FHyperAIStudioUIContracts::ValidateOperationShape(Operation, Backend, Error));
	FHyperAIStudioUIValueSnapshot Desired;
	FHyperAIUIPlanEffects Effects;
	TArray<FHyperAIUIIssue> Issues;
	TestTrue(TEXT("Shadow replay succeeds without UObject access"),
		FHyperAIStudioUIValueContracts::ReplayShadowPlan(
			Base, {Backend}, Desired, Effects, Issues, Error));
	TestNotEqual(TEXT("Desired fingerprint differs"),
		Desired.PersistedFingerprint, Base.PersistedFingerprint);
	TestEqual(TEXT("One property edit"), Effects.PropertyEdits, 1);
	TestTrue(TEXT("Lifecycle would compile once"), Effects.bWouldCompileOnce);
	TestTrue(TEXT("Lifecycle would save once"), Effects.bWouldSaveOnce);
	TestTrue(TEXT("Lifecycle would verify fresh once"), Effects.bWouldVerifyFreshOnce);

	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> EditPayload =
		Payload(EHyperAIStudioUIPlanSafety::Edit);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		EditPayload->CloneImmutable();
	TestTrue(TEXT("Clone is detached"), &Clone.Get() != &EditPayload.Get());
	TestEqual(TEXT("Clone semantic identity"), Clone->GetSemanticFingerprint(),
		EditPayload->GetSemanticFingerprint());
	EditPayload->Operations[0].StringValue = TEXT("mutated-after-clone");
	TestNotEqual(TEXT("Clone does not alias nested operation strings"),
		static_cast<const FHyperAIStudioUIPlanPayload&>(Clone.Get()).Operations[0].StringValue,
		EditPayload->Operations[0].StringValue);

	FHyperAIStudioPreparedTypedArtifact EditPrepared;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> PureEdit =
		Payload(EHyperAIStudioUIPlanSafety::Edit);
	TestTrue(TEXT("Public pure Prepare seals edit authority"),
		Prepare(PureEdit, EditPrepared, PrepareError));
	TestTrue(TEXT("Edit plan hash canonical"),
		FHyperAIStudioUIContracts::IsCanonicalSha256(EditPrepared.PlanHash));
	FHyperAIStudioPreparedTypedArtifact DestructivePrepared;
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> PureDestructive =
		Payload(EHyperAIStudioUIPlanSafety::Destructive);
	TestTrue(TEXT("Public pure Prepare seals destructive authority separately"),
		Prepare(PureDestructive, DestructivePrepared, PrepareError));
	TestNotEqual(TEXT("Edit and destructive authority hashes differ"),
		EditPrepared.AuthorizationPlanHash, DestructivePrepared.AuthorizationPlanHash);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
