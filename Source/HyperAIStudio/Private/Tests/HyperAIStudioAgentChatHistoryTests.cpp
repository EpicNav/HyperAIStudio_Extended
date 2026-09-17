// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentChatHistoryTest,
	"HyperAIStudio.Chat.AgentChatHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentChatHistoryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;
	const FString ClaudeId = TEXT("7350db99-a8d7-4f9d-9c85-9b141c4163a6");
	const FString CodexId = TEXT("01a0975b-2c7e-7e72-a9ce-13cfc8c4a95d");

	TestEqual(TEXT("Claude resume flag"), BuildResumeArguments(TEXT("Claude Code"), ClaudeId), TEXT("--resume ") + ClaudeId);
	TestEqual(TEXT("Codex resume subcommand"), BuildResumeArguments(TEXT("Codex"), CodexId), TEXT("resume ") + CodexId);
	TestTrue(TEXT("Gemini is not resumable"), BuildResumeArguments(TEXT("Gemini"), ClaudeId).IsEmpty());
	TestFalse(TEXT("Shell metacharacters are refused"), IsSafeSessionId(TEXT("abcdef12 & del x")));
	TestFalse(TEXT("Short ids are refused"), IsSafeSessionId(TEXT("abc")));
	TestEqual(TEXT("Claude project folder name"), ClaudeProjectDirectoryName(TEXT("E:/Unreal Projects/Aetherfall/")), FString(TEXT("E--Unreal-Projects-Aetherfall")));

	const FString ClaudeLines = FString::Join(TArray<FString>{
		TEXT("{\"type\":\"user\",\"isMeta\":true,\"message\":{\"role\":\"user\",\"content\":\"Caveat: injected\"}}"),
		TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<command-name>/model</command-name>\"}}"),
		TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Fix the knockdown get-up\\nand camera shake\"}]}}"),
		TEXT("{\"type\":\"ai-title\",\"aiTitle\":\"Combat polish\"}"),
		TEXT("{\"type\":\"custom-ti")}, TEXT("\n"));
	TestEqual(TEXT("AI title beats the first prompt; a cut line is ignored"), ExtractClaudeTitle(ClaudeLines), FString(TEXT("Combat polish")));
	TestEqual(TEXT("Custom title beats the AI title"),
		ExtractClaudeTitle(ClaudeLines + TEXT("\n{\"type\":\"custom-title\",\"customTitle\":\"Knockdown pass\"}")), FString(TEXT("Knockdown pass")));
	TestEqual(TEXT("First typed prompt, first line only"),
		ExtractClaudeTitle(ClaudeLines.Left(ClaudeLines.Find(TEXT("{\"type\":\"ai-title\"")))), FString(TEXT("Fix the knockdown get-up")));

	const FString CodexLines = FString::Join(TArray<FString>{
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"# AGENTS.md instructions\"}]}}"),
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"/fast off\"}]}}"),
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"Build the title screen\"}]}}")}, TEXT("\n"));
	TestEqual(TEXT("Codex skips injected and slash turns"), ExtractCodexFirstPrompt(CodexLines), FString(TEXT("Build the title screen")));

	// Fake agent stores, pointed at through the agents' own override variables.
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudioTests/ChatHistory"));
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	const FString Root = TEXT("E:/Some Project");
	const FString OtherRoot = TEXT("E:/Other Project");
	FFileHelper::SaveStringToFile(ClaudeLines, *(Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root) / ClaudeId + TEXT(".jsonl")));
	FFileHelper::SaveStringToFile(TEXT("{\"type\":\"mode\"}"), *(Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root) / TEXT("aaaaaaaa-0000-0000-0000-000000000000.jsonl")));
	auto CodexMeta = [](const FString& Id, const FString& Cwd)
	{
		return FString::Printf(TEXT("{\"type\":\"session_meta\",\"payload\":{\"id\":\"%s\",\"cwd\":\"%s\"}}\n"), *Id, *Cwd.Replace(TEXT("/"), TEXT("\\\\")));
	};
	FFileHelper::SaveStringToFile(CodexMeta(CodexId, Root) + CodexLines, *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-a.jsonl")));
	FFileHelper::SaveStringToFile(CodexMeta(TEXT("bbbbbbbb-0000-0000-0000-000000000000"), OtherRoot) + CodexLines, *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-b.jsonl")));
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"id\":\"%s\",\"thread_name\":\"Title screen UMG\"}\n"), *CodexId), *(Scratch / TEXT("codex/session_index.jsonl")));

	const FString PreviousClaude = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
	const FString PreviousCodex = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *(Scratch / TEXT("claude")));
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *(Scratch / TEXT("codex")));
	const TArray<FHyperAIStudioChatSession> Sessions = ListSessions(Root + TEXT("/"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *PreviousClaude);
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *PreviousCodex);
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);

	TestEqual(TEXT("One chat per agent in this project; empty and foreign sessions are skipped"), Sessions.Num(), 2);
	const FHyperAIStudioChatSession* Claude = Sessions.FindByPredicate([](const FHyperAIStudioChatSession& S) { return S.AgentName == TEXT("Claude Code"); });
	const FHyperAIStudioChatSession* Codex = Sessions.FindByPredicate([](const FHyperAIStudioChatSession& S) { return S.AgentName == TEXT("Codex"); });
	TestTrue(TEXT("Claude chat listed with its title"), Claude && Claude->SessionId == ClaudeId && Claude->Title == TEXT("Combat polish"));
	TestTrue(TEXT("Codex chat listed with its thread name"), Codex && Codex->SessionId == CodexId && Codex->Title == TEXT("Title screen UMG"));
	return true;
}

#endif
