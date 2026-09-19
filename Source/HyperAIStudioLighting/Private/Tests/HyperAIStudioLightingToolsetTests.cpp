// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioLightingToolset.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLightingGate.h"
#include "HyperAIStudioSettings.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Lighting::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	constexpr int32 Side = 64;

	TArray<FColor> Solid(const FColor Colour)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Colour, Side * Side);
		return Pixels;
	}

	FHyperAILightingOp Op(const TCHAR* Kind, const TCHAR* Property = TEXT(""), const TCHAR* Value = TEXT(""), const TCHAR* Label = TEXT(""))
	{
		FHyperAILightingOp Result;
		Result.Kind = Kind;
		Result.Property = Property;
		Result.Value = Value;
		Result.ActorLabel = Label;
		return Result;
	}

	FString ValueOf(const TArray<FHyperAILightingActorRecord>& Actors, const TCHAR* Kind, const TCHAR* Key)
	{
		for (const FHyperAILightingActorRecord& Actor : Actors)
		{
			if (Actor.Kind != Kind) continue;
			for (const FHyperAILightingProperty& Property : Actor.Properties)
			{
				if (Property.Key == Key) return Property.Value;
			}
		}
		return TEXT("<missing>");
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
	FHyperAIStudioLightingCohortTest,
	"HyperAIStudio.Lighting.Contracts.ExactThreeToolCohort",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioLightingContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioLightingContracts::GetAdapterDescriptor();
	TestEqual(TEXT("one variant per tool"), Descriptor.Variants.Num(), Names.Num());
	TestFalse(TEXT("adapter fingerprint sealed"), Descriptor.AdapterFingerprint.IsEmpty());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioLightingToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
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
	FHyperAIStudioLightingImageMetricsTest,
	"HyperAIStudio.Lighting.Metrics.MeasureAndCompare",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingImageMetricsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;

	// sRGB 118 is linear 0.18: photographic middle grey.
	const FHyperAILightingImageMetrics Grey = Gate::MeasurePixels(Solid(FColor(118, 118, 118)), Side, Side);
	TestTrue(TEXT("grey measures"), Grey.bValid);
	TestTrue(*FString::Printf(TEXT("median luminance is middle grey (%f)"), Grey.LuminanceP50), FMath::IsNearlyEqual(Grey.LuminanceP50, 0.18f, 0.005f));
	TestTrue(*FString::Printf(TEXT("neutral grey is about 6500 K (%f)"), Grey.ColorTemperatureK), FMath::IsNearlyEqual(Grey.ColorTemperatureK, 6500.f, 200.f));
	TestEqual(TEXT("grey has no saturation"), Grey.MeanSaturation, 0.f);
	TestTrue(TEXT("flat image has contrast 1"), FMath::IsNearlyEqual(Grey.ContrastRatio, 1.f, 0.001f));
	float Sum = 0.f;
	for (const float Bin : Grey.Histogram) Sum += Bin;
	TestEqual(TEXT("16 histogram bins"), Grey.Histogram.Num(), 16);
	TestTrue(TEXT("histogram sums to 1"), FMath::IsNearlyEqual(Sum, 1.f, 0.0001f));

	const FHyperAILightingImageMetrics Warm = Gate::MeasurePixels(Solid(FColor(240, 180, 120)), Side, Side);
	const FHyperAILightingImageMetrics Cool = Gate::MeasurePixels(Solid(FColor(150, 190, 240)), Side, Side);
	TestTrue(*FString::Printf(TEXT("orange reads warm (%f K)"), Warm.ColorTemperatureK), Warm.ColorTemperatureK < 4000.f);
	TestTrue(*FString::Printf(TEXT("blue reads cool (%f K)"), Cool.ColorTemperatureK), Cool.ColorTemperatureK > 9000.f);
	TestTrue(TEXT("colour is saturated"), Warm.MeanSaturation > 0.4f);

	TArray<FColor> Split = Solid(FColor::Black);
	for (int32 Index = Split.Num() / 2; Index < Split.Num(); ++Index) Split[Index] = FColor::White;
	const FHyperAILightingImageMetrics Hard = Gate::MeasurePixels(Split, Side, Side);
	TestTrue(*FString::Printf(TEXT("black and white is high contrast (%f)"), Hard.ContrastRatio), Hard.ContrastRatio > 100.f);
	TestTrue(TEXT("half the pixels are shadow"), FMath::IsNearlyEqual(Hard.ShadowFraction, 0.5f, 0.01f));
	TestTrue(TEXT("half the pixels clip"), FMath::IsNearlyEqual(Hard.HighlightFraction, 0.5f, 0.01f));

	const FHyperAILightingImageMetrics Black = Gate::MeasurePixels(Solid(FColor::Black), Side, Side);
	const FHyperAILightingImageMetrics White = Gate::MeasurePixels(Solid(FColor::White), Side, Side);
	TestEqual(TEXT("an image matches itself"), Gate::HistogramDistance(Grey.Histogram, Grey.Histogram), 0.f);
	TestTrue(TEXT("black and white are as far apart as possible"), FMath::IsNearlyEqual(Gate::HistogramDistance(Black.Histogram, White.Histogram), 1.f, 0.0001f));
	TestFalse(TEXT("wrong pixel count does not measure"), Gate::MeasurePixels(Solid(FColor::Black), Side, Side + 1).bValid);

	// sRGB 60 is linear 0.045: two stops under middle grey.
	const FHyperAILightingImageMetrics Dark = Gate::MeasurePixels(Solid(FColor(60, 60, 60)), Side, Side);
	FHyperAILightingCompareReport Locked;
	Gate::CompareMetrics(Grey, Dark, /*bExposureLocked=*/true, 0.05f, Locked);
	TestTrue(*FString::Printf(TEXT("two stops brighter wanted (%f)"), Locked.ExposureDeltaEV), FMath::IsNearlyEqual(Locked.ExposureDeltaEV, 2.f, 0.1f));
	TestFalse(TEXT("not matched"), Locked.bMatched);
	TestTrue(TEXT("first suggestion brightens through the locked exposure: ") + (Locked.Suggestions.IsEmpty() ? FString() : Locked.Suggestions[0]),
		!Locked.Suggestions.IsEmpty() && Locked.Suggestions[0].StartsWith(TEXT("Brighten")) && Locked.Suggestions[0].Contains(TEXT("lock_exposure with EV100 lowered")));
	FHyperAILightingCompareReport Unlocked;
	Gate::CompareMetrics(Grey, Dark, /*bExposureLocked=*/false, 0.05f, Unlocked);
	TestTrue(TEXT("auto exposure is flagged first"), !Unlocked.Suggestions.IsEmpty() && Unlocked.Suggestions[0].Contains(TEXT("lock_exposure first")));
	FHyperAILightingCompareReport Warmer;
	Gate::CompareMetrics(Warm, Cool, true, 0.05f, Warmer);
	TestTrue(TEXT("a warmer reference asks for warmth"), Warmer.ColorTemperatureDeltaK < 0.f
		&& Warmer.Suggestions.ContainsByPredicate([](const FString& Text) { return Text.StartsWith(TEXT("Warm the image")); }));
	FHyperAILightingCompareReport Same;
	Gate::CompareMetrics(Grey, Grey, true, 0.05f, Same);
	TestTrue(TEXT("identical images match"), Same.bMatched && Same.HistogramDistance == 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCompareFilesTest,
	"HyperAIStudio.Lighting.Metrics.CompareImageFiles",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCompareFilesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests");
	TestTrue(TEXT("reference written"), Gate::WritePng(Folder / TEXT("lighting_reference.png"), Solid(FColor(118, 118, 118)), Side, Side));
	TestTrue(TEXT("current written"), Gate::WritePng(Folder / TEXT("lighting_current.png"), Solid(FColor(60, 60, 60)), Side, Side));

	FHyperAILightingCompareRequest Request;
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.png");
	Request.CurrentImagePath = FPaths::ConvertRelativePathToFull(Folder / TEXT("lighting_current.png"));
	const FHyperAILightingCompareReport Report = FHyperAIStudioLightingContracts::Compare(Request);
	TestTrue(*FString::Printf(TEXT("compared (%s: %s)"), *Report.Status, *Report.Diagnostic), Report.bOk);
	TestEqual(TEXT("status says it differs"), Report.Status, FString(TEXT("differs")));
	TestTrue(TEXT("PNG round trip keeps the two-stop gap"), FMath::IsNearlyEqual(Report.ExposureDeltaEV, 2.f, 0.1f));
	TestTrue(TEXT("a supplied image writes no capture"), Report.CurrentImageWrittenPath.IsEmpty());

	Request.CurrentImagePath = Request.ReferenceImagePath;
	TestEqual(TEXT("an image matches itself"), FHyperAIStudioLightingContracts::Compare(Request).Status, FString(TEXT("matched")));

	Request.ReferenceImagePath = TEXT("../../outside_the_project.png");
	const FHyperAILightingCompareReport Outside = FHyperAIStudioLightingContracts::Compare(Request);
	TestEqual(TEXT("paths outside the project are refused"), Outside.Status, FString(TEXT("reference_unreadable")));
	TestTrue(TEXT("and the reason says so"), Outside.Diagnostic.Contains(TEXT("inside the project")));
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.exr");
	TestTrue(TEXT("HDR and other formats are refused"), FHyperAIStudioLightingContracts::Compare(Request).Diagnostic.Contains(TEXT("PNG, JPG")));
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.png");
	Request.MatchThreshold = 0.f;
	TestEqual(TEXT("threshold must be positive"), FHyperAIStudioLightingContracts::Compare(Request).Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingOpsTest,
	"HyperAIStudio.Lighting.Ops.ValidateApplyAndUndo",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingOpsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingTestWorld"));
	if (!World)
	{
		AddError(TEXT("Could not create a test world."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	TestEqual(TEXT("a new world has no lighting"), Gate::ReadActors(*World).Num(), 0);

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_moon"), TEXT("intensity"), TEXT("1"))}, TEXT("Unknown kind"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("999999"))}, TEXT("from 0 to 200000"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("nan"))}, TEXT("needs a number"));
	Rejects({Op(TEXT("set_sun"), TEXT("color"), TEXT("1,1"))}, TEXT("r,g,b"));
	Rejects({Op(TEXT("set_sun"), TEXT("luminance"), TEXT("1"))}, TEXT("no property"));
	Rejects({Op(TEXT("lock_exposure"))}, TEXT("EV100"));
	Rejects({Op(TEXT("lock_exposure"), TEXT(""), TEXT("12")), Op(TEXT("set_post_process"), TEXT("exposure_bias"), TEXT("1"))}, TEXT("exposure is locked"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("5"), TEXT("Nope"))}, TEXT("no sun actor is labelled"));
	FString Error;
	TestTrue(TEXT("a later op may name the actor an earlier op adds"), Gate::ValidateOps(*World,
		{Op(TEXT("set_sun"), TEXT("intensity"), TEXT("5")), Op(TEXT("set_sun"), TEXT("temperature"), TEXT("4000"), TEXT("HyperAI_Sun"))}, Error));
	TestEqual(TEXT("validation adds nothing"), Gate::ReadActors(*World).Num(), 0);

	const FString Before = Gate::ComputeRevision(*World);
	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_sun"), TEXT("intensity"), TEXT("7.5")),
		Op(TEXT("set_sun"), TEXT("temperature"), TEXT("4500")),
		Op(TEXT("set_sun"), TEXT("pitch"), TEXT("-20")),
		Op(TEXT("set_sun"), TEXT("color"), TEXT("1,0.5,0.25")),
		Op(TEXT("set_fog"), TEXT("density"), TEXT("0.05")),
		Op(TEXT("set_sky_light"), TEXT("intensity"), TEXT("2")),
		Op(TEXT("set_sky_atmosphere"), TEXT("mie_scale"), TEXT("1.5")),
		Op(TEXT("set_post_process"), TEXT("saturation"), TEXT("0.8")),
		Op(TEXT("lock_exposure"), TEXT(""), TEXT("12"))};
	TArray<AActor*> Touched;
	TestTrue(TEXT("plan applies: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error));
	TestEqual(TEXT("one actor per kind was added"), Touched.Num(), 5);
	const TArray<FHyperAILightingActorRecord> After = Gate::ReadActors(*World);
	TestEqual(TEXT("sun intensity"), ValueOf(After, TEXT("sun"), TEXT("intensity")), FString(TEXT("7.5")));
	TestEqual(TEXT("sun temperature"), ValueOf(After, TEXT("sun"), TEXT("temperature")), FString(TEXT("4500")));
	TestEqual(TEXT("temperature switched on"), ValueOf(After, TEXT("sun"), TEXT("use_temperature")), FString(TEXT("true")));
	TestEqual(TEXT("sun pitch"), ValueOf(After, TEXT("sun"), TEXT("pitch")), FString(TEXT("-20")));
	TestEqual(TEXT("fog density"), ValueOf(After, TEXT("fog"), TEXT("density")), FString(TEXT("0.05")));
	TestEqual(TEXT("sky light intensity"), ValueOf(After, TEXT("sky_light"), TEXT("intensity")), FString(TEXT("2")));
	TestEqual(TEXT("mie scale"), ValueOf(After, TEXT("sky_atmosphere"), TEXT("mie_scale")), FString(TEXT("1.5")));
	TestEqual(TEXT("saturation"), ValueOf(After, TEXT("post_process"), TEXT("saturation")), FString(TEXT("0.8")));
	TestEqual(TEXT("exposure is manual"), ValueOf(After, TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("manual")));
	TestEqual(TEXT("and locked at EV100 12"), ValueOf(After, TEXT("post_process"), TEXT("exposure_ev100")), FString(TEXT("12")));
	TestTrue(TEXT("the level reports exposure locked"), Gate::IsExposureLocked(*World));
	TestEqual(TEXT("added actors are labelled"), After[0].Label, FString(TEXT("HyperAI_Sun")));
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*World), Before);
	TestEqual(TEXT("every op resolves to its actor"), Gate::ResolveTargets(*World, Ops).Num(), 5);

	Touched.Reset();
	TestTrue(TEXT("unlock then bias applies: ") + Error, Gate::ApplyOps(*World,
		{Op(TEXT("unlock_exposure")), Op(TEXT("set_post_process"), TEXT("exposure_bias"), TEXT("1.5"))}, Touched, Error));
	const TArray<FHyperAILightingActorRecord> Unlocked = Gate::ReadActors(*World);
	TestEqual(TEXT("exposure back to the default method"), ValueOf(Unlocked, TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("default")));
	TestEqual(TEXT("bias set"), ValueOf(Unlocked, TEXT("post_process"), TEXT("exposure_bias")), FString(TEXT("1.5")));
	TestEqual(TEXT("the existing volume was reused"), Unlocked.Num(), After.Num());

	if (GEditor && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("one undo restores the lock"), ValueOf(Gate::ReadActors(*World), TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("manual")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingFrontDoorTest,
	"HyperAIStudio.Lighting.Contracts.PlanFrontDoor",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	const FHyperAILightingInspectReport Inspected = FHyperAIStudioLightingContracts::Inspect({});
	if (!Inspected.bOk)
	{
		AddInfo(TEXT("No editor level is open; front-door checks need one. ") + Inspected.Diagnostic);
		return true;
	}
	TestTrue(TEXT("inspect returns a canonical revision"), FHyperAIStudioLightingContracts::IsCanonicalSha256(Inspected.Revision));
	TestFalse(TEXT("inspect names the level"), Inspected.LevelPath.IsEmpty());

	FHyperAILightingApplyPlanRequest Plan;
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05"))};
	TestEqual(TEXT("revision required"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_revision")));
	Plan.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('0'));
	TestEqual(TEXT("stale revision refused"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("stale_revision")));
	Plan.ExpectedRevision = Inspected.Revision;
	Plan.OperationId = TEXT("lighting-front-door");
	TestEqual(TEXT("a dry run takes no operation id"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("unexpected_submission_fields")));
	Plan.OperationId.Reset();
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("9"))};
	TestEqual(TEXT("out-of-range ops refused before anything is prepared"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op")));
	Plan.Ops.Reset();
	TestEqual(TEXT("empty plans refused"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op_count")));

	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05"))};
	if (!FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("lighting_lookdev is not admitted here; the prepared dry run is skipped."));
		return true;
	}
	const FHyperAILightingApplyPlanReport DryRun = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
	TestEqual(TEXT("dry run changes nothing"), FHyperAIStudioLightingContracts::Inspect({}).Revision, Inspected.Revision);
	TestEqual(TEXT("the same plan seals the same hash"), FHyperAIStudioLightingContracts::BuildPlan(Plan).PlanHash, DryRun.PlanHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingEndToEndTest,
	"HyperAIStudio.Lighting.Execution.JournaledApplyAndUndo",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	// This edits the open level, so it runs only in unattended test runs, never in an editor someone is using.
	if (!FApp::IsUnattended())
	{
		AddInfo(TEXT("Runs only in unattended test runs, so it never edits the level you have open."));
		return true;
	}
	const FHyperAILightingInspectReport Before = FHyperAIStudioLightingContracts::Inspect({});
	if (!Before.bOk || !FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("No editor level, or lighting_lookdev is not admitted here; skipped."));
		return true;
	}
	// The approval queue has its own test; this one exercises the executor, so it runs the edit unattended.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bRequiredApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bRequiredApproval;
	};
	FHyperAILightingApplyPlanRequest Plan;
	Plan.ExpectedRevision = Before.Revision;
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05")), Op(TEXT("lock_exposure"), TEXT(""), TEXT("11"))};
	const FHyperAILightingApplyPlanReport DryRun = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	if (!TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk))
	{
		return false;
	}
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("lighting-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAILightingApplyPlanReport Submitted = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	PumpUntil([&]()
	{
		return !Submitted.bExecutionSubmitted
			|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
	}, 30.0);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	const FHyperAILightingInspectReport After = FHyperAIStudioLightingContracts::Inspect({});
	TestNotEqual(TEXT("the lighting changed"), After.Revision, Before.Revision);
	TestTrue(TEXT("exposure is locked"), After.bExposureLocked);
	TestEqual(TEXT("contrast applied"), ValueOf(After.Actors, TEXT("post_process"), TEXT("contrast")), FString(TEXT("1.05")));
	TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction());
	TestEqual(TEXT("one undo restores the level's lighting"), FHyperAIStudioLightingContracts::Inspect({}).Revision, Before.Revision);
	return true;
}

#endif
