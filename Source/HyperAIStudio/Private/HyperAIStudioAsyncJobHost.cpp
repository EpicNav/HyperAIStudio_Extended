// Games by Hyper 2026.

#include "HyperAIStudioAsyncJobHost.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "Misc/Guid.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAsyncJob, Log, All);

namespace HyperAIStudio::AsyncJob::Private
{
	constexpr int32 MaxFinishedKept = 16;

	struct FJob
	{
		FHyperAIStudioAsyncJobStatus Status;
		FHyperAIStudioAsyncJobHost::FPoll Poll;
		FTSTicker::FDelegateHandle TickerHandle;
		int64 DeadlineMs = 0;
		double StartedSeconds = 0.0;
	};

	TArray<TSharedPtr<FJob>>& Jobs()
	{
		static TArray<TSharedPtr<FJob>> Storage;
		return Storage;
	}

	int32 CountRunning()
	{
		int32 Count = 0;
		for (const TSharedPtr<FJob>& Job : Jobs())
		{
			Count += Job.IsValid() && Job->Status.State == EHyperAIStudioAsyncJobState::Running ? 1 : 0;
		}
		return Count;
	}

	void TrimFinished()
	{
		int32 Finished = 0;
		for (int32 Index = 0; Index < Jobs().Num();)
		{
			const bool bDone = Jobs()[Index].IsValid() && Jobs()[Index]->Status.State != EHyperAIStudioAsyncJobState::Running;
			if (bDone && ++Finished > MaxFinishedKept)
			{
				Jobs().RemoveAt(Index);
				continue;
			}
			++Index;
		}
	}

	void Finish(const TSharedPtr<FJob>& Job, const EHyperAIStudioAsyncJobState State, const FString& Diagnostic)
	{
		Job->Status.State = State;
		Job->Status.Diagnostic = Diagnostic;
		if (Job->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Job->TickerHandle);
			Job->TickerHandle.Reset();
		}
		// Dropping the poll releases whatever the pack captured, on the game thread.
		Job->Poll = nullptr;

		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = State == EHyperAIStudioAsyncJobState::Completed
			? EHyperAIStudioActivityKind::JobFinished : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = Job->Status.PackId;
		Entry.ToolName = Job->Status.ToolName;
		Entry.Target = Job->Status.Target;
		Entry.OperationId = Job->Status.OperationId;
		Entry.StatusCode = FHyperAIStudioAsyncJobHost::LexToString(State);
		Entry.Detail = Diagnostic;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		UE_LOG(LogHyperAIStudioAsyncJob, Log, TEXT("Job %s (%s) %s after %lld ms. %s"),
			*Job->Status.JobId, *Job->Status.ToolName, FHyperAIStudioAsyncJobHost::LexToString(State),
			Job->Status.ElapsedMs, *Diagnostic);
		TrimFinished();
		FHyperAIStudioAsyncJobHost::OnChanged().Broadcast();
	}
}

bool FHyperAIStudioAsyncJobHost::Start(
	const FHyperAIStudioAsyncJobRequest& Request,
	FPoll Poll,
	FString& OutJobId,
	FString& OutError)
{
	using namespace HyperAIStudio::AsyncJob::Private;
	OutJobId.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Jobs start on the game thread.");
		return false;
	}
	if (!Poll || Request.PackId.IsEmpty() || Request.ToolName.IsEmpty())
	{
		OutError = TEXT("A job needs a pack, a tool name and a poll.");
		return false;
	}
	if (Request.DeadlineMs <= 0 || Request.DeadlineMs > MaxDeadlineMs || Request.PollIntervalMs < MinPollIntervalMs)
	{
		OutError = TEXT("The job deadline or poll interval is outside the allowed bounds.");
		return false;
	}
	if (CountRunning() >= MaxConcurrentJobs)
	{
		OutError = TEXT("Too many jobs are already running.");
		return false;
	}

	const TSharedPtr<FJob> Job = MakeShared<FJob>();
	Job->Status.JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Job->Status.PackId = Request.PackId;
	Job->Status.ToolName = Request.ToolName;
	Job->Status.Target = Request.Target;
	Job->Status.OperationId = Request.OperationId;
	Job->Status.StartedUtc = FDateTime::UtcNow();
	Job->Status.Progress = TEXT("started");
	Job->Poll = MoveTemp(Poll);
	Job->DeadlineMs = Request.DeadlineMs;
	Job->StartedSeconds = FPlatformTime::Seconds();

	const float Interval = Request.PollIntervalMs / 1000.0f;
	TWeakPtr<FJob> WeakJob = Job;
	Job->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakJob](float) -> bool
		{
			const TSharedPtr<FJob> Pinned = WeakJob.Pin();
			if (!Pinned.IsValid() || Pinned->Status.State != EHyperAIStudioAsyncJobState::Running || !Pinned->Poll)
			{
				return false;
			}
			const double PollStarted = FPlatformTime::Seconds();
			Pinned->Status.ElapsedMs = static_cast<int64>((PollStarted - Pinned->StartedSeconds) * 1000.0);

			FString Progress;
			FString Diagnostic;
			const EHyperAIStudioAsyncJobPoll Result = Pinned->Poll(Progress, Diagnostic);
			const int64 PollMs = static_cast<int64>((FPlatformTime::Seconds() - PollStarted) * 1000.0);
			if (!Progress.IsEmpty() && Progress != Pinned->Status.Progress)
			{
				Pinned->Status.Progress = Progress;
				OnChanged().Broadcast();
			}
			if (PollMs > MaxPollGameThreadMs)
			{
				Finish(Pinned, EHyperAIStudioAsyncJobState::Failed,
					FString::Printf(TEXT("A poll took %lld ms, over the %d ms budget; the job was stopped."), PollMs, MaxPollGameThreadMs));
				return false;
			}
			if (Result == EHyperAIStudioAsyncJobPoll::Completed)
			{
				Finish(Pinned, EHyperAIStudioAsyncJobState::Completed, Diagnostic);
				return false;
			}
			if (Result == EHyperAIStudioAsyncJobPoll::Failed)
			{
				Finish(Pinned, EHyperAIStudioAsyncJobState::Failed, Diagnostic);
				return false;
			}
			if (Pinned->Status.ElapsedMs >= Pinned->DeadlineMs)
			{
				Finish(Pinned, EHyperAIStudioAsyncJobState::TimedOut,
					FString::Printf(TEXT("Still running after %lld ms; the work may finish on its own, but nothing here waits for it."),
						Pinned->Status.ElapsedMs));
				return false;
			}
			return true;
		}), Interval);

	Jobs().Add(Job);
	TrimFinished();

	FHyperAIStudioActivityEntry Entry;
	Entry.Kind = EHyperAIStudioActivityKind::JobStarted;
	Entry.PackId = Request.PackId;
	Entry.ToolName = Request.ToolName;
	Entry.Target = Request.Target;
	Entry.OperationId = Request.OperationId;
	Entry.StatusCode = TEXT("running");
	FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));

	OutJobId = Job->Status.JobId;
	OnChanged().Broadcast();
	return true;
}

bool FHyperAIStudioAsyncJobHost::Query(const FString& JobId, FHyperAIStudioAsyncJobStatus& OutStatus, FString& OutError)
{
	OutStatus = FHyperAIStudioAsyncJobStatus{};
	OutError.Reset();
	for (const TSharedPtr<HyperAIStudio::AsyncJob::Private::FJob>& Job : HyperAIStudio::AsyncJob::Private::Jobs())
	{
		if (Job.IsValid() && Job->Status.JobId == JobId)
		{
			OutStatus = Job->Status;
			return true;
		}
	}
	OutError = TEXT("No job with that id is known to this editor session.");
	return false;
}

TArray<FHyperAIStudioAsyncJobStatus> FHyperAIStudioAsyncJobHost::GetSnapshot()
{
	TArray<FHyperAIStudioAsyncJobStatus> Snapshot;
	for (const TSharedPtr<HyperAIStudio::AsyncJob::Private::FJob>& Job : HyperAIStudio::AsyncJob::Private::Jobs())
	{
		if (Job.IsValid())
		{
			Snapshot.Add(Job->Status);
		}
	}
	Snapshot.Sort([](const FHyperAIStudioAsyncJobStatus& A, const FHyperAIStudioAsyncJobStatus& B)
	{
		const bool bARunning = A.State == EHyperAIStudioAsyncJobState::Running;
		const bool bBRunning = B.State == EHyperAIStudioAsyncJobState::Running;
		return bARunning != bBRunning ? bARunning : A.StartedUtc > B.StartedUtc;
	});
	return Snapshot;
}

bool FHyperAIStudioAsyncJobHost::IsRunning(const FString& OperationId)
{
	for (const TSharedPtr<HyperAIStudio::AsyncJob::Private::FJob>& Job : HyperAIStudio::AsyncJob::Private::Jobs())
	{
		if (Job.IsValid() && Job->Status.State == EHyperAIStudioAsyncJobState::Running
			&& (OperationId.IsEmpty() || Job->Status.OperationId == OperationId))
		{
			return true;
		}
	}
	return false;
}

void FHyperAIStudioAsyncJobHost::Shutdown()
{
	for (const TSharedPtr<HyperAIStudio::AsyncJob::Private::FJob>& Job : HyperAIStudio::AsyncJob::Private::Jobs())
	{
		if (Job.IsValid() && Job->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Job->TickerHandle);
			Job->TickerHandle.Reset();
			Job->Poll = nullptr;
		}
	}
	HyperAIStudio::AsyncJob::Private::Jobs().Reset();
}

FSimpleMulticastDelegate& FHyperAIStudioAsyncJobHost::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

const TCHAR* FHyperAIStudioAsyncJobHost::LexToString(const EHyperAIStudioAsyncJobState State)
{
	switch (State)
	{
	case EHyperAIStudioAsyncJobState::Running: return TEXT("running");
	case EHyperAIStudioAsyncJobState::Completed: return TEXT("completed");
	case EHyperAIStudioAsyncJobState::TimedOut: return TEXT("timed_out");
	default: return TEXT("failed");
	}
}
