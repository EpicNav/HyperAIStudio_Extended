// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioCapabilityInventory.h"

enum class EHyperAIStudioDetailedInventoryScope : uint8
{
	/** Describe only non-HyperAI toolsets; HyperAI rows already come from the local exact catalog. */
	EpicToolsets,
	/** Describe every toolset returned by list_toolsets. Intended for explicit diagnostics only. */
	AllToolsets
};

struct FHyperAIStudioCapabilityInventoryResult
{
	bool bSuccess = false;
	bool bFromCache = false;
	bool bStale = false;
	bool bRefreshing = false;
	bool bSuperseded = false;
	bool bSchemaFromCache = false;
	uint64 RefreshGeneration = 0;
	FString Endpoint;
	FString Message;
	FDateTime CompletedUtc;
	FHyperAIStudioCapabilitySnapshot Snapshot;
};

/**
 * Coalesced, serial and cached deep inventory of Epic's local Unreal MCP endpoint.
 * It is diagnostic only: callers must not gate the fast core Ready path on this client.
 */
class FHyperAIStudioCapabilityInventoryClient
{
public:
	using FCompletion = TFunction<void(const FHyperAIStudioCapabilityInventoryResult&)>;

	/** Returns a fresh cached result (successful or failed) without performing I/O. */
	static TOptional<FHyperAIStudioCapabilityInventoryResult> GetCached(
		const FString& Endpoint,
		FTimespan MaxAge = FTimespan::FromSeconds(30));

	/** Coalesces concurrent callers for the same endpoint. All MCP requests execute serially. */
	static void RefreshAsync(const FString& Endpoint, bool bForce, FCompletion OnComplete);

	/** Returns a fresh cached detailed result without performing I/O. */
	static TOptional<FHyperAIStudioCapabilityInventoryResult> GetDetailedCached(
		const FString& Endpoint,
		EHyperAIStudioDetailedInventoryScope Scope = EHyperAIStudioDetailedInventoryScope::EpicToolsets,
		FTimespan MaxAge = FTimespan::FromSeconds(30));

	/** Returns the last exact successful detailed snapshot regardless of age, without performing I/O. */
	static TOptional<FHyperAIStudioCapabilityInventoryResult> GetLastDetailedSuccess(
		const FString& Endpoint,
		EHyperAIStudioDetailedInventoryScope Scope = EHyperAIStudioDetailedInventoryScope::EpicToolsets);

	/**
	 * Explicit user-requested deep inventory. It serially describes every discovered toolset in the
	 * requested scope, reports the exact aggregate DescribedToolCount, and never participates in the
	 * normal Ready/sentinel refresh path. Concurrent identical requests are coalesced and cached.
	 */
	static void RefreshDetailedAsync(
		const FString& Endpoint,
		bool bForce,
		EHyperAIStudioDetailedInventoryScope Scope,
		FCompletion OnComplete);

	static void Invalidate(const FString& Endpoint);

	/** Invalidates only detailed tool inventories, leaving the lightweight Ready-path cache intact. */
	static void InvalidateDetailed(const FString& Endpoint);

	/**
	 * Invalidates inventory and releases a fail-closed serial lock only after the caller has
	 * positively confirmed that the previous Unreal MCP server/session epoch ended.
	 */
	static void NotifyConfirmedServerStopped(const FString& Endpoint);
};
