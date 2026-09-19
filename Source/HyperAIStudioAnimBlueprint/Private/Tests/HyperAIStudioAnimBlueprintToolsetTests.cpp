// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioAnimBlueprintToolset.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HyperAIStudioAnimBlueprintCookbook.h"
#include "HyperAIStudioAnimBlueprintGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::AnimBlueprint::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const Skeleton = TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin.SK_Mannequin");
	const TCHAR* const Idle = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MM_Unarmed_Idle_Ready.MM_Unarmed_Idle_Ready");
	const TCHAR* const Jog = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/BS_MM_Unarmed_Jog_Walk.BS_MM_Unarmed_Jog_Walk");
	const TCHAR* const FallLoop = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MM_Unarmed_Jump_Fall_Loop.MM_Unarmed_Jump_Fall_Loop");
	const TCHAR* const Land = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MM_Unarmed_Jump_Fall_Land.MM_Unarmed_Jump_Fall_Land");

	FHyperAIAnimBlueprintOp Op(const TCHAR* Kind, const TCHAR* Machine, const TCHAR* Name, const TCHAR* To = TEXT(""), const TCHAR* Rule = TEXT(""),
		const TCHAR* Value = TEXT(""), const TCHAR* Asset = TEXT(""), const TCHAR* X = TEXT(""), const TCHAR* Y = TEXT(""))
	{
		FHyperAIAnimBlueprintOp Result;
		Result.Kind = Kind;
		Result.Machine = Machine;
		Result.Name = Name;
		Result.To = To;
		Result.Rule = Rule;
		Result.Value = Value;
		Result.Asset = Asset;
		Result.XVariable = X;
		Result.YVariable = Y;
		return Result;
	}

	/** Idle, jog blend space, fall loop and a one-shot land, with drivers and hysteresis on the jog threshold. */
	TArray<FHyperAIAnimBlueprintOp> Locomotion(const bool bJog2D)
	{
		return {
			Op(TEXT("bind_variable"), TEXT(""), TEXT("Speed"), TEXT(""), TEXT(""), TEXT("speed")),
			Op(TEXT("bind_variable"), TEXT(""), TEXT("IsFalling"), TEXT(""), TEXT(""), TEXT("is_falling")),
			Op(TEXT("add_variable"), TEXT(""), TEXT("Direction"), TEXT(""), TEXT(""), TEXT("float")),
			Op(TEXT("add_state_machine"), TEXT(""), TEXT("Locomotion")),
			Op(TEXT("add_state"), TEXT("Locomotion"), TEXT("Idle"), TEXT(""), TEXT(""), TEXT(""), Idle),
			Op(TEXT("add_state"), TEXT("Locomotion"), TEXT("Jog"), TEXT(""), TEXT(""), TEXT(""), Jog, TEXT("Speed"), bJog2D ? TEXT("Direction") : TEXT("")),
			Op(TEXT("add_state"), TEXT("Locomotion"), TEXT("Fall"), TEXT(""), TEXT(""), TEXT(""), FallLoop),
			Op(TEXT("add_state"), TEXT("Locomotion"), TEXT("Land"), TEXT(""), TEXT(""), TEXT("once"), Land),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Idle"), TEXT("Jog"), TEXT("Speed > 10")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Jog"), TEXT("Idle"), TEXT("Speed < 5"), TEXT("0.25")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Idle"), TEXT("Fall"), TEXT("IsFalling")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Jog"), TEXT("Fall"), TEXT("IsFalling")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Fall"), TEXT("Land"), TEXT("!IsFalling"), TEXT("0.1")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Land"), TEXT("Idle"), TEXT("time_remaining < 0.2")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Land"), TEXT("Jog"), TEXT("Speed > 10 && !IsFalling"))};
	}

	bool AssetsPresent(FAutomationTestBase& Test)
	{
		for (const TCHAR* Path : {Skeleton, Idle, Jog, FallLoop, Land})
		{
			if (!LoadObject<UObject>(nullptr, Path))
			{
				Test.AddInfo(FString::Printf(TEXT("%s is missing; the Lyra mannequin content is needed for this test."), Path));
				return false;
			}
		}
		return true;
	}

	bool Jog2D()
	{
		return !LoadObject<UObject>(nullptr, Jog)->IsA<UBlendSpace1D>();
	}

	FString TestPath()
	{
		const FString Name = TEXT("ABP_HyperAITest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		return FString::Printf(TEXT("/Game/__HyperAIStudioTests/AnimBlueprint/%s.%s"), *Name, *Name);
	}

	const FHyperAIAnimBlueprintState* FindState(const FHyperAIAnimBlueprintMachine& Machine, const TCHAR* Name)
	{
		return Machine.States.FindByPredicate([Name](const FHyperAIAnimBlueprintState& State) { return State.Name == Name; });
	}

	bool HasIssue(const TArray<FHyperAIAnimBlueprintIssue>& Issues, const TCHAR* Code)
	{
		return Issues.ContainsByPredicate([Code](const FHyperAIAnimBlueprintIssue& Issue) { return Issue.Code == Code; });
	}

	bool PumpUntil(TFunctionRef<bool()> Done, const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline) return false;
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FlushRenderingCommands();
		}
		return true;
	}

	void Discard(UObject* Asset)
	{
		if (Asset) ObjectTools::ForceDeleteObjects({Asset}, /*bShowConfirmation=*/false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimBlueprintCohortTest,
	"HyperAIStudio.AnimBlueprint.Contracts.ExactThreeToolCohort",
	HyperAIStudio::AnimBlueprint::Tests::Flags)

bool FHyperAIStudioAnimBlueprintCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioAnimBlueprintContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	TestEqual(TEXT("one variant per tool"), FHyperAIStudioAnimBlueprintContracts::GetAdapterDescriptor().Variants.Num(), Names.Num());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioAnimBlueprintToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());

	TestTrue(TEXT("the cookbook is in the project"), HyperAIStudio::AnimBlueprint::EnsureCookbook());
	FString Cookbook;
	FFileHelper::LoadFileToString(Cookbook, *HyperAIStudio::AnimBlueprint::GetCookbookPath());
	TestTrue(TEXT("and names the tool and the checks it teaches"), Cookbook.Contains(TEXT("hyper_anim_blueprint_apply_plan"))
		&& Cookbook.Contains(TEXT("no_hysteresis")) && Cookbook.Contains(TEXT("time_remaining")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimBlueprintGraphTest,
	"HyperAIStudio.AnimBlueprint.Graph.LocomotionBuildCompileAndChecks",
	HyperAIStudio::AnimBlueprint::Tests::Flags)

bool FHyperAIStudioAnimBlueprintGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::AnimBlueprint::Tests;
	namespace Gate = HyperAIStudio::AnimBlueprint::Gate;
	if (!AssetsPresent(*this)) return true;
	USkeleton* TargetSkeleton = Gate::ResolveSkeleton(Skeleton);
	FString Error;
	UAnimBlueprint* Blueprint = Gate::Create(TestPath(), *TargetSkeleton, *UAnimInstance::StaticClass(), Error);
	if (!TestNotNull(TEXT("an Animation Blueprint is created: ") + Error, Blueprint)) return false;
	ON_SCOPE_EXIT { Discard(Blueprint); };

	auto Rejects = [&](const TArray<FHyperAIAnimBlueprintOp>& Ops, const TCHAR* Expected)
	{
		FString Why;
		const bool bValid = Gate::ValidateOps(Blueprint, TargetSkeleton, Ops, Why);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Why), !bValid && Why.Contains(Expected));
	};
	Rejects({Op(TEXT("add_state"), TEXT("Nowhere"), TEXT("Idle"))}, TEXT("no state machine is named"));
	Rejects({Op(TEXT("bind_variable"), TEXT(""), TEXT("Speed"), TEXT(""), TEXT(""), TEXT("velocity"))}, TEXT("must be a driver"));
	Rejects({Op(TEXT("add_variable"), TEXT(""), TEXT("2Fast"), TEXT(""), TEXT(""), TEXT("float"))}, TEXT("identifier"));
	Rejects({Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")), Op(TEXT("add_state"), TEXT("M"), TEXT("A")), Op(TEXT("add_state"), TEXT("M"), TEXT("B")),
		Op(TEXT("add_transition"), TEXT("M"), TEXT("A"), TEXT("B"), TEXT("Speed > 10"))}, TEXT("must be a float variable"));
	Rejects({Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")), Op(TEXT("add_state"), TEXT("M"), TEXT("A")), Op(TEXT("add_state"), TEXT("M"), TEXT("B")),
		Op(TEXT("add_transition"), TEXT("M"), TEXT("A"), TEXT("B"), TEXT(""))}, TEXT("needs a rule"));
	Rejects({Op(TEXT("add_variable"), TEXT(""), TEXT("Speed"), TEXT(""), TEXT(""), TEXT("float")), Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")),
		Op(TEXT("add_state"), TEXT("M"), TEXT("A"), TEXT(""), TEXT(""), TEXT(""), Jog)}, TEXT("x_variable"));
	Rejects({Op(TEXT("add_variable"), TEXT(""), TEXT("Speed"), TEXT(""), TEXT(""), TEXT("float")), Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")),
		Op(TEXT("add_state"), TEXT("M"), TEXT("A"), TEXT(""), TEXT(""), TEXT(""), Jog, TEXT("Speed"), Jog2D() ? TEXT("Speed") : TEXT("")),
		Op(TEXT("add_state"), TEXT("M"), TEXT("B"), TEXT(""), TEXT(""), TEXT(""), Idle),
		Op(TEXT("add_transition"), TEXT("M"), TEXT("A"), TEXT("B"), TEXT("time_remaining < 0.2"))}, TEXT("play a sequence"));
	Rejects({Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")), Op(TEXT("add_state"), TEXT("M"), TEXT("A"), TEXT(""), TEXT(""), TEXT(""),
		TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin.SK_Mannequin"))}, TEXT("not an animation sequence or blend space"));
	TestTrue(TEXT("the locomotion plan validates: ") + Error, Gate::ValidateOps(Blueprint, TargetSkeleton, Locomotion(Jog2D()), Error));

	{
		const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioAnimBlueprintTests", "Build", "Build"));
		TestTrue(TEXT("the locomotion graph builds: ") + Error, Gate::ApplyOps(*Blueprint, Locomotion(Jog2D()), Error));
	}
	FString FirstError;
	int32 Errors = 0;
	int32 Warnings = 0;
	TestTrue(*FString::Printf(TEXT("and compiles (%d errors, first: %s)"), Errors, *FirstError), Gate::Compile(*Blueprint, FirstError, Errors, Warnings));
	TestEqual(TEXT("status is up to date"), Gate::CompileStatusOf(*Blueprint).StartsWith(TEXT("up_to_date")) || Gate::CompileStatusOf(*Blueprint) == TEXT("warnings"), true);

	TArray<FHyperAIAnimBlueprintVariable> Variables;
	TArray<FHyperAIAnimBlueprintMachine> Machines;
	Gate::Read(*Blueprint, Variables, Machines);
	const FHyperAIAnimBlueprintVariable* Speed = Variables.FindByPredicate([](const FHyperAIAnimBlueprintVariable& V) { return V.Name == TEXT("Speed"); });
	TestTrue(TEXT("Speed is a float driven by speed"), Speed && Speed->Type == TEXT("float") && Speed->Driver == TEXT("speed"));
	const FHyperAIAnimBlueprintVariable* Falling = Variables.FindByPredicate([](const FHyperAIAnimBlueprintVariable& V) { return V.Name == TEXT("IsFalling"); });
	TestTrue(TEXT("IsFalling is a bool driven by is_falling"), Falling && Falling->Type == TEXT("bool") && Falling->Driver == TEXT("is_falling"));
	if (TestEqual(TEXT("one state machine"), Machines.Num(), 1))
	{
		const FHyperAIAnimBlueprintMachine& Machine = Machines[0];
		TestTrue(TEXT("it drives the output pose"), Machine.bDrivesOutput);
		TestEqual(TEXT("four states"), Machine.States.Num(), 4);
		TestEqual(TEXT("seven transitions"), Machine.Transitions.Num(), 7);
		TestTrue(TEXT("Idle is the entry and plays its sequence"), FindState(Machine, TEXT("Idle")) && FindState(Machine, TEXT("Idle"))->bEntry
			&& FindState(Machine, TEXT("Idle"))->Player == TEXT("sequence"));
		TestTrue(TEXT("Jog blends by Speed"), FindState(Machine, TEXT("Jog")) && FindState(Machine, TEXT("Jog"))->Player == TEXT("blend_space")
			&& FindState(Machine, TEXT("Jog"))->XVariable == TEXT("Speed"));
		TestTrue(TEXT("Land plays once"), FindState(Machine, TEXT("Land")) && !FindState(Machine, TEXT("Land"))->bLooping);
		TestTrue(TEXT("rules read back as written"), Machine.Transitions.ContainsByPredicate([](const FHyperAIAnimBlueprintTransition& T)
		{
			return T.From == TEXT("Land") && T.To == TEXT("Jog") && T.Rule == TEXT("Speed > 10 && !IsFalling");
		}));
	}
	TArray<FHyperAIAnimBlueprintIssue> Issues = Gate::Check(*Blueprint);
	TestFalse(TEXT("the built graph has no errors"), Issues.ContainsByPredicate([](const FHyperAIAnimBlueprintIssue& Issue) { return Issue.Severity == TEXT("error"); }));
	TestFalse(TEXT("the jog threshold has hysteresis"), HasIssue(Issues, TEXT("no_hysteresis")));
	TestFalse(TEXT("Land has a timed exit, so its pose does not freeze"), HasIssue(Issues, TEXT("frozen_pose")));
	if (Jog2D())
	{
		TestTrue(TEXT("Direction is read but nothing sets it"), HasIssue(Issues, TEXT("never_set")));
	}

	// Plant the classic mistakes and check each is caught; then undo them.
	const FString Built = Gate::ComputeRevision(*Blueprint);
	{
		const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioAnimBlueprintTests", "Break", "Break"));
		TestTrue(TEXT("the mistakes apply: ") + Error, Gate::ApplyOps(*Blueprint, {
			Op(TEXT("remove_transition"), TEXT("Locomotion"), TEXT("Jog"), TEXT("Idle")),
			Op(TEXT("add_transition"), TEXT("Locomotion"), TEXT("Jog"), TEXT("Idle"), TEXT("Speed < 10"), TEXT("0")),
			Op(TEXT("add_state"), TEXT("Locomotion"), TEXT("Orphan")),
			Op(TEXT("remove_transition"), TEXT("Locomotion"), TEXT("Land"), TEXT("Idle")),
			Op(TEXT("remove_transition"), TEXT("Locomotion"), TEXT("Land"), TEXT("Jog"))}, Error));
	}
	Issues = Gate::Check(*Blueprint);
	TestTrue(TEXT("same enter and leave threshold is flagged"), HasIssue(Issues, TEXT("no_hysteresis")));
	TestTrue(TEXT("a zero blend is flagged"), HasIssue(Issues, TEXT("instant_blend")));
	TestTrue(TEXT("a state with no pose is an error"), HasIssue(Issues, TEXT("empty_state")));
	TestTrue(TEXT("a state nothing leads to is flagged"), HasIssue(Issues, TEXT("unreachable_state")));
	TestTrue(TEXT("a one-shot state with no exit freezes"), HasIssue(Issues, TEXT("frozen_pose")));
	TestTrue(TEXT("and it is a dead end"), HasIssue(Issues, TEXT("dead_end")));
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*Blueprint), Built);

	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("one undo restores the graph"), Gate::ComputeRevision(*Blueprint), Built);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimBlueprintFrontDoorTest,
	"HyperAIStudio.AnimBlueprint.Contracts.PlanFrontDoor",
	HyperAIStudio::AnimBlueprint::Tests::Flags)

bool FHyperAIStudioAnimBlueprintFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::AnimBlueprint::Tests;
	FHyperAIAnimBlueprintInspectRequest Inspect;
	Inspect.TargetPath = TEXT("C:/Nope");
	TestEqual(TEXT("non-/Game paths are refused"), FHyperAIStudioAnimBlueprintContracts::Inspect(Inspect).Status, FString(TEXT("invalid_request")));
	const FString Path = TestPath();
	Inspect.TargetPath = Path;
	TestEqual(TEXT("a missing Blueprint says so"), FHyperAIStudioAnimBlueprintContracts::Inspect(Inspect).Status, FString(TEXT("target_not_found")));

	FHyperAIAnimBlueprintApplyPlanRequest Plan;
	Plan.TargetPath = Path;
	Plan.bCreate = true;
	TestEqual(TEXT("creating needs a skeleton"), FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_skeleton")));
	Plan.bCreate = false;
	Plan.Ops = {Op(TEXT("add_state_machine"), TEXT(""), TEXT("M"))};
	TestEqual(TEXT("editing needs a revision"), FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_target_or_revision")));
	if (!AssetsPresent(*this)) return true;

	Plan.bCreate = true;
	Plan.SkeletonPath = Skeleton;
	Plan.Ops = {Op(TEXT("add_state_machine"), TEXT(""), TEXT("M")), Op(TEXT("add_transition"), TEXT("M"), TEXT("A"), TEXT("B"), TEXT("x"))};
	TestEqual(TEXT("bad ops are refused before anything is prepared"), FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op")));
	Plan.Ops = Locomotion(Jog2D());
	if (!FHyperAIStudioAnimBlueprintContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("anim_blueprint is not admitted here; the prepared dry run is skipped."));
		return true;
	}
	const FHyperAIAnimBlueprintApplyPlanReport DryRun = FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
	TestEqual(TEXT("and creates nothing"), FHyperAIStudioAnimBlueprintContracts::Inspect(Inspect).Status, FString(TEXT("target_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimBlueprintEndToEndTest,
	"HyperAIStudio.AnimBlueprint.Execution.JournaledCreateCompileAndValidate",
	HyperAIStudio::AnimBlueprint::Tests::Flags)

bool FHyperAIStudioAnimBlueprintEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::AnimBlueprint::Tests;
	// Creates an asset through the executor, so it runs only in unattended test runs.
	if (!FApp::IsUnattended())
	{
		AddInfo(TEXT("Runs only in unattended test runs."));
		return true;
	}
	if (!AssetsPresent(*this) || !FHyperAIStudioAnimBlueprintContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("Mannequin content missing, or anim_blueprint is not admitted here; skipped."));
		return true;
	}
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bRequiredApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bRequiredApproval; };

	FHyperAIAnimBlueprintApplyPlanRequest Plan;
	Plan.TargetPath = TestPath();
	Plan.bCreate = true;
	Plan.SkeletonPath = Skeleton;
	Plan.Ops = Locomotion(Jog2D());
	const FHyperAIAnimBlueprintApplyPlanReport DryRun = FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan);
	if (!TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk))
	{
		return false;
	}
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("anim-blueprint-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAIAnimBlueprintApplyPlanReport Submitted = FHyperAIStudioAnimBlueprintContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	ON_SCOPE_EXIT { Discard(HyperAIStudio::AnimBlueprint::Gate::Resolve(Plan.TargetPath, false)); };
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	PumpUntil([&]()
	{
		return !Submitted.bExecutionSubmitted
			|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
	}, 60.0);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	FHyperAIAnimBlueprintInspectRequest Inspect;
	Inspect.TargetPath = Plan.TargetPath;
	const FHyperAIAnimBlueprintInspectReport Inspected = FHyperAIStudioAnimBlueprintContracts::Inspect(Inspect);
	TestTrue(*FString::Printf(TEXT("the Blueprint exists and compiled (%s: %s)"), *Inspected.Status, *Inspected.CompileStatus),
		Inspected.bOk && (Inspected.CompileStatus == TEXT("up_to_date") || Inspected.CompileStatus == TEXT("warnings")));
	TestTrue(TEXT("with its state machine"), Inspected.Machines.Num() == 1 && Inspected.Machines[0].States.Num() == 4);
	FHyperAIAnimBlueprintValidateRequest Validate;
	Validate.TargetPath = Plan.TargetPath;
	const FHyperAIAnimBlueprintValidateReport Validated = FHyperAIStudioAnimBlueprintContracts::Validate(Validate);
	TestEqual(*FString::Printf(TEXT("validate finds no errors (%s)"), *Validated.Diagnostic), Validated.ErrorCount, 0);
	return true;
}

#endif
