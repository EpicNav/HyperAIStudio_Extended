// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioLegacyMigration.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::LegacyMigration::Tests
{
	FHyperAIStudioLegacyProfileInput BaseInput(const FString& ServerId)
	{
		FHyperAIStudioLegacyProfileInput Input;
		Input.ServerId = ServerId;
		Input.Transport = TEXT("stdio");
		return Input;
	}

	FHyperAIStudioLegacyProfileInput DirectUnreal()
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-ue-mcp"));
		const TCHAR HistoricalCommand[] = {
			68, 58, 92, 82, 101, 112, 111, 115, 105, 116, 111, 114, 105, 101, 115,
			92, 85, 69, 53, 77, 67, 80, 92, 77, 67, 80, 83, 101, 114, 118, 101, 114,
			92, 104, 121, 112, 101, 114, 45, 97, 103, 101, 110, 116, 45, 115, 101,
			114, 118, 101, 114, 46, 101, 120, 101, 0};
		Input.Command = HistoricalCommand;
		return Input;
	}

	FHyperAIStudioLegacyProfileInput PowerShellUnreal(const bool bFullLauncher = false)
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-ue-mcp"));
		Input.Command = TEXT("powershell");
		Input.Arguments = {
			TEXT("-NoProfile"),
			TEXT("-ExecutionPolicy"),
			TEXT("Bypass"),
			TEXT("-File"),
			bFullLauncher
				? TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCPFull\\Start-HyperUEMCPFull.ps1")
				: TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCP\\Start-HyperUEMCP.ps1")
		};
		return Input;
	}

	FHyperAIStudioLegacyProfileInput SharedPython(const FString& ServerId, const FString& ProfileFolder)
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(ServerId);
		Input.Command = TEXT("python");
		Input.Arguments = {
			TEXT("D:\\Plugin\\Resources\\MCPServers\\Shared\\mcp_stdio_server.py"),
			TEXT("--manifest"),
			FString::Printf(TEXT("D:\\Plugin\\Resources\\MCPServers\\%s\\manifest.json"), *ProfileFolder)
		};
		return Input;
	}

	FHyperAIStudioLegacyProfileInput FullPython(const FString& ServerId = TEXT("hyper-ue-mcp"))
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(ServerId);
		Input.Command = TEXT("python");
		Input.Arguments = {
			TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCPFull\\RuntimeSource\\hyper_ue_mcp_stdio.py"),
			TEXT("--manifest"),
			TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCPFull\\RuntimeSource\\tool_manifest.json"),
			TEXT("--config"),
			TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCPFull\\RuntimeSource\\config.json")
		};
		return Input;
	}

	FHyperAIStudioLegacyProfileInput PowerShellKnowledge()
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-knowledge-mcp"));
		Input.Command = TEXT("powershell");
		Input.Arguments = {
			TEXT("-NoProfile"),
			TEXT("-ExecutionPolicy"),
			TEXT("Bypass"),
			TEXT("-File"),
			TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperKnowledgeMCP\\Start-HyperKnowledgeMCP.ps1")
		};
		return Input;
	}

	struct FExactCase
	{
		const TCHAR* Label;
		FHyperAIStudioLegacyProfileInput Input;
		EHyperAIStudioLegacyProfileKind Kind;
		EHyperAIStudioLegacyProfileFamily Family;
		const TCHAR* Fingerprint;
	};

	struct FClassificationCase
	{
		const TCHAR* Label;
		FHyperAIStudioLegacyProfileInput Input;
		EHyperAIStudioLegacyProfileClassification Classification;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLegacyProfileRecognizerTest,
	"HyperAIStudio.NativeTools.LegacyMigration.ProfileRecognizer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLegacyProfileRecognizerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LegacyMigration::Tests;

	FHyperAIStudioLegacyProfileInput CaseInsensitiveDirect = DirectUnreal();
	CaseInsensitiveDirect.ServerId = TEXT("  HYPER.UE.MCP  ");
	CaseInsensitiveDirect.Transport = TEXT(" Command ");
	CaseInsensitiveDirect.Command.ReplaceInline(TEXT("\\"), TEXT("/"));
	CaseInsensitiveDirect.Command = CaseInsensitiveDirect.Command.ToLower();

	FHyperAIStudioLegacyProfileInput CaseInsensitivePowerShell = PowerShellUnreal();
	CaseInsensitivePowerShell.Command = TEXT("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\POWERSHELL.EXE");
	CaseInsensitivePowerShell.Arguments[4] = TEXT("c:/PLUGIN/resources/mcpservers/hyperuemcp/start-hyperuemcp.PS1");

	FHyperAIStudioLegacyProfileInput CaseInsensitivePython = SharedPython(
		TEXT("HYPER-KNOWLEDGE-MCP"),
		TEXT("HyperKnowledgeMCP"));
	CaseInsensitivePython.Command = TEXT("C:\\Python\\PYTHON.EXE");
	CaseInsensitivePython.Arguments[0] = TEXT("c:/plugin/RESOURCES/mcpservers/shared/MCP_STDIO_SERVER.PY");

	const TArray<FExactCase> ExactCases = {
		{
			TEXT("Historical direct executable"),
			DirectUnreal(),
			EHyperAIStudioLegacyProfileKind::DirectHistoricalExecutable,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.direct-executable.v2")
		},
		{
			TEXT("Direct executable comparison normalizes id, path case, and separators"),
			CaseInsensitiveDirect,
			EHyperAIStudioLegacyProfileKind::DirectHistoricalExecutable,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.direct-executable.v2")
		},
		{
			TEXT("Historical packaged HyperUEMCP PowerShell launcher"),
			PowerShellUnreal(),
			EHyperAIStudioLegacyProfileKind::PowerShellHyperUEMCPLauncher,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.powershell-wrapper.v1")
		},
		{
			TEXT("PowerShell basename and packaged paths compare case-insensitively"),
			CaseInsensitivePowerShell,
			EHyperAIStudioLegacyProfileKind::PowerShellHyperUEMCPLauncher,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.powershell-wrapper.v1")
		},
		{
			TEXT("Historical packaged HyperUEMCPFull PowerShell launcher"),
			PowerShellUnreal(true),
			EHyperAIStudioLegacyProfileKind::PowerShellHyperUEMCPFullLauncher,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.powershell-full-wrapper.v1")
		},
		{
			TEXT("Historical Shared Python Unreal wrapper"),
			SharedPython(TEXT("hyper-ue-mcp"), TEXT("HyperUEMCP")),
			EHyperAIStudioLegacyProfileKind::SharedPythonUnrealWrapper,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.shared-python-wrapper.v1")
		},
		{
			TEXT("Historical full-manifest Python wrapper"),
			FullPython(),
			EHyperAIStudioLegacyProfileKind::FullManifestPythonWrapper,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.full-manifest-wrapper.v1")
		},
		{
			TEXT("Packaged manifest id matches only the exact full wrapper"),
			FullPython(TEXT("hyper-ue-mcp-full")),
			EHyperAIStudioLegacyProfileKind::FullManifestPythonWrapper,
			EHyperAIStudioLegacyProfileFamily::Unreal,
			TEXT("hyperai.legacy.unreal.full-manifest-wrapper.v1")
		},
		{
			TEXT("Historical Hyper Knowledge PowerShell launcher"),
			PowerShellKnowledge(),
			EHyperAIStudioLegacyProfileKind::PowerShellHyperKnowledgeLauncher,
			EHyperAIStudioLegacyProfileFamily::Knowledge,
			TEXT("hyperai.legacy.knowledge.powershell-wrapper.v1")
		},
		{
			TEXT("Historical Hyper Knowledge Shared Python wrapper"),
			SharedPython(TEXT("hyper-knowledge-mcp"), TEXT("HyperKnowledgeMCP")),
			EHyperAIStudioLegacyProfileKind::SharedPythonKnowledgeWrapper,
			EHyperAIStudioLegacyProfileFamily::Knowledge,
			TEXT("hyperai.legacy.knowledge.shared-python-wrapper.v1")
		},
		{
			TEXT("Python basename and packaged paths compare case-insensitively"),
			CaseInsensitivePython,
			EHyperAIStudioLegacyProfileKind::SharedPythonKnowledgeWrapper,
			EHyperAIStudioLegacyProfileFamily::Knowledge,
			TEXT("hyperai.legacy.knowledge.shared-python-wrapper.v1")
		}
	};

	const TArray<FString> ExpectedHistoricalJsonClients = {
		TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini")
	};
	for (const FExactCase& ExactCase : ExactCases)
	{
		const FHyperAIStudioLegacyProfileMatch Match =
			FHyperAIStudioLegacyProfileRecognizer::Recognize(ExactCase.Input);
		TestEqual(ExactCase.Label, Match.Classification, EHyperAIStudioLegacyProfileClassification::ExactOwned);
		TestEqual(TEXT("Exact historical kind"), Match.Kind, ExactCase.Kind);
		TestEqual(TEXT("Exact historical family"), Match.Family, ExactCase.Family);
		TestEqual(TEXT("Stable, non-secret fingerprint"), Match.Fingerprint, FString(ExactCase.Fingerprint));
		TestTrue(TEXT("Exact ownership flag"), Match.bExactBuiltInMatch);
		TestTrue(TEXT("Exact signatures use a reserved id"), Match.bUsesReservedId);
		TestTrue(TEXT("Exact historical Unreal/Knowledge profile is retired"),
			FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(Match));
		const TArray<FString> HistoricalJsonClients =
			FHyperAIStudioLegacyProfileRecognizer::GetHistoricalJsonClientIdsForCleanup(Match);
		TestTrue(
			TEXT("Exact historical profiles bind cleanup to all four legacy JSON clients"),
			HistoricalJsonClients == ExpectedHistoricalJsonClients);
	}

	TArray<FClassificationCase> ClassificationCases;
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-ue-mcp"));
		ClassificationCases.Add({ TEXT("Reserved id alone is only customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-knowledge-mcp"));
		ClassificationCases.Add({ TEXT("Reserved Knowledge id alone is only customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.ServerId = TEXT("my-hyper-ue-mcp");
		ClassificationCases.Add({ TEXT("Exact-looking invocation under a foreign id is foreign"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Foreign });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("third-party-server"));
		Input.Transport = TEXT("streamable-http");
		Input.Url = TEXT("http://127.0.0.1:55557/mcp");
		ClassificationCases.Add({ TEXT("Port alone is foreign evidence"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Foreign });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = BaseInput(TEXT("hyper-ue-mcp"));
		Input.Transport = TEXT("streamable-http");
		Input.Url = TEXT("http://127.0.0.1:8765/mcp");
		ClassificationCases.Add({ TEXT("Reserved id with custom HTTP transport is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = PowerShellUnreal();
		Swap(Input.Arguments[0], Input.Arguments[1]);
		ClassificationCases.Add({ TEXT("PowerShell switch order change is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = PowerShellUnreal();
		Input.Arguments.RemoveAt(2);
		ClassificationCases.Add({ TEXT("Missing PowerShell switch is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = PowerShellUnreal();
		Input.Arguments.Add(TEXT("-Install"));
		ClassificationCases.Add({ TEXT("Extra PowerShell argument is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = PowerShellUnreal();
		Input.Command = TEXT("pwsh");
		ClassificationCases.Add({ TEXT("Alternate PowerShell launcher is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = PowerShellUnreal();
		Input.Arguments[4] = TEXT("D:\\Custom\\HyperUEMCP\\Start.ps1");
		ClassificationCases.Add({ TEXT("Merely containing HyperUEMCP is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = SharedPython(TEXT("hyper-ue-mcp"), TEXT("HyperUEMCP"));
		Input.Command = TEXT("python3");
		ClassificationCases.Add({ TEXT("Changed Python command is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = SharedPython(TEXT("hyper-ue-mcp"), TEXT("HyperUEMCP"));
		Input.Arguments[2] = TEXT("D:\\Custom\\HyperUEMCP\\manifest.json");
		ClassificationCases.Add({ TEXT("Changed Shared manifest path is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = FullPython();
		Swap(Input.Arguments[1], Input.Arguments[3]);
		ClassificationCases.Add({ TEXT("Full-wrapper option order change is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = FullPython();
		Input.Arguments[4] = TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCPFull\\RuntimeSource\\custom-config.json");
		ClassificationCases.Add({ TEXT("Changed Full config path is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = FullPython();
		Input.Arguments.Add(TEXT("--verbose"));
		ClassificationCases.Add({ TEXT("Extra Full-wrapper argument is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = SharedPython(TEXT("hyper-ue-mcp-full"), TEXT("HyperUEMCP"));
		ClassificationCases.Add({ TEXT("Packaged full id rejects the Shared wrapper"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.ServerId = TEXT("hyper-ue-mcp-full");
		ClassificationCases.Add({ TEXT("Packaged full id rejects the direct executable"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.bHasEnvironment = true;
		ClassificationCases.Add({ TEXT("Environment field makes the reserved profile customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.bHasUnrecognizedFields = true;
		ClassificationCases.Add({ TEXT("Unrecognized field makes the reserved profile customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Url = TEXT("http://127.0.0.1:8765");
		ClassificationCases.Add({ TEXT("Non-empty URL makes the reserved profile customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = SharedPython(TEXT("hyper-knowledge-mcp"), TEXT("HyperKnowledgeMCP"));
		Input.ServerId = TEXT("hyper-ue-mcp");
		ClassificationCases.Add({ TEXT("Knowledge invocation under Unreal id is customized"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = SharedPython(TEXT("hyper-knowledge-mcp"), TEXT("HyperKnowledgeMCP"));
		Input.Arguments.Add(TEXT("--customized"));
		ClassificationCases.Add({ TEXT("Customized Knowledge invocation is never exact-owned"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::CustomizedReservedId });
	}

	for (const FClassificationCase& ClassificationCase : ClassificationCases)
	{
		const FHyperAIStudioLegacyProfileMatch Match =
			FHyperAIStudioLegacyProfileRecognizer::Recognize(ClassificationCase.Input);
		TestEqual(ClassificationCase.Label, Match.Classification, ClassificationCase.Classification);
		TestEqual(TEXT("Non-owned profiles expose no historical kind"), Match.Kind, EHyperAIStudioLegacyProfileKind::None);
		TestFalse(TEXT("Non-owned profiles never set exact ownership"), Match.bExactBuiltInMatch);
		TestFalse(TEXT("Customized, foreign, and reserved-id-only profiles are preserved"),
			FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(Match));
		TestTrue(TEXT("Non-owned profiles expose no ownership fingerprint"), Match.Fingerprint.IsEmpty());
		TestTrue(
			TEXT("Non-owned profiles bind no historical JSON cleanup clients"),
			FHyperAIStudioLegacyProfileRecognizer::GetHistoricalJsonClientIdsForCleanup(Match).IsEmpty());
		if (ClassificationCase.Classification == EHyperAIStudioLegacyProfileClassification::CustomizedReservedId)
		{
			TestTrue(TEXT("Customized classification records only that the id is reserved"), Match.bUsesReservedId);
		}
		else if (ClassificationCase.Classification == EHyperAIStudioLegacyProfileClassification::Foreign)
		{
			TestFalse(TEXT("Foreign classification does not reserve its id"), Match.bUsesReservedId);
		}
	}

	TArray<FClassificationCase> MalformedCases;
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.bStructurallyMalformed = true;
		MalformedCases.Add({ TEXT("Parser-reported structural error is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.ServerId.Reset();
		MalformedCases.Add({ TEXT("Empty server id is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.ServerId = FString::ChrN(FHyperAIStudioLegacyProfileRecognizer::MaxServerIdCharacters + 1, TEXT('a'));
		MalformedCases.Add({ TEXT("Oversized server id is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Transport = FString::ChrN(FHyperAIStudioLegacyProfileRecognizer::MaxTransportCharacters + 1, TEXT('s'));
		MalformedCases.Add({ TEXT("Oversized transport is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Command = FString::ChrN(FHyperAIStudioLegacyProfileRecognizer::MaxCommandCharacters + 1, TEXT('c'));
		MalformedCases.Add({ TEXT("Oversized command is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Url = FString::ChrN(FHyperAIStudioLegacyProfileRecognizer::MaxUrlCharacters + 1, TEXT('u'));
		MalformedCases.Add({ TEXT("Oversized URL is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Arguments.SetNum(FHyperAIStudioLegacyProfileRecognizer::MaxArguments + 1);
		MalformedCases.Add({ TEXT("Excessive argument count is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		Input.Arguments.Add(FString::ChrN(FHyperAIStudioLegacyProfileRecognizer::MaxArgumentCharacters + 1, TEXT('a')));
		MalformedCases.Add({ TEXT("Oversized argument is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Input.Arguments.Add(FString::ChrN(7000, TEXT('a')));
		}
		MalformedCases.Add({ TEXT("Oversized total argument payload is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}
	{
		FHyperAIStudioLegacyProfileInput Input = DirectUnreal();
		// A trailing NUL is FString's normal terminator. Insert one inside the
		// logical payload so this fixture exercises a true embedded-NUL value.
		Input.Command.GetCharArray().Insert(TEXT('\0'), FMath::Min(2, Input.Command.Len()));
		MalformedCases.Add({ TEXT("Embedded null is malformed"), MoveTemp(Input), EHyperAIStudioLegacyProfileClassification::Malformed });
	}

	for (const FClassificationCase& MalformedCase : MalformedCases)
	{
		const FHyperAIStudioLegacyProfileMatch Match =
			FHyperAIStudioLegacyProfileRecognizer::Recognize(MalformedCase.Input);
		TestEqual(MalformedCase.Label, Match.Classification, EHyperAIStudioLegacyProfileClassification::Malformed);
		TestEqual(TEXT("Malformed input exposes no historical kind"), Match.Kind, EHyperAIStudioLegacyProfileKind::None);
		TestFalse(TEXT("Malformed input never sets exact ownership"), Match.bExactBuiltInMatch);
		TestTrue(TEXT("Malformed input exposes no ownership fingerprint"), Match.Fingerprint.IsEmpty());
		TestFalse(TEXT("Malformed input has a stable reason code"), Match.ReasonCode.IsEmpty());
	}

	return true;
}

#endif
