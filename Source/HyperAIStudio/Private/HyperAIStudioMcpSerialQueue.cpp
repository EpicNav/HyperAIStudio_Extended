// Games by Hyper 2026.

#include "HyperAIStudioMcpSerialQueue.h"

#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::McpSerialQueue::Private
{
	constexpr int32 MaxPendingPerEndpoint = 16;
	constexpr int32 MaxEndpointQueues = 32;
	const FString AmbiguousLockReason = TEXT(
		"Unreal MCP calls are fail-closed because an earlier request has an unknown transport outcome. "
		"Confirm an Unreal MCP server restart before retrying.");

	struct FQueuedWork
	{
		FGuid Token;
		FHyperAIStudioMcpSerialQueue::FWork Work;
		FHyperAIStudioMcpSerialQueue::FRejected Rejected;
	};

	struct FEndpointQueue
	{
		FGuid ActiveToken;
		TArray<FQueuedWork> Pending;
		bool bAmbiguouslyLocked = false;
	};

	FCriticalSection Mutex;
	TMap<FString, FEndpointQueue> Queues;

	void Start(const FString& Endpoint, FQueuedWork&& Item);

	bool ParsePort(const FString& PortText, int32& OutPort)
	{
		if (PortText.IsEmpty())
		{
			return false;
		}
		int64 Port = 0;
		for (const TCHAR Character : PortText)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
			Port = Port * 10 + (Character - TEXT('0'));
			if (Port > 65535)
			{
				return false;
			}
		}
		if (Port <= 0)
		{
			return false;
		}
		OutPort = static_cast<int32>(Port);
		return true;
	}

	void Complete(
		const FString& Endpoint,
		const FGuid Token,
		const FHyperAIStudioMcpSerialQueue::ECompletionDisposition Disposition)
	{
		TOptional<FQueuedWork> Next;
		TArray<FQueuedWork> Rejected;
		{
			FScopeLock Lock(&Mutex);
			FEndpointQueue* Queue = Queues.Find(Endpoint);
			if (!Queue || Queue->ActiveToken != Token)
			{
				return;
			}
			if (Queue->bAmbiguouslyLocked)
			{
				return;
			}
			if (Disposition == FHyperAIStudioMcpSerialQueue::ECompletionDisposition::AmbiguousTransport)
			{
				Queue->bAmbiguouslyLocked = true;
				Rejected = MoveTemp(Queue->Pending);
				Queue->Pending.Reset();
			}
			else if (Queue->Pending.IsEmpty())
			{
				Queues.Remove(Endpoint);
				return;
			}
			else
			{
				Next = MoveTemp(Queue->Pending[0]);
				Queue->Pending.RemoveAt(0, 1, EAllowShrinking::No);
				Queue->ActiveToken = Next->Token;
			}
		}
		for (FQueuedWork& Item : Rejected)
		{
			if (Item.Rejected)
			{
				Item.Rejected(AmbiguousLockReason);
			}
		}
		if (Next.IsSet())
		{
			Start(Endpoint, MoveTemp(Next.GetValue()));
		}
	}

	void Start(const FString& Endpoint, FQueuedWork&& Item)
	{
		FHyperAIStudioMcpSerialQueue::FWork Work = MoveTemp(Item.Work);
		const FGuid Token = Item.Token;
		Work([Endpoint, Token](const FHyperAIStudioMcpSerialQueue::ECompletionDisposition Disposition)
		{
			Complete(Endpoint, Token, Disposition);
		});
	}
}

FString FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(const FString& Endpoint)
{
	FString Value = Endpoint.TrimStartAndEnd();
	int32 SchemeSeparator = INDEX_NONE;
	if (!Value.FindChar(TEXT(':'), SchemeSeparator)
		|| !Value.Mid(SchemeSeparator, 3).Equals(TEXT("://"), ESearchCase::CaseSensitive))
	{
		return FString();
	}

	FString Scheme = Value.Left(SchemeSeparator).ToLower();
	if (Scheme != TEXT("http") && Scheme != TEXT("https"))
	{
		return FString();
	}
	FString Remainder = Value.Mid(SchemeSeparator + 3);
	int32 SuffixStart = Remainder.Len();
	const TCHAR Delimiters[] = { TEXT('/'), TEXT('?'), TEXT('#') };
	for (const TCHAR Delimiter : Delimiters)
	{
		const int32 Index = Remainder.Find(FString::Chr(Delimiter), ESearchCase::CaseSensitive);
		if (Index != INDEX_NONE)
		{
			SuffixStart = FMath::Min(SuffixStart, Index);
		}
	}
	const FString Authority = Remainder.Left(SuffixStart);
	if (Authority.IsEmpty() || Authority.Contains(TEXT("@")))
	{
		return FString();
	}

	FString Host;
	FString PortText;
	if (Authority.StartsWith(TEXT("[")))
	{
		int32 ClosingBracket = INDEX_NONE;
		if (!Authority.FindChar(TEXT(']'), ClosingBracket) || ClosingBracket <= 1)
		{
			return FString();
		}
		Host = Authority.Mid(1, ClosingBracket - 1);
		const FString AfterHost = Authority.Mid(ClosingBracket + 1);
		if (!AfterHost.IsEmpty())
		{
			if (!AfterHost.StartsWith(TEXT(":")) || AfterHost.Len() <= 1)
			{
				return FString();
			}
			PortText = AfterHost.Mid(1);
		}
	}
	else
	{
		int32 ColonIndex = INDEX_NONE;
		if (Authority.FindLastChar(TEXT(':'), ColonIndex))
		{
			if (Authority.Left(ColonIndex).Contains(TEXT(":")))
			{
				return FString();
			}
			Host = Authority.Left(ColonIndex);
			PortText = Authority.Mid(ColonIndex + 1);
		}
		else
		{
			Host = Authority;
		}
	}

	Host = Host.ToLower();
	if (Host.EndsWith(TEXT(".")))
	{
		Host.LeftChopInline(1, EAllowShrinking::No);
	}
	if (Host == TEXT("localhost")
		|| Host == TEXT("127.0.0.1")
		|| Host == TEXT("::1")
		|| Host == TEXT("0:0:0:0:0:0:0:1"))
	{
		Host = TEXT("127.0.0.1");
	}
	if (Host.IsEmpty())
	{
		return FString();
	}

	int32 Port = Scheme == TEXT("http") ? 80 : 443;
	if (!PortText.IsEmpty() && !HyperAIStudio::McpSerialQueue::Private::ParsePort(PortText, Port))
	{
		return FString();
	}
	if (Authority.EndsWith(TEXT(":")))
	{
		return FString();
	}

	const FString CanonicalHost = Host.Contains(TEXT(":")) ? FString::Printf(TEXT("[%s]"), *Host) : Host;
	return FString::Printf(TEXT("%s://%s:%d"), *Scheme, *CanonicalHost, Port);
}

bool FHyperAIStudioMcpSerialQueue::Enqueue(const FString& Endpoint, FWork Work, FRejected Rejected)
{
	const FString EndpointKey = CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty() || !Work)
	{
		if (Rejected)
		{
			Rejected(TEXT("The Unreal MCP endpoint or queued work is invalid."));
		}
		return false;
	}

	HyperAIStudio::McpSerialQueue::Private::FQueuedWork Item;
	Item.Token = FGuid::NewGuid();
	Item.Work = MoveTemp(Work);
	Item.Rejected = MoveTemp(Rejected);
	bool bStartNow = false;
	FRejected RejectNow;
	FString RejectReason;
	{
		FScopeLock Lock(&HyperAIStudio::McpSerialQueue::Private::Mutex);
		HyperAIStudio::McpSerialQueue::Private::FEndpointQueue* Queue =
			HyperAIStudio::McpSerialQueue::Private::Queues.Find(EndpointKey);
		if (!Queue
			&& HyperAIStudio::McpSerialQueue::Private::Queues.Num()
				>= HyperAIStudio::McpSerialQueue::Private::MaxEndpointQueues)
		{
			RejectNow = MoveTemp(Item.Rejected);
			RejectReason = TEXT("The bounded serial Unreal MCP endpoint queue limit is reached.");
		}
		else
		{
			if (!Queue)
			{
				Queue = &HyperAIStudio::McpSerialQueue::Private::Queues.Add(
					EndpointKey,
					HyperAIStudio::McpSerialQueue::Private::FEndpointQueue());
			}
			if (Queue->bAmbiguouslyLocked)
			{
				RejectNow = MoveTemp(Item.Rejected);
				RejectReason = HyperAIStudio::McpSerialQueue::Private::AmbiguousLockReason;
			}
			else if (!Queue->ActiveToken.IsValid())
			{
				Queue->ActiveToken = Item.Token;
				bStartNow = true;
			}
			else if (Queue->Pending.Num() >= HyperAIStudio::McpSerialQueue::Private::MaxPendingPerEndpoint)
			{
				RejectNow = MoveTemp(Item.Rejected);
				RejectReason = TEXT("The bounded serial Unreal MCP queue is full.");
			}
			else
			{
				Queue->Pending.Add(MoveTemp(Item));
			}
		}
	}
	if (!RejectReason.IsEmpty())
	{
		if (RejectNow)
		{
			RejectNow(RejectReason);
		}
		return false;
	}
	if (bStartNow)
	{
		HyperAIStudio::McpSerialQueue::Private::Start(EndpointKey, MoveTemp(Item));
	}
	return true;
}

bool FHyperAIStudioMcpSerialQueue::ResetAfterConfirmedServerStop(const FString& Endpoint)
{
	const FString EndpointKey = CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty())
	{
		return false;
	}

	TArray<HyperAIStudio::McpSerialQueue::Private::FQueuedWork> Rejected;
	{
		FScopeLock Lock(&HyperAIStudio::McpSerialQueue::Private::Mutex);
		HyperAIStudio::McpSerialQueue::Private::FEndpointQueue* Queue =
			HyperAIStudio::McpSerialQueue::Private::Queues.Find(EndpointKey);
		if (!Queue)
		{
			return true;
		}
		Rejected = MoveTemp(Queue->Pending);
		HyperAIStudio::McpSerialQueue::Private::Queues.Remove(EndpointKey);
	}
	for (HyperAIStudio::McpSerialQueue::Private::FQueuedWork& Item : Rejected)
	{
		if (Item.Rejected)
		{
			Item.Rejected(TEXT("Unreal MCP work was cancelled because the previous server was confirmed stopped."));
		}
	}
	return true;
}

bool FHyperAIStudioMcpSerialQueue::IsBusy(const FString& Endpoint)
{
	const FString EndpointKey = CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty())
	{
		return false;
	}
	FScopeLock Lock(&HyperAIStudio::McpSerialQueue::Private::Mutex);
	return HyperAIStudio::McpSerialQueue::Private::Queues.Contains(EndpointKey);
}

bool FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(const FString& Endpoint)
{
	const FString EndpointKey = CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty())
	{
		return false;
	}
	FScopeLock Lock(&HyperAIStudio::McpSerialQueue::Private::Mutex);
	const HyperAIStudio::McpSerialQueue::Private::FEndpointQueue* Queue =
		HyperAIStudio::McpSerialQueue::Private::Queues.Find(EndpointKey);
	return Queue && Queue->bAmbiguouslyLocked;
}
