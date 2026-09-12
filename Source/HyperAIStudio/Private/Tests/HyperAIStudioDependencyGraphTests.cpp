// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDependencyGraphToolset.h"

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::DependencyGraph::Tests
{
	FHyperAIStudioDependencyGraphRequest MakeRequest()
	{
		FHyperAIStudioDependencyGraphRequest Request;
		Request.AssetPath = TEXT("/Game/Test/Root");
		Request.Direction = TEXT("dependencies");
		Request.MaxDepth = 16;
		Request.MaxNodes = 64;
		Request.PageSize = 256;
		return Request;
	}

	FHyperAIStudioDependencyGraphSnapshot MakeSnapshot(bool bRootFound = true)
	{
		FHyperAIStudioDependencyGraphSnapshot Snapshot;
		Snapshot.RootPackage = TEXT("/Game/Test/Root");
		Snapshot.bRootFound = bRootFound;
		Snapshot.bSearchAllAssetsAtStart = true;
		Snapshot.bSearchAllAssetsAtEnd = true;
		Snapshot.bUpstreamRelationAllocationBounded = true;
		return Snapshot;
	}

	FHyperAIStudioDependencyGraphSnapshotEdge MakePackageEdge(
		const TCHAR* Source,
		const TCHAR* Target,
		TArray<FString> Properties = { TEXT("game"), TEXT("hard"), TEXT("not_build") })
	{
		Properties.Sort();
		FHyperAIStudioDependencyGraphSnapshotEdge Edge;
		Edge.SourceIdentifier = Source;
		Edge.TargetIdentifier = Target;
		Edge.Category = TEXT("package");
		Edge.Properties = MoveTemp(Properties);
		Edge.Reason = FString::Printf(TEXT("package[%s]"), *FString::Join(Edge.Properties, TEXT(",")));
		Edge.bReasonAvailable = true;
		return Edge;
	}

	bool HasDiagnostic(const FHyperAIStudioDependencyGraphResult& Result, const TCHAR* Code)
	{
		return Result.Diagnostics.ContainsByPredicate([Code](const FHyperAIStudioDependencyGraphDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
	}

	TArray<FString> RecordIds(const FHyperAIStudioDependencyGraphResult& Result)
	{
		TArray<FString> ResultIds;
		for (const FHyperAIStudioDependencyGraphRecord& Record : Result.Records)
		{
			ResultIds.Add(Record.RecordId);
		}
		return ResultIds;
	}

	bool ArraysEqual(const TArray<FString>& Left, const TArray<FString>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (Left[Index] != Right[Index])
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphCycleAndDeterminismTest,
	"HyperAIStudio.NativeTools.DependencyGraph.CyclesAndDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphCycleAndDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	FHyperAIStudioDependencyGraphSnapshot Snapshot = MakeSnapshot();
	Snapshot.Edges = {
		MakePackageEdge(TEXT("/Game/Test/C"), TEXT("/Game/Test/D")),
		MakePackageEdge(TEXT("/Game/Test/B"), TEXT("/Game/Test/C")),
		MakePackageEdge(TEXT("/Game/Test/C"), TEXT("/Game/Test/Root")),
		MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/B"))
	};
	FHyperAIStudioDependencyGraphSnapshotEdge DuplicateWithoutReason =
		MakePackageEdge(TEXT("/Game/Test/B"), TEXT("/Game/Test/C"));
	DuplicateWithoutReason.Reason = TEXT("less-useful duplicate evidence");
	DuplicateWithoutReason.bReasonAvailable = false;
	Snapshot.Edges.Add(MoveTemp(DuplicateWithoutReason));
	const FHyperAIStudioDependencyGraphRequest Request = MakeRequest();
	const FHyperAIStudioDependencyGraphResult First =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);

	TestEqual(TEXT("Complete fixture is explicitly complete only for on-disk state"),
		First.Status, FString(TEXT("complete_on_disk")));
	TestEqual(TEXT("Observation scope is machine-readable"),
		First.ObservationScope, FString(TEXT("asset_registry_on_disk")));
	TestFalse(TEXT("Unsaved changes are never claimed"), First.bIncludesUnsavedChanges);
		TestTrue(TEXT("complete fixture explicitly supplies a bounded immutable relation snapshot"),
			First.bUpstreamRelationAllocationBounded);
	TestEqual(TEXT("All reachable identifiers are represented"), First.NodeCount, 4);
	TestEqual(TEXT("All direct edges are represented"), First.EdgeCount, 4);
	TestEqual(TEXT("Cycle plus leaf form two SCCs"), First.StronglyConnectedComponentCount, 2);
	TestEqual(TEXT("Exactly one SCC is cyclic"), First.CycleComponentCount, 1);
	TestFalse(TEXT("Cycle analysis does not imply graph truncation"), First.bGraphTruncated);

	TArray<const FHyperAIStudioDependencyGraphRecord*> CycleMembers;
	for (const FHyperAIStudioDependencyGraphRecord& Record : First.Records)
	{
		if (Record.Kind == TEXT("scc_member"))
		{
			CycleMembers.Add(&Record);
		}
	}
	TestEqual(TEXT("Every member of the three-node cycle has SCC evidence"), CycleMembers.Num(), 3);
	if (CycleMembers.Num() == 3)
	{
		TestTrue(TEXT("Cycle members share a stable component id"),
			CycleMembers[0]->ComponentId == CycleMembers[1]->ComponentId
			&& CycleMembers[1]->ComponentId == CycleMembers[2]->ComponentId);
		TestEqual(TEXT("Cycle component size is explicit"), CycleMembers[0]->ComponentSize, 3);
	}
	TestTrue(TEXT("Direct edge evidence includes a reason"),
		First.Records.ContainsByPredicate([](const FHyperAIStudioDependencyGraphRecord& Record)
		{
			return Record.Kind == TEXT("edge")
				&& Record.bReasonAvailable
				&& Record.Relation == TEXT("source_depends_on_target")
				&& Record.Reason == TEXT("package[game,hard,not_build]");
		}));

	Algo::Reverse(Snapshot.Edges);
	const FHyperAIStudioDependencyGraphResult Permuted =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Source ordering does not change the snapshot fingerprint"),
		Permuted.SnapshotFingerprint, First.SnapshotFingerprint);
	TestTrue(TEXT("Source ordering does not change paged record ordering"),
		ArraysEqual(RecordIds(Permuted), RecordIds(First)));

	FHyperAIStudioDependencyGraphSnapshot SelfLoopSnapshot = MakeSnapshot();
	SelfLoopSnapshot.Edges.Add(MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/Root")));
	const FHyperAIStudioDependencyGraphResult SelfLoop =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(SelfLoopSnapshot, Request);
	TestEqual(TEXT("A self-loop is a cycle SCC"), SelfLoop.CycleComponentCount, 1);
	TestTrue(TEXT("A one-node self-loop has explicit SCC membership"),
		SelfLoop.Records.ContainsByPredicate([](const FHyperAIStudioDependencyGraphRecord& Record)
		{
			return Record.Kind == TEXT("scc_member")
				&& Record.Identifier == TEXT("/Game/Test/Root")
				&& Record.ComponentSize == 1
				&& Record.bCycle;
		}));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphPagingAndCursorTest,
	"HyperAIStudio.NativeTools.DependencyGraph.PagingAndCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphPagingAndCursorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	FHyperAIStudioDependencyGraphSnapshot Snapshot = MakeSnapshot();
	Snapshot.Edges = {
		MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/B")),
		MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/C")),
		MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/D"))
	};
	FHyperAIStudioDependencyGraphRequest Request = MakeRequest();
	Request.PageSize = 2;
	const FHyperAIStudioDependencyGraphResult First =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("First page obeys PageSize"), First.ReturnedRecords, 2);
	TestTrue(TEXT("First page has a continuation"), First.bHasMore);
	TestFalse(TEXT("Continuation cursor is present"), First.NextCursor.IsEmpty());
	TestEqual(TEXT("No input cursor has none state"), First.CursorStatus, FString(TEXT("none")));

	Request.Cursor = First.NextCursor;
	const FHyperAIStudioDependencyGraphResult Second =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Valid continuation is accepted"), Second.CursorStatus, FString(TEXT("accepted")));
	TestEqual(TEXT("Second page starts at the prior page end"), Second.PageOffset, 2);
	TestTrue(TEXT("Cursor acceptance is diagnostic"), HasDiagnostic(Second, TEXT("cursor_accepted")));
	if (!First.Records.IsEmpty() && !Second.Records.IsEmpty())
	{
		TestNotEqual(TEXT("Pages do not repeat the first record"),
			Second.Records[0].RecordId, First.Records[0].RecordId);
	}

	FString Tampered = First.NextCursor;
	Tampered[Tampered.Len() - 1] = Tampered[Tampered.Len() - 1] == TEXT('0') ? TEXT('1') : TEXT('0');
	Request.Cursor = Tampered;
	const FHyperAIStudioDependencyGraphResult TamperedResult =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Tampered cursor fails closed"), TamperedResult.Status, FString(TEXT("invalid_request")));
	TestEqual(TEXT("Tampered cursor is rejected"), TamperedResult.CursorStatus, FString(TEXT("rejected")));
	TestEqual(TEXT("Tamper has a precise code"),
		TamperedResult.CursorDiagnosticCode, FString(TEXT("cursor_checksum_mismatch")));
	TestEqual(TEXT("Rejected cursor returns no records"), TamperedResult.ReturnedRecords, 0);

	FHyperAIStudioDependencyGraphRequest ChangedRequest = MakeRequest();
	ChangedRequest.PageSize = 2;
	ChangedRequest.MaxDepth = 15;
	ChangedRequest.Cursor = First.NextCursor;
	const FHyperAIStudioDependencyGraphResult RequestMismatch =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, ChangedRequest);
	TestEqual(TEXT("Cursor is bound to the normalized request"),
		RequestMismatch.CursorDiagnosticCode, FString(TEXT("cursor_request_mismatch")));

	FHyperAIStudioDependencyGraphSnapshot ChangedSnapshot = Snapshot;
	ChangedSnapshot.Edges.Add(MakePackageEdge(TEXT("/Game/Test/D"), TEXT("/Game/Test/E")));
	Request = MakeRequest();
	Request.PageSize = 2;
	Request.Cursor = First.NextCursor;
	const FHyperAIStudioDependencyGraphResult SnapshotMismatch =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(ChangedSnapshot, Request);
	TestEqual(TEXT("Cursor is bound to the graph snapshot"),
		SnapshotMismatch.CursorDiagnosticCode, FString(TEXT("cursor_snapshot_changed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphBoundsTest,
	"HyperAIStudio.NativeTools.DependencyGraph.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphBoundsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	FHyperAIStudioNormalizedDependencyGraphRequest Normalized;
	FString Code;
	FString Message;
	FHyperAIStudioDependencyGraphRequest Request = MakeRequest();
	Request.MaxDepth = FHyperAIStudioDependencyGraphAnalyzer::HardMaxDepth;
	Request.MaxNodes = FHyperAIStudioDependencyGraphAnalyzer::HardMaxNodes;
	Request.PageSize = FHyperAIStudioDependencyGraphAnalyzer::HardMaxPageSize;
	TestTrue(TEXT("Exact hard bounds are valid"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	TestEqual(TEXT("Canonical package root is retained"),
		Normalized.RootPackage, FString(TEXT("/Game/Test/Root")));

	Request = MakeRequest();
	Request.AssetPath = TEXT("/Game/Test/Root.Root");
	TestTrue(TEXT("Canonical /Game object path is valid"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	TestEqual(TEXT("Object path normalizes to its package"),
		Normalized.RootPackage, FString(TEXT("/Game/Test/Root")));

	Request = MakeRequest();
	Request.MaxDepth = 17;
	TestFalse(TEXT("Depth above 16 is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	TestEqual(TEXT("Depth rejection is precise"), Code, FString(TEXT("max_depth_out_of_range")));
	Request = MakeRequest();
	Request.MaxNodes = 4097;
	TestFalse(TEXT("Nodes above 4096 are rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.PageSize = 257;
	TestFalse(TEXT("Pages above 256 are rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));

	Request = MakeRequest();
	Request.AssetPath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
	TestFalse(TEXT("Input root outside /Game is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.AssetPath = TEXT(" /Game/Test/Root");
	TestFalse(TEXT("Noncanonical whitespace is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.Direction = TEXT("recursive");
	TestFalse(TEXT("Unknown direction is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.Categories.Reset();
	TestFalse(TEXT("Empty category allowlist is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.Categories = { TEXT("package"), TEXT("filesystem") };
	TestFalse(TEXT("Unknown category is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.Properties = { TEXT("hard"), TEXT("arbitrary") };
	TestFalse(TEXT("Unknown property is rejected"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));
	Request = MakeRequest();
	Request.Cursor = FString::ChrN(FHyperAIStudioDependencyGraphAnalyzer::HardMaxCursorChars + 1, TEXT('a'));
	TestFalse(TEXT("Overlong cursor is rejected before analysis"),
		FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(Request, Normalized, Code, Message));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphTruncationTest,
	"HyperAIStudio.NativeTools.DependencyGraph.Truncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphTruncationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	FHyperAIStudioDependencyGraphSnapshot Snapshot = MakeSnapshot();
	Snapshot.Edges = {
		MakePackageEdge(TEXT("/Game/Test/Root"), TEXT("/Game/Test/B")),
		MakePackageEdge(TEXT("/Game/Test/B"), TEXT("/Game/Test/C")),
		MakePackageEdge(TEXT("/Game/Test/C"), TEXT("/Game/Test/D"))
	};
	FHyperAIStudioDependencyGraphRequest Request = MakeRequest();
	Request.MaxNodes = 2;
	const FHyperAIStudioDependencyGraphResult NodeBound =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Node bound is enforced"), NodeBound.NodeCount, 2);
	TestTrue(TEXT("Node truncation is explicit"), NodeBound.bNodeLimitReached);
	TestTrue(TEXT("Node truncation makes result partial"), NodeBound.Status == TEXT("partial"));
	TestTrue(TEXT("Node truncation has a diagnostic"), HasDiagnostic(NodeBound, TEXT("max_nodes_reached")));

	Request = MakeRequest();
	Request.MaxDepth = 1;
	const FHyperAIStudioDependencyGraphResult DepthBound =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Depth bound returns only root and first hop"), DepthBound.NodeCount, 2);
	TestTrue(TEXT("Depth truncation is explicit"), DepthBound.bDepthLimitReached);
	TestTrue(TEXT("Depth truncation has a diagnostic"), HasDiagnostic(DepthBound, TEXT("max_depth_reached")));

	Snapshot.bCaptureEdgeLimitReached = true;
	Request = MakeRequest();
	const FHyperAIStudioDependencyGraphResult EdgeBound =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestTrue(TEXT("Capture edge bound survives pure analysis"), EdgeBound.bEdgeLimitReached);
	TestTrue(TEXT("Edge truncation has a diagnostic"), HasDiagnostic(EdgeBound, TEXT("hard_edge_limit_reached")));
	TestTrue(TEXT("Any structural bound marks graph truncated"), EdgeBound.bGraphTruncated);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphHighFanoutSelectionTest,
	"HyperAIStudio.NativeTools.DependencyGraph.HighFanoutSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphHighFanoutSelectionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	TArray<FHyperAIStudioDependencyGraphSnapshotEdge> Fanout;
	Fanout.Reserve(2048);
	for (int32 Index = 2047; Index >= 0; --Index)
	{
		Fanout.Add(MakePackageEdge(
			TEXT("/Game/Test/Root"),
			*FString::Printf(TEXT("/Game/Test/Node_%04d"), Index)));
	}
	bool bFirstTruncated = false;
	const TArray<FHyperAIStudioDependencyGraphSnapshotEdge> First =
		FHyperAIStudioDependencyGraphSnapshotBuilder::SelectDeterministicBoundedEdges(
			Fanout, 64, bFirstTruncated);
	TestTrue(TEXT("High fanout is explicitly truncated"), bFirstTruncated);
	TestEqual(TEXT("High-fanout candidate memory/output is hard-bounded"), First.Num(), 64);
	if (First.Num() == 64)
	{
		TestEqual(TEXT("Bounded selection retains the lexically first edge"),
			First[0].TargetIdentifier, FString(TEXT("/Game/Test/Node_0000")));
		TestEqual(TEXT("Bounded selection retains exactly the first 64 deterministic edges"),
			First.Last().TargetIdentifier, FString(TEXT("/Game/Test/Node_0063")));
	}

	Algo::Reverse(Fanout);
	bool bSecondTruncated = false;
	const TArray<FHyperAIStudioDependencyGraphSnapshotEdge> Second =
		FHyperAIStudioDependencyGraphSnapshotBuilder::SelectDeterministicBoundedEdges(
			Fanout, 64, bSecondTruncated);
	TestTrue(TEXT("Permuted high fanout is still explicitly truncated"), bSecondTruncated);
	TestEqual(TEXT("Permuted high fanout retains the same bounded count"), Second.Num(), First.Num());
	for (int32 Index = 0; Index < First.Num() && Index < Second.Num(); ++Index)
	{
		TestEqual(TEXT("Bounded high-fanout selection is independent of source order"),
			Second[Index].TargetIdentifier, First[Index].TargetIdentifier);
	}

	FHyperAIStudioDependencyGraphSnapshot WorkBoundSnapshot = MakeSnapshot();
	WorkBoundSnapshot.Edges = First;
	WorkBoundSnapshot.bCaptureWorkBudgetReached = true;
	WorkBoundSnapshot.RawRelationRowsObserved = 100000;
	WorkBoundSnapshot.RawRelationRowsProcessed =
		FHyperAIStudioDependencyGraphAnalyzer::HardMaxRawRowsProcessed;
	FHyperAIStudioDependencyGraphRequest WorkBoundRequest = MakeRequest();
	WorkBoundRequest.PageSize = 8;
	const FHyperAIStudioDependencyGraphResult WorkBound =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(WorkBoundSnapshot, WorkBoundRequest);
	TestEqual(TEXT("Capture work-budget exhaustion is partial"),
		WorkBound.Status, FString(TEXT("partial")));
	TestTrue(TEXT("Capture work-budget state is explicit"), WorkBound.bCaptureWorkBudgetReached);
	TestEqual(TEXT("Observed high fanout is reported"), WorkBound.RawRelationRowsObserved, int64(100000));
	TestEqual(TEXT("Processed raw rows stop at the fixed budget"),
		WorkBound.RawRelationRowsProcessed,
		int64(FHyperAIStudioDependencyGraphAnalyzer::HardMaxRawRowsProcessed));
	TestTrue(TEXT("Work-budget exhaustion has a diagnostic"),
		HasDiagnostic(WorkBound, TEXT("capture_work_budget_reached")));
	TestTrue(TEXT("Work-budget fixture has more bounded records than its first page"), WorkBound.bHasMore);
	TestTrue(TEXT("Unstable work-budget capture suppresses its continuation cursor"),
		WorkBound.NextCursor.IsEmpty());
	TestEqual(TEXT("Suppressed continuation has a machine-readable code"),
		WorkBound.CursorDiagnosticCode,
		FString(TEXT("continuation_suppressed_unstable_capture")));
	TestTrue(TEXT("Suppressed continuation has a diagnostic"),
		HasDiagnostic(WorkBound, TEXT("continuation_suppressed_unstable_capture")));

	WorkBoundRequest.Cursor = TEXT("synthetic-prior-cursor");
	const FHyperAIStudioDependencyGraphResult RejectedWorkBoundCursor =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(WorkBoundSnapshot, WorkBoundRequest);
	TestEqual(TEXT("Work-budget partial snapshots reject continuation attempts"),
		RejectedWorkBoundCursor.Status, FString(TEXT("invalid_request")));
	TestEqual(TEXT("Work-budget cursor rejection is precise"),
		RejectedWorkBoundCursor.CursorDiagnosticCode,
		FString(TEXT("cursor_unstable_work_budget")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDependencyGraphIncompleteRegistryTest,
	"HyperAIStudio.NativeTools.DependencyGraph.IncompleteRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioDependencyGraphIncompleteRegistryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DependencyGraph::Tests;
	const FHyperAIStudioDependencyGraphRequest Request = MakeRequest();
	FHyperAIStudioDependencyGraphSnapshot Snapshot = MakeSnapshot();
	Snapshot.bRegistryGatheringAtStart = true;
	Snapshot.bRegistryGatheringAtEnd = true;
	Snapshot.bQueryIncomplete = true;
	const FHyperAIStudioDependencyGraphResult Indexing =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Loading registry is never reported complete"), Indexing.Status, FString(TEXT("partial")));
	TestEqual(TEXT("Loading registry has indexing state"), Indexing.RegistryStatus, FString(TEXT("indexing")));
	TestTrue(TEXT("Loading registry explicitly reports incomplete"), Indexing.bIncomplete);
	TestTrue(TEXT("Loading registry has indexing diagnostic"), HasDiagnostic(Indexing, TEXT("registry_indexing")));
	TestTrue(TEXT("Incomplete query has a diagnostic"), HasDiagnostic(Indexing, TEXT("registry_query_incomplete")));

	Snapshot = MakeSnapshot();
	Snapshot.bSearchAllAssetsAtStart = false;
	Snapshot.bSearchAllAssetsAtEnd = false;
	const FHyperAIStudioDependencyGraphResult SearchNotObserved =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Missing SearchAllAssets evidence is partial"),
		SearchNotObserved.Status, FString(TEXT("partial")));
	TestTrue(TEXT("Missing SearchAllAssets evidence is incomplete"), SearchNotObserved.bIncomplete);
	TestTrue(TEXT("Missing SearchAllAssets evidence has a diagnostic"),
		HasDiagnostic(SearchNotObserved, TEXT("registry_full_search_not_observed")));

	Snapshot = MakeSnapshot();
	Snapshot.bSearchAllAssetsAtEnd = false;
	const FHyperAIStudioDependencyGraphResult SearchBoundaryChanged =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("SearchAllAssets must be observed at both capture boundaries"),
		SearchBoundaryChanged.Status, FString(TEXT("partial")));

	Snapshot = MakeSnapshot();
	Snapshot.bUpstreamRelationAllocationBounded = false;
	const FHyperAIStudioDependencyGraphResult RawRelationBackendMissing =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("raw UE relation arrays can never certify a complete graph"),
		RawRelationBackendMissing.Status, FString(TEXT("partial")));
	TestTrue(TEXT("bounded relation-cache requirement is machine readable"),
		HasDiagnostic(RawRelationBackendMissing, TEXT("async_dependency_index_backend_required")));

	Snapshot = MakeSnapshot();
	Snapshot.DirtyLoadedPackages = { TEXT("/Game/Test/Root") };
	const FHyperAIStudioDependencyGraphResult DirtyPackage =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Dirty package does not weaken the explicit on-disk completeness claim"),
		DirtyPackage.Status, FString(TEXT("complete_on_disk")));
	TestEqual(TEXT("Dirty loaded package evidence is counted"), DirtyPackage.DirtyLoadedPackageCount, 1);
	TestTrue(TEXT("Dirty loaded package evidence is returned"),
		DirtyPackage.DirtyLoadedPackages.Contains(TEXT("/Game/Test/Root")));
	TestTrue(TEXT("Unsaved exclusion is diagnosed"),
		HasDiagnostic(DirtyPackage, TEXT("dirty_loaded_packages_excluded")));

	Snapshot = MakeSnapshot(false);
	Snapshot.bRegistryGatheringAtStart = true;
	const FHyperAIStudioDependencyGraphResult UnknownRoot =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Missing root during indexing is partial, not definitive not-found"),
		UnknownRoot.Status, FString(TEXT("partial")));
	TestTrue(TEXT("Missing root during indexing is explained"),
		HasDiagnostic(UnknownRoot, TEXT("root_unconfirmed_while_incomplete")));

	Snapshot = MakeSnapshot(false);
	Snapshot.bQueryIncomplete = true;
	FHyperAIStudioDependencyGraphDiagnostic& InMemoryOnlyDiagnostic =
		Snapshot.Diagnostics.AddDefaulted_GetRef();
	InMemoryOnlyDiagnostic.Code = TEXT("root_in_memory_only_unconfirmed_on_disk");
	InMemoryOnlyDiagnostic.Severity = TEXT("warning");
	InMemoryOnlyDiagnostic.Field = TEXT("asset_path");
	InMemoryOnlyDiagnostic.Message = TEXT("Fixture root exists only in UObject-derived state.");
	const FHyperAIStudioDependencyGraphResult InMemoryOnlyRoot =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("An in-memory-only root is partial, never complete_on_disk"),
		InMemoryOnlyRoot.Status, FString(TEXT("partial")));
	TestTrue(TEXT("An in-memory-only root stays explicitly unconfirmed"),
		InMemoryOnlyRoot.bIncomplete);
	TestTrue(TEXT("An in-memory-only root carries its specific on-disk exclusion diagnostic"),
		HasDiagnostic(InMemoryOnlyRoot, TEXT("root_in_memory_only_unconfirmed_on_disk")));

	Snapshot = MakeSnapshot(false);
	const FHyperAIStudioDependencyGraphResult MissingRoot =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request);
	TestEqual(TEXT("Missing root in a ready registry is not-found"),
		MissingRoot.Status, FString(TEXT("not_found")));
	TestFalse(TEXT("Definitive not-found is not mislabeled incomplete"), MissingRoot.bIncomplete);

	Snapshot = MakeSnapshot();
	FHyperAIStudioDependencyGraphRequest ReferencerRequest = MakeRequest();
	ReferencerRequest.Direction = TEXT("referencers");
	const FHyperAIStudioDependencyGraphResult NoReferencers =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, ReferencerRequest);
	TestEqual(TEXT("No known referencers can still be a complete observation"),
		NoReferencers.Status, FString(TEXT("complete_on_disk")));
	TestEqual(TEXT("No known referencers leaves only the root node"), NoReferencers.NodeCount, 1);
	TestEqual(TEXT("No known referencers is never a deletion assessment"),
		NoReferencers.DeletionAssessment, FString(TEXT("not_performed")));
	TestTrue(TEXT("Delete-safety limitation is machine-readable"),
		HasDiagnostic(NoReferencers, TEXT("no_delete_inference")));

	Snapshot.Edges.Add(MakePackageEdge(TEXT("/Game/Test/Referrer"), TEXT("/Game/Test/Root")));
	const FHyperAIStudioDependencyGraphResult OneReferencer =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, ReferencerRequest);
	TestEqual(TEXT("Referencer traversal follows incoming canonical edges"), OneReferencer.NodeCount, 2);
	TestTrue(TEXT("Referencer evidence retains canonical source and target"),
		OneReferencer.Records.ContainsByPredicate([](const FHyperAIStudioDependencyGraphRecord& Record)
		{
			return Record.Kind == TEXT("edge")
				&& Record.SourceIdentifier == TEXT("/Game/Test/Referrer")
				&& Record.TargetIdentifier == TEXT("/Game/Test/Root");
		}));
	FHyperAIStudioDependencyGraphRequest NonMatchingProperty = ReferencerRequest;
	NonMatchingProperty.Properties = { TEXT("direct") };
	const FHyperAIStudioDependencyGraphResult Filtered =
		FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, NonMatchingProperty);
	TestEqual(TEXT("Property allowlist filters nonmatching edges before traversal"), Filtered.NodeCount, 1);
	TestEqual(TEXT("Property allowlist leaves no matching edge"), Filtered.EdgeCount, 0);

	return true;
}

#endif
