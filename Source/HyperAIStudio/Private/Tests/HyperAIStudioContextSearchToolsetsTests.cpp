// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioCapabilityPackRegistry.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace HyperAIStudio::ContextSearch::Tests
{
	FHyperAIContextSearchValueRecord MakeRecord(
		const TCHAR* Kind,
		const TCHAR* Id,
		const TCHAR* Name,
		const TCHAR* ExtraField = TEXT("path"),
		const TCHAR* ExtraValue = TEXT("/Game/Test"))
	{
		FHyperAIContextSearchValueRecord Record;
		Record.Kind = Kind;
		Record.RecordId = Id;
		Record.SearchTextLower = FString(Name).ToLower() + TEXT(" ") + FString(ExtraValue).ToLower();
		Record.Fields.Add({ TEXT("name"), TEXT("name"), Name });
		Record.Fields.Add({ ExtraField, TEXT("string"), ExtraValue });
		return Record;
	}

	FHyperAIProjectIndexValueSnapshot MakeReadyIndex()
	{
		FHyperAIProjectIndexValueSnapshot Snapshot;
		Snapshot.Status = TEXT("ready");
		Snapshot.StartedUtc = TEXT("2026-08-14T10:00:00.000Z");
		Snapshot.CompletedUtc = TEXT("2026-08-14T10:00:01.000Z");
		Snapshot.Value.SnapshotUtc = Snapshot.CompletedUtc;
		Snapshot.Value.ObservationScope = TEXT("project_on_disk_index");
		Snapshot.Value.bOnDiskOnly = true;
		Snapshot.Value.Generation = 7;
		Snapshot.Value.Records.Add(MakeRecord(
			TEXT("asset"),
			TEXT("/Game/Characters/Hero.Hero"),
			TEXT("Hero"),
			TEXT("package_name"),
			TEXT("/Game/Characters/Hero")));
		FHyperAIContextSearchValueRecord Symbol = MakeRecord(
			TEXT("cpp_symbol"),
			TEXT("symbol-1"),
			TEXT("BuildHero"),
			TEXT("relative_path"),
			TEXT("Source/Game/Hero.cpp"));
		Symbol.Fields.Add({ TEXT("symbol_kind"), TEXT("string"), TEXT("function_candidate") });
		Symbol.Fields.Add({ TEXT("line"), TEXT("integer"), TEXT("42") });
		Snapshot.Value.Records.Add(MoveTemp(Symbol));
		Snapshot.AssetRecordCount = 1;
		Snapshot.CppFileCount = 1;
		Snapshot.CppSymbolCount = 1;
		Snapshot.SnapshotFingerprint =
			FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Snapshot.Value);
		return Snapshot;
	}

	bool HasDiagnostic(const FHyperAIReadReport& Report, const TCHAR* Code)
	{
		return Report.Diagnostics.ContainsByPredicate([Code](const FHyperAIReadDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
	}

	FString MakeFixtureRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("HyperAIStudioContextSearchTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchManifestTest,
	"HyperAIStudio.NativeTools.ContextSearch.ManifestAndCohorts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchManifestTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	const TArray<FHyperAIContextSearchManifestEntry>& Manifest =
		FHyperAIStudioContextSearchContracts::GetManifest();
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	TestEqual(TEXT("Exactly seven source candidates exist"), Manifest.Num(), 7);
	TSet<FString> Names;
	TSet<FString> Toolsets;
	TSet<FString> AtomicCohorts;
	for (const FHyperAIContextSearchManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		Toolsets.Add(Entry.Toolset);
		TestEqual(
			TEXT("Every entry remains SourceCandidate"),
			Entry.AdmissionState,
			FHyperAIContextSearchManifestEntry::EAdmissionState::SourceCandidate);
		TestFalse(
			TEXT("Source candidates are not registered by production policy"),
			FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(Entry.Toolset, false));
		TestTrue(
			TEXT("Dev-only pending switch may expose each member of the generated cohort"),
			FHyperAIStudioContextSearchContracts::IsRegistrationAllowed(Entry.Toolset, true));
		const FHyperAIStudioCapabilityToolDefinition* CatalogTool =
			Catalog.Tools.FindByPredicate([&Entry](const FHyperAIStudioCapabilityToolDefinition& Tool)
			{
				return Tool.Name == Entry.Name;
			});
		TestNotNull(TEXT("Every manifest entry has one generated-catalog contract"), CatalogTool);
		if (CatalogTool)
		{
			AtomicCohorts.Add(CatalogTool->AtomicCohortId);
			TestEqual(
				TEXT("Manifest and generated catalog agree on source-candidate admission"),
				CatalogTool->AdmissionState,
				EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
		}
	}
	TestEqual(TEXT("No names are duplicated"), Names.Num(), 7);
	TestEqual(TEXT("Seven contracts are exposed by five owned toolset classes"), Toolsets.Num(), 5);
	TestEqual(TEXT("All seven contracts share one generated atomic admission cohort"), AtomicCohorts.Num(), 1);
	TestTrue(
		TEXT("Generated context/search cohort identity is stable"),
		AtomicCohorts.Contains(TEXT("cohort.source.hyperaistudiocontextsearchtoolsets.v1")));
	TestTrue(TEXT("Batch name present"), Names.Contains(TEXT("hyper_batch_query")));
	TestTrue(TEXT("Context name present"), Names.Contains(TEXT("hyper_context_snapshot")));
	TestTrue(TEXT("Scene name present"), Names.Contains(TEXT("hyper_scene_inspect")));
	TestTrue(TEXT("Scene apply name present"), Names.Contains(TEXT("hyper_scene_apply_plan")));
	TestTrue(TEXT("Scene validate name present"), Names.Contains(TEXT("hyper_scene_validate")));
	TestTrue(TEXT("Project search name present"), Names.Contains(TEXT("hyper_project_search")));
	TestTrue(TEXT("Index status name present"), Names.Contains(TEXT("hyper_project_index_status")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectionCursorTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectionAndCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectionCursorTest::RunTest(const FString& Parameters)
{
	TSet<FString> Allowed = { TEXT("name"), TEXT("path") };
	TArray<FString> Projection;
	FString Code;
	FString Error;
	TestTrue(
		TEXT("Projection normalizes case, order, and duplicates"),
		FHyperAIStudioContextSearchContracts::NormalizeProjection(
			{ TEXT("Path"), TEXT("name"), TEXT("path") },
			Allowed,
			{ TEXT("name") },
			Projection,
			Code,
			Error));
	TestEqual(TEXT("Projection deduplicated"), Projection.Num(), 2);
	TestEqual(TEXT("Projection stable sort first"), Projection[0], FString(TEXT("name")));
	TestEqual(TEXT("Projection stable sort second"), Projection[1], FString(TEXT("path")));
	TestFalse(
		TEXT("Unknown projection fails closed"),
		FHyperAIStudioContextSearchContracts::NormalizeProjection(
			{ TEXT("raw_source") }, Allowed, { TEXT("name") }, Projection, Code, Error));
	TestEqual(TEXT("Unknown field has stable code"), Code, FString(TEXT("value_not_allowlisted")));

	const FString RequestA = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("request-a") });
	const FString RequestB = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("request-b") });
	const FString SnapshotA = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("snapshot-a") });
	const FString SnapshotB = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("snapshot-b") });
	TestTrue(TEXT("Request fingerprint is SHA-256"), RequestA.StartsWith(TEXT("sha256:")) && RequestA.Len() == 71);
	TestEqual(
		TEXT("Local bounded SHA-256 matches the canonical UTF-8 vector"),
		FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("abc") }),
		FString(TEXT("sha256:aab5f9ae99b2e38fb462025c8f72f570c9c811705d2a4277dc855d7fa293fe97")));
	FHyperAIContextSearchValueSnapshot FingerprintFixture;
	FingerprintFixture.ObservationScope = TEXT("scope");
	FingerprintFixture.Generation = 9;
	FingerprintFixture.DirtyPackages = { TEXT("/Game/Z"), TEXT("/Game/A") };
	FHyperAIContextSearchValueRecord FingerprintRecord;
	FingerprintRecord.QueryId = TEXT("query");
	FingerprintRecord.Kind = TEXT("kind");
	FingerprintRecord.RecordId = TEXT("record");
	FingerprintRecord.Fields = {
		{ TEXT("z"), TEXT("string"), TEXT("last") },
		{ TEXT("a"), TEXT("string"), TEXT("first") }
	};
	FingerprintFixture.Records.Add(MoveTemp(FingerprintRecord));
	const FString MaterializedReference = FHyperAIStudioContextSearchContracts::HashTokens({
		TEXT("scope"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("0"), TEXT("9"),
		TEXT("/Game/A"), TEXT("/Game/Z"), TEXT("query"), TEXT("kind"), TEXT("record"),
		TEXT("a"), TEXT("string"), TEXT("first"), TEXT("z"), TEXT("string"), TEXT("last")
	});
	TestEqual(
		TEXT("Streaming snapshot hash stays byte-identical to the materialized canonical token route"),
		FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(FingerprintFixture),
		MaterializedReference);
	FString MalformedUtf16;
	MalformedUtf16.AppendChar(static_cast<TCHAR>(0xd800));
	TestTrue(
		TEXT("Malformed UTF-16 is rejected before canonical hashing"),
		FHyperAIStudioContextSearchContracts::HashTokens({ MalformedUtf16 }).IsEmpty());
	const FString Cursor = FHyperAIStudioContextSearchContracts::MakeCursor(RequestA, SnapshotA, 3);
	int32 Offset = INDEX_NONE;
	TestTrue(
		TEXT("Bound cursor parses"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestA, SnapshotA, 10, Offset, Code, Error));
	TestEqual(TEXT("Offset round trips"), Offset, 3);
	TestFalse(
		TEXT("Changed request rejects cursor"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestB, SnapshotA, 10, Offset, Code, Error));
	TestEqual(TEXT("Request mismatch code"), Code, FString(TEXT("cursor_request_mismatch")));
	TestFalse(
		TEXT("Changed snapshot rejects cursor"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Cursor, RequestA, SnapshotB, 10, Offset, Code, Error));
	TestEqual(TEXT("Snapshot mismatch code"), Code, FString(TEXT("cursor_snapshot_changed")));
	FString Tampered = Cursor;
	Tampered[15] = Tampered[15] == TEXT('a') ? TEXT('b') : TEXT('a');
	TestFalse(
		TEXT("Tampered cursor rejects"),
		FHyperAIStudioContextSearchContracts::ParseCursor(Tampered, RequestA, SnapshotA, 10, Offset, Code, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSnapshotAssetBoundTest,
	"HyperAIStudio.NativeTools.ContextSearch.ContextSnapshotAssetBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSnapshotAssetBoundTest::RunTest(const FString& Parameters)
{
	FHyperAIContextSnapshotRequest InvalidRequest;
	InvalidRequest.MaxSelectedAssets = 1;
	const FHyperAIReadReport InvalidReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(InvalidRequest);
	TestFalse(TEXT("Positive asset selection bound fails closed"), InvalidReport.bOk);
	TestEqual(TEXT("Positive asset selection bound is invalid"), InvalidReport.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Positive asset selection bound has stable diagnostic"),
		HyperAIStudio::ContextSearch::Tests::HasDiagnostic(InvalidReport, TEXT("selected_assets_not_supported")));

	FHyperAIContextSnapshotRequest Request;
	Request.MaxSelectedActors = 0;
	Request.MaxSelectedAssets = 0;
	Request.MaxBlueprintNodes = 0;
	Request.bIncludeBlueprintPins = false;
	const FHyperAIReadReport Report = UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(Request);
	TestNotEqual(TEXT("Zero asset bound is accepted"), Report.Status, FString(TEXT("invalid_request")));
	TestFalse(TEXT("Zero asset bound emits no selected-asset incomplete diagnostic"),
		Report.Diagnostics.ContainsByPredicate([](const FHyperAIReadDiagnostic& Diagnostic)
		{
			return Diagnostic.Code.Contains(TEXT("asset"), ESearchCase::IgnoreCase)
				|| Diagnostic.Field.Contains(TEXT("asset"), ESearchCase::IgnoreCase);
		}));
	TestFalse(TEXT("Context snapshot never returns selected-asset records"),
		Report.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind == TEXT("selected_asset");
		}));

	FHyperAIContextSnapshotRequest FocusedRequest;
	FocusedRequest.Fields = {
		TEXT("path"), TEXT("location"), TEXT("rotation"), TEXT("scale")
	};
	FocusedRequest.MaxSelectedActors = 8;
	FocusedRequest.MaxSelectedAssets = 0;
	FocusedRequest.MaxBlueprintNodes = 0;
	FocusedRequest.bIncludeBlueprintPins = false;
	const FHyperAIReadReport FocusedReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(FocusedRequest);
	TestTrue(TEXT("Actor plus viewport fast path succeeds"), FocusedReport.bOk);
	TestTrue(TEXT("Actor plus viewport fast path records the Unreal frame"), FocusedReport.SnapshotGeneration > 0);
	TestFalse(TEXT("Actor plus viewport fast path is complete"), FocusedReport.bIncomplete);
	TestTrue(TEXT("Actor plus viewport fast path captures a viewport"),
		FocusedReport.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind == TEXT("viewport");
		}));
	TestFalse(TEXT("Actor plus viewport fast path materializes no other categories"),
		FocusedReport.Records.ContainsByPredicate([](const FHyperAIReadRecord& Record)
		{
			return Record.Kind != TEXT("viewport") && Record.Kind != TEXT("selected_actor");
		}));
	TestEqual(TEXT("Actor plus viewport fast path captures no dirty-package inventory"),
		FocusedReport.DirtyPackageCount, 0);

	FHyperAIContextSnapshotRequest AssetFieldRequest;
	AssetFieldRequest.Fields = { TEXT("asset_name") };
	const FHyperAIReadReport AssetFieldReport =
		UHyperAIStudioContextSnapshotToolset::hyper_context_snapshot(AssetFieldRequest);
	TestFalse(TEXT("Removed asset projection fails closed"), AssetFieldReport.bOk);
	TestTrue(TEXT("Removed asset projection is not allowlisted"),
		HyperAIStudio::ContextSearch::Tests::HasDiagnostic(AssetFieldReport, TEXT("value_not_allowlisted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchPageTest,
	"HyperAIStudio.NativeTools.ContextSearch.ImmutablePagingBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchPageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIContextSearchValueSnapshot Snapshot;
	Snapshot.SnapshotUtc = TEXT("2026-08-14T10:00:00.000Z");
	Snapshot.ObservationScope = TEXT("test_value_snapshot");
	Snapshot.Generation = 3;
	Snapshot.bDirty = true;
	Snapshot.DirtyPackages.Add(TEXT("/Game/Dirty"));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("z"), TEXT("Zulu"), TEXT("path"), TEXT("/Game/Z")));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("a"), TEXT("Alpha"), TEXT("path"), TEXT("/Game/A")));
	Snapshot.Records.Add(MakeRecord(TEXT("actor"), TEXT("m"), TEXT("Mike"), TEXT("path"), TEXT("/Game/M")));
	const FString RequestFingerprint = FHyperAIStudioContextSearchContracts::HashTokens({ TEXT("page-test") });
	FHyperAIReadReport First;
	TestTrue(
		TEXT("First page succeeds"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, FString(), 4096, First));
	TestEqual(TEXT("Stable record sort first"), First.Records[0].RecordId, FString(TEXT("a")));
	TestEqual(TEXT("Stable record sort second"), First.Records[1].RecordId, FString(TEXT("m")));
	TestEqual(TEXT("Projection strips unrequested path"), First.Records[0].Fields.Num(), 1);
	TestTrue(TEXT("Dirty evidence preserved"), First.bDirty);
	TestEqual(TEXT("Dirty package preserved"), First.DirtyPackages[0], FString(TEXT("/Game/Dirty")));
	TestFalse(TEXT("Continuation cursor emitted"), First.NextCursor.IsEmpty());

	FHyperAIReadReport Second;
	TestTrue(
		TEXT("Second page succeeds"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, First.NextCursor, 4096, Second));
	TestEqual(TEXT("Second page contains final record"), Second.Records[0].RecordId, FString(TEXT("z")));
	Snapshot.Records[0].Fields[0].Value = TEXT("Changed");
	FHyperAIReadReport Stale;
	TestFalse(
		TEXT("Snapshot mutation invalidates cursor"),
		FHyperAIStudioContextSearchContracts::ProjectPage(
			Snapshot, { TEXT("name") }, RequestFingerprint, 2, First.NextCursor, 4096, Stale));
	TestEqual(TEXT("Stale cursor is invalid_request"), Stale.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Stale cursor diagnostic present"), HasDiagnostic(Stale, TEXT("cursor_snapshot_changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchSourcePrivacyTest,
	"HyperAIStudio.NativeTools.ContextSearch.CppIndexPrivacyAndContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchSourcePrivacyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	const FString FixtureBase = MakeFixtureRoot();
	const FString ProjectRoot = FPaths::Combine(FixtureBase, TEXT("Project"));
	const FString SourceDirectory = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Game"));
	const FString SourcePath = FPaths::Combine(SourceDirectory, TEXT("Hero.cpp"));
	const FString OutsideRoot = FPaths::Combine(FixtureBase, TEXT("Outside"));
	const FString OutsidePath = FPaths::Combine(OutsideRoot, TEXT("Secret.cpp"));
	IFileManager& FileManager = IFileManager::Get();
	TestTrue(TEXT("Fixture project source directory created"), FileManager.MakeDirectory(*SourceDirectory, true));
	TestTrue(TEXT("Fixture outside directory created"), FileManager.MakeDirectory(*OutsideRoot, true));
	const FString Secret = TEXT("DO_NOT_RETURN_SECRET_VALUE_987");
	const FString Source = FString::Printf(
		TEXT("class FHero final {};\nstruct FState {};\nvoid BuildHero(int32 Count) { const TCHAR* Token = TEXT(\"%s\"); }\n"),
		*Secret);
	TestTrue(
		TEXT("Fixture source saved"),
		FFileHelper::SaveStringToFile(Source, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(
		TEXT("Fixture outside source saved"),
		FFileHelper::SaveStringToFile(TEXT("class FSecret {};"), *OutsidePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

#if !PLATFORM_WINDOWS
	FString UnsupportedRelativePath;
	TestFalse(
		TEXT("Unsupported platforms fail closed instead of using lexical containment"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, SourcePath, UnsupportedRelativePath));
	TestTrue(TEXT("Unsupported-platform fixture tree removed"), FileManager.DeleteDirectory(*FixtureBase, false, true));
	return true;
#endif

	FString RelativePath;
	TestTrue(
		TEXT("Project C++ source is contained"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, SourcePath, RelativePath));
	TestEqual(TEXT("Only project-relative path remains"), RelativePath, FString(TEXT("Source/Game/Hero.cpp")));
	TestFalse(
		TEXT("External source is rejected"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, OutsidePath, RelativePath));
	TestFalse(
		TEXT("Non-C++ file is rejected"),
		FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
			ProjectRoot, FPaths::Combine(SourceDirectory, TEXT("Secrets.env")), RelativePath));

	FHyperAIProjectSourceReadResult Read;
	FString ReadCode;
	TestTrue(
		TEXT("Same-handle contained source read succeeds"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, SourcePath, Read, ReadCode, 4096));
	TestEqual(TEXT("Same-handle read returns only relative identity"), Read.RelativePath, FString(TEXT("Source/Game/Hero.cpp")));
	TestEqual(TEXT("Same-handle read reports exact bytes"), Read.BytesRead, static_cast<int64>(FTCHARToUTF8(*Source).Length()));
	TestFalse(
		TEXT("Same-handle read rejects before allocating beyond requested byte cap"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, SourcePath, Read, ReadCode, 4));
	TestEqual(TEXT("Oversize source has stable rejection"), ReadCode, FString(TEXT("source_file_size_bound_rejected")));

	for (int32 Index = 0; Index < 16; ++Index)
	{
		const FString FanoutPath = FPaths::Combine(
			ProjectRoot,
			TEXT("Source"),
			FString::Printf(TEXT("Fanout%02d.cpp"), Index));
		TestTrue(
			TEXT("High-fanout fixture saved"),
			FFileHelper::SaveStringToFile(TEXT("class FBounded {};"), *FanoutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}
	TArray<FString> Enumerated;
	bool bEnumerationTruncated = false;
	FString EnumerationCode;
	TestTrue(
		TEXT("Bounded directory traversal returns a partial observation"),
		FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
			ProjectRoot, 32, 4, Enumerated, bEnumerationTruncated, EnumerationCode));
	TestTrue(TEXT("High fanout is explicitly truncated"), bEnumerationTruncated);
	TestTrue(TEXT("High fanout never exceeds entry work bound"), Enumerated.Num() <= 4);
	for (int32 Index = 1; Index < Enumerated.Num(); ++Index)
	{
		TestTrue(TEXT("Observed candidates are deterministically sorted"), Enumerated[Index - 1] < Enumerated[Index]);
	}

#if PLATFORM_WINDOWS
	const FString LinkPath = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("ExternalLink"));
	const DWORD LinkFlags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2u;
	if (::CreateSymbolicLinkW(*LinkPath, *OutsideRoot, LinkFlags))
	{
		TestFalse(
			TEXT("Final-handle containment rejects an external directory symlink"),
			FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
				ProjectRoot, FPaths::Combine(LinkPath, TEXT("Secret.cpp")), RelativePath));
		Enumerated.Reset();
		bEnumerationTruncated = false;
		EnumerationCode.Reset();
		TestTrue(
			TEXT("Traversal remains available after rejecting a reparse entry"),
			FHyperAIStudioContextSearchContracts::EnumerateProjectCppSourceFiles(
				ProjectRoot, 64, 128, Enumerated, bEnumerationTruncated, EnumerationCode));
		TestTrue(TEXT("Rejected reparse traversal is explicitly partial"), bEnumerationTruncated);
		TestFalse(TEXT("External symlink target is never enumerated"), Enumerated.ContainsByPredicate([&LinkPath](const FString& Path)
		{
			return Path.StartsWith(LinkPath, ESearchCase::IgnoreCase);
		}));
		TestTrue(TEXT("Directory symlink removed without following it"), ::RemoveDirectoryW(*LinkPath) != 0);
	}
	else
	{
		AddInfo(TEXT("Directory symlink creation was unavailable; Win64 final-handle containment test was skipped best-effort."));
	}

	const FString HardlinkPath = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("ExternalHardlink.cpp"));
	if (::CreateHardLinkW(*HardlinkPath, *OutsidePath, nullptr))
	{
		TestFalse(
			TEXT("Project-local hardlink identity is rejected by containment policy"),
			FHyperAIStudioContextSearchContracts::IsProjectContainedSourcePath(
				ProjectRoot, HardlinkPath, RelativePath));
		TestFalse(
			TEXT("Same-handle source read rejects multiply-linked source identity"),
			FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
				ProjectRoot, HardlinkPath, Read, ReadCode, 4096));
		TestEqual(TEXT("Hardlink rejection has a stable code"), ReadCode, FString(TEXT("source_hardlink_rejected")));
		TestTrue(TEXT("Hardlink fixture removed without touching target"), ::DeleteFileW(*HardlinkPath) != 0);
	}
	else
	{
		AddInfo(TEXT("Hardlink creation was unavailable; Win64 multiply-linked source identity test was skipped best-effort."));
	}
#endif

	const FString InvalidUtf8Path = FPaths::Combine(SourceDirectory, TEXT("InvalidUtf8.cpp"));
	TArray<uint8> InvalidUtf8 = { 0xc0u, 0xafu };
	TestTrue(TEXT("Invalid UTF-8 fixture saved"), FFileHelper::SaveArrayToFile(InvalidUtf8, *InvalidUtf8Path));
	TestFalse(
		TEXT("Malformed UTF-8 source fails closed"),
		FHyperAIStudioContextSearchContracts::ReadProjectContainedCppSource(
			ProjectRoot, InvalidUtf8Path, Read, ReadCode, 4096));
	TestEqual(TEXT("Malformed UTF-8 has stable rejection"), ReadCode, FString(TEXT("invalid_utf8_source")));

	bool bTruncated = false;
	const TArray<FHyperAIContextSearchValueRecord> Symbols =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Hero.cpp"), Source, 16, bTruncated);
	TestFalse(TEXT("Small source is not truncated"), bTruncated);
	TestTrue(TEXT("Class symbol found"), Symbols.ContainsByPredicate([](const FHyperAIContextSearchValueRecord& Record)
	{
		return Record.Fields.ContainsByPredicate([](const FHyperAIReadField& Field)
		{
			return Field.Name == TEXT("name") && Field.Value == TEXT("FHero");
		});
	}));
	for (const FHyperAIContextSearchValueRecord& Record : Symbols)
	{
		for (const FHyperAIReadField& Field : Record.Fields)
		{
			TestFalse(TEXT("No raw source or secret value is exported"), Field.Value.Contains(Secret));
			TestFalse(TEXT("No source body is exported"), Field.Name == TEXT("source") || Field.Name == TEXT("excerpt") || Field.Name == TEXT("signature"));
			if (Field.Name == TEXT("relative_path"))
			{
				TestFalse(TEXT("Indexed source path is never absolute"), FPaths::IsRelative(Field.Value) == false);
			}
		}
	}
	bool bBounded = false;
	const TArray<FHyperAIContextSearchValueRecord> OneSymbol =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Hero.cpp"), Source, 1, bBounded);
	TestEqual(TEXT("Symbol output hard bounded"), OneSymbol.Num(), 1);
	TestTrue(TEXT("Bound truncation explicit"), bBounded);

	const FString DenseNewlines = FString::ChrN(100000, TEXT('\n')) + TEXT("class FAfterDenseNewlines {};");
	bool bDenseTruncated = false;
	const TArray<FHyperAIContextSearchValueRecord> DenseSymbols =
		FHyperAIStudioContextSearchContracts::ExtractCppSymbols(
			TEXT("Source/Game/Dense.cpp"), DenseNewlines, 16, bDenseTruncated);
	TestTrue(TEXT("Newline-dense source reaches the fixed streaming line bound"), bDenseTruncated);
	TestTrue(TEXT("Newline-dense source performs no unbounded line materialization"), DenseSymbols.IsEmpty());

	TestTrue(TEXT("Fixture tree removed"), FileManager.DeleteDirectory(*FixtureBase, false, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchScalarPreviewBoundsTest,
	"HyperAIStudio.NativeTools.ContextSearch.ScalarPreviewBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchScalarPreviewBoundsTest::RunTest(const FString& Parameters)
{
	const FString Exact = FString::ChrN(
		FHyperAIStudioContextSearchContracts::MaxFieldValueCharacters,
		TEXT('x'));
	FString Preview;
	TestTrue(
		TEXT("Exact-bound FString preview is copied"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(Exact, Preview));
	TestEqual(TEXT("Exact-bound preview remains exact"), Preview.Len(), Exact.Len());

	const FString Oversized = Exact + TEXT("x");
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Oversized FString is rejected before copy"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(Oversized, Preview));
	TestTrue(TEXT("Rejected FString produces no preview"), Preview.IsEmpty());

	const FText OversizedText = FText::FromString(Oversized);
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Oversized FText display string is inspected by reference before copy"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedTextPreview(OversizedText, Preview));
	TestTrue(TEXT("Rejected FText produces no preview"), Preview.IsEmpty());

	static const TCHAR EmbeddedNullData[] = TEXT("before\0after");
	const FString EmbeddedNull = FString::ConstructFromPtrSize(
		EmbeddedNullData,
		UE_ARRAY_COUNT(EmbeddedNullData) - 1);
	TestEqual(TEXT("Embedded-null fixture retains the full logical range"), EmbeddedNull.Len(), 12);
	Preview = TEXT("must-clear");
	TestFalse(
		TEXT("Embedded-null scalar preview fails closed"),
		FHyperAIStudioContextSearchContracts::TryCopyBoundedPreview(EmbeddedNull, Preview));
	TestTrue(TEXT("Rejected embedded-null preview is cleared"), Preview.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectLifecycleTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectSearchLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIProjectIndexValueSnapshot Uninitialized;
	Uninitialized.Status = TEXT("uninitialized");
	FHyperAIProjectSearchRequest Request;
	Request.Query = TEXT("hero");
	Request.Fields = { TEXT("name"), TEXT("relative_path"), TEXT("package_name") };
	Request.PageSize = 1;
	Request.bStartIndexIfUnavailable = false;
	const FHyperAIReadReport Missing =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Uninitialized, Request);
	TestFalse(TEXT("Uninitialized index is not a false success"), Missing.bOk);
	TestEqual(TEXT("Uninitialized lifecycle returned"), Missing.Status, FString(TEXT("uninitialized")));
	TestTrue(TEXT("Uninitialized is incomplete"), Missing.bIncomplete);

	FHyperAIProjectIndexValueSnapshot Ready = MakeReadyIndex();
	const FHyperAIReadReport First =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestTrue(TEXT("Ready immutable index searches"), First.bOk);
	TestEqual(TEXT("Both asset and C++ symbol match hero"), First.TotalRecords, 2);
	TestEqual(TEXT("Page bound applied"), First.ReturnedRecords, 1);
	TestFalse(TEXT("Search cursor emitted"), First.NextCursor.IsEmpty());
	Request.Cursor = First.NextCursor;
	const FHyperAIReadReport Second =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestTrue(TEXT("Search continuation succeeds"), Second.bOk);
	TestEqual(TEXT("Continuation returns one"), Second.ReturnedRecords, 1);

	Request.Kinds = { TEXT("raw_file") };
	Request.Cursor.Reset();
	const FHyperAIReadReport UnsafeKind =
		FHyperAIStudioContextSearchContracts::SearchProjectIndex(Ready, Request);
	TestEqual(TEXT("Raw file kind fails closed"), UnsafeKind.Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchProjectIndexSharedViewTest,
	"HyperAIStudio.NativeTools.ContextSearch.ProjectIndexSharedView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchProjectIndexSharedViewTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIProjectIndexValueSnapshot Ready = MakeReadyIndex();
	Ready.Value.Records.Reserve(2048);
	for (int32 Index = Ready.Value.Records.Num(); Index < 2048; ++Index)
	{
		Ready.Value.Records.Add(MakeRecord(
			TEXT("cpp_symbol"),
			*FString::Printf(TEXT("symbol-%04d"), Index),
			*FString::Printf(TEXT("Symbol%04d"), Index),
			TEXT("relative_path"),
			TEXT("Source/Game/Generated.cpp")));
	}
	Ready.SnapshotFingerprint =
		FHyperAIStudioContextSearchContracts::ComputeSnapshotFingerprint(Ready.Value);

	FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
	Store.SetSnapshotForTests(Ready);
	const auto First = Store.GetSnapshotShared();
	const auto Second = Store.GetSnapshotShared();
	TestTrue(TEXT("Unchanged index generation reuses one immutable snapshot"), &First.Get() == &Second.Get());
	TestEqual(TEXT("Shared snapshot retains the complete index"), First->Value.Records.Num(), 2048);

	const double SharedStart = FPlatformTime::Seconds();
	for (int32 Iteration = 0; Iteration < 1000; ++Iteration)
	{
		const auto View = Store.GetSnapshotShared();
		if (View->Value.Records.Num() != 2048)
		{
			AddError(TEXT("Shared index view changed during the read-only micro benchmark."));
			break;
		}
	}
	const double SharedMicroseconds = (FPlatformTime::Seconds() - SharedStart) * 1000000.0 / 1000.0;
	const double CopyStart = FPlatformTime::Seconds();
	for (int32 Iteration = 0; Iteration < 20; ++Iteration)
	{
		const FHyperAIProjectIndexValueSnapshot Copy = Store.GetSnapshot();
		if (Copy.Value.Records.Num() != 2048)
		{
			AddError(TEXT("Copied index changed during the read-only micro benchmark."));
			break;
		}
	}
	const double CopyMicroseconds = (FPlatformTime::Seconds() - CopyStart) * 1000000.0 / 20.0;
	AddInfo(FString::Printf(
		TEXT("Project-index access micro benchmark: shared %.3f us/call; deep-copy %.3f us/call."),
		SharedMicroseconds,
		CopyMicroseconds));

	FHyperAIProjectIndexValueSnapshot Cleared;
	Store.SetSnapshotForTests(Cleared);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchBatchTest,
	"HyperAIStudio.NativeTools.ContextSearch.BatchAllowlistAndSnapshotBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchBatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ContextSearch::Tests;
	FHyperAIContextSearchValueSnapshot Editor;
	Editor.SnapshotUtc = TEXT("2026-08-14T10:00:00.000Z");
	Editor.ObservationScope = TEXT("loaded_editor_context");
	Editor.Records.Add(MakeRecord(TEXT("project"), TEXT("project"), TEXT("Game"), TEXT("project_name"), TEXT("Game")));
	FHyperAIBatchQueryRequest Unsafe;
	FHyperAIBatchReadOperation UnsafeRead;
	UnsafeRead.QueryId = TEXT("unsafe");
	UnsafeRead.Primitive = TEXT("execute_tool");
	Unsafe.Reads.Add(UnsafeRead);
	const FHyperAIReadReport Rejected =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Unsafe);
	TestEqual(TEXT("Raw dispatch primitive rejected"), Rejected.Status, FString(TEXT("invalid_request")));
	TestTrue(TEXT("Allowlist diagnostic returned"), HasDiagnostic(Rejected, TEXT("batch_primitive_not_allowlisted")));

	FHyperAIBatchQueryRequest Batch;
	Batch.PageSize = 2;
	FHyperAIBatchReadOperation Context;
	Context.QueryId = TEXT("context-now");
	Context.Primitive = TEXT("context");
	Context.Fields = { TEXT("project_name") };
	Context.MaxItems = 2;
	Batch.Reads.Add(Context);
	FHyperAIBatchReadOperation Search;
	Search.QueryId = TEXT("find-hero");
	Search.Primitive = TEXT("project_search");
	Search.Query = TEXT("hero");
	Search.Fields = { TEXT("name"), TEXT("package_name"), TEXT("relative_path") };
	Search.MaxItems = 2;
	Batch.Reads.Add(Search);
	const FHyperAIReadReport First =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Batch);
	TestTrue(TEXT("Allowlisted compound read succeeds"), First.bOk);
	TestFalse(TEXT("Batch request fingerprint bound"), First.RequestFingerprint.IsEmpty());
	TestFalse(TEXT("Batch snapshot fingerprint bound"), First.SnapshotFingerprint.IsEmpty());
	for (const FHyperAIReadRecord& Record : First.Records)
	{
		TestTrue(TEXT("Every batch record names its query"), Record.QueryId == TEXT("context-now") || Record.QueryId == TEXT("find-hero"));
	}
	Batch.Reads[0].Fields = { TEXT("project_root") };
	Batch.Cursor = First.NextCursor;
	const FHyperAIReadReport ChangedRequest =
		FHyperAIStudioContextSearchContracts::AnalyzeBatch(Editor, MakeReadyIndex(), Batch);
	TestEqual(TEXT("Changed batch projection invalidates cursor"), ChangedRequest.Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchSceneValueValidationTest,
	"HyperAIStudio.NativeTools.ContextSearch.SceneValueValidationAndZeroEffectPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchSceneValueValidationTest::RunTest(const FString& Parameters)
{
	FHyperAIContextSearchValueSnapshot Snapshot;
	Snapshot.ObservationScope = TEXT("loaded_editor_scene");
	FHyperAIContextSearchValueRecord Actor;
	Actor.Kind = TEXT("actor");
	Actor.RecordId = TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorA");
	Actor.Fields = {
		{TEXT("path"), TEXT("object_path"), Actor.RecordId},
		{TEXT("class_path"), TEXT("class_path"), TEXT("/Script/Engine.Actor")},
		{TEXT("location"), TEXT("vector3"), TEXT("0,0,0")},
		{TEXT("rotation"), TEXT("rotator"), TEXT("0,0,0")},
		{TEXT("scale"), TEXT("vector3"), TEXT("1,1,1")}};
	Snapshot.Records.Add(Actor);
	FHyperAIContextSearchValueRecord Component;
	Component.Kind = TEXT("component");
	Component.RecordId = Actor.RecordId + TEXT(".Root");
	Component.Fields = {
		{TEXT("path"), TEXT("object_path"), Component.RecordId},
		{TEXT("owner_path"), TEXT("object_path"), Actor.RecordId},
		{TEXT("class_path"), TEXT("class_path"), TEXT("/Script/Engine.SceneComponent")}};
	Snapshot.Records.Add(Component);
	bool bTruncated = false;
	const TArray<FHyperAIReadDiagnostic> ValidIssues =
		FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(Snapshot, 16, bTruncated);
	TestFalse(TEXT("closed value snapshot has no validation error"),
		ValidIssues.ContainsByPredicate([](const FHyperAIReadDiagnostic& Issue)
		{
			return Issue.Severity == TEXT("error");
		}));

	Snapshot.Records[1].Fields[1].Value = TEXT("/Game/Maps/Test.Test:PersistentLevel.Missing");
	const TArray<FHyperAIReadDiagnostic> CorruptIssues =
		FHyperAIStudioContextSearchContracts::ValidateSceneSnapshot(Snapshot, 16, bTruncated);
	TestTrue(TEXT("orphan component is independently rejected"),
		CorruptIssues.ContainsByPredicate([](const FHyperAIReadDiagnostic& Issue)
		{
			return Issue.Code == TEXT("component_owner_invalid")
				&& Issue.Severity == TEXT("error");
		}));

	FHyperAISceneApplyPlanRequest InvalidPlan;
	FHyperAISceneLayoutOperation Layout;
	Layout.ActorPaths = {Actor.RecordId, TEXT("/Game/Maps/Test.Test:PersistentLevel.ActorB")};
	Layout.ExpectedActorRevisions = {TEXT("sha256:") + FString::ChrN(64, TEXT('a'))};
	InvalidPlan.Operations.Add(Layout);
	const FHyperAISceneApplyPlanReport InvalidReport =
		UHyperAIStudioSceneInspectToolset::hyper_scene_apply_plan(InvalidPlan);
	TestFalse(TEXT("invalid layout never reports success"), InvalidReport.bOk);
	TestFalse(TEXT("invalid layout never stages"), InvalidReport.bStaged);
	TestFalse(TEXT("invalid layout never submits"), InvalidReport.bExecutionSubmitted);
	TestEqual(TEXT("parallel CAS cardinality is enforced"), InvalidReport.Status,
		FString(TEXT("invalid_operation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIContextSearchToolsetSchemaTest,
	"HyperAIStudio.NativeTools.ContextSearch.ToolsetSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIContextSearchToolsetSchemaTest::RunTest(const FString& Parameters)
{
	const struct { UClass* Class; const TCHAR* Function; } Cases[] = {
		{ UHyperAIStudioBatchQueryToolset::StaticClass(), TEXT("hyper_batch_query") },
		{ UHyperAIStudioContextSnapshotToolset::StaticClass(), TEXT("hyper_context_snapshot") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_inspect") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_apply_plan") },
		{ UHyperAIStudioSceneInspectToolset::StaticClass(), TEXT("hyper_scene_validate") },
		{ UHyperAIStudioProjectSearchToolset::StaticClass(), TEXT("hyper_project_search") },
		{ UHyperAIStudioProjectIndexStatusToolset::StaticClass(), TEXT("hyper_project_index_status") }
	};
	for (const auto& Case : Cases)
	{
		UFunction* Function = Case.Class->FindFunctionByName(Case.Function);
		TestNotNull(TEXT("Tool function exists"), Function);
		if (Function)
		{
			const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(Function);
			TestTrue(TEXT("Tool schema validation has a value"), Callability.HasValue());
			if (Callability.HasValue())
			{
				TestTrue(TEXT("Tool function is AICallable"), Callability.GetValue());
			}
		}
	}
	TestTrue(TEXT("Batch records expose QueryId"), FHyperAIReadRecord::StaticStruct()->FindPropertyByName(TEXT("QueryId")) != nullptr);
	TestTrue(TEXT("Report exposes request fingerprint"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("RequestFingerprint")) != nullptr);
	TestTrue(TEXT("Report exposes snapshot fingerprint"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("SnapshotFingerprint")) != nullptr);
	TestTrue(TEXT("Report exposes incomplete semantics"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("bIncomplete")) != nullptr);
	TestTrue(TEXT("Report exposes on-disk semantics"), FHyperAIReadReport::StaticStruct()->FindPropertyByName(TEXT("bOnDiskOnly")) != nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
