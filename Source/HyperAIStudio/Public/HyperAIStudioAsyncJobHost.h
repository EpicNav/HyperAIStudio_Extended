// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

enum class EHyperAIStudioAsyncJobState : uint8
{
	Running,
	Completed,
	Failed,
	TimedOut
};

/** What one poll of a job reports. A poll must return promptly; it never blocks or waits. */
enum class EHyperAIStudioAsyncJobPoll : uint8
{
	Running,
	Completed,
	Failed
};

struct FHyperAIStudioAsyncJobRequest
{
	FString PackId;
	FString ToolName;
	/** Object path this job works on, shown in the Activity panel. */
	FString Target;
	/** Operation this job belongs to, when it follows a journaled mutation. */
	FString OperationId;
	/** Bounded: a job that outlives this is reported timed out, never silently abandoned. */
	int32 DeadlineMs = 60 * 1000;
	int32 PollIntervalMs = 250;
};

struct FHyperAIStudioAsyncJobStatus
{
	FString JobId;
	FString PackId;
	FString ToolName;
	FString Target;
	FString OperationId;
	EHyperAIStudioAsyncJobState State = EHyperAIStudioAsyncJobState::Running;
	FString Progress;
	FString Diagnostic;
	FDateTime StartedUtc = FDateTime::MinValue();
	int64 ElapsedMs = 0;
};

/**
 * Runs work that spans many editor ticks and can be watched: a compile, an export, a render.
 *
 * The typed-artifact executor deliberately runs one bounded step per tick and cannot wait, so work
 * that finishes later used to be started and forgotten. A job keeps that work observable: a poll on
 * the game thread every PollIntervalMs, a hard deadline, live progress for the Activity panel, and a
 * status any tool can report back to the agent.
 *
 * Game thread only. The poll runs inside a ticker, so it must do no blocking work of its own.
 */
class HYPERAISTUDIO_API FHyperAIStudioAsyncJobHost
{
public:
	static constexpr int32 MaxConcurrentJobs = 8;
	static constexpr int32 MaxDeadlineMs = 10 * 60 * 1000;
	static constexpr int32 MinPollIntervalMs = 50;
	/** A single poll that runs longer than this is a bug in the pack, and the job fails closed. */
	static constexpr int32 MaxPollGameThreadMs = 100;

	using FPoll = TFunction<EHyperAIStudioAsyncJobPoll(FString& OutProgress, FString& OutDiagnostic)>;

	static bool Start(const FHyperAIStudioAsyncJobRequest& Request, FPoll Poll, FString& OutJobId, FString& OutError);
	static bool Query(const FString& JobId, FHyperAIStudioAsyncJobStatus& OutStatus, FString& OutError);
	/** Running jobs first, then the most recent finished ones. */
	static TArray<FHyperAIStudioAsyncJobStatus> GetSnapshot();
	/** True while any job for this operation, or any job at all when the id is empty, is running. */
	static bool IsRunning(const FString& OperationId);
	static void Shutdown();

	static FSimpleMulticastDelegate& OnChanged();
	static const TCHAR* LexToString(EHyperAIStudioAsyncJobState State);
};
