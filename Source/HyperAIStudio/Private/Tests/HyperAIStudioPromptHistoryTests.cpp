// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPromptHistory.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptHistoryTest,
	"HyperAIStudio.Chat.PromptHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPromptHistoryTest::RunTest(const FString& Parameters)
{
	// Operates on a local array, never the CDO, so running the test cannot touch the user's history.
	TArray<FHyperAIStudioPromptHistoryEntry> History;

	TestFalse(TEXT("Empty text is rejected"), UHyperAIStudioPromptHistory::Push(History, FString(), TEXT("Codex")));
	TestFalse(TEXT("Text over the cap is rejected"),
		UHyperAIStudioPromptHistory::Push(History, FString::ChrN(UHyperAIStudioPromptHistory::MaxEntryChars + 1, TEXT('a')), TEXT("Codex")));
	TestEqual(TEXT("Rejected pushes leave history empty"), History.Num(), 0);

	UHyperAIStudioPromptHistory::Push(History, TEXT("first"), TEXT("Codex"));
	UHyperAIStudioPromptHistory::Push(History, TEXT("second"), TEXT("Claude Code"));
	TestEqual(TEXT("Newest entry is first"), History[0].Text, FString(TEXT("second")));
	TestEqual(TEXT("Agent is recorded"), History[0].AgentName, FString(TEXT("Claude Code")));

	UHyperAIStudioPromptHistory::Push(History, TEXT("first"), TEXT("Gemini"));
	TestEqual(TEXT("An exact duplicate is not added twice"), History.Num(), 2);
	TestEqual(TEXT("A duplicate moves to the front"), History[0].Text, FString(TEXT("first")));
	TestEqual(TEXT("A duplicate takes the latest agent"), History[0].AgentName, FString(TEXT("Gemini")));

	UHyperAIStudioPromptHistory::Push(History, TEXT("First"), TEXT("Codex"));
	TestEqual(TEXT("Dedupe is case-sensitive"), History.Num(), 3);

	History.Reset();
	for (int32 Index = 0; Index < UHyperAIStudioPromptHistory::MaxEntries + 5; ++Index)
	{
		UHyperAIStudioPromptHistory::Push(History, FString::Printf(TEXT("prompt %d"), Index), FString());
	}
	TestEqual(TEXT("History is capped"), History.Num(), UHyperAIStudioPromptHistory::MaxEntries);
	TestEqual(TEXT("The oldest entries fall off"), History.Last().Text, FString(TEXT("prompt 5")));
	return true;
}

#endif
