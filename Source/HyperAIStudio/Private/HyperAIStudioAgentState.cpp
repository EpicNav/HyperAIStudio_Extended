// Games by Hyper 2026.

#include "HyperAIStudioAgentState.h"

#include "HyperAIStudioSettings.h"
#include "Internationalization/Internationalization.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioAgentState"

namespace HyperAIStudio::AgentState::Private
{
	/** Case-insensitive, and only over the last rows, so an old prompt further up the scrollback cannot match. */
	bool MatchesAny(const FString& Tail, const TArray<FString>& Patterns, FString& OutEvidence)
	{
		for (const FString& Pattern : Patterns)
		{
			const FString Trimmed = Pattern.TrimStartAndEnd();
			if (!Trimmed.IsEmpty() && Tail.Contains(Trimmed, ESearchCase::IgnoreCase))
			{
				OutEvidence = Trimmed;
				return true;
			}
		}
		return false;
	}
}

FHyperAIStudioAgentStateSnapshot FHyperAIStudioAgentStateEvaluator::Evaluate(const FHyperAIStudioAgentStateInputs& Inputs)
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return EvaluateWithPatterns(
		Inputs,
		Settings && !Settings->AgentNeedsInputPatterns.IsEmpty() ? Settings->AgentNeedsInputPatterns : DefaultNeedsInputPatterns(),
		Settings && !Settings->AgentPlanReviewPatterns.IsEmpty() ? Settings->AgentPlanReviewPatterns : DefaultPlanReviewPatterns(),
		Settings && !Settings->AgentBlockedPatterns.IsEmpty() ? Settings->AgentBlockedPatterns : DefaultBlockedPatterns());
}

FHyperAIStudioAgentStateSnapshot FHyperAIStudioAgentStateEvaluator::EvaluateWithPatterns(
	const FHyperAIStudioAgentStateInputs& Inputs,
	const TArray<FString>& NeedsInputPatterns,
	const TArray<FString>& PlanReviewPatterns,
	const TArray<FString>& BlockedPatterns)
{
	using namespace HyperAIStudio::AgentState::Private;
	FHyperAIStudioAgentStateSnapshot Snapshot;

	// Facts the editor owns come first; the screen is only consulted for what it alone can tell.
	if (!Inputs.bSessionRunning)
	{
		Snapshot.State = EHyperAIStudioAgentState::Stopped;
		Snapshot.Label = LOCTEXT("Stopped", "stopped");
		return Snapshot;
	}
	if (Inputs.bApprovalPending)
	{
		Snapshot.State = EHyperAIStudioAgentState::Blocked;
		Snapshot.Label = LOCTEXT("AwaitingApproval", "needs approval");
		Snapshot.Evidence = TEXT("A plan from this tab is waiting in the Activity panel.");
		return Snapshot;
	}
	if (Inputs.bStartupPending)
	{
		Snapshot.State = EHyperAIStudioAgentState::Starting;
		Snapshot.Label = LOCTEXT("Starting", "starting");
		return Snapshot;
	}

	// Claude's plan prompt also reads like a permission prompt ("Would you like to", "1. Yes"), so it is
	// recognised by its approval options before the generic question patterns get a chance to claim it.
	const FHyperAIStudioPlanApprovalChoice PlanChoice = FindPlanAutoApprovalChoice(Inputs.Tail);
	if (PlanChoice.Digit != INDEX_NONE)
	{
		Snapshot.State = EHyperAIStudioAgentState::PlanReview;
		Snapshot.Label = LOCTEXT("PlanReview", "plan to review");
		Snapshot.Evidence = PlanChoice.Line;
		return Snapshot;
	}

	FString Evidence;
	if (MatchesAny(Inputs.Tail, NeedsInputPatterns, Evidence))
	{
		Snapshot.State = EHyperAIStudioAgentState::NeedsInput;
		Snapshot.Label = LOCTEXT("NeedsInput", "needs you");
		Snapshot.Evidence = Evidence;
		return Snapshot;
	}
	if (MatchesAny(Inputs.Tail, PlanReviewPatterns, Evidence))
	{
		Snapshot.State = EHyperAIStudioAgentState::PlanReview;
		Snapshot.Label = LOCTEXT("PlanReview", "plan to review");
		Snapshot.Evidence = Evidence;
		return Snapshot;
	}
	if (MatchesAny(Inputs.Tail, BlockedPatterns, Evidence))
	{
		Snapshot.State = EHyperAIStudioAgentState::Blocked;
		Snapshot.Label = LOCTEXT("Blocked", "blocked");
		Snapshot.Evidence = Evidence;
		return Snapshot;
	}

	if (Inputs.SecondsSinceOutput >= 0.0 && Inputs.SecondsSinceOutput < WorkingWindowSeconds)
	{
		Snapshot.State = EHyperAIStudioAgentState::Working;
		Snapshot.Label = LOCTEXT("Working", "working");
		return Snapshot;
	}
	Snapshot.State = EHyperAIStudioAgentState::Idle;
	Snapshot.Label = LOCTEXT("Idle", "ready");
	return Snapshot;
}

FHyperAIStudioPlanApprovalChoice FHyperAIStudioAgentStateEvaluator::FindPlanAutoApprovalChoice(const FString& Tail)
{
	FHyperAIStudioPlanApprovalChoice AutoMode;
	FHyperAIStudioPlanApprovalChoice AutoAccept;
	TArray<FString> Lines;
	Tail.ParseIntoArrayLines(Lines);
	for (const FString& Raw : Lines)
	{
		// Options sit inside a box border, and the highlighted one carries Claude's selection cursor.
		FString Line = Raw.TrimStartAndEnd();
		while (!Line.IsEmpty() && (Line[0] == TEXT('|') || Line[0] == 0x2502))
		{
			Line = Line.Mid(1).TrimStart();
		}
		bool bCursor = false;
		if (!Line.IsEmpty() && (Line[0] == 0x276F || Line[0] == 0x203A || Line[0] == TEXT('>')))
		{
			bCursor = true;
			Line = Line.Mid(1).TrimStart();
		}
		if (Line.Len() < 4 || !FChar::IsDigit(Line[0]) || Line[1] != TEXT('.'))
		{
			continue;
		}
		const FString Option = Line.Mid(2).TrimStart();
		if (!Option.StartsWith(TEXT("Yes"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FHyperAIStudioPlanApprovalChoice* Target =
			Option.Contains(TEXT("auto mode"), ESearchCase::IgnoreCase) ? &AutoMode
			: Option.Contains(TEXT("auto-accept"), ESearchCase::IgnoreCase) ? &AutoAccept
			: nullptr;
		if (Target && Target->Digit == INDEX_NONE)
		{
			Target->Digit = Line[0] - TEXT('0');
			Target->bCursorOnChoice = bCursor;
			Target->Line = Line;
		}
	}
	return AutoMode.Digit != INDEX_NONE ? AutoMode : AutoAccept;
}

const TCHAR* FHyperAIStudioAgentStateEvaluator::LexToString(const EHyperAIStudioAgentState State)
{
	switch (State)
	{
	case EHyperAIStudioAgentState::Stopped: return TEXT("stopped");
	case EHyperAIStudioAgentState::Starting: return TEXT("starting");
	case EHyperAIStudioAgentState::Working: return TEXT("working");
	case EHyperAIStudioAgentState::NeedsInput: return TEXT("needs_input");
	case EHyperAIStudioAgentState::PlanReview: return TEXT("plan_review");
	case EHyperAIStudioAgentState::Blocked: return TEXT("blocked");
	default: return TEXT("idle");
	}
}

bool FHyperAIStudioAgentStateEvaluator::WantsAttention(const EHyperAIStudioAgentState State)
{
	return State == EHyperAIStudioAgentState::NeedsInput
		|| State == EHyperAIStudioAgentState::PlanReview
		|| State == EHyperAIStudioAgentState::Blocked;
}

TArray<FString> FHyperAIStudioAgentStateEvaluator::DefaultNeedsInputPatterns()
{
	// Wording as the three CLIs print it today. Edit these in settings when a CLI changes its prompts.
	return {
		TEXT("Do you want to proceed?"),
		TEXT("Do you want to continue?"),
		TEXT("Would you like to"),
		TEXT("Allow command"),
		TEXT("Allow this"),
		TEXT("Apply this change"),
		TEXT("Press Enter to continue"),
		TEXT("(y/n)"),
		TEXT("(y/N)"),
		TEXT("[y/N]"),
		TEXT("1. Yes"),
		TEXT("Yes, and don't ask again"),
		TEXT("Choose an option")
	};
}

TArray<FString> FHyperAIStudioAgentStateEvaluator::DefaultPlanReviewPatterns()
{
	return {
		TEXT("Ready to code?"),
		TEXT("proceed with this plan"),
		TEXT("Here is the plan"),
		TEXT("Approve plan")
	};
}

TArray<FString> FHyperAIStudioAgentStateEvaluator::DefaultBlockedPatterns()
{
	return {
		TEXT("usage limit"),
		TEXT("rate limit"),
		TEXT("quota exceeded"),
		TEXT("Please run /login"),
		TEXT("authentication failed"),
		TEXT("project_execution_busy"),
		TEXT("awaiting_user_approval")
	};
}

#undef LOCTEXT_NAMESPACE
