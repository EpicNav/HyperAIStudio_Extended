// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioManagedConfigMigration.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::ManagedConfigMigration::Tests
{
	const FString HashA = TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const FString HashB = TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

	struct FManagedBlockCase
	{
		const TCHAR* Label;
		FString Source;
		EHyperAIStudioManagedBlockState Expected;
	};

	struct FInspectionCase
	{
		const TCHAR* Label;
		FString Json;
		EHyperAIStudioJsonEntryState Expected;
	};

	struct FCodexTomlCase
	{
		const TCHAR* Label;
		FString Source;
		TArray<FString> DesiredIds;
		EHyperAIStudioCodexTomlCollisionState ExpectedState;
		TArray<FString> ExpectedCollisions;
		FString ExpectedReason;
	};

	struct FPlannerCase
	{
		const TCHAR* Label;
		FHyperAIStudioManagedEntryReconcileInput Input;
		EHyperAIStudioManagedEntryDecision ExpectedDecision;
		EHyperAIStudioManagedEntryOwnership ExpectedOwnership = EHyperAIStudioManagedEntryOwnership::None;
		bool bExpectedBlocks = false;
		bool bExpectedDropLedger = false;
	};

	FHyperAIStudioManagedEntryReconcileInput ExistingObject(const FString& Hash = HashA)
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.ExistingState = EHyperAIStudioJsonEntryState::EntryObject;
		Input.ExistingObjectHash = Hash;
		return Input;
	}

	FHyperAIStudioManagedConfigOwnershipLedger MakeValidLedger()
	{
		FHyperAIStudioManagedConfigOwnershipLedger Ledger;
		FHyperAIStudioManagedConfigLedgerClient Cursor;
		Cursor.ClientId = TEXT("cursor");
		Cursor.RelativePath = TEXT(".cursor/mcp.json");
		Cursor.Container = TEXT("mcpServers");
		Cursor.Entries.Add({ TEXT("unreal-mcp"), HashA, 1 });
		Ledger.Clients.Add(MoveTemp(Cursor));

		FHyperAIStudioManagedConfigLedgerClient Claude;
		Claude.ClientId = TEXT("claude");
		Claude.RelativePath = TEXT(".mcp.json");
		Claude.Container = TEXT("mcpServers");
		Claude.Entries.Add({ TEXT("z-extra"), HashB, 1 });
		Claude.Entries.Add({ TEXT("a-extra"), HashA, 1 });
		Ledger.Clients.Add(MoveTemp(Claude));
		return Ledger;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioManagedBlockParserTest,
	"HyperAIStudio.NativeTools.ManagedConfig.ManagedBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioManagedBlockParserTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Tests;
	const FString Begin = TEXT("# BEGIN HYPERAISTUDIO MANAGED MCP");
	const FString End = TEXT("# END HYPERAISTUDIO MANAGED MCP");
	const TArray<FManagedBlockCase> Cases = {
		{ TEXT("Zero markers"), TEXT("user content\r\n"), EHyperAIStudioManagedBlockState::Missing },
		{ TEXT("One ordered pair"), TEXT("prefix\r\n# BEGIN HYPERAISTUDIO MANAGED MCP\ninside\n# END HYPERAISTUDIO MANAGED MCP\r\nsuffix  \n"), EHyperAIStudioManagedBlockState::ValidSinglePair },
		{ TEXT("Lone begin"), Begin, EHyperAIStudioManagedBlockState::BeginWithoutEnd },
		{ TEXT("Lone end"), End, EHyperAIStudioManagedBlockState::EndWithoutBegin },
		{ TEXT("Repeated begin without end"), Begin + TEXT("\n") + Begin, EHyperAIStudioManagedBlockState::DuplicateMarkers },
		{ TEXT("Repeated end without begin"), End + TEXT("\n") + End, EHyperAIStudioManagedBlockState::DuplicateMarkers },
		{ TEXT("Reversed pair"), End + TEXT("\n") + Begin, EHyperAIStudioManagedBlockState::Reversed },
		{ TEXT("Nested pair"), Begin + TEXT("\n") + Begin + TEXT("\n") + End + TEXT("\n") + End, EHyperAIStudioManagedBlockState::Nested },
		{ TEXT("Two complete pairs"), Begin + TEXT("\n") + End + TEXT("\n") + Begin + TEXT("\n") + End, EHyperAIStudioManagedBlockState::DuplicatePairs },
		{ TEXT("Duplicate begin marker"), Begin + TEXT("\n") + End + TEXT("\n") + Begin, EHyperAIStudioManagedBlockState::DuplicateMarkers },
		{ TEXT("Duplicate end marker"), Begin + TEXT("\n") + End + TEXT("\n") + End, EHyperAIStudioManagedBlockState::DuplicateMarkers }
	};

	for (const FManagedBlockCase& Case : Cases)
	{
		const FHyperAIStudioManagedBlockParseResult Result =
			FHyperAIStudioManagedBlockParser::Parse(Case.Source, Begin, End);
		TestEqual(Case.Label, Result.State, Case.Expected);
	}

	const FString ExactPrefix = TEXT("prefix\r\n\t");
	const FString ExactBlock = Begin + TEXT("\r\nmanaged\n") + End;
	const FString ExactSuffix = TEXT("  \r\nuser suffix\n\n");
	const FHyperAIStudioManagedBlockParseResult Valid =
		FHyperAIStudioManagedBlockParser::Parse(ExactPrefix + ExactBlock + ExactSuffix, Begin, End);
	TestEqual(TEXT("Valid prefix is an exact slice"), Valid.Prefix, ExactPrefix);
	TestEqual(TEXT("Valid inclusive block is an exact slice"), Valid.InclusiveBlock, ExactBlock);
	TestEqual(TEXT("Valid suffix is an exact slice"), Valid.Suffix, ExactSuffix);
	TestEqual(TEXT("Removing the block preserves all surrounding text"), Valid.Prefix + Valid.Suffix, ExactPrefix + ExactSuffix);
	TestTrue(TEXT("Only a valid pair is replaceable/removable"), Valid.HasEditableSinglePair());

	const FHyperAIStudioManagedBlockParseResult InvalidMarkers =
		FHyperAIStudioManagedBlockParser::Parse(TEXT("anything"), Begin, Begin);
	TestEqual(TEXT("Identical markers are rejected"), InvalidMarkers.State, EHyperAIStudioManagedBlockState::InvalidMarkers);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCodexTomlCollisionInspectorTest,
	"HyperAIStudio.NativeTools.ManagedConfig.CodexTomlCollisions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCodexTomlCollisionInspectorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Tests;
	const FString Begin = TEXT("# BEGIN HYPERAISTUDIO MANAGED MCP");
	const FString End = TEXT("# END HYPERAISTUDIO MANAGED MCP");
	const TArray<FCodexTomlCase> Cases = {
		{
			TEXT("Unrelated scalar config is safe"),
			TEXT("model = \"gpt-5\"\r\napproval_policy = 'on-request'\r\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Comments and string values cannot fabricate a collision"),
			TEXT("# [mcp_servers.unreal-mcp]\nnote = \"[mcp_servers.unreal-mcp] # still a string\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Unrelated MCP table is preserved"),
			TEXT("[mcp_servers.other]\nurl = \"http://127.0.0.1:9000/mcp\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Unrelated quoted and dotted MCP ids are preserved"),
			TEXT("mcp_servers.'other'.url = \"http://127.0.0.1:9000/mcp\"\n[mcp_servers.\"second\"]\nurl = \"http://127.0.0.1:9001/mcp\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Unrelated assignment inside mcp_servers table is preserved"),
			TEXT("[mcp_servers]\nother = { command = \"other\" }\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Bare table id collides"),
			TEXT("[mcp_servers.unreal-mcp]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Double quoted table id collides"),
			TEXT("[mcp_servers.\"unreal-mcp\"]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Literal quoted table id collides"),
			TEXT("[mcp_servers.'unreal-mcp']\nurl = 'http://example.invalid'\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Quoted root table segment collides"),
			TEXT("[\"mcp_servers\".\"unreal-mcp\"]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Root dotted key collides"),
			TEXT("mcp_servers.\"unreal-mcp\".url = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Assignment inside mcp_servers table collides"),
			TEXT("[mcp_servers]\n\"unreal-mcp\" = { url = \"http://example.invalid\" }\nother = { url = \"http://127.0.0.1:9000/mcp\" }\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Dotted assignment inside mcp_servers table collides"),
			TEXT("[mcp_servers]\n\"unreal-mcp\".url = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Inline root definition detects desired id"),
			TEXT("mcp_servers = { other = { command = \"other\" }, \"unreal-mcp\" = { url = \"http://example.invalid\" } }\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Inline root table with only unrelated ids cannot be extended safely"),
			TEXT("mcp_servers = { other = { command = \"other\" } }\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_inline_mcp_servers_not_extendable")
		},
		{
			TEXT("Ids compare exactly and case-sensitively"),
			TEXT("[mcp_servers.Unreal-MCP]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Quoted dot remains part of one server id"),
			TEXT("[mcp_servers.\"team.server\"]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("team.server") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("team.server") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Managed definition is ignored"),
			Begin + TEXT("\n[mcp_servers.\"unreal-mcp\"]\nurl = \"http://managed.invalid\"\n") + End + TEXT("\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Unmanaged definition before managed block still collides"),
			TEXT("[mcp_servers.\"unreal-mcp\"]\nurl = \"http://user.invalid\"\n")
				+ Begin + TEXT("\n[mcp_servers.\"unreal-mcp\"]\nurl = \"http://managed.invalid\"\n") + End + TEXT("\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("unreal-mcp") },
			TEXT("codex_toml_unmanaged_id_collision")
		},
		{
			TEXT("Suffix beginning with a new table has explicit context"),
			Begin + TEXT("\n[mcp_servers.\"unreal-mcp\"]\nurl = \"http://managed.invalid\"\n") + End
				+ TEXT("\n[mcp_servers.other]\nurl = \"http://127.0.0.1:9000/mcp\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Safe,
			{},
			TEXT("codex_toml_no_unmanaged_collision")
		},
		{
			TEXT("Assignment after managed block has ambiguous table context"),
			Begin + TEXT("\n[mcp_servers.\"unreal-mcp\"]\nurl = \"http://managed.invalid\"\n") + End
				+ TEXT("\nmodel = \"gpt-5\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_suffix_context_ambiguous")
		},
		{
			TEXT("Malformed managed markers fail closed"),
			Begin + TEXT("\n[mcp_servers.other]\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_invalid_managed_markers")
		},
		{
			TEXT("Unclosed TOML string fails closed"),
			TEXT("model = \"unterminated\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_newline_in_basic_string")
		},
		{
			TEXT("Array-of-tables MCP shape fails closed"),
			TEXT("[[mcp_servers.unreal-mcp]]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_mcp_servers_array_table")
		},
		{
			TEXT("Duplicate MCP assignment fails closed"),
			TEXT("mcp_servers.other.url = \"one\"\nmcp_servers.other.url = \"two\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_duplicate_mcp_assignment")
		},
		{
			TEXT("Ambiguous escaped MCP key fails closed"),
			TEXT("[mcp_servers.\"unreal\\u002dmcp\"]\nurl = \"http://example.invalid\"\n"),
			{ TEXT("unreal-mcp") },
			EHyperAIStudioCodexTomlCollisionState::Rejected,
			{},
			TEXT("codex_toml_ambiguous_key_escape")
		},
		{
			TEXT("Multiple desired collisions are sorted deterministically"),
			TEXT("[mcp_servers.zeta]\ncommand = \"z\"\n[mcp_servers.alpha]\ncommand = \"a\"\n"),
			{ TEXT("zeta"), TEXT("alpha") },
			EHyperAIStudioCodexTomlCollisionState::Collision,
			{ TEXT("alpha"), TEXT("zeta") },
			TEXT("codex_toml_unmanaged_id_collision")
		}
	};

	for (const FCodexTomlCase& Case : Cases)
	{
		const FHyperAIStudioCodexTomlCollisionInspection Result =
			FHyperAIStudioCodexTomlCollisionInspector::Inspect(
				Case.Source,
				Begin,
				End,
				Case.DesiredIds);
		TestEqual(Case.Label, Result.State, Case.ExpectedState);
		TestEqual(TEXT("Collision list is exact and deterministic"), Result.CollidingServerIds, Case.ExpectedCollisions);
		TestEqual(TEXT("Reason code identifies the safety decision"), Result.ReasonCode, Case.ExpectedReason);
		TestEqual(
			TEXT("Only Safe permits a managed write"),
			Result.MayWriteManagedBlock(),
			Case.ExpectedState == EHyperAIStudioCodexTomlCollisionState::Safe);
	}

	const FString OversizedId = FString::ChrN(
		FHyperAIStudioCodexTomlCollisionInspector::MaxServerIdCharacters + 1,
		TEXT('x'));
	const FHyperAIStudioCodexTomlCollisionInspection OversizedDesired =
		FHyperAIStudioCodexTomlCollisionInspector::Inspect(
			TEXT("model = \"gpt-5\"\n"),
			Begin,
			End,
			{ OversizedId });
	TestEqual(
		TEXT("Oversized desired ids fail closed"),
		OversizedDesired.State,
		EHyperAIStudioCodexTomlCollisionState::Rejected);
	TestEqual(
		TEXT("Oversized desired id reason"),
		OversizedDesired.ReasonCode,
		FString(TEXT("codex_toml_invalid_or_duplicate_desired_id")));

	const FHyperAIStudioCodexTomlCollisionInspection DuplicateDesired =
		FHyperAIStudioCodexTomlCollisionInspector::Inspect(
			TEXT("model = \"gpt-5\"\n"),
			Begin,
			End,
			{ TEXT("same"), TEXT("same") });
	TestEqual(
		TEXT("Duplicate desired ids fail closed"),
		DuplicateDesired.State,
		EHyperAIStudioCodexTomlCollisionState::Rejected);

	const FHyperAIStudioCodexTomlCollisionInspection OversizedSource =
		FHyperAIStudioCodexTomlCollisionInspector::Inspect(
			FString::ChrN(FHyperAIStudioCodexTomlCollisionInspector::MaxTomlCharacters + 1, TEXT(' ')),
			Begin,
			End,
			{ TEXT("unreal-mcp") });
	TestEqual(
		TEXT("Oversized TOML fails closed before parsing"),
		OversizedSource.State,
		EHyperAIStudioCodexTomlCollisionState::Rejected);
	TestEqual(
		TEXT("Oversized TOML reason"),
		OversizedSource.ReasonCode,
		FString(TEXT("codex_toml_input_too_large")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCanonicalJsonTest,
	"HyperAIStudio.NativeTools.ManagedConfig.CanonicalJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCanonicalJsonTest::RunTest(const FString& Parameters)
{
	const FHyperAIStudioCanonicalJsonResult EmptyObject = FHyperAIStudioCanonicalJson::HashObject(TEXT("{}"));
	TestTrue(TEXT("Empty object hashes"), EmptyObject.bSuccess);
	TestEqual(TEXT("Empty object canonical form"), EmptyObject.CanonicalJson, FString(TEXT("{}")));
	TestEqual(
		TEXT("SHA-256 implementation matches the known empty-object vector"),
		EmptyObject.ObjectHash,
		FString(TEXT("sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a")));

	const FHyperAIStudioCanonicalJsonResult First = FHyperAIStudioCanonicalJson::HashObject(
		TEXT("{ \"z\": 1, \"a\": { \"y\": false, \"x\": [\"v\", null] } }"));
	const FHyperAIStudioCanonicalJsonResult Second = FHyperAIStudioCanonicalJson::HashObject(
		TEXT("{\"a\":{\"x\":[\"v\",null],\"y\":false},\"z\":1}"));
	TestTrue(TEXT("Nested object hashes"), First.bSuccess && Second.bSuccess);
	TestEqual(TEXT("Object keys are recursively sorted"), First.CanonicalJson, FString(TEXT("{\"a\":{\"x\":[\"v\",null],\"y\":false},\"z\":1}")));
	TestEqual(TEXT("Whitespace and input object order do not change the hash"), First.ObjectHash, Second.ObjectHash);

	const FHyperAIStudioCanonicalJsonResult ReorderedArray = FHyperAIStudioCanonicalJson::HashObject(
		TEXT("{\"a\":[null,\"v\"],\"z\":1}"));
	TestNotEqual(TEXT("Array order remains part of identity"), First.ObjectHash, ReorderedArray.ObjectHash);
	const FHyperAIStudioCanonicalJsonResult StringOne = FHyperAIStudioCanonicalJson::HashObject(TEXT("{\"v\":\"1\"}"));
	const FHyperAIStudioCanonicalJsonResult NumberOne = FHyperAIStudioCanonicalJson::HashObject(TEXT("{\"v\":1}"));
	TestNotEqual(TEXT("JSON types remain part of identity"), StringOne.ObjectHash, NumberOne.ObjectHash);

	TestFalse(TEXT("Invalid JSON is rejected"), FHyperAIStudioCanonicalJson::HashObject(TEXT("{")).bSuccess);
	TestFalse(TEXT("A non-object root is rejected"), FHyperAIStudioCanonicalJson::HashObject(TEXT("[]")).bSuccess);
	TestFalse(TEXT("Duplicate object keys are rejected before DOM overwrite"), FHyperAIStudioCanonicalJson::HashObject(TEXT("{\"a\":1,\"a\":2}")).bSuccess);

	FString TooDeep;
	for (int32 Depth = 0; Depth < FHyperAIStudioCanonicalJson::MaxDepth + 1; ++Depth)
	{
		TooDeep += TEXT("{\"a\":");
	}
	TooDeep += TEXT("{}");
	for (int32 Depth = 0; Depth < FHyperAIStudioCanonicalJson::MaxDepth + 1; ++Depth)
	{
		TooDeep += TEXT("}");
	}
	TestFalse(TEXT("Depth beyond the bound is rejected"), FHyperAIStudioCanonicalJson::HashObject(TooDeep).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioOwnershipLedgerTest,
	"HyperAIStudio.NativeTools.ManagedConfig.OwnershipLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioOwnershipLedgerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Tests;
	const FHyperAIStudioManagedConfigOwnershipLedger Source = MakeValidLedger();
	FString Serialized;
	FString ReasonCode;
	TestTrue(TEXT("Valid ownership ledger serializes"), FHyperAIStudioManagedConfigLedgerCodec::Serialize(Source, Serialized, ReasonCode));
	TestTrue(TEXT("Clients are serialized deterministically"), Serialized.Find(TEXT("\"claude\"")) < Serialized.Find(TEXT("\"cursor\"")));
	TestTrue(TEXT("Entries are serialized deterministically"), Serialized.Find(TEXT("\"a-extra\"")) < Serialized.Find(TEXT("\"z-extra\"")));
	TestFalse(TEXT("Ledger has no command field"), Serialized.Contains(TEXT("\"command\"")));
	TestFalse(TEXT("Ledger has no args field"), Serialized.Contains(TEXT("\"args\"")));
	TestFalse(TEXT("Ledger has no env field"), Serialized.Contains(TEXT("\"env\"")));
	TestFalse(TEXT("Ledger has no token field"), Serialized.Contains(TEXT("\"token\"")));

	FHyperAIStudioManagedConfigOwnershipLedger Parsed;
	TestTrue(TEXT("Serialized ledger parses"), FHyperAIStudioManagedConfigLedgerCodec::Parse(Serialized, Parsed, ReasonCode));
	TestEqual(TEXT("Round-trip client count"), Parsed.Clients.Num(), 2);
	const FHyperAIStudioManagedConfigLedgerEntry* ExactEntry =
		Parsed.FindEntry(TEXT("cursor"), TEXT(".cursor/mcp.json"), TEXT("mcpServers"), TEXT("unreal-mcp"));
	TestNotNull(TEXT("Lookup requires the complete ownership coordinate"), ExactEntry);
	TestNull(TEXT("Wrong path does not return ownership"), Parsed.FindEntry(TEXT("cursor"), TEXT(".mcp.json"), TEXT("mcpServers"), TEXT("unreal-mcp")));

	const FString ValidSingle = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{\"claude\":{\"path\":\".mcp.json\",\"container\":\"mcpServers\",\"entries\":{\"unreal-mcp\":{\"objectHash\":\"%s\",\"writerVersion\":1}}}}}"),
		*HashA);
	const TArray<TPair<FString, FString>> InvalidCases = {
		{ TEXT("Invalid JSON"), TEXT("{") },
		{ TEXT("Root array"), TEXT("[]") },
		{ TEXT("Clients must be an object"), TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":[]}") },
		{ TEXT("Unknown root field rejects secret smuggling"), TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{},\"token\":\"secret\"}") },
		{ TEXT("Duplicate JSON keys"), TEXT("{\"schemaVersion\":1,\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{}}") },
		{ TEXT("Traversal path"), FString::Printf(TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{\"claude\":{\"path\":\"../.mcp.json\",\"container\":\"mcpServers\",\"entries\":{\"x\":{\"objectHash\":\"%s\",\"writerVersion\":1}}}}}"), *HashA) },
		{ TEXT("Client coordinate allowlist"), FString::Printf(TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{\"claude\":{\"path\":\"Config/DefaultEngine.ini\",\"container\":\"mcpServers\",\"entries\":{\"x\":{\"objectHash\":\"%s\",\"writerVersion\":1}}}}}"), *HashA) },
		{ TEXT("Entries must be an object"), TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{\"claude\":{\"path\":\".mcp.json\",\"container\":\"mcpServers\",\"entries\":[]}}}") },
		{ TEXT("Unknown entry field"), FString::Printf(TEXT("{\"schemaVersion\":1,\"generator\":\"HyperAIStudio\",\"clients\":{\"claude\":{\"path\":\".mcp.json\",\"container\":\"mcpServers\",\"entries\":{\"x\":{\"objectHash\":\"%s\",\"writerVersion\":1,\"command\":\"secret\"}}}}}"), *HashA) },
		{ TEXT("Uppercase hashes are non-canonical"), ValidSingle.Replace(TEXT("aaaaaaaa"), TEXT("AAAAAAAA"), ESearchCase::CaseSensitive) },
		{ TEXT("Writer version must be the exact supported version"), ValidSingle.Replace(TEXT("\"writerVersion\":1"), TEXT("\"writerVersion\":0"), ESearchCase::CaseSensitive) },
		{ TEXT("Future writer version cannot claim ownership"), ValidSingle.Replace(TEXT("\"writerVersion\":1"), TEXT("\"writerVersion\":2"), ESearchCase::CaseSensitive) }
	};

	for (const TPair<FString, FString>& Case : InvalidCases)
	{
		FHyperAIStudioManagedConfigOwnershipLedger Ignored;
		TestFalse(*Case.Key, FHyperAIStudioManagedConfigLedgerCodec::Parse(Case.Value, Ignored, ReasonCode));
	}
	TestEqual(
		TEXT("Future writer versions use an explicit unsupported-version reason"),
		ReasonCode,
		FString(TEXT("ledger_unsupported_writer_version")));

	FHyperAIStudioManagedConfigOwnershipLedger FutureWriterLedger = Source;
	FutureWriterLedger.Clients[0].Entries[0].WriterVersion = 2;
	TestFalse(
		TEXT("Serializer cannot mint future ownership evidence"),
		FHyperAIStudioManagedConfigLedgerCodec::Serialize(FutureWriterLedger, Serialized, ReasonCode));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioJsonInspectionAndReconciliationTest,
	"HyperAIStudio.NativeTools.ManagedConfig.Reconciliation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioJsonInspectionAndReconciliationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Tests;
	const TArray<FInspectionCase> InspectionCases = {
		{ TEXT("Empty file"), TEXT(" \r\n\t"), EHyperAIStudioJsonEntryState::EmptyFile },
		{ TEXT("Invalid JSON"), TEXT("{"), EHyperAIStudioJsonEntryState::InvalidJson },
		{ TEXT("Duplicate keys"), TEXT("{\"mcpServers\":{},\"mcpServers\":{}}"), EHyperAIStudioJsonEntryState::InvalidJson },
		{ TEXT("Root is not an object"), TEXT("[]"), EHyperAIStudioJsonEntryState::RootNotObject },
		{ TEXT("Container missing"), TEXT("{\"other\":{}}"), EHyperAIStudioJsonEntryState::ContainerMissing },
		{ TEXT("Container is not an object"), TEXT("{\"mcpServers\":[]}"), EHyperAIStudioJsonEntryState::ContainerNotObject },
		{ TEXT("Entry missing"), TEXT("{\"mcpServers\":{}}"), EHyperAIStudioJsonEntryState::EntryMissing },
		{ TEXT("Entry is not an object"), TEXT("{\"mcpServers\":{\"unreal-mcp\":\"edited\"}}"), EHyperAIStudioJsonEntryState::EntryNotObject },
		{ TEXT("Entry object"), TEXT("{\"mcpServers\":{\"unreal-mcp\":{\"url\":\"http://127.0.0.1:8765/sse\"}}}"), EHyperAIStudioJsonEntryState::EntryObject }
	};
	for (const FInspectionCase& Case : InspectionCases)
	{
		const FHyperAIStudioJsonEntryInspection Result =
			FHyperAIStudioJsonEntryInspector::InspectExistingFile(Case.Json, TEXT("mcpServers"), TEXT("unreal-mcp"));
		TestEqual(Case.Label, Result.State, Case.Expected);
		if (Case.Expected == EHyperAIStudioJsonEntryState::EntryObject)
		{
			TestTrue(TEXT("Inspected object receives a valid canonical hash"), FHyperAIStudioCanonicalJson::IsValidObjectHash(Result.ExistingObjectHash));
		}
	}

	TArray<FPlannerCase> Cases;
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.ExistingState = EHyperAIStudioJsonEntryState::MissingFile;
		Cases.Add({ TEXT("Disabled missing client is never created"), Input, EHyperAIStudioManagedEntryDecision::NoOp });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashA;
		Input.ExistingState = EHyperAIStudioJsonEntryState::MissingFile;
		Cases.Add({ TEXT("Enabled desired entry creates missing file"), Input, EHyperAIStudioManagedEntryDecision::Write });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bExactHistoricalSettingsOwnership = true;
		Input.HistoricalObjectHash = HashA;
		Cases.Add({ TEXT("Enabled exact historical cleanup"), Input, EHyperAIStudioManagedEntryDecision::Remove, EHyperAIStudioManagedEntryOwnership::HistoricalExact });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bExactHistoricalSettingsOwnership = true;
		Input.HistoricalObjectHash = HashA;
		Cases.Add({ TEXT("Disabled exact historical stale entry is removed"), Input, EHyperAIStudioManagedEntryDecision::Remove, EHyperAIStudioManagedEntryOwnership::HistoricalExact });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.LedgerObjectHash = HashA;
		Cases.Add({ TEXT("Disabled exact ledger-owned stale entry is removed"), Input, EHyperAIStudioManagedEntryDecision::Remove, EHyperAIStudioManagedEntryOwnership::LedgerExact, false, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject(HashB);
		Input.LedgerObjectHash = HashA;
		Cases.Add({ TEXT("Modified ledger-owned entry conflicts"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject(HashB);
		Input.bExactHistoricalSettingsOwnership = true;
		Input.HistoricalObjectHash = HashA;
		Cases.Add({ TEXT("Exact settings evidence cannot delete edited JSON"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		const FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Cases.Add({ TEXT("Unowned stale entry is preserved"), Input, EHyperAIStudioManagedEntryDecision::Preserve });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashA;
		Cases.Add({ TEXT("Exact desired entry is a no-op without claiming it"), Input, EHyperAIStudioManagedEntryDecision::NoOp });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashA;
		Input.LedgerObjectHash = HashA;
		Cases.Add({ TEXT("Exact pending-write evidence recovers as owned no-op"), Input, EHyperAIStudioManagedEntryDecision::NoOp, EHyperAIStudioManagedEntryOwnership::LedgerExact });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashB;
		Input.LedgerObjectHash = HashA;
		Cases.Add({ TEXT("Owned stale desired entry is rewritten"), Input, EHyperAIStudioManagedEntryDecision::Write, EHyperAIStudioManagedEntryOwnership::LedgerExact });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashB;
		Cases.Add({ TEXT("Unowned desired-id collision fails closed"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input = ExistingObject();
		Input.bClientEnabledForGeneration = true;
		Input.bDesiredEntry = true;
		Input.bAllowReplaceUnownedDesired = true;
		Input.DesiredObjectHash = HashB;
		Cases.Add({ TEXT("Explicitly authorized desired replacement writes"), Input, EHyperAIStudioManagedEntryDecision::Write, EHyperAIStudioManagedEntryOwnership::None });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.ExistingState = EHyperAIStudioJsonEntryState::InvalidJson;
		Cases.Add({ TEXT("Invalid JSON blocks migration"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.ExistingState = EHyperAIStudioJsonEntryState::ContainerNotObject;
		Cases.Add({ TEXT("Invalid container blocks migration"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.bDesiredEntry = true;
		Input.DesiredObjectHash = HashA;
		Input.ExistingState = EHyperAIStudioJsonEntryState::MissingFile;
		Cases.Add({ TEXT("Disabled client cannot carry desired state"), Input, EHyperAIStudioManagedEntryDecision::Conflict, EHyperAIStudioManagedEntryOwnership::None, true });
	}
	{
		FHyperAIStudioManagedEntryReconcileInput Input;
		Input.ExistingState = EHyperAIStudioJsonEntryState::EntryMissing;
		Input.LedgerObjectHash = HashA;
		Cases.Add({ TEXT("Absent entry drops stale ledger metadata"), Input, EHyperAIStudioManagedEntryDecision::NoOp, EHyperAIStudioManagedEntryOwnership::None, false, true });
	}

	for (const FPlannerCase& Case : Cases)
	{
		const FHyperAIStudioManagedEntryReconcileResult Result =
			FHyperAIStudioManagedEntryReconciler::Plan(Case.Input);
		TestEqual(Case.Label, Result.Decision, Case.ExpectedDecision);
		TestEqual(TEXT("Ownership classification"), Result.Ownership, Case.ExpectedOwnership);
		TestEqual(TEXT("Migration blocking flag"), Result.bBlocksMigration, Case.bExpectedBlocks);
		TestEqual(TEXT("Ledger cleanup flag"), Result.bDropLedgerRecordAfterSuccess, Case.bExpectedDropLedger);
		if (Result.Decision == EHyperAIStudioManagedEntryDecision::Write)
		{
			TestTrue(TEXT("Successful writes may establish ledger ownership"), Result.bMayRecordLedgerAfterSuccessfulWrite);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioManagedConfigTransactionEvidenceTest,
	"HyperAIStudio.NativeTools.ManagedConfig.TransactionEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioManagedConfigTransactionEvidenceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ManagedConfigMigration::Tests;

	FHyperAIStudioManagedConfigPendingTransaction Pending;
	Pending.TransactionId = FString::ChrN(32, TEXT('a'));
	Pending.Phase = TEXT("targets_prepared");
	FHyperAIStudioManagedConfigPendingTarget Target;
	Target.ClientId = TEXT("cursor");
	Target.RelativePath = TEXT(".cursor/mcp.json");
	Target.Container = TEXT("mcpServers");
	Target.bOriginalExists = true;
	Target.OriginalFileHash = HashA;
	Target.bDesiredExists = true;
	Target.DesiredFileHash = HashB;
	Target.DesiredLedgerEntries.Add({ TEXT("unreal-mcp"), HashB, 1 });
	Pending.Targets.Add(Target);

	FString PendingJson;
	FString Reason;
	TestTrue(TEXT("Bound pending evidence serializes"),
		FHyperAIStudioManagedConfigPendingCodec::Serialize(Pending, PendingJson, Reason));
	FHyperAIStudioManagedConfigPendingTransaction ParsedPending;
	TestTrue(TEXT("Bound pending evidence parses strictly"),
		FHyperAIStudioManagedConfigPendingCodec::Parse(PendingJson, ParsedPending, Reason));
	TestEqual(TEXT("Transaction id round-trips"), ParsedPending.TransactionId, Pending.TransactionId);
	TestEqual(TEXT("Exact original is retry-safe"),
		FHyperAIStudioManagedConfigPendingCodec::Classify(Target, true, HashA),
		EHyperAIStudioTransactionEvidenceMatch::ExactOriginal);
	TestEqual(TEXT("Exact desired may finish the ledger"),
		FHyperAIStudioManagedConfigPendingCodec::Classify(Target, true, HashB),
		EHyperAIStudioTransactionEvidenceMatch::ExactDesired);
	TestEqual(TEXT("A third target state blocks"),
		FHyperAIStudioManagedConfigPendingCodec::Classify(Target, false, FString()),
		EHyperAIStudioTransactionEvidenceMatch::Conflict);

	FString UnknownFieldJson = PendingJson;
	UnknownFieldJson.ReplaceInline(TEXT("\"phase\":"), TEXT("\"unknown\":1,\"phase\":"));
	TestFalse(TEXT("Unknown pending fields are rejected"),
		FHyperAIStudioManagedConfigPendingCodec::Parse(UnknownFieldJson, ParsedPending, Reason));
	FHyperAIStudioManagedConfigPendingTransaction Forged = Pending;
	Forged.TransactionId = TEXT("not-a-transaction-id");
	TestFalse(TEXT("Non-allowlisted transaction ids are rejected"),
		FHyperAIStudioManagedConfigPendingCodec::Serialize(Forged, PendingJson, Reason));
	Forged = Pending;
	Forged.Targets[0].DesiredFileHash = Forged.Targets[0].OriginalFileHash;
	TestFalse(TEXT("Evidence cannot bind identical old and desired states"),
		FHyperAIStudioManagedConfigPendingCodec::Serialize(Forged, PendingJson, Reason));

	FHyperAIStudioManagedConfigSettingsTransaction SettingsEvidence;
	SettingsEvidence.TransactionId = FString::ChrN(32, TEXT('b'));
	SettingsEvidence.Phase = TEXT("settings_replace_prepared");
	SettingsEvidence.bOriginalExists = true;
	SettingsEvidence.OriginalFileHash = HashA;
	SettingsEvidence.StagedFileHash = HashB;
	FString SettingsJson;
	TestTrue(TEXT("Settings replace evidence serializes"),
		FHyperAIStudioManagedConfigSettingsCodec::Serialize(SettingsEvidence, SettingsJson, Reason));
	FHyperAIStudioManagedConfigSettingsTransaction ParsedSettings;
	TestTrue(TEXT("Settings replace evidence parses"),
		FHyperAIStudioManagedConfigSettingsCodec::Parse(SettingsJson, ParsedSettings, Reason));
	TestEqual(TEXT("Exact old settings permit retry"),
		FHyperAIStudioManagedConfigSettingsCodec::Classify(SettingsEvidence, true, HashA),
		EHyperAIStudioTransactionEvidenceMatch::ExactOriginal);
	TestEqual(TEXT("Exact staged settings complete the interrupted replace"),
		FHyperAIStudioManagedConfigSettingsCodec::Classify(SettingsEvidence, true, HashB),
		EHyperAIStudioTransactionEvidenceMatch::ExactDesired);
	TestEqual(TEXT("Third settings bytes block recovery"),
		FHyperAIStudioManagedConfigSettingsCodec::Classify(SettingsEvidence, true,
			TEXT("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc")),
		EHyperAIStudioTransactionEvidenceMatch::Conflict);

	FHyperAIStudioManagedConfigSettingsTransaction IdleSettings;
	TestTrue(TEXT("Canonical idle settings evidence is valid"),
		FHyperAIStudioManagedConfigSettingsCodec::Serialize(IdleSettings, SettingsJson, Reason));
	return true;
}

#endif
