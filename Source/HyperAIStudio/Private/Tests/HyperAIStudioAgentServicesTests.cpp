// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
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

#endif
