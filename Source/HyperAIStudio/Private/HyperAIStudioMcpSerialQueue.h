// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Process-local endpoint queue for complete MCP sessions. A task owns the endpoint until it
 * invokes its idempotent completion callback. This prevents status probes and deep inventory from
 * overlapping Unreal game-thread tool calls.
 */
class FHyperAIStudioMcpSerialQueue
{
public:
	enum class ECompletionDisposition : uint8
	{
		SafeToRelease,
		AmbiguousTransport
	};

	using FComplete = TFunction<void(ECompletionDisposition)>;
	using FWork = TFunction<void(FComplete)>;
	using FRejected = TFunction<void(const FString& Reason)>;

	/**
	 * Returns a stable HTTP(S) origin identity or an empty string for malformed input.
	 * Path, query and fragment aliases share the same queue because they reach the same MCP
	 * listener. Loopback aliases localhost, 127.0.0.1 and [::1] also share one key.
	 */
	static FString CanonicalizeEndpointKey(const FString& Endpoint);

	/**
	 * Returns false and invokes Rejected when the bounded queue cannot accept the work.
	 * Rejected is also invoked for work that was waiting when the active request becomes
	 * ambiguous, so no caller remains parked indefinitely.
	 */
	static bool Enqueue(const FString& Endpoint, FWork Work, FRejected Rejected = FRejected());

	/**
	 * Clears endpoint state only after the caller has positively established that the old
	 * server/session epoch ended (for example, Epic's in-process server is confirmed stopped).
	 * This is deliberately not part of cache invalidation or an elapsed-time policy.
	 */
	static bool ResetAfterConfirmedServerStop(const FString& Endpoint);

	static bool IsBusy(const FString& Endpoint);
	static bool IsAmbiguouslyLocked(const FString& Endpoint);
};
