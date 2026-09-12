// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDynamicMaterialToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Components/DMMaterialEffectStack.h"
#include "Components/DMMaterialLayer.h"
#include "Components/DMMaterialProperty.h"
#include "Components/DMMaterialSlot.h"
#include "Components/DMMaterialStage.h"
#include "Components/MaterialStageInputs/DMMSIValue.h"
#include "Material/DynamicMaterialInstance.h"
#include "Misc/AutomationTest.h"
#include "Components/MaterialValues/DMMaterialValueFloat1.h"
#include "Components/MaterialValues/DMMaterialValueFloat2.h"
#include "Components/MaterialValues/DMMaterialValueFloat3RGB.h"
#include "Model/DynamicMaterialModel.h"
#include "Model/DynamicMaterialModelDynamic.h"
#include "Model/DynamicMaterialModelEditorOnlyData.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <limits>

namespace HyperAIStudio::DynamicMaterial::Tests
{
	FString FakeRevision(const TCHAR* Seed)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Seed);
	}

	FHyperAIDynamicMaterialPlanOperation ScalarOperation(
		const FString& Target,
		const FString& Component,
		const FString& Revision,
		const double Value)
	{
		FHyperAIDynamicMaterialPlanOperation Operation;
		Operation.Type = TEXT("value.set_scalar"); Operation.TargetPath = Target;
		Operation.TargetFamily = TEXT("dynamic_material_model"); Operation.ExpectedRevision = Revision;
		Operation.ComponentPath = Component; Operation.ScalarValue = Value;
		return Operation;
	}

	void AddClosedRootPropertySlotProjection(FHyperAIDynamicMaterialAssetRecord& Record)
	{
		Record.EditorState = static_cast<int32>(EDMState::Idle);
		Record.MaterialDomain = static_cast<int32>(MD_Surface);
		Record.BlendMode = static_cast<int32>(BLEND_Opaque);
		Record.ShadingModel = static_cast<int32>(EDMMaterialShadingModel::Unlit);
		FHyperAIDynamicMaterialSlotView Slot;
		Slot.ComponentPath = TEXT("Slots(0)");
		Slot.ClassPath = UDMMaterialSlot::StaticClass()->GetPathName();
		Slot.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
		Slot.SlotIndex = 0;
		for (int32 PropertyIndex = static_cast<int32>(EDMMaterialPropertyType::None) + 1;
			PropertyIndex < static_cast<int32>(EDMMaterialPropertyType::Any);
			++PropertyIndex)
			Slot.OutputConnectorTypeSets.Add(FString::Printf(TEXT("property:%d|types:none"), PropertyIndex));
		Slot.bSemanticProjectionComplete = true;
		Record.Slots.Add(Slot); Record.SlotCount = 1;
		for (int32 PropertyIndex = static_cast<int32>(EDMMaterialPropertyType::None) + 1;
			PropertyIndex < static_cast<int32>(EDMMaterialPropertyType::Any);
			++PropertyIndex)
		{
			FHyperAIDynamicMaterialPropertyView Property;
			Property.ComponentPath = FString::Printf(TEXT("Properties(%d)"), PropertyIndex);
			Property.ClassPath =
				FHyperAIStudioDynamicMaterialContracts::ExpectedMaterialPropertyClassPath(
					PropertyIndex);
			Property.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
			Property.MaterialProperty = PropertyIndex;
			Property.bEnabled = PropertyIndex == static_cast<int32>(EDMMaterialPropertyType::BaseColor);
			Property.bMaterialPin = true;
			Property.InputConnectorType = static_cast<int32>(EDMValueType::VT_Float1);
			const EDMMaterialPropertyType PropertyType =
				static_cast<EDMMaterialPropertyType>(PropertyIndex);
			Property.bHasAlphaValueComponent =
				PropertyType != EDMMaterialPropertyType::Custom1
				&& PropertyType != EDMMaterialPropertyType::Custom2
				&& PropertyType != EDMMaterialPropertyType::Custom3
				&& PropertyType != EDMMaterialPropertyType::Custom4;
			if (Property.bHasAlphaValueComponent)
				Property.AlphaValueComponentIdentity = FString::Printf(
					TEXT("alpha-property-%d"), PropertyIndex);
			if (Property.bEnabled)
			{
				Property.SlotComponentPath = Slot.ComponentPath;
				FHyperAIDynamicMaterialConnectionChannelView Channel;
				Channel.SourceIndex = FDMMaterialStageConnectorChannel::NO_SOURCE;
				Channel.MaterialProperty = static_cast<int32>(EDMMaterialPropertyType::None);
				Channel.OutputIndex = 0; Channel.OutputChannel = 0;
				Property.InputChannels.Add(Channel);
			}
			Property.bSemanticProjectionComplete = true;
			Record.MaterialProperties.Add(MoveTemp(Property));
		}
		Record.MaterialPropertyCount = Record.MaterialProperties.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialManifestReflectionCohortTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.ManifestReflectionCohort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialManifestReflectionCohortTest::RunTest(const FString& Parameters)
{
	const auto& Manifest = FHyperAIStudioDynamicMaterialContracts::GetManifest();
	TestEqual(TEXT("optional DynamicMaterial cohort owns exactly three names"), Manifest.Num(), 3);
	TestEqual(TEXT("canonical runtime toolset identity"),
		FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioDynamicMaterial.HyperAIStudioDynamicMaterialToolset")));
	TSet<FString> Names;
	for (const auto& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		TestEqual(TEXT("one central qualified owner"), Entry.QualifiedToolset,
			FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName());
	}
	TestTrue(TEXT("exact inspect"), Names.Contains(TEXT("hyper_dynamic_material_inspect")));
	TestTrue(TEXT("exact apply"), Names.Contains(TEXT("hyper_dynamic_material_apply_plan")));
	TestTrue(TEXT("exact validate"), Names.Contains(TEXT("hyper_dynamic_material_validate")));
	TSet<FString> Reflected;
	for (TFieldIterator<UFunction> It(UHyperAIStudioDynamicMaterialToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
		if (It->HasMetaData(TEXT("AICallable"))) Reflected.Add(It->GetName());
	TestEqual(TEXT("reflection exposes only this atomic cohort"), Reflected.Num(), 3);
	TestTrue(TEXT("manifest/reflection exact"), Names.Difference(Reflected).IsEmpty()
		&& Reflected.Difference(Names).IsEmpty());
	TArray<FString> Exact = Names.Array(); Exact.Sort();
	FHyperAIStudioExtensionCohortAdmission Admission;
	const bool bGeneratedExact = FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
		FHyperAIStudioDynamicMaterialContracts::PackId,
		FHyperAIStudioDynamicMaterialContracts::AtomicCohortId, Exact, Admission);
	TestTrue(TEXT("catalog valid"), Admission.bCatalogValid);
	TestEqual(TEXT("exact-match flag agrees with generated lookup"),
		Admission.bExactCohortMatch, bGeneratedExact);
	TestEqual(TEXT("production registration agrees with admission"),
		FHyperAIStudioDynamicMaterialContracts::IsRegistrationAllowed(false),
		bGeneratedExact && Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted);
	if (!bGeneratedExact)
		TestFalse(TEXT("optional module remains fail-closed before central catalog regeneration"),
			FHyperAIStudioDynamicMaterialContracts::IsRegistrationAllowed(true));
	TestFalse(TEXT("foreign qualifier cannot own class"), FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioDynamicMaterialToolset::StaticClass(), TEXT("/Script/Foreign.DynamicMaterial")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialClosedSchemaMatrixTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.ClosedSchemaAndMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialClosedSchemaMatrixTest::RunTest(const FString& Parameters)
{
	const UStruct* Operation = FHyperAIDynamicMaterialPlanOperation::StaticStruct();
	TestNull(TEXT("no authorization token"), Operation->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("no script"), Operation->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("no class selector"), Operation->FindPropertyByName(TEXT("ClassPath")));
	TestNull(TEXT("no property reflection selector"), Operation->FindPropertyByName(TEXT("PropertyName")));
	const UStruct* Record = FHyperAIDynamicMaterialAssetRecord::StaticStruct();
	TestNull(TEXT("no unbounded dependency array"), Record->FindPropertyByName(TEXT("Dependencies")));
	TestNull(TEXT("no unbounded referencer array"), Record->FindPropertyByName(TEXT("Referencers")));
	TestTrue(TEXT("payload schema canonical"), FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(
		FHyperAIStudioDynamicMaterialContracts::PayloadSchemaFingerprint()));
	TestTrue(TEXT("result schema canonical"), FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(
		FHyperAIStudioDynamicMaterialContracts::ResultSchemaFingerprint()));
	const auto Matrix = FHyperAIStudioDynamicMaterialContracts::GetCapabilityMatrix();
	TestEqual(TEXT("core model and optional media gap rows"), Matrix.Num(), 2);
	const auto* Core = Matrix.FindByPredicate([](const auto& Row)
	{
		return Row.Family == TEXT("dynamic_material_model_components");
	});
	const auto* Media = Matrix.FindByPredicate([](const auto& Row)
	{
		return Row.Family == TEXT("media_stream_bridge");
	});
	TestNotNull(TEXT("core row"), Core); TestNotNull(TEXT("media row"), Media);
	if (Core)
	{
		TestFalse(TEXT("mutation backend is not advertised while bounded compile/CAS host is absent"),
			Core->bTypedBackendImplemented);
		TestTrue(TEXT("closed DTO projection remains pure non-executable evidence"),
			Core->SupportedCases.Contains(
				TEXT("pure_non_executable_typed_shadow_and_independent_validation_evidence")));
		TestTrue(TEXT("typed mutation is explicitly blocked"), Core->UnsupportedCases.Contains(
			TEXT("typed_value_layer_or_stage_mutation_without_bounded_async_cas_backend")));
		TestTrue(TEXT("unprojected sources/effects fail closed"), Core->UnsupportedCases.Contains(
			TEXT("unprojected_blend_cross_slot_stage_source_input_or_nonempty_effect_stack")));
		TestTrue(TEXT("generated material granular authoring delegated"), Core->DelegatedEpicCases.Contains(
			TEXT("generated_material_expression_authoring")));
		TestTrue(TEXT("dependency fanout delegated"), Core->DelegatedEpicCases.Contains(
			TEXT("hyper_asset_dependency_graph")));
		TestTrue(TEXT("raw reflection forbidden"), Core->UnsupportedCases.Contains(
			TEXT("raw_script_or_reflection_dispatch")));
	}
	if (Media)
	{
		TestFalse(TEXT("no false media backend"), Media->bTypedBackendImplemented);
		TestTrue(TEXT("private-setter gap explicit"), Media->UnsupportedCases.Contains(
			TEXT("media_stream_value_mutation_without_public_setter")));
	}
	const auto& Descriptor = FHyperAIStudioDynamicMaterialContracts::GetAdapterDescriptor();
	TestEqual(TEXT("optional adapter binds exactly one non-blocking requirement group"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 1);
	TestEqual(TEXT("optional adapter requirement group exact"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds[0], FString(TEXT("dynamic_material_variant")));
	TestEqual(TEXT("requirement group is bound by contract fingerprint recomputation"),
		Descriptor.ContractFingerprint,
		FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor));
	TestEqual(TEXT("requirement group is bound by adapter fingerprint recomputation"),
		Descriptor.AdapterFingerprint,
		FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialNoLoadDerivedTypeTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.NoLoadAndDerivedType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialNoLoadDerivedTypeTest::RunTest(const FString& Parameters)
{
	const FString Missing = TEXT("/Game/__HyperAIStudioAutomation/DM_NotLoaded.DM_NotLoaded");
	TestNull(TEXT("starts unresolved"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIDynamicMaterialInspectRequest Request;
	Request.TargetPath = Missing; Request.TargetFamily = TEXT("dynamic_material_model");
	Request.Scope = TEXT("on_disk_index");
	const auto Report = UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_inspect(Request);
	TestFalse(TEXT("missing exact row fails"), Report.bOk);
	TestEqual(TEXT("no-load status"), Report.Status, FString(TEXT("asset_not_found_on_disk")));
	TestNull(TEXT("still unresolved"), FSoftObjectPath(Missing).ResolveObject());
	// UE 5.8 models the editable and dynamic variants as sibling classes under
	// UDynamicMaterialModelBase, not as a direct derived-type relationship.
	TestTrue(TEXT("dynamic variant shares the UE 5.8 model base"),
		UDynamicMaterialModelDynamic::StaticClass()->IsChildOf(
			UDynamicMaterialModelBase::StaticClass()));
	TestFalse(TEXT("dynamic variant is not an editable model subclass"),
		UDynamicMaterialModelDynamic::StaticClass()->IsChildOf(
			UDynamicMaterialModel::StaticClass()));
	TestFalse(TEXT("derived dynamic model is rejected by exact family"),
		FHyperAIStudioDynamicMaterialContracts::IsExactLoadedFamily(
			UDynamicMaterialModelDynamic::StaticClass()->GetDefaultObject(), TEXT("dynamic_material_model")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialRevisionAndValidatorTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.RevisionAndValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialRevisionAndValidatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DynamicMaterial::Tests;
	FHyperAIStudioDynamicMaterialValueSnapshot Base;
	Base.bComplete = true; Base.Record.bRevisionComplete = true; Base.Record.bLoaded = true;
	Base.Record.Family = TEXT("dynamic_material_model"); Base.Record.AssetPath = TEXT("/Game/Test/DM.DM");
	Base.Record.ClassPath = TEXT("/Script/DynamicMaterial.DynamicMaterialModel");
	Base.Record.ModelPath = TEXT("/Game/Test/DM.DM"); Base.Record.ModelClassPath = Base.Record.ClassPath;
	Base.Record.GeneratedMaterialPath = TEXT("/Game/Test/DM_Mat.DM_Mat");
	Base.Record.GeneratedMaterialStateId = TEXT("state-a"); Base.Record.bModelValid = true;
	Base.Record.bEditorModelDataAvailable = true; Base.Record.bCompileStateKnown = true;
	Base.Record.DiskExistence = TEXT("exists"); Base.Record.bExistsOnDisk = true;
	Base.Record.bComponentOwnershipProjectionComplete = true;
	Base.Record.ComponentOwnershipIdentities.Add(
		TEXT("Values(0)|owner=/Game/Test/DM.DM|parent=/Game/Test/DM.DM|package=/Game/Test|roundtrip=1"));
	Base.Record.bMutationRevisionComplete = false;
	Base.Record.MutationRevisionBlockers = {
		TEXT("property_component_membership_unenumerable"),
		TEXT("parameter_map_membership_unenumerable"),
		TEXT("dynamic_material_instance_mid_state_unenumerable")};
	AddClosedRootPropertySlotProjection(Base.Record);
	FHyperAIDynamicMaterialValueView Value;
	Value.ComponentPath = TEXT("Values(0)");
	Value.ClassPath = UDMMaterialValueFloat1::StaticClass()->GetPathName();
	Value.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Value.ValueKind = TEXT("scalar"); Value.ValueType = 2;
	Value.ParameterName = TEXT("Roughness"); Value.ScalarValue = 0.5;
	Value.DefaultScalarValue = 0.25; Value.bSemanticProjectionComplete = true;
	Base.Record.Values.Add(Value); Base.Record.ValueCount = 1;
	Base.Record.RuntimeComponentIdentities.Add(
		TEXT("9:Values(0)|55:/Script/DynamicMaterial.DMMaterialValueFloat1|1:1|"));
	FHyperAIDynamicMaterialLayerView Layer;
	Layer.ComponentPath = TEXT("Slots(0)/Layers(0)");
	Layer.ClassPath = UDMMaterialLayerObject::StaticClass()->GetPathName();
	Layer.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Layer.SlotIndex = 0; Layer.LayerIndex = 0;
	Layer.MaterialProperty = static_cast<int32>(EDMMaterialPropertyType::BaseColor);
	Layer.bEnabled = true;
	Layer.EffectStackComponentPath = Layer.ComponentPath + TEXT("/Effects");
	Layer.EffectStackClassPath = UDMMaterialEffectStack::StaticClass()->GetPathName();
	Layer.EffectStackLifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Layer.bEffectStackEnabled = true; Layer.EffectCount = 0;
	Layer.bSemanticProjectionComplete = true;
	FHyperAIDynamicMaterialStageView Stage;
	Stage.ComponentPath = TEXT("Slots(0)/Layers(0)/Stages(0)");
	Stage.ClassPath = UDMMaterialStage::StaticClass()->GetPathName();
	Stage.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Stage.StageIndex = 0; Stage.StageType = static_cast<int32>(EDMMaterialLayerStage::Base);
	Stage.SourceClassPath = UDMMaterialStageInputValue::StaticClass()->GetPathName();
	Stage.bEnabled = true; Stage.bCanChangeSource = true;
	Stage.bSemanticProjectionComplete = true;
	Stage.Source.ComponentPath = Stage.ComponentPath + TEXT("/Source");
	Stage.Source.ClassPath = Stage.SourceClassPath;
	Stage.Source.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Stage.Source.ValueComponentPath = Value.ComponentPath;
	Stage.Source.bSemanticProjectionComplete = true;
	FHyperAIDynamicMaterialConnectorView Output;
	Output.Index = 0; Output.Name = TEXT("Value"); Output.ValueType = 2;
	Stage.Source.OutputConnectors.Add(Output);
	Layer.Stages.Add(Stage); Base.Record.Layers.Add(Layer);
	Base.Record.LayerCount = 1; Base.Record.StageCount = 1;
	const FString Revision = FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Base);
	FHyperAIStudioDynamicMaterialBackendOperation LayerEdit;
	LayerEdit.Kind = EHyperAIStudioDynamicMaterialOperationKind::SetLayerEnabled;
	LayerEdit.Type = TEXT("layer.set_enabled"); LayerEdit.TargetPath = Base.Record.AssetPath;
	LayerEdit.TargetFamily = Base.Record.Family; LayerEdit.ComponentPath = Layer.ComponentPath;
	FHyperAIStudioDynamicMaterialBackendOperation StageEdit = LayerEdit;
	StageEdit.Kind = EHyperAIStudioDynamicMaterialOperationKind::SetStageEnabled;
	StageEdit.Type = TEXT("stage.set_enabled"); StageEdit.ComponentPath = Stage.ComponentPath;
	TestFalse(TEXT("hidden membership prevents layer mutation certification"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			LayerEdit, Base.Record));
	TestFalse(TEXT("hidden membership prevents stage mutation certification"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, Base.Record));
	auto Mutates = [&](TFunctionRef<void(FHyperAIStudioDynamicMaterialValueSnapshot&)> Change)
	{
		auto Copy = Base; Change(Copy);
		return FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Copy) != Revision;
	};
	TestTrue(TEXT("dirty mutates CAS"), Mutates([](auto& S){ S.Record.bPackageDirty = true; }));
	TestTrue(TEXT("generated dirty mutates CAS"), Mutates([](auto& S){ S.Record.bGeneratedMaterialPackageDirty = true; }));
	TestTrue(TEXT("compile mutates CAS"), Mutates([](auto& S){ S.Record.bCompiling = true; }));
	TestTrue(TEXT("compile error mutates CAS"), Mutates([](auto& S){ S.Record.bCompileError = true; }));
	TestTrue(TEXT("queued model build mutates CAS"), Mutates([](auto& S){ S.Record.bBuildRequested = true; }));
	TestTrue(TEXT("preview state mutates CAS"), Mutates([](auto& S){ S.Record.bPreviewModified = true; }));
	TestTrue(TEXT("generated material state mutates CAS"), Mutates([](auto& S){ S.Record.GeneratedMaterialStateId = TEXT("state-b"); }));
	TestTrue(TEXT("disk existence mutates CAS"), Mutates([](auto& S){ S.Record.DiskExistence = TEXT("unknown"); }));
	TestTrue(TEXT("reference evidence scope mutates CAS"), Mutates([](auto& S){ S.Record.ReferenceEvidence = TEXT("changed"); }));
	TestTrue(TEXT("runtime component membership mutates CAS"), Mutates([](auto& S)
	{
		S.Record.RuntimeComponentIdentities[0] += TEXT("changed");
	}));
	TestTrue(TEXT("exact component ownership path mutates CAS"), Mutates([](auto& S)
	{
		S.Record.ComponentOwnershipIdentities[0] += TEXT("|foreign-owner");
	}));
	TestTrue(TEXT("component ownership completeness mutates CAS"), Mutates([](auto& S)
	{
		S.Record.bComponentOwnershipProjectionComplete = false;
	}));
	TestTrue(TEXT("hidden membership blocker mutates CAS"), Mutates([](auto& S)
	{
		S.Record.MutationRevisionBlockers.Add(TEXT("foreign_hidden_membership"));
	}));
	TestTrue(TEXT("typed object reference mutates CAS"), Mutates([](auto& S){ S.Record.Values[0].ObjectValuePath = TEXT("/Game/T.T"); }));
	TestTrue(TEXT("typed value mutates CAS"), Mutates([](auto& S){ S.Record.Values[0].ScalarValue = 0.75; }));
	TestTrue(TEXT("public setter range mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Values[0].bHasValueRange = true;
		S.Record.Values[0].ValueRangeMin = 0.0;
		S.Record.Values[0].ValueRangeMax = 1.0;
	}));
	TestTrue(TEXT("same-shape persisted default value mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Values[0].DefaultScalarValue = 0.75;
	}));
	TestTrue(TEXT("same-shape explicit parameter identity mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Values[0].bHasExplicitParameter = true;
		S.Record.Values[0].ParameterComponentPath = TEXT("Parameters/Roughness");
		S.Record.Values[0].ParameterClassPath = TEXT("/Script/DynamicMaterial.DMMaterialParameter");
		S.Record.Values[0].ParameterLifetimeState =
			static_cast<int32>(EDMComponentLifetimeState::Added);
		S.Record.Values[0].ParameterParentComponentPath = S.Record.Values[0].ComponentPath;
		S.Record.Values[0].ExplicitParameterName = TEXT("RoughnessExplicit");
	}));
	TestTrue(TEXT("same-shape model root flags mutate CAS"), Mutates([](auto& S)
	{
		S.Record.GeneralFlags ^= 1;
	}));
	TestTrue(TEXT("same-shape material-property input type mutates CAS"), Mutates([](auto& S)
	{
		S.Record.MaterialProperties[0].InputConnectorType += 1;
	}));
	TestTrue(TEXT("same-shape material-property connection mutates CAS"), Mutates([](auto& S)
	{
		S.Record.MaterialProperties[0].InputChannels[0].OutputChannel = 1;
	}));
	TestTrue(TEXT("same-shape material-property alpha link mutates CAS"), Mutates([](auto& S)
	{
		S.Record.MaterialProperties[0].AlphaValueComponentIdentity = TEXT("changed-alpha-link");
	}));
	TestTrue(TEXT("same-shape slot connector state mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Slots[0].OutputConnectorTypeSets[0] = TEXT("property:1|types:changed");
	}));
	TestTrue(TEXT("same-shape layer lifetime mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].LifetimeState =
			static_cast<int32>(EDMComponentLifetimeState::Removed);
	}));
	TestTrue(TEXT("same-shape effect-stack state mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].bEffectStackEnabled = false;
	}));
	TestTrue(TEXT("same-shape stage lifetime mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].Stages[0].LifetimeState =
			static_cast<int32>(EDMComponentLifetimeState::Removed);
	}));
	TestTrue(TEXT("same-shape stage source class mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].Stages[0].Source.ClassPath = TEXT("changed-source-class");
	}));
	TestTrue(TEXT("same-shape source value link mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].Stages[0].Source.ValueComponentPath = TEXT("Values(1)");
	}));
	TestTrue(TEXT("same-shape connector contract mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].Stages[0].Source.OutputConnectors[0].ValueType = 7;
	}));
	TestTrue(TEXT("layer UV link mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Layers[0].bTextureUVLinkEnabled = true;
	}));
	auto UnprojectedInputTopology = Base;
	UnprojectedInputTopology.bComplete = false;
	UnprojectedInputTopology.Record.bRevisionComplete = false;
	FHyperAIDynamicMaterialComponentView Input;
	Input.ComponentPath = Stage.ComponentPath + TEXT("/Inputs(0)");
	Input.ClassPath = Stage.SourceClassPath;
	Input.LifetimeState = static_cast<int32>(EDMComponentLifetimeState::Added);
	Input.ValueComponentPath = Value.ComponentPath;
	Input.bSemanticProjectionComplete = true;
	Input.OutputConnectors.Add(Output);
	UnprojectedInputTopology.Record.Layers[0].Stages[0].Inputs.Add(Input);
	FHyperAIDynamicMaterialConnectionView Connection;
	Connection.InputIndex = 0;
	FHyperAIDynamicMaterialConnectionChannelView Channel;
	Channel.SourceIndex = 1; Channel.OutputIndex = 0; Channel.OutputChannel = 1;
	Connection.Channels.Add(Channel);
	UnprojectedInputTopology.Record.Layers[0].Stages[0].InputConnections.Add(Connection);
	UnprojectedInputTopology.Record.Layers[0].Stages[0].bSemanticProjectionComplete = false;
	UnprojectedInputTopology.Record.Layers[0].bSemanticProjectionComplete = false;
	const FString InputTopologyRevision =
		FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(UnprojectedInputTopology);
	auto MutatesInputTopology = [&](TFunctionRef<void(FHyperAIStudioDynamicMaterialValueSnapshot&)> Change)
	{
		auto Copy = UnprojectedInputTopology; Change(Copy);
		return FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Copy)
			!= InputTopologyRevision;
	};
	TestTrue(TEXT("same-shape component-input link mutates incomplete CAS"),
		MutatesInputTopology([](auto& S)
		{
			S.Record.Layers[0].Stages[0].Inputs[0].ValueComponentPath = TEXT("Values(1)");
		}));
	TestTrue(TEXT("same-shape input connection mutates incomplete CAS"),
		MutatesInputTopology([](auto& S)
		{
			S.Record.Layers[0].Stages[0].InputConnections[0].Channels[0].OutputChannel = 2;
		}));
	TestFalse(TEXT("nonempty direct-value input topology is not executable"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, UnprojectedInputTopology.Record));
	auto ForeignOwner = Base;
	ForeignOwner.Record.bComponentOwnershipProjectionComplete = false;
	ForeignOwner.Record.ComponentOwnershipIdentities[0] =
		TEXT("Values(0)|owner=/Game/Foreign/DM.DM|roundtrip=0");
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(ForeignOwner);
	TestFalse(TEXT("foreign owner/path roundtrip blocks mutation projection"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, ForeignOwner.Record));
	auto HypotheticalCompleteMembership = Base;
	HypotheticalCompleteMembership.Record.bMutationRevisionComplete = true;
	HypotheticalCompleteMembership.Record.MutationRevisionBlockers.Reset();
	TestTrue(TEXT("synthetic exact-Exists control satisfies the pure closed projection"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, HypotheticalCompleteMembership.Record));
	auto AbsentOnDisk = HypotheticalCompleteMembership;
	AbsentOnDisk.Record.DiskExistence = TEXT("does_not_exist");
	AbsentOnDisk.Record.bExistsOnDisk = false;
	TestFalse(TEXT("DoesNotExist can never certify an edit target"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, AbsentOnDisk.Record));
	bool Truncated = false;
	TestFalse(TEXT("valid base has no independent errors"),
		FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(Base, false, 16, Truncated)
			.ContainsByPredicate([](const auto& Issue){ return Issue.Severity == TEXT("error"); }));
	auto Media = Base;
	Media.bComplete = false; Media.Record.bRevisionComplete = false;
	Media.Record.Values[0].ValueKind = TEXT("media_stream_bridge");
	Media.Record.Values[0].bMediaBridgeValue = true; Media.Record.Values[0].bMediaSourceIdentityKnown = false;
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Media);
	const auto MediaIssues = FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(Media, false, 16, Truncated);
	TestTrue(TEXT("media identity gap fails closed"), MediaIssues.ContainsByPredicate([](const auto& Issue)
	{
		return Issue.Code == TEXT("media_bridge_source_identity_unproven");
	}));
	auto UnprojectedSource = Base;
	UnprojectedSource.bComplete = false;
	UnprojectedSource.Record.bRevisionComplete = false;
	UnprojectedSource.Record.Layers[0].Stages[0].Source.bSemanticProjectionComplete = false;
	UnprojectedSource.Record.Layers[0].Stages[0].bSemanticProjectionComplete = false;
	UnprojectedSource.Record.Layers[0].bSemanticProjectionComplete = false;
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(UnprojectedSource);
	const auto SourceIssues = FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
		UnprojectedSource, false, 32, Truncated);
	TestTrue(TEXT("unprojected same-shape source state cannot issue CAS"),
		SourceIssues.ContainsByPredicate([](const auto& Issue)
		{
				return Issue.Code == TEXT("component_semantic_projection_incomplete")
					|| Issue.Code == TEXT("stage_semantic_projection_incomplete");
			}));
	TestFalse(TEXT("unprojected source apply target fails closed"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, UnprojectedSource.Record));
	auto WrongSourceClass = Base;
	WrongSourceClass.Record.Layers[0].Stages[0].SourceClassPath = TEXT("wrong-source-class");
	WrongSourceClass.Record.Layers[0].Stages[0].Source.ClassPath = TEXT("wrong-source-class");
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(WrongSourceClass);
	TestFalse(TEXT("wrong stage-source component class blocks apply despite same shape"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			StageEdit, WrongSourceClass.Record));
	auto UnprojectedProperty = Base;
	UnprojectedProperty.bComplete = false;
	UnprojectedProperty.Record.bRevisionComplete = false;
	UnprojectedProperty.Record.MaterialProperties[0].bSemanticProjectionComplete = false;
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(UnprojectedProperty);
	const auto PropertyIssues = FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
		UnprojectedProperty, false, 32, Truncated);
	TestTrue(TEXT("unknown material-property state cannot issue complete CAS"),
		PropertyIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("material_property_projection_incomplete")
				|| Issue.Code == TEXT("revision_incomplete");
		}));
	TestFalse(TEXT("unknown material-property state blocks layer apply"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			LayerEdit, UnprojectedProperty.Record));
	auto WrongPropertyClass = Base;
	WrongPropertyClass.Record.MaterialProperties[0].ClassPath =
		UDMMaterialProperty::StaticClass()->GetPathName();
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(WrongPropertyClass);
	const auto WrongPropertyClassIssues =
		FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
			WrongPropertyClass, false, 32, Truncated);
	TestTrue(TEXT("unknown same-shape material-property subclass is rejected"),
		WrongPropertyClassIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("material_property_projection_incomplete");
		}));
	TestFalse(TEXT("wrong material-property subclass blocks apply"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			LayerEdit, WrongPropertyClass.Record));
	auto NonEmptyEffect = Base;
	NonEmptyEffect.bComplete = false;
	NonEmptyEffect.Record.bRevisionComplete = false;
	NonEmptyEffect.Record.Layers[0].EffectCount = 1;
	NonEmptyEffect.Record.Layers[0].bSemanticProjectionComplete = false;
	FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(NonEmptyEffect);
	const auto EffectIssues = FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
		NonEmptyEffect, false, 32, Truncated);
	TestTrue(TEXT("non-empty effect source state cannot issue complete CAS"),
		EffectIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("layer_semantic_projection_incomplete")
				|| Issue.Code == TEXT("revision_incomplete");
		}));
	TestFalse(TEXT("non-empty effect layer apply target fails closed"),
		FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
			LayerEdit, NonEmptyEffect.Record));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialTriStateNormalizationAndTypeTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.TriStateNormalizationAndType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialTriStateNormalizationAndTypeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DynamicMaterial::Tests;
	using UE::AssetRegistry::EExists;
	TestEqual(TEXT("Exists disclosed"),
		FHyperAIStudioDynamicMaterialContracts::ClassifyAssetRegistryExistence(EExists::Exists),
		FString(TEXT("exists")));
	TestEqual(TEXT("DoesNotExist disclosed"),
		FHyperAIStudioDynamicMaterialContracts::ClassifyAssetRegistryExistence(EExists::DoesNotExist),
		FString(TEXT("does_not_exist")));
	TestEqual(TEXT("Unknown disclosed"),
		FHyperAIStudioDynamicMaterialContracts::ClassifyAssetRegistryExistence(EExists::Unknown),
		FString(TEXT("unknown")));
	TestFalse(TEXT("high-density value collection rejected before traversal"),
		FHyperAIStudioDynamicMaterialContracts::IsComponentCollectionWithinBound(
			FHyperAIStudioDynamicMaterialContracts::MaxComponents + 1,
			FHyperAIStudioDynamicMaterialContracts::MaxComponents));
	TestTrue(TEXT("bounded plain semantic text accepted"),
		FHyperAIStudioDynamicMaterialContracts::IsBoundedSimpleSemanticText(
			FText::FromString(TEXT("bounded")),
			FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters));
	TestFalse(TEXT("giant semantic text rejected before persisted-history serialization"),
		FHyperAIStudioDynamicMaterialContracts::IsBoundedSimpleSemanticText(
			FText::FromString(FString::ChrN(
				FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters + 1,
				TEXT('x'))),
			FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters));
	const FText ComplexText = FText::Format(
		FText::FromString(TEXT("{0}")), FText::FromString(TEXT("formatted")));
	TestFalse(TEXT("complex format history fails closed"),
		FHyperAIStudioDynamicMaterialContracts::IsBoundedSimpleSemanticText(
			ComplexText, FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters));

	FHyperAIDynamicMaterialPlanOperation Public = ScalarOperation(
		TEXT("/Game/Test/DM.DM"), TEXT("Values(0)"), FakeRevision(TEXT("revision")), 0.1);
	FHyperAIStudioDynamicMaterialBackendOperation Backend; FString Code, Error;
	TestTrue(TEXT("float-backed scalar normalizes"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(Public, Backend, Code, Error));
	const double PersistedScalar = static_cast<double>(static_cast<float>(Public.ScalarValue));
	TestEqual(TEXT("backend seals exact persisted scalar"), Backend.ScalarValue, PersistedScalar);
	TestTrue(TEXT("pre-cast double is not retained in semantic payload"),
		Backend.ScalarValue != Public.ScalarValue);
	const FString Base = FakeRevision(TEXT("base"));
	const TArray<FHyperAIStudioDynamicMaterialBackendOperation> NormalizedOperations = {Backend};
	const FString NormalizedFingerprint =
		FHyperAIStudioDynamicMaterialContracts::ComputePayloadSemanticFingerprint(
			NormalizedOperations, Base);
	auto PersistedBackend = Backend; PersistedBackend.ScalarValue = PersistedScalar;
	const TArray<FHyperAIStudioDynamicMaterialBackendOperation> PersistedOperations = {PersistedBackend};
	TestEqual(TEXT("plan/effect fingerprint matches persisted representation"),
		NormalizedFingerprint,
		FHyperAIStudioDynamicMaterialContracts::ComputePayloadSemanticFingerprint(
			PersistedOperations, Base));

	auto Overflow = Public; Overflow.ScalarValue = std::numeric_limits<double>::max();
	TestFalse(TEXT("DBL_MAX rejected before float setter"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
			Overflow, Backend, Code, Error));
	auto ColorOverflow = Public; ColorOverflow.Type = TEXT("value.set_color");
	ColorOverflow.ScalarValue = 0.0; ColorOverflow.VectorValue.X = std::numeric_limits<double>::max();
	TestFalse(TEXT("color float overflow rejected"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
			ColorOverflow, Backend, Code, Error));
	auto Vector2Unused = Public; Vector2Unused.Type = TEXT("value.set_vector2");
	Vector2Unused.ScalarValue = 0.0; Vector2Unused.VectorValue = FVector4(1, 2, 3, 0);
	TestFalse(TEXT("vector2 unused Z rejected"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
			Vector2Unused, Backend, Code, Error));

	TestTrue(TEXT("scalar operation restored"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(Public, Backend, Code, Error));
	FHyperAIDynamicMaterialValueView Value;
	Value.ComponentPath = Backend.ComponentPath; Value.ValueKind = TEXT("scalar");
	Value.ClassPath = UDMMaterialValueFloat1::StaticClass()->GetPathName();
	TestTrue(TEXT("exact scalar component class matches"),
		FHyperAIStudioDynamicMaterialContracts::IsExactValueOperationMatch(Backend, Value));
	Value.bHasValueRange = true; Value.ValueRangeMin = 0.0; Value.ValueRangeMax = 0.05;
	TestTrue(TEXT("scalar is normalized through exact public setter range"),
		FHyperAIStudioDynamicMaterialContracts::NormalizeOperationForCapturedValue(
			Backend, Value, Code));
	TestEqual(TEXT("range-clamped scalar seals persisted float"), Backend.ScalarValue,
		static_cast<double>(static_cast<float>(0.05)));
	Value.ScalarValue = Backend.ScalarValue;
	TestFalse(TEXT("setter-near-equal scalar is a pure-shadow no-op"),
		FHyperAIStudioDynamicMaterialContracts::WouldValueSetterHaveEffect(Backend, Value));
	Backend.ScalarValue = static_cast<double>(static_cast<float>(0.75));
	TestTrue(TEXT("meaningful scalar delta remains visible to pure shadow"),
		FHyperAIStudioDynamicMaterialContracts::WouldValueSetterHaveEffect(Backend, Value));

	FHyperAIDynamicMaterialPlanOperation ColorPublic;
	ColorPublic.Type = TEXT("value.set_color"); ColorPublic.TargetPath = Public.TargetPath;
	ColorPublic.TargetFamily = Public.TargetFamily; ColorPublic.ExpectedRevision = Public.ExpectedRevision;
	ColorPublic.ComponentPath = Public.ComponentPath; ColorPublic.VectorValue = FVector4(0.1, 0.2, 0.3, 0.4);
	FHyperAIStudioDynamicMaterialBackendOperation ColorBackend;
	TestTrue(TEXT("color shape normalizes"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
			ColorPublic, ColorBackend, Code, Error));
	FHyperAIDynamicMaterialValueView RgbValue;
	RgbValue.ComponentPath = ColorBackend.ComponentPath; RgbValue.ValueKind = TEXT("color");
	RgbValue.ClassPath = UDMMaterialValueFloat3RGB::StaticClass()->GetPathName();
	TestTrue(TEXT("RGB target-specific normalization succeeds"),
		FHyperAIStudioDynamicMaterialContracts::NormalizeOperationForCapturedValue(
			ColorBackend, RgbValue, Code));
	TestEqual(TEXT("RGB public setter seals alpha one"), ColorBackend.VectorValue.W, 1.0);
	RgbValue.VectorValue = ColorBackend.VectorValue;
	RgbValue.VectorValue.W = 0.0;
	TestFalse(TEXT("RGB alpha-only difference matches UE 5.8 setter no-op gate"),
		FHyperAIStudioDynamicMaterialContracts::WouldValueSetterHaveEffect(
			ColorBackend, RgbValue));
	RgbValue.VectorValue.X = 0.9;
	TestTrue(TEXT("RGB channel delta remains a semantic effect"),
		FHyperAIStudioDynamicMaterialContracts::WouldValueSetterHaveEffect(
			ColorBackend, RgbValue));

	Value.ClassPath = UDMMaterialValueFloat2::StaticClass()->GetPathName();
	TestFalse(TEXT("wrong component class rejected despite forged value kind"),
		FHyperAIStudioDynamicMaterialContracts::IsExactValueOperationMatch(Backend, Value));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialShapeContradictionStagedSafetyTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.ShapeContradictionAndStagedSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialShapeContradictionStagedSafetyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DynamicMaterial::Tests;
	const FString Revision = FakeRevision(TEXT("dynamic-revision"));
	const FString Target = TEXT("/Game/__HyperAIStudioAutomation/DM_Missing.DM_Missing");
	const auto Operation = ScalarOperation(Target, TEXT("Values(0)"), Revision, 0.25);
	FHyperAIStudioDynamicMaterialBackendOperation Backend; FString Code, Error;
	TestTrue(TEXT("closed scalar operation normalizes"),
		FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(Operation, Backend, Code, Error));
	auto NonFinite = Operation; NonFinite.ScalarValue = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("non-finite rejected"), FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
		NonFinite, Backend, Code, Error));
	auto Media = Operation; Media.Type = TEXT("media_stream.set_source"); Media.ScalarValue = 0.0;
	TestFalse(TEXT("media private setter not dispatched"), FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
		Media, Backend, Code, Error));

	FHyperAIDynamicMaterialApplyPlanRequest DryRequest;
	DryRequest.bDryRun = true;
	DryRequest.OperationId = TEXT("dynamic-material-dry-evidence-001");
	DryRequest.Operations = {Operation};
	const auto DryReport =
		UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_apply_plan(DryRequest);
	TestFalse(TEXT("dry-run cannot certify executable success"), DryReport.bOk);
	TestFalse(TEXT("dry-run never stages"), DryReport.bStaged);
	TestFalse(TEXT("dry-run never submits execution"), DryReport.bExecutionSubmitted);
	TestFalse(TEXT("dry-run never permits fallback"), DryReport.bFallbackPermitted);
	TestFalse(TEXT("dry-run never certifies typed replay"),
		DryReport.Effects.bTypedShadowReplayComplete);
	TestFalse(TEXT("dry-run never certifies a transaction"),
		DryReport.Effects.bTransactionOnce);
	TestFalse(TEXT("dry-run never certifies compile"), DryReport.Effects.bCompileOnce);
	TestFalse(TEXT("dry-run never certifies save"), DryReport.Effects.bSaveOnce);
	TestFalse(TEXT("dry-run never certifies validation"), DryReport.Effects.bValidateOnce);
	TestFalse(TEXT("dry-run never certifies fresh verification"),
		DryReport.Effects.bFreshVerifyOnce);
	TestTrue(TEXT("dry-run exposes no executable authorization"),
		DryReport.AuthorizationPlanHash.IsEmpty());
	TestTrue(TEXT("dry-run exposes no effect fingerprint"),
		DryReport.EffectFingerprint.IsEmpty());
	TestNull(TEXT("dry-run did not load or mutate missing target"),
		FSoftObjectPath(Target).ResolveObject());

	FHyperAIDynamicMaterialApplyPlanRequest Request;
	Request.bDryRun = false; Request.OperationId = TEXT("dynamic-material-automation-001");
	Request.ExpectedPlanHash = FakeRevision(TEXT("plan"));
	Request.Operations = {Operation, Operation};
	const auto Report = UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_apply_plan(Request);
	TestFalse(TEXT("unproven non-dry plan cannot claim success"), Report.bOk);
	TestFalse(TEXT("no local stage"), Report.bStaged);
	TestFalse(TEXT("no execution"), Report.bExecutionSubmitted);
	TestFalse(TEXT("no fallback"), Report.bFallbackPermitted);
	TestTrue(TEXT("contradictory component write diagnosed"), Report.Issues.ContainsByPredicate([](const auto& Issue)
	{
		return Issue.Code == TEXT("contradictory_component_writes");
	}));
	TestEqual(TEXT("admitted valid non-dry terminal state is centrally gated"),
		FString(FHyperAIStudioDynamicMaterialContracts::NonDryCallableState),
		FString(TEXT("bounded_compile_or_runtime_cas_backend_required")));
	TestNull(TEXT("callable did not load or mutate missing target"), FSoftObjectPath(Target).ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialBoundsCloneTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.BoundsAndClone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialBoundsCloneTest::RunTest(const FString& Parameters)
{
	FHyperAIDynamicMaterialInspectRequest Request;
	Request.TargetPath = TEXT("/Game/Test/DM.DM"); Request.MaxComponents = 0;
	const auto Report = UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_inspect(Request);
	TestFalse(TEXT("invalid bound fails"), Report.bOk);
	TestEqual(TEXT("bound status"), Report.Status, FString(TEXT("invalid_request_bounds")));
	Request.MaxComponents = FHyperAIStudioDynamicMaterialContracts::MaxComponents;
	Request.MaxOutputBytes = FHyperAIStudioDynamicMaterialContracts::MinOutputBytes - 1;
	const auto OutputBoundReport =
		UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_inspect(Request);
	TestFalse(TEXT("undersized output envelope fails before capture"), OutputBoundReport.bOk);
	TestEqual(TEXT("output envelope bound status"), OutputBoundReport.Status,
		FString(TEXT("invalid_request_bounds")));

	FHyperAIStudioDynamicMaterialTypedPayload Payload;
	FHyperAIStudioDynamicMaterialBackendOperation Operation;
	Operation.Type = TEXT("value.set_scalar"); Operation.Kind = EHyperAIStudioDynamicMaterialOperationKind::SetScalar;
	Operation.TargetPath = TEXT("/Game/Test/DM.DM"); Operation.TargetFamily = TEXT("dynamic_material_model");
	Operation.ExpectedRevision = HyperAIStudio::DynamicMaterial::Tests::FakeRevision(TEXT("revision"));
	Operation.ComponentPath = TEXT("Values(0)"); Operation.ScalarValue = 0.5;
	Payload.Operations.Add(Operation); Payload.BaseRevision = HyperAIStudio::DynamicMaterial::Tests::FakeRevision(TEXT("base"));
	Payload.SemanticFingerprint = FHyperAIStudioDynamicMaterialContracts::ComputePayloadSemanticFingerprint(
		Payload.Operations, Payload.BaseRevision);
	const auto CloneBase = Payload.CloneImmutable();
	const auto& Clone = static_cast<const FHyperAIStudioDynamicMaterialTypedPayload&>(CloneBase.Get());
	Payload.Operations[0].ComponentPath = TEXT("Changed");
	TestNotEqual(TEXT("clone owns operation strings"), Clone.Operations[0].ComponentPath,
		Payload.Operations[0].ComponentPath);
	TestTrue(TEXT("bounded byte size accepted"), Clone.GetBoundedByteSize() > 0
		&& Clone.GetBoundedByteSize() <= FHyperAIStudioDomainLimits::MaxRequestBytes);

	FHyperAIStudioDynamicMaterialValueSnapshot Oversized;
	Oversized.bComplete = true;
	Oversized.Record.bRevisionComplete = true;
	Oversized.Record.AssetPath = FString::ChrN(
		static_cast<int32>(FHyperAIStudioExtensionRuntime::MaxHashInputBytes) + 1,
		TEXT('x'));
	TestTrue(TEXT("oversized canonical element cannot produce a revision"),
		FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Oversized).IsEmpty());
	TestFalse(TEXT("oversized canonical element clears completeness"), Oversized.bComplete);

	FHyperAIStudioDynamicMaterialValueSnapshot Expired;
	Expired.bComplete = true;
	Expired.Record.bRevisionComplete = true;
	TestTrue(TEXT("expired canonical seal cannot produce a revision"),
		FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(Expired, 0.0).IsEmpty());
	TestFalse(TEXT("expired canonical seal clears completeness"), Expired.bComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIDynamicMaterialAdapterZeroEffectTest,
	"HyperAIStudio.NativeTools.DynamicMaterial.AdapterHardZeroEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIDynamicMaterialAdapterZeroEffectTest::RunTest(const FString& Parameters)
{
	using Action = EHyperAIStudioDomainExecutionActionKind;
	FHyperAIStudioDynamicMaterialTypedPayload Payload;
	FHyperAIStudioDomainDispatchContext Context;
	FHyperAIStudioDynamicMaterialDomainAdapter Adapter;
	const TArray<Action> Actions = {
		Action::Apply, Action::Compile, Action::Save, Action::Validate, Action::VerifyFresh};
	for (const Action ActionKind : Actions)
	{
		Context.ActionKind = ActionKind;
		const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, Payload);
		TestEqual(TEXT("stable backend blocker for every mutation phase"), Result.StatusCode,
			FString(TEXT("bounded_compile_or_runtime_cas_backend_required")));
		TestEqual(TEXT("every mutation phase rejects before effect"),
			static_cast<uint8>(Result.Outcome),
			static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect));
		TestFalse(TEXT("blocked phase has no result payload"), Result.Payload.IsValid());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
