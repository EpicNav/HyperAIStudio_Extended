// Games by Hyper 2026.

#include "HyperAIStudioApprovalGate.h"

#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecutionInternal.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioApproval, Log, All);

namespace HyperAIStudio::Approval::Private
{
	struct FHeldApproval
	{
		FHyperAIStudioPendingApproval Pending;
		FHyperAIStudioTrustedPreparedArtifact Prepared;
	};

	TArray<FHeldApproval>& Held()
	{
		static TArray<FHeldApproval> Storage;
		return Storage;
	}

	int32 IndexOf(const FString& OperationId)
	{
		return Held().IndexOfByPredicate([&OperationId](const FHeldApproval& Item)
		{
			return Item.Pending.OperationId == OperationId;
		});
	}

	FHyperAIStudioActivityEntry MakeEntry(
		const EHyperAIStudioActivityKind Kind,
		const FHyperAIStudioApprovalSummary& Summary,
		const FString& OperationId)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Kind;
		Entry.PackId = Summary.PackId;
		Entry.ToolName = Summary.ToolName;
		Entry.Target = Summary.EffectTarget;
		Entry.OperationId = OperationId;
		return Entry;
	}
}

bool FHyperAIStudioApprovalGate::IsApprovalRequired(const EHyperAIStudioDomainSafety Safety)
{
	if (Safety == EHyperAIStudioDomainSafety::Read)
	{
		return false;
	}
	// Destructive and external-effect variants need a server-issued grant, and that grant exists only
	// as the product of a person clicking Approve.
	if (Safety != EHyperAIStudioDomainSafety::Edit)
	{
		return true;
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return !Settings || Settings->bRequireApprovalForAgentEdits;
}

bool FHyperAIStudioApprovalGate::Request(
	const FHyperAIStudioTrustedPreparedArtifact& Prepared,
	const FString& OperationId,
	const FHyperAIStudioApprovalSummary& Summary,
	FString& OutError)
{
	using namespace HyperAIStudio::Approval::Private;
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Approval requests are queued on the game thread.");
		return false;
	}
	if (!Prepared.IsValid() || OperationId.IsEmpty() || Summary.EffectTarget.IsEmpty())
	{
		OutError = TEXT("An approval request needs a prepared plan, an operation id and an effect target.");
		return false;
	}
	if (IndexOf(OperationId) != INDEX_NONE)
	{
		// A resubmitted operation id is the same intent; the queued plan already covers it.
		return true;
	}
	if (Held().Num() >= MaxPending)
	{
		OutError = TEXT("Too many plans are already waiting for approval; approve or reject some first.");
		return false;
	}

	FHeldApproval Item;
	Item.Pending.OperationId = OperationId;
	Item.Pending.Summary = Summary;
	Item.Pending.RequestedUtc = FDateTime::UtcNow();
	Item.Prepared = Prepared;
	Held().Add(MoveTemp(Item));

	FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalRequested, Summary, OperationId);
	Entry.StatusCode = TEXT("awaiting_user_approval");
	Entry.Detail = Summary.Effects.Num() > 0 ? Summary.Effects[0] : FString();
	FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	OnChanged().Broadcast();
	return true;
}

TArray<FHyperAIStudioPendingApproval> FHyperAIStudioApprovalGate::GetPending()
{
	TArray<FHyperAIStudioPendingApproval> Pending;
	for (const HyperAIStudio::Approval::Private::FHeldApproval& Item : HyperAIStudio::Approval::Private::Held())
	{
		Pending.Add(Item.Pending);
	}
	return Pending;
}

bool FHyperAIStudioApprovalGate::HasPending(const FString& OperationId)
{
	return HyperAIStudio::Approval::Private::IndexOf(OperationId) != INDEX_NONE;
}

bool FHyperAIStudioApprovalGate::Approve(const FString& OperationId, FString& OutError)
{
	using namespace HyperAIStudio::Approval::Private;
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Approvals are granted on the game thread.");
		return false;
	}
	const int32 Index = IndexOf(OperationId);
	if (Index == INDEX_NONE)
	{
		OutError = TEXT("That plan is no longer waiting for approval.");
		return false;
	}
	const FHeldApproval Item = Held()[Index];
	Held().RemoveAt(Index);

	auto Fail = [&Item, &OperationId](const FString& Status, const FString& Diagnostic)
	{
		FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::Failed, Item.Pending.Summary, OperationId);
		Entry.StatusCode = Status;
		Entry.Detail = Diagnostic;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		OnChanged().Broadcast();
	};

	const TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> Host =
		HyperAIStudio::TrustedExecution::Private::GetCoreHost();
	if (!Host.IsValid())
	{
		OutError = TEXT("The trusted execution host is unavailable.");
		Fail(TEXT("host_unavailable"), OutError);
		return false;
	}

	// Staging happens now, not when the agent asked, so the stage lifetime covers only the run itself.
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
	if (!Host->StageExact(Item.Prepared, OperationId, Receipt, Status, OutError))
	{
		if (OutError.IsEmpty()) OutError = Status.Diagnostic;
		Fail(Status.StatusCode.IsEmpty() ? FString(TEXT("stage_failed")) : Status.StatusCode, OutError);
		return false;
	}

	FString AuthorizationToken;
	if (Item.Pending.Summary.Safety != EHyperAIStudioDomainSafety::Edit)
	{
		// The grant is the click: core issues it here and nowhere else.
		if (!HyperAIStudio::TrustedExecution::Private::IssueCoreUiAuthorizationGrant(Receipt, AuthorizationToken, OutError))
		{
			Fail(TEXT("authorization_not_issued"), OutError);
			return false;
		}
	}

	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	if (!Host->SubmitExact(Receipt, AuthorizationToken, Submission, Status, OutError))
	{
		if (OutError.IsEmpty()) OutError = Status.Diagnostic;
		Fail(Status.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : Status.StatusCode, OutError);
		return false;
	}

	FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalApproved, Item.Pending.Summary, OperationId);
	Entry.StatusCode = TEXT("submitted");
	Entry.Detail = TEXT("Approved; the edit is running.");
	FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	UE_LOG(LogHyperAIStudioApproval, Log, TEXT("Approved %s on %s (%s)."),
		*Item.Pending.Summary.ToolName, *Item.Pending.Summary.EffectTarget, *OperationId);
	OnChanged().Broadcast();
	return true;
}

void FHyperAIStudioApprovalGate::Reject(const FString& OperationId, const FString& Reason)
{
	using namespace HyperAIStudio::Approval::Private;
	if (!IsInGameThread())
	{
		return;
	}
	const int32 Index = IndexOf(OperationId);
	if (Index == INDEX_NONE)
	{
		return;
	}
	const FHyperAIStudioApprovalSummary Summary = Held()[Index].Pending.Summary;
	Held().RemoveAt(Index);
	FHyperAIStudioActivityEntry Entry = MakeEntry(EHyperAIStudioActivityKind::ApprovalRejected, Summary, OperationId);
	Entry.StatusCode = TEXT("rejected_by_user");
	Entry.Detail = Reason;
	FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	OnChanged().Broadcast();
}

void FHyperAIStudioApprovalGate::Clear()
{
	HyperAIStudio::Approval::Private::Held().Reset();
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioApprovalGate::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}
