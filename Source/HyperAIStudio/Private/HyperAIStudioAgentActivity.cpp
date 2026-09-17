// Games by Hyper 2026.

#include "HyperAIStudioAgentActivity.h"

namespace HyperAIStudio::Activity::Private
{
	TArray<FHyperAIStudioActivityEntry>& Entries()
	{
		// Newest first, bounded: the panel shows a session, not an archive.
		static TArray<FHyperAIStudioActivityEntry> Storage;
		return Storage;
	}
}

void FHyperAIStudioAgentActivityLog::Record(FHyperAIStudioActivityEntry Entry)
{
	if (!IsInGameThread())
	{
		return;
	}
	if (Entry.Utc == FDateTime::MinValue())
	{
		Entry.Utc = FDateTime::UtcNow();
	}
	TArray<FHyperAIStudioActivityEntry>& Storage = HyperAIStudio::Activity::Private::Entries();
	Storage.Insert(MoveTemp(Entry), 0);
	if (Storage.Num() > MaxEntries)
	{
		Storage.SetNum(MaxEntries);
	}
	OnChanged().Broadcast();
}

TArray<FHyperAIStudioActivityEntry> FHyperAIStudioAgentActivityLog::GetSnapshot()
{
	return HyperAIStudio::Activity::Private::Entries();
}

void FHyperAIStudioAgentActivityLog::Clear()
{
	HyperAIStudio::Activity::Private::Entries().Reset();
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioAgentActivityLog::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

const TCHAR* FHyperAIStudioAgentActivityLog::LexToString(const EHyperAIStudioActivityKind Kind)
{
	switch (Kind)
	{
	case EHyperAIStudioActivityKind::ApprovalRequested: return TEXT("Waiting for approval");
	case EHyperAIStudioActivityKind::ApprovalApproved: return TEXT("Approved");
	case EHyperAIStudioActivityKind::ApprovalRejected: return TEXT("Rejected");
	case EHyperAIStudioActivityKind::Submitted: return TEXT("Running");
	case EHyperAIStudioActivityKind::JobStarted: return TEXT("Started");
	case EHyperAIStudioActivityKind::JobFinished: return TEXT("Finished");
	case EHyperAIStudioActivityKind::Completed: return TEXT("Completed");
	default: return TEXT("Failed");
	}
}
