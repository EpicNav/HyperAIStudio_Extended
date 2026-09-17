// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentModelTest,
	"HyperAIStudio.Chat.AgentModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentModelTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioAgentModelRoute Route;
	Route.AgentName = TEXT("Claude Code");
	Route.Models = { TEXT("opus"), TEXT("claude-fable-5"), TEXT("vendor/model@2026") };

	FString Argument;
	FString Error;
	auto Resolve = [&](const FString& Selected, const FString& Format)
	{
		Route.SelectedModel = Selected;
		Route.ModelFlagFormat = Format;
		return UHyperAIStudioSettings::TryResolveModelArgument(Route, Argument, Error);
	};

	TestTrue(TEXT("No selection resolves"), Resolve(FString(), TEXT("--model {0}")));
	TestTrue(TEXT("No selection passes no flag, so the agent keeps its own default"), Argument.IsEmpty());

	TestTrue(TEXT("A listed model resolves"), Resolve(TEXT("opus"), TEXT("--model {0}")));
	TestEqual(TEXT("The flag format is filled in"), Argument, FString(TEXT("--model opus")));
	TestTrue(TEXT("Ids with dots, slashes and @ are allowed"), Resolve(TEXT("vendor/model@2026"), TEXT("-m {0}")));
	TestEqual(TEXT("Alternate flag spelling"), Argument, FString(TEXT("-m vendor/model@2026")));

	// Everything below reaches a cmd.exe line, so each case must fail closed with no argument.
	auto ExpectRejected = [&](const TCHAR* What, const FString& Selected, const FString& Format)
	{
		const bool bResolved = Resolve(Selected, Format);
		TestFalse(What, bResolved);
		TestTrue(*FString::Printf(TEXT("%s: no argument is produced"), What), Argument.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s: an error explains why"), What), Error.IsEmpty());
	};

	ExpectRejected(TEXT("A model not on the curated list"), TEXT("sonnet"), TEXT("--model {0}"));
	Route.Models.Add(TEXT("opus & calc"));
	ExpectRejected(TEXT("A listed id with a command separator"), TEXT("opus & calc"), TEXT("--model {0}"));
	Route.Models.Add(TEXT("--dangerously-skip-permissions"));
	ExpectRejected(TEXT("A listed id that starts with a dash would be read as a flag"), TEXT("--dangerously-skip-permissions"), TEXT("--model {0}"));
	ExpectRejected(TEXT("A format without a placeholder"), TEXT("opus"), TEXT("--model"));
	ExpectRejected(TEXT("A format with two placeholders"), TEXT("opus"), TEXT("--model {0} {0}"));
	ExpectRejected(TEXT("A format with a shell pipe"), TEXT("opus"), TEXT("--model {0} | calc"));
	ExpectRejected(TEXT("A format with a quote"), TEXT("opus"), TEXT("--model \"{0}\""));
	ExpectRejected(TEXT("An empty format"), TEXT("opus"), FString());

	// NewObject copies the CDO, which HyperAI Chat may already have seeded; start genuinely empty.
	UHyperAIStudioSettings* Settings = NewObject<UHyperAIStudioSettings>(GetTransientPackage());
	Settings->AgentModelRoutes.Reset();
	TestTrue(TEXT("Defaults are added to empty settings"), Settings->EnsureDefaultAgentModelRoutes());
	TestFalse(TEXT("Adding defaults again changes nothing"), Settings->EnsureDefaultAgentModelRoutes());
	TestNotNull(TEXT("Codex has a route"), Settings->FindAgentModelRoute(TEXT("Codex")));
	TestNotNull(TEXT("Lookup ignores case"), Settings->FindAgentModelRoute(TEXT("claude code")));
	if (const FHyperAIStudioAgentModelRoute* Claude = Settings->FindAgentModelRoute(TEXT("Claude Code")))
	{
		TestTrue(TEXT("Claude Code is seeded with the aliases its --help documents"), Claude->Models.Contains(TEXT("opus")));
		TestTrue(TEXT("Seeded routes default to the agent's own model"), Claude->SelectedModel.IsEmpty());

		FHyperAIStudioAgentModelRoute Seeded = *Claude;
		Seeded.SelectedModel = TEXT("sonnet");
		TestTrue(TEXT("Every seeded route resolves with a seeded model"), UHyperAIStudioSettings::TryResolveModelArgument(Seeded, Argument, Error));
	}
	return true;
}

#endif
