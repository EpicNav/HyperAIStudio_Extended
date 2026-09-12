// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioBlueprintWorkflowToolset.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::BlueprintWorkflow::Tests
{
	FGuid Guid(const uint32 Value)
	{
		return FGuid(Value, Value + 1, Value + 2, Value + 3);
	}

	FString GuidText(const FGuid& Value)
	{
		return Value.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	FHyperAIBlueprintGraphId GraphId()
	{
		return { Guid(10), TEXT("/Game/Test/BP_Workflow.BP_Workflow:EventGraph") };
	}

	FHyperAIBlueprintGraphRef GraphRef()
	{
		FHyperAIBlueprintGraphRef Result;
		Result.GraphGuid = GuidText(GraphId().GraphGuid);
		Result.GraphPath = GraphId().GraphPath;
		return Result;
	}

	FHyperAIBlueprintNodeRef NodeRef(const uint32 Value)
	{
		FHyperAIBlueprintNodeRef Result;
		Result.Graph = GraphRef();
		Result.NodeGuid = GuidText(Guid(Value));
		return Result;
	}

	FHyperAIBlueprintPinRef PinRef(
		const uint32 NodeValue,
		const TCHAR* Name,
		const TCHAR* Direction,
		const int32 Index)
	{
		FHyperAIBlueprintPinRef Pin;
		Pin.Node = NodeRef(NodeValue);
		Pin.PinName = Name;
		Pin.Direction = Direction;
		Pin.PinIndex = Index;
		return Pin;
	}

	FHyperAIBlueprintPatchSnapshot Snapshot()
	{
		FHyperAIBlueprintPatchSnapshot Result;
		Result.bComplete = true;
		Result.Status = TEXT("complete_loaded_revision");
		Result.BlueprintAssetPath = TEXT("/Game/Test/BP_Workflow.BP_Workflow");
		Result.CompileStatus = TEXT("up_to_date");
		Result.GeneratedClassPath = TEXT("/Game/Test/BP_Workflow.BP_Workflow_C");
		Result.Revision = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
		FHyperAIBlueprintPatchGraphSnapshot& Graph = Result.Graphs.AddDefaulted_GetRef();
		Graph.Id = GraphId();
		Graph.SchemaClassPath = TEXT("/Script/BlueprintGraph.EdGraphSchema_K2");
		FHyperAIBlueprintPatchNodeSnapshot& A = Graph.Nodes.AddDefaulted_GetRef();
		A.Id = { Graph.Id, Guid(100) };
		A.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
		A.X = 100;
		A.Y = 200;
		A.bCanUserDelete = true;
		FHyperAIBlueprintPatchPinSnapshot& Pin = A.Pins.AddDefaulted_GetRef();
		Pin.Id.Node = A.Id;
		Pin.Id.PinName = TEXT("Value");
		Pin.Id.Direction = EHyperAIBlueprintPinDirection::Input;
		Pin.Id.PinIndex = 0;
		Pin.PinType = TEXT("int//0/0/0");
		Pin.DefaultValue = TEXT("7");
		FHyperAIBlueprintPatchNodeSnapshot& B = Graph.Nodes.AddDefaulted_GetRef();
		B.Id = { Graph.Id, Guid(200) };
		B.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
		B.X = 500;
		B.Y = 200;
		B.bCanUserDelete = true;
		Result.GraphCount = 1;
		Result.NodeCount = 2;
		Result.PinCount = 1;
		return Result;
	}

	FHyperAIBlueprintPatchInput MoveInput()
	{
		const FHyperAIBlueprintPatchSnapshot Base = Snapshot();
		FHyperAIBlueprintPatchInput Patch;
		Patch.AssetPath = Base.BlueprintAssetPath;
		Patch.ExpectedRevision = Base.Revision;
		FHyperAIBlueprintPatchOperationInput& Move = Patch.Operations.AddDefaulted_GetRef();
		Move.Type = TEXT("move_node");
		Move.Node = NodeRef(100);
		Move.X = 300;
		Move.Y = 400;
		return Patch;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintWorkflowReflectionAdmissionTest,
	"HyperAIStudio.BlueprintWorkflow.ReflectionAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintWorkflowReflectionAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Tests;
	const TArray<FString> Expected = {
		TEXT("hyper_blueprint_snapshot"), TEXT("hyper_blueprint_discover_actions"),
		TEXT("hyper_blueprint_validate_patch"), TEXT("hyper_blueprint_apply_patch"),
		TEXT("hyper_blueprint_build_batch"), TEXT("hyper_blueprint_compile_validate"),
		TEXT("hyper_blueprint_repair"), TEXT("hyper_blueprint_diff"),
		TEXT("hyper_blueprint_layout")
	};
	const TArray<FHyperAIStudioBlueprintWorkflowManifestEntry>& Manifest =
		FHyperAIStudioBlueprintWorkflowContracts::GetManifest();
	TestEqual(TEXT("Exactly nine Blueprint callables are compiled"), Manifest.Num(), 9);
	TSet<FString> Names;
	for (const FHyperAIStudioBlueprintWorkflowManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		TestEqual(TEXT("Every callable maps to the one atomic toolset"), Entry.QualifiedToolset,
			FString(TEXT("HyperAIStudio.HyperAIStudioBlueprintWorkflowToolset")));
		TestEqual(TEXT("Compiled ceiling remains SourceCandidate"), Entry.CompiledCeiling,
			FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate);
	}
	for (const FString& Name : Expected)
	{
		TestTrue(FString::Printf(TEXT("Manifest contains %s"), *Name), Names.Contains(Name));
		const UFunction* Function = UHyperAIStudioBlueprintWorkflowToolset::StaticClass()->FindFunctionByName(FName(*Name));
		TestNotNull(FString::Printf(TEXT("UHT exports exact function %s"), *Name), Function);
		if (Function)
		{
			TestTrue(FString::Printf(TEXT("%s is AICallable"), *Name), Function->HasMetaData(TEXT("AICallable")));
		}
	}
	TestEqual(TEXT("Workflow issue has a Blueprint-specific UHT name"),
		FHyperAIBlueprintWorkflowIssue::StaticStruct()->GetName(), FString(TEXT("HyperAIBlueprintWorkflowIssue")));
	TestNull(TEXT("Closed operation contract has no raw script field"),
		FHyperAIBlueprintPatchOperationInput::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("Closed operation contract has no reflected class path"),
		FHyperAIBlueprintPatchOperationInput::StaticStruct()->FindPropertyByName(TEXT("ClassPath")));

	TArray<FString> CatalogErrors;
	TestTrue(TEXT("Generated catalog is the sole valid admission authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors));
	TOptional<EHyperAIStudioCapabilityAdmissionState> State;
	FString Cohort;
	for (const FString& Name : Expected)
	{
		const FHyperAIStudioCapabilityToolDefinition* Tool =
			FHyperAIStudioCapabilityPackRegistry::GetCatalog().Tools.FindByPredicate(
				[&Name](const FHyperAIStudioCapabilityToolDefinition& Candidate) { return Candidate.Name == Name; });
		TestNotNull(FString::Printf(TEXT("Generated catalog contains %s"), *Name), Tool);
		if (Tool)
		{
			TestEqual(TEXT("Exact generated pack id"), Tool->PackId, FString(TEXT("blueprint")));
			if (!State.IsSet()) { State = Tool->AdmissionState; Cohort = Tool->AtomicCohortId; }
			TestEqual(TEXT("All nine share one generated admission state"), Tool->AdmissionState, State.GetValue());
			TestEqual(TEXT("All nine share one generated atomic cohort"), Tool->AtomicCohortId, Cohort);
		}
	}
	TestFalse(TEXT("Generated Planned/SourceCandidate evidence never registers in production"),
		State.IsSet() && State.GetValue() != EHyperAIStudioCapabilityAdmissionState::Admitted
			? FHyperAIStudioBlueprintWorkflowContracts::IsRegistrationAllowed(false) : false);
#if WITH_DEV_AUTOMATION_TESTS
	if (State.IsSet() && State.GetValue() == EHyperAIStudioCapabilityAdmissionState::SourceCandidate)
	{
		TestTrue(TEXT("Explicit dev admission enables the exact generated SourceCandidate cohort"),
			FHyperAIStudioBlueprintWorkflowContracts::IsRegistrationAllowed(true));
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintWorkflowClosedPatchTest,
	"HyperAIStudio.BlueprintWorkflow.ClosedPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintWorkflowClosedPatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Tests;
	FHyperAIBlueprintPatch Converted;
	FString Error;
	FHyperAIBlueprintPatchInput Valid = MoveInput();
	TestTrue(TEXT("Closed move DTO converts"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Valid, Converted, Error));
	TestEqual(TEXT("Exact revision crosses the DTO boundary"), Converted.ExpectedRevision, Valid.ExpectedRevision);
	FHyperAIBlueprintPatchInput Tampered = Valid;
	Tampered.Operations[0].NodeKind = TEXT("branch");
	TestFalse(TEXT("Unused union-field smuggling is rejected"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Tampered, Converted, Error));
	FHyperAIBlueprintPatchInput Raw = Valid;
	Raw.Operations[0].Type = TEXT("execute_script");
	TestFalse(TEXT("Raw execution operation is absent"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Raw, Converted, Error));
	FHyperAIBlueprintPatchInput Stale = Valid;
	Stale.ExpectedRevision = TEXT("not-a-revision");
	TestFalse(TEXT("Malformed revision CAS is rejected at admission"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Stale, Converted, Error));
	FHyperAIBlueprintPatchInput Overflow = Valid;
	while (Overflow.Operations.Num() <= FHyperAIStudioBlueprintPatch::MaxOperations)
	{
		Overflow.Operations.Add(Valid.Operations[0]);
	}
	TestFalse(TEXT("Operation work is bounded"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Overflow, Converted, Error));

	FHyperAIBlueprintPatchInput Delete = Valid;
	Delete.Operations.Reset();
	FHyperAIBlueprintPatchOperationInput& DeleteOp = Delete.Operations.AddDefaulted_GetRef();
	DeleteOp.Type = TEXT("delete_node");
	DeleteOp.Node = NodeRef(200);
	DeleteOp.DeleteMode = TEXT("orphan_only");
	TestTrue(TEXT("Orphan-only delete has a closed representation"),
		FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(Delete, Converted, Error));
	const FHyperAIBlueprintPatchPlan DeletePlan =
		FHyperAIStudioBlueprintPatch::PlanSnapshot(Snapshot(), Converted);
	TestTrue(TEXT("Delete patch plans"), DeletePlan.bValid);
	TestEqual(TEXT("Delete cannot alias edit safety"), DeletePlan.Safety,
		EHyperAIBlueprintPatchSafety::Destructive);
	FString SafetyError;
	TestFalse(TEXT("Delete is rejected by blueprint.apply_patch typed route"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(
			DeletePlan, EHyperAIBlueprintPatchSafety::Edit, SafetyError));
	TestTrue(TEXT("Delete is accepted only by its destructive typed route"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(
			DeletePlan, EHyperAIBlueprintPatchSafety::Destructive, SafetyError));

	FHyperAIBlueprintSnapshotRequest Missing;
	Missing.AssetPath = TEXT("/Game/DefinitelyMissing/BP_NotLoaded.BP_NotLoaded");
	const FHyperAIBlueprintSnapshotReport MissingReport =
		UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_snapshot(Missing);
	TestFalse(TEXT("Snapshot never loads a missing asset"), MissingReport.bOk);
	TestEqual(TEXT("Loaded-only failure is explicit"), MissingReport.Status, FString(TEXT("asset_not_loaded")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintWorkflowCreatePlanTest,
	"HyperAIStudio.BlueprintWorkflow.CreateConnectDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintWorkflowCreatePlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Tests;
	const FHyperAIBlueprintPatchSnapshot Base = Snapshot();
	FHyperAIBlueprintPatch Patch;
	Patch.TargetAssetPath = Base.BlueprintAssetPath;
	Patch.ExpectedRevision = Base.Revision;
	FHyperAIBlueprintCreateNode Sequence;
	Sequence.Graph = GraphId();
	Sequence.NewNodeGuid = Guid(400);
	Sequence.Kind = EHyperAIBlueprintCreateNodeKind::Sequence;
	Sequence.X = 0;
	Sequence.Y = 0;
	FHyperAIBlueprintCreateNode Branch;
	Branch.Graph = GraphId();
	Branch.NewNodeGuid = Guid(500);
	Branch.Kind = EHyperAIBlueprintCreateNodeKind::Branch;
	Branch.X = 400;
	Branch.Y = 0;
	FHyperAIBlueprintPatchOperation CreateSequence;
	CreateSequence.Value.Set<FHyperAIBlueprintCreateNode>(Sequence);
	Patch.Operations.Add(MoveTemp(CreateSequence));
	FHyperAIBlueprintPatchOperation CreateBranch;
	CreateBranch.Value.Set<FHyperAIBlueprintCreateNode>(Branch);
	Patch.Operations.Add(MoveTemp(CreateBranch));
	FHyperAIBlueprintPatchNodeSnapshot SequenceTemplate;
	FHyperAIBlueprintPatchNodeSnapshot BranchTemplate;
	FString Error;
	TestTrue(TEXT("Sequence is a native allowlisted template"),
		FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(Sequence, SequenceTemplate, Error));
	TestTrue(TEXT("Branch is a native allowlisted template"),
		FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(Branch, BranchTemplate, Error));
	TestEqual(TEXT("Sequence class is exact"), SequenceTemplate.NodeClassPath,
		FString(TEXT("/Script/BlueprintGraph.K2Node_ExecutionSequence")));
	TestEqual(TEXT("Branch class is exact"), BranchTemplate.NodeClassPath,
		FString(TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));
	FHyperAIBlueprintPatchOperation Connect;
	Connect.Value.Set<FHyperAIBlueprintConnectPins>({ SequenceTemplate.Pins[1].Id, BranchTemplate.Pins[0].Id });
	Patch.Operations.Add(MoveTemp(Connect));
	FHyperAIBlueprintPatchOperation Default;
	Default.Value.Set<FHyperAIBlueprintSetLiteralDefault>({ BranchTemplate.Pins[1].Id, TEXT("false") });
	Patch.Operations.Add(MoveTemp(Default));
	const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Base, Patch);
	TestTrue(TEXT("Create-connect-default batch plans in one closed patch"), Plan.bValid);
	TestEqual(TEXT("Two node creates counted"), Plan.Effects.NodesCreated, 2);
	TestEqual(TEXT("Created exec link counted"), Plan.Effects.LinksAdded, 1);
	TestEqual(TEXT("Created Branch literal counted"), Plan.Effects.DefaultsChanged, 1);
	TestTrue(TEXT("Batch compiles once"), Plan.Effects.bRequiresCompile);
	TestTrue(TEXT("Batch saves once through executor"), Plan.Effects.bRequiresCallerSave);
	TestTrue(TEXT("Semantic effect hash is strong and deterministic"),
		FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(Plan).StartsWith(TEXT("sha256:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintWorkflowDiffRepairTest,
	"HyperAIStudio.BlueprintWorkflow.DiffRepairDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintWorkflowDiffRepairTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Tests;
	const FHyperAIBlueprintPatchSnapshot Base = Snapshot();
	FHyperAIBlueprintPatchSnapshot Current = Base;
	Current.Graphs[0].Nodes[0].X = 900;
	Current.Graphs[0].Nodes[0].Pins[0].DefaultValue = TEXT("9");
	Current.Revision = TEXT("sha256:") + FString::ChrN(64, TEXT('b'));
	const FHyperAIBlueprintDiffReport First =
		FHyperAIStudioBlueprintWorkflowContracts::DiffSnapshots(Base, Current, 32);
	const FHyperAIBlueprintDiffReport Second =
		FHyperAIStudioBlueprintWorkflowContracts::DiffSnapshots(Base, Current, 32);
	TestTrue(TEXT("Stable diff succeeds"), First.bOk);
	TestEqual(TEXT("Stable diff cardinality is deterministic"), First.Changes.Num(), Second.Changes.Num());
	for (int32 Index = 0; Index < First.Changes.Num(); ++Index)
	{
		TestEqual(TEXT("Diff stable id order"), First.Changes[Index].StableId, Second.Changes[Index].StableId);
		TestEqual(TEXT("Diff field order"), First.Changes[Index].Field, Second.Changes[Index].Field);
	}
	const FHyperAIBlueprintDiffReport Truncated =
		FHyperAIStudioBlueprintWorkflowContracts::DiffSnapshots(Base, Current, 1);
	TestTrue(TEXT("Diff output bound is disclosed"), Truncated.bTruncated);

	FHyperAIBlueprintPatchSnapshot Duplicate = Base;
	Duplicate.Graphs[0].Nodes[1].X = Duplicate.Graphs[0].Nodes[0].X;
	Duplicate.Graphs[0].Nodes[1].Y = Duplicate.Graphs[0].Nodes[0].Y;
	TArray<FHyperAIBlueprintWorkflowIssue> FindingsA;
	TArray<FHyperAIBlueprintWorkflowIssue> FindingsB;
	const FHyperAIBlueprintPatch RepairA =
		FHyperAIStudioBlueprintWorkflowContracts::BuildDeterministicRepairPatch(Duplicate, FindingsA);
	const FHyperAIBlueprintPatch RepairB =
		FHyperAIStudioBlueprintWorkflowContracts::BuildDeterministicRepairPatch(Duplicate, FindingsB);
	TestEqual(TEXT("Only one safe full-graph layout is proposed"), RepairA.Operations.Num(), 1);
	TestEqual(TEXT("Repair generation is deterministic"), RepairA.Operations.Num(), RepairB.Operations.Num());
	const FHyperAIBlueprintWorkflowIssue* Safe = FindingsA.FindByPredicate(
		[](const FHyperAIBlueprintWorkflowIssue& Finding)
		{
			return Finding.Code == TEXT("duplicate_position_safe_layout");
		});
	TestNotNull(TEXT("Safe repair has explicit evidence"), Safe);
	if (Safe)
	{
		TestEqual(TEXT("Only confidence-1 repair may stage"), Safe->Confidence, 1.0);
		TestFalse(TEXT("Confidence is evidence-backed"), Safe->Evidence.IsEmpty());
	}
	FHyperAIBlueprintPatchSnapshot Broken = Base;
	Broken.CompileStatus = TEXT("error");
	Broken.Graphs[0].Nodes[0].Pins[0].bOrphaned = true;
	Broken.Graphs[0].Nodes[0].NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_IfThenElse");
	FHyperAIBlueprintPatchPinSnapshot& MissingReciprocal =
		Broken.Graphs[0].Nodes[1].Pins.AddDefaulted_GetRef();
	MissingReciprocal.Id.Node = Broken.Graphs[0].Nodes[1].Id;
	MissingReciprocal.Id.PinName = TEXT("RequiredInput");
	MissingReciprocal.Id.Direction = EHyperAIBlueprintPinDirection::Input;
	MissingReciprocal.Id.PinIndex = 0;
	MissingReciprocal.PinType = TEXT("object//0/0/0");
	Broken.Graphs[0].Nodes[0].Pins[0].LinkedPinKeys.Add(MissingReciprocal.Id.StableKey());
	Broken.Graphs[0].Nodes[1].NodeClassPath = TEXT("/Script/Test.REINST_BrokenNode_C_1");
	TArray<FHyperAIBlueprintWorkflowIssue> Suggestions;
	const FHyperAIBlueprintPatch Unsafe =
		FHyperAIStudioBlueprintWorkflowContracts::BuildDeterministicRepairPatch(Broken, Suggestions);
	TestTrue(TEXT("Compile/orphan ambiguity never creates a semantic rewrite"), Unsafe.Operations.IsEmpty());
	TestTrue(TEXT("Ambiguous repair remains below confidence 1"), Suggestions.ContainsByPredicate(
		[](const FHyperAIBlueprintWorkflowIssue& Finding) { return Finding.Confidence < 1.0; }));
	const TArray<FString> ExpectedSuggestionCodes = {
		TEXT("broken_node_reference"), TEXT("missing_expected_pin"), TEXT("orphaned_pin_suggestion"),
		TEXT("nonreciprocal_pin_link"), TEXT("unconnected_required_pin_candidate")
	};
	for (const FString& Code : ExpectedSuggestionCodes)
	{
		TestTrue(FString::Printf(TEXT("Repair diagnoses %s without unsafe mutation"), *Code),
			Suggestions.ContainsByPredicate([&Code](const FHyperAIBlueprintWorkflowIssue& Finding)
			{
				return Finding.Code == Code && Finding.Confidence < 1.0 && !Finding.Evidence.IsEmpty();
			}));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
