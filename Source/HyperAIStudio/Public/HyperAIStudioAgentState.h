// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** What a tab's agent is doing, as far as the editor can tell. */
enum class EHyperAIStudioAgentState : uint8
{
	/** No terminal session; nothing is running. */
	Stopped,
	/** The launch command was sent; the CLI has not settled yet. */
	Starting,
	/** Producing output right now. */
	Working,
	/** Stopped at a question: a permission prompt, a choice, a confirmation. */
	NeedsInput,
	/** Showing a plan and waiting to be told to go ahead. */
	PlanReview,
	/** Cannot proceed: a rate limit, an auth failure, or a plan waiting for your approval. */
	Blocked,
	/** Quiet with a prompt: it finished, or it is waiting for you to say something. */
	Idle
};

struct FHyperAIStudioAgentStateInputs
{
	bool bSessionRunning = false;
	/** The agent launch command has been sent but the CLI has produced nothing yet. */
	bool bStartupPending = false;
	/** A plan from this tab is waiting for approval in the Activity panel. */
	bool bApprovalPending = false;
	/** Seconds since the terminal last produced output; negative when it never has. */
	double SecondsSinceOutput = -1.0;
	/** The last few rows of the agent's screen. */
	FString Tail;
};

/** A numbered plan-approval option that hands building straight to the agent, as found on its screen. */
struct FHyperAIStudioPlanApprovalChoice
{
	/** The option's number, or INDEX_NONE when no such prompt is showing. */
	int32 Digit = INDEX_NONE;
	/** The agent's selection cursor is on this option. */
	bool bCursorOnChoice = false;
	/** The option as shown, for the tooltip and the activity log. */
	FString Line;
};

struct FHyperAIStudioAgentStateSnapshot
{
	EHyperAIStudioAgentState State = EHyperAIStudioAgentState::Stopped;
	/** Short label for the tab, e.g. "needs you". */
	FText Label;
	/** The line that decided it, for the tooltip. Empty when the state came from the session, not the screen. */
	FString Evidence;
};

/**
 * Derives a tab's agent state from what the terminal shows plus what the editor knows.
 *
 * Only some of this is certain. The session running, the startup being in flight, output arriving and a
 * plan waiting for approval are facts the editor owns. Telling a permission prompt from a plan review
 * means reading the agent's own screen, which is a heuristic: the CLIs change their wording. Those
 * patterns therefore live in settings, so a wording change is an edit, not a rebuild.
 */
class HYPERAISTUDIO_API FHyperAIStudioAgentStateEvaluator
{
public:
	/** Seconds of silence after which an agent counts as idle rather than working. */
	static constexpr double WorkingWindowSeconds = 2.0;
	static constexpr int32 TailRows = 14;

	static FHyperAIStudioAgentStateSnapshot Evaluate(const FHyperAIStudioAgentStateInputs& Inputs);
	/** Evaluate against explicit pattern lists instead of the user's settings. */
	static FHyperAIStudioAgentStateSnapshot EvaluateWithPatterns(
		const FHyperAIStudioAgentStateInputs& Inputs,
		const TArray<FString>& NeedsInputPatterns,
		const TArray<FString>& PlanReviewPatterns,
		const TArray<FString>& BlockedPatterns);

	/**
	 * Claude Code's plan approval offers "Yes, and use auto mode", or "Yes, and auto-accept edits" where auto
	 * mode is unavailable. Returns that option, preferring auto mode. Permission prompts offer neither.
	 */
	static FHyperAIStudioPlanApprovalChoice FindPlanAutoApprovalChoice(const FString& Tail);

	static const TCHAR* LexToString(EHyperAIStudioAgentState State);
	/** True while the state is one a person has to act on. */
	static bool WantsAttention(EHyperAIStudioAgentState State);

	static TArray<FString> DefaultNeedsInputPatterns();
	static TArray<FString> DefaultPlanReviewPatterns();
	static TArray<FString> DefaultBlockedPatterns();
};
