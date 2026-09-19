// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioLevelDesignToolset.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLevelDesignGate.h"
#include "HyperAIStudioSettings.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::LevelDesign::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAILevelDesignOp Op(const TCHAR* Kind, const TCHAR* Name, const TCHAR* Role = TEXT(""), const TCHAR* Location = TEXT(""),
		const TCHAR* Size = TEXT(""), const TCHAR* Value = TEXT(""))
	{
		FHyperAILevelDesignOp Result;
		Result.Kind = Kind;
		Result.Name = Name;
		Result.Role = Role;
		Result.Location = Location;
		Result.Size = Size;
		Result.Value = Value;
		return Result;
	}

	const FHyperAILevelDesignPiece* FindPiece(const TArray<FHyperAILevelDesignPiece>& Pieces, const TCHAR* Name)
	{
		return Pieces.FindByPredicate([Name](const FHyperAILevelDesignPiece& Piece) { return Piece.Name == Name; });
	}

	bool HasIssue(const FHyperAILevelDesignValidateReport& Report, const TCHAR* Code, const TCHAR* Subject = nullptr)
	{
		return Report.Issues.ContainsByPredicate([Code, Subject](const FHyperAILevelDesignIssue& Issue)
		{
			return Issue.Code == Code && (!Subject || Issue.Subject == Subject);
		});
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelDesignCohortTest,
	"HyperAIStudio.LevelDesign.Contracts.ExactThreeToolCohort",
	HyperAIStudio::LevelDesign::Tests::Flags)

bool FHyperAIStudioLevelDesignCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioLevelDesignContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	TestEqual(TEXT("one variant per tool"), FHyperAIStudioLevelDesignContracts::GetAdapterDescriptor().Variants.Num(), Names.Num());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioLevelDesignToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelDesignMetricsTest,
	"HyperAIStudio.LevelDesign.Metrics.PlayerAndRecommendations",
	HyperAIStudio::LevelDesign::Tests::Flags)

bool FHyperAIStudioLevelDesignMetricsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Gate = HyperAIStudio::LevelDesign::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILevelDesignMetricsWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	// The engine Character: 34 cm radius, 176 cm tall, 45 cm steps, 420 cm/s jump, 600 cm/s walk, 980 gravity.
	FHyperAIPlayerMetrics Metrics;
	FString Error;
	TestTrue(TEXT("the engine Character measures"), Gate::MeasurePlayer(*World, TEXT("/Script/Engine.Character"), Metrics, Error));
	TestEqual(TEXT("its own defaults are used"), Metrics.Source, FString(TEXT("pawn class defaults")));
	TestTrue(TEXT("height"), FMath::IsNearlyEqual(Metrics.PlayerHeight, 176.0f, 0.5f));
	TestTrue(TEXT("radius"), FMath::IsNearlyEqual(Metrics.CapsuleRadius, 34.0f, 0.5f));
	TestTrue(TEXT("step"), FMath::IsNearlyEqual(Metrics.MaxStepHeight, 45.0f, 0.5f));
	TestTrue(*FString::Printf(TEXT("jump height from v^2 / 2g (%f)"), Metrics.JumpHeight), FMath::IsNearlyEqual(Metrics.JumpHeight, 420.0f * 420.0f / 1960.0f, 1.0f));
	TestTrue(*FString::Printf(TEXT("jump gap from speed x air time (%f)"), Metrics.MaxJumpGap), FMath::IsNearlyEqual(Metrics.MaxJumpGap, 600.0f * 840.0f / 980.0f, 2.0f));
	TestTrue(TEXT("doors fit twice the player's width"), Metrics.MinDoorWidth >= Metrics.CapsuleRadius * 4.0f);
	TestTrue(TEXT("corridors are wider than doors"), Metrics.MinCorridorWidth > Metrics.MinDoorWidth);
	TestTrue(TEXT("ceilings clear the player"), Metrics.MinCeilingHeight > Metrics.PlayerHeight);
	TestTrue(TEXT("low cover sits below high cover"), Metrics.LowCoverMin < Metrics.LowCoverMax && Metrics.LowCoverMax < Metrics.HighCoverMin);
	TestTrue(TEXT("a safe gap is shorter than the longest"), Metrics.SafeJumpGap < Metrics.MaxJumpGap);

	TestFalse(TEXT("a class that is not a pawn is refused"), Gate::MeasurePlayer(*World, TEXT("/Script/Engine.StaticMeshActor"), Metrics, Error));
	TestTrue(TEXT("the project's default pawn, or engine defaults, always measure"), Gate::MeasurePlayer(*World, FString(), Metrics, Error) && Metrics.PlayerHeight > 0.0f);
	AddInfo(FString::Printf(TEXT("Default pawn: %s (%s)."), *Metrics.PawnClass, *Metrics.Source));

	const FBox Bounds(FVector(-1000, -1000, 0), FVector(1000, 1000, 100));
	TestEqual(TEXT("+X is the top of the preview"), Gate::ToPixel(FVector(999, 0, 0), Bounds, 100), FIntPoint(50, 0));
	TestEqual(TEXT("+Y is its right"), Gate::ToPixel(FVector(0, 999, 0), Bounds, 100), FIntPoint(99, 50));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelDesignOpsTest,
	"HyperAIStudio.LevelDesign.Ops.BuildCheckAndPreview",
	HyperAIStudio::LevelDesign::Tests::Flags)

bool FHyperAIStudioLevelDesignOpsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::LevelDesign::Tests;
	namespace Gate = HyperAIStudio::LevelDesign::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILevelDesignTestWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	FHyperAIPlayerMetrics Metrics;
	FString Error;
	Gate::MeasurePlayer(*World, TEXT("/Script/Engine.Character"), Metrics, Error);
	const float Step = Metrics.MaxStepHeight;
	const float Slope = Metrics.MaxWalkableSlopeDegrees;

	TArray<FString> Notes;
	auto Rejects = [&](const TArray<FHyperAILevelDesignOp>& Ops, const TCHAR* Expected)
	{
		FString Why;
		const bool bValid = Gate::ValidateOps(*World, Ops, Step, Slope, Notes, Why);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Why), !bValid && Why.Contains(Expected));
	};
	Rejects({Op(TEXT("add_block"), TEXT("bad name!"), TEXT("wall"), TEXT("0,0,0"), TEXT("100,100,100"))}, TEXT("name must be"));
	Rejects({Op(TEXT("add_tree"), TEXT("Tree"), TEXT(""), TEXT("0,0,0"))}, TEXT("unknown kind"));
	Rejects({Op(TEXT("add_block"), TEXT("Wall"), TEXT("window"), TEXT("0,0,0"), TEXT("100,100,100"))}, TEXT("role must be one of"));
	Rejects({Op(TEXT("add_block"), TEXT("Wall"), TEXT("wall"), TEXT("0,0"), TEXT("100,100,100"))}, TEXT("location must be"));
	Rejects({Op(TEXT("add_block"), TEXT("Wall"), TEXT("wall"), TEXT("0,0,0"), TEXT("0,100,100"))}, TEXT("size must be"));
	Rejects({Op(TEXT("add_ramp"), TEXT("Steep"), TEXT(""), TEXT("0,0,0"), TEXT("100,200,200"))}, TEXT("walks up at most"));
	Rejects({Op(TEXT("add_marker"), TEXT("Here"), TEXT("treasure"), TEXT("0,0,0"))}, TEXT("role must be one of"));
	Rejects({Op(TEXT("delete"), TEXT("Nothing"))}, TEXT("no piece placed"));
	Rejects({Op(TEXT("add_marker"), TEXT("Twin"), TEXT("start"), TEXT("0,0,0")), Op(TEXT("add_marker"), TEXT("twin"), TEXT("goal"), TEXT("0,0,0"))}, TEXT("already has that name"));
	TestTrue(TEXT("a later op may move what an earlier one adds"), Gate::ValidateOps(*World,
		{Op(TEXT("add_marker"), TEXT("Twin"), TEXT("start"), TEXT("0,0,0")), Op(TEXT("move"), TEXT("Twin"), TEXT(""), TEXT("10,0,0"))}, Step, Slope, Notes, Error));
	TestEqual(TEXT("validation builds nothing"), Gate::ReadPieces(*World).Num(), 0);

	const FString Before = Gate::ComputeRevision(*World);
	Notes.Reset();
	TestTrue(TEXT("stairs are described before they are built"), Gate::ValidateOps(*World,
		{Op(TEXT("add_stairs"), TEXT("Steps"), TEXT(""), TEXT("500,-1200,0"), TEXT("300,200,150"))}, Step, Slope, Notes, Error)
		&& Notes.Num() == 1 && Notes[0].Contains(TEXT("4 steps")));

	// A 40 x 40 m floor, stairs, a ramp, a pillar, low cover and markers.
	int32 Changed = 0;
	const TArray<FHyperAILevelDesignOp> Build = {
		Op(TEXT("add_block"), TEXT("Floor"), TEXT("floor"), TEXT("0,0,-20"), TEXT("4000,4000,20")),
		Op(TEXT("add_stairs"), TEXT("Steps"), TEXT(""), TEXT("500,-1200,0"), TEXT("300,200,150")),
		Op(TEXT("add_ramp"), TEXT("Ramp"), TEXT(""), TEXT("-500,-1200,0"), TEXT("600,200,150"), TEXT("90")),
		Op(TEXT("add_block"), TEXT("Pillar"), TEXT("pillar"), TEXT("1200,-1200,0"), TEXT("200,200,800")),
		Op(TEXT("add_block"), TEXT("Crate"), TEXT("cover_low"), TEXT("0,1200,0"), TEXT("200,60,300")),
		Op(TEXT("add_marker"), TEXT("Start"), TEXT("start"), TEXT("-1500,0,0")),
		Op(TEXT("add_marker"), TEXT("Goal"), TEXT("goal"), TEXT("1500,0,0")),
		Op(TEXT("add_marker"), TEXT("Sniper"), TEXT("spawn"), TEXT("1500,1500,0")),
		Op(TEXT("add_marker"), TEXT("Hide"), TEXT("cover"), TEXT("0,1100,0")),
		Op(TEXT("add_marker"), TEXT("Floating"), TEXT("pickup"), TEXT("0,0,2000")),
		Op(TEXT("add_marker"), TEXT("Buried"), TEXT("checkpoint"), TEXT("1200,-1200,0"))};
	TestTrue(TEXT("the blockout builds: ") + Error, Gate::ApplyOps(*World, Build, Step, Slope, Changed, Error));
	TArray<FHyperAILevelDesignPiece> Pieces = Gate::ReadPieces(*World);
	TestEqual(TEXT("one entry per piece"), Pieces.Num(), Build.Num());
	TestTrue(TEXT("the stairs have four climbable steps"), FindPiece(Pieces, TEXT("Steps")) && FindPiece(Pieces, TEXT("Steps"))->ActorCount == 4);
	TestEqual(TEXT("actors added"), Changed, 14);
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*World), Before);

	FHyperAILevelDesignValidateReport Report;
	Gate::CheckLayout(*World, Metrics, 1000.0f, Report);
	TestTrue(TEXT("markers on the floor stand fine"), !HasIssue(Report, TEXT("no_floor"), TEXT("Start")) && !HasIssue(Report, TEXT("no_room"), TEXT("Goal")));
	TestTrue(TEXT("a marker in the air has no floor"), HasIssue(Report, TEXT("no_floor"), TEXT("Floating")));
	TestTrue(TEXT("a marker inside a pillar has no room"), HasIssue(Report, TEXT("no_room"), TEXT("Buried")));
	TestTrue(TEXT("3 m tall low cover is flagged"), HasIssue(Report, TEXT("cover_height"), TEXT("Crate")));
	TestFalse(TEXT("cover beside a crate is not exposed"), HasIssue(Report, TEXT("exposed_cover"), TEXT("Hide")));
	TestTrue(TEXT("with no navmesh, routes are reported unchecked"), !Report.bNavigationReady && HasIssue(Report, TEXT("no_navmesh")));
	TestTrue(TEXT("an open 15 m line from a spawn is flagged past 10 m"), HasIssue(Report, TEXT("long_sightline"), TEXT("Sniper")));
	TestTrue(TEXT("errors are counted"), Report.ErrorCount >= 2);

	FString PreviewPath;
	TestTrue(TEXT("the top-down preview renders: ") + Error, Gate::WritePreview(*World, Report, PreviewPath, Error));
	FImage Preview;
	if (TestTrue(TEXT("and it loads back"), FImageUtils::LoadImage(*PreviewPath, Preview)))
	{
		Preview.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Pixels = Preview.AsBGRA8();
		// Heights are grey and overlays coloured, so the brightest grey pixel is the top of the tallest piece.
		int32 Brightest = 0;
		int32 BrightestIndex = 0;
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			const FColor Pixel = Pixels[Index];
			if (Pixel.R == Pixel.G && Pixel.G == Pixel.B && Pixel.R > Brightest)
			{
				Brightest = Pixel.R;
				BrightestIndex = Index;
			}
		}
		// The pillar is the tallest thing, at +X and -Y of centre: upper left of the image.
		const int32 X = BrightestIndex % Preview.SizeX;
		const int32 Y = BrightestIndex / Preview.SizeX;
		TestTrue(*FString::Printf(TEXT("the tallest piece is brightest (%d)"), Brightest), Brightest > 200);
		TestTrue(*FString::Printf(TEXT("and sits upper left, where +X and -Y map (%d, %d)"), X, Y), X < Preview.SizeX / 2 && Y < Preview.SizeY / 2);
	}

	Changed = 0;
	TestTrue(TEXT("move and delete apply: ") + Error, Gate::ApplyOps(*World,
		{Op(TEXT("move"), TEXT("Start"), TEXT(""), TEXT("-1400,100,0")), Op(TEXT("delete"), TEXT("Ramp")), Op(TEXT("delete"), TEXT("Steps"))},
		Step, Slope, Changed, Error));
	Pieces = Gate::ReadPieces(*World);
	TestTrue(TEXT("the moved marker is at its new place"), FindPiece(Pieces, TEXT("Start")) && FindPiece(Pieces, TEXT("Start"))->Location == TEXT("-1400,100,0"));
	TestTrue(TEXT("deleted pieces are gone"), !FindPiece(Pieces, TEXT("Ramp")) && !FindPiece(Pieces, TEXT("Steps")));

	// Every editor undo checks graph pins, which a package still loading in the background can leave unresolved.
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestTrue(TEXT("one undo brings the deleted stairs back"), FindPiece(Gate::ReadPieces(*World), TEXT("Steps")) != nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelDesignFrontDoorTest,
	"HyperAIStudio.LevelDesign.Contracts.PlanFrontDoor",
	HyperAIStudio::LevelDesign::Tests::Flags)

bool FHyperAIStudioLevelDesignFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::LevelDesign::Tests;
	const FHyperAILevelDesignInspectReport Inspected = FHyperAIStudioLevelDesignContracts::Inspect({});
	if (!Inspected.bOk)
	{
		AddInfo(TEXT("No editor level is open; front-door checks need one. ") + Inspected.Diagnostic);
		return true;
	}
	TestTrue(TEXT("inspect returns a canonical revision"), FHyperAIStudioLevelDesignContracts::IsCanonicalSha256(Inspected.Revision));
	TestTrue(TEXT("inspect measures the player"), Inspected.Metrics.PlayerHeight > 0.0f && Inspected.Metrics.MinDoorWidth > 0.0f);
	FHyperAILevelDesignInspectRequest BadPawn;
	BadPawn.PawnClassPath = TEXT("/Script/Engine.StaticMeshActor");
	TestEqual(TEXT("a non-pawn class is refused"), FHyperAIStudioLevelDesignContracts::Inspect(BadPawn).Status, FString(TEXT("invalid_pawn_class")));

	FHyperAILevelBlockoutApplyPlanRequest Plan;
	Plan.Ops = {Op(TEXT("add_marker"), TEXT("FrontDoorProbe"), TEXT("start"), TEXT("0,0,100000"))};
	TestEqual(TEXT("revision required"), FHyperAIStudioLevelDesignContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_revision")));
	Plan.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('0'));
	TestEqual(TEXT("stale revision refused"), FHyperAIStudioLevelDesignContracts::BuildPlan(Plan).Status, FString(TEXT("stale_revision")));
	Plan.ExpectedRevision = Inspected.Revision;
	Plan.Ops = {Op(TEXT("add_ramp"), TEXT("FrontDoorRamp"), TEXT(""), TEXT("0,0,100000"), TEXT("50,100,100"))};
	TestEqual(TEXT("an unwalkable ramp is refused before anything is prepared"), FHyperAIStudioLevelDesignContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op")));
	Plan.Ops.Reset();
	TestEqual(TEXT("empty plans refused"), FHyperAIStudioLevelDesignContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op_count")));

	Plan.Ops = {Op(TEXT("add_stairs"), TEXT("FrontDoorStairs"), TEXT(""), TEXT("0,0,100000"), TEXT("400,200,200"))};
	if (!FHyperAIStudioLevelDesignContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("level_design is not admitted here; the prepared dry run is skipped."));
		return true;
	}
	const FHyperAILevelBlockoutApplyPlanReport DryRun = FHyperAIStudioLevelDesignContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
	TestTrue(TEXT("and says how the stairs will be built"), DryRun.Notes.Num() == 1 && DryRun.Notes[0].Contains(TEXT("steps")));
	TestEqual(TEXT("dry run changes nothing"), FHyperAIStudioLevelDesignContracts::Inspect({}).Revision, Inspected.Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelDesignEndToEndTest,
	"HyperAIStudio.LevelDesign.Execution.JournaledApplyValidateAndUndo",
	HyperAIStudio::LevelDesign::Tests::Flags)

bool FHyperAIStudioLevelDesignEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::LevelDesign::Tests;
	// This edits the open level, so it runs only in unattended test runs, never in an editor someone is using.
	if (!FApp::IsUnattended())
	{
		AddInfo(TEXT("Runs only in unattended test runs, so it never edits the level you have open."));
		return true;
	}
	const FHyperAILevelDesignInspectReport Before = FHyperAIStudioLevelDesignContracts::Inspect({});
	if (!Before.bOk || !FHyperAIStudioLevelDesignContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("No editor level, or level_design is not admitted here; skipped."));
		return true;
	}
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bRequiredApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bRequiredApproval; };

	// Far above the level so it touches nothing else.
	FHyperAILevelBlockoutApplyPlanRequest Plan;
	Plan.ExpectedRevision = Before.Revision;
	Plan.Ops = {
		Op(TEXT("add_block"), TEXT("E2EFloor"), TEXT("floor"), TEXT("0,0,99980"), TEXT("2000,2000,20")),
		Op(TEXT("add_marker"), TEXT("E2EStart"), TEXT("start"), TEXT("-500,0,100000")),
		Op(TEXT("add_marker"), TEXT("E2EGoal"), TEXT("goal"), TEXT("500,0,100000"))};
	const FHyperAILevelBlockoutApplyPlanReport DryRun = FHyperAIStudioLevelDesignContracts::BuildPlan(Plan);
	if (!TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk))
	{
		return false;
	}
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("level-design-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAILevelBlockoutApplyPlanReport Submitted = FHyperAIStudioLevelDesignContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	PumpUntil([&]()
	{
		return !Submitted.bExecutionSubmitted
			|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
	}, 30.0);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	const FHyperAILevelDesignInspectReport After = FHyperAIStudioLevelDesignContracts::Inspect({});
	TestTrue(TEXT("the pieces are in the level"), FindPiece(After.Pieces, TEXT("E2EFloor")) && FindPiece(After.Pieces, TEXT("E2EGoal")));
	const FHyperAILevelDesignValidateReport Validated = FHyperAIStudioLevelDesignContracts::Validate({});
	TestTrue(*FString::Printf(TEXT("validate runs (%s: %s)"), *Validated.Status, *Validated.Diagnostic), Validated.bOk);
	TestFalse(TEXT("both markers stand on the new floor"), HasIssue(Validated, TEXT("no_floor"), TEXT("E2EStart")) || HasIssue(Validated, TEXT("no_floor"), TEXT("E2EGoal")));
	TestTrue(TEXT("a preview is written"), !Validated.PreviewImagePath.IsEmpty() && IFileManager::Get().FileExists(*Validated.PreviewImagePath));
	FImage Preview;
	if (FImageUtils::LoadImage(*Validated.PreviewImagePath, Preview))
	{
		// A flat floor has a single height and must still show as floor, not as empty space.
		Preview.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		int64 Floor = 0;
		for (const FColor& Pixel : Preview.AsBGRA8())
		{
			Floor += Pixel.R == Pixel.G && Pixel.G == Pixel.B && Pixel.R >= 100 ? 1 : 0;
		}
		TestTrue(*FString::Printf(TEXT("the flat floor fills much of the preview (%lld pixels)"), Floor), Floor > Preview.SizeX * Preview.SizeY / 5);
	}

	FlushAsyncLoading();
	TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction());
	TestEqual(TEXT("one undo restores the blockout"), FHyperAIStudioLevelDesignContracts::Inspect({}).Revision, Before.Revision);
	return true;
}

#endif
