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
};

/**
 * Session-scoped record of agent mutations, approvals and long-running jobs, for the chat panel's
 * Activity view. The durable record stays the operation journal; this only adds what the journal
 * deliberately never stores: which tool touched which asset. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioAgentActivityLog
{
public:
	static constexpr int32 MaxEntries = 200;

	static void Record(FHyperAIStudioActivityEntry Entry);
	/** Newest first. */
	static TArray<FHyperAIStudioActivityEntry> GetSnapshot();
	static void Clear();
	static FSimpleMulticastDelegate& OnChanged();
	static const TCHAR* LexToString(EHyperAIStudioActivityKind Kind);
};
