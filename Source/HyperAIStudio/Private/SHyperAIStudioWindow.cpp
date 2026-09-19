// Games by Hyper 2026.

#include "SHyperAIStudioWindow.h"

#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HyperAIStudioCapabilityCatalogCounts.h"
#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioLegacyMigration.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioToolInventoryViewModel.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "UnrealEdMisc.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioWindow"

namespace HyperAIStudio::Private
{
	using FToolInventoryItem = TSharedPtr<FHyperAIStudioToolInventoryRow>;

	struct FToolInventoryDialogState
	{
		FHyperAIStudioToolInventoryView View;
		TArray<FToolInventoryItem> AllRows;
		TArray<FToolInventoryItem> FilteredRows;
		TOptional<EHyperAIStudioToolInventorySource> SourceFilter;
		FString Query;
		FString RefreshNotice;
		bool bRefreshing = false;
		TWeakPtr<SListView<FToolInventoryItem>> ListView;
		TFunction<void()> RefreshInventory;

		void SetView(FHyperAIStudioToolInventoryView InView)
		{
			View = MoveTemp(InView);
			AllRows.Reset(View.Rows.Num());
			for (const FHyperAIStudioToolInventoryRow& Row : View.Rows)
			{
				AllRows.Add(MakeShared<FHyperAIStudioToolInventoryRow>(Row));
			}
			RefreshFilter();
		}

		void RefreshFilter()
		{
			FilteredRows.Reset(AllRows.Num());
			for (const FToolInventoryItem& Row : AllRows)
			{
				if (Row.IsValid()
					&& FHyperAIStudioToolInventoryViewModel::Matches(*Row, Query, SourceFilter))
				{
					FilteredRows.Add(Row);
				}
			}
			if (const TSharedPtr<SListView<FToolInventoryItem>> PinnedList = ListView.Pin())
			{
				PinnedList->RequestListRefresh();
			}
		}

		FText GetSourceFilterText() const
		{
			if (!SourceFilter.IsSet())
			{
				return LOCTEXT("ToolInventorySourceAll", "All sources");
			}
			return SourceFilter.GetValue() == EHyperAIStudioToolInventorySource::Epic
				? LOCTEXT("ToolInventorySourceEpic", "Epic")
				: LOCTEXT("ToolInventorySourceHyperAI", "HyperAI");
		}

		FText GetSummaryText() const
		{
			if (!View.bExactLiveInventory)
			{
				return FText::FromString(View.Message.IsEmpty()
					? TEXT("Loading the exact live Epic tool inventory...")
					: View.Message);
			}
			FString Summary = FString::Printf(
				TEXT("%d tools available now — %d Epic + %d HyperAI  |  %d toolsets (%d Epic / %d HyperAI)  |  %d discovery dispatchers"),
				View.GetAvailableToolCount(),
				View.LiveEpicToolCount,
				View.AvailableHyperToolCount,
				View.LiveToolsetCount,
				View.LiveEpicToolsetCount,
				View.LiveHyperToolsetCount,
				View.DispatcherCount);
			if (bRefreshing)
			{
				Summary += TEXT("  |  Refreshing...");
			}
			else if (!RefreshNotice.IsEmpty())
			{
				Summary += TEXT("  |  ") + RefreshNotice;
			}
			return FText::FromString(MoveTemp(Summary));
		}
	};

	FSlateColor GoodColor()
	{
		return FSlateColor(FLinearColor(0.18f, 0.68f, 0.38f));
	}

	FSlateColor WarningColor()
	{
		return FSlateColor(FLinearColor(0.93f, 0.62f, 0.18f));
	}

	FSlateColor ErrorColor()
	{
		return FSlateColor(FLinearColor(0.92f, 0.24f, 0.24f));
	}

	FSlateColor MutedColor()
	{
		return FSlateColor(FLinearColor(0.62f, 0.66f, 0.70f));
	}

	ECheckBoxState CheckedState(bool bValue)
	{
		return bValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}

	const FHyperAIStudioMCPServerEntry* FindMCPServerEntryById(const UHyperAIStudioSettings* Settings, const FString& ServerId)
	{
		if (!Settings)
		{
			return nullptr;
		}

		return Settings->ExtraServers.FindByPredicate([&ServerId](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return Entry.Id.Equals(ServerId, ESearchCase::IgnoreCase);
		});
	}

	FString BuildInstallCommandForTerminal(const FString& PrerequisiteId, const FString& InstallCommand)
	{
		if (PrerequisiteId.Equals(TEXT("gemini"), ESearchCase::IgnoreCase))
		{
			return TEXT("where npm >nul 2>nul && npm install -g @google/gemini-cli || echo Gemini CLI install needs Node.js 20+ and npm. Run Install Missing Prerequisites first if npm is missing.");
		}
		return InstallCommand.TrimStartAndEnd();
	}

	FString BuildPrerequisiteTerminalCommand(const FString& PrerequisiteId, const FString&, const FString& InstallCommand, const FString&)
	{
		return BuildInstallCommandForTerminal(PrerequisiteId, InstallCommand);
	}

	bool IsMissingInstallablePrerequisite(const FHyperAIStudioPrerequisiteStatus& Entry)
	{
		return !Entry.bRequired && !Entry.bAvailable && !Entry.InstallCommand.TrimStartAndEnd().IsEmpty();
	}

	FString GetAgentNameForPrerequisiteId(const FString& Id)
	{
		if (Id.Equals(TEXT("codex"), ESearchCase::IgnoreCase))
		{
			return TEXT("Codex");
		}
		if (Id.Equals(TEXT("claude"), ESearchCase::IgnoreCase))
		{
			return TEXT("Claude Code");
		}
		if (Id.Equals(TEXT("gemini"), ESearchCase::IgnoreCase))
		{
			return TEXT("Gemini");
		}
		if (Id.Equals(TEXT("cursor"), ESearchCase::IgnoreCase))
		{
			return TEXT("Cursor");
		}
		if (Id.Equals(TEXT("vscode"), ESearchCase::IgnoreCase))
		{
			return TEXT("VS Code/Copilot");
		}
		return FString();
	}

	bool IsEnabledAgentInstallPrerequisite(const FHyperAIStudioPrerequisiteStatus& Entry)
	{
		const FString AgentName = GetAgentNameForPrerequisiteId(Entry.Id);
		if (AgentName.IsEmpty())
		{
			return false;
		}

		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		return !Settings || Settings->IsBuiltInAgentEnabled(AgentName);
	}

	int32 GetPrerequisiteInstallPriority(const FString& Id)
	{
		if (Id.Equals(TEXT("node"), ESearchCase::IgnoreCase)
			|| Id.Equals(TEXT("npm"), ESearchCase::IgnoreCase)
			|| Id.Equals(TEXT("npx"), ESearchCase::IgnoreCase))
		{
			return 10;
		}
		if (Id.Equals(TEXT("git"), ESearchCase::IgnoreCase))
		{
			return 20;
		}
		if (Id.Equals(TEXT("codex"), ESearchCase::IgnoreCase)
			|| Id.Equals(TEXT("claude"), ESearchCase::IgnoreCase)
			|| Id.Equals(TEXT("uv"), ESearchCase::IgnoreCase))
		{
			return 30;
		}
		if (Id.Equals(TEXT("gemini"), ESearchCase::IgnoreCase))
		{
			return 40;
		}
		return 50;
	}

	FString BuildMissingPrerequisitesTerminalCommand(const TArray<FHyperAIStudioPrerequisiteStatus>& Prerequisites, const FString&)
	{
		struct FQueuedInstallCommand
		{
			FString Key;
			FString TerminalCommand;
			FString DisplayNames;
			int32 Priority = 50;
		};

		TArray<FQueuedInstallCommand> InstallCommands;
		for (const FHyperAIStudioPrerequisiteStatus& Entry : Prerequisites)
		{
			if (!IsMissingInstallablePrerequisite(Entry))
			{
				continue;
			}

			const FString TerminalCommand = BuildInstallCommandForTerminal(Entry.Id, Entry.InstallCommand);
			if (TerminalCommand.IsEmpty())
			{
				continue;
			}

			const FString Key = TerminalCommand.ToLower();
			FQueuedInstallCommand* Existing = InstallCommands.FindByPredicate([&Key](const FQueuedInstallCommand& Candidate)
			{
				return Candidate.Key == Key;
			});
			if (Existing)
			{
				Existing->DisplayNames += FString::Printf(TEXT(", %s"), *Entry.DisplayName);
				Existing->Priority = FMath::Min(Existing->Priority, GetPrerequisiteInstallPriority(Entry.Id));
				continue;
			}

			FQueuedInstallCommand Queued;
			Queued.Key = Key;
			Queued.TerminalCommand = TerminalCommand;
			Queued.DisplayNames = Entry.DisplayName;
			Queued.Priority = GetPrerequisiteInstallPriority(Entry.Id);
			InstallCommands.Add(Queued);
		}

		if (InstallCommands.IsEmpty())
		{
			return FString();
		}

		InstallCommands.Sort([](const FQueuedInstallCommand& Left, const FQueuedInstallCommand& Right)
		{
			if (Left.Priority == Right.Priority)
			{
				return Left.DisplayNames < Right.DisplayNames;
			}
			return Left.Priority < Right.Priority;
		});

		TArray<FString> Commands;
		for (const FQueuedInstallCommand& Queued : InstallCommands)
		{
			Commands.Add(Queued.TerminalCommand);
		}
		return FString::Join(Commands, TEXT(" & "));
	}

	bool ProjectRelativePathExists(const FString& RelativePath)
	{
		const FString FullPath = FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), RelativePath);
		return FPaths::FileExists(FullPath) || FPaths::DirectoryExists(FullPath);
	}

	FString GetConfigPathForAgentName(const FString& AgentName)
	{
		if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
		{
			return TEXT(".mcp.json");
		}
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			return TEXT(".codex/config.toml");
		}
		if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			return TEXT(".gemini/settings.json");
		}
		if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			return TEXT(".cursor/mcp.json");
		}
		if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			return TEXT(".vscode/mcp.json");
		}
		return FString();
	}

	FString GetInstructionFileForAgentName(const FString& AgentName)
	{
		return AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
			? FString(TEXT("CLAUDE.md"))
			: FString(TEXT("AGENTS.md"));
	}

	void SelectGenerationForProjectRelativePath(UHyperAIStudioSettings& Settings, const FString& RelativePath)
	{
		if (RelativePath.Equals(TEXT(".codex/config.toml"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableCodexAgent = true;
			Settings.bGenerateCodexConfig = true;
		}
		else if (RelativePath.Equals(TEXT(".mcp.json"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableClaudeAgent = true;
			Settings.bGenerateClaudeConfig = true;
			Settings.bGenerateClaudeMarkdown = true;
		}
		else if (RelativePath.Equals(TEXT("CLAUDE.md"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableClaudeAgent = true;
			Settings.bGenerateClaudeMarkdown = true;
		}
		else if (RelativePath.Equals(TEXT(".cursor/mcp.json"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableCursorAgent = true;
			Settings.bGenerateCursorConfig = true;
		}
		else if (RelativePath.Equals(TEXT(".vscode/mcp.json"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableVSCodeAgent = true;
			Settings.bGenerateVSCodeConfig = true;
		}
		else if (RelativePath.Equals(TEXT(".gemini/settings.json"), ESearchCase::IgnoreCase))
		{
			Settings.bEnableGeminiAgent = true;
			Settings.bGenerateGeminiConfig = true;
		}
	}

	FString FilterToolDialogDetails(const FString& Details, const FString& Query)
	{
		const FString CleanQuery = Query.TrimStartAndEnd();
		if (CleanQuery.IsEmpty())
		{
			return Details;
		}

		TArray<FString> Lines;
		Details.ParseIntoArrayLines(Lines);
		TArray<FString> Matches;
		for (const FString& Line : Lines)
		{
			if (Line.Contains(CleanQuery, ESearchCase::IgnoreCase))
			{
				Matches.Add(Line);
			}
		}

		if (Matches.IsEmpty())
		{
			return FString::Printf(TEXT("No tools matched `%s`."), *CleanQuery);
		}
		return FString::Join(Matches, LINE_TERMINATOR);
	}

	FText GetProbeStatusText(const FHyperAIStudioStatus& Status)
	{
		if (Status.bToolsListReachable)
		{
			return LOCTEXT("ProbeStatusReady", "Ready");
		}
		if (Status.bProbeInProgress)
		{
			return LOCTEXT("ProbeStatusChecking", "Checking...");
		}
		if (!Status.bHasProbeRun)
		{
			return LOCTEXT("ProbeStatusNotChecked", "Not checked yet");
		}
		return LOCTEXT("ProbeStatusFailed", "Probe failed");
	}

	FText GetProbeStatusDetailText(const FHyperAIStudioStatus& Status)
	{
		if (Status.bToolsListReachable)
		{
			if (Status.bToolSearchMode)
			{
					return FText::FromString(FString::Printf(
						TEXT("Unreal MCP is ready in tool-search mode: %d discovery dispatchers are advertised (not the tool total). Open Tools to load the exact live Epic and HyperAI inventory."),
						Status.ProbeToolCount));
			}
			return Status.ProbeMessage.IsEmpty()
				? LOCTEXT("ProbeDetailReady", "tools/list is reachable.")
				: FText::FromString(Status.ProbeMessage);
		}
		if (Status.bProbeInProgress)
		{
			return FText::FromString(FString::Printf(TEXT("Checking %s with initialize and tools/list."), Status.Endpoint.IsEmpty() ? TEXT("the configured endpoint") : *Status.Endpoint));
		}
		if (!Status.bHasProbeRun)
		{
			return Status.bServerRunning && Status.bPortListening
				? LOCTEXT("ProbeDetailNotCheckedServerReady", "Server and port look ready; click Verify to run the MCP tools/list handshake.")
				: LOCTEXT("ProbeDetailNotCheckedServerNotReady", "Start setup or start Unreal MCP, then verify the endpoint.");
		}
		return Status.ProbeMessage.IsEmpty()
			? LOCTEXT("ProbeDetailFailedUnknown", "The MCP tools/list handshake did not complete.")
			: FText::FromString(Status.ProbeMessage);
	}

	FSlateColor GetProbeStatusColor(const FHyperAIStudioStatus& Status)
	{
		if (Status.bToolsListReachable)
		{
			return GoodColor();
		}
		if (Status.bProbeInProgress || !Status.bHasProbeRun)
		{
			return WarningColor();
		}
		return ErrorColor();
	}
}

int32 SHyperAIStudioWindow::GetInitialTabIndexForStatus(const FHyperAIStudioStatus& InStatus)
{
	return InStatus.IsReady() ? 1 : 0;
}

int32 SHyperAIStudioWindow::GetPostRefreshTabIndex(int32 CurrentTabIndex, const FHyperAIStudioStatus& NewStatus, bool bUserSelectedTab, bool bAutoRoutePending)
{
	if (bAutoRoutePending && !bUserSelectedTab && CurrentTabIndex == 0 && NewStatus.IsReady())
	{
		return 1;
	}
	return CurrentTabIndex;
}

FText SHyperAIStudioWindow::GetTerminalPanelHintText()
{
	return LOCTEXT("TerminalPanelHint", "Install buttons run visible commands in HyperAI Chat's embedded terminal after resetting any active agent session. For manual startup, use the Unreal Terminal when it is available. If Terminal is missing or disabled, enable the Terminal plugin and restart the editor; meanwhile use Copy Startup Commands and run them from Windows Terminal, PowerShell, or CMD.");
}

FString SHyperAIStudioWindow::GetTerminalStartupRecipeText(const FString& ProjectRoot)
{
	return FString::Printf(
		TEXT("set TERM=xterm-256color\ncd /d \"%s\"\ncodex\nREM or run: claude\nREM or run: gemini\nREM or open editor agents: cursor .\nREM or: code ."),
		*ProjectRoot);
}

FHyperAIStudioWorkbenchSurfaceSmoke SHyperAIStudioWindow::GetWorkbenchSurfaceSmoke()
{
	FHyperAIStudioWorkbenchSurfaceSmoke Surface;
	Surface.NavigationTabs = {
		TEXT("Setup"),
		TEXT("Agents"),
		TEXT("MCP"),
		TEXT("Docs")
	};
	Surface.FirstRunLabels = {
		TEXT("HyperAIStudio"),
		TEXT("Start Setup"),
		TEXT("Install Prerequisites"),
		TEXT("Verify"),
		TEXT("Sync Toolsets"),
		TEXT("Configure Agents"),
		TEXT("View Prerequisites"),
		TEXT("Open Chat / Terminal"),
		TEXT("Agent Files"),
		TEXT("Open")
	};
	Surface.AgentTargetActions = {
		TEXT("Enabled"),
		TEXT("Use in Chat"),
		TEXT("Copy Codex Prompt"),
		TEXT("Copy Claude Prompt"),
		TEXT("Copy Gemini Prompt"),
		TEXT("Copy Cursor Prompt"),
		TEXT("Copy VS Code Prompt"),
		TEXT("Generate Config"),
		TEXT("Files"),
		TEXT("Install")
	};
	Surface.HandoffActions = {
		TEXT("HyperAI Chat"),
		TEXT("Ready"),
		TEXT("Needs setup"),
		TEXT("Agent"),
		TEXT("Model"),
		TEXT("Context"),
		TEXT("Copy Prompt"),
		TEXT("Stop Agent"),
		TEXT("Open Setup")
	};
	Surface.AdvancedLabels = {
		TEXT("Prerequisites"),
		TEXT("Agent Files"),
		TEXT("MCP Tools")
	};
	Surface.AgentConfigLabels = {
		TEXT("AGENTS.md"),
		TEXT(".codex/config.toml"),
		TEXT(".mcp.json"),
		TEXT(".cursor/mcp.json"),
		TEXT(".vscode/mcp.json"),
		TEXT(".gemini/settings.json")
	};
	Surface.MCPServerActions = {
		TEXT("Unreal MCP"),
		TEXT("Extended Hyper Tools"),
		TEXT("Primary"),
		TEXT("Tools"),
		FString::Printf(TEXT("Epic + %d Hyper"), HyperAIStudio::CapabilityCatalog::GeneratedToolCount),
		TEXT("3 discovery dispatchers"),
		TEXT("Optional UE 5.8 source catalog"),
		TEXT("Search tool name, description, or toolset"),
		TEXT("Unreal MCP + Extended Hyper Tools"),
		TEXT("Unreal MCP Only"),
		TEXT("Fast"),
		TEXT("Strict Safety"),
		TEXT("Add"),
		TEXT("Import Profile"),
		TEXT("Copy URL"),
		TEXT("Edit"),
		TEXT("Test")
	};
	Surface.BannedSubstrings = {
		TEXT("Retired built-in profile"),
		TEXT("109 included"),
		TEXT("prepare prompt"),
		TEXT("Create Task Pack"),
		TEXT("Agent Console"),
		TEXT("What do you want to do in Unreal"),
		TEXT("debug endpoint"),
		TEXT("Open Test"),
		TEXT("Open Files"),
		TEXT("Copy Last Prompt"),
		TEXT("Copy Recipe"),
		TEXT("MCP Servers"),
		TEXT("Copy Codex Provider Patch"),
		TEXT("Add OpenAI Provider"),
		TEXT("Model Providers"),
		TEXT("Add Custom Agent"),
		TEXT("Custom Agent"),
		TEXT("Prompt only")
	};
	return Surface;
}

bool SHyperAIStudioWindow::IsRetiredExactOwnedMCPServerEntryForWorkbench(
	const FHyperAIStudioMCPServerEntry& Entry)
{
	FHyperAIStudioLegacyProfileInput Input;
	Input.ServerId = Entry.Id.IsEmpty() ? Entry.DisplayName : Entry.Id;
	Input.Transport = Entry.Transport == EHyperAIStudioMCPTransport::Command
		? TEXT("command")
		: TEXT("http");
	Input.Command = Entry.Command;
	Input.Arguments = Entry.Arguments;
	Input.Url = Entry.Transport == EHyperAIStudioMCPTransport::Command
		? FString()
		: Entry.Url;
	return FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(
		FHyperAIStudioLegacyProfileRecognizer::Recognize(Input));
}

FString SHyperAIStudioWindow::BuildPrerequisiteInstallTerminalCommandForSmoke(const FString& PrerequisiteId, const FString& DisplayName, const FString& InstallCommand, const FString& ProjectRoot)
{
	return HyperAIStudio::Private::BuildPrerequisiteTerminalCommand(PrerequisiteId, DisplayName, InstallCommand, ProjectRoot);
}

void SHyperAIStudioWindow::Construct(const FArguments& InArgs)
{
	Service = MakeShared<FHyperAIStudioService>();
	Status = Service->GetStatusSync();
	RefreshPrerequisitesCache();
	ActiveTabIndex = GetInitialTabIndexForStatus(Status);
	LastMessage = Status.IsReady()
		? LOCTEXT("InitialReadyMessage", "Ready. Use Chat / Terminal for daily handoff, or adjust agents and MCP profiles here.")
		: LOCTEXT("InitialMessage", "Setup is incomplete. Run Start Setup, verify blockers, or configure at least one agent.");
	AddSessionLogEntry(Status.IsReady()
		? TEXT("HyperAIStudio opened. Unreal MCP is ready.")
		: TEXT("HyperAIStudio opened. Setup is not ready yet."));

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 4.0f, 8.0f, 0.0f)
		[
			BuildTabBar()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(18.0f, 8.0f, 18.0f, 0.0f)
		[
			BuildHeader()
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(MainScrollBox, SScrollBox)
			+ SScrollBox::Slot()
			.Padding(18.0f, 12.0f, 18.0f, 18.0f)
			[
				SAssignNew(MainTabSwitcher, SWidgetSwitcher)
				.WidgetIndex_Lambda([this]() { return ActiveTabIndex; })
				+ SWidgetSwitcher::Slot()
				[
					BuildSetupTab()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildAgentsTab()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildMCPServersTab()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildHelpTab()
				]
			]
		]
	];

	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioWindow::RunDeferredRefresh));
}

void SHyperAIStudioWindow::RefreshPrerequisitesCache()
{
	CachedPrerequisites = Service.IsValid()
		? Service->GetPrerequisites()
		: TArray<FHyperAIStudioPrerequisiteStatus>();
}

const FHyperAIStudioPrerequisiteStatus* SHyperAIStudioWindow::FindCachedPrerequisite(const FString& PrerequisiteId) const
{
	return CachedPrerequisites.FindByPredicate([&PrerequisiteId](const FHyperAIStudioPrerequisiteStatus& Entry)
	{
		return Entry.Id == PrerequisiteId;
	});
}

bool SHyperAIStudioWindow::HasCachedMissingInstallablePrerequisites() const
{
	return CachedPrerequisites.ContainsByPredicate([](const FHyperAIStudioPrerequisiteStatus& Entry)
	{
		return HyperAIStudio::Private::IsMissingInstallablePrerequisite(Entry);
	});
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTabBar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(6.0f, 5.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				BuildTabButton(LOCTEXT("SetupTab", "Setup"), TEXT("Icons.Settings"), 0)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				BuildTabButton(LOCTEXT("AgentsTab", "Agents"), TEXT("Icons.Comment"), 1)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				BuildTabButton(LOCTEXT("MCPTab", "MCP"), TEXT("Icons.Package"), 2)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				BuildTabButton(LOCTEXT("HelpTab", "Docs"), TEXT("Icons.Help"), 3)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SBox)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return Status.IsReady() ? LOCTEXT("TopbarReady", "Ready") : LOCTEXT("TopbarNeedsSetup", "Needs setup"); })
				.ColorAndOpacity_Lambda([this]() { return Status.IsReady() ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Visibility_Lambda([this]() { return Status.IsReady() ? EVisibility::Visible : EVisibility::Collapsed; })
				.ToolTipText(LOCTEXT("OpenChatTerminalTooltip", "Open the separate Chat / Terminal handoff panel."))
				.OnClicked(this, &SHyperAIStudioWindow::OnOpenChatClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.Comment")))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("OpenChatTerminalButton", "Open Chat / Terminal"))
					]
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ToolTipText(LOCTEXT("RefreshIconTooltip", "Refresh readiness checks."))
				.OnClicked(this, &SHyperAIStudioWindow::OnRefreshClicked)
				[
					SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.Refresh")))
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTabButton(const FText& Label, const FName IconName, int32 TabIndex)
{
	return SNew(SButton)
		.ButtonColorAndOpacity_Lambda([this, TabIndex]()
		{
			return ActiveTabIndex == TabIndex
				? FSlateColor(FLinearColor(0.12f, 0.32f, 0.46f))
				: FSlateColor(FLinearColor::White);
		})
		.OnClicked_Lambda([this, TabIndex]()
		{
			bUserSelectedTab = true;
			bAutoRouteAfterFirstRefresh = false;
			ActiveTabIndex = TabIndex;
			ResetContentScroll();
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SImage).Image(FAppStyle::GetBrush(IconName))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(Label)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildIconTextButton(const FText& Label, const FName IconName, const FText& ToolTip, FOnClicked OnClicked) const
{
	return SNew(SButton)
		.ToolTipText(ToolTip)
		.OnClicked(OnClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage).Image(FAppStyle::GetBrush(IconName))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(Label)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildSetupTab()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildSetupSummaryPanel()
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildSetupSummaryPanel()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SetupSectionTitle", "Setup"))
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						if (Status.IsReady())
						{
							return LOCTEXT("SetupReadySummary", "Verified connection layer. Daily chat stays in your external agent; HyperAIStudio only prepares context, prompts, files, and MCP routing.");
						}
						if (Status.Warnings.Num() > 0)
						{
							return FText::FromString(Status.Warnings[0]);
						}
						return LOCTEXT("SetupNeededSummary", "Run setup once, verify Unreal MCP, and configure at least one external agent.");
					})
					.AutoWrapText(true)
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return Status.IsReady() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					BuildIconTextButton(
						LOCTEXT("SetupPrimaryButton", "Start Setup"),
						TEXT("Icons.Play"),
						LOCTEXT("SetupPrimaryTooltip", "Configure Unreal MCP, generate selected agent files, and verify readiness."),
						FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnSetupClicked))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]()
				{
					if (Status.IsReady())
					{
						return EVisibility::Collapsed;
					}
					return HasCachedMissingInstallablePrerequisites() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					BuildIconTextButton(
						LOCTEXT("SetupInstallPrerequisitesButton", "Install Prerequisites"),
						TEXT("Icons.Plus"),
						LOCTEXT("SetupInstallPrerequisitesTooltip", "Open HyperAI Chat, stop any active embedded agent terminal, and run the visible installer commands for missing external tools."),
						FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnInstallMissingPrerequisitesClicked))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return Status.IsReady() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					BuildIconTextButton(
						LOCTEXT("SetupOpenChatButton", "Open Chat / Terminal"),
						TEXT("Icons.Comment"),
						LOCTEXT("SetupOpenChatTooltip", "Open the separate daily handoff panel."),
						FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenChatClicked))
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.Visibility_Lambda([this]()
			{
				return !LastMessage.IsEmpty()
					&& !LastMessage.ToString().Contains(TEXT("failed"), ESearchCase::IgnoreCase)
					? EVisibility::Visible
					: EVisibility::Collapsed;
			})
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(8.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return LastMessage; })
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([this]()
				{
					return LastMessage.ToString().Contains(TEXT("restart"), ESearchCase::IgnoreCase)
						? HyperAIStudio::Private::WarningColor()
						: HyperAIStudio::Private::MutedColor();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.Visibility_Lambda([this]()
			{
				return LastMessage.ToString().Contains(TEXT("failed"), ESearchCase::IgnoreCase) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SetupFailedTitle", "Setup failed"))
					.ColorAndOpacity(HyperAIStudio::Private::ErrorColor())
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return LastMessage; })
					.AutoWrapText(true)
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton).Text(LOCTEXT("SetupFailedTryAgain", "Try Again")).OnClicked(this, &SHyperAIStudioWindow::OnSetupClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton).Text(LOCTEXT("SetupFailedOpenDocs", "Open Docs")).OnClicked(this, &SHyperAIStudioWindow::OnOpenIncludedDocsClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton).Text(LOCTEXT("SetupFailedPrereqs", "Fix Help")).OnClicked(this, &SHyperAIStudioWindow::OnViewPrerequisitesClicked)
					]
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				BuildIconTextButton(LOCTEXT("VerifySetupButton", "Verify"), TEXT("Icons.Refresh"), LOCTEXT("VerifySetupTooltip", "Rerun readiness checks."), FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnRefreshClicked))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				BuildIconTextButton(LOCTEXT("ConfigureAgentsButton", "Configure Agents"), TEXT("Icons.Comment"), LOCTEXT("ConfigureAgentsTooltip", "Open the Agents tab."), FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnConfigureAgentsClicked))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				BuildIconTextButton(LOCTEXT("ViewPrerequisitesButton", "View Prerequisites"), TEXT("Icons.Search"), LOCTEXT("ViewPrerequisitesTooltip", "Open the detailed prerequisite checklist."), FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnViewPrerequisitesClicked))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				BuildIconTextButton(LOCTEXT("OpenAgentFilesButton", "Agent Files"), TEXT("Icons.FolderOpen"), LOCTEXT("OpenAgentFilesTooltip", "Open or reveal generated project agent files."), FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenAgentFilesDialogClicked))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildSetupSummaryRow(
					LOCTEXT("SetupRequiredPlugins", "Epic MCP toolsets"),
					[this]()
					{
						return FText::FromString(Status.bDesiredCapabilityPluginsReady
							? FString::Printf(TEXT("%d/%d active"), Status.EnabledCapabilityPluginCount, Status.DesiredCapabilityPluginCount)
							: FString::Printf(TEXT("%d/%d - sync/restart needed"), Status.EnabledCapabilityPluginCount, Status.DesiredCapabilityPluginCount));
					},
					[this]() { return BoolColor(Status.bDesiredCapabilityPluginsReady); },
					LOCTEXT("SetupSyncToolsets", "Sync Toolsets"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnSetupClicked))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildSetupSummaryRow(
					LOCTEXT("SetupUnrealMCPServer", "Unreal MCP server"),
					[this]() { return Status.bServerRunning && Status.bPortListening ? LOCTEXT("SetupMCPReady", "Ready") : LOCTEXT("SetupMCPNotReady", "Not ready"); },
					[this]() { return Status.bServerRunning && Status.bPortListening ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); },
					LOCTEXT("SetupVerifyRow", "Verify"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnRefreshClicked))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildSetupSummaryRow(
					LOCTEXT("SetupToolsListProbe", "tools/list probe"),
					[this]() { return HyperAIStudio::Private::GetProbeStatusText(Status); },
					[this]() { return HyperAIStudio::Private::GetProbeStatusColor(Status); },
					LOCTEXT("SetupVerifyToolsRow", "Verify"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnRefreshClicked))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(9.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return HyperAIStudio::Private::GetProbeStatusDetailText(Status); })
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildSetupSummaryRow(
					LOCTEXT("SetupAgentFilesRow", "Agent instructions and configs"),
					[this]() { return Status.bAgentFilesReady ? LOCTEXT("SetupAgentFilesGenerated", "Generated") : LOCTEXT("SetupAgentFilesMissing", "Missing"); },
					[this]() { return Status.bAgentFilesReady ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); },
					LOCTEXT("SetupAgentFilesOpen", "Open"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenAgentFilesDialogClicked))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildSetupSummaryRow(
					LOCTEXT("SetupAgentsUsableRow", "Agents usable"),
					[this]() { return FText::FromString(FString::Printf(TEXT("%d/%d"), Status.ConfiguredAgentCount, Status.SupportedAgentCount)); },
					[this]() { return Status.ConfiguredAgentCount > 0 ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); },
					LOCTEXT("SetupConfigureAgentsRow", "Configure Agents"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnConfigureAgentsClicked))
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildSetupSummaryRow(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter, const FText& ActionLabel, FOnClicked Action) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 7.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.58f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.22f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([ValueGetter]() { return ValueGetter(); })
				.ColorAndOpacity_Lambda([ColorGetter]() { return ColorGetter(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(ActionLabel)
				.OnClicked(Action)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildFirstRunGuidePanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FirstRunGuideTitle", "First Run Flow"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FirstRunGuideHint", "HyperAIStudio makes Epic Unreal MCP usable from your external agent. Set up once, test the connection, then continue in Codex, Claude Code, Gemini, Cursor, or VS Code/Copilot with generated project instructions."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.34f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					BuildHeaderMetric(
						LOCTEXT("FirstRunStepSetup", "1. Setup"),
						[this]() { return Status.bUnrealMCPSettingsConfigured && Status.bAgentFilesReady ? LOCTEXT("FirstRunStepSetupReady", "Configured") : LOCTEXT("FirstRunStepSetupNeeded", "Run setup"); },
						[this]() { return Status.bUnrealMCPSettingsConfigured && Status.bAgentFilesReady ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.33f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					BuildHeaderMetric(
						LOCTEXT("FirstRunStepVerify", "2. Verify MCP"),
						[this]() { return Status.bToolsListReachable ? LOCTEXT("FirstRunStepVerifyReady", "tools/list works") : LOCTEXT("FirstRunStepVerifyNeeded", "Refresh/test"); },
						[this]() { return Status.bToolsListReachable ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.33f)
				[
					BuildHeaderMetric(
						LOCTEXT("FirstRunStepAgent", "3. Continue In Agent"),
						[this]() { return Status.IsReady() ? LOCTEXT("FirstRunStepAgentReady", "Ready") : LOCTEXT("FirstRunStepAgentNeeded", "Run a connection test"); },
						[this]() { return Status.IsReady() ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::MutedColor(); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FirstRunOpenDocs", "Open Docs"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenIncludedDocsClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FirstRunCopyPrompt", "Copy Starter Instructions"))
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyFirstRunPromptClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FirstRunOpenEpicDocs", "Open Epic MCP Docs"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenOfficialMCPDocsClicked)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FirstRunOpenProjectFolder", "Open Project Folder"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenProjectFolderClicked)
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentsTab()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AgentsSectionTitle", "Agents"))
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("%d/%d usable. Ready requires at least one enabled agent with config and CLI/launcher found."), Status.ConfiguredAgentCount, Status.SupportedAgentCount)); })
					.AutoWrapText(true)
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				BuildIconTextButton(
					LOCTEXT("RegenerateAgentFiles", "Regenerate Files"),
					TEXT("Icons.Refresh"),
					LOCTEXT("RegenerateAgentFilesTooltip", "Regenerate selected agent instructions and configs while preserving managed/user content."),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnRegenerateAgentFilesClicked))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildAgentTableHeader()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildAgentRow(TEXT("codex"), TEXT("Codex"), TEXT(".codex/config.toml"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildAgentRow(TEXT("claude"), TEXT("Claude Code"), TEXT(".mcp.json"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildAgentRow(TEXT("gemini"), TEXT("Gemini"), TEXT(".gemini/settings.json"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildAgentRow(TEXT("cursor"), TEXT("Cursor"), TEXT(".cursor/mcp.json"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildAgentRow(TEXT("vscode"), TEXT("VS Code/Copilot"), TEXT(".vscode/mcp.json"))
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentTableHeader() const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.20f)[SNew(STextBlock).Text(LOCTEXT("AgentHeaderAgent", "Agent")).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
			+ SHorizontalBox::Slot().FillWidth(0.13f)[SNew(STextBlock).Text(LOCTEXT("AgentHeaderCli", "CLI")).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
			+ SHorizontalBox::Slot().FillWidth(0.15f)[SNew(STextBlock).Text(LOCTEXT("AgentHeaderConfig", "Config")).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
			+ SHorizontalBox::Slot().FillWidth(0.12f)[SNew(STextBlock).Text(LOCTEXT("AgentHeaderEnabled", "Enabled")).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
			+ SHorizontalBox::Slot().FillWidth(0.40f)[SNew(STextBlock).Text(LOCTEXT("AgentHeaderActions", "Actions")).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentRow(const FString& PrerequisiteId, const FString& AgentName, const FString& ConfigRelativePath)
{
	const FHyperAIStudioPrerequisiteStatus* Prerequisite = FindCachedPrerequisite(PrerequisiteId);

	const FString InstallCommand = Prerequisite ? Prerequisite->InstallCommand : FString();
	const FString DisplayConfigPath = ConfigRelativePath;
	const FString PromptLabel = AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
		? TEXT("Copy VS Code Prompt")
		: FString::Printf(TEXT("Copy %s Prompt"), *AgentName.Replace(TEXT(" Code"), TEXT("")));
	const FString InstructionFile = HyperAIStudio::Private::GetInstructionFileForAgentName(AgentName);
	auto IsAgentAvailable = [this, PrerequisiteId]()
	{
		const FHyperAIStudioPrerequisiteStatus* CurrentPrerequisite = FindCachedPrerequisite(PrerequisiteId);
		return CurrentPrerequisite && CurrentPrerequisite->bAvailable;
	};
	auto IsAgentConfigured = [ConfigRelativePath]()
	{
		return FPaths::FileExists(FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), ConfigRelativePath));
	};
	auto IsAgentEnabled = [AgentName]()
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		return !Settings || Settings->IsBuiltInAgentEnabled(AgentName);
	};
	auto IsAgentUsable = [IsAgentEnabled, IsAgentAvailable, IsAgentConfigured]()
	{
		return IsAgentEnabled() && IsAgentAvailable() && IsAgentConfigured();
	};
	auto IsAgentPromptAvailable = [IsAgentEnabled, IsAgentAvailable, IsAgentConfigured]()
	{
		return IsAgentEnabled() && (IsAgentAvailable() || IsAgentConfigured());
	};

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 7.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.20f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(AgentName))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(DisplayConfigPath)).ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.13f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([IsAgentAvailable]()
				{
					return FText::FromString(IsAgentAvailable() ? TEXT("Found") : TEXT("Missing"));
				})
				.ColorAndOpacity_Lambda([IsAgentAvailable]()
				{
					return IsAgentAvailable() ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor();
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.15f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([IsAgentConfigured]()
				{
					return FText::FromString(IsAgentConfigured() ? TEXT("Configured") : TEXT("Missing"));
				})
				.ColorAndOpacity_Lambda([IsAgentConfigured]()
				{
					return IsAgentConfigured() ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor();
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.12f)
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([IsAgentEnabled]()
				{
					return HyperAIStudio::Private::CheckedState(IsAgentEnabled());
				})
				.OnCheckStateChanged_Lambda([this, AgentName](ECheckBoxState NewState)
				{
					if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
					{
						Settings->SetBuiltInAgentEnabled(AgentName, NewState == ECheckBoxState::Checked);
						Settings->SaveConfig();
					}
					RefreshStatus();
				})
				.ToolTipText(LOCTEXT("AgentEnabledTooltip", "Show this installed agent in HyperAIStudio chat and context-menu actions."))
				[
					SNew(STextBlock)
					.Text_Lambda([IsAgentEnabled]()
					{
						return IsAgentEnabled() ? LOCTEXT("AgentEnabledText", "On") : LOCTEXT("AgentDisabledText", "Off");
					})
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.40f)
			.VAlign(VAlign_Center)
			[
				SNew(SWrapBox)
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentUseInChat", "Use in Chat"))
					.ToolTipText_Lambda([IsAgentUsable]()
					{
						return IsAgentUsable()
							? LOCTEXT("AgentUseInChatTooltip", "Open Chat / Terminal with this installed and configured agent selected.")
							: LOCTEXT("AgentUseInChatDisabledTooltip", "Enable this agent, install the agent app, and generate its config before using it in Chat / Terminal.");
					})
					.IsEnabled_Lambda(IsAgentUsable)
					.OnClicked(this, &SHyperAIStudioWindow::OnUseAgentInChatClicked, AgentName)
				]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(PromptLabel))
					.ToolTipText(FText::FromString(FString::Printf(
						TEXT("Copy a starter prompt for this agent. Opening the project root lets it load the generated config and %s instructions."),
						*InstructionFile)))
					.IsEnabled_Lambda(IsAgentPromptAvailable)
					.OnClicked(this, &SHyperAIStudioWindow::OnPromptInAgentClicked, AgentName)
				]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentGenerateConfig", "Generate Config"))
					.ToolTipText(LOCTEXT("AgentGenerateConfigTooltip", "Enable this agent for config generation and write its project config file. Existing user content is merged safely; incompatible files stop generation without modification."))
					.IsEnabled_Lambda([IsAgentConfigured]()
					{
						return !IsAgentConfigured();
					})
					.OnClicked(this, &SHyperAIStudioWindow::OnGenerateAgentConfigClicked, AgentName)
				]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentFiles", "Files"))
					.ToolTipText(LOCTEXT("AgentFilesTooltip", "Open the files relevant to this agent."))
					.IsEnabled_Lambda(IsAgentConfigured)
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenAgentFilesForAgentClicked, AgentName)
				]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentInstall", "Install"))
					.ToolTipText(LOCTEXT("AgentInstallTooltip", "Start the visible install flow for this missing agent app."))
					.IsEnabled_Lambda([IsAgentAvailable, InstallCommand]()
					{
						return !IsAgentAvailable() && !InstallCommand.IsEmpty();
					})
					.OnClicked(this, &SHyperAIStudioWindow::OnInstallPrerequisiteClicked, PrerequisiteId, AgentName, InstallCommand)
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTerminalTab()
{
	return BuildTerminalPanel();
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildStatusTab()
{
	return BuildStatusPanel();
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTestsTab()
{
	return BuildTestCommandsPanel();
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildNativeToolSettingsBar()
{
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	int32 ExtendedToolCount = 0;
	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		ExtendedToolCount += Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			|| Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted ? 1 : 0;
	}
	const FText ChannelTooltip = FText::FromString(FString::Printf(
		TEXT("Choose Epic's Unreal MCP by itself or add %d Extended Hyper Tools on the same local endpoint. Changing the tool set requires an editor restart."),
		ExtendedToolCount));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 7.0f))
		[
			SNew(SWrapBox)
			.UseAllottedSize(true)
			+ SWrapBox::Slot().Padding(0.0f, 0.0f, 14.0f, 5.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("HyperAIToolModesLabel", "Unreal MCP tool set"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SWrapBox::Slot().Padding(0.0f, 0.0f, 14.0f, 5.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NativeToolChannelLabel", "Tool set"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboButton)
					.ToolTipText(ChannelTooltip)
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
							return Settings && Settings->NativeToolChannel == EHyperAIStudioNativeToolChannel::StableOnly
								? LOCTEXT("NativeToolChannelStable", "Unreal MCP Only")
								: LOCTEXT("NativeToolChannelPreview", "Unreal MCP + Extended Hyper Tools");
						})
					]
					.MenuContent()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SButton)
							.Text(LOCTEXT("SelectNativeToolPreview", "Unreal MCP + Extended Hyper Tools"))
							.ToolTipText(FText::FromString(FString::Printf(
								TEXT("Use Epic Unreal MCP plus %d Extended Hyper Tools on the same endpoint. Recommended."),
								ExtendedToolCount)))
							.OnClicked(this, &SHyperAIStudioWindow::OnSetNativeToolChannelClicked, EHyperAIStudioNativeToolChannel::Preview)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SButton)
							.Text(LOCTEXT("SelectNativeToolStable", "Unreal MCP Only"))
							.ToolTipText(LOCTEXT("SelectNativeToolStableTooltip", "Use Epic Unreal MCP without HyperAIStudio's extended tools."))
							.OnClicked(this, &SHyperAIStudioWindow::OnSetNativeToolChannelClicked, EHyperAIStudioNativeToolChannel::StableOnly)
						]
					]
				]
			]
			+ SWrapBox::Slot().Padding(0.0f, 0.0f, 14.0f, 5.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NativeExecutionModeLabel", "Execution"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboButton)
					.ToolTipText(LOCTEXT("NativeExecutionModeTooltip", "Fast minimizes repeated work for reads and reversible edits. Destructive and external effects remain strict in both modes."))
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
							return Settings && Settings->NativeExecutionMode == EHyperAIStudioNativeExecutionMode::StrictSafety
								? LOCTEXT("NativeExecutionStrict", "Strict Safety")
								: LOCTEXT("NativeExecutionFast", "Fast");
						})
					]
					.MenuContent()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SButton)
							.Text(LOCTEXT("SelectNativeExecutionFast", "Fast"))
							.OnClicked(this, &SHyperAIStudioWindow::OnSetNativeExecutionModeClicked, EHyperAIStudioNativeExecutionMode::Fast)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SButton)
							.Text(LOCTEXT("SelectNativeExecutionStrict", "Strict Safety"))
							.OnClicked(this, &SHyperAIStudioWindow::OnSetNativeExecutionModeClicked, EHyperAIStudioNativeExecutionMode::StrictSafety)
						]
					]
				]
			]
			+ SWrapBox::Slot().Padding(0.0f, 2.0f, 0.0f, 5.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NativeToolModesHint", "Extended tools are listed under Tools. Destructive and external-effect tools always retain strict safety."))
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildMCPServersTab()
{
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	const int32 CatalogToolCount = Catalog.Tools.Num();
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (Settings)
	{
		for (int32 Index = 0; Index < Settings->ExtraServers.Num(); ++Index)
		{
			const FHyperAIStudioMCPServerEntry& Entry = Settings->ExtraServers[Index];
			if (IsRetiredExactOwnedMCPServerEntryForWorkbench(Entry))
			{
				continue;
			}
			const FString Identity = FString::Printf(TEXT("%s %s"), *Entry.Id, *Entry.DisplayName).ToLower();
			const bool bLooksLikeSmokeOrTest = Identity.Contains(TEXT("smoke"))
				|| Identity.Contains(TEXT("test docs"))
				|| Identity.Contains(TEXT("test-docs"))
				|| Identity.Contains(TEXT("custom server 4"))
				|| Identity.Contains(TEXT("meshy"))
				|| Identity.Contains(TEXT("tripo"))
				|| Identity.Contains(TEXT("elevenlabs"))
				|| Identity.Contains(TEXT("provider mcp"));
			const bool bHasRunnableTarget = Entry.Transport == EHyperAIStudioMCPTransport::Command
				? !Entry.Command.TrimStartAndEnd().IsEmpty()
				: !Entry.Url.TrimStartAndEnd().IsEmpty();
			if (bLooksLikeSmokeOrTest || !bHasRunnableTarget)
			{
				continue;
			}

			const FString Type = Entry.Transport == EHyperAIStudioMCPTransport::Command ? TEXT("STDIO") : TEXT("HTTP");
			const FString StatusText = Entry.bEnabled ? TEXT("Enabled") : TEXT("Disabled");
			const FText StatusTextValue = FText::FromString(StatusText);
			Rows->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildMCPStaticRow(
					FText::FromString(Entry.DisplayName),
					FText::FromString(Type),
					[StatusTextValue]() { return StatusTextValue; },
					[Entry]() { return Entry.bEnabled ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::MutedColor(); },
					FText::AsNumber(Entry.Priority),
					[]() { return LOCTEXT("MCPUnknownTools", "not probed"); },
					LOCTEXT("MCPTestButton", "Test"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnTestMCPServerClicked, Index),
					LOCTEXT("MCPEditButton", "Edit"),
					FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnEditMCPServerClicked, Index))
			];
		}
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MCPSectionTitle", "MCP"))
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MCPSectionHint", "Unreal MCP stays the locked primary server. Extended Hyper Tools, when enabled, use that same endpoint. Other MCP entries are exported to agents by advisory priority; HyperAIStudio does not route tool calls."))
					.AutoWrapText(true)
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				BuildIconTextButton(LOCTEXT("MCPImportProfile", "Import Profile"), TEXT("Icons.Import"), LOCTEXT("MCPImportProfileTooltip", "Import a hyperai.mcp.v1 profile from the clipboard."), FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnImportMCPProfileClicked))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				BuildIconTextButton(
					LOCTEXT("MCPAdd", "Add"),
					TEXT("Icons.Plus"),
					LOCTEXT("MCPAddTooltip", "Add a custom MCP profile entry."),
					FOnClicked::CreateLambda([this]() { return OnEditMCPServerClicked(INDEX_NONE); }))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			BuildNativeToolSettingsBar()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildMCPStaticRow(
				LOCTEXT("UnrealMCPName", "Unreal MCP"),
				LOCTEXT("UnrealMCPType", "Streamable HTTP"),
				[this]() { return HyperAIStudio::Private::GetProbeStatusText(Status); },
				[this]() { return HyperAIStudio::Private::GetProbeStatusColor(Status); },
					LOCTEXT("UnrealMCPPrimary", "Primary"),
					[this, CatalogToolCount]()
					{
						const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
						const bool bExtended = !Settings
							|| Settings->NativeToolChannel != EHyperAIStudioNativeToolChannel::StableOnly;
						if (CachedLiveEpicToolCount >= 0 && CachedLiveHyperToolCount >= 0)
						{
							return bExtended
								? FText::FromString(FString::Printf(
									TEXT("%d Epic + %d Hyper"),
									CachedLiveEpicToolCount,
									CachedLiveHyperToolCount))
								: FText::FromString(FString::Printf(TEXT("%d Epic tools"), CachedLiveEpicToolCount));
						}
						return bExtended
							? FText::FromString(FString::Printf(TEXT("Epic + %d Hyper"), CatalogToolCount))
							: LOCTEXT("UnrealMCPOnlyToolCount", "Epic tools");
					},
				LOCTEXT("UnrealMCPToolsButton", "Tools"),
				FOnClicked::CreateLambda([this]()
				{
					return OnOpenMCPToolsClicked(TEXT("Unreal MCP"), FString());
				}),
				LOCTEXT("UnrealMCPCopyUrl", "Copy URL"),
				FOnClicked::CreateLambda([this]()
				{
					Service->CopyTextToClipboard(Status.Endpoint);
					LastMessage = LOCTEXT("UnrealMCPEndpointCopied", "Unreal MCP endpoint copied.");
					return FReply::Handled();
				}))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(9.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return HyperAIStudio::Private::GetProbeStatusDetailText(Status); })
			.AutoWrapText(true)
			.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(9.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
				.Text_Lambda([this, CatalogToolCount]()
				{
					const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
					const TCHAR* ToolSet = Settings && Settings->NativeToolChannel == EHyperAIStudioNativeToolChannel::StableOnly
						? TEXT("Unreal MCP Only") : TEXT("Unreal MCP + Extended Hyper Tools");
					const TCHAR* Execution = Settings && Settings->NativeExecutionMode == EHyperAIStudioNativeExecutionMode::StrictSafety
						? TEXT("Strict Safety") : TEXT("Fast");
					const FString ToolsetSummary = Status.DiscoverableToolsetCount > 0
						? FString::Printf(TEXT("%d toolsets available now"), Status.DiscoverableToolsetCount)
						: TEXT("live toolsets load on Verify");
					return FText::FromString(FString::Printf(
						TEXT("%s | %s | Execution: %s. Open Tools for the exact live Epic + Hyper action count; the 3 discovery tools are only the gateway."),
						ToolSet, *ToolsetSummary, Execution));
				})
			.AutoWrapText(true)
			.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Rows
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildMCPStaticRow(const FText& Name, const FText& Type, TFunction<FText()> StatusGetter, TFunction<FSlateColor()> ColorGetter, const FText& Priority, TFunction<FText()> ToolsGetter, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const
{
	TSharedRef<SWrapBox> Actions = SNew(SWrapBox);
	if (!PrimaryLabel.IsEmpty())
	{
		Actions->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
		[
			SNew(SButton).Text(PrimaryLabel).OnClicked(PrimaryAction)
		];
	}
	if (!SecondaryLabel.IsEmpty())
	{
		Actions->AddSlot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
		[
			SNew(SButton).Text(SecondaryLabel).OnClicked(SecondaryAction)
		];
	}
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 7.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.25f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Name)
			]
			+ SHorizontalBox::Slot().FillWidth(0.16f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Type).ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SHorizontalBox::Slot().FillWidth(0.14f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([StatusGetter]() { return StatusGetter(); })
				.ColorAndOpacity_Lambda([ColorGetter]() { return ColorGetter(); })
			]
			+ SHorizontalBox::Slot().FillWidth(0.10f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Priority).ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SHorizontalBox::Slot().FillWidth(0.10f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([ToolsGetter]() { return ToolsGetter(); })
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SHorizontalBox::Slot().FillWidth(0.25f).VAlign(VAlign_Center)
			[
				Actions
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildHelpTab()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("HelpTitle", "Docs"))
			.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("HelpHint", "Compact shortcuts for website docs, generated guidance, Unreal MCP reference, troubleshooting, and MCP profile templates."))
			.AutoWrapText(true)
			.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildDocsShortcutRow(
				LOCTEXT("DocsLocalTitle", "Website Docs"),
				LOCTEXT("DocsLocalBody", "HyperAIStudio guide on gamesbyhyper.com and current project folder."),
				LOCTEXT("DocsOpenDocs", "Open Docs"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenIncludedDocsClicked),
				LOCTEXT("DocsProjectFolder", "Project Folder"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenProjectFolderClicked))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildDocsShortcutRow(
				LOCTEXT("DocsAgentTitle", "Agent Instructions"),
				LOCTEXT("DocsAgentBody", "Canonical AGENTS.md plus a starter prompt to paste in your external agent."),
				LOCTEXT("DocsOpenAgents", "Open AGENTS.md"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenAgentsClicked),
				LOCTEXT("DocsCopyStarter", "Copy Starter"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnCopyFirstRunPromptClicked))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildDocsShortcutRow(
				LOCTEXT("DocsEpicTitle", "Epic Unreal MCP"),
				LOCTEXT("DocsEpicBody", "Official reference and MCP Inspector command for Streamable HTTP."),
				LOCTEXT("DocsOpenEpic", "Open Epic Docs"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnOpenOfficialMCPDocsClicked),
				LOCTEXT("DocsCopyInspector", "Copy Inspector"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnCopyInspectorCommandClicked))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildDocsShortcutRow(
				LOCTEXT("DocsTroubleshootingTitle", "Troubleshooting"),
				LOCTEXT("DocsTroubleshootingBody", "Port conflicts, toolset visibility, missing files, and config repair routes."),
				LOCTEXT("DocsViewPrereqs", "Open Guide"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnViewPrerequisitesClicked),
				LOCTEXT("DocsVerify", "Verify"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnRefreshClicked))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildDocsShortcutRow(
				LOCTEXT("DocsMCPProfilesTitle", "MCP Profiles"),
				LOCTEXT("DocsMCPProfilesBody", "Import reviewed hyperai.mcp.v1 profiles or add a local MCP server that is already runnable."),
				LOCTEXT("DocsCopyExampleProfile", "Copy Example"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnCopyExampleMCPProfileClicked),
				LOCTEXT("DocsImportProfile", "Import Profile"),
				FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnImportMCPProfileClicked))
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildDocsShortcutRow(const FText& Title, const FText& Body, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 7.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.28f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Title)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.46f)
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock).Text(Body).AutoWrapText(true).ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.26f)
			.VAlign(VAlign_Center)
			[
				SNew(SWrapBox)
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton).Text(PrimaryLabel).OnClicked(PrimaryAction)
				]
				+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					SNew(SButton).Text(SecondaryLabel).OnClicked(SecondaryAction)
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildHeader()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Title", "HyperAIStudio"))
						.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SHyperAIStudioWindow::GetHeadlineText)
						.ColorAndOpacity(this, &SHyperAIStudioWindow::GetHeadlineColor)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("DocsLink", "Documentation"))
					.OnNavigate_Lambda([]()
					{
						const FString Url = FHyperAIStudioService::GetDocumentationUrl();
						FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SHyperAIStudioWindow::GetMessageText)
				.AutoWrapText(true)
				.Font_Lambda([this]()
				{
					return LastMessage.ToString().Contains(TEXT("Project-root note:"))
						? FAppStyle::GetFontStyle(TEXT("BoldFont"))
						: FAppStyle::GetFontStyle(TEXT("NormalFont"));
				})
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.32f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					BuildHeaderMetric(
						LOCTEXT("HeaderMetricMCP", "Unreal MCP"),
						[this]() { return Status.IsReady() ? LOCTEXT("HeaderReady", "Ready") : (Status.bProbeInProgress ? LOCTEXT("HeaderChecking", "Checking") : LOCTEXT("HeaderSetupNeeded", "Setup needed")); },
						[this]() { return Status.IsReady() ? HyperAIStudio::Private::GoodColor() : (Status.bProbeInProgress ? HyperAIStudio::Private::WarningColor() : HyperAIStudio::Private::ErrorColor()); })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.36f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					BuildHeaderMetric(
						LOCTEXT("HeaderMetricEndpoint", "Endpoint"),
						[this]() { return FText::FromString(Status.Endpoint.IsEmpty() ? FString(TEXT("Not configured")) : Status.Endpoint); },
						[this]() { return Status.bPortListening ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::MutedColor(); })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.32f)
				[
					BuildHeaderMetric(
						LOCTEXT("HeaderMetricAgentFiles", "Agent Files"),
						[this]() { return Status.bAgentFilesReady ? LOCTEXT("HeaderAgentFilesReady", "Generated") : LOCTEXT("HeaderAgentFilesMissing", "Missing"); },
						[this]() { return Status.bAgentFilesReady ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor(); })
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildHeaderMetric(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([ValueGetter]() { return ValueGetter(); })
				.ColorAndOpacity_Lambda([ColorGetter]() { return ColorGetter(); })
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildActionBar()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SetupButton", "Set Up HyperAIStudio"))
			.OnClicked(this, &SHyperAIStudioWindow::OnSetupClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("EnablePluginsButton", "Enable Plugins"))
			.OnClicked(this, &SHyperAIStudioWindow::OnEnablePluginsClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("RefreshButton", "Refresh"))
			.OnClicked(this, &SHyperAIStudioWindow::OnRefreshClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("StartButton", "Start MCP"))
			.OnClicked(this, &SHyperAIStudioWindow::OnStartClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("StopButton", "Stop MCP"))
			.OnClicked(this, &SHyperAIStudioWindow::OnStopClicked)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenUnrealTerminalButton", "Open UE Terminal"))
			.OnClicked(this, &SHyperAIStudioWindow::OnOpenUnrealTerminalClicked)
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildPrerequisitesPanel()
{
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FHyperAIStudioPrerequisiteStatus& Prerequisite : CachedPrerequisites)
	{
		Rows->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			BuildPrerequisiteRow(Prerequisite)
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PrereqTitle", "Prerequisites"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PrereqHint", "HyperAIStudio checks the Unreal MCP stack, Terminal, and common agent apps. Missing external apps stay manual; required Unreal plugins can be enabled here."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				Rows
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildContextChipsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ContextChipsTitle", "Current Context"))
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					BuildContextChip(
						LOCTEXT("ContextActorsLabel", "Actors"),
						[this]()
						{
							return FText::AsNumber(Service->GetCurrentContextSnapshot().SelectedActorCount);
						},
						[this]()
						{
							return Service->GetCurrentContextSnapshot().SelectedActorCount > 0 ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::MutedColor();
						},
						LOCTEXT("ContextActorsTooltip", "Selected level actors that will be included in a context pack."))
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					BuildContextChip(
						LOCTEXT("ContextAssetsLabel", "Assets"),
						[this]()
						{
							return FText::AsNumber(Service->GetCurrentContextSnapshot().SelectedAssetCount);
						},
						[this]()
						{
							return Service->GetCurrentContextSnapshot().SelectedAssetCount > 0 ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::MutedColor();
						},
						LOCTEXT("ContextAssetsTooltip", "Selected Content Browser assets that will be included in a context pack."))
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					BuildContextChip(
						LOCTEXT("ContextBlueprintLabel", "Blueprint Nodes"),
						[this]()
						{
							const FHyperAIStudioContextSnapshot Snapshot = Service->GetCurrentContextSnapshot();
							return Snapshot.SelectedBlueprintNodeCount > 0
								? FText::AsNumber(Snapshot.SelectedBlueprintNodeCount)
								: LOCTEXT("ContextBlueprintMenuValue", "Graph menu");
						},
						[this]()
						{
							return Service->GetCurrentContextSnapshot().SelectedBlueprintNodeCount > 0 ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor();
						},
						LOCTEXT("ContextBlueprintTooltip", "Blueprint node context is captured from the Blueprint graph right-click menu."))
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					BuildContextChip(
						LOCTEXT("ContextMapLabel", "Map"),
						[this]()
						{
							return FText::FromString(Service->GetCurrentContextSnapshot().CurrentMap);
						},
						[]()
						{
							return HyperAIStudio::Private::MutedColor();
						},
						LOCTEXT("ContextMapTooltip", "Current editor map recorded in generated context packs."))
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 0.0f, 8.0f, 8.0f)
				[
					BuildContextChip(
						LOCTEXT("ContextProjectRootLabel", "Project Root"),
						[this]()
						{
							return FText::FromString(FPaths::GetBaseFilename(FPaths::GetProjectFilePath()));
						},
						[]()
						{
							return HyperAIStudio::Private::MutedColor();
						},
						LOCTEXT("ContextProjectRootTooltip", "Agent clients opened from this project root can load the generated project-scoped config and rules."))
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildContextChip(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter, const FText& ToolTip) const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		.ToolTipText(ToolTip)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([ValueGetter]() { return ValueGetter(); })
				.ColorAndOpacity_Lambda([ColorGetter]() { return ColorGetter(); })
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentStatusPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AgentStatusTitle", "Agent Targets"))
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				BuildAgentStatusRow(TEXT("codex"), TEXT("Codex"), LOCTEXT("OpenProjectInCodex", "Open Codex Test"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildAgentStatusRow(TEXT("claude"), TEXT("Claude Code"), LOCTEXT("OpenProjectInClaude", "Open Claude Code Test"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildAgentStatusRow(TEXT("gemini"), TEXT("Gemini"), LOCTEXT("OpenProjectInGemini", "Open Gemini Test"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildAgentStatusRow(TEXT("cursor"), TEXT("Cursor"), LOCTEXT("OpenProjectInCursor", "Open Cursor Test"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildAgentStatusRow(TEXT("vscode"), TEXT("VS Code/Copilot"), LOCTEXT("OpenProjectInVSCode", "Open VS Code Test"))
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentStatusRow(const FString& PrerequisiteId, const FString& AgentName, const FText& OpenLabel)
{
	const FHyperAIStudioPrerequisiteStatus* Prerequisite = FindCachedPrerequisite(PrerequisiteId);

	const bool bAvailable = Prerequisite && Prerequisite->bAvailable;
	const FString Detail = Prerequisite ? Prerequisite->ActionHint : TEXT("Prerequisite state is unavailable.");
	const FString InstallCommand = Prerequisite ? Prerequisite->InstallCommand : FString();
	const FString DocumentationUrl = Prerequisite ? Prerequisite->DocumentationUrl : FString();

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(0.22f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(AgentName))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(0.18f)
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(bAvailable ? TEXT("Installed") : TEXT("Missing")))
			.ColorAndOpacity(bAvailable ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::WarningColor())
		]
		+ SHorizontalBox::Slot()
		.FillWidth(0.34f)
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Detail))
			.AutoWrapText(true)
			.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(OpenLabel)
			.ToolTipText(LOCTEXT("OpenProjectInAgentTooltip", "Create a connection-test prompt and open the selected agent from the project root."))
			.IsEnabled(bAvailable)
			.OnClicked(this, &SHyperAIStudioWindow::OnOpenAgentTestClicked, AgentName)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyAgentInstall", "Copy Install"))
			.ToolTipText(LOCTEXT("CopyAgentInstallTooltip", "Copy the install command for this agent app."))
			.IsEnabled(!InstallCommand.IsEmpty())
			.OnClicked(this, &SHyperAIStudioWindow::OnCopyPrerequisiteCommandClicked, AgentName, InstallCommand)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenAgentDocs", "Docs"))
			.ToolTipText(LOCTEXT("OpenAgentDocsTooltip", "Open setup documentation for this agent app."))
			.IsEnabled(!DocumentationUrl.IsEmpty())
			.OnClicked(this, &SHyperAIStudioWindow::OnOpenPrerequisiteDocsClicked, AgentName, DocumentationUrl)
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildSessionLogPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SessionLogTitle", "Session Log"))
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SHyperAIStudioWindow::GetSessionLogText)
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTerminalPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TerminalPanelTitle", "Terminal"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(GetTerminalPanelHintText())
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("TerminalOpenUnreal", "Open UE Terminal"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenUnrealTerminalClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyTerminalSetup", "Copy Startup Commands"))
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyTerminalSetupClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyInspectorCommand", "Copy MCP Inspector"))
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyInspectorCommandClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("TerminalOpenFolder", "Open Project Folder"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenProjectFolderClicked)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(10.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TerminalRecipeTitle", "Project-root startup recipe"))
						.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return FText::FromString(GetTerminalStartupRecipeText(FHyperAIStudioService::GetProjectRoot()));
						})
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 9))
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(10.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TerminalInspectorTitle", "MCP Inspector"))
						.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return FText::FromString(FString::Printf(
								TEXT("npx @modelcontextprotocol/inspector\nEndpoint: %s\nTransport: Streamable HTTP"),
								*Status.Endpoint));
						})
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 9))
						.AutoWrapText(true)
					]
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTestCommandsPanel()
{
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FHyperAIStudioTestCommand& Command : Service->GetTestCommands())
	{
		Rows->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			BuildTestCommandRow(Command)
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TestCommandsTitle", "Test Commands"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TestCommandsHint", "Use these to validate the same flow from terminal, Codex, Claude Code, Gemini, Cursor, or VS Code/Copilot."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Rows
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildStatusPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ReadinessTitle", "Readiness"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("ModuleRow", "Epic Unreal MCP"), [this]() { return BoolText(Status.bUnrealMCPModuleAvailable, TEXT("Detected"), TEXT("Not loaded")); }, [this]() { return BoolColor(Status.bUnrealMCPModuleAvailable); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("SettingsRow", "Server Settings"), [this]() { return BoolText(Status.bUnrealMCPSettingsConfigured, TEXT("Configured"), TEXT("Needs setup")); }, [this]() { return BoolColor(Status.bUnrealMCPSettingsConfigured); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("ServerRow", "Server"), [this]() { return FText::FromString(Status.bServerRunning ? FString::Printf(TEXT("Running on port %u"), Status.ActivePort) : TEXT("Stopped")); }, [this]() { return BoolColor(Status.bServerRunning); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("PortRow", "Port"), [this]() { return BoolText(Status.bPortListening, TEXT("Listening"), TEXT("Closed")); }, [this]() { return BoolColor(Status.bPortListening); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("ToolsListRow", "tools/list"), [this]()
				{
					if (Status.bToolsListReachable && Status.ProbeToolCount > 0)
					{
						return FText::FromString(FString::Printf(TEXT("Reachable (%d tools)"), Status.ProbeToolCount));
					}
					return HyperAIStudio::Private::GetProbeStatusText(Status);
				}, [this]() { return HyperAIStudio::Private::GetProbeStatusColor(Status); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				BuildStatusRow(LOCTEXT("AgentFilesRow", "Agent Files"), [this]() { return BoolText(Status.bAgentFilesReady, TEXT("Present"), TEXT("Missing")); }, [this]() { return BoolColor(Status.bAgentFilesReady); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("Endpoint: %s"), *Status.Endpoint)); })
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return HyperAIStudio::Private::GetProbeStatusDetailText(Status); })
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildAgentConfigPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AgentConfigTitle", "Agent Configs"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AgentConfigHint", "Agent config files are written where each external agent expects them. HyperAIStudio only writes configs for agents that are enabled and checked here; disabled agents do not get new config files."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateCodexConfig); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateCodexConfig = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("CodexConfig", "Codex: .codex/config.toml"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(20.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CodexTomlSafetyHint", "Codex TOML updates stay inside a HyperAIStudio managed block. Copy the block first if you want to review or manually apply it to an existing config."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(20.0f, 7.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CopyCodexTomlPatchButton", "Copy Codex TOML Patch"))
				.OnClicked(this, &SHyperAIStudioWindow::OnCopyCodexTomlPatchClicked)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateClaudeConfig); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateClaudeConfig = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("ClaudeConfig", "Claude Code: .mcp.json"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateClaudeMarkdown); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateClaudeMarkdown = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("ClaudeMd", "Claude instructions: CLAUDE.md"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateCursorConfig); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateCursorConfig = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("CursorConfig", "Cursor: .cursor/mcp.json"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateVSCodeConfig); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateVSCodeConfig = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("VSCodeConfig", "VS Code: .vscode/mcp.json"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bGenerateGeminiConfig); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bGenerateGeminiConfig = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("GeminiConfig", "Gemini: .gemini/settings.json"))
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildFilesAndPermissionsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("FilesPermissionsTitle", "Files And Permissions"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenAgentsButton", "Open AGENTS.md"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenAgentsClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenClaudeButton", "Open CLAUDE.md"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenClaudeClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenProjectFolderButton", "Open Project Folder"))
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenProjectFolderClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bAllowClipboardPromptCopy); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bAllowClipboardPromptCopy = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("AllowClipboardCopy", "Copy generated instructions to clipboard"))
				]
			]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bShowQuickAutoCommentInBlueprintMenu); })
					.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bShowQuickAutoCommentInBlueprintMenu = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
					.ToolTipText(LOCTEXT("ShowQuickAutoCommentTooltip", "Show the fast local title-based comment action in Blueprint and PCG graph context menus. Smart Auto Comment stays AI-assisted."))
					[
						SNew(STextBlock).Text(LOCTEXT("ShowQuickAutoComment", "Show Quick Auto Comment in Blueprint menu"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bAllowTerminalLaunch); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bAllowTerminalLaunch = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("AllowTerminalLaunch", "Allow HyperAIStudio to open external agent windows"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bAllowExternalAgentLaunch); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bAllowExternalAgentLaunch = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("AllowAgentLaunch", "Allow future direct external agent launch"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([]() { return HyperAIStudio::Private::CheckedState(GetDefault<UHyperAIStudioSettings>()->bRequireConfirmationForDestructiveActions); })
				.OnCheckStateChanged_Lambda([](ECheckBoxState State) { UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>(); Settings->bRequireConfirmationForDestructiveActions = State == ECheckBoxState::Checked; Settings->SaveConfig(); })
				[
					SNew(STextBlock).Text(LOCTEXT("RequireDestructiveConfirm", "Require confirmation for destructive agent actions"))
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildMCPRegistryPanel()
{
	TSharedRef<SVerticalBox> ServerRows = SNew(SVerticalBox);
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (Settings)
	{
		for (int32 Index = 0; Index < Settings->ExtraServers.Num(); ++Index)
		{
			ServerRows->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildMCPServerRow(Index)
			];
		}
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MCPRegistryTitle", "MCP Servers"))
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("Locked primary: unreal-mcp -> %s"), *Status.Endpoint)); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
					const int32 EnabledCount = Settings ? Settings->ExtraServers.FilterByPredicate([](const FHyperAIStudioMCPServerEntry& Entry) { return Entry.bEnabled; }).Num() : 0;
					const int32 TotalCount = Settings ? Settings->ExtraServers.Num() : 0;
					return FText::FromString(FString::Printf(TEXT("Community MCP entries: %d enabled, %d total. Changes regenerate client configs and agent instructions."), EnabledCount, TotalCount));
				})
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				ServerRows
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AddServerButton", "Add MCP Server Entry"))
					.OnClicked(this, &SHyperAIStudioWindow::OnAddServerClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportExampleMCPProfileButton", "Import Example Profile"))
					.OnClicked(this, &SHyperAIStudioWindow::OnImportExampleMCPProfileClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportMCPProfileButton", "Import Profile From Clipboard"))
					.OnClicked(this, &SHyperAIStudioWindow::OnImportMCPProfileClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyExampleMCPProfileButton", "Copy Example Profile"))
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyExampleMCPProfileClicked)
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildMCPServerRow(int32 ServerIndex)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([ServerIndex]()
					{
						const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
						return HyperAIStudio::Private::CheckedState(Settings && Settings->ExtraServers.IsValidIndex(ServerIndex) && Settings->ExtraServers[ServerIndex].bEnabled);
					})
					.OnCheckStateChanged(this, &SHyperAIStudioWindow::OnMCPServerEnabledChanged, ServerIndex)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.42f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([ServerIndex]()
					{
						const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
						return Settings && Settings->ExtraServers.IsValidIndex(ServerIndex)
							? FText::FromString(Settings->ExtraServers[ServerIndex].DisplayName)
							: LOCTEXT("MissingMCPServerName", "Missing server");
					})
					.OnTextCommitted(this, &SHyperAIStudioWindow::OnMCPServerNameCommitted, ServerIndex)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MCPPriorityLabel", "Priority"))
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(64.0f)
					[
						SNew(SEditableTextBox)
						.Text_Lambda([ServerIndex]()
						{
							const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
							return Settings && Settings->ExtraServers.IsValidIndex(ServerIndex)
								? FText::AsNumber(Settings->ExtraServers[ServerIndex].Priority)
								: FText::GetEmpty();
						})
						.OnTextCommitted(this, &SHyperAIStudioWindow::OnMCPServerPriorityCommitted, ServerIndex)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("TestMCPServer", "Test"))
					.OnClicked(this, &SHyperAIStudioWindow::OnTestMCPServerClicked, ServerIndex)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RemoveMCPServer", "Remove"))
					.OnClicked(this, &SHyperAIStudioWindow::OnRemoveMCPServerClicked, ServerIndex)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("MCPDescriptionHint", "Description for generated agent instructions..."))
				.Text_Lambda([ServerIndex]()
				{
					const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
					return Settings && Settings->ExtraServers.IsValidIndex(ServerIndex)
						? FText::FromString(Settings->ExtraServers[ServerIndex].Description)
						: FText::GetEmpty();
				})
				.OnTextCommitted(this, &SHyperAIStudioWindow::OnMCPServerDescriptionCommitted, ServerIndex)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([ServerIndex]()
					{
						const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
						if (Settings && Settings->ExtraServers.IsValidIndex(ServerIndex) && Settings->ExtraServers[ServerIndex].Transport == EHyperAIStudioMCPTransport::Command)
						{
							return LOCTEXT("MCPCommandLabel", "Command");
						}
						return LOCTEXT("MCPUrlLabel", "URL");
					})
					.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([ServerIndex]()
					{
						const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
						if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
						{
							return FText::GetEmpty();
						}
						const FHyperAIStudioMCPServerEntry& Entry = Settings->ExtraServers[ServerIndex];
						return FText::FromString(Entry.Transport == EHyperAIStudioMCPTransport::Command ? Entry.Command : Entry.Url);
					})
					.OnTextCommitted(this, &SHyperAIStudioWindow::OnMCPServerEndpointCommitted, ServerIndex)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([ServerIndex]()
				{
					const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
					if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
					{
						return LOCTEXT("MCPServerMissingHint", "Server entry no longer exists.");
					}
					const FHyperAIStudioMCPServerEntry& Entry = Settings->ExtraServers[ServerIndex];
					const FString Transport = Entry.Transport == EHyperAIStudioMCPTransport::Command ? TEXT("command/stdio") : TEXT("streamable HTTP");
					const FString Id = Entry.Id.IsEmpty() ? TEXT("custom-mcp") : Entry.Id;
					FString Detail = FString::Printf(TEXT("id `%s`, %s. Enabled entries are exported next to unreal-mcp; HyperAIStudio does not route tool calls."), *Id, *Transport);
					if (Entry.Domains.Num() > 0)
					{
						Detail += FString::Printf(TEXT(" Domains: %s."), *FString::Join(Entry.Domains, TEXT(", ")));
					}
					if (Entry.TargetClients.Num() > 0)
					{
						Detail += FString::Printf(TEXT(" Targets: %s."), *FString::Join(Entry.TargetClients, TEXT(", ")));
					}
					if (Entry.Arguments.Num() > 0)
					{
						Detail += FString::Printf(TEXT(" Args: %s."), *FString::Join(Entry.Arguments, TEXT(" ")));
					}
					if (!Entry.SetupNotes.IsEmpty())
					{
						Detail += FString::Printf(TEXT(" %s"), *Entry.SetupNotes);
					}
					if (Entry.AgentInstructions.Num() > 0)
					{
						Detail += FString::Printf(TEXT(" Agent instructions: %s."), *FString::Join(Entry.AgentInstructions, TEXT(" ")));
					}
					return FText::FromString(Detail);
				})
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildPrerequisiteRow(const FHyperAIStudioPrerequisiteStatus& Prerequisite)
{
	const bool bOk = Prerequisite.bAvailable && Prerequisite.bEnabled;
	const FSlateColor StatusColor = bOk
		? HyperAIStudio::Private::GoodColor()
		: (Prerequisite.bRequired ? HyperAIStudio::Private::ErrorColor() : HyperAIStudio::Private::WarningColor());
	const FString DetailText = Prerequisite.ActionHint.IsEmpty()
		? Prerequisite.Detail
		: FString::Printf(TEXT("%s - %s"), *Prerequisite.Detail, *Prerequisite.ActionHint);

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Prerequisite.DisplayName))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(bOk ? TEXT("Ready") : (Prerequisite.bAvailable ? TEXT("Needs enable") : TEXT("Missing"))))
					.ColorAndOpacity(StatusColor)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyInstallCommand", "Copy Install"))
					.IsEnabled(!Prerequisite.InstallCommand.IsEmpty())
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyPrerequisiteCommandClicked, Prerequisite.DisplayName, Prerequisite.InstallCommand)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenPrereqDocs", "Docs"))
					.IsEnabled(!Prerequisite.DocumentationUrl.IsEmpty())
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenPrerequisiteDocsClicked, Prerequisite.DisplayName, Prerequisite.DocumentationUrl)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(DetailText))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildTestCommandRow(const FHyperAIStudioTestCommand& Command)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Command.Title))
					.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyTestPrompt", "Copy Instructions"))
					.OnClicked(this, &SHyperAIStudioWindow::OnCopyTestPromptClicked, Command.Id)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenTestTerminal", "Open Test Terminal"))
					.IsEnabled(!Command.TerminalCommand.IsEmpty())
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenTestTerminalClicked, Command.Id)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Command.Description))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioWindow::BuildStatusRow(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter) const
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(0.38f)
		[
			SNew(STextBlock)
			.Text(Label)
			.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
		]
		+ SHorizontalBox::Slot()
		.FillWidth(0.62f)
		[
			SNew(STextBlock)
			.Text_Lambda([ValueGetter]() { return ValueGetter(); })
			.ColorAndOpacity_Lambda([ColorGetter]() { return ColorGetter(); })
		];
}

void SHyperAIStudioWindow::ResetContentScroll()
{
	if (MainScrollBox.IsValid())
	{
		MainScrollBox->ScrollToStart();
	}
}

FReply SHyperAIStudioWindow::OnSetupClicked()
{
	FText Message;
	const EHyperAIStudioSetupOutcome Outcome = Service->SetUpHyperAIStudio(Message);
	const bool bOk = Outcome != EHyperAIStudioSetupOutcome::Failed;
	if (bOk)
	{
		FHyperAIStudioCapabilityInventoryClient::InvalidateDetailed(Status.Endpoint);
		CachedLiveEpicToolCount = INDEX_NONE;
		CachedLiveHyperToolCount = INDEX_NONE;
		if (Outcome == EHyperAIStudioSetupOutcome::RestartRequired)
		{
			PendingReadinessRetryCount = 0;
			LastMessage = Message;
		}
		else
		{
			bReportNextRefreshResult = true;
			PendingReadinessRetryCount = 10;
			LastMessage = FText::Format(LOCTEXT("SetupFinishedValidating", "{0} Validating readiness..."), Message);
			const TArray<FHyperAIStudioPrerequisiteStatus> EnabledMissingPrerequisites = Service->GetPrerequisites().FilterByPredicate([](const FHyperAIStudioPrerequisiteStatus& Entry)
			{
				return HyperAIStudio::Private::IsMissingInstallablePrerequisite(Entry)
					&& HyperAIStudio::Private::IsEnabledAgentInstallPrerequisite(Entry);
			});
			if (!EnabledMissingPrerequisites.IsEmpty())
			{
				const FString TerminalCommand = HyperAIStudio::Private::BuildMissingPrerequisitesTerminalCommand(EnabledMissingPrerequisites, FHyperAIStudioService::GetProjectRoot());
				if (!TerminalCommand.IsEmpty())
				{
					Service->CopyTextToClipboard(TerminalCommand);
					ActiveInstallingPrerequisiteId = TEXT("__setup__");
					FText TerminalMessage;
					if (OpenChatTerminalInstallCommand(TerminalCommand, TEXT("enabled agent prerequisites install"), TerminalMessage))
					{
						LastMessage = FText::Format(
							LOCTEXT("SetupFinishedInstallAgents", "{0} Opened HyperAI Chat and stopped the agent terminal before installing missing enabled-agent CLI prerequisite(s). Click Verify after it finishes."),
							Message);
					}
					else
					{
						LastMessage = FText::Format(
							LOCTEXT("SetupFinishedInstallAgentsFailed", "{0} Could not open HyperAI Chat for missing CLI prerequisite(s): {1} Command copied as fallback."),
							Message,
							TerminalMessage);
					}
				}
			}
		}
	}
	else
	{
		PendingReadinessRetryCount = 0;
		LastMessage = FText::Format(LOCTEXT("SetupFailed", "Setup failed: {0}"), Message);
	}
	AddSessionLogEntry(FString::Printf(TEXT("Setup %s: %s"), bOk ? TEXT("completed") : TEXT("failed"), *Message.ToString()));
	if (Outcome == EHyperAIStudioSetupOutcome::RestartRequired)
	{
		Status = Service->GetStatusSync();
		FUnrealEdMisc::Get().RestartEditor(true);
		return FReply::Handled();
	}
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnEnablePluginsClicked()
{
	// Enabling an Epic toolset wrapper is restart-bound. Route every Enable/Sync entry point
	// through the complete idempotent setup flow so agent files are generated, the standard
	// restart is offered once, and post-restart MCP setup resumes automatically.
	return OnSetupClicked();
}

FReply SHyperAIStudioWindow::OnRefreshClicked()
{
	ActiveInstallingPrerequisiteId.Empty();
	bReportNextRefreshResult = true;
	PendingReadinessRetryCount = 3;
	LastMessage = LOCTEXT("ValidationStarted", "Validating HyperAIStudio readiness...");
	AddSessionLogEntry(TEXT("Validation started."));
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnStartClicked()
{
	FText Message;
	Service->StartUnrealMCP(Message);
	PendingReadinessRetryCount = 10;
	LastMessage = Message;
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnStopClicked()
{
	FText Message;
	Service->StopUnrealMCP(Message);
	PendingReadinessRetryCount = 0;
	LastMessage = Message;
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnAddServerClicked()
{
	FText Message;
	if (Service->AddDefaultCustomServer(Message))
	{
		SaveMCPServerChange(Message.ToString());
	}
	else
	{
		LastMessage = Message;
		RefreshStatus();
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnEditMCPServerClicked(int32 ServerIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FReply::Handled();
	}

	FHyperAIStudioMCPServerEntry InitialEntry;
	InitialEntry.Id = TEXT("custom-mcp");
	InitialEntry.DisplayName = TEXT("Custom MCP");
	InitialEntry.Description = TEXT("Custom local MCP server.");
	InitialEntry.Transport = EHyperAIStudioMCPTransport::StreamableHttp;
	InitialEntry.Url = TEXT("http://127.0.0.1:9000/mcp");
	InitialEntry.Priority = 100;
	InitialEntry.TargetClients = { TEXT("codex"), TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini") };

	const UHyperAIStudioSettings* CurrentSettings = GetDefault<UHyperAIStudioSettings>();
	const bool bEditingExisting = CurrentSettings && CurrentSettings->ExtraServers.IsValidIndex(ServerIndex);
	if (bEditingExisting)
	{
		InitialEntry = CurrentSettings->ExtraServers[ServerIndex];
	}

	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> EndpointBox;
	TSharedPtr<SEditableTextBox> PriorityBox;
	TSharedPtr<SEditableTextBox> DescriptionBox;
	TSharedPtr<SEditableTextBox> ArgsBox;
	TSharedPtr<SCheckBox> EnabledBox;
	TSharedPtr<SCheckBox> StdioBox;

	const TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(bEditingExisting ? LOCTEXT("EditMCPDialogTitle", "Edit MCP Server") : LOCTEXT("AddMCPDialogTitle", "Add MCP Server"))
		.ClientSize(FVector2D(760.0f, 520.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakDialog = Dialog;

	Dialog->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("MCPDialogName", "Name"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(NameBox, SEditableTextBox)
				.Text(FText::FromString(InitialEntry.DisplayName))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 18.0f, 0.0f)
				[
					SAssignNew(EnabledBox, SCheckBox)
					.IsChecked(InitialEntry.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					[
						SNew(STextBlock).Text(LOCTEXT("MCPDialogEnabled", "Enabled"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SAssignNew(StdioBox, SCheckBox)
					.IsChecked(InitialEntry.Transport == EHyperAIStudioMCPTransport::Command ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					[
						SNew(STextBlock).Text(LOCTEXT("MCPDialogStdio", "STDIO"))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("MCPDialogEndpoint", "Command or URL"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(EndpointBox, SEditableTextBox)
				.Text(FText::FromString(InitialEntry.Transport == EHyperAIStudioMCPTransport::Command ? InitialEntry.Command : InitialEntry.Url))
				.HintText(LOCTEXT("MCPDialogEndpointHint", "npx / uvx / local executable, or http://127.0.0.1:9000/mcp"))
			]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("MCPDialogArgs", "Arguments"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(ArgsBox, SEditableTextBox)
				.Text(FText::FromString(FString::Join(InitialEntry.Arguments, TEXT(" "))))
				.HintText(LOCTEXT("MCPDialogArgsHint", "STDIO arguments, space-separated"))
			]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("MCPDialogPriority", "Priority"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(PriorityBox, SEditableTextBox)
				.Text(FText::AsNumber(InitialEntry.Priority))
			]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("MCPDialogDescription", "Description"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
			[
				SAssignNew(DescriptionBox, SEditableTextBox)
				.Text(FText::FromString(InitialEntry.Description))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MCPDialogExportHint", "Export to agents: Codex, Claude Code, Cursor, VS Code/Copilot, Gemini. Environment values are not stored here."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)[SNew(SBox)]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("MCPDialogSave", "Save"))
					.OnClicked_Lambda([this, WeakDialog, ServerIndex, NameBox, EndpointBox, ArgsBox, PriorityBox, DescriptionBox, EnabledBox, StdioBox]()
					{
						UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
						if (!Settings)
						{
							LastMessage = LOCTEXT("MCPDialogNoSettings", "HyperAIStudio settings are unavailable.");
							return FReply::Handled();
						}

						FHyperAIStudioMCPServerEntry Entry;
						if (Settings->ExtraServers.IsValidIndex(ServerIndex))
						{
							Entry = Settings->ExtraServers[ServerIndex];
						}
						Entry.DisplayName = NameBox.IsValid() ? NameBox->GetText().ToString().TrimStartAndEnd() : TEXT("Custom MCP");
						if (Entry.DisplayName.IsEmpty())
						{
							Entry.DisplayName = TEXT("Custom MCP");
						}
						Entry.Id = Entry.Id.IsEmpty() ? Entry.DisplayName : Entry.Id;
						Entry.Description = DescriptionBox.IsValid() ? DescriptionBox->GetText().ToString().TrimStartAndEnd() : FString();
						Entry.Priority = FMath::Clamp(PriorityBox.IsValid() ? FCString::Atoi(*PriorityBox->GetText().ToString()) : 100, 0, 10000);
						Entry.bEnabled = EnabledBox.IsValid() && EnabledBox->IsChecked();
						Entry.Transport = StdioBox.IsValid() && StdioBox->IsChecked() ? EHyperAIStudioMCPTransport::Command : EHyperAIStudioMCPTransport::StreamableHttp;
						const FString EndpointValue = EndpointBox.IsValid() ? EndpointBox->GetText().ToString().TrimStartAndEnd() : FString();
						if (Entry.Transport == EHyperAIStudioMCPTransport::Command)
						{
							Entry.Command = EndpointValue;
							Entry.Url.Reset();
							Entry.Arguments.Reset();
							if (ArgsBox.IsValid())
							{
								ArgsBox->GetText().ToString().ParseIntoArrayWS(Entry.Arguments);
							}
						}
						else
						{
							Entry.Url = EndpointValue;
							Entry.Command.Reset();
							Entry.Arguments.Reset();
						}
						if (Entry.TargetClients.Num() == 0)
						{
							Entry.TargetClients = { TEXT("codex"), TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini") };
						}

						if (Settings->ExtraServers.IsValidIndex(ServerIndex))
						{
							Settings->ExtraServers[ServerIndex] = Entry;
						}
						else
						{
							Settings->ExtraServers.Add(Entry);
						}
						SaveMCPServerChange(FString::Printf(TEXT("Saved MCP server `%s`."), *Entry.DisplayName));
						if (const TSharedPtr<SWindow> PinnedDialog = WeakDialog.Pin())
						{
							PinnedDialog->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("MCPDialogCancel", "Cancel"))
					.OnClicked_Lambda([WeakDialog]()
					{
						if (const TSharedPtr<SWindow> PinnedDialog = WeakDialog.Pin())
						{
							PinnedDialog->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]);
	FSlateApplication::Get().AddWindow(Dialog);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenChatClicked()
{
	if (FSlateApplication::IsInitialized())
	{
		if (IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(TEXT("HyperAIStudio.Chat")))
		{
			if (IConsoleCommand* ConsoleCommand = ConsoleObject->AsCommand())
			{
				ConsoleCommand->Execute(TArray<FString>(), nullptr, *GLog);
				LastMessage = LOCTEXT("ChatPanelOpened", "Opened Chat / Terminal.");
				return FReply::Handled();
			}
		}
		LastMessage = LOCTEXT("ChatPanelUnavailable", "Chat / Terminal command is unavailable.");
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnConfigureAgentsClicked()
{
	ActiveTabIndex = 1;
	bUserSelectedTab = true;
	bAutoRouteAfterFirstRefresh = false;
	ResetContentScroll();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnViewPrerequisitesClicked()
{
	if (!FSlateApplication::IsInitialized())
	{
		return FReply::Handled();
	}

	RefreshPrerequisitesCache();
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FHyperAIStudioPrerequisiteStatus& Prerequisite : CachedPrerequisites)
	{
		const bool bOk = Prerequisite.bAvailable && Prerequisite.bEnabled;
		const bool bInstalling = ActiveInstallingPrerequisiteId == Prerequisite.Id;
		Rows->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.26f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Prerequisite.DisplayName))]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)[SNew(STextBlock).Text(FText::FromString(Prerequisite.Detail)).ColorAndOpacity(HyperAIStudio::Private::MutedColor())]
				]
				+ SHorizontalBox::Slot().FillWidth(0.16f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(bInstalling ? TEXT("Installing...") : (bOk ? TEXT("Ready") : (Prerequisite.bAvailable ? TEXT("Needs enable") : TEXT("Missing")))))
					.ColorAndOpacity(bOk ? HyperAIStudio::Private::GoodColor() : (bInstalling ? HyperAIStudio::Private::WarningColor() : HyperAIStudio::Private::WarningColor()))
				]
				+ SHorizontalBox::Slot().FillWidth(0.36f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Prerequisite.ActionHint)).AutoWrapText(true).ColorAndOpacity(HyperAIStudio::Private::MutedColor())
				]
				+ SHorizontalBox::Slot().FillWidth(0.22f).VAlign(VAlign_Center)
				[
					SNew(SWrapBox)
					+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
					[
						SNew(SButton)
							.Text(Prerequisite.bCanEnable ? LOCTEXT("PrereqEnable", "Enable") : LOCTEXT("PrereqInstall", "Install"))
							.IsEnabled(!bOk && (Prerequisite.bCanEnable || !Prerequisite.InstallCommand.IsEmpty()))
							.OnClicked(Prerequisite.bCanEnable
								? FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnEnablePluginsClicked)
								: FOnClicked::CreateSP(this, &SHyperAIStudioWindow::OnInstallPrerequisiteClicked, Prerequisite.Id, Prerequisite.DisplayName, Prerequisite.InstallCommand))
					]
					+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("PrereqVerify", "Verify"))
						.OnClicked(this, &SHyperAIStudioWindow::OnRefreshClicked)
					]
					+ SWrapBox::Slot().Padding(0.0f, 0.0f, 6.0f, 6.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("PrereqDocs", "Docs"))
						.IsEnabled(!Prerequisite.DocumentationUrl.IsEmpty())
						.OnClicked(this, &SHyperAIStudioWindow::OnOpenPrerequisiteDocsClicked, Prerequisite.DisplayName, Prerequisite.DocumentationUrl)
					]
				]
			]
		];
	}

	const TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(LOCTEXT("PrerequisitesDialogTitle", "Prerequisites"))
		.ClientSize(FVector2D(900.0f, 560.0f))
		.SupportsMaximize(true)
		.SupportsMinimize(false);

	Dialog->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PrerequisitesDialogIntro", "Install or enable only what you need. Install commands open visibly in HyperAI Chat after resetting any active embedded agent terminal."))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("InstallMissingPrerequisites", "Install Missing Prerequisites"))
					.ToolTipText(LOCTEXT("InstallMissingPrerequisitesTooltip", "Open HyperAI Chat, stop any active embedded agent terminal, and run the visible installer commands for missing external tools."))
					.IsEnabled_Lambda([this]()
					{
						return HasCachedMissingInstallablePrerequisites();
					})
					.OnClicked(this, &SHyperAIStudioWindow::OnInstallMissingPrerequisitesClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PrereqDialogVerify", "Verify"))
					.OnClicked(this, &SHyperAIStudioWindow::OnRefreshClicked)
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()[Rows]
			]
		]);
	FSlateApplication::Get().AddWindow(Dialog);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenAgentFilesDialogClicked()
{
	if (!FSlateApplication::IsInitialized())
	{
		return FReply::Handled();
	}

	const TArray<FString> Files = {
		TEXT("AGENTS.md"),
		TEXT("CLAUDE.md"),
		TEXT(".codex/config.toml"),
		TEXT(".mcp.json"),
		TEXT(".cursor/mcp.json"),
		TEXT(".vscode/mcp.json"),
		TEXT(".gemini/settings.json"),
		TEXT(".hyperai")
	};

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FString& File : Files)
	{
		auto FileExists = [File]()
		{
			return HyperAIStudio::Private::ProjectRelativePathExists(File);
		};
		Rows->AddSlot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(File))
					.ColorAndOpacity_Lambda([FileExists]()
					{
						return FileExists() ? FSlateColor::UseForeground() : HyperAIStudio::Private::MutedColor();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentFileCreate", "Create"))
					.ToolTipText(LOCTEXT("AgentFileCreateTooltip", "Generate this missing HyperAIStudio file now. Existing managed/user content is preserved; incompatible files stop generation without modification."))
					.IsEnabled_Lambda([FileExists]()
					{
						return !FileExists();
					})
					.OnClicked(this, &SHyperAIStudioWindow::OnCreateAgentFileClicked, File)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentFileOpen", "Open"))
					.IsEnabled_Lambda(FileExists)
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenProjectPathClicked, File)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentFileReveal", "Reveal"))
					.IsEnabled_Lambda(FileExists)
					.OnClicked(this, &SHyperAIStudioWindow::OnRevealProjectPathClicked, File)
				]
			]
		];
	}

	const TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(LOCTEXT("AgentFilesDialogTitle", "Agent Files"))
		.ClientSize(FVector2D(720.0f, 420.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	Dialog->SetContent(SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(14.0f)[SNew(SScrollBox) + SScrollBox::Slot()[Rows]]);
	FSlateApplication::Get().AddWindow(Dialog);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenAgentFilesForAgentClicked(FString AgentName)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FReply::Handled();
	}

	TArray<FString> Files = { TEXT("AGENTS.md") };
	if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
	{
		Files.Add(TEXT("CLAUDE.md"));
		Files.Add(TEXT(".mcp.json"));
	}
	else if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		Files.Add(TEXT(".codex/config.toml"));
	}
	else if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
	{
		Files.Add(TEXT(".gemini/settings.json"));
	}
	else if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
	{
		Files.Add(TEXT(".cursor/mcp.json"));
	}
	else if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase))
	{
		Files.Add(TEXT(".vscode/mcp.json"));
	}
	Files.Add(TEXT(".hyperai"));

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FString& File : Files)
	{
		auto FileExists = [File]()
		{
			return HyperAIStudio::Private::ProjectRelativePathExists(File);
		};
		Rows->AddSlot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(File))]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentSpecificFileCreate", "Create"))
					.IsEnabled_Lambda([FileExists]()
					{
						return !FileExists();
					})
					.OnClicked(this, &SHyperAIStudioWindow::OnCreateAgentFileClicked, File)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentSpecificFileOpen", "Open"))
					.IsEnabled_Lambda(FileExists)
					.OnClicked(this, &SHyperAIStudioWindow::OnOpenProjectPathClicked, File)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AgentSpecificFileReveal", "Reveal"))
					.IsEnabled_Lambda(FileExists)
					.OnClicked(this, &SHyperAIStudioWindow::OnRevealProjectPathClicked, File)
				]
			]
		];
	}

	const TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(FText::FromString(FString::Printf(TEXT("%s Files"), *AgentName)))
		.ClientSize(FVector2D(680.0f, 360.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	Dialog->SetContent(SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(14.0f)[SNew(SScrollBox) + SScrollBox::Slot()[Rows]]);
	FSlateApplication::Get().AddWindow(Dialog);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCreateAgentFileClicked(FString RelativePath)
{
	if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		HyperAIStudio::Private::SelectGenerationForProjectRelativePath(*Settings, RelativePath);
		Settings->SaveConfig();
	}

	FText Message;
	const bool bGenerated = Service->GenerateProjectFiles(Message);
	const bool bExists = HyperAIStudio::Private::ProjectRelativePathExists(RelativePath);
	if (bGenerated && bExists)
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Created %s."), *RelativePath));
		AddSessionLogEntry(FString::Printf(TEXT("Created %s through Agent Files."), *RelativePath));
	}
	else if (bGenerated)
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Generate ran, but %s is still missing. %s"), *RelativePath, *Message.ToString()));
		AddSessionLogEntry(FString::Printf(TEXT("Generate ran but %s is still missing: %s"), *RelativePath, *Message.ToString()));
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("CreateAgentFileFailed", "Create failed for {0}: {1}"), FText::FromString(RelativePath), Message);
		AddSessionLogEntry(FString::Printf(TEXT("Create failed for %s: %s"), *RelativePath, *Message.ToString()));
	}
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnRegenerateAgentFilesClicked()
{
	FText Message;
	const bool bOk = Service->GenerateProjectFiles(Message);
	LastMessage = bOk
		? Message
		: FText::Format(LOCTEXT("RegenerateAgentFilesFailed", "Regenerate files failed: {0}"), Message);
	AddSessionLogEntry(FString::Printf(TEXT("Regenerate files %s: %s"), bOk ? TEXT("completed") : TEXT("failed"), *Message.ToString()));
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnGenerateAgentConfigClicked(FString AgentName)
{
	const FString ConfigPath = HyperAIStudio::Private::GetConfigPathForAgentName(AgentName);
	if (ConfigPath.IsEmpty())
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("No project config path is defined for %s."), *AgentName));
		return FReply::Handled();
	}

	if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		Settings->SetBuiltInAgentEnabled(AgentName, true);
		HyperAIStudio::Private::SelectGenerationForProjectRelativePath(*Settings, ConfigPath);
		Settings->SaveConfig();
	}

	FText Message;
	const bool bGenerated = Service->GenerateProjectFiles(Message);
	const bool bExists = HyperAIStudio::Private::ProjectRelativePathExists(ConfigPath);
	if (bGenerated && bExists)
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Generated %s config at %s."), *AgentName, *ConfigPath));
		AddSessionLogEntry(FString::Printf(TEXT("Generated %s config at %s."), *AgentName, *ConfigPath));
	}
	else if (bGenerated)
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Generate ran, but %s is still missing for %s. %s"), *ConfigPath, *AgentName, *Message.ToString()));
		AddSessionLogEntry(FString::Printf(TEXT("Generate ran but %s is still missing for %s: %s"), *ConfigPath, *AgentName, *Message.ToString()));
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("GenerateAgentConfigFailed", "Generate config failed for {0}: {1}"), FText::FromString(AgentName), Message);
		AddSessionLogEntry(FString::Printf(TEXT("Generate config failed for %s: %s"), *AgentName, *Message.ToString()));
	}
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnUseAgentInChatClicked(FString AgentName)
{
	const FString TrimmedAgentName = AgentName.TrimStartAndEnd();
	if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		Settings->PreferredAgent = TrimmedAgentName;
		Settings->SaveConfig();
	}
	if (FSlateApplication::IsInitialized())
	{
		if (IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(TEXT("HyperAIStudio.Chat")))
		{
			if (IConsoleCommand* ConsoleCommand = ConsoleObject->AsCommand())
			{
				TArray<FString> Args;
				if (!TrimmedAgentName.IsEmpty())
				{
					Args.Add(TrimmedAgentName);
				}
				ConsoleCommand->Execute(Args, nullptr, *GLog);
				LastMessage = FText::Format(LOCTEXT("AgentChatPanelOpened", "Opened Chat / Terminal for {0}."), FText::FromString(TrimmedAgentName));
				return FReply::Handled();
			}
		}
		LastMessage = LOCTEXT("ChatPanelUnavailable", "Chat / Terminal command is unavailable.");
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnPromptInAgentClicked(FString AgentName)
{
	FText Message;
	if (!Service->CreateQuickConnectPromptPackage(AgentName, LastPromptPackage, Message))
	{
		LastMessage = FText::Format(LOCTEXT("CopyAgentPromptFailed", "Copy prompt failed: {0}"), Message);
		AddSessionLogEntry(FString::Printf(TEXT("%s prompt copy failed: %s"),
			AgentName.IsEmpty() ? TEXT("Preferred agent") : *AgentName,
			*Message.ToString()));
		return FReply::Handled();
	}

	FString PromptText = LastPromptPackage.PromptText;
	if (PromptText.IsEmpty() && !LastPromptPackage.PromptPath.IsEmpty())
	{
		FFileHelper::LoadFileToString(PromptText, *LastPromptPackage.PromptPath);
	}
	if (PromptText.IsEmpty())
	{
		LastMessage = LOCTEXT("CopyAgentPromptEmpty", "Prompt file was created, but the prompt text is empty.");
		AddSessionLogEntry(FString::Printf(TEXT("%s prompt copy failed: empty prompt"),
			AgentName.IsEmpty() ? TEXT("Preferred agent") : *AgentName));
		return FReply::Handled();
	}

	Service->CopyTextToClipboard(PromptText);
	const FString InstructionFile = HyperAIStudio::Private::GetInstructionFileForAgentName(AgentName);
	LastMessage = FText::FromString(FString::Printf(TEXT("Prompt copied for %s. Project-root note: open this project root when possible so the agent can load its generated config and %s instructions."), *AgentName, *InstructionFile));
	AddSessionLogEntry(FString::Printf(TEXT("%s prompt copied for manual paste: %s"),
		AgentName.IsEmpty() ? TEXT("Preferred agent") : *AgentName,
		*LastPromptPackage.PromptPath));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenMCPToolsClicked(FString ServerName, FString ToolsSummary)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FReply::Handled();
	}

	const bool bIsUnrealMCP = ServerName.Equals(TEXT("Unreal MCP"), ESearchCase::IgnoreCase);
	const TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(FText::FromString(ServerName + TEXT(" Tools")))
		.ClientSize(FVector2D(980.0f, 620.0f))
		.SupportsMaximize(true)
		.SupportsMinimize(false);

	if (!bIsUnrealMCP)
	{
		Dialog->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(14.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ToolsSummary.IsEmpty()
					? TEXT("No tool inventory is available for this custom server yet.")
					: ToolsSummary))
				.AutoWrapText(true)
			]
			);
		FSlateApplication::Get().AddWindow(Dialog);
		return FReply::Handled();
	}

	const TSharedRef<HyperAIStudio::Private::FToolInventoryDialogState> State =
		MakeShared<HyperAIStudio::Private::FToolInventoryDialogState>();
	const TOptional<FHyperAIStudioCapabilityInventoryResult> CachedInventory =
		FHyperAIStudioCapabilityInventoryClient::GetLastDetailedSuccess(
			Status.Endpoint,
			EHyperAIStudioDetailedInventoryScope::EpicToolsets);
	if (CachedInventory.IsSet())
	{
		FHyperAIStudioToolInventoryView CachedView =
			FHyperAIStudioToolInventoryViewModel::Build(CachedInventory.GetValue());
		if (CachedView.bExactLiveInventory)
		{
			CachedLiveEpicToolCount = CachedView.LiveEpicToolCount;
			CachedLiveHyperToolCount = CachedView.AvailableHyperToolCount;
		}
		State->SetView(MoveTemp(CachedView));
	}
	else
	{
		FHyperAIStudioCapabilityInventoryResult InitialInventory;
		InitialInventory.Message = TEXT("Loading the exact live Epic tool inventory...");
		State->SetView(FHyperAIStudioToolInventoryViewModel::Build(InitialInventory));
	}
	State->bRefreshing = true;
	TSharedPtr<SListView<HyperAIStudio::Private::FToolInventoryItem>> ToolList;

	auto MakeSourceChoice = [State](
		const FText& Label,
		const TOptional<EHyperAIStudioToolInventorySource> Source)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "Menu.Button")
			.Text(Label)
			.OnClicked_Lambda([State, Source]()
			{
				State->SourceFilter = Source;
				State->RefreshFilter();
				FSlateApplication::Get().DismissAllMenus();
				return FReply::Handled();
			});
	};

	Dialog->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([State]() { return State->GetSummaryText(); })
				.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("Optional UE 5.8 source catalog: %d known Epic declarations. The live count above includes only tools available in this project."),
						FHyperAIStudioToolInventoryView::KnownEpicUE58DeclarationCount)))
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::Private::MutedColor())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("MCPToolsSearchHint", "Search tool name, description, or toolset"))
					.OnTextChanged_Lambda([State](const FText& Text)
					{
						State->Query = Text.ToString();
						State->RefreshFilter();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SComboButton)
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text_Lambda([State]() { return State->GetSourceFilterText(); })
					]
					.MenuContent()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							MakeSourceChoice(LOCTEXT("ToolInventoryAllSources", "All sources"), {})
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							MakeSourceChoice(LOCTEXT("ToolInventoryEpicSource", "Epic"), EHyperAIStudioToolInventorySource::Epic)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							MakeSourceChoice(LOCTEXT("ToolInventoryHyperSource", "HyperAI"), EHyperAIStudioToolInventorySource::HyperAI)
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ToolInventoryRefresh", "Refresh"))
					.ToolTipText(LOCTEXT("ToolInventoryRefreshTooltip", "Rescan every currently registered Unreal MCP toolset."))
					.IsEnabled_Lambda([State]() { return !State->bRefreshing; })
					.OnClicked_Lambda([State]()
					{
						if (State->RefreshInventory)
						{
							State->RefreshInventory();
						}
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 3.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.23f)[SNew(STextBlock).Text(LOCTEXT("ToolInventoryToolHeader", "Tool"))]
				+ SHorizontalBox::Slot().FillWidth(0.45f).Padding(8.0f, 0.0f)[SNew(STextBlock).Text(LOCTEXT("ToolInventoryDescriptionHeader", "Description"))]
				+ SHorizontalBox::Slot().FillWidth(0.10f).Padding(8.0f, 0.0f)[SNew(STextBlock).Text(LOCTEXT("ToolInventorySourceHeader", "Source"))]
				+ SHorizontalBox::Slot().FillWidth(0.14f).Padding(8.0f, 0.0f)[SNew(STextBlock).Text(LOCTEXT("ToolInventoryTypeHeader", "Type"))]
				+ SHorizontalBox::Slot().FillWidth(0.08f).Padding(8.0f, 0.0f)[SNew(STextBlock).Text(LOCTEXT("ToolInventoryStatusHeader", "Status"))]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(4.0f)
				[
					SAssignNew(ToolList, SListView<HyperAIStudio::Private::FToolInventoryItem>)
					.ListItemsSource(&State->FilteredRows)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow_Lambda([](
						const HyperAIStudio::Private::FToolInventoryItem& Item,
						const TSharedRef<STableViewBase>& OwnerTable)
					{
						const FSlateColor TextColor = Item->bAvailableNow
							? FSlateColor::UseForeground()
							: HyperAIStudio::Private::MutedColor();
						return SNew(STableRow<HyperAIStudio::Private::FToolInventoryItem>, OwnerTable)
							.ToolTipText(FText::FromString(Item->QualifiedName + TEXT("\n") + Item->Toolset + TEXT("\n\n") + Item->Description))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().FillWidth(0.23f).VAlign(VAlign_Center)
								[
									SNew(STextBlock).Text(FText::FromString(Item->Name)).ColorAndOpacity(TextColor)
								]
								+ SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center).Padding(8.0f, 0.0f)
								[
									SNew(STextBlock).Text(FText::FromString(Item->Description)).ColorAndOpacity(TextColor)
								]
								+ SHorizontalBox::Slot().FillWidth(0.10f).VAlign(VAlign_Center).Padding(8.0f, 0.0f)
								[
									SNew(STextBlock).Text(Item->Source == EHyperAIStudioToolInventorySource::Epic
										? LOCTEXT("ToolInventoryRowEpic", "Epic")
										: LOCTEXT("ToolInventoryRowHyperAI", "HyperAI")).ColorAndOpacity(TextColor)
								]
								+ SHorizontalBox::Slot().FillWidth(0.14f).VAlign(VAlign_Center).Padding(8.0f, 0.0f)
								[
									SNew(STextBlock).Text(FText::FromString(Item->Capability)).ColorAndOpacity(TextColor)
								]
								+ SHorizontalBox::Slot().FillWidth(0.08f).VAlign(VAlign_Center).Padding(8.0f, 0.0f)
								[
									SNew(STextBlock).Text(Item->bAvailableNow
										? LOCTEXT("ToolInventoryAvailable", "Available")
										: LOCTEXT("ToolInventoryUnavailable", "Not loaded")).ColorAndOpacity(TextColor)
								]
							];
					})
				]
			]
		]);
	State->ListView = ToolList;
	State->RefreshFilter();
	FSlateApplication::Get().AddWindow(Dialog);

	const TWeakPtr<HyperAIStudio::Private::FToolInventoryDialogState> WeakState = State;
	const TWeakPtr<SHyperAIStudioWindow> WeakWindow = SharedThis(this);
	const FString InventoryEndpoint = Status.Endpoint;
	State->RefreshInventory = [WeakState, WeakWindow, InventoryEndpoint]()
	{
		if (const TSharedPtr<HyperAIStudio::Private::FToolInventoryDialogState> PinnedState = WeakState.Pin())
		{
			PinnedState->bRefreshing = true;
			PinnedState->RefreshNotice.Reset();
		}
		FHyperAIStudioCapabilityInventoryClient::RefreshDetailedAsync(
			InventoryEndpoint,
			true,
			EHyperAIStudioDetailedInventoryScope::EpicToolsets,
			[WeakState, WeakWindow](const FHyperAIStudioCapabilityInventoryResult& Result)
			{
				FHyperAIStudioToolInventoryView View = FHyperAIStudioToolInventoryViewModel::Build(Result);
				if (View.bExactLiveInventory)
				{
					if (const TSharedPtr<SHyperAIStudioWindow> PinnedWindow = WeakWindow.Pin())
					{
						PinnedWindow->CachedLiveEpicToolCount = View.LiveEpicToolCount;
						PinnedWindow->CachedLiveHyperToolCount = View.AvailableHyperToolCount;
					}
				}
				if (const TSharedPtr<HyperAIStudio::Private::FToolInventoryDialogState> PinnedState = WeakState.Pin())
				{
					PinnedState->bRefreshing = false;
					if (View.bExactLiveInventory || !PinnedState->View.bExactLiveInventory)
					{
						PinnedState->RefreshNotice.Reset();
						PinnedState->SetView(MoveTemp(View));
					}
					else
					{
						PinnedState->RefreshNotice = FString::Printf(
							TEXT("Refresh failed; showing last exact snapshot: %s"),
							*Result.Message);
					}
				}
			});
	};
	State->RefreshInventory();

	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnSetNativeExecutionModeClicked(const EHyperAIStudioNativeExecutionMode Mode)
{
	if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		Settings->NativeExecutionMode = Mode;
		Settings->SaveConfig();
		LastMessage = Mode == EHyperAIStudioNativeExecutionMode::Fast
			? LOCTEXT("NativeExecutionFastSelected", "HyperAI execution set to Fast. Destructive and external-effect tools remain strict.")
			: LOCTEXT("NativeExecutionStrictSelected", "HyperAI execution set to Strict Safety.");
	}
	FSlateApplication::Get().DismissAllMenus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnSetNativeToolChannelClicked(const EHyperAIStudioNativeToolChannel Channel)
{
	if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		Settings->NativeToolChannel = Channel;
		Settings->SaveConfig();
		FHyperAIStudioCapabilityInventoryClient::InvalidateDetailed(Status.Endpoint);
		CachedLiveEpicToolCount = INDEX_NONE;
		CachedLiveHyperToolCount = INDEX_NONE;
		LastMessage = Channel == EHyperAIStudioNativeToolChannel::Preview
			? LOCTEXT("NativeToolPreviewSelected", "Unreal MCP + Extended Hyper Tools selected. Restart Unreal Editor to load the extended tools.")
			: LOCTEXT("NativeToolStableSelected", "Unreal MCP Only selected. Restart Unreal Editor to unload HyperAIStudio's extended tools.");
	}
	FSlateApplication::Get().DismissAllMenus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenProjectPathClicked(FString RelativePath)
{
	const FString Path = FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), RelativePath);
	if (FPaths::FileExists(Path))
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
		LastMessage = FText::FromString(FString::Printf(TEXT("Opened %s."), *RelativePath));
	}
	else if (FPaths::DirectoryExists(Path))
	{
		FPlatformProcess::ExploreFolder(*Path);
		LastMessage = FText::FromString(FString::Printf(TEXT("Opened %s."), *RelativePath));
	}
	else
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("%s does not exist yet."), *RelativePath));
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnRevealProjectPathClicked(FString RelativePath)
{
	const FString Path = FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), RelativePath);
	const FString Folder = FPaths::DirectoryExists(Path) ? Path : FPaths::GetPath(Path);
	if (FPaths::DirectoryExists(Folder))
	{
		FPlatformProcess::ExploreFolder(*Folder);
		LastMessage = FText::FromString(FString::Printf(TEXT("Revealed %s."), *RelativePath));
	}
	else
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("%s does not exist yet."), *RelativePath));
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnInstallPrerequisiteClicked(FString PrerequisiteId, FString DisplayName, FString InstallCommand)
{
	if (InstallCommand.IsEmpty())
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("No install command is defined for %s."), *DisplayName));
		return FReply::Handled();
	}

	ActiveInstallingPrerequisiteId = PrerequisiteId;
	Service->CopyTextToClipboard(InstallCommand);
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const FString TerminalCommand = HyperAIStudio::Private::BuildPrerequisiteTerminalCommand(PrerequisiteId, DisplayName, InstallCommand, ProjectRoot);
	if (TerminalCommand.IsEmpty())
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Could not build installer command for %s. Command copied; paste it in a terminal."), *DisplayName));
		RefreshStatus();
		return FReply::Handled();
	}

	FText TerminalMessage;
	if (OpenChatTerminalInstallCommand(TerminalCommand, FString::Printf(TEXT("%s install"), *DisplayName), TerminalMessage))
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("%s install opened in HyperAI Chat. Any active agent terminal was stopped first. Command copied as fallback. Click Verify after it finishes."), *DisplayName));
	}
	else
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Could not open HyperAI Chat for %s: %s Command copied; paste it manually."), *DisplayName, *TerminalMessage.ToString()));
	}
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnInstallMissingPrerequisitesClicked()
{
	const TArray<FHyperAIStudioPrerequisiteStatus> Prerequisites = Service->GetPrerequisites();
	const TArray<FHyperAIStudioPrerequisiteStatus> MissingInstallable = Prerequisites.FilterByPredicate([](const FHyperAIStudioPrerequisiteStatus& Entry)
	{
		return HyperAIStudio::Private::IsMissingInstallablePrerequisite(Entry);
	});
	if (MissingInstallable.IsEmpty())
	{
		LastMessage = LOCTEXT("NoMissingPrerequisites", "No missing installable external prerequisites were found.");
		RefreshStatus();
		return FReply::Handled();
	}

	const FString TerminalCommand = HyperAIStudio::Private::BuildMissingPrerequisitesTerminalCommand(Prerequisites, FHyperAIStudioService::GetProjectRoot());
	if (TerminalCommand.IsEmpty())
	{
		LastMessage = LOCTEXT("MissingPrerequisitesCommandFailed", "Could not build the missing-prerequisites install command.");
		RefreshStatus();
		return FReply::Handled();
	}

	Service->CopyTextToClipboard(TerminalCommand);
	ActiveInstallingPrerequisiteId = TEXT("__missing__");
	FText TerminalMessage;
	if (OpenChatTerminalInstallCommand(TerminalCommand, TEXT("missing prerequisites install"), TerminalMessage))
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Opened HyperAI Chat and stopped the agent terminal before installing %d missing external prerequisite(s). Click Verify after it finishes."), MissingInstallable.Num()));
	}
	else
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("Could not open HyperAI Chat for missing prerequisites: %s Command copied; paste it manually."), *TerminalMessage.ToString()));
	}
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyExampleMCPProfileClicked()
{
	Service->CopyTextToClipboard(Service->GetExampleMCPProfileJson());
	LastMessage = LOCTEXT("ExampleMCPProfileCopied", "Example hyperai-mcp.json profile copied to clipboard.");
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnImportExampleMCPProfileClicked()
{
	FText Message;
	if (Service->ImportExampleMCPProfile(Message))
	{
		SaveMCPServerChange(Message.ToString());
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("ImportExampleMCPProfileFailed", "Example MCP profile import failed: {0}"), Message);
		RefreshStatus();
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnImportMCPProfileClicked()
{
	FText Message;
	if (Service->ImportMCPProfileFromClipboard(Message))
	{
		SaveMCPServerChange(Message.ToString());
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("ImportMCPProfileFailed", "MCP profile import failed: {0}"), Message);
		RefreshStatus();
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnRemoveMCPServerClicked(int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("RemoveMCPServerMissing", "That MCP server entry no longer exists.");
		return FReply::Handled();
	}

	const FString RemovedName = Settings->ExtraServers[ServerIndex].DisplayName;
	Settings->ExtraServers.RemoveAt(ServerIndex);
	SaveMCPServerChange(FString::Printf(TEXT("Removed MCP server `%s`."), *RemovedName));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnTestMCPServerClicked(int32 ServerIndex)
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString DisplayName = Settings && Settings->ExtraServers.IsValidIndex(ServerIndex)
		? Settings->ExtraServers[ServerIndex].DisplayName
		: FString(TEXT("MCP server"));

	LastMessage = FText::FromString(FString::Printf(TEXT("Testing `%s`..."), *DisplayName));
	TWeakPtr<SHyperAIStudioWindow> WeakThis = StaticCastSharedRef<SHyperAIStudioWindow>(AsShared());
	Service->TestExtraMCPServerAsync(ServerIndex, [WeakThis](bool bOk, const FString& Message)
	{
		if (const TSharedPtr<SHyperAIStudioWindow> Pinned = WeakThis.Pin())
		{
			Pinned->LastMessage = bOk
				? FText::FromString(Message)
				: FText::Format(LOCTEXT("TestMCPServerFailed", "MCP server test failed: {0}"), FText::FromString(Message));
			Pinned->RefreshStatus();
		}
	});
	return FReply::Handled();
}

void SHyperAIStudioWindow::OnMCPServerEnabledChanged(ECheckBoxState State, int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("EnableMCPServerMissing", "That MCP server entry no longer exists.");
		return;
	}

	Settings->ExtraServers[ServerIndex].bEnabled = State == ECheckBoxState::Checked;
	SaveMCPServerChange(FString::Printf(TEXT("%s `%s`."), Settings->ExtraServers[ServerIndex].bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *Settings->ExtraServers[ServerIndex].DisplayName));
}

void SHyperAIStudioWindow::OnMCPServerNameCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("RenameMCPServerMissing", "That MCP server entry no longer exists.");
		return;
	}

	const FString NewName = Text.ToString().TrimStartAndEnd();
	if (NewName.IsEmpty() || Settings->ExtraServers[ServerIndex].DisplayName == NewName)
	{
		return;
	}

	Settings->ExtraServers[ServerIndex].DisplayName = NewName;
	SaveMCPServerChange(FString::Printf(TEXT("Renamed MCP server to `%s`."), *NewName));
}

void SHyperAIStudioWindow::OnMCPServerDescriptionCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("DescriptionMCPServerMissing", "That MCP server entry no longer exists.");
		return;
	}

	const FString NewDescription = Text.ToString().TrimStartAndEnd();
	if (Settings->ExtraServers[ServerIndex].Description == NewDescription)
	{
		return;
	}

	Settings->ExtraServers[ServerIndex].Description = NewDescription;
	SaveMCPServerChange(FString::Printf(TEXT("Updated description for `%s`."), *Settings->ExtraServers[ServerIndex].DisplayName));
}

void SHyperAIStudioWindow::OnMCPServerEndpointCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("EndpointMCPServerMissing", "That MCP server entry no longer exists.");
		return;
	}

	const FString NewValue = Text.ToString().TrimStartAndEnd();
	if (Settings->ExtraServers[ServerIndex].Transport == EHyperAIStudioMCPTransport::Command)
	{
		if (NewValue.IsEmpty() || Settings->ExtraServers[ServerIndex].Command == NewValue)
		{
			return;
		}
		Settings->ExtraServers[ServerIndex].Command = NewValue;
	}
	else
	{
		if (NewValue.IsEmpty() || Settings->ExtraServers[ServerIndex].Url == NewValue)
		{
			return;
		}
		Settings->ExtraServers[ServerIndex].Url = NewValue;
	}

	SaveMCPServerChange(FString::Printf(TEXT("Updated endpoint for `%s`."), *Settings->ExtraServers[ServerIndex].DisplayName));
}

void SHyperAIStudioWindow::OnMCPServerPriorityCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		LastMessage = LOCTEXT("PriorityMCPServerMissing", "That MCP server entry no longer exists.");
		return;
	}

	const int32 NewPriority = FMath::Clamp(FCString::Atoi(*Text.ToString()), 0, 10000);
	if (Settings->ExtraServers[ServerIndex].Priority == NewPriority)
	{
		return;
	}

	Settings->ExtraServers[ServerIndex].Priority = NewPriority;
	SaveMCPServerChange(FString::Printf(TEXT("Updated priority for `%s`."), *Settings->ExtraServers[ServerIndex].DisplayName));
}

void SHyperAIStudioWindow::SaveMCPServerChange(const FString& ChangeSummary)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (Settings)
	{
		Settings->SaveConfig();
	}

	FText Message;
	if (Service->GenerateProjectFiles(Message))
	{
		LastMessage = FText::FromString(ChangeSummary + TEXT(" Agent configs regenerated."));
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("MCPServerConfigRegenerateFailed", "{0} Config regeneration failed: {1}"), FText::FromString(ChangeSummary), Message);
	}
	RefreshStatus();
}

FReply SHyperAIStudioWindow::OnOpenAgentTestClicked(const FString AgentName)
{
	FText Message;
	const bool bOk = Service->CreateTestPromptPackage(TEXT("agent-connection"), AgentName, LastPromptPackage, Message)
		&& Service->OpenTerminalForPrompt(LastPromptPackage, Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenAgentTestFailed", "Open agent test failed: {0}"), Message);
	AddSessionLogEntry(FString::Printf(TEXT("%s connection test %s: %s"),
		AgentName.IsEmpty() ? TEXT("Preferred agent") : *AgentName,
		bOk ? TEXT("opened") : TEXT("failed"),
		*Message.ToString()));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenUnrealTerminalClicked()
{
	FText Message;
	const bool bOk = Service->OpenUnrealTerminal(Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenUnrealTerminalFailed", "Open UE Terminal failed: {0}"), Message);
	AddSessionLogEntry(FString::Printf(TEXT("UE Terminal %s: %s"), bOk ? TEXT("opened") : TEXT("failed"), *Message.ToString()));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyTerminalSetupClicked()
{
	FString Commands = GetTerminalStartupRecipeText(FHyperAIStudioService::GetProjectRoot());
	Commands.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
	Commands += TEXT("\r\n");
	Service->CopyTextToClipboard(Commands);
	LastMessage = LOCTEXT("TerminalSetupCopied", "Project-root terminal startup commands copied.");
	AddSessionLogEntry(TEXT("Project-root terminal startup commands copied."));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyInspectorCommandClicked()
{
	const FString Commands = FString::Printf(
		TEXT("npx @modelcontextprotocol/inspector\r\nREM In the Inspector, use Streamable HTTP and endpoint: %s\r\n"),
		*Status.Endpoint);
	Service->CopyTextToClipboard(Commands);
	LastMessage = LOCTEXT("InspectorCommandCopied", "MCP Inspector command copied.");
	AddSessionLogEntry(TEXT("MCP Inspector command copied."));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenAgentsClicked()
{
	FText Message;
	const bool bOk = Service->OpenAgentsFile(Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenAgentsFailed", "Open AGENTS.md failed: {0}"), Message);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenClaudeClicked()
{
	FText Message;
	const bool bOk = Service->OpenClaudeFile(Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenClaudeFailed", "Open CLAUDE.md failed: {0}"), Message);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenIncludedDocsClicked()
{
	FText Message;
	const bool bOk = Service->OpenIncludedDocs(Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenIncludedDocsFailed", "Open docs failed: {0}"), Message);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyFirstRunPromptClicked()
{
	Service->CopyTextToClipboard(Service->GetFirstRunPrompt());
	LastMessage = LOCTEXT("FirstRunPromptCopied", "Starter instructions copied. Paste them into your configured agent client; open the project root when possible so project-scoped config and rules load.");
	AddSessionLogEntry(TEXT("Starter instructions copied."));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyCodexTomlPatchClicked()
{
	Service->CopyTextToClipboard(Service->GetCodexTomlPatch());
	LastMessage = LOCTEXT("CodexTomlPatchCopied", "Codex TOML managed block copied. Review or paste it into .codex/config.toml if you do not want automatic setup to write Codex config.");
	AddSessionLogEntry(TEXT("Codex TOML managed block copied."));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenOfficialMCPDocsClicked()
{
	FPlatformProcess::LaunchURL(TEXT("https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor"), nullptr, nullptr);
	LastMessage = LOCTEXT("OfficialMCPDocsOpened", "Opened Epic Unreal MCP documentation.");
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenProjectFolderClicked()
{
	FText Message;
	const bool bOk = Service->OpenProjectFolder(Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenProjectFolderFailed", "Open project folder failed: {0}"), Message);
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyPrerequisiteCommandClicked(FString DisplayName, FString InstallCommand)
{
	if (InstallCommand.IsEmpty())
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("No install command is defined for %s."), *DisplayName));
		return FReply::Handled();
	}

	Service->CopyTextToClipboard(InstallCommand);
	LastMessage = FText::FromString(FString::Printf(TEXT("%s install command copied. Review it before running."), *DisplayName));
	AddSessionLogEntry(FString::Printf(TEXT("%s install command copied."), *DisplayName));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenPrerequisiteDocsClicked(FString DisplayName, FString DocumentationUrl)
{
	if (DocumentationUrl.IsEmpty())
	{
		LastMessage = FText::FromString(FString::Printf(TEXT("No documentation URL is defined for %s."), *DisplayName));
		return FReply::Handled();
	}

	FPlatformProcess::LaunchURL(*DocumentationUrl, nullptr, nullptr);
	LastMessage = FText::FromString(FString::Printf(TEXT("Opened %s documentation."), *DisplayName));
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnCopyTestPromptClicked(FString TestId)
{
	FText Message;
	const bool bOk = Service->CreateTestPromptPackage(TestId, FString(), LastPromptPackage, Message);
	if (bOk)
	{
		Service->CopyTextToClipboard(LastPromptPackage.PromptText);
		LastMessage = FText::Format(LOCTEXT("TestPromptCopied", "{0} instructions copied to clipboard."), Message);
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("TestPromptFailed", "Test instructions failed: {0}"), Message);
	}
	return FReply::Handled();
}

FReply SHyperAIStudioWindow::OnOpenTestTerminalClicked(FString TestId)
{
	FText Message;
	const bool bOk = Service->CreateTestPromptPackage(TestId, FString(), LastPromptPackage, Message)
		&& Service->OpenTerminalForPrompt(LastPromptPackage, Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("OpenTestTerminalFailed", "Open test terminal failed: {0}"), Message);
	return FReply::Handled();
}

bool SHyperAIStudioWindow::OpenChatTerminalInstallCommand(const FString& TerminalCommand, const FString& Label, FText& OutMessage)
{
	const FString TrimmedCommand = TerminalCommand.TrimStartAndEnd();
	if (TrimmedCommand.IsEmpty())
	{
		OutMessage = LOCTEXT("InstallTerminalEmptyCommand", "No install command is available.");
		return false;
	}

	const FString TrimmedLabel = Label.TrimStartAndEnd().IsEmpty()
		? FString(TEXT("prerequisite install"))
		: Label.TrimStartAndEnd();

	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = LOCTEXT("InstallChatNoSlate", "Slate is not initialized. Paste the copied command manually in unattended runs.");
		return false;
	}

	if (IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(TEXT("HyperAIStudio.Chat")))
	{
		if (IConsoleCommand* ConsoleCommand = ConsoleObject->AsCommand())
		{
			SHyperAIStudioQuickActionWindow::EnqueueVisibleTerminalCommand(TrimmedCommand, TrimmedLabel);
			ConsoleCommand->Execute(TArray<FString>(), nullptr, *GLog);
			OutMessage = FText::FromString(FString::Printf(TEXT("Opened HyperAI Chat for %s."), *TrimmedLabel));
			AddSessionLogEntry(FString::Printf(TEXT("Install command queued in HyperAI Chat: %s"), *TrimmedLabel));
			return true;
		}
	}

	OutMessage = LOCTEXT("InstallChatUnavailable", "HyperAI Chat command is unavailable. Command copied; paste it manually.");
	return false;
}

void SHyperAIStudioWindow::RefreshStatus()
{
	if (bRefreshing)
	{
		return;
	}

	bRefreshing = true;
	Status = Service->GetStatusSync();
	RefreshPrerequisitesCache();
	TWeakPtr<SHyperAIStudioWindow> WeakThis = StaticCastSharedRef<SHyperAIStudioWindow>(AsShared());
	Service->RefreshStatusAsync([WeakThis](const FHyperAIStudioStatus& NewStatus)
	{
		if (const TSharedPtr<SHyperAIStudioWindow> Pinned = WeakThis.Pin())
		{
			Pinned->Status = NewStatus;
			Pinned->bRefreshing = NewStatus.bProbeInProgress;
			const bool bShouldRetryReadiness =
				!NewStatus.bProbeInProgress
				&& !NewStatus.bToolsListReachable
				&& Pinned->PendingReadinessRetryCount > 0
				&& NewStatus.bUnrealMCPModuleAvailable
				&& NewStatus.bUnrealMCPSettingsConfigured
				&& NewStatus.bRequiredPluginsReady;
			if (bShouldRetryReadiness)
			{
				--Pinned->PendingReadinessRetryCount;
				const FString DetailText = HyperAIStudio::Private::GetProbeStatusDetailText(NewStatus).ToString();
				Pinned->Status.bProbeInProgress = true;
				Pinned->LastMessage = FText::FromString(FString::Printf(TEXT("Waiting for Unreal MCP tools/list... %s"), *DetailText));
				Pinned->RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(Pinned.ToSharedRef(), &SHyperAIStudioWindow::RunReadinessRetry));
				return;
			}
			if (!NewStatus.bProbeInProgress)
			{
				Pinned->PendingReadinessRetryCount = 0;
			}
			if (!NewStatus.bProbeInProgress && Pinned->bAutoRouteAfterFirstRefresh)
			{
				const int32 RoutedTabIndex = GetPostRefreshTabIndex(
					Pinned->ActiveTabIndex,
					NewStatus,
					Pinned->bUserSelectedTab,
					Pinned->bAutoRouteAfterFirstRefresh);
				if (RoutedTabIndex != Pinned->ActiveTabIndex)
				{
					Pinned->ActiveTabIndex = RoutedTabIndex;
					Pinned->ResetContentScroll();
					Pinned->LastMessage = LOCTEXT("AutoRoutedReadyMessage", "Unreal MCP is ready. Open HyperAI Chat to copy or launch an external-agent handoff.");
					Pinned->AddSessionLogEntry(TEXT("Ready route selected Agents after tools/list validation."));
				}
				Pinned->bAutoRouteAfterFirstRefresh = false;
			}
			if (!NewStatus.bProbeInProgress && Pinned->bReportNextRefreshResult)
			{
				Pinned->bReportNextRefreshResult = false;
				const FString ProbeText = NewStatus.ProbeMessage.IsEmpty()
					? FString(TEXT("No probe message."))
					: NewStatus.ProbeMessage;
				if (NewStatus.IsReady())
				{
					Pinned->LastMessage = FText::FromString(FString::Printf(TEXT("Validation passed: Ready. %s"), *ProbeText));
					Pinned->AddSessionLogEntry(FString::Printf(TEXT("Validation passed: %s"), *ProbeText));
				}
				else
				{
					const FString WarningText = NewStatus.Warnings.IsEmpty()
						? ProbeText
						: FString::Join(NewStatus.Warnings, TEXT(" "));
					Pinned->LastMessage = FText::FromString(FString::Printf(TEXT("Validation needs attention: %s"), *WarningText));
					Pinned->AddSessionLogEntry(FString::Printf(TEXT("Validation needs attention: %s"), *WarningText));
				}
			}
		}
	});
}

EActiveTimerReturnType SHyperAIStudioWindow::RunDeferredRefresh(double CurrentTime, float DeltaTime)
{
	RefreshStatus();
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioWindow::RunReadinessRetry(double CurrentTime, float DeltaTime)
{
	RefreshStatus();
	return EActiveTimerReturnType::Stop;
}

void SHyperAIStudioWindow::AddSessionLogEntry(const FString& Entry)
{
	if (Entry.IsEmpty())
	{
		return;
	}

	SessionLog.Insert(FString::Printf(TEXT("[%s] %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S")), *Entry), 0);
	constexpr int32 MaxSessionLogEntries = 8;
	if (SessionLog.Num() > MaxSessionLogEntries)
	{
		SessionLog.SetNum(MaxSessionLogEntries);
	}
}

FText SHyperAIStudioWindow::GetHeadlineText() const
{
	if (Status.IsReady())
	{
		return LOCTEXT("ReadyHeadline", "Ready: Unreal MCP is listening and tools/list is reachable.");
	}
	if (Status.bProbeInProgress)
	{
		return LOCTEXT("CheckingHeadline", "Checking Unreal MCP readiness...");
	}
	return LOCTEXT("NeedsSetupHeadline", "Setup needed: run Set Up HyperAIStudio.");
}

FSlateColor SHyperAIStudioWindow::GetHeadlineColor() const
{
	if (Status.IsReady())
	{
		return HyperAIStudio::Private::GoodColor();
	}
	if (Status.bProbeInProgress)
	{
		return HyperAIStudio::Private::WarningColor();
	}
	return HyperAIStudio::Private::ErrorColor();
}

FText SHyperAIStudioWindow::GetMessageText() const
{
	return LastMessage;
}

FText SHyperAIStudioWindow::GetSessionLogText() const
{
	if (SessionLog.Num() == 0)
	{
		return LOCTEXT("SessionLogEmpty", "No HyperAIStudio actions in this session yet.");
	}

	return FText::FromString(FString::Join(SessionLog, LINE_TERMINATOR));
}

FText SHyperAIStudioWindow::BoolText(bool bValue, const FString& TrueText, const FString& FalseText) const
{
	return FText::FromString(bValue ? TrueText : FalseText);
}

FSlateColor SHyperAIStudioWindow::BoolColor(bool bValue) const
{
	return bValue ? HyperAIStudio::Private::GoodColor() : HyperAIStudio::Private::ErrorColor();
}

#undef LOCTEXT_NAMESPACE
