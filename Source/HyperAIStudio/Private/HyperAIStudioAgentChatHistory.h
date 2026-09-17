// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** One past agent conversation that ran in the project root, where the project's MCP config connects it to Unreal. */
struct FHyperAIStudioChatSession
{
	/** Built-in agent route name: "Claude Code" or "Codex". */
	FString AgentName;
	FString SessionId;
	FString Title;
	FDateTime LastActiveUtc;
	/** An Unreal MCP tool call was found in the part of the log that was read. */
	bool bUsedUnrealMcp = false;
};

/**
 * Reads the agents' own session stores; nothing is copied or persisted by the plugin.
 * Claude Code: <CLAUDE_CONFIG_DIR or ~/.claude>/projects/<root with non-alphanumerics as '-'>/<id>.jsonl.
 * Codex: <CODEX_HOME or ~/.codex>/sessions/**\/rollout-*.jsonl whose session_meta cwd is the root; titles from
 * session_index.jsonl. Gemini only resumes by list index, which shifts, so it is not offered.
 * Safe to call off the game thread.
 */
namespace HyperAIStudio::ChatHistory
{
	TArray<FHyperAIStudioChatSession> ListSessions(const FString& ProjectRoot, int32 MaxSessions = 50);

	/** CLI arguments that resume the session, e.g. "--resume <id>"; empty when the id or agent is not resumable. */
	FString BuildResumeArguments(const FString& AgentName, const FString& SessionId);

	bool IsSafeSessionId(const FString& SessionId);
	/** True when the text records a call to an Unreal or Hyper MCP tool, not merely a tool listing. */
	bool MentionsUnrealMcpCall(const FString& JsonlText);
	FString ClaudeProjectDirectoryName(const FString& ProjectRoot);
	/** Title from Claude Code jsonl text: last custom title, else last AI title, else the first typed prompt. */
	FString ExtractClaudeTitle(const FString& JsonlText);
	/** First typed prompt from Codex jsonl text, skipping injected AGENTS.md, environment and slash-command turns. */
	FString ExtractCodexFirstPrompt(const FString& JsonlText);
}
