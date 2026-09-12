// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioMaterialsToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <limits>

namespace HyperAIStudio::Materials::Tests
{
	FString UniqueName(const TCHAR* Prefix)
	{
		return FString(Prefix) + TEXT("_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	UMaterial* CreateMaterialFixture(FString& OutPath, const int32 NodeCount = 2)
	{
		const FString Name = UniqueName(TEXT("M_HyperAIMaterialTest"));
		UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioAutomation/") + Name));
		if (!Package) return nullptr;
		UMaterial* Material = NewObject<UMaterial>(Package, UMaterial::StaticClass(), FName(*Name),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Material) return nullptr;
		for (int32 Index = 0; Index < NodeCount; ++Index)
		{
			UMaterialExpressionConstant* Constant =
				NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
			Constant->R = static_cast<float>(Index + 1);
			Constant->MaterialExpressionEditorX = Index * 100;
			Constant->Material = Material;
			Constant->UpdateMaterialExpressionGuid(true, false);
			Material->GetExpressionCollection().AddExpression(Constant);
			if (Index == 0)
			{
				FExpressionInput* BaseColor = Material->GetExpressionInputForProperty(MP_BaseColor);
				BaseColor->Expression = Constant;
			}
		}
		OutPath = Material->GetPathName();
		return Material;
	}

	void Discard(UObject* Object)
	{
		if (Object && Object->GetOutermost()) Object->GetOutermost()->SetDirtyFlag(false);
	}

	FHyperAIMaterialPlanOperation CompoundOperation(const FString& TargetPath)
	{
		FHyperAIMaterialPlanOperation Operation;
		Operation.Type = TEXT("compound_create_configure_graph");
		Operation.TargetPath = TargetPath;
		Operation.TargetFamily = TEXT("material");
		FHyperAIMaterialNodeSpec Constant;
		Constant.Kind = TEXT("constant"); Constant.NodeId = TEXT("base");
		Constant.Scalar = 0.25; Constant.EditorX = -200;
		Operation.Nodes.Add(Constant);
		FHyperAIMaterialNodeSpec Roughness;
		Roughness.Kind = TEXT("scalar_parameter"); Roughness.NodeId = TEXT("roughness");
		Roughness.Name = TEXT("Roughness"); Roughness.Group = TEXT("Surface");
		Roughness.Scalar = 0.5; Roughness.EditorX = -200; Roughness.EditorY = 150;
		Operation.Nodes.Add(Roughness);
		FHyperAIMaterialNodeSpec Add;
		Add.Kind = TEXT("add"); Add.NodeId = TEXT("sum"); Add.EditorX = 50;
		Operation.Nodes.Add(Add);
		FHyperAIMaterialEdgeSpec AddA;
		AddA.FromNodeId = TEXT("base"); AddA.ToNodeId = TEXT("sum"); AddA.ToInput = TEXT("A");
		Operation.Edges.Add(AddA);
		FHyperAIMaterialEdgeSpec AddB;
		AddB.FromNodeId = TEXT("roughness"); AddB.ToNodeId = TEXT("sum"); AddB.ToInput = TEXT("B");
		Operation.Edges.Add(AddB);
		FHyperAIMaterialOutputSpec BaseOutput;
		BaseOutput.Property = TEXT("base_color"); BaseOutput.FromNodeId = TEXT("sum");
		Operation.Outputs.Add(BaseOutput);
		FHyperAIMaterialOutputSpec RoughnessOutput;
		RoughnessOutput.Property = TEXT("roughness"); RoughnessOutput.FromNodeId = TEXT("roughness");
		Operation.Outputs.Add(RoughnessOutput);
		return Operation;
	}

	FHyperAIStudioMaterialValueSnapshot MatchingCompoundSnapshot(
		const FHyperAIStudioMaterialBackendOperation& Operation)
	{
		FHyperAIStudioMaterialValueSnapshot Snapshot;
		Snapshot.bComplete = true;
		auto& Record = Snapshot.Record;
		Record.Family = TEXT("material"); Record.AssetPath = Operation.TargetPath;
		Record.ClassPath = UMaterial::StaticClass()->GetPathName();
		Record.OuterPath = FPackageName::ObjectPathToPackageName(Operation.TargetPath);
		Record.OutermostPackageName = Record.OuterPath;
		Record.bRootOwnershipComplete = true;
		Record.bRevisionComplete = true; Record.bLoaded = true;
		Record.DiskExistence = TEXT("exists"); Record.bExistsOnDisk = true;
		Record.bCompileStateKnown = true;
		FString ProjectionError;
		if (!FHyperAIStudioMaterialsContracts::BuildExpectedCompoundSemanticGraph(
			Operation, Record.Nodes, Record.Edges, ProjectionError))
		{
			Snapshot.bComplete = false; Record.bRevisionComplete = false;
		}
		if (!FHyperAIStudioMaterialsContracts::BuildExpectedCompoundPropertyState(
			Operation, Record.PropertyInputs, ProjectionError))
		{
			Snapshot.bComplete = false; Record.bRevisionComplete = false;
		}
		Record.NodeCount = Record.Nodes.Num(); Record.EdgeCount = Record.Edges.Num();
		FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Snapshot);
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsManifestReflectionCohortTest,
	"HyperAIStudio.NativeTools.Materials.ManifestReflectionCohort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsManifestReflectionCohortTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIStudioMaterialManifestEntry>& Manifest =
		FHyperAIStudioMaterialsContracts::GetManifest();
	TestEqual(TEXT("atomic core Materials cohort owns exactly three names"), Manifest.Num(), 3);
	TestEqual(TEXT("canonical runtime toolset identity"),
		FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioMaterials.HyperAIStudioMaterialsToolset")));
	TSet<FString> Names;
	for (const FHyperAIStudioMaterialManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		TestEqual(TEXT("one central qualified owner"), Entry.QualifiedToolset,
			FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName());
	}
	TestTrue(TEXT("exact inspect"), Names.Contains(TEXT("hyper_material_inspect")));
	TestTrue(TEXT("exact apply"), Names.Contains(TEXT("hyper_material_apply_plan")));
	TestTrue(TEXT("exact validate"), Names.Contains(TEXT("hyper_material_validate")));
	TSet<FString> Reflected;
	for (TFieldIterator<UFunction> It(UHyperAIStudioMaterialsToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
		if (It->HasMetaData(TEXT("AICallable"))) Reflected.Add(It->GetName());
	TestEqual(TEXT("reflection exposes only the atomic cohort"), Reflected.Num(), 3);
	TestTrue(TEXT("reflection/manifest exact"), Reflected.Difference(Names).IsEmpty()
		&& Names.Difference(Reflected).IsEmpty());
	TArray<FString> ExactNames = Names.Array(); ExactNames.Sort();
	FHyperAIStudioExtensionCohortAdmission Admission;
	const bool bGeneratedExact = FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
		FHyperAIStudioMaterialsContracts::PackId,
		FHyperAIStudioMaterialsContracts::AtomicCohortId, ExactNames, Admission);
	TestTrue(TEXT("generated catalog valid"), Admission.bCatalogValid);
	TestEqual(TEXT("exact-match flag agrees with generated lookup"),
		Admission.bExactCohortMatch, bGeneratedExact);
	TestEqual(TEXT("production registration is generated-admission only"),
		FHyperAIStudioMaterialsContracts::IsRegistrationAllowed(false),
		bGeneratedExact && Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted);
	if (!bGeneratedExact)
		TestFalse(TEXT("module remains fail-closed before central catalog regeneration"),
			FHyperAIStudioMaterialsContracts::IsRegistrationAllowed(true));
	TestFalse(TEXT("tool class cannot be foreign-owned under a second qualifier"),
		FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioMaterialsToolset::StaticClass(), TEXT("/Script/Foreign.Materials")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsClosedSchemaAndMatrixTest,
	"HyperAIStudio.NativeTools.Materials.ClosedSchemaAndMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsClosedSchemaAndMatrixTest::RunTest(const FString& Parameters)
{
	const UStruct* Operation = FHyperAIMaterialPlanOperation::StaticStruct();
	TestNull(TEXT("no authorization token exposed"), Operation->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("no script field"), Operation->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("no raw class path"), Operation->FindPropertyByName(TEXT("ClassPath")));
	TestNull(TEXT("no raw property dispatch"), Operation->FindPropertyByName(TEXT("PropertyName")));
	const UStruct* Record = FHyperAIMaterialAssetRecord::StaticStruct();
	TestNull(TEXT("no unbounded dependency array"), Record->FindPropertyByName(TEXT("Dependencies")));
	TestNull(TEXT("no unbounded referencer array"), Record->FindPropertyByName(TEXT("Referencers")));
	TestTrue(TEXT("payload schema canonical"), FHyperAIStudioMaterialsContracts::IsCanonicalSha256(
		FHyperAIStudioMaterialsContracts::PayloadSchemaFingerprint()));
	TestTrue(TEXT("result schema canonical"), FHyperAIStudioMaterialsContracts::IsCanonicalSha256(
		FHyperAIStudioMaterialsContracts::ResultSchemaFingerprint()));
	const TArray<FHyperAIMaterialCapabilityStatus> Matrix =
		FHyperAIStudioMaterialsContracts::GetCapabilityMatrix();
	TestEqual(TEXT("matrix has exact two core families"), Matrix.Num(), 2);
	const FHyperAIMaterialCapabilityStatus* Material = Matrix.FindByPredicate([](const auto& Row)
	{
		return Row.Family == TEXT("material");
	});
	TestNotNull(TEXT("material row"), Material);
	if (Material)
	{
		TestFalse(TEXT("mutation backend is not advertised"), Material->bCompoundBackendImplemented);
		TestTrue(TEXT("only pure non-executable shadow evidence declared"),
			Material->SupportedCases.Contains(
				TEXT("pure_non_executable_compound_and_repair_shadow_evidence")));
		TestTrue(TEXT("bounded async mutation backend gap explicit"),
			Material->UnsupportedCases.Contains(
				TEXT("compound_or_repair_mutation_without_bounded_async_cas_backend")));
		TestTrue(TEXT("Epic create delegated"), Material->DelegatedEpicCases.Contains(TEXT("create_material")));
		TestTrue(TEXT("Epic granular connect delegated"), Material->DelegatedEpicCases.Contains(TEXT("connect_expressions")));
		TestTrue(TEXT("dependency fanout delegated"), Material->DelegatedEpicCases.Contains(
			TEXT("hyper_asset_dependency_graph")));
		TestTrue(TEXT("raw reflection unsupported"), Material->UnsupportedCases.Contains(
			TEXT("raw_script_or_reflection_dispatch")));
	}
	const auto& Descriptor = FHyperAIStudioMaterialsContracts::GetAdapterDescriptor();
	TestTrue(TEXT("core Materials adapter has no optional requirement group"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.IsEmpty());
	TestEqual(TEXT("empty core requirement group set is bound by contract fingerprint recomputation"),
		Descriptor.ContractFingerprint,
		FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor));
	TestEqual(TEXT("empty core requirement group set is bound by adapter fingerprint recomputation"),
		Descriptor.AdapterFingerprint,
		FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsExactTypeNoLoadTest,
	"HyperAIStudio.NativeTools.Materials.ExactTypeAndNoLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsExactTypeNoLoadTest::RunTest(const FString& Parameters)
{
	const FString Missing = TEXT("/Game/__HyperAIStudioAutomation/M_NotLoaded.M_NotLoaded");
	TestNull(TEXT("fixture starts unresolved"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIMaterialInspectRequest Request;
	Request.TargetPath = Missing; Request.Scope = TEXT("on_disk_index");
	const FHyperAIMaterialInspectReport MissingReport =
		UHyperAIStudioMaterialsToolset::hyper_material_inspect(Request);
	TestFalse(TEXT("missing on-disk row fails"), MissingReport.bOk);
	TestEqual(TEXT("no-load status exact"), MissingReport.Status, FString(TEXT("asset_not_found_on_disk")));
	TestNull(TEXT("inspection did not synchronously load"), FSoftObjectPath(Missing).ResolveObject());

	const FString Name = HyperAIStudio::Materials::Tests::UniqueName(TEXT("MI_Derived"));
	UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioAutomation/") + Name));
	UMaterialInstanceConstant* Derived = NewObject<UMaterialInstanceConstant>(Package,
		UMaterialInstanceConstant::StaticClass(), FName(*Name));
	TestFalse(TEXT("derived MaterialInterface is outside exact material family"),
		FHyperAIStudioMaterialsContracts::IsExactLoadedFamily(Derived, TEXT("material")));
	HyperAIStudio::Materials::Tests::Discard(Derived);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsRevisionMutationTest,
	"HyperAIStudio.NativeTools.Materials.RevisionMutations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsRevisionMutationTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioMaterialValueSnapshot Base;
	Base.bComplete = true;
	Base.Record.Family = TEXT("material");
	Base.Record.AssetPath = TEXT("/Game/Test/M.M");
	Base.Record.ClassPath = TEXT("/Script/Engine.Material");
	Base.Record.StateId = FGuid::NewGuid().ToString();
	Base.Record.bRevisionComplete = true;
	Base.Record.bLoaded = true;
	Base.Record.DiskExistence = TEXT("does_not_exist");
	Base.Record.bCompileStateKnown = true;
	FHyperAIMaterialNodeView Node;
	Node.StableId = TEXT("guid:a"); Node.PersistedGuid = TEXT("a"); Node.bGuidValid = true;
	Node.Kind = TEXT("scalar_parameter");
	Node.ClassPath = TEXT("/Script/Engine.MaterialExpressionScalarParameter");
	Node.MaterialOwnerPath = Base.Record.AssetPath;
	Node.Name = TEXT("P"); Node.ParameterGuid = TEXT("parameter-a");
	Node.ScalarControlType = 0; Node.SortPriority = 32;
	Node.bShowInputs = true; Node.bShowOutputs = true;
	Node.MenuCategories = {TEXT("Parameters")};
	Node.bSemanticProjectionComplete = true;
	FHyperAIMaterialOutputPortView OutputPort;
	OutputPort.Index = 0; OutputPort.Name = TEXT(""); OutputPort.ValueType = 1;
	Node.OutputPorts.Add(OutputPort);
	Base.Record.Nodes.Add(Node);
	FHyperAIMaterialNodeView AddNode;
	AddNode.StableId = TEXT("guid:add"); AddNode.PersistedGuid = TEXT("add");
	AddNode.bGuidValid = true; AddNode.bSemanticProjectionComplete = true;
	AddNode.Kind = TEXT("add"); AddNode.ClassPath = TEXT("/Script/Engine.MaterialExpressionAdd");
	AddNode.MaterialOwnerPath = Base.Record.AssetPath;
	AddNode.ConstA = 0.0; AddNode.ConstB = 1.0;
	FHyperAIMaterialInputPortView AddInput;
	AddInput.Index = 0; AddInput.Name = TEXT("A"); AddNode.InputPorts.Add(AddInput);
	Base.Record.Nodes.Add(AddNode);
	FHyperAIMaterialNodeView FunctionInput;
	FunctionInput.StableId = TEXT("guid:function-input");
	FunctionInput.PersistedGuid = TEXT("function-input"); FunctionInput.bGuidValid = true;
	FunctionInput.bSemanticProjectionComplete = true; FunctionInput.Kind = TEXT("function_input");
	FunctionInput.MaterialOwnerPath = Base.Record.AssetPath;
	FunctionInput.FunctionId = TEXT("function-id-a"); FunctionInput.FunctionInputType = 0;
	FunctionInput.FunctionPreviewValue = FVector4(1, 2, 3, 4);
	Base.Record.Nodes.Add(FunctionInput);
	FHyperAIMaterialNodeView FunctionOutput;
	FunctionOutput.StableId = TEXT("guid:function-output");
	FunctionOutput.PersistedGuid = TEXT("function-output"); FunctionOutput.bGuidValid = true;
	FunctionOutput.bSemanticProjectionComplete = true; FunctionOutput.Kind = TEXT("function_output");
	FunctionOutput.MaterialOwnerPath = Base.Record.AssetPath;
	FunctionOutput.FunctionId = TEXT("function-id-b");
	Base.Record.Nodes.Add(FunctionOutput);
	Base.Record.NodeCount = Base.Record.Nodes.Num();
	const FString Revision = FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Base);
	auto Mutates = [&](TFunctionRef<void(FHyperAIStudioMaterialValueSnapshot&)> Change)
	{
		FHyperAIStudioMaterialValueSnapshot Copy = Base; Change(Copy);
		return FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Copy) != Revision;
	};
	TestTrue(TEXT("dirty state mutates CAS"), Mutates([](auto& S){ S.Record.bPackageDirty = true; }));
	TestTrue(TEXT("compile state mutates CAS"), Mutates([](auto& S){ S.Record.bCompiling = true; }));
	TestTrue(TEXT("compile error mutates CAS"), Mutates([](auto& S){ S.Record.bCompileError = true; }));
	TestTrue(TEXT("disk existence mutates CAS"), Mutates([](auto& S)
	{
		S.Record.DiskExistence = TEXT("exists"); S.Record.bExistsOnDisk = true;
	}));
	TestTrue(TEXT("reference evidence scope mutates CAS"), Mutates([](auto& S)
	{
		S.Record.ReferenceEvidence = TEXT("changed");
	}));
	TestTrue(TEXT("bounded semantic edge reference mutates CAS"), Mutates([](auto& S)
	{
		FHyperAIMaterialEdgeView Edge; Edge.FromStableId = TEXT("guid:a");
		Edge.ToStableId = TEXT("$material_output"); Edge.ToInputIndex = MP_BaseColor;
		S.Record.Edges.Add(Edge); S.Record.EdgeCount = 1;
	}));
	TestTrue(TEXT("state id mutates CAS"), Mutates([](auto& S){ S.Record.StateId = TEXT("changed"); }));
	TestTrue(TEXT("semantic node mutates CAS"), Mutates([](auto& S){ S.Record.Nodes[0].Scalar = 2.0; }));
	TestTrue(TEXT("same-shape parameter GUID mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].ParameterGuid = TEXT("parameter-b");
	}));
	TestTrue(TEXT("same-shape scalar metadata mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].ScalarControlType = 1;
		S.Record.Nodes[0].EnumerationPath = TEXT("/Game/Enum.Enum");
	}));
	TestTrue(TEXT("same-shape Add fallback mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[1].ConstB = 7.0;
	}));
	TestTrue(TEXT("same-shape input mask mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[1].InputPorts[0].MaskR = 1;
	}));
	TestTrue(TEXT("same-shape output schema mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].OutputPorts[0].Name = TEXT("changed");
	}));
	TestTrue(TEXT("same-shape expression owner context mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].MaterialOwnerPath = TEXT("/Game/Test/Other.Other");
	}));
	TestTrue(TEXT("same-shape persisted base flag mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].bRealtimePreview = !S.Record.Nodes[0].bRealtimePreview;
	}));
	TestTrue(TEXT("same-shape persisted menu category mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].MenuCategories[0] = TEXT("Changed");
	}));
	TestTrue(TEXT("FunctionInput semantics mutate CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[2].FunctionPreviewValue.X = 9.0;
		S.Record.Nodes[2].bUseFunctionPreviewValueAsDefault = true;
	}));
	TestTrue(TEXT("FunctionOutput semantics mutate CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[3].FunctionId = TEXT("function-id-c");
		S.Record.Nodes[3].bFunctionOutputLastPreviewed = true;
	}));
	TestTrue(TEXT("material constant state mutates CAS"), Mutates([](auto& S)
	{
		FHyperAIMaterialPropertyInputView Property;
		Property.PropertyIndex = MP_Roughness; Property.bAvailable = true;
		Property.ConstantState = TEXT("float:0.5"); S.Record.PropertyInputs.Add(Property);
	}));
	TestTrue(TEXT("actual outer identity mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].OuterPath = TEXT("/Game/Foreign/Foreign.Foreign");
	}));
	TestTrue(TEXT("subgraph root identity mutates CAS"), Mutates([](auto& S)
	{
		S.Record.Nodes[0].SubgraphRootStableId = TEXT("guid:foreign");
	}));
	TestTrue(TEXT("asset root package identity mutates CAS"), Mutates([](auto& S)
	{
		S.Record.OutermostPackageName = TEXT("/Game/Foreign/Foreign");
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsTriStatePinsNormalizationAndPostconditionsTest,
	"HyperAIStudio.NativeTools.Materials.TriStatePinsNormalizationAndPostconditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsTriStatePinsNormalizationAndPostconditionsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::Tests;
	using UE::AssetRegistry::EExists;
	TestEqual(TEXT("Exists disclosed"),
		FHyperAIStudioMaterialsContracts::ClassifyAssetRegistryExistence(EExists::Exists),
		FString(TEXT("exists")));
	TestEqual(TEXT("DoesNotExist disclosed"),
		FHyperAIStudioMaterialsContracts::ClassifyAssetRegistryExistence(EExists::DoesNotExist),
		FString(TEXT("does_not_exist")));
	TestEqual(TEXT("Unknown disclosed"),
		FHyperAIStudioMaterialsContracts::ClassifyAssetRegistryExistence(EExists::Unknown),
		FString(TEXT("unknown")));
	TestTrue(TEXT("only exact DoesNotExist proves create absence"),
		FHyperAIStudioMaterialsContracts::IsCreateAbsenceProven(false, EExists::DoesNotExist));
	TestFalse(TEXT("Unknown never proves absence"),
		FHyperAIStudioMaterialsContracts::IsCreateAbsenceProven(false, EExists::Unknown));
	TestFalse(TEXT("Exists never proves absence even if row payload is unavailable"),
		FHyperAIStudioMaterialsContracts::IsCreateAbsenceProven(false, EExists::Exists));
	TestFalse(TEXT("loaded object overrides disk absence"),
		FHyperAIStudioMaterialsContracts::IsCreateAbsenceProven(true, EExists::DoesNotExist));
	TestTrue(TEXT("bounded plain semantic text accepted"),
		FHyperAIStudioMaterialsContracts::IsBoundedSimpleSemanticText(
			FText::FromString(TEXT("bounded"))));
	TestFalse(TEXT("giant semantic text rejected before persisted-history serialization"),
		FHyperAIStudioMaterialsContracts::IsBoundedSimpleSemanticText(FText::FromString(
			FString::ChrN(FHyperAIStudioMaterialsContracts::MaxSemanticTextCharacters + 1, TEXT('x')))));
	const FText Complex = FText::Format(
		FText::FromString(TEXT("{0}")), FText::FromString(TEXT("formatted")));
	TestFalse(TEXT("complex format history fails closed"),
		FHyperAIStudioMaterialsContracts::IsBoundedSimpleSemanticText(Complex));

	FHyperAIMaterialPlanOperation Public = CompoundOperation(
		TEXT("/Game/__HyperAIStudioAutomation/M_Postcondition.M_Postcondition"));
	Public.Nodes[0].Scalar = 0.1;
	FHyperAIStudioMaterialBackendOperation Backend; FString Code, Error;
	TestTrue(TEXT("float-backed compound normalizes"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(Public, Backend, Code, Error));
	TestEqual(TEXT("scalar sealed to persisted float"), Backend.Nodes[0].Scalar,
		static_cast<double>(static_cast<float>(0.1)));
	auto Overflow = Public; Overflow.Nodes[0].Scalar = std::numeric_limits<double>::max();
	TestFalse(TEXT("finite double that overflows persisted float rejected"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(Overflow, Backend, Code, Error));

	auto MissingPin = Public;
	FHyperAIMaterialNodeSpec Add; Add.Kind = TEXT("add"); Add.NodeId = TEXT("bad_sum");
	MissingPin.Nodes.Add(Add);
	FHyperAIMaterialEdgeSpec Missing;
	Missing.FromNodeId = TEXT("base"); Missing.ToNodeId = TEXT("bad_sum"); Missing.ToInput = TEXT("Missing");
	MissingPin.Edges.Add(Missing);
	TestFalse(TEXT("missing closed pin rejected before Prepare"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(MissingPin, Backend, Code, Error));
	TestEqual(TEXT("missing pin code"), Code, FString(TEXT("closed_pin_not_found")));

	auto Incompatible = Public;
	FHyperAIMaterialNodeSpec Vector; Vector.Kind = TEXT("vector_parameter");
	Vector.NodeId = TEXT("tint"); Vector.Name = TEXT("Tint");
	Incompatible.Nodes.Add(Vector);
	Incompatible.Outputs[1].FromNodeId = TEXT("tint");
	Incompatible.Outputs[1].FromOutput = TEXT("RGB");
	TestFalse(TEXT("vector-to-scalar material output rejected in typed shadow"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(Incompatible, Backend, Code, Error));
	TestEqual(TEXT("output type code"), Code, FString(TEXT("material_output_type_mismatch")));

	TestTrue(TEXT("normalized operation restored"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(Public, Backend, Code, Error));
	FHyperAIStudioMaterialValueSnapshot Exact = MatchingCompoundSnapshot(Backend);
	FString Effect;
	TestTrue(TEXT("exact node/edge/output postcondition accepted"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, Exact, Effect, Error));
	TestTrue(TEXT("fresh effect fingerprint canonical"),
		FHyperAIStudioMaterialsContracts::IsCanonicalSha256(Effect));

	auto WrongValue = Exact;
	WrongValue.Record.Nodes[0].Scalar += 1.0;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongValue);
	TestFalse(TEXT("same-count wrong value cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongValue, Effect, Error));
	auto WrongParameterGuid = Exact;
	WrongParameterGuid.Record.Nodes[1].ParameterGuid = FGuid::NewGuid().ToString(
		EGuidFormats::DigitsWithHyphensLower);
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongParameterGuid);
	TestFalse(TEXT("same-count wrong parameter GUID cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongParameterGuid, Effect, Error));
	auto WrongFallback = Exact;
	WrongFallback.Record.Nodes[2].ConstA += 1.0;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongFallback);
	TestFalse(TEXT("same-count wrong Add fallback cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongFallback, Effect, Error));
	auto WrongNodeInputMask = Exact;
	WrongNodeInputMask.Record.Nodes[2].InputPorts[0].MaskR ^= 1;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongNodeInputMask);
	TestFalse(TEXT("same-count wrong persisted node input mask cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongNodeInputMask, Effect, Error));
	auto WrongBaseFlag = Exact;
	WrongBaseFlag.Record.Nodes[0].bCollapsed = !WrongBaseFlag.Record.Nodes[0].bCollapsed;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongBaseFlag);
	TestFalse(TEXT("same-count wrong persisted base flag cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongBaseFlag, Effect, Error));
	auto WrongOwner = Exact;
	WrongOwner.Record.Nodes[0].MaterialOwnerPath = TEXT("/Game/Other/Other.Other");
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongOwner);
	TestFalse(TEXT("same-count wrong expression owner cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongOwner, Effect, Error));
	auto WrongMenuCategory = Exact;
	WrongMenuCategory.Record.Nodes[0].MenuCategories.Add(TEXT("Unexpected"));
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongMenuCategory);
	TestFalse(TEXT("same-count wrong persisted menu category cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongMenuCategory, Effect, Error));
	auto WrongEdge = Exact;
	WrongEdge.Record.Edges[0].FromStableId = WrongEdge.Record.Nodes[1].StableId;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongEdge);
	TestFalse(TEXT("same-count wrong edge cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongEdge, Effect, Error));
	auto WrongPin = Exact;
	WrongPin.Record.Edges[0].FromOutputIndex = 1;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongPin);
	TestFalse(TEXT("same-count wrong source pin cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongPin, Effect, Error));
	auto WrongOutput = Exact;
	WrongOutput.Record.Edges[Backend.Edges.Num()].ToInputIndex = MP_Metallic;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongOutput);
	TestFalse(TEXT("same-count wrong material output cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongOutput, Effect, Error));
	auto WrongPropertyConstant = Exact;
	WrongPropertyConstant.Record.PropertyInputs[0].ConstantState += TEXT("changed");
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongPropertyConstant);
	TestFalse(TEXT("same-count wrong material property state cannot certify"),
		FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
			Backend, WrongPropertyConstant, Effect, Error));

	FHyperAIStudioMaterialBackendOperation Repair;
	Repair.Kind = EHyperAIStudioMaterialOperationKind::RepairSemanticGraph;
	Repair.TargetPath = Exact.Record.AssetPath; Repair.TargetFamily = TEXT("material");
	Repair.ExpectedRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("before-repair"));
	Repair.RepairKinds = {TEXT("regenerate_duplicate_guids"),
		TEXT("disconnect_dangling_inputs"), TEXT("disconnect_cycles")};
	TestTrue(TEXT("all requested repair defects explicitly proven absent"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			Repair, Exact, Effect, Error));
	auto DuplicateGuid = Exact;
	DuplicateGuid.Record.Nodes[1].PersistedGuid = DuplicateGuid.Record.Nodes[0].PersistedGuid;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(DuplicateGuid);
	TestFalse(TEXT("requested GUID defect cannot remain"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			Repair, DuplicateGuid, Effect, Error));
	auto Dangling = Exact;
	FHyperAIMaterialEdgeView DanglingEdge;
	DanglingEdge.FromStableId = TEXT("outside");
	DanglingEdge.ToStableId = Dangling.Record.Nodes[0].StableId;
	Dangling.Record.Edges.Add(DanglingEdge); Dangling.Record.EdgeCount = Dangling.Record.Edges.Num();
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Dangling);
	TestFalse(TEXT("requested dangling defect cannot remain"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			Repair, Dangling, Effect, Error));
	auto Cyclic = Exact;
	Cyclic.Record.Edges.Reset();
	FHyperAIMaterialEdgeView Forward; Forward.FromStableId = Cyclic.Record.Nodes[0].StableId;
	Forward.ToStableId = Cyclic.Record.Nodes[1].StableId;
	FHyperAIMaterialEdgeView Back = Forward; Swap(Back.FromStableId, Back.ToStableId);
	Cyclic.Record.Edges = {Forward, Back}; Cyclic.Record.EdgeCount = 2;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Cyclic);
	TestFalse(TEXT("requested cycle defect cannot remain"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			Repair, Cyclic, Effect, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsCompoundContradictionAndStagedSafetyTest,
	"HyperAIStudio.NativeTools.Materials.CompoundContradictionsAndStagedSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsCompoundContradictionAndStagedSafetyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::Tests;
	const FString Name = UniqueName(TEXT("M_Compound"));
	const FString Target = TEXT("/Game/__HyperAIStudioAutomation/") + Name + TEXT(".") + Name;
	FHyperAIMaterialApplyPlanRequest Request;
	Request.Operations.Add(CompoundOperation(Target));
	const FHyperAIMaterialApplyPlanReport DryRun =
		UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(Request);
	TestFalse(TEXT("dry-run evidence cannot claim executable success"), DryRun.bOk);
	TestEqual(TEXT("dry run exact blocker"), DryRun.Status,
		FString(TEXT("bounded_compile_or_runtime_cas_backend_required")));
	TestFalse(TEXT("compile effect is not certified"), DryRun.Effects.bCompileOnce);
	TestFalse(TEXT("save effect is not certified"), DryRun.Effects.bSaveOnce);
	TestFalse(TEXT("validate effect is not certified"), DryRun.Effects.bValidateOnce);
	TestFalse(TEXT("fresh effect is not certified"), DryRun.Effects.bFreshVerifyOnce);
	TestFalse(TEXT("shadow replay is not advertised as executable"),
		DryRun.Effects.bTypedShadowReplayComplete);
	TestFalse(TEXT("transaction effect is not certified"), DryRun.Effects.bTransactionOnce);
	TestTrue(TEXT("pure expected post-projection evidence is sealed"),
		FHyperAIStudioMaterialsContracts::IsCanonicalSha256(
			DryRun.ExpectedPostProjectionFingerprint));
	TestTrue(TEXT("pure Prepare plan evidence remains canonical"),
		FHyperAIStudioMaterialsContracts::IsCanonicalSha256(DryRun.PlanHash));
	TestTrue(TEXT("no executable authorization hash exposed"),
		DryRun.AuthorizationPlanHash.IsEmpty());
	TestTrue(TEXT("no effect fingerprint exposed"), DryRun.EffectFingerprint.IsEmpty());
	TestNull(TEXT("dry run created no UObject"), FSoftObjectPath(Target).ResolveObject());

	FHyperAIMaterialPlanOperation Contradiction = CompoundOperation(
		TEXT("/Game/__HyperAIStudioAutomation/M_Contradiction.M_Contradiction"));
	FHyperAIMaterialEdgeSpec A; A.FromNodeId = TEXT("base"); A.ToNodeId = TEXT("sum"); A.ToInput = TEXT("A");
	FHyperAIMaterialEdgeSpec B = A; B.FromNodeId = TEXT("roughness");
	Contradiction.Edges = {A, B};
	FHyperAIMaterialApplyPlanRequest Bad; Bad.Operations.Add(Contradiction);
	const FHyperAIMaterialApplyPlanReport BadReport =
		UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(Bad);
	TestFalse(TEXT("contradictory same-input writes rejected"), BadReport.bOk);
	TestTrue(TEXT("contradiction issue emitted"), BadReport.Issues.ContainsByPredicate([](const auto& Issue)
	{
		return Issue.Code == TEXT("contradictory_input_assignments");
	}));

	FHyperAIMaterialPlanOperation DuplicateParameter = CompoundOperation(
		TEXT("/Game/__HyperAIStudioAutomation/M_DuplicateParameter.M_DuplicateParameter"));
	FHyperAIMaterialNodeSpec Duplicate;
	Duplicate.Kind = TEXT("scalar_parameter"); Duplicate.NodeId = TEXT("roughness_duplicate");
	Duplicate.Name = TEXT("roughness"); Duplicate.Scalar = 0.75;
	DuplicateParameter.Nodes.Add(Duplicate);
	FHyperAIMaterialApplyPlanRequest DuplicateRequest;
	DuplicateRequest.Operations.Add(DuplicateParameter);
	const FHyperAIMaterialApplyPlanReport DuplicateReport =
		UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(DuplicateRequest);
	TestFalse(TEXT("FName-equivalent duplicate parameter rejected before Prepare"),
		DuplicateReport.bOk);
	TestTrue(TEXT("duplicate parameter issue emitted"),
		DuplicateReport.Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("duplicate_parameter_name");
		}));

	FHyperAIMaterialApplyPlanRequest NonDry = Request;
	NonDry.bDryRun = false;
	NonDry.OperationId = TEXT("materials-automation-operation-001");
	NonDry.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAIMaterialApplyPlanReport Staged =
		UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(NonDry);
	TestFalse(TEXT("callable mutation cannot claim success"), Staged.bOk);
	TestFalse(TEXT("no local staging claim"), Staged.bStaged);
	TestFalse(TEXT("no execution submitted"), Staged.bExecutionSubmitted);
	TestFalse(TEXT("no fallback"), Staged.bFallbackPermitted);
	TestEqual(TEXT("central bounded host required"), Staged.Status,
		FString(TEXT("bounded_compile_or_runtime_cas_backend_required")));
	TestNull(TEXT("non-dry callable still caused no mutation"), FSoftObjectPath(Target).ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsBoundsPaginationCloneTest,
	"HyperAIStudio.NativeTools.Materials.BoundsPaginationAndClone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsBoundsPaginationCloneTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::Tests;
	FString Path;
	UMaterial* Material = CreateMaterialFixture(Path, 3);
	TestNotNull(TEXT("fixture"), Material);
	if (!Material) return false;
	FHyperAIMaterialInspectRequest Inspect;
	Inspect.TargetPath = Path; Inspect.PageSize = 1;
	// The test exercises node pagination, so retain the full bounded semantic
	// envelope instead of allowing the conservative JSON budget to trim the page.
	Inspect.MaxOutputBytes = FHyperAIStudioMaterialsContracts::MaxOutputBytes;
	const FHyperAIMaterialInspectReport First = UHyperAIStudioMaterialsToolset::hyper_material_inspect(Inspect);
	TestTrue(TEXT("bounded exact inspect"), First.bOk);
	TestEqual(TEXT("one node page"), First.Target.Nodes.Num(), 1);
	TestTrue(TEXT("page truncation explicit"), First.bTruncated);
	TestFalse(TEXT("next cursor present"), First.NextCursor.IsEmpty());
	Inspect.Cursor = First.NextCursor;
	const FHyperAIMaterialInspectReport Second = UHyperAIStudioMaterialsToolset::hyper_material_inspect(Inspect);
	TestTrue(TEXT("second page succeeds"), Second.bOk);

	FHyperAIStudioMaterialValueSnapshot Truncated;
	FString Status, Diagnostic;
	TestTrue(TEXT("bounded capture reports rather than scans"),
		FHyperAIStudioMaterialsContracts::CaptureExact(Path, TEXT("material"), TEXT("loaded_only"),
			1, FHyperAIStudioMaterialsContracts::MaxEdges, 100, Truncated, Status, Diagnostic));
	TestFalse(TEXT("truncated graph cannot issue CAS"), Truncated.bComplete);
	const TConstArrayView<TObjectPtr<UMaterialExpression>> FixtureExpressions =
		Material->GetExpressions();
	if (FixtureExpressions.Num() > 0 && FixtureExpressions[0])
	{
		UMaterialExpression* FirstExpression = FixtureExpressions[0];
		FirstExpression->Material = nullptr;
		FHyperAIStudioMaterialValueSnapshot WrongOwnerCapture;
		TestTrue(TEXT("wrong-owner capture returns explicit evidence"),
			FHyperAIStudioMaterialsContracts::CaptureExact(Path, TEXT("material"),
				TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes,
				FHyperAIStudioMaterialsContracts::MaxEdges, 100,
				WrongOwnerCapture, Status, Diagnostic));
		TestFalse(TEXT("wrong-owner expression cannot issue complete CAS"),
			WrongOwnerCapture.bComplete);
		TestTrue(TEXT("wrong-owner projection is marked incomplete"),
			WrongOwnerCapture.Record.Nodes.Num() > 0
				&& !WrongOwnerCapture.Record.Nodes[0].bSemanticProjectionComplete);
		FirstExpression->Material = Material;
	}
	UMaterialExpressionConstant* ExternalSource =
		NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
	UMaterialExpressionAdd* DanglingTarget =
		NewObject<UMaterialExpressionAdd>(Material, NAME_None, RF_Transactional);
	ExternalSource->Material = Material;
	ExternalSource->UpdateMaterialExpressionGuid(true, false);
	DanglingTarget->Material = Material;
	DanglingTarget->UpdateMaterialExpressionGuid(true, false);
	DanglingTarget->A.Expression = ExternalSource;
	Material->GetExpressionCollection().AddExpression(DanglingTarget);
	FHyperAIStudioMaterialValueSnapshot DanglingCapture;
	TestTrue(TEXT("bounded external input identity captures without traversing fanout"),
		FHyperAIStudioMaterialsContracts::CaptureExact(Path, TEXT("material"),
			TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes,
			FHyperAIStudioMaterialsContracts::MaxEdges, 100,
			DanglingCapture, Status, Diagnostic));
	TestTrue(TEXT("bounded dangling edge retains complete repair CAS"), DanglingCapture.bComplete);
	// -nullrhi deliberately leaves transient test materials without a render shader map,
	// which UE reports through the same flag as a compile error. This pure graph test
	// supplies closed clean compile evidence explicitly, then reseals the value snapshot.
	DanglingCapture.Record.bCompileStateKnown = true;
	DanglingCapture.Record.bCompiling = false;
	DanglingCapture.Record.bCompileError = false;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(DanglingCapture);
	FHyperAIMaterialApplyPlanRequest RepairRequest;
	FHyperAIMaterialPlanOperation Repair;
	Repair.Type = TEXT("repair_semantic_graph");
	Repair.TargetPath = Path;
	Repair.TargetFamily = TEXT("material");
	Repair.ExpectedRevision = DanglingCapture.Revision;
	Repair.RepairKinds = {TEXT("disconnect_dangling_inputs")};
	RepairRequest.Operations.Add(Repair);
	FHyperAIStudioMaterialBackendOperation RepairBackend;
	FString RepairCode, RepairError;
	TestTrue(TEXT("repair operation normalizes"),
		FHyperAIStudioMaterialsContracts::ValidateOperationShape(
			Repair, RepairBackend, RepairCode, RepairError));
	FHyperAIStudioMaterialValueSnapshot PureRepairShadow;
	TestTrue(TEXT("bounded pure planner builds exact dangling-input post-projection"),
		FHyperAIStudioMaterialsContracts::BuildExpectedRepairSemanticGraph(
			DanglingCapture, RepairBackend, 100, PureRepairShadow, RepairError));
	const FHyperAIMaterialNodeView* PureShadowAdd =
		PureRepairShadow.Record.Nodes.FindByPredicate([](const auto& Node)
		{
			return Node.Kind == TEXT("add");
		});
	TestNotNull(TEXT("pure repair shadow retains exact target node"), PureShadowAdd);
	if (PureShadowAdd && !PureShadowAdd->InputPorts.IsEmpty())
		TestFalse(TEXT("pure repair shadow clears persisted target-input state"),
			PureShadowAdd->InputPorts[0].bConnected);
	TestTrue(TEXT("pure repair shadow seals a fresh exact revision"),
		FHyperAIStudioMaterialsContracts::IsCanonicalSha256(PureRepairShadow.Revision)
			&& PureRepairShadow.Revision != DanglingCapture.Revision);
	FString RepairEffect;
	TestTrue(TEXT("pure repair postcondition binds exact repaired projection"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			RepairBackend, PureRepairShadow, RepairEffect, RepairError));
	const FHyperAIMaterialApplyPlanReport RepairDryRun =
		UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(RepairRequest);
	TestFalse(TEXT("unsaved DoesNotExist repair target fails closed"), RepairDryRun.bOk);
	TestTrue(TEXT("exact AR Exists requirement is reported"),
		RepairDryRun.Issues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("mutation_target_not_persisted");
		}));
	DanglingTarget->A.Expression = nullptr;
	DanglingTarget->A.OutputIndex = 0;
	DanglingTarget->A.Mask = DanglingTarget->A.MaskR = DanglingTarget->A.MaskG =
		DanglingTarget->A.MaskB = DanglingTarget->A.MaskA = 0;
	FHyperAIStudioMaterialValueSnapshot RepairedCapture;
	TestTrue(TEXT("fresh repaired state captures"),
		FHyperAIStudioMaterialsContracts::CaptureExact(Path, TEXT("material"),
			TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes,
			FHyperAIStudioMaterialsContracts::MaxEdges, 100,
			RepairedCapture, Status, Diagnostic));
	RepairedCapture.Record.bCompileStateKnown = true;
	RepairedCapture.Record.bCompiling = false;
	RepairedCapture.Record.bCompileError = false;
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(RepairedCapture);
	TestTrue(TEXT("fresh repair postcondition proves requested dangling defect absent"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			RepairBackend, RepairedCapture, RepairEffect, RepairError));
	auto WrongDisconnectedProjection = RepairedCapture;
	if (FHyperAIMaterialNodeView* AddNode =
		WrongDisconnectedProjection.Record.Nodes.FindByPredicate([](const auto& Node)
		{
			return Node.Kind == TEXT("add");
		}))
	{
		if (!AddNode->InputPorts.IsEmpty()) AddNode->InputPorts[0].bConnected = true;
	}
	FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(WrongDisconnectedProjection);
	TestFalse(TEXT("edge-count-correct but still-connected persisted input cannot certify repair"),
		FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
			RepairBackend, WrongDisconnectedProjection, RepairEffect, RepairError));
	TestEqual(TEXT("dangling repair requires exact persisted input shadow"), RepairError,
		FString(TEXT("material_repair_post_projection_invalid")));
	TestFalse(TEXT("high-density expression collection rejected before traversal"),
		FHyperAIStudioMaterialsContracts::IsExpressionCollectionWithinBound(
			FHyperAIStudioMaterialsContracts::MaxNodes + 1,
			FHyperAIStudioMaterialsContracts::MaxNodes));
	FString DensePath;
	UMaterial* Dense = CreateMaterialFixture(
		DensePath, FHyperAIStudioMaterialsContracts::MaxNodes + 1);
	TestNotNull(TEXT("high-density fixture"), Dense);
	if (Dense)
	{
		FHyperAIStudioMaterialValueSnapshot DenseCapture;
		TestTrue(TEXT("high-density capture returns explicit incomplete evidence"),
			FHyperAIStudioMaterialsContracts::CaptureExact(DensePath, TEXT("material"),
				TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes,
				FHyperAIStudioMaterialsContracts::MaxEdges, 500,
				DenseCapture, Status, Diagnostic));
		TestFalse(TEXT("high-density graph cannot issue complete CAS"), DenseCapture.bComplete);
		TestTrue(TEXT("high-density output never exceeds node work cap"),
			DenseCapture.Record.Nodes.Num() <= FHyperAIStudioMaterialsContracts::MaxNodes);
		Discard(Dense);
	}

	FHyperAIStudioMaterialTypedPayload Payload;
	FHyperAIStudioMaterialBackendOperation Backend;
	FString Code, Error;
	const FHyperAIMaterialPlanOperation Operation = CompoundOperation(
		TEXT("/Game/__HyperAIStudioAutomation/M_Clone.M_Clone"));
	TestTrue(TEXT("operation normalizes"), FHyperAIStudioMaterialsContracts::ValidateOperationShape(
		Operation, Backend, Code, Error));
	Payload.Operations.Add(Backend);
	Payload.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("base"));
	Payload.SemanticFingerprint = FHyperAIStudioMaterialsContracts::ComputePayloadSemanticFingerprint(
		Payload.Operations, Payload.BaseRevision);
	const auto CloneBase = Payload.CloneImmutable();
	const auto& Clone = static_cast<const FHyperAIStudioMaterialTypedPayload&>(CloneBase.Get());
	Payload.Operations[0].Nodes[0].Scalar = 99.0;
	TestNotEqual(TEXT("clone deeply owns nested node array"),
		Clone.Operations[0].Nodes[0].Scalar, Payload.Operations[0].Nodes[0].Scalar);

	FHyperAIStudioMaterialValueSnapshot Oversized;
	Oversized.bComplete = true;
	Oversized.Record.bRevisionComplete = true;
	Oversized.Record.AssetPath = FString::ChrN(
		static_cast<int32>(FHyperAIStudioExtensionRuntime::MaxHashInputBytes) + 1,
		TEXT('x'));
	TestTrue(TEXT("oversized canonical element cannot produce a revision"),
		FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Oversized).IsEmpty());
	TestFalse(TEXT("oversized canonical element clears completeness"), Oversized.bComplete);

	FHyperAIStudioMaterialValueSnapshot Expired;
	Expired.bComplete = true;
	Expired.Record.bRevisionComplete = true;
	TestTrue(TEXT("expired canonical seal cannot produce a revision"),
		FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Expired, 0.0).IsEmpty());
	TestFalse(TEXT("expired canonical seal clears completeness"), Expired.bComplete);
	Discard(Material);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIMaterialsAdapterZeroEffectTest,
	"HyperAIStudio.NativeTools.Materials.AdapterHardZeroEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIMaterialsAdapterZeroEffectTest::RunTest(const FString& Parameters)
{
	using Action = EHyperAIStudioDomainExecutionActionKind;
	FString Path;
	UMaterial* Material = HyperAIStudio::Materials::Tests::CreateMaterialFixture(Path, 1);
	TestNotNull(TEXT("zero-effect fixture"), Material);
	if (!Material) return false;
	const bool bDirtyBefore = Material->GetOutermost()->IsDirty();
	const FGuid StateBefore = Material->StateId;
	const int32 NodeCountBefore = Material->GetExpressions().Num();
	FHyperAIStudioMaterialTypedPayload Payload;
	FHyperAIStudioDomainDispatchContext Context;
	FHyperAIStudioMaterialsDomainAdapter Adapter;
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
	TestEqual(TEXT("package dirty state unchanged"), Material->GetOutermost()->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("material state id unchanged"), Material->StateId, StateBefore);
	TestEqual(TEXT("expression collection unchanged"), Material->GetExpressions().Num(), NodeCountBefore);
	HyperAIStudio::Materials::Tests::Discard(Material);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
