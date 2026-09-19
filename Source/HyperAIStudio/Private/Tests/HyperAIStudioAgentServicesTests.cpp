// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgentState.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioResultPreview.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::ServiceTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Pumps the ticker the job host runs on, with real time passing for deadline checks. */
	void Pump(const int32 Ticks)
	{
		for (int32 Index = 0; Index < Ticks; ++Index)
		{
			FPlatformProcess::Sleep(0.02f);
			FTSTicker::GetCoreTicker().Tick(0.02f);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioActivityLogTest,
	"HyperAIStudio.Chat.ActivityLog",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioActivityLogTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioActivityEntry> Existing = FHyperAIStudioAgentActivityLog::GetSnapshot();
	FHyperAIStudioAgentActivityLog::Clear();

	FHyperAIStudioActivityEntry First;
	First.ToolName = TEXT("first");
	FHyperAIStudioAgentActivityLog::Record(First);
	FHyperAIStudioActivityEntry Second;
	Second.ToolName = TEXT("second");
	Second.Kind = EHyperAIStudioActivityKind::Failed;
	FHyperAIStudioAgentActivityLog::Record(Second);

	TArray<FHyperAIStudioActivityEntry> Snapshot = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestEqual(TEXT("Both entries are kept"), Snapshot.Num(), 2);
	TestEqual(TEXT("Newest first"), Snapshot[0].ToolName, FString(TEXT("second")));
	TestTrue(TEXT("A recorded entry is stamped"), Snapshot[0].Utc > FDateTime::MinValue());

	for (int32 Index = 0; Index < FHyperAIStudioAgentActivityLog::MaxEntries + 10; ++Index)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.ToolName = FString::Printf(TEXT("entry%d"), Index);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
	Snapshot = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestEqual(TEXT("The log is bounded"), Snapshot.Num(), FHyperAIStudioAgentActivityLog::MaxEntries);
	TestEqual(TEXT("Oldest entries fall off"), Snapshot[0].ToolName,
		FString::Printf(TEXT("entry%d"), FHyperAIStudioAgentActivityLog::MaxEntries + 9));

	FHyperAIStudioAgentActivityLog::Clear();
	for (int32 Index = Existing.Num() - 1; Index >= 0; --Index)
	{
		FHyperAIStudioAgentActivityLog::Record(Existing[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioApprovalGateTest,
	"HyperAIStudio.Chat.ApprovalGate",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioApprovalGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bPrevious = Settings->bRequireApprovalForAgentEdits;

	Settings->bRequireApprovalForAgentEdits = true;
	TestFalse(TEXT("Reads never need approval"), FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Read));
	TestTrue(TEXT("Edits need approval while the setting is on"), FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit));
	Settings->bRequireApprovalForAgentEdits = false;
	TestFalse(TEXT("Edits run unattended once the setting is off"), FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit));
	TestTrue(TEXT("Destructive always needs approval: its grant is the click"),
		FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Destructive));
	TestTrue(TEXT("External effects always need approval"),
		FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::ExternalEffect));
	Settings->bRequireApprovalForAgentEdits = bPrevious;

	// An unprepared plan is never queued, and an unknown id is never approved.
	FHyperAIStudioTrustedPreparedArtifact Unprepared;
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = TEXT("niagara_vfx");
	Summary.ToolName = TEXT("hyper_niagara_apply_plan");
	Summary.EffectTarget = TEXT("/Game/VFX/NS_Test.NS_Test");
	FString Error;
	TestFalse(TEXT("An invalid preparation is refused"),
		FHyperAIStudioApprovalGate::Request(Unprepared, TEXT("approval-test-1"), Summary, Error));
	TestFalse(TEXT("The refusal explains itself"), Error.IsEmpty());
	TestFalse(TEXT("Approving an unknown plan fails"), FHyperAIStudioApprovalGate::Approve(TEXT("approval-test-missing"), Error));
	TestFalse(TEXT("Nothing is pending"), FHyperAIStudioApprovalGate::HasPending(TEXT("approval-test-1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAsyncJobHostTest,
	"HyperAIStudio.Chat.AsyncJobHost",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioAsyncJobHostTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ServiceTests;

	FHyperAIStudioAsyncJobRequest Request;
	Request.PackId = TEXT("texture_graph");
	Request.ToolName = TEXT("hyper_texture_graph_apply_plan");
	Request.Target = TEXT("/Game/T/TG_Test.TG_Test");
	Request.DeadlineMs = 5000;
	Request.PollIntervalMs = 50;

	FString JobId;
	FString Error;
	FHyperAIStudioAsyncJobRequest Invalid = Request;
	Invalid.DeadlineMs = FHyperAIStudioAsyncJobHost::MaxDeadlineMs + 1;
	TestFalse(TEXT("An unbounded deadline is refused"),
		FHyperAIStudioAsyncJobHost::Start(Invalid, [](FString&, FString&) { return EHyperAIStudioAsyncJobPoll::Completed; }, JobId, Error));
	Invalid = Request;
	Invalid.PollIntervalMs = 1;
	TestFalse(TEXT("A busy-wait poll interval is refused"),
		FHyperAIStudioAsyncJobHost::Start(Invalid, [](FString&, FString&) { return EHyperAIStudioAsyncJobPoll::Completed; }, JobId, Error));
	TestFalse(TEXT("A job without work is refused"),
		FHyperAIStudioAsyncJobHost::Start(Request, nullptr, JobId, Error));

	// Completes on the third poll, like an export that finishes a few ticks later.
	TSharedRef<int32> Polls = MakeShared<int32>(0);
	TestTrue(*FString::Printf(TEXT("A job starts (%s)"), *Error),
		FHyperAIStudioAsyncJobHost::Start(Request, [Polls](FString& OutProgress, FString& OutDiagnostic)
		{
			OutProgress = TEXT("working");
			if (++(*Polls) < 3)
			{
				return EHyperAIStudioAsyncJobPoll::Running;
			}
			OutDiagnostic = TEXT("done");
			return EHyperAIStudioAsyncJobPoll::Completed;
		}, JobId, Error));

	FHyperAIStudioAsyncJobStatus Status;
	TestTrue(TEXT("The job is queryable"), FHyperAIStudioAsyncJobHost::Query(JobId, Status, Error));
	TestEqual(TEXT("It starts running"), Status.State, EHyperAIStudioAsyncJobState::Running);
	TestTrue(TEXT("It is reported as running"), FHyperAIStudioAsyncJobHost::IsRunning(FString()));

	Pump(12);
	TestTrue(TEXT("The finished job is still queryable"), FHyperAIStudioAsyncJobHost::Query(JobId, Status, Error));
	TestEqual(*FString::Printf(TEXT("It completes (%s)"), *Status.Diagnostic), Status.State, EHyperAIStudioAsyncJobState::Completed);
	TestEqual(TEXT("Its last progress is kept"), Status.Progress, FString(TEXT("working")));
	TestTrue(TEXT("Completion is in the activity log"),
		FHyperAIStudioAgentActivityLog::GetSnapshot().ContainsByPredicate([&](const FHyperAIStudioActivityEntry& Entry)
		{
			return Entry.Kind == EHyperAIStudioActivityKind::JobFinished && Entry.ToolName == TEXT("hyper_texture_graph_apply_plan");
		}));

	// A job that never finishes gives up at its deadline instead of ticking forever.
	FHyperAIStudioAsyncJobRequest Stuck = Request;
	Stuck.DeadlineMs = 120;
	Stuck.ToolName = TEXT("stuck_tool");
	FString StuckId;
	TestTrue(TEXT("The stuck job starts"),
		FHyperAIStudioAsyncJobHost::Start(Stuck, [](FString& OutProgress, FString&)
		{
			OutProgress = TEXT("waiting");
			return EHyperAIStudioAsyncJobPoll::Running;
		}, StuckId, Error));
	Pump(20);
	TestTrue(TEXT("The stuck job is queryable"), FHyperAIStudioAsyncJobHost::Query(StuckId, Status, Error));
	TestEqual(TEXT("It times out"), Status.State, EHyperAIStudioAsyncJobState::TimedOut);
	TestFalse(TEXT("Nothing is left running"), FHyperAIStudioAsyncJobHost::IsRunning(FString()));

	FHyperAIStudioAsyncJobHost::Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUnrealMcpEvidenceTest,
	"HyperAIStudio.Chat.UnrealMcpEvidence",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioUnrealMcpEvidenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ChatHistory;
	TestTrue(TEXT("A called Unreal MCP tool counts"),
		MentionsUnrealMcpCall(TEXT("{\"type\":\"tool_use\",\"name\":\"mcp__unreal-mcp__call_tool\"}")));
	TestTrue(TEXT("A Hyper MCP tool counts"),
		MentionsUnrealMcpCall(TEXT("{\"name\":\"mcp__hyper-ue-mcp__describe_toolset\"}")));
	// The agent is shown a catalogue of tool names inside a prompt string; that is not a call.
	TestFalse(TEXT("A quoted tool catalogue does not count"),
		MentionsUnrealMcpCall(TEXT("{\"text\":\"mcp__unreal_mcp__call_tool\\\",\\\"description\\\":\\\"Call a tool\"}")));
	TestFalse(TEXT("Unrelated text does not count"), MentionsUnrealMcpCall(TEXT("{\"name\":\"Read\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentStateTest,
	"HyperAIStudio.Chat.AgentState",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioAgentStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using FEval = FHyperAIStudioAgentStateEvaluator;
	const TArray<FString> NeedsInput = FEval::DefaultNeedsInputPatterns();
	const TArray<FString> PlanReview = FEval::DefaultPlanReviewPatterns();
	const TArray<FString> Blocked = FEval::DefaultBlockedPatterns();
	auto Evaluate = [&](const FHyperAIStudioAgentStateInputs& Inputs)
	{
		return FEval::EvaluateWithPatterns(Inputs, NeedsInput, PlanReview, Blocked);
	};

	FHyperAIStudioAgentStateInputs Inputs;
	TestEqual(TEXT("No session means stopped"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Stopped);

	// Facts the editor owns outrank whatever the screen happens to show.
	Inputs.bSessionRunning = true;
	Inputs.bApprovalPending = true;
	Inputs.Tail = TEXT("working...");
	TestEqual(TEXT("A plan waiting for approval blocks the tab"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Blocked);
	Inputs.bApprovalPending = false;
	Inputs.bStartupPending = true;
	TestEqual(TEXT("Startup is reported while the CLI boots"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Starting);
	Inputs.bStartupPending = false;

	Inputs.SecondsSinceOutput = 0.2;
	Inputs.Tail = TEXT("Reading files Editing NS_Fire");
	TestEqual(TEXT("Recent output means working"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Working);

	Inputs.SecondsSinceOutput = 30.0;
	TestEqual(TEXT("Silence with no prompt means ready"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Idle);

	// A question outranks working: the agent may still be redrawing its prompt.
	Inputs.SecondsSinceOutput = 0.1;
	Inputs.Tail = TEXT("Edit file NS_Fire.uasset ? 1. Yes 2. No");
	FHyperAIStudioAgentStateSnapshot Snapshot = Evaluate(Inputs);
	TestEqual(TEXT("A choice prompt needs the user"), Snapshot.State, EHyperAIStudioAgentState::NeedsInput);
	TestEqual(TEXT("The matched line is kept as evidence"), Snapshot.Evidence, FString(TEXT("1. Yes")));
	TestTrue(TEXT("Needing input asks for attention"), FEval::WantsAttention(Snapshot.State));

	Inputs.Tail = TEXT("Here is the plan 1. Rework the emitter");
	TestEqual(TEXT("A plan is distinguished from a permission prompt"), Evaluate(Inputs).State, EHyperAIStudioAgentState::PlanReview);

	Inputs.Tail = TEXT("Error: usage limit reached, resets at 4pm");
	TestEqual(TEXT("A usage limit blocks"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Blocked);

	Inputs.Tail = TEXT("status: awaiting_user_approval");
	TestEqual(TEXT("Our own awaiting_user_approval blocks the tab that submitted it"),
		Evaluate(Inputs).State, EHyperAIStudioAgentState::Blocked);

	Inputs.Tail = TEXT("all done, nothing to do");
	TestEqual(TEXT("Ordinary output does not match a pattern"), Evaluate(Inputs).State, EHyperAIStudioAgentState::Working);
	TestFalse(TEXT("Working needs no attention"), FEval::WantsAttention(EHyperAIStudioAgentState::Working));
	TestFalse(TEXT("Idle needs no attention"), FEval::WantsAttention(EHyperAIStudioAgentState::Idle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCohortMismatchTest,
	"HyperAIStudio.Chat.CohortMismatchDiagnostic",
	HyperAIStudio::ServiceTests::Flags)

bool FHyperAIStudioCohortMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using FRuntime = FHyperAIStudioExtensionRuntime;
	// Niagara is in the generated catalog, so its real cohort must report no mismatch at all.
	const TArray<FString> NiagaraTools = {
		TEXT("hyper_niagara_inspect"), TEXT("hyper_niagara_apply_plan"), TEXT("hyper_niagara_validate")};
	const FString Cohort = TEXT("cohort.source.hyperaistudioniagaratoolset.v1");
	TestEqual(TEXT("A catalog-matching cohort reports nothing"),
		FRuntime::DescribeExactGeneratedCohortMismatch(TEXT("niagara_vfx"), Cohort, NiagaraTools), FString());

	const FString UnknownPack = FRuntime::DescribeExactGeneratedCohortMismatch(
		TEXT("texture_graph_not_yet_generated"), Cohort, NiagaraTools);
	TestTrue(TEXT("An absent pack is named"), UnknownPack.Contains(TEXT("texture_graph_not_yet_generated")));
	TestTrue(TEXT("An absent pack says to regenerate"), UnknownPack.Contains(TEXT("Regenerate")));

	TestTrue(TEXT("An unknown cohort is named"),
		FRuntime::DescribeExactGeneratedCohortMismatch(TEXT("niagara_vfx"), TEXT("cohort.does.not.exist.v1"), NiagaraTools)
			.Contains(TEXT("cohort.does.not.exist.v1")));

	// The case that used to vanish silently: one extra tool name in the module.
	TArray<FString> WithExtra = NiagaraTools;
	WithExtra.Add(TEXT("hyper_niagara_topology"));
	const FString ExtraReason = FRuntime::DescribeExactGeneratedCohortMismatch(TEXT("niagara_vfx"), Cohort, WithExtra);
	TestTrue(TEXT("The tool the catalog lacks is named"), ExtraReason.Contains(TEXT("hyper_niagara_topology")));
	TestTrue(TEXT("It says the catalog lacks it"), ExtraReason.Contains(TEXT("the catalog lacks")));

	TArray<FString> Missing = NiagaraTools;
	Missing.RemoveAt(0);
	const FString MissingReason = FRuntime::DescribeExactGeneratedCohortMismatch(TEXT("niagara_vfx"), Cohort, Missing);
	TestTrue(TEXT("The tool the module lacks is named"), MissingReason.Contains(TEXT("hyper_niagara_inspect")));
	TestTrue(TEXT("It says the module lacks it"), MissingReason.Contains(TEXT("the module lacks")));

	TestFalse(TEXT("An empty declaration is refused with a reason"),
		FRuntime::DescribeExactGeneratedCohortMismatch(FString(), Cohort, NiagaraTools).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioResultPreviewTest,
	"HyperAIStudio.Chat.ResultPreview",
	HyperAIStudio::ServiceTests::Flags)
bool FHyperAIStudioResultPreviewTest::RunTest(const FString& Parameters)
{
	const FString Path = FHyperAIStudioResultPreview::GetAssetPreviewPath(TEXT("/Game/FX/M_Fire.M_Fire"));
	const FString PreviewRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Previews"));
	TestTrue(TEXT("Previews live under Saved/HyperAIStudio/Previews"), Path.StartsWith(PreviewRoot));
	TestTrue(TEXT("Previews are PNG files"), Path.EndsWith(TEXT(".png")));
	TestEqual(TEXT("The path is deterministic"),
		FHyperAIStudioResultPreview::GetAssetPreviewPath(TEXT("/Game/FX/M_Fire.M_Fire")), Path);
	TestNotEqual(TEXT("Paths that sanitise alike still get distinct files"),
		FHyperAIStudioResultPreview::GetAssetPreviewPath(TEXT("/Game/A_B.X")),
		FHyperAIStudioResultPreview::GetAssetPreviewPath(TEXT("/Game/A/B.X")));
	TestFalse(TEXT("No path separators leak into the file name"),
		FPaths::GetCleanFilename(Path).Contains(TEXT("/")));

	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("No renderer in this run; skipping the thumbnail render."));
		return true;
	}
	UObject* Cube = LoadObject<UObject>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube loads"), Cube))
	{
		return false;
	}
	FString Written;
	FString Error;
	TestTrue(TEXT("The cube renders to a preview: ") + Error,
		FHyperAIStudioResultPreview::WriteAssetPreview(*Cube, Written, Error));
	TArray<uint8> Bytes;
	TestTrue(TEXT("The preview file exists"), FFileHelper::LoadFileToArray(Bytes, *Written));
	TestTrue(TEXT("The preview is a PNG"), Bytes.Num() > 8 && Bytes[1] == 'P' && Bytes[2] == 'N' && Bytes[3] == 'G');
	IFileManager::Get().Delete(*Written);
	return true;
}

#endif
