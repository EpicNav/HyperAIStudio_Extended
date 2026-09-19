// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HyperAIStudioSettings.generated.h"

UENUM()
enum class EHyperAIStudioCapabilityPreset : uint8
{
	// Keep these numeric values stable: existing per-project user configs serialize this enum.
	Core = 0 UMETA(DisplayName = "Project Plugins (Recommended)"),
	Domains = 1 UMETA(DisplayName = "Selected Domains"),
	Everything = 2 UMETA(DisplayName = "Everything (Epic All Toolsets)")
};

UENUM()
enum class EHyperAIStudioCapabilityDomain : uint8
{
	Animation UMETA(DisplayName = "Animation"),
	VfxAndSimulation UMETA(DisplayName = "VFX and Simulation"),
	Worldbuilding UMETA(DisplayName = "Worldbuilding"),
	Gameplay UMETA(DisplayName = "Gameplay"),
	UiAndTest UMETA(DisplayName = "UI and Test"),
	ProjectAndPipeline UMETA(DisplayName = "Project and Pipeline")
};

UENUM()
enum class EHyperAIStudioMCPTransport : uint8
{
	StreamableHttp UMETA(DisplayName = "Streamable HTTP"),
	Command UMETA(DisplayName = "Command")
};

/** Runtime policy for native HyperAI tools. Risky effects remain strict in every mode. */
UENUM()
enum class EHyperAIStudioNativeExecutionMode : uint8
{
	Fast UMETA(DisplayName = "Fast (Recommended)"),
	StrictSafety UMETA(DisplayName = "Strict Safety")
};

/** User-facing Unreal MCP tool set. Internal evidence/admission state remains separate. */
UENUM()
enum class EHyperAIStudioNativeToolChannel : uint8
{
	Preview UMETA(DisplayName = "Unreal MCP + Extended Hyper Tools"),
	StableOnly UMETA(DisplayName = "Unreal MCP Only")
};

USTRUCT()
struct FHyperAIStudioMCPServerEntry
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString Id = TEXT("custom-mcp");

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString DisplayName = TEXT("Custom MCP Server");

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString Description;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	EHyperAIStudioMCPTransport Transport = EHyperAIStudioMCPTransport::StreamableHttp;

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::StreamableHttp"))
	FString Url = TEXT("http://127.0.0.1:9000/mcp");

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::Command"))
	FString Command;

	UPROPERTY(Config, EditAnywhere, Category = "MCP", meta = (EditCondition = "Transport == EHyperAIStudioMCPTransport::Command"))
	TArray<FString> Arguments;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	int32 Priority = 100;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> Domains;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> TargetClients;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	TArray<FString> AgentInstructions;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	FString SetupNotes;

	UPROPERTY(Config, EditAnywhere, Category = "MCP")
	bool bEnabled = false;
};

/** Model choices for one HyperAI Chat agent. */
USTRUCT()
struct FHyperAIStudioAgentModelRoute
{
	GENERATED_BODY()

	/** Chat agent this applies to: Codex, Claude Code or Gemini. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString AgentName;

	/** Launch argument with exactly one {0} for the model id. Codex, Claude Code and Gemini all accept --model {0}. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString ModelFlagFormat = TEXT("--model {0}");

	/** Model ids offered in the chat Model menu, spelled exactly as the agent CLI accepts them. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	TArray<FString> Models;

	/** Empty means the agent's own configured default: no model flag is passed. */
	UPROPERTY(Config, EditAnywhere, Category = "Models")
	FString SelectedModel;
};

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (DisplayName = "HyperAIStudio"))
class HYPERAISTUDIO_API UHyperAIStudioSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category = "First Run")
	bool bShowWelcomeOnStartup = true;

	/** Internal one-shot marker used to finish a toolset sync after Unreal restarts. */
	UPROPERTY(Config)
	bool bResumeSetupAfterRestart = false;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP", meta = (ClampMin = "1", ClampMax = "65535"))
	uint32 UnrealMCPPort = 8000;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	FString UnrealMCPUrlPath = TEXT("/mcp");

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	bool bAutoStartUnrealMCP = true;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP")
	bool bEnableToolSearch = true;

	/**
	 * Fast avoids repeated deep trust work for core-owned reads and reversible edits. Destructive
	 * and external-effect routes always retain strict validation, regardless of this setting.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Performance")
	EHyperAIStudioNativeExecutionMode NativeExecutionMode = EHyperAIStudioNativeExecutionMode::Fast;

	/** Select Epic's Unreal MCP alone or add HyperAIStudio's extended tools on the same endpoint. */
	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (DisplayName = "Unreal MCP Tool Set", ConfigRestartRequired = true))
	EHyperAIStudioNativeToolChannel NativeToolChannel = EHyperAIStudioNativeToolChannel::Preview;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (
		DisplayName = "Epic MCP Toolsets",
		ToolTip = "Project Plugins automatically enables the matching Epic MCP toolset wrappers for domain plugins already active in this project. It never enables missing domain plugins."))
	EHyperAIStudioCapabilityPreset CapabilityPreset = EHyperAIStudioCapabilityPreset::Core;

	UPROPERTY(Config, EditAnywhere, Category = "Unreal MCP|Capabilities", meta = (EditCondition = "CapabilityPreset == EHyperAIStudioCapabilityPreset::Domains"))
	TArray<EHyperAIStudioCapabilityDomain> EnabledCapabilityDomains;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableChaosClothAssetToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableLiveCodingToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableMetaHumanGenerator = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableMVVMToolset = false;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category = "Unreal MCP|Capabilities")
	bool bEnableSequencerAnimMixerToolset = false;

	/** Reserved managed-config migration level. Advance only after ownership-safe reconciliation succeeds completely. */
	UPROPERTY(Config, VisibleAnywhere, Category = "Unreal MCP")
	int32 NativeToolArchitectureMigrationVersion = 0;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateCodexConfig = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateClaudeConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateCursorConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateVSCodeConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateGeminiConfig = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Files")
	bool bGenerateClaudeMarkdown = false;

	UPROPERTY(Config, EditAnywhere, Category = "Community MCP")
	TArray<FHyperAIStudioMCPServerEntry> ExtraServers;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	FString PreferredAgent = TEXT("Codex");

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableCodexAgent = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableClaudeAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableGeminiAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableCursorAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bEnableVSCodeAgent = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowClipboardPromptCopy = true;

	/**
	 * Hold every agent asset edit in the chat panel's Activity view until you approve it. Turn this off and
	 * edits run as soon as the agent submits them. Destructive and external-effect tools always need approval,
	 * because their server grant is issued only by that click.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bRequireApprovalForAgentEdits = true;

	/**
	 * Lines that mean an agent is waiting for an answer: a permission prompt, a choice, a confirmation.
	 * Matched case-insensitively against the last rows of that tab's terminal. Empty restores the built-in list.
	 * These exist because agent CLIs change their wording; editing them here beats waiting for a plugin build.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentNeedsInputPatterns;

	/** Lines that mean an agent is showing a plan and waiting to be told to go ahead. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentPlanReviewPatterns;

	/** Lines that mean an agent cannot proceed: a rate limit, a login, a busy editor lane. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Tab Status")
	TArray<FString> AgentBlockedPatterns;

	/** Models offered per chat agent. Agents without an entry get one the first time HyperAI Chat opens. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	TArray<FHyperAIStudioAgentModelRoute> AgentModelRoutes;

	/** Model Claude Code plans with when the Model menu is set to Smart. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	FString SmartPlanModel = TEXT("claude-fable-5-1");

	/** Model Claude Code builds with once a Smart plan is accepted. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	FString SmartBuildModel = TEXT("claude-opus-5");

	/** In Smart mode, answer Claude Code's plan approval with its auto-mode choice so building starts without you. Unreal edits still pass the approval gate. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Models")
	bool bAutoAcceptPlansInSmartMode = true;

	/** Model menu id for Smart: plan with SmartPlanModel, build with SmartBuildModel. */
	static constexpr const TCHAR* SmartModelId = TEXT("smart");

	/** Smart relies on Claude Code's opusplan alias, so only Claude Code offers it. */
	static bool IsSmartModelAgent(const FString& AgentName)
	{
		return AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase);
	}

	/** Shows the fast local title-based comment action in Blueprint/PCG graph context menus. */
	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow|Blueprint Context Menu", meta = (
		DisplayName = "Show Quick Auto Comment in Blueprint Menu",
		ToolTip = "Adds the local title-based Quick Auto Comment action to Blueprint and PCG graph context menus. Smart Auto Comment remains the normal AI-assisted action."))
	bool bShowQuickAutoCommentInBlueprintMenu = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowTerminalLaunch = true;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bAllowExternalAgentLaunch = false;

	UPROPERTY(Config, EditAnywhere, Category = "Agent Workflow")
	bool bRequireConfirmationForDestructiveActions = true;

	UPROPERTY(Config, EditAnywhere, Category = "Documentation")
	FString DocumentationUrl = TEXT("https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/");

	const FHyperAIStudioAgentModelRoute* FindAgentModelRoute(const FString& AgentName) const
	{
		return AgentModelRoutes.FindByPredicate([&AgentName](const FHyperAIStudioAgentModelRoute& Route)
		{
			return Route.AgentName.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}

	FHyperAIStudioAgentModelRoute* FindAgentModelRoute(const FString& AgentName)
	{
		return AgentModelRoutes.FindByPredicate([&AgentName](const FHyperAIStudioAgentModelRoute& Route)
		{
			return Route.AgentName.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}

	/** Adds a route for each bundled chat agent that has none. Returns true if anything was added. */
	bool EnsureDefaultAgentModelRoutes()
	{
		// Claude Code's --help documents these aliases. Codex and Gemini document no model names, so their lists
		// start empty and Default uses whatever model the agent's own config selects.
		struct FSeed
		{
			const TCHAR* AgentName;
			TArray<FString> Models;
		};
		const FSeed Seeds[] = {
			{ TEXT("Codex"), {} },
			{ TEXT("Claude Code"), { TEXT("fable"), TEXT("opus"), TEXT("sonnet") } },
			{ TEXT("Gemini"), {} }
		};

		bool bAdded = false;
		for (const FSeed& Seed : Seeds)
		{
			if (!FindAgentModelRoute(Seed.AgentName))
			{
				FHyperAIStudioAgentModelRoute& Route = AgentModelRoutes.AddDefaulted_GetRef();
				Route.AgentName = Seed.AgentName;
				Route.Models = Seed.Models;
				bAdded = true;
			}
		}
		return bAdded;
	}

	/**
	 * Builds the launch argument for Route's selected model. True with an empty OutArgument means "use the agent's
	 * default". False means the route is unsafe to launch with; OutArgument stays empty and OutError says why.
	 * The argument is spliced into a cmd.exe line, so anything that could chain commands or inject a flag is refused.
	 */
	static bool TryResolveModelArgument(const FHyperAIStudioAgentModelRoute& Route, FString& OutArgument, FString& OutError)
	{
		OutArgument.Reset();
		OutError.Reset();
		const FString& Model = Route.SelectedModel;
		if (Model.IsEmpty())
		{
			return true;
		}

		if (!IsSafeModelId(Model))
		{
			OutError = FString::Printf(TEXT("Model \"%s\" is not a plain model id, so it was not passed to the agent."), *Model);
			return false;
		}
		if (!Route.Models.Contains(Model))
		{
			OutError = FString::Printf(TEXT("Model \"%s\" is not in the %s model list."), *Model, *Route.AgentName);
			return false;
		}

		const FString& Format = Route.ModelFlagFormat;
		const FString ShellCharacters = TEXT("&|<>^\"'%!`");
		const int32 Placeholder = Format.Find(TEXT("{0}"), ESearchCase::CaseSensitive);
		bool bFormatSafe = Format.Len() <= 64
			&& Placeholder != INDEX_NONE
			&& Format.Find(TEXT("{0}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Placeholder + 3) == INDEX_NONE;
		for (const TCHAR Char : Format)
		{
			int32 Unused = INDEX_NONE;
			bFormatSafe &= Char >= 0x20 && Char < 0x7F && !ShellCharacters.FindChar(Char, Unused);
		}
		if (!bFormatSafe)
		{
			OutError = FString::Printf(TEXT("The %s model flag format \"%s\" must contain {0} exactly once and no shell characters."), *Route.AgentName, *Format);
			return false;
		}

		OutArgument = Format.Replace(TEXT("{0}"), *Model, ESearchCase::CaseSensitive);
		return true;
	}

	/** A plain model id. It lands in a cmd.exe line, so no shell characters, and no leading '-' that would read as a flag. */
	static bool IsSafeModelId(const FString& Model)
	{
		auto IsAsciiAlnum = [](TCHAR Char)
		{
			return (Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'));
		};
		bool bSafe = !Model.IsEmpty() && Model.Len() <= 128 && IsAsciiAlnum(Model[0]);
		for (const TCHAR Char : Model)
		{
			bSafe &= IsAsciiAlnum(Char) || Char == TEXT('.') || Char == TEXT('_') || Char == TEXT('-')
				|| Char == TEXT(':') || Char == TEXT('/') || Char == TEXT('@');
		}
		return bSafe;
	}

	/**
	 * Claude Code's Smart launch. The opusplan alias runs whatever `opus` names while in plan mode and whatever
	 * `sonnet` names otherwise; these two variables re-point those names. OutPrefix goes inside the agent's own
	 * launch parentheses so only that process sees them, and the quoted `set "NAME=value"` form keeps cmd from
	 * storing the space before `&&` in the value.
	 */
	static bool TryBuildSmartModelLaunch(const FString& PlanModel, const FString& BuildModel, FString& OutPrefix, FString& OutArgument, FString& OutError)
	{
		OutPrefix.Reset();
		OutArgument.Reset();
		OutError.Reset();
		if (!IsSafeModelId(PlanModel) || !IsSafeModelId(BuildModel))
		{
			OutError = FString::Printf(TEXT("Smart plan model \"%s\" and build model \"%s\" must both be plain model ids."), *PlanModel, *BuildModel);
			return false;
		}
		OutPrefix = FString::Printf(
			TEXT("set \"ANTHROPIC_DEFAULT_OPUS_MODEL=%s\" && set \"ANTHROPIC_DEFAULT_SONNET_MODEL=%s\" && "), *PlanModel, *BuildModel);
		OutArgument = TEXT("--model opusplan");
		return true;
	}

	/** Route's launch prefix and model argument, Smart included. Same contract as TryResolveModelArgument. */
	bool TryResolveModelLaunch(const FHyperAIStudioAgentModelRoute& Route, FString& OutPrefix, FString& OutArgument, FString& OutError) const
	{
		OutPrefix.Reset();
		if (Route.SelectedModel != SmartModelId)
		{
			return TryResolveModelArgument(Route, OutArgument, OutError);
		}
		if (!IsSmartModelAgent(Route.AgentName))
		{
			OutArgument.Reset();
			OutError = FString::Printf(TEXT("Smart is only available for Claude Code, not %s."), *Route.AgentName);
			return false;
		}
		return TryBuildSmartModelLaunch(SmartPlanModel, SmartBuildModel, OutPrefix, OutArgument, OutError);
	}

	bool IsBuiltInAgentEnabled(const FString& AgentName) const
	{
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			return bEnableCodexAgent;
		}
		if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
		{
			return bEnableClaudeAgent;
		}
		if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			return bEnableGeminiAgent;
		}
		if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			return bEnableCursorAgent;
		}
		if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			return bEnableVSCodeAgent;
		}
		return true;
	}

	void SetBuiltInAgentEnabled(const FString& AgentName, bool bEnabled)
	{
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			bEnableCodexAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateCodexConfig = true;
			}
			return;
		}
			if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
			{
				bEnableClaudeAgent = bEnabled;
				if (bEnabled)
				{
					bGenerateClaudeConfig = true;
					bGenerateClaudeMarkdown = true;
				}
				return;
			}
		if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			bEnableGeminiAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateGeminiConfig = true;
			}
			return;
		}
		if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			bEnableCursorAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateCursorConfig = true;
			}
			return;
		}
		if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			bEnableVSCodeAgent = bEnabled;
			if (bEnabled)
			{
				bGenerateVSCodeConfig = true;
			}
		}
	}

	bool EnsureEnabledBuiltInAgentConfigsSelected()
	{
		bool bChanged = false;
		if (bEnableCodexAgent && !bGenerateCodexConfig)
		{
			bGenerateCodexConfig = true;
			bChanged = true;
		}
			if (bEnableClaudeAgent && !bGenerateClaudeConfig)
			{
				bGenerateClaudeConfig = true;
				bChanged = true;
			}
			if (bEnableClaudeAgent && !bGenerateClaudeMarkdown)
			{
				bGenerateClaudeMarkdown = true;
				bChanged = true;
			}
		if (bEnableGeminiAgent && !bGenerateGeminiConfig)
		{
			bGenerateGeminiConfig = true;
			bChanged = true;
		}
		if (bEnableCursorAgent && !bGenerateCursorConfig)
		{
			bGenerateCursorConfig = true;
			bChanged = true;
		}
		if (bEnableVSCodeAgent && !bGenerateVSCodeConfig)
		{
			bGenerateVSCodeConfig = true;
			bChanged = true;
		}
		return bChanged;
	}
};
