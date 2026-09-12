// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

enum class EHyperAIStudioToolDiscoveryMode : uint8
{
	Unknown,
	ToolSearch,
	Eager,
	Degraded
};

struct FHyperAIStudioDiscoveredToolset
{
	FString Name;
	FString Description;
};

struct FHyperAIStudioDescribedTool
{
	FString Name;
	FString Description;
};

struct FHyperAIStudioDescribedToolset
{
	FString Name;
	FString Version;
	FString Description;
	int32 ToolCount = 0;
	TArray<FHyperAIStudioDescribedTool> Tools;
	FString SchemaHash;
};

struct FHyperAIStudioCapabilitySnapshot
{
	EHyperAIStudioToolDiscoveryMode DiscoveryMode = EHyperAIStudioToolDiscoveryMode::Unknown;
	int32 TopLevelToolCount = 0;
	int32 DiscoverableToolsetCount = 0;
	int32 DescribedToolCount = 0;
	int32 DetailedTargetToolsetCount = 0;
	TArray<FString> TopLevelToolNames;
	TArray<FString> MissingDispatchers;
	TArray<FHyperAIStudioDiscoveredToolset> Toolsets;
	TArray<FHyperAIStudioDescribedToolset> DescribedToolsets;
	FString InventoryFingerprint;
	FString NextCursor;
	FDateTime CapturedAtUtc;
	uint64 RefreshGeneration = 0;
	bool bTruncated = false;
	bool bStale = false;
	bool bRefreshing = false;
	bool bDetailedInventoryComplete = false;
	FString ProbeError;
};

/** Bounded pure parsers used by both the runtime inventory and fixture tests. */
class FHyperAIStudioCapabilityInventoryParser
{
public:
	static constexpr int32 MaxResponseBytes = 2 * 1024 * 1024;
	static constexpr int32 MaxSchemaBytes = 512 * 1024;
	static constexpr int32 MaxToolsets = 512;
	static constexpr int32 MaxTopLevelTools = 4096;
	static constexpr int32 MaxToolsPerToolset = 4096;
	static constexpr int32 MaxDetailedTools = 8192;
	static constexpr int32 MaxToolNameBytes = 1024;
	static constexpr int32 MaxToolDescriptionBytes = 32 * 1024;
	static constexpr int32 MaxCanonicalJsonDepth = 64;

	/** Validates HTTP metadata and buffered bytes before callers allocate a response FString. */
	static bool ValidateResponseByteLengths(
		uint64 DeclaredContentBytes,
		int64 BufferedContentBytes,
		FString& OutError);

	/** Parses one raw JSON or SSE response and returns the exact matching JSON-RPC result object. */
	static bool ParseJsonRpcResult(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);

	static bool ParseToolsListResponse(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		FHyperAIStudioCapabilitySnapshot& OutSnapshot,
		FString& OutError);

	static bool ParseListToolsetsResponse(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		TArray<FHyperAIStudioDiscoveredToolset>& OutToolsets,
		bool& bOutTruncated,
		int32& OutMalformedLineCount,
		FString& OutError);

	static bool ParseDescribeToolsetResponse(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		FHyperAIStudioDescribedToolset& OutToolset,
		FString& OutError);

	/** Canonical, recursively key-sorted compact JSON used only for schema identity. */
	static bool CanonicalizeJson(const FString& JsonText, FString& OutCanonicalJson, FString& OutError);

private:
	static bool ParseJsonRpcEnvelope(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		TSharedPtr<FJsonObject>& OutEnvelope,
		FString& OutError);
	static bool ExtractToolText(
		const FString& ResponseBody,
		int64 ExpectedRequestId,
		int32 MaxBytes,
		FString& OutText,
		FString& OutError);
	static bool CanonicalizeValue(
		const TSharedPtr<FJsonValue>& Value,
		int32 Depth,
		FString& OutCanonicalJson,
		FString& OutError);
	static FString HashCanonicalJson(const FString& CanonicalJson);
};
