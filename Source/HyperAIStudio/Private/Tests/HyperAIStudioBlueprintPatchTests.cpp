// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioBlueprintPatch.h"

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::BlueprintPatch::Tests
{
	FGuid Guid(const uint32 Value)
	{
		return FGuid(Value, Value + 1, Value + 2, Value + 3);
	}

	FHyperAIBlueprintGraphId GraphId()
	{
		return { Guid(10), TEXT("/Game/Test/BP_Patch.BP_Patch:Graph") };
	}

	FHyperAIBlueprintNodeId NodeId(const uint32 Value)
	{
		return { GraphId(), Guid(Value) };
	}

	FHyperAIBlueprintPinId PinId(
		const uint32 NodeValue,
		const TCHAR* Name,
		const EHyperAIBlueprintPinDirection Direction,
		const int32 Index)
	{
		FHyperAIBlueprintPinId Result;
		Result.Node = NodeId(NodeValue);
		Result.PinName = FName(Name);
		Result.Direction = Direction;
		Result.PinIndex = Index;
		return Result;
	}

	FHyperAIBlueprintPatchSnapshot Snapshot()
	{
		FHyperAIBlueprintPatchSnapshot Result;
		Result.bComplete = true;
		Result.Status = TEXT("complete_loaded_revision");
		Result.BlueprintAssetPath = TEXT("/Game/Test/BP_Patch.BP_Patch");
		Result.CompileStatus = TEXT("up_to_date");
		Result.GeneratedClassPath = TEXT("/Game/Test/BP_Patch.BP_Patch_C");
		Result.Revision = TEXT("sha256:") + FString::ChrN(64, TEXT('1'));

		FHyperAIBlueprintPatchGraphSnapshot& Graph = Result.Graphs.AddDefaulted_GetRef();
		Graph.Id = GraphId();
		Graph.SchemaClassPath = TEXT("/Script/BlueprintGraph.EdGraphSchema_K2");

		FHyperAIBlueprintPatchNodeSnapshot& Producer = Graph.Nodes.AddDefaulted_GetRef();
		Producer.Id = NodeId(100);
		Producer.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
		Producer.X = 20;
		Producer.Y = 40;
		Producer.bCanUserDelete = true;
		FHyperAIBlueprintPatchPinSnapshot& Output = Producer.Pins.AddDefaulted_GetRef();
		Output.Id = PinId(100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0);
		Output.PinType = TEXT("int");

		FHyperAIBlueprintPatchNodeSnapshot& Consumer = Graph.Nodes.AddDefaulted_GetRef();
		Consumer.Id = NodeId(200);
		Consumer.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
		Consumer.X = 500;
		Consumer.Y = 40;
		Consumer.bCanUserDelete = true;
		FHyperAIBlueprintPatchPinSnapshot& Input = Consumer.Pins.AddDefaulted_GetRef();
		Input.Id = PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0);
		Input.PinType = TEXT("int");
		Input.DefaultValue = TEXT("7");

		FHyperAIBlueprintPatchNodeSnapshot& Orphan = Graph.Nodes.AddDefaulted_GetRef();
		Orphan.Id = NodeId(300);
		Orphan.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_CallFunction");
		Orphan.X = 900;
		Orphan.Y = 40;
		Orphan.bCanUserDelete = true;

		Result.GraphCount = 1;
		Result.NodeCount = 3;
		Result.PinCount = 2;
		return Result;
	}

	FHyperAIBlueprintPatch PatchFor(const FHyperAIBlueprintPatchSnapshot& Snapshot)
	{
		FHyperAIBlueprintPatch Result;
		Result.TargetAssetPath = Snapshot.BlueprintAssetPath;
		Result.ExpectedRevision = Snapshot.Revision;
		return Result;
	}

	bool HasIssue(const FHyperAIBlueprintPatchPlan& Plan, const FString& Code)
	{
		return Plan.Issues.ContainsByPredicate([&Code](const FHyperAIBlueprintPatchIssue& Issue)
		{
			return Issue.Code == Code;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintPatchIdentityAndLayoutTest,
	"HyperAIStudio.BlueprintPatch.IdentityAndLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintPatchIdentityAndLayoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintPatch::Tests;
	const FHyperAIBlueprintPinId FallbackPin = PinId(
		100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0);
	TestTrue(TEXT("Graph identity is valid"), GraphId().IsValid());
	TestTrue(TEXT("Node identity is valid"), NodeId(100).IsValid());
	TestTrue(TEXT("Fallback pin identity is valid within a revision"), FallbackPin.IsValid());
	TestTrue(TEXT("Fallback is disclosed"), FallbackPin.UsesRevisionBoundFallback());
	TestTrue(TEXT("Fallback key binds name, direction, and index"),
		FallbackPin.StableKey().Contains(TEXT("fallback:Result:out:0")));
	FHyperAIBlueprintPinId InvalidDirection = FallbackPin;
	InvalidDirection.Direction = static_cast<EHyperAIBlueprintPinDirection>(255);
	TestFalse(TEXT("Unknown pin direction enum fails identity validation"), InvalidDirection.IsValid());
	TestTrue(TEXT("Unknown pin direction never aliases input/output identity"),
		InvalidDirection.StableKey().Contains(TEXT("invalid-255")));

	FHyperAIBlueprintLayoutNodes Layout;
	Layout.Graph = GraphId();
	Layout.Nodes = { NodeId(300), NodeId(100), NodeId(200) };
	Layout.OriginX = 100;
	Layout.OriginY = 200;
	Layout.Columns = 2;
	Layout.HorizontalSpacing = 400;
	Layout.VerticalSpacing = 250;
	FString Error;
	const TMap<FString, FIntPoint> First = FHyperAIStudioBlueprintPatch::ComputeDeterministicLayout(Layout, Error);
	TestTrue(TEXT("Valid layout has no error"), Error.IsEmpty());
	TestEqual(TEXT("All layout nodes are represented"), First.Num(), 3);

	Algo::Reverse(Layout.Nodes);
	const TMap<FString, FIntPoint> Second = FHyperAIStudioBlueprintPatch::ComputeDeterministicLayout(Layout, Error);
	TestEqual(TEXT("Input order preserves layout cardinality"), Second.Num(), First.Num());
	for (const TPair<FString, FIntPoint>& Pair : First)
	{
		const FIntPoint* Other = Second.Find(Pair.Key);
		TestTrue(TEXT("Input order preserves every deterministic position"), Other && *Other == Pair.Value);
	}
	TestEqual(TEXT("First stable node is at origin"), First.FindRef(NodeId(100).StableKey()), FIntPoint(100, 200));
	TestEqual(TEXT("Second stable node advances one column"), First.FindRef(NodeId(200).StableKey()), FIntPoint(500, 200));
	TestEqual(TEXT("Third stable node advances one row"), First.FindRef(NodeId(300).StableKey()), FIntPoint(100, 450));

	Layout.Nodes.Add(NodeId(300));
	FHyperAIStudioBlueprintPatch::ComputeDeterministicLayout(Layout, Error);
	TestEqual(TEXT("Duplicate layout nodes fail explicitly"), Error, FString(TEXT("invalid_or_duplicate_layout_node")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintPatchPlannerTableTest,
	"HyperAIStudio.BlueprintPatch.PlannerTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintPatchPlannerTableTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintPatch::Tests;
	const FHyperAIBlueprintPatchSnapshot Base = Snapshot();

	struct FCase
	{
		const TCHAR* Name;
		TFunction<void(FHyperAIBlueprintPatch&, FHyperAIBlueprintPatchSnapshot&)> Arrange;
		bool bExpectedValid;
		const TCHAR* ExpectedIssue;
	};
	const TArray<FCase> Cases = {
		{
			TEXT("move"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintMoveNode>({ NodeId(100), 120, 240 });
			},
			true, TEXT("")
		},
		{
			TEXT("stale revision"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				Patch.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('2'));
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintMoveNode>({ NodeId(100), 120, 240 });
			},
			false, TEXT("revision_precondition_failed")
		},
		{
			TEXT("duplicate position write rejected before mutation"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& First = Patch.Operations.AddDefaulted_GetRef();
				First.Value.Set<FHyperAIBlueprintMoveNode>({ NodeId(100), 120, 240 });
				FHyperAIBlueprintPatchOperation& Second = Patch.Operations.AddDefaulted_GetRef();
				Second.Value.Set<FHyperAIBlueprintMoveNode>({ NodeId(100), 140, 260 });
			},
			false, TEXT("conflicting_position_operations")
		},
		{
			TEXT("connect opposite pins"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintConnectPins>({
					PinId(100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0),
					PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0) });
			},
			true, TEXT("")
		},
		{
			TEXT("same direction rejected"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintConnectPins>({
					PinId(100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0),
					PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Output, 0) });
			},
			false, TEXT("pin_not_found")
		},
		{
			TEXT("unknown pin direction enum rejected"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPinId Invalid = PinId(
					100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0);
				Invalid.Direction = static_cast<EHyperAIBlueprintPinDirection>(255);
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintConnectPins>({Invalid,
					PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0)});
			},
			false, TEXT("invalid_pin_direction")
		},
		{
			TEXT("orphan delete"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintDeleteNode>({ NodeId(300), EHyperAIBlueprintDeleteMode::OrphanOnly });
			},
			true, TEXT("")
		},
		{
			TEXT("unconfirmed linked orphan delete"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot& Snapshot)
			{
				auto& ProducerPin = Snapshot.Graphs[0].Nodes[0].Pins[0];
				auto& ConsumerPin = Snapshot.Graphs[0].Nodes[1].Pins[0];
				ProducerPin.LinkedPinKeys.Add(ConsumerPin.Id.StableKey());
				ConsumerPin.LinkedPinKeys.Add(ProducerPin.Id.StableKey());
				Snapshot.LinkCount = 1;
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintDeleteNode>({ NodeId(100), EHyperAIBlueprintDeleteMode::OrphanOnly });
			},
			false, TEXT("node_not_orphan")
		},
		{
			TEXT("explicit linked delete"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot& Snapshot)
			{
				auto& ProducerPin = Snapshot.Graphs[0].Nodes[0].Pins[0];
				auto& ConsumerPin = Snapshot.Graphs[0].Nodes[1].Pins[0];
				ProducerPin.LinkedPinKeys.Add(ConsumerPin.Id.StableKey());
				ConsumerPin.LinkedPinKeys.Add(ProducerPin.Id.StableKey());
				Snapshot.LinkCount = 1;
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintDeleteNode>({ NodeId(100), EHyperAIBlueprintDeleteMode::ExplicitConfirmed });
			},
			true, TEXT("")
		},
		{
			TEXT("unknown delete mode enum rejected"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot&)
			{
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintDeleteNode>({
					NodeId(300), static_cast<EHyperAIBlueprintDeleteMode>(255)});
			},
			false, TEXT("invalid_delete_mode")
		},
		{
			TEXT("readonly default"),
			[](FHyperAIBlueprintPatch& Patch, FHyperAIBlueprintPatchSnapshot& Snapshot)
			{
				Snapshot.Graphs[0].Nodes[1].Pins[0].bDefaultReadOnly = true;
				FHyperAIBlueprintPatchOperation& Op = Patch.Operations.AddDefaulted_GetRef();
				Op.Value.Set<FHyperAIBlueprintSetLiteralDefault>({
					PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0), TEXT("9") });
			},
			false, TEXT("unsupported_default_target")
		}
	};

	for (const FCase& Case : Cases)
	{
		FHyperAIBlueprintPatchSnapshot CaseSnapshot = Base;
		FHyperAIBlueprintPatch Patch = PatchFor(CaseSnapshot);
		Case.Arrange(Patch, CaseSnapshot);
		const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(CaseSnapshot, Patch);
		TestEqual(FString::Printf(TEXT("%s validity"), Case.Name), Plan.bValid, Case.bExpectedValid);
		if (FCString::Strlen(Case.ExpectedIssue) > 0)
		{
			TestTrue(FString::Printf(TEXT("%s issue"), Case.Name), HasIssue(Plan, Case.ExpectedIssue));
		}
		TestFalse(FString::Printf(TEXT("%s plan hash"), Case.Name), Plan.PlanHash.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintPatchDryRunEffectsTest,
	"HyperAIStudio.BlueprintPatch.DryRunEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintPatchDryRunEffectsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintPatch::Tests;
	FHyperAIBlueprintPatchSnapshot Base = Snapshot();
	FHyperAIBlueprintPatch Patch = PatchFor(Base);

	FHyperAIBlueprintPatchOperation& Move = Patch.Operations.AddDefaulted_GetRef();
	Move.Value.Set<FHyperAIBlueprintMoveNode>({ NodeId(100), 120, 240 });
	FHyperAIBlueprintPatchOperation& Connect = Patch.Operations.AddDefaulted_GetRef();
	Connect.Value.Set<FHyperAIBlueprintConnectPins>({
		PinId(100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0),
		PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0) });
	FHyperAIBlueprintPatchOperation& Delete = Patch.Operations.AddDefaulted_GetRef();
	Delete.Value.Set<FHyperAIBlueprintDeleteNode>({ NodeId(300), EHyperAIBlueprintDeleteMode::OrphanOnly });

	const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Base, Patch);
	TestTrue(TEXT("Combined dry-run plan is valid"), Plan.bValid);
	TestEqual(TEXT("A plan containing node deletion is classified destructive"),
		Plan.Safety, EHyperAIBlueprintPatchSafety::Destructive);
	FString SafetyError;
	TestFalse(TEXT("A delete plan cannot enter blueprint.apply_patch"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(Plan, EHyperAIBlueprintPatchSafety::Edit, SafetyError));
	TestEqual(TEXT("Edit-route delete rejection is explicit"), SafetyError,
		FString(TEXT("delete_operation_requires_destructive_route")));
	TestTrue(TEXT("The same classified plan is accepted by the destructive route"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(Plan, EHyperAIBlueprintPatchSafety::Destructive, SafetyError));
	FHyperAIBlueprintPatch EditOnlyPatch = Patch;
	EditOnlyPatch.Operations.RemoveAt(EditOnlyPatch.Operations.Num() - 1);
	const FHyperAIBlueprintPatchPlan EditOnlyPlan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Base, EditOnlyPatch);
	TestTrue(TEXT("Delete-free patch remains a valid edit plan"), EditOnlyPlan.bValid);
	TestEqual(TEXT("Delete-free patch classification"), EditOnlyPlan.Safety, EHyperAIBlueprintPatchSafety::Edit);
	TestTrue(TEXT("Delete-free patch is admitted only by the edit route"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(
			EditOnlyPlan, EHyperAIBlueprintPatchSafety::Edit, SafetyError));
	TestFalse(TEXT("Destructive route cannot be used to bypass required delete effects"),
		FHyperAIStudioBlueprintPatch::ValidatePlanSafety(
			EditOnlyPlan, EHyperAIBlueprintPatchSafety::Destructive, SafetyError));
	EHyperAIBlueprintPatchSafety TypedSafety = static_cast<EHyperAIBlueprintPatchSafety>(255);
	TestTrue(TEXT("blueprint.apply_patch binds only to edit safety"),
		FHyperAIStudioBlueprintPatch::TrySafetyForTypedOperation(
			FHyperAIStudioBlueprintPatch::EditTypedOperationType, TypedSafety));
	TestEqual(TEXT("Edit typed route classification"), TypedSafety, EHyperAIBlueprintPatchSafety::Edit);
	TestTrue(TEXT("blueprint.delete_patch binds only to destructive safety"),
		FHyperAIStudioBlueprintPatch::TrySafetyForTypedOperation(
			FHyperAIStudioBlueprintPatch::DeleteTypedOperationType, TypedSafety));
	TestEqual(TEXT("Delete typed route classification"), TypedSafety, EHyperAIBlueprintPatchSafety::Destructive);
	TestTrue(TEXT("Plan hash uses canonical sha256"), Plan.PlanHash.StartsWith(TEXT("sha256:")) && Plan.PlanHash.Len() == 71);
	const FHyperAIBlueprintPatchPlan SamePlan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Base, Patch);
	TestEqual(TEXT("Identical canonical input has a deterministic plan hash"), SamePlan.PlanHash, Plan.PlanHash);
	FHyperAIBlueprintPatch ChangedPatch = Patch;
	ChangedPatch.Operations[0].Value.Get<FHyperAIBlueprintMoveNode>().X = 121;
	const FHyperAIBlueprintPatchPlan ChangedPlan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Base, ChangedPatch);
	TestNotEqual(TEXT("A changed operation changes the canonical plan hash"), ChangedPlan.PlanHash, Plan.PlanHash);
	TestEqual(TEXT("Move effect is counted"), Plan.Effects.NodesMoved, 1);
	TestEqual(TEXT("Connect effect is counted"), Plan.Effects.LinksAdded, 1);
	TestEqual(TEXT("Delete effect is counted"), Plan.Effects.NodesDeleted, 1);
	TestTrue(TEXT("Mutation effects require one compile"), Plan.Effects.bRequiresCompile);
	TestTrue(TEXT("Mutation effects require caller save"), Plan.Effects.bRequiresCallerSave);

	const FHyperAIBlueprintPatchResult NullDryRun = FHyperAIStudioBlueprintPatch::DryRunLoaded(
		nullptr, Patch, FHyperAIStudioBlueprintPatch::DeleteTypedOperationType);
	TestFalse(TEXT("Executor never loads a missing Blueprint"), NullDryRun.bOk);
	TestEqual(TEXT("Missing loaded Blueprint is explicit"), NullDryRun.Status, FString(TEXT("blueprint_unavailable")));
	TestTrue(TEXT("Null validation remains a dry-run"), NullDryRun.bDryRun);
	TestFalse(TEXT("Dry-run never claims save"), NullDryRun.bSavePerformed);
	const FHyperAIBlueprintPatchResult UnknownRoute = FHyperAIStudioBlueprintPatch::DryRunLoaded(
		nullptr, Patch, TEXT("blueprint.untrusted_patch"));
	TestEqual(TEXT("Unknown typed operation route fails before touching a Blueprint"),
		UnknownRoute.Status, FString(TEXT("typed_operation_not_supported")));
	FHyperAIBlueprintPatch InvalidDeletePatch = PatchFor(Base);
	FHyperAIBlueprintPatchOperation& InvalidDelete = InvalidDeletePatch.Operations.AddDefaulted_GetRef();
	InvalidDelete.Value.Set<FHyperAIBlueprintDeleteNode>({
		NodeId(300), static_cast<EHyperAIBlueprintDeleteMode>(255)});
	const FHyperAIBlueprintPatchResult InvalidLiveApply = FHyperAIStudioBlueprintPatch::ApplyLoaded(
		nullptr, InvalidDeletePatch, FHyperAIStudioBlueprintPatch::DeleteTypedOperationType);
	TestEqual(TEXT("Live apply independently rejects unknown delete mode before Blueprint access"),
		InvalidLiveApply.Status, FString(TEXT("invalid_operation_enum")));
	TestFalse(TEXT("Invalid live enum never starts mutation"), InvalidLiveApply.bMutationStarted);
	FHyperAIBlueprintPatch InvalidDirectionPatch = PatchFor(Base);
	FHyperAIBlueprintPinId InvalidPin = PinId(
		100, TEXT("Result"), EHyperAIBlueprintPinDirection::Output, 0);
	InvalidPin.Direction = static_cast<EHyperAIBlueprintPinDirection>(255);
	FHyperAIBlueprintPatchOperation& InvalidConnect = InvalidDirectionPatch.Operations.AddDefaulted_GetRef();
	InvalidConnect.Value.Set<FHyperAIBlueprintConnectPins>({InvalidPin,
		PinId(200, TEXT("Value"), EHyperAIBlueprintPinDirection::Input, 0)});
	const FHyperAIBlueprintPatchResult InvalidDirectionApply = FHyperAIStudioBlueprintPatch::ApplyLoaded(
		nullptr, InvalidDirectionPatch, FHyperAIStudioBlueprintPatch::EditTypedOperationType);
	TestEqual(TEXT("Live apply independently rejects unknown pin direction before Blueprint access"),
		InvalidDirectionApply.Status, FString(TEXT("invalid_operation_enum")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
