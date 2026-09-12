// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioMcpSerialQueue.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMcpSerialQueueTest,
	"HyperAIStudio.NativeTools.McpSerialQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMcpSerialQueueTest::RunTest(const FString& Parameters)
{
	using EDisposition = FHyperAIStudioMcpSerialQueue::ECompletionDisposition;
	const FString Localhost = TEXT(" HTTP://LOCALHOST:064991/mcp?client=a#ignored ");
	const FString Ipv4 = TEXT("http://127.0.0.1:64991/another/path?client=b");
	const FString Ipv6 = TEXT("http://[::1]:64991/");
	const FString ExpectedKey = TEXT("http://127.0.0.1:64991");
	TestEqual(TEXT("localhost canonicalizes to the IPv4 loopback key"),
		FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Localhost), ExpectedKey);
	TestEqual(TEXT("Path and query do not split an origin queue"),
		FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Ipv4), ExpectedKey);
	TestEqual(TEXT("IPv6 loopback shares the same key"),
		FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Ipv6), ExpectedKey);
	TestEqual(TEXT("HTTPS remains a distinct scheme and receives its default port"),
		FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(TEXT("https://localhost/mcp")),
		TEXT("https://127.0.0.1:443"));
	TestTrue(TEXT("Credentials make an endpoint key invalid"),
		FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(TEXT("http://user@localhost:64991/mcp")).IsEmpty());

	int32 ActiveWork = 0;
	int32 MaxActiveWork = 0;
	TArray<int32> StartOrder;
	FHyperAIStudioMcpSerialQueue::FComplete CompleteFirst;
	FHyperAIStudioMcpSerialQueue::FComplete CompleteSecond;
	TestTrue(TEXT("First alias starts immediately"), FHyperAIStudioMcpSerialQueue::Enqueue(Localhost,
		[&](FHyperAIStudioMcpSerialQueue::FComplete Complete)
		{
			++ActiveWork;
			MaxActiveWork = FMath::Max(MaxActiveWork, ActiveWork);
			StartOrder.Add(1);
			CompleteFirst = MoveTemp(Complete);
		}));
	TestTrue(TEXT("Equivalent alias is accepted behind the first"), FHyperAIStudioMcpSerialQueue::Enqueue(Ipv4,
		[&](FHyperAIStudioMcpSerialQueue::FComplete Complete)
		{
			++ActiveWork;
			MaxActiveWork = FMath::Max(MaxActiveWork, ActiveWork);
			StartOrder.Add(2);
			CompleteSecond = MoveTemp(Complete);
		}));
	TestEqual(TEXT("Equivalent endpoints never start concurrently"), StartOrder.Num(), 1);
	TestTrue(TEXT("Every loopback alias sees the same busy queue"), FHyperAIStudioMcpSerialQueue::IsBusy(Ipv6));
	if (!CompleteFirst)
	{
		AddError(TEXT("First serial-queue work item did not provide a completion callback."));
		return false;
	}

	--ActiveWork;
	CompleteFirst(EDisposition::SafeToRelease);
	TestEqual(TEXT("Release starts the next queued alias"), StartOrder.Num(), 2);
	if (StartOrder.Num() > 1)
	{
		TestEqual(TEXT("FIFO order is preserved"), StartOrder[1], 2);
	}
	TestEqual(TEXT("Maximum in-flight work remains one"), MaxActiveWork, 1);
	CompleteFirst(EDisposition::SafeToRelease);
	TestTrue(TEXT("A stale duplicate release cannot release the active successor"), FHyperAIStudioMcpSerialQueue::IsBusy(Ipv4));
	if (!CompleteSecond)
	{
		AddError(TEXT("Second serial-queue work item did not provide a completion callback."));
		return false;
	}

	--ActiveWork;
	CompleteSecond(EDisposition::SafeToRelease);
	TestFalse(TEXT("Queue is removed after its final release"), FHyperAIStudioMcpSerialQueue::IsBusy(Localhost));
	TestEqual(TEXT("All test work has been released"), ActiveWork, 0);

	const FString AmbiguousEndpoint = TEXT("http://localhost:64992/mcp");
	FHyperAIStudioMcpSerialQueue::FComplete CompleteAmbiguous;
	int32 AmbiguousStarts = 0;
	int32 Rejections = 0;
	TestTrue(TEXT("Ambiguous test owner starts"), FHyperAIStudioMcpSerialQueue::Enqueue(AmbiguousEndpoint,
		[&](FHyperAIStudioMcpSerialQueue::FComplete Complete)
		{
			++AmbiguousStarts;
			CompleteAmbiguous = MoveTemp(Complete);
		}));
	TestTrue(TEXT("A waiter can queue before ambiguity is known"), FHyperAIStudioMcpSerialQueue::Enqueue(
		TEXT("http://127.0.0.1:64992/different"),
		[&](FHyperAIStudioMcpSerialQueue::FComplete)
		{
			++AmbiguousStarts;
		},
		[&](const FString& Reason)
		{
			++Rejections;
			TestTrue(TEXT("Ambiguous waiter receives an actionable rejection"), Reason.Contains(TEXT("fail-closed")));
		}));
	if (!CompleteAmbiguous)
	{
		AddError(TEXT("Ambiguous owner did not receive a completion callback."));
		return false;
	}
	CompleteAmbiguous(EDisposition::AmbiguousTransport);
	TestEqual(TEXT("Queued work never starts after an ambiguous outcome"), AmbiguousStarts, 1);
	TestEqual(TEXT("Already queued work is rejected instead of parked forever"), Rejections, 1);
	TestTrue(TEXT("Ambiguous endpoint remains busy without a timer"), FHyperAIStudioMcpSerialQueue::IsBusy(AmbiguousEndpoint));
	TestTrue(TEXT("Ambiguous state is observable"), FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(AmbiguousEndpoint));
	CompleteAmbiguous(EDisposition::SafeToRelease);
	TestTrue(TEXT("A stale duplicate completion cannot clear an ambiguous lock"),
		FHyperAIStudioMcpSerialQueue::IsAmbiguouslyLocked(AmbiguousEndpoint));

	TestFalse(TEXT("Later callers fail fast while ambiguity remains"), FHyperAIStudioMcpSerialQueue::Enqueue(
		TEXT("http://[::1]:64992/mcp?later=true"),
		[&](FHyperAIStudioMcpSerialQueue::FComplete)
		{
			++AmbiguousStarts;
		},
		[&](const FString& Reason)
		{
			++Rejections;
			TestTrue(TEXT("Later rejection explains restart requirement"), Reason.Contains(TEXT("restart")));
		}));
	TestEqual(TEXT("Fail-fast rejection is delivered synchronously"), Rejections, 2);
	TestEqual(TEXT("No caller overlaps the ambiguous owner"), AmbiguousStarts, 1);

	TestTrue(TEXT("Confirmed server epoch reset clears the lock"),
		FHyperAIStudioMcpSerialQueue::ResetAfterConfirmedServerStop(AmbiguousEndpoint));
	TestFalse(TEXT("Reset removes ambiguous busy state"), FHyperAIStudioMcpSerialQueue::IsBusy(AmbiguousEndpoint));
	FHyperAIStudioMcpSerialQueue::FComplete CompleteAfterReset;
	TestTrue(TEXT("Work can start in the confirmed new server epoch"), FHyperAIStudioMcpSerialQueue::Enqueue(
		AmbiguousEndpoint,
		[&](FHyperAIStudioMcpSerialQueue::FComplete Complete)
		{
			++AmbiguousStarts;
			CompleteAfterReset = MoveTemp(Complete);
		}));
	TestEqual(TEXT("Exactly one new-epoch caller starts"), AmbiguousStarts, 2);
	if (!CompleteAfterReset)
	{
		AddError(TEXT("New-epoch owner did not receive a completion callback."));
		return false;
	}
	CompleteAmbiguous(EDisposition::SafeToRelease);
	TestTrue(TEXT("Old-epoch completion cannot release new-epoch ownership"),
		FHyperAIStudioMcpSerialQueue::IsBusy(AmbiguousEndpoint));
	CompleteAfterReset(EDisposition::SafeToRelease);
	TestFalse(TEXT("New-epoch safe completion releases normally"), FHyperAIStudioMcpSerialQueue::IsBusy(AmbiguousEndpoint));
	return true;
}

#endif
