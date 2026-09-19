// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

enum class EHyperAIStudioActivityKind : uint8
{
	ApprovalRequested,
	ApprovalApproved,
	ApprovalRejected,
	Submitted,
	JobStarted,
	JobFinished,
	Completed,
	Failed
};

/** One line of what an agent did, or asked to do, in this editor session. */
struct FHyperAIStudioActivityEntry
{
	FDateTime Utc = FDateTime::MinValue();
	EHyperAIStudioActivityKind Kind = EHyperAIStudioActivityKind::Submitted;
	FString PackId;
	FString ToolName;
	/** Object path of the affected asset, when the entry has one. */
	FString Target;
	FString OperationId;
	FString StatusCode;
	FString Detail;
	/** The chat agent credited with it, and the model that agent ran; empty when that could not be told. */
	FString Agent;
	FString Model;
};

/**
 * Session-scoped record of agent mutations, approvals and long-running jobs, for the chat panel's
 * Activity view. The durable record stays the operation journal; this only adds what the journal
 * deliberately never stores: which tool touched which asset, and which agent asked. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioAgentActivityLog
{
public:
	static constexpr int32 MaxEntries = 200;

	/**
	 * Later entries for an operation inherit its first entry's agent and target, so an approval clicked
	 * minutes later is still credited to the agent that asked. Only an operation's first entry asks the
	 * resolver who is working now.
	 */
	static void Record(FHyperAIStudioActivityEntry Entry);
	/** Newest first. */
	static TArray<FHyperAIStudioActivityEntry> GetSnapshot();
	static void Clear();
	static FSimpleMulticastDelegate& OnChanged();
	static const TCHAR* LexToString(EHyperAIStudioActivityKind Kind);

	/** Names the one agent working right now; returns false when none is, or more than one is. */
	using FAgentResolver = TFunction<bool(FString& OutAgent, FString& OutModel)>;
	/** Returns the resolver it replaces. */
	static FAgentResolver SetAgentResolver(FAgentResolver Resolver);

	/**
	 * " The last agent edit to it was ..." for a stale-revision message, so an agent learns that another tab
	 * changed the asset rather than retrying blind. Empty when this session recorded no completed edit to it.
	 */
	static FString DescribeLastChange(const FString& Target);
};

struct FHyperAIStudioAgentScore
{
	FString Agent;
	FString Model;
	FString PackId;
	int32 Completed = 0;
	int32 Failed = 0;
	/** Plans the user rejected at approval. */
	int32 Rejected = 0;

	int32 Total() const { return Completed + Failed + Rejected; }
};

struct FHyperAIStudioRouteChoice
{
	FString Agent;
	FString Model;
	/** One sentence on why this agent was chosen. */
	FString Reason;
};

/**
 * How each agent and model has done per pack: operations completed, failed, and rejected by the user.
 * Fed by the activity log and kept in Saved/HyperAIStudio/AgentScoreboard.json across sessions. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioAgentScoreboard
{
public:
	/** An agent needs this many operations on a task's packs before its record can choose it. */
	static constexpr int32 MinSamplesToPick = 3;

	/** Most operations first. */
	static TArray<FHyperAIStudioAgentScore> GetScores();
	/** Counts a finished, failed or rejected operation that has an agent and a pack. */
	static void Tally(const FHyperAIStudioActivityEntry& Entry);
	/** One agent and model's totals over these packs, or over every pack when PackIds is empty. */
	static FHyperAIStudioAgentScore Summarize(const FString& Agent, const FString& Model, const TArray<FString>& PackIds);
	/**
	 * FixedAgent when it is usable; otherwise the usable agent and model with the best record on these packs;
	 * otherwise the fallback agent. Records are ranked by (completed + 1) / (total + 2), so a short lucky run
	 * does not beat a long good one.
	 */
	static FHyperAIStudioRouteChoice ChooseAgent(const FString& FixedAgent, const FString& FixedModel,
		const TArray<FString>& PackIds, const TArray<FString>& UsableAgents, const FString& FallbackAgent);
	static void Reset();
	/** Tests point this at a scratch file; empty restores the project's scoreboard. */
	static void SetStoragePathForTests(const FString& Path);
};
