// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioContextSnapshot.h"

#include "Misc/AutomationTest.h"

#include <limits>

namespace HyperAIStudio::ContextSnapshot::Tests
{
	FHyperAIStudioContextSnapshotInput MakeCompleteInput()
	{
		FHyperAIStudioContextSnapshotInput Input;
		Input.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
		Input.CaptureFrameNumber = 1234;
		Input.bActorSelectionAvailable = true;
		Input.SelectedActorTotal = 2;
		Input.SelectedActorPaths = {
			TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorB"),
			TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorA")
		};
		Input.bViewportAvailable = true;
		Input.ViewportTransform = FTransform(
			FRotator(10.0, 20.0, 30.0),
			FVector(100.0, 200.0, 300.0),
			FVector::OneVector);
		Input.bAssetSelectionAvailable = true;
		Input.SelectedAssetTotal = 2;
		Input.SelectedAssetPackagePaths = {
			TEXT("/Game/Zebra/Zebra"),
			TEXT("/Game/Alpha/Alpha")
		};
		return Input;
	}

	int32 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioContextSnapshotProjectionTest,
	"HyperAIStudio.NativeTools.ContextSnapshot.Projection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioContextSnapshotProjectionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSnapshot::Tests;
	FHyperAIStudioContextSnapshotLimits Limits;
	FString Error;
	FHyperAIStudioContextSnapshotResult Result;
	const FHyperAIStudioContextSnapshotInput Input = MakeCompleteInput();

	TestTrue(TEXT("A complete value snapshot projects"),
		FHyperAIStudioContextSnapshotCandidate::Project(Input, Limits, Result, Error));
	TestEqual(TEXT("All three available fields produce complete status"),
		Result.Status, EHyperAIStudioContextSnapshotStatus::Complete);
	TestEqual(TEXT("Freshness frame is retained"), Result.CaptureFrameNumber, static_cast<uint64>(1234));
	TestEqual(TEXT("Freshness timestamp is retained"), Result.CapturedAtUtc, Input.CapturedAtUtc);
	TestEqual(TEXT("Actor selection order is preserved because selection order may be meaningful"),
		Result.SelectedActorPaths[0], Input.SelectedActorPaths[0]);
	TestEqual(TEXT("Asset paths are sorted when order has no selection semantics"),
		Result.SelectedAssetPackagePaths[0], FString(TEXT("/Game/Alpha/Alpha")));
	TestTrue(TEXT("Camera location is retained"),
		Result.ViewportTransform.GetLocation().Equals(FVector(100.0, 200.0, 300.0)));

	FString JsonA;
	TestTrue(TEXT("Projected result serializes within its declared budget"),
		FHyperAIStudioContextSnapshotCandidate::SerializeJson(Result, JsonA, Error));
	TestTrue(TEXT("Schema is explicit"), JsonA.Contains(TEXT("\"schema_version\":\"hyperai.context-snapshot.v1\"")));
	TestTrue(TEXT("Camera transform is explicit"), JsonA.Contains(TEXT("\"viewport\":{")));
	TestTrue(TEXT("Serialized bytes stay bounded"), Utf8Bytes(JsonA) <= Limits.MaxOutputBytes);

	FHyperAIStudioContextSnapshotInput Permuted = Input;
	Swap(Permuted.SelectedAssetPackagePaths[0], Permuted.SelectedAssetPackagePaths[1]);
	FHyperAIStudioContextSnapshotResult PermutedResult;
	FString JsonB;
	TestTrue(TEXT("Permuted asset source projects"),
		FHyperAIStudioContextSnapshotCandidate::Project(Permuted, Limits, PermutedResult, Error));
	TestTrue(TEXT("Permuted asset source serializes"),
		FHyperAIStudioContextSnapshotCandidate::SerializeJson(PermutedResult, JsonB, Error));
	TestEqual(TEXT("Semantically unordered asset selection has deterministic JSON"), JsonB, JsonA);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioContextSnapshotBoundsTest,
	"HyperAIStudio.NativeTools.ContextSnapshot.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioContextSnapshotBoundsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSnapshot::Tests;
	FHyperAIStudioContextSnapshotInput Input = MakeCompleteInput();
	Input.SelectedActorTotal = 12;
	Input.SelectedAssetTotal = 12;
	Input.SelectedActorPaths.Reset();
	Input.SelectedAssetPackagePaths.Reset();
	for (int32 Index = 0; Index < 12; ++Index)
	{
		const FString Padding = FString::ChrN(180, static_cast<TCHAR>(TEXT('a') + (Index % 20)));
		Input.SelectedActorPaths.Add(FString::Printf(TEXT("/Game/Map.Map:PersistentLevel.Actor%02d_%s"), Index, *Padding));
		Input.SelectedAssetPackagePaths.Add(FString::Printf(TEXT("/Game/Asset%02d_%s"), Index, *Padding));
	}

	FHyperAIStudioContextSnapshotLimits Limits;
	Limits.MaxSelectedActors = 12;
	Limits.MaxSelectedAssets = 12;
	Limits.MaxPathChars = 256;
	Limits.MaxOutputBytes = 2048;
	FHyperAIStudioContextSnapshotResult Result;
	FString Error;
	TestTrue(TEXT("A large source is reduced to the byte budget"),
		FHyperAIStudioContextSnapshotCandidate::Project(Input, Limits, Result, Error));
	TestEqual(TEXT("Byte reduction makes completeness explicit"),
		Result.Status, EHyperAIStudioContextSnapshotStatus::Partial);
	TestTrue(TEXT("Output budget truncation is explicit"), Result.bOutputBudgetTruncated);
	TestTrue(TEXT("At least one collection reports truncation"), Result.bActorsTruncated || Result.bAssetsTruncated);
	FString Json;
	TestTrue(TEXT("Bounded result serializes"), FHyperAIStudioContextSnapshotCandidate::SerializeJson(Result, Json, Error));
	TestTrue(TEXT("Final UTF-8 size obeys the hard request budget"), Utf8Bytes(Json) <= Limits.MaxOutputBytes);

	FHyperAIStudioContextSnapshotLimits CountLimits;
	CountLimits.MaxSelectedActors = 1;
	CountLimits.MaxSelectedAssets = 1;
	FHyperAIStudioContextSnapshotResult CountResult;
	TestTrue(TEXT("Item caps project"),
		FHyperAIStudioContextSnapshotCandidate::Project(MakeCompleteInput(), CountLimits, CountResult, Error));
	TestEqual(TEXT("Actor return count is capped"), CountResult.SelectedActorPaths.Num(), 1);
	TestEqual(TEXT("Actor total remains the source total"), CountResult.SelectedActorTotal, 2);
	TestTrue(TEXT("Actor cap is explicit"), CountResult.bActorsTruncated);
	TestEqual(TEXT("Asset return count is capped"), CountResult.SelectedAssetPackagePaths.Num(), 1);
	TestTrue(TEXT("Asset cap is explicit"), CountResult.bAssetsTruncated);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioContextSnapshotUnavailableTest,
	"HyperAIStudio.NativeTools.ContextSnapshot.Unavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioContextSnapshotUnavailableTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioContextSnapshotInput Input;
	Input.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	Input.CaptureFrameNumber = 99;
	Input.Diagnostics = {
		{ TEXT("editor_unavailable"), TEXT("selected_actors"), TEXT("The editor is unavailable.") },
		{ TEXT("viewport_unavailable"), TEXT("viewport"), TEXT("The viewport is unavailable.") },
		{ TEXT("asset_selection_unavailable"), TEXT("selected_assets"), TEXT("The Content Browser is unavailable.") }
	};
	FHyperAIStudioContextSnapshotLimits Limits;
	FHyperAIStudioContextSnapshotResult Result;
	FString Error;
	TestTrue(TEXT("Unavailable state is a valid diagnostic result"),
		FHyperAIStudioContextSnapshotCandidate::Project(Input, Limits, Result, Error));
	TestEqual(TEXT("No available source is unavailable, not false success"),
		Result.Status, EHyperAIStudioContextSnapshotStatus::Unavailable);
	TestEqual(TEXT("Unavailable diagnostics are retained"), Result.Diagnostics.Num(), 3);
	TestEqual(TEXT("Unavailable actors claim no total"), Result.SelectedActorTotal, 0);
	TestEqual(TEXT("Unavailable actors claim no items"), Result.SelectedActorPaths.Num(), 0);
	TestFalse(TEXT("Unavailable actors claim no truncation"), Result.bActorsTruncated);
	TestEqual(TEXT("Unavailable assets claim no total"), Result.SelectedAssetTotal, 0);
	TestEqual(TEXT("Unavailable assets claim no items"), Result.SelectedAssetPackagePaths.Num(), 0);
	TestFalse(TEXT("Unavailable assets claim no truncation"), Result.bAssetsTruncated);
	TestTrue(TEXT("Unavailable viewport carries no transform"),
		Result.ViewportTransform.Equals(FTransform::Identity));

	FHyperAIStudioContextSnapshotInput InvalidCamera = HyperAIStudio::ContextSnapshot::Tests::MakeCompleteInput();
	InvalidCamera.ViewportTransform.SetLocation(FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
	TestTrue(TEXT("Non-finite camera returns a diagnostic partial result"),
		FHyperAIStudioContextSnapshotCandidate::Project(InvalidCamera, Limits, Result, Error));
	TestFalse(TEXT("Non-finite camera is never advertised as available"), Result.bViewportAvailable);
	TestEqual(TEXT("A bad camera makes the compound partial"),
		Result.Status, EHyperAIStudioContextSnapshotStatus::Partial);
	TestTrue(TEXT("Bad camera has a machine-readable diagnostic"),
		Result.Diagnostics.ContainsByPredicate([](const FHyperAIStudioContextSnapshotDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("invalid_viewport_transform");
		}));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioContextSnapshotInvalidInputTest,
	"HyperAIStudio.NativeTools.ContextSnapshot.InvalidInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioContextSnapshotInvalidInputTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSnapshot::Tests;
	FString Error;
	FHyperAIStudioContextSnapshotResult Result;
	FHyperAIStudioContextSnapshotLimits InvalidLimits;
	InvalidLimits.MaxOutputBytes = 1;
	TestFalse(TEXT("An unsafe output budget is rejected"),
		FHyperAIStudioContextSnapshotCandidate::Project(MakeCompleteInput(), InvalidLimits, Result, Error));
	TestFalse(TEXT("Limit rejection is explained"), Error.IsEmpty());
	InvalidLimits = FHyperAIStudioContextSnapshotLimits();
	InvalidLimits.MaxDiagnostics = 2;
	TestFalse(TEXT("Diagnostic bounds must retain one unavailable diagnostic per source"),
		FHyperAIStudioContextSnapshotCandidate::Project(MakeCompleteInput(), InvalidLimits, Result, Error));

	FHyperAIStudioContextSnapshotInput InvalidTotals = MakeCompleteInput();
	InvalidTotals.SelectedActorTotal = 1;
	FHyperAIStudioContextSnapshotLimits Limits;
	TestFalse(TEXT("A contradictory actor total fails closed"),
		FHyperAIStudioContextSnapshotCandidate::Project(InvalidTotals, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput EmbeddedNull = MakeCompleteInput();
	EmbeddedNull.SelectedAssetPackagePaths[0].GetCharArray().Insert(TEXT('\0'), 2);
	TestTrue(TEXT("An invalid identity is omitted with explicit partial status"),
		FHyperAIStudioContextSnapshotCandidate::Project(EmbeddedNull, Limits, Result, Error));
	TestTrue(TEXT("Invalid identity marks assets truncated"), Result.bAssetsTruncated);
	TestEqual(TEXT("Invalid identity cannot survive projection"), Result.SelectedAssetPackagePaths.Num(), 1);

	FHyperAIStudioContextSnapshotInput InvalidTimestamp = MakeCompleteInput();
	InvalidTimestamp.CapturedAtUtc = TEXT("2026-08-14T12:34:56Z");
	TestFalse(TEXT("Timestamp without canonical milliseconds is rejected"),
		FHyperAIStudioContextSnapshotCandidate::Project(InvalidTimestamp, Limits, Result, Error));
	InvalidTimestamp = MakeCompleteInput();
	InvalidTimestamp.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000+00:00");
	TestFalse(TEXT("UTC offsets are rejected in favor of a canonical Z timestamp"),
		FHyperAIStudioContextSnapshotCandidate::Project(InvalidTimestamp, Limits, Result, Error));
	InvalidTimestamp = MakeCompleteInput();
	InvalidTimestamp.CapturedAtUtc = TEXT("2026-02-30T12:34:56.000Z");
	TestFalse(TEXT("An impossible calendar timestamp is rejected"),
		FHyperAIStudioContextSnapshotCandidate::Project(InvalidTimestamp, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput MissingFrame = MakeCompleteInput();
	MissingFrame.CaptureFrameNumber = 0;
	TestFalse(TEXT("Missing Unreal frame evidence is rejected"),
		FHyperAIStudioContextSnapshotCandidate::Project(MissingFrame, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput MissingTotal = MakeCompleteInput();
	MissingTotal.SelectedActorTotal = INDEX_NONE;
	TestFalse(TEXT("Available actor selection needs an explicit total"),
		FHyperAIStudioContextSnapshotCandidate::Project(MissingTotal, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput UnavailableClaimsItems;
	UnavailableClaimsItems.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	UnavailableClaimsItems.CaptureFrameNumber = 12;
	UnavailableClaimsItems.SelectedActorTotal = 1;
	UnavailableClaimsItems.SelectedActorPaths = { TEXT("/Game/Test.Test:PersistentLevel.Actor") };
	TestFalse(TEXT("Unavailable actor state cannot claim items or totals"),
		FHyperAIStudioContextSnapshotCandidate::Project(UnavailableClaimsItems, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput UnavailableAssetsClaimItems;
	UnavailableAssetsClaimItems.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	UnavailableAssetsClaimItems.CaptureFrameNumber = 12;
	UnavailableAssetsClaimItems.SelectedAssetTotal = 1;
	UnavailableAssetsClaimItems.SelectedAssetPackagePaths = { TEXT("/Game/Asset") };
	TestFalse(TEXT("Unavailable asset state cannot claim items or totals"),
		FHyperAIStudioContextSnapshotCandidate::Project(UnavailableAssetsClaimItems, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput UnavailableClaimsCamera;
	UnavailableClaimsCamera.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	UnavailableClaimsCamera.CaptureFrameNumber = 13;
	UnavailableClaimsCamera.ViewportTransform.SetLocation(FVector(1.0, 2.0, 3.0));
	TestFalse(TEXT("Unavailable viewport cannot claim a transform"),
		FHyperAIStudioContextSnapshotCandidate::Project(UnavailableClaimsCamera, Limits, Result, Error));

	FHyperAIStudioContextSnapshotInput InformationalDiagnostic = MakeCompleteInput();
	InformationalDiagnostic.Diagnostics.Add({
		TEXT("unexpected_warning"),
		TEXT("selected_assets"),
		TEXT("A complete fixture cannot silently carry a warning.") });
	TestTrue(TEXT("Diagnostic state still projects"),
		FHyperAIStudioContextSnapshotCandidate::Project(InformationalDiagnostic, Limits, Result, Error));
	TestEqual(TEXT("Any diagnostic prevents complete status"),
		Result.Status, EHyperAIStudioContextSnapshotStatus::Partial);

	FHyperAIStudioContextSnapshotResult ContradictoryResult;
	ContradictoryResult.Status = EHyperAIStudioContextSnapshotStatus::Complete;
	ContradictoryResult.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	ContradictoryResult.CaptureFrameNumber = 14;
	ContradictoryResult.OutputBudgetBytes = 32768;
	FString Json;
	TestFalse(TEXT("Serializer rejects a status that contradicts unavailable fields"),
		FHyperAIStudioContextSnapshotCandidate::SerializeJson(ContradictoryResult, Json, Error));

	FHyperAIStudioContextSnapshotResult SilentCollectionTruncation;
	SilentCollectionTruncation.Status = EHyperAIStudioContextSnapshotStatus::Complete;
	SilentCollectionTruncation.CapturedAtUtc = TEXT("2026-08-14T12:34:56.000Z");
	SilentCollectionTruncation.CaptureFrameNumber = 15;
	SilentCollectionTruncation.OutputBudgetBytes = 32768;
	SilentCollectionTruncation.bActorSelectionAvailable = true;
	SilentCollectionTruncation.SelectedActorTotal = 1;
	SilentCollectionTruncation.bViewportAvailable = true;
	SilentCollectionTruncation.bAssetSelectionAvailable = true;
	TestFalse(TEXT("Serializer rejects omitted items without an explicit truncation flag"),
		FHyperAIStudioContextSnapshotCandidate::SerializeJson(SilentCollectionTruncation, Json, Error));

	return true;
}

#endif
