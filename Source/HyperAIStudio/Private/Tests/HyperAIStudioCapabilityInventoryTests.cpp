// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioCapabilityInventory.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::CapabilityInventory::Tests
{
	FString MakeToolCallResponse(const FString& Text, const int64 RequestId = 2)
	{
		TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("type"), TEXT("text"));
		Content->SetStringField(TEXT("text"), Text);
		TArray<TSharedPtr<FJsonValue>> ContentArray = { MakeShared<FJsonValueObject>(Content) };

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("content"), ContentArray);
		Result->SetBoolField(TEXT("isError"), false);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetNumberField(TEXT("id"), RequestId);
		Root->SetObjectField(TEXT("result"), Result);

		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}

	FString MakeToolsListResponse(const TArray<FString>& Names, const FString& NextCursor = FString())
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FString& Name : Names)
		{
			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), Name);
			Tools.Add(MakeShared<FJsonValueObject>(Tool));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), Tools);
		if (!NextCursor.IsEmpty())
		{
			Result->SetStringField(TEXT("nextCursor"), NextCursor);
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetNumberField(TEXT("id"), 1);
		Root->SetObjectField(TEXT("result"), Result);
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCapabilityInventoryParserTest,
	"HyperAIStudio.NativeTools.CapabilityInventoryParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCapabilityInventoryParserTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("HTTP response byte bound accepts a small response"),
		FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(128, 128, Error));
	TestFalse(TEXT("HTTP response byte bound rejects oversized declared content"),
		FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(
			FHyperAIStudioCapabilityInventoryParser::MaxResponseBytes + 1ULL, 0, Error));
	TestFalse(TEXT("HTTP response byte bound rejects oversized buffered content"),
		FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(
			0, FHyperAIStudioCapabilityInventoryParser::MaxResponseBytes + 1LL, Error));
	FHyperAIStudioCapabilitySnapshot Snapshot;
	const FString SearchResponse = TEXT(R"JSON({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"list_toolsets"},{"name":"describe_toolset"},{"name":"call_tool"}]}})JSON");
	TestTrue(TEXT("Search tools/list parses"), FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(SearchResponse, 1, Snapshot, Error));
	TestEqual(TEXT("Exact trio is tool-search mode"), Snapshot.DiscoveryMode, EHyperAIStudioToolDiscoveryMode::ToolSearch);
	TestEqual(TEXT("Count is labeled top-level"), Snapshot.TopLevelToolCount, 3);

	const FString EagerResponse = TEXT(R"JSON({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"read_actor"},{"name":"write_actor"}]}})JSON");
	TestTrue(TEXT("Eager tools/list parses"), FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(EagerResponse, 1, Snapshot, Error));
	TestEqual(TEXT("No dispatchers is eager mode"), Snapshot.DiscoveryMode, EHyperAIStudioToolDiscoveryMode::Eager);
	TestTrue(TEXT("Eager mode does not claim missing dispatchers"), Snapshot.MissingDispatchers.IsEmpty());

	const FString DegradedResponse = TEXT(R"JSON({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"list_toolsets"},{"name":"call_tool"}]}})JSON");
	TestTrue(TEXT("Partial dispatcher list still parses diagnostically"), FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(DegradedResponse, 1, Snapshot, Error));
	TestEqual(TEXT("Partial trio is degraded"), Snapshot.DiscoveryMode, EHyperAIStudioToolDiscoveryMode::Degraded);
	TestTrue(TEXT("Missing dispatcher is explicit"), Snapshot.MissingDispatchers.Contains(TEXT("describe_toolset")));

	const FString ListText = TEXT(
		"Available toolsets:\r\n"
		"- Editor.Scene: Scene tools: actors and levels\r\n"
		"- Editor.Scene: duplicate\r\n"
		"- Editor.NoDescription\r\n"
		"- : invalid\r\n"
		"- NiagaraToolsets.Assets: Asset discovery.\r\n"
		"Provides:\r\n"
		"- GetAssetDiscoveryInfo: nested action, not a toolset\r\n"
		"    SequencerOutlinerTools.\r\n"
		"- HyperAI.Context: One bounded snapshot\r\n");
	TArray<FHyperAIStudioDiscoveredToolset> Toolsets;
	bool bTruncated = false;
	int32 Malformed = 0;
	TestTrue(TEXT("list_toolsets content parses"), FHyperAIStudioCapabilityInventoryParser::ParseListToolsetsResponse(
		HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(ListText),
		2,
		Toolsets,
		bTruncated,
		Malformed,
		Error));
	TestEqual(TEXT("Duplicate and nested description rows are ignored"), Toolsets.Num(), 4);
	TestEqual(TEXT("Descriptions split on the first colon"), Toolsets[0].Description, FString(TEXT("Scene tools: actors and levels")));
	TestEqual(TEXT("Epic qualified no-description rows are accepted"), Toolsets[1].Description, FString());
	TestEqual(TEXT("The following real toolset remains aligned after nested bullets"),
		Toolsets[3].Name, FString(TEXT("HyperAI.Context")));
	TestEqual(TEXT("Empty-name toolset rows are counted as malformed"), Malformed, 1);
	TestFalse(TEXT("Small result is not truncated"), bTruncated);

	const FString SchemaA = TEXT(R"JSON({"description":"Core sentinel","tools":[{"description":"Echo","name":"Sentinel.Echo"}],"version":"1","name":"Sentinel"})JSON");
	const FString SchemaB = TEXT(R"JSON({"name":"Sentinel","version":"1","tools":[{"name":"Sentinel.Echo","description":"Echo"}],"description":"Core sentinel"})JSON");
	FHyperAIStudioDescribedToolset DescribedA;
	FHyperAIStudioDescribedToolset DescribedB;
	TestTrue(TEXT("describe_toolset schema parses"), FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
		HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(SchemaA), 2, DescribedA, Error));
	TestTrue(TEXT("Same schema with different key order parses"), FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
		HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(SchemaB), 2, DescribedB, Error));
	TestEqual(TEXT("Tool count is schema-derived"), DescribedA.ToolCount, 1);
	TestEqual(TEXT("Described tool rows are retained"), DescribedA.Tools.Num(), 1);
	TestEqual(TEXT("Described tool name is retained"), DescribedA.Tools[0].Name, FString(TEXT("Sentinel.Echo")));
	TestEqual(TEXT("Described tool description is retained"), DescribedA.Tools[0].Description, FString(TEXT("Echo")));
	TestEqual(TEXT("Canonical schema hash ignores object key order"), DescribedA.SchemaHash, DescribedB.SchemaHash);
	const FString MissingDescriptionSchema = TEXT(R"JSON({"name":"Sentinel","tools":[{"name":"Sentinel.NoDescription"}]})JSON");
	TestTrue(TEXT("Missing tool descriptions remain valid and empty"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(MissingDescriptionSchema),
			2,
			DescribedA,
			Error));
	TestEqual(TEXT("Missing description is retained as empty"), DescribedA.Tools[0].Description, FString());

	const FString SseResponse = FString::Printf(
		TEXT("event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}\n\nevent: message\ndata: %s\n\n"),
		*HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(ListText));
	Toolsets.Reset();
	TestTrue(TEXT("Final JSON-RPC SSE data event is selected"), FHyperAIStudioCapabilityInventoryParser::ParseListToolsetsResponse(
		SseResponse, 2, Toolsets, bTruncated, Malformed, Error));
	TestEqual(TEXT("SSE toolsets match JSON path"), Toolsets.Num(), 4);

	const FString InvalidSchema = TEXT(R"JSON({"name":"Sentinel","tools":[{}]})JSON");
	TestFalse(TEXT("Invalid nested tool schema fails closed"), FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
		HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(InvalidSchema), 2, DescribedA, Error));
	const FString InvalidDescriptionSchema = TEXT(R"JSON({"name":"Sentinel","tools":[{"name":"Echo","description":7}]})JSON");
	TestFalse(TEXT("Non-string tool descriptions fail closed"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(InvalidDescriptionSchema),
			2,
			DescribedA,
			Error));
	const FString OversizedToolName = FString::ChrN(
		FHyperAIStudioCapabilityInventoryParser::MaxToolNameBytes + 1,
		TEXT('x'));
	const FString OversizedNameSchema = FString::Printf(
		TEXT("{\"name\":\"Sentinel\",\"tools\":[{\"name\":\"%s\"}]}"),
		*OversizedToolName);
	TestFalse(TEXT("Oversized tool names fail closed before storage"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(OversizedNameSchema),
			2,
			DescribedA,
			Error));

	const FString InitializeSse = TEXT("event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}\n\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\"}}\n\n");
	TSharedPtr<FJsonObject> InitializeResult;
	TestTrue(TEXT("Initialize accepts SSE with a matching response after notifications"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(InitializeSse, 1, InitializeResult, Error));
	TestFalse(TEXT("Wrong JSON-RPC version is rejected"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
			TEXT("{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{}}"), 1, InitializeResult, Error));
	TestFalse(TEXT("Wrong numeric id is rejected"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}"), 1, InitializeResult, Error));
	TestFalse(TEXT("String id is rejected for a numeric request"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{}}"), 1, InitializeResult, Error));
	TestFalse(TEXT("Matching JSON-RPC error is rejected"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
			TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1,\"message\":\"failed\"}}"), 1, InitializeResult, Error));
	TestFalse(TEXT("A stray SSE response is rejected even if a matching result follows"),
		FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
			TEXT("data: {\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{}}\n\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"),
			1,
			InitializeResult,
			Error));

	const FString Paginated = HyperAIStudio::CapabilityInventory::Tests::MakeToolsListResponse({ TEXT("read_actor") }, TEXT("page-2"));
	TestTrue(TEXT("Paginated tools/list remains parseable"),
		FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(Paginated, 1, Snapshot, Error));
	TestTrue(TEXT("Pagination is surfaced as incomplete"), Snapshot.bTruncated);
	TestEqual(TEXT("Pagination without proven dispatchers is degraded"), Snapshot.DiscoveryMode, EHyperAIStudioToolDiscoveryMode::Degraded);
	TestEqual(TEXT("nextCursor is retained diagnostically"), Snapshot.NextCursor, FString(TEXT("page-2")));

	const FString DuplicateTop = HyperAIStudio::CapabilityInventory::Tests::MakeToolsListResponse({ TEXT("read_actor"), TEXT("read_actor") });
	TestFalse(TEXT("Exact duplicate top-level tool names fail closed"),
		FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(DuplicateTop, 1, Snapshot, Error));
	const FString CollisionTop = HyperAIStudio::CapabilityInventory::Tests::MakeToolsListResponse({ TEXT("ReadActor"), TEXT("readactor") });
	TestFalse(TEXT("Case-insensitive top-level tool collisions fail closed"),
		FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(CollisionTop, 1, Snapshot, Error));

	TArray<FString> BoundedNames;
	for (int32 Index = 0; Index < FHyperAIStudioCapabilityInventoryParser::MaxTopLevelTools + 4; ++Index)
	{
		BoundedNames.Add(FString::Printf(TEXT("bounded_tool_%d"), Index));
	}
	BoundedNames.Add(TEXT("list_toolsets"));
	BoundedNames.Add(TEXT("describe_toolset"));
	BoundedNames.Add(TEXT("call_tool"));
	const FString DispatchersAfterBound = HyperAIStudio::CapabilityInventory::Tests::MakeToolsListResponse(BoundedNames);
	TestTrue(TEXT("tools/list continues scanning after its stored-name bound"),
		FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(DispatchersAfterBound, 1, Snapshot, Error));
	TestEqual(TEXT("Dispatchers after storage bound still prove search mode"), Snapshot.DiscoveryMode, EHyperAIStudioToolDiscoveryMode::ToolSearch);
	TestTrue(TEXT("Stored-name truncation is explicit"), Snapshot.bTruncated);
	TestEqual(TEXT("Full parsed top-level count remains distinct from stored names"), Snapshot.TopLevelToolCount, BoundedNames.Num());

	const FString DuplicateSchema = TEXT(R"JSON({"name":"Sentinel","tools":[{"name":"Echo"},{"name":"Echo"}]})JSON");
	TestFalse(TEXT("Exact duplicate described tool names fail closed"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(DuplicateSchema), 2, DescribedA, Error));
	const FString CollisionSchema = TEXT(R"JSON({"name":"Sentinel","tools":[{"name":"Echo"},{"name":"echo"}]})JSON");
	TestFalse(TEXT("Case-insensitive described tool collisions fail closed"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(CollisionSchema), 2, DescribedA, Error));

	FString TooManyToolsSchema = TEXT("{\"name\":\"Bounded\",\"tools\":[");
	for (int32 Index = 0; Index <= FHyperAIStudioCapabilityInventoryParser::MaxToolsPerToolset; ++Index)
	{
		if (Index > 0)
		{
			TooManyToolsSchema += TEXT(",");
		}
		TooManyToolsSchema += FString::Printf(TEXT("{\"name\":\"tool_%d\"}"), Index);
	}
	TooManyToolsSchema += TEXT("]}");
	TestFalse(TEXT("Per-toolset row bound fails closed instead of truncating"),
		FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
			HyperAIStudio::CapabilityInventory::Tests::MakeToolCallResponse(TooManyToolsSchema),
			2,
			DescribedA,
			Error));
	TestTrue(TEXT("Aggregate detailed bound can hold more than one maximum-size toolset"),
		FHyperAIStudioCapabilityInventoryParser::MaxDetailedTools
			>= FHyperAIStudioCapabilityInventoryParser::MaxToolsPerToolset * 2);

	FString TooDeep = TEXT("0");
	for (int32 Depth = 0; Depth < FHyperAIStudioCapabilityInventoryParser::MaxCanonicalJsonDepth + 2; ++Depth)
	{
		TooDeep = TEXT("[") + TooDeep + TEXT("]");
	}
	FString Canonical;
	TestFalse(TEXT("Canonical JSON nesting deeper than 64 fails closed"),
		FHyperAIStudioCapabilityInventoryParser::CanonicalizeJson(TooDeep, Canonical, Error));

	return true;
}

#endif
