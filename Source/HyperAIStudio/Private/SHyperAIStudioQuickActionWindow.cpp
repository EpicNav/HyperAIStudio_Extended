// Games by Hyper 2026.

#include "SHyperAIStudioQuickActionWindow.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioTerminalRawInput.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioPromptComposer.h"
#include "STerminal.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "TerminalSettings.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioQuickActionWindow"

namespace HyperAIStudio::QuickAction
{
	// Theme-aware: fixed linear colours were illegible on the light editor theme.
	FSlateColor GoodColor()
	{
		return FStyleColors::Success;
	}

	FSlateColor WarningColor()
	{
		return FStyleColors::Warning;
	}

	FSlateColor ErrorColor()
	{
		return FStyleColors::Error;
	}

	FSlateColor MutedColor()
	{
		return FSlateColor::UseSubduedForeground();
	}

	void OpenSettingsPage(const UDeveloperSettings* Settings)
	{
		if (Settings)
		{
			FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer(
				Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
		}
	}

	FText InitialReadyMessage()
	{
		return LOCTEXT("InitialReadyMessage", "Message the agent from the box under the terminal: Enter sends, Shift+Enter adds a line, Up recalls history. Copy Prompt is only for manual handoff/context fallback.");
	}

	FText InitialSetupMessage()
	{
		return LOCTEXT("InitialSetupMessage", "Setup is incomplete. Open Setup before using Chat / Terminal.");
	}

	FText RefreshingMessage()
	{
		return LOCTEXT("RefreshingMessage", "Refreshing HyperAIStudio readiness...");
	}

	FText MessageForStatus(const FHyperAIStudioStatus& InStatus)
	{
		return InStatus.IsReady()
			? InitialReadyMessage()
			: InitialSetupMessage();
	}

	bool IsDefaultStatusMessage(const FText& Message)
	{
		const FString Current = Message.ToString();
		return Current == InitialReadyMessage().ToString()
			|| Current == InitialSetupMessage().ToString()
			|| Current == RefreshingMessage().ToString();
	}

	struct FAgentRoute
	{
		FString Name;
		FString PrerequisiteId;
		FString ConfigPath;
		FString LaunchCommand;
	};

	struct FVisibleTerminalCommand
	{
		FString Command;
		FString Label;
	};

	TArray<FVisibleTerminalCommand>& QueuedVisibleTerminalCommands()
	{
		static TArray<FVisibleTerminalCommand> Commands;
		return Commands;
	}

	TWeakPtr<SHyperAIStudioQuickActionWindow>& ActiveQuickActionWindow()
	{
		static TWeakPtr<SHyperAIStudioQuickActionWindow> Window;
		return Window;
	}

	const TArray<FAgentRoute>& BuiltInAgentRoutes()
	{
		static const TArray<FAgentRoute> Routes = {
			{ TEXT("Codex"), TEXT("codex"), TEXT(".codex/config.toml"), TEXT("codex") },
			{ TEXT("Claude Code"), TEXT("claude"), TEXT(".mcp.json"), TEXT("claude") },
			{ TEXT("Gemini"), TEXT("gemini"), TEXT(".gemini/settings.json"), TEXT("gemini") }
		};
		return Routes;
	}

	FString EscapeForCmdEcho(FString Value)
	{
		Value.ReplaceInline(TEXT("&"), TEXT("^&"));
		Value.ReplaceInline(TEXT("|"), TEXT("^|"));
		Value.ReplaceInline(TEXT("<"), TEXT("^<"));
		Value.ReplaceInline(TEXT(">"), TEXT("^>"));
		return Value;
	}

	FString NoUsableAgentsLabel()
	{
		return TEXT("No usable agents");
	}

	bool IsPrerequisiteAvailable(const TArray<FHyperAIStudioPrerequisiteStatus>& Prerequisites, const FString& Id)
	{
		const FHyperAIStudioPrerequisiteStatus* Match = Prerequisites.FindByPredicate([&Id](const FHyperAIStudioPrerequisiteStatus& Entry)
		{
			return Entry.Id == Id;
		});
		return Match && Match->bAvailable;
	}

	const FAgentRoute* FindBuiltInRoute(const FString& AgentName)
	{
		return BuiltInAgentRoutes().FindByPredicate([&AgentName](const FAgentRoute& Route)
		{
			return Route.Name.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}
}

void SHyperAIStudioQuickActionWindow::Construct(const FArguments& InArgs)
{
	OnOpenWorkbench = InArgs._OnOpenWorkbench;
	Service = MakeShared<FHyperAIStudioService>();
	Status = Service->GetStatusSync();
	LastMessage = HyperAIStudio::QuickAction::MessageForStatus(Status);
	RefreshAgentOptions();
	ResetTranscript();
	SAssignNew(TerminalScrollBar, SScrollBar)
		.Orientation(Orient_Vertical)
		.AlwaysShowScrollbar(true)
		.Thickness(FVector2D(6.0f, 6.0f));

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildHeader()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildStatusLine()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				BuildAgentActions()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
				.Padding(6.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SAssignNew(TerminalHost, SBox)
							[
								BuildTerminalSessionWidget()
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							TerminalScrollBar.ToSharedRef()
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SAssignNew(PromptComposer, SHyperAIStudioPromptComposer)
				.OnSubmit(this, &SHyperAIStudioQuickActionWindow::SubmitComposerText)
				.OnEscape_Lambda([this]()
				{
					if (TerminalWidget.IsValid())
					{
						FSlateApplication::Get().SetKeyboardFocus(TerminalWidget, EFocusCause::SetDirectly);
					}
				})
				.AgentName_Lambda([this]() { return ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName; })
				.IsEnabled_Lambda([this]() { return TerminalWidget.IsValid() && TerminalWidget->IsSessionRunning(); })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				BuildSessionLog()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return LastMessage; })
				.AutoWrapText(true)
				.ColorAndOpacity(HyperAIStudio::QuickAction::MutedColor())
			]
		]
	];

	HyperAIStudio::QuickAction::ActiveQuickActionWindow() = StaticCastSharedRef<SHyperAIStudioQuickActionWindow>(AsShared());
	if (HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands().Num() > 0)
	{
		bTerminalStartupPaused = true;
		bTerminalStartupSent = false;
		ActiveTerminalAgentName.Reset();
	}
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredRefresh));
	RegisterActiveTimer(0.35f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup));
	RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunQueuedVisibleTerminalCommandPoll));
}

bool SHyperAIStudioQuickActionWindow::IsActiveOrSelectedAgent(const FString& AgentName) const
{
	const FString TrimmedAgentName = AgentName.TrimStartAndEnd();
	if (TrimmedAgentName.IsEmpty())
	{
		return false;
	}

	const FString ActiveAgent = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	return ActiveAgent.Equals(TrimmedAgentName, ESearchCase::IgnoreCase);
}

void SHyperAIStudioQuickActionWindow::SelectAgentByName(const FString& AgentName)
{
	const FString TrimmedAgentName = AgentName.TrimStartAndEnd();
	if (TrimmedAgentName.IsEmpty())
	{
		return;
	}

	RefreshAgentOptions();
	for (const TSharedPtr<FString>& Agent : AgentOptions)
	{
		if (Agent.IsValid() && Agent->Equals(TrimmedAgentName, ESearchCase::IgnoreCase))
		{
			SelectedAgent = Agent;
			if (AgentComboBox.IsValid())
			{
				AgentComboBox->SetSelectedItem(SelectedAgent);
			}
			AddTranscriptLine(FString::Printf(TEXT("Agent selected: %s"), **Agent));
			QueueTerminalStartup(true, true);
			return;
		}
	}
}

void SHyperAIStudioQuickActionWindow::RunVisibleTerminalCommand(const FString& Command, const FString& Label)
{
	const FString TrimmedCommand = Command.TrimStartAndEnd();
	if (TrimmedCommand.IsEmpty())
	{
		return;
	}

	const FString TrimmedLabel = Label.TrimStartAndEnd();
	const FString RawStoppedAgentName = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	const FString StoppedAgentName = RawStoppedAgentName.IsEmpty() ? FString(TEXT("embedded terminal")) : RawStoppedAgentName;
	const bool bHadActiveAgentTerminal = bTerminalStartupSent || !ActiveTerminalAgentName.IsEmpty();
	PendingVisibleTerminalCommand = TrimmedCommand;
	PendingVisibleTerminalLabel = TrimmedLabel.IsEmpty() ? FString(TEXT("HyperAIStudio command")) : TrimmedLabel;
	bVisibleTerminalCommandPending = true;
	bTerminalStartupPaused = true;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RecreateTerminalSession();

	LastMessage = FText::FromString(FString::Printf(
		TEXT("%s opened in the embedded terminal after resetting any active agent session. Review the visible output, then return to HyperAIStudio and click Verify."),
		*PendingVisibleTerminalLabel));
	if (bHadActiveAgentTerminal)
	{
		AddTranscriptLine(FString::Printf(TEXT("Stopped active embedded agent terminal: %s"), *StoppedAgentName));
	}
	AddTranscriptLine(FString::Printf(TEXT("Visible terminal command queued: %s"), *PendingVisibleTerminalLabel));
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredVisibleTerminalCommand));
}

void SHyperAIStudioQuickActionWindow::EnqueueVisibleTerminalCommand(const FString& Command, const FString& Label)
{
	const FString TrimmedCommand = Command.TrimStartAndEnd();
	if (TrimmedCommand.IsEmpty())
	{
		return;
	}

	if (const TSharedPtr<SHyperAIStudioQuickActionWindow> ActiveWindow = HyperAIStudio::QuickAction::ActiveQuickActionWindow().Pin())
	{
		ActiveWindow->RunVisibleTerminalCommand(TrimmedCommand, Label);
		return;
	}

	HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands().Add({ TrimmedCommand, Label.TrimStartAndEnd() });
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildTerminalSessionWidget()
{
	TSharedRef<STerminal> NewTerminal = SNew(STerminal)
		.ExternalScrollbar(TerminalScrollBar);
	TerminalWidget = NewTerminal;
	return NewTerminal;
}

FReply SHyperAIStudioQuickActionWindow::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// STerminal translates Shift+Enter to a bare CR, which submits. Preview runs before the terminal sees the key,
	// so send ESC+CR instead: the meta-Return agent TUIs bind to "insert newline". Only while bracketed paste is on
	// (an agent TUI is live); at a shell prompt ESC would wipe the typed line, so plain Enter falls through.
	if (InKeyEvent.GetKey() == EKeys::Enter
		&& InKeyEvent.IsShiftDown() && !InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown()
		&& TerminalWidget.IsValid()
		&& FSlateApplication::Get().GetKeyboardFocusedWidget() == TerminalWidget
		&& HyperAIStudio::TerminalRawInput::IsBracketedPasteEnabled(*TerminalWidget)
		&& HyperAIStudio::TerminalRawInput::WriteRawBytes(*TerminalWidget, HyperAIStudio::TerminalRawInput::BuildNewlineInsertPayload()))
	{
		return FReply::Handled();
	}

	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

bool SHyperAIStudioQuickActionWindow::SubmitComposerText(const FString& Text)
{
	if (!TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		LastMessage = LOCTEXT("ComposerNoSession", "The embedded terminal is not running. Start the agent, then send again.");
		return false;
	}

	const bool bMultiLine = Text.Contains(TEXT("\n"));
	bool bSent = false;
	if (HyperAIStudio::TerminalRawInput::IsAvailable())
	{
		// Without bracketed paste every newline is a real Enter, so a multi-line message would run line by line at a
		// shell prompt. Agent TUIs switch bracketed paste on; its absence means no agent is listening.
		if (bMultiLine && !HyperAIStudio::TerminalRawInput::IsBracketedPasteEnabled(*TerminalWidget))
		{
			LastMessage = LOCTEXT("ComposerNoAgentPrompt", "Nothing sent: no agent prompt is accepting multi-line input. Start the agent, or send a single line to the shell.");
			AddTranscriptLine(LastMessage.ToString());
			return false;
		}
		bSent = HyperAIStudio::TerminalRawInput::WriteText(*TerminalWidget, Text, true);
	}
	else if (!bMultiLine)
	{
		TerminalWidget->ExecuteCommand(Text);
		bSent = true;
	}

	if (!bSent)
	{
		LastMessage = LOCTEXT("ComposerSendFailed", "Nothing sent: multi-line messages need raw terminal input, which is compiled out for this engine version.");
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}

	FString Preview = Text.Left(80);
	int32 NewlineIndex = INDEX_NONE;
	if (Preview.FindChar(TEXT('\n'), NewlineIndex))
	{
		Preview.LeftInline(NewlineIndex);
	}
	AddTranscriptLine(FString::Printf(TEXT("You: %s%s"), *Preview, Preview.Len() < Text.Len() ? TEXT(" ...") : TEXT("")));
	return true;
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildSessionLog()
{
	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("SessionLogTitle", "Session Log"))
		.BodyContent()
		[
			SNew(SBox)
			.MaxDesiredHeight(180.0f)
			[
				// Read-only editable text so past entries can be selected and copied.
				SNew(SMultiLineEditableTextBox)
				.Text(this, &SHyperAIStudioQuickActionWindow::GetTranscriptText)
				.IsReadOnly(true)
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("HeaderTitle", "HyperAI Chat"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.Title")
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(10.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
			.Padding(FMargin(8.0f, 2.0f))
			.ToolTipText_Lambda([this]()
			{
				return FText::Format(LOCTEXT("ReadinessTooltip", "Unreal MCP endpoint: {0}"),
					FText::FromString(Status.Endpoint.IsEmpty() ? FString(TEXT("<not configured>")) : Status.Endpoint));
			})
			[
				SNew(STextBlock)
				.Text(this, &SHyperAIStudioQuickActionWindow::GetReadinessText)
				.ColorAndOpacity(this, &SHyperAIStudioQuickActionWindow::GetReadinessColor)
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBox)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("RefreshTooltip", "Re-check Unreal MCP readiness and installed agents."))
			.OnClicked(this, &SHyperAIStudioQuickActionWindow::OnRefreshClicked)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Refresh")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
			.HasDownArrow(false)
			.ToolTipText(LOCTEXT("ChatSettingsTooltip", "Setup, chat settings and terminal appearance."))
			.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildSettingsMenu)
			.ButtonContent()
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Settings")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildSettingsMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(
		LOCTEXT("ChatOpenSetup", "Open Setup"),
		LOCTEXT("ChatOpenSetupTooltip", "Open the main HyperAIStudio Setup tab."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { OnOpenWorkbenchClicked(); })));
	Menu.AddMenuEntry(
		LOCTEXT("ChatSettingsEntry", "Chat Settings..."),
		LOCTEXT("ChatSettingsEntryTooltip", "Agent models, prompt history persistence and agent options."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UHyperAIStudioSettings>()); })));
	Menu.AddMenuEntry(
		LOCTEXT("TerminalAppearance", "Terminal Appearance..."),
		LOCTEXT("TerminalAppearanceTooltip", "Font, size, colour scheme and scrollback for the embedded terminal. Changes apply the next time the agent starts."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UTerminalSettings>()); })));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildStatusLine()
{
	// When ready the badge already says so; the hint only earns its space while something needs fixing.
	return SNew(SBox)
		.Visibility_Lambda([this]() { return Status.IsReady() ? EVisibility::Collapsed : EVisibility::Visible; })
		.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SHyperAIStudioQuickActionWindow::GetNextActionText)
			.AutoWrapText(true)
			.ColorAndOpacity(this, &SHyperAIStudioQuickActionWindow::GetReadinessColor)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildAgentActions()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			BuildAgentSelector()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			BuildModelSelector()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
			.Padding(FMargin(8.0f, 2.0f))
			.ToolTipText(LOCTEXT("ChatContextManualTooltip", "Context is manual by default. Use context-menu actions when you want selected Unreal context attached."))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ChatContextManual", "Context: Manual"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBox)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyPrompt", "Copy Prompt"))
			.ToolTipText(LOCTEXT("CopyPromptTooltip", "Copy a context prompt for pasting into an agent outside Unreal."))
			.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
			.OnClicked_Lambda([this]() { return OnPreparePromptClicked(GetSelectedAgentName()); })
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text_Lambda([this]()
			{
				return bTerminalStartupPaused ? LOCTEXT("StartAgent", "Start Agent") : LOCTEXT("StopAgent", "Stop Agent");
			})
			.ToolTipText_Lambda([this]()
			{
				return bTerminalStartupPaused
					? LOCTEXT("StartAgentTooltip", "Start the selected agent in a fresh embedded terminal session.")
					: LOCTEXT("StopAgentTooltip", "Stop the current embedded agent terminal session without changing the selected agent.");
			})
			.IsEnabled_Lambda([this]()
			{
				return TerminalWidget.IsValid() && (HasUsableSelectedAgent() || !ActiveTerminalAgentName.IsEmpty());
			})
			.OnClicked(this, &SHyperAIStudioQuickActionWindow::OnToggleTerminalAgentClicked)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildModelSelector()
{
	return SNew(SComboButton)
		.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
		.ToolTipText(LOCTEXT("ModelTooltip", "Model the agent starts with. Changing it restarts a running agent."))
		.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildModelMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &SHyperAIStudioQuickActionWindow::GetModelButtonText)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildModelMenu()
{
	const FString AgentName = GetSelectedAgentName();
	const FHyperAIStudioAgentModelRoute* Route = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(AgentName);

	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, FText::Format(LOCTEXT("ModelMenuHeading", "{0} Model"), FText::FromString(AgentName)));
	auto AddChoice = [this, &Menu](const FString& ModelId, const FText& Label, const FText& ToolTip)
	{
		Menu.AddMenuEntry(
			Label,
			ToolTip,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SHyperAIStudioQuickActionWindow::SelectModel, ModelId),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SHyperAIStudioQuickActionWindow::IsModelSelected, ModelId)),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	};
	AddChoice(FString(), LOCTEXT("ModelDefaultEntry", "Default"), LOCTEXT("ModelDefaultEntryTooltip", "Pass no model flag; the agent uses its own configured model."));
	if (Route)
	{
		for (const FString& Model : Route->Models)
		{
			AddChoice(Model, FText::FromString(Model), FText::GetEmpty());
		}
	}
	Menu.EndSection();

	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("EditModelList", "Edit Model List..."),
		LOCTEXT("EditModelListTooltip", "Add the model ids your account can use, spelled exactly as the agent CLI accepts them."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UHyperAIStudioSettings>()); })));
	return Menu.MakeWidget();
}

void SHyperAIStudioQuickActionWindow::SelectModel(FString ModelId)
{
	const FString AgentName = GetSelectedAgentName();
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	FHyperAIStudioAgentModelRoute* Route = Settings->FindAgentModelRoute(AgentName);
	if (!Route || Route->SelectedModel == ModelId)
	{
		return;
	}

	Route->SelectedModel = ModelId;
	Settings->SaveConfig();

	// The flag only applies at process start: restart a running agent, otherwise it applies on the next start.
	const FText ModelLabel = ModelId.IsEmpty() ? LOCTEXT("ModelDefaultLabel", "its default model") : FText::FromString(ModelId);
	if (!bTerminalStartupPaused)
	{
		LastMessage = FText::Format(LOCTEXT("ModelRestarting", "Restarting {0} with {1}."), FText::FromString(AgentName), ModelLabel);
		QueueTerminalStartup(true, true);
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("ModelSaved", "{0} will start with {1}."), FText::FromString(AgentName), ModelLabel);
	}
	AddTranscriptLine(LastMessage.ToString());
}

bool SHyperAIStudioQuickActionWindow::IsModelSelected(FString ModelId) const
{
	const FHyperAIStudioAgentModelRoute* Route = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(GetSelectedAgentName());
	return Route ? Route->SelectedModel == ModelId : ModelId.IsEmpty();
}

FText SHyperAIStudioQuickActionWindow::GetModelButtonText() const
{
	const FHyperAIStudioAgentModelRoute* Route = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(GetSelectedAgentName());
	return FText::Format(LOCTEXT("ModelButton", "Model: {0}"),
		Route && !Route->SelectedModel.IsEmpty() ? FText::FromString(Route->SelectedModel) : LOCTEXT("ModelDefault", "Default"));
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildAgentSelector()
{
	return SAssignNew(AgentComboBox, SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&AgentOptions)
		.InitiallySelectedItem(SelectedAgent)
		.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
		})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
			{
				if (Item.IsValid())
				{
					SelectedAgent = Item;
					if (SelectInfo == ESelectInfo::Direct)
					{
						return;
					}

					LastMessage = FText::Format(
						LOCTEXT("TerminalAgentSwitching", "Switching embedded terminal to {0}."),
						FText::FromString(*Item));
					AddTranscriptLine(LastMessage.ToString());
					QueueTerminalStartup(true, true);
				}
			})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("Agent: %s"), SelectedAgent.IsValid() ? **SelectedAgent : TEXT("None")));
			})
		];
}

FReply SHyperAIStudioQuickActionWindow::OnRefreshClicked()
{
	LastMessage = HyperAIStudio::QuickAction::RefreshingMessage();
	AddTranscriptLine(TEXT("Refreshing readiness probe..."));
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnPreparePromptClicked(FString AgentName)
{
	if (!HasUsableSelectedAgent())
	{
		LastMessage = LOCTEXT("NoUsableAgentCopy", "No usable agent is selected. Open Agents to install or configure an agent first.");
		AddTranscriptLine(LastMessage.ToString());
		return FReply::Handled();
	}

	FText Message;
	const bool bOk = Service->CreateQuickConnectPromptPackage(AgentName, LastPromptPackage, Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("PromptCopyFailed", "Prompt copy failed: {0}"), Message);
	AddTranscriptLine(LastMessage.ToString());
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnToggleTerminalAgentClicked()
{
	if (bTerminalStartupPaused)
	{
		const FString AgentName = GetSelectedAgentName();
		const FString AgentLabel = AgentName.IsEmpty() ? TEXT("the selected agent") : AgentName;
		LastMessage = FText::Format(
			LOCTEXT("TerminalAgentStarting", "Starting {0} in a fresh embedded terminal."),
			FText::FromString(AgentLabel));
		AddTranscriptLine(LastMessage.ToString());
		QueueTerminalStartup(true, true);
		return FReply::Handled();
	}

	const FString StoppedAgentName = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	bTerminalStartupPaused = true;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RecreateTerminalSession();
	const FString StoppedAgentLabel = StoppedAgentName.IsEmpty() ? TEXT("the embedded agent") : StoppedAgentName;
	LastMessage = FText::Format(
		LOCTEXT("TerminalAgentStopped", "Stopped {0}. Use Start Agent or choose another agent to launch a fresh terminal session."),
		FText::FromString(StoppedAgentLabel));
	AddTranscriptLine(LastMessage.ToString());
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnOpenWorkbenchClicked()
{
	if (OnOpenWorkbench.IsBound())
	{
		OnOpenWorkbench.Execute();
	}
	return FReply::Handled();
}

void SHyperAIStudioQuickActionWindow::RefreshStatus()
{
	if (!Service.IsValid())
	{
		return;
	}

	bRefreshing = true;
	Status = Service->GetStatusSync();
	TWeakPtr<SHyperAIStudioQuickActionWindow> WeakThis = StaticCastSharedRef<SHyperAIStudioQuickActionWindow>(AsShared());
	Service->RefreshStatusAsync([WeakThis](const FHyperAIStudioStatus& NewStatus)
	{
		if (const TSharedPtr<SHyperAIStudioQuickActionWindow> Pinned = WeakThis.Pin())
		{
			const bool bShouldUpdateDefaultMessage = HyperAIStudio::QuickAction::IsDefaultStatusMessage(Pinned->LastMessage);
			const FString PreviousReadiness = Pinned->GetReadinessText().ToString();
			Pinned->Status = NewStatus;
			Pinned->bRefreshing = NewStatus.bProbeInProgress;
			if (bShouldUpdateDefaultMessage)
			{
				Pinned->LastMessage = HyperAIStudio::QuickAction::MessageForStatus(NewStatus);
			}
			Pinned->RefreshAgentOptions();
			const FString NewReadiness = Pinned->GetReadinessText().ToString();
			if (NewReadiness != PreviousReadiness)
			{
				Pinned->AddTranscriptLine(FString::Printf(TEXT("Status: %s"), *NewReadiness));
			}
			Pinned->QueueTerminalStartup(false);
		}
	});
}

void SHyperAIStudioQuickActionWindow::RefreshAgentOptions()
{
	if (!Service.IsValid())
	{
		return;
	}

	const FString PreviousAgent = GetSelectedAgentName();
	if (UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>();
		MutableSettings && MutableSettings->EnsureDefaultAgentModelRoutes())
	{
		MutableSettings->SaveConfig();
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString PreferredAgent = Settings ? Settings->PreferredAgent : FString();
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	CachedPrerequisites = Service->GetPrerequisites();

	AgentOptions.Reset();
	auto AddAgent = [this](const FString& AgentName)
	{
		if (!AgentOptions.ContainsByPredicate([&AgentName](const TSharedPtr<FString>& Existing)
		{
			return Existing.IsValid() && Existing->Equals(AgentName, ESearchCase::IgnoreCase);
		}))
		{
			AgentOptions.Add(MakeShared<FString>(AgentName));
		}
	};

	for (const HyperAIStudio::QuickAction::FAgentRoute& Route : HyperAIStudio::QuickAction::BuiltInAgentRoutes())
	{
		const bool bConfigured = FPaths::FileExists(FPaths::Combine(ProjectRoot, Route.ConfigPath));
		const bool bInstalled = HyperAIStudio::QuickAction::IsPrerequisiteAvailable(CachedPrerequisites, Route.PrerequisiteId);
		const bool bEnabled = !Settings || Settings->IsBuiltInAgentEnabled(Route.Name);
		if (bEnabled && bConfigured && bInstalled)
		{
			AddAgent(Route.Name);
		}
	}

	if (AgentOptions.Num() == 0)
	{
		AgentOptions.Add(MakeShared<FString>(HyperAIStudio::QuickAction::NoUsableAgentsLabel()));
	}

	const FString DesiredAgent = !PreviousAgent.IsEmpty() ? PreviousAgent : PreferredAgent;
	SelectedAgent = AgentOptions[0];
	for (const TSharedPtr<FString>& Agent : AgentOptions)
	{
		if (Agent.IsValid() && Agent->Equals(DesiredAgent, ESearchCase::IgnoreCase))
		{
			SelectedAgent = Agent;
			break;
		}
	}

	if (AgentComboBox.IsValid())
	{
		AgentComboBox->RefreshOptions();
		AgentComboBox->SetSelectedItem(SelectedAgent);
	}
}

void SHyperAIStudioQuickActionWindow::ResetTranscript()
{
	TranscriptLines.Reset();
	AddTranscriptLine(TEXT("HyperAIStudio Chat"));
	AddTranscriptLine(FString::Printf(TEXT("Status: %s"), *GetReadinessText().ToString()));
	AddTranscriptLine(FString::Printf(TEXT("Endpoint: %s"), Status.Endpoint.IsEmpty() ? TEXT("<not configured>") : *Status.Endpoint));

	if (Status.bProbeInProgress || bRefreshing)
	{
		AddTranscriptLine(TEXT("Unreal MCP: checking initialize and tools/list..."));
	}
	else if (Status.bToolsListReachable)
	{
		if (Status.ProbeToolCount > 0 && Status.ProbeToolCount <= 3)
		{
			AddTranscriptLine(TEXT("Unreal MCP: reachable; tools/list returned tool-search meta-tools. HyperAIStudio will not expand them automatically."));
		}
		else if (Status.ProbeToolCount > 0)
		{
			AddTranscriptLine(FString::Printf(TEXT("Unreal MCP: reachable, tools/list returned %d tools."), Status.ProbeToolCount));
		}
		else
		{
			AddTranscriptLine(TEXT("Unreal MCP: reachable."));
		}
	}
	else if (Status.bPortListening)
	{
		AddTranscriptLine(TEXT("Unreal MCP: port is listening, but tools/list is not reachable yet."));
	}
	else
	{
		AddTranscriptLine(TEXT("Unreal MCP: not reachable yet."));
	}

	if (!Status.ProbeMessage.IsEmpty())
	{
		AddTranscriptLine(FString::Printf(TEXT("Probe: %s"), *Status.ProbeMessage));
	}

	AddTranscriptLine(GetSelectedAgentStatusLine());
	AddTranscriptLine(TEXT("Context: Manual unless opened from a Content Browser, level selection, or Blueprint context action."));
	AddTranscriptLine(FString::Printf(TEXT("Last action: %s"), *LastMessage.ToString()));
}

void SHyperAIStudioQuickActionWindow::AddTranscriptLine(const FString& Line)
{
	if (Line.IsEmpty())
	{
		return;
	}

	TranscriptLines.Add(Line);
	constexpr int32 MaxTranscriptLines = 500;
	if (TranscriptLines.Num() > MaxTranscriptLines)
	{
		TranscriptLines.RemoveAt(0, TranscriptLines.Num() - MaxTranscriptLines);
	}

	Invalidate(EInvalidateWidgetReason::Paint);
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredRefresh(double CurrentTime, float DeltaTime)
{
	RefreshStatus();
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup(double CurrentTime, float DeltaTime)
{
	if (bTerminalStartupPaused)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (bTerminalStartupSent)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (!TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return EActiveTimerReturnType::Continue;
	}

	if (!Status.IsReady())
	{
		return EActiveTimerReturnType::Stop;
	}

	const FString AgentName = GetSelectedAgentName();
	if (const FHyperAIStudioAgentModelRoute* ModelRoute = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(AgentName))
	{
		FString ModelArgument;
		FString ModelError;
		if (!UHyperAIStudioSettings::TryResolveModelArgument(*ModelRoute, ModelArgument, ModelError))
		{
			LastMessage = FText::FromString(ModelError + TEXT(" Starting with the agent's default model."));
			AddTranscriptLine(LastMessage.ToString());
		}
	}
	TerminalWidget->ExecuteCommand(BuildTerminalBootstrapCommand(AgentName));
	bTerminalStartupSent = true;
	ActiveTerminalAgentName = AgentName;
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredVisibleTerminalCommand(double CurrentTime, float DeltaTime)
{
	if (!bVisibleTerminalCommandPending)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (!TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return EActiveTimerReturnType::Continue;
	}

	TerminalWidget->ExecuteCommand(PendingVisibleTerminalCommand);
	AddTranscriptLine(FString::Printf(TEXT("Visible terminal command started: %s"), *PendingVisibleTerminalLabel));
	PendingVisibleTerminalCommand.Empty();
	PendingVisibleTerminalLabel.Empty();
	bVisibleTerminalCommandPending = false;
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunQueuedVisibleTerminalCommandPoll(double CurrentTime, float DeltaTime)
{
	if (!bVisibleTerminalCommandPending)
	{
		TArray<HyperAIStudio::QuickAction::FVisibleTerminalCommand>& Queue = HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands();
		if (Queue.Num() > 0)
		{
			const HyperAIStudio::QuickAction::FVisibleTerminalCommand Command = Queue[0];
			Queue.RemoveAt(0);
			RunVisibleTerminalCommand(Command.Command, Command.Label);
		}
	}

	return EActiveTimerReturnType::Continue;
}

void SHyperAIStudioQuickActionWindow::QueueTerminalStartup(bool bForce, bool bResetTerminal)
{
	const FString AgentName = GetSelectedAgentName();
	if (!bForce && bTerminalStartupPaused)
	{
		return;
	}

	if (!bForce && bTerminalStartupSent && ActiveTerminalAgentName.Equals(AgentName, ESearchCase::IgnoreCase))
	{
		return;
	}

	if (bResetTerminal)
	{
		RecreateTerminalSession();
	}

	bTerminalStartupPaused = false;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup));
}

void SHyperAIStudioQuickActionWindow::RecreateTerminalSession()
{
	if (TerminalHost.IsValid())
	{
		TerminalHost->SetContent(BuildTerminalSessionWidget());
	}
	else
	{
		TerminalWidget.Reset();
	}

	if (TerminalScrollBar.IsValid())
	{
		TerminalScrollBar->SetState(0.0f, 1.0f);
	}

	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

FString SHyperAIStudioQuickActionWindow::GetTerminalLaunchCommandForAgent(const FString& AgentName) const
{
	if (const HyperAIStudio::QuickAction::FAgentRoute* Route = HyperAIStudio::QuickAction::FindBuiltInRoute(AgentName))
	{
		FString LaunchCommand = FHyperAIStudioService::GetAgentTerminalLaunchCommand(Route->Name);
		const FHyperAIStudioAgentModelRoute* ModelRoute = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(Route->Name);
		FString ModelArgument;
		FString ModelError;
		if (!LaunchCommand.IsEmpty() && ModelRoute
			&& UHyperAIStudioSettings::TryResolveModelArgument(*ModelRoute, ModelArgument, ModelError)
			&& !ModelArgument.IsEmpty())
		{
			LaunchCommand += TEXT(" ") + ModelArgument;
		}
		return LaunchCommand;
	}

	return FString();
}

FString SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommand(const FString& AgentName) const
{
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const FString LaunchCommand = GetTerminalLaunchCommandForAgent(AgentName);
	return BuildTerminalBootstrapCommandForStatus(Status, AgentName, ProjectRoot, LaunchCommand);
}

FString SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(const FHyperAIStudioStatus& InStatus, const FString& AgentName, const FString& ProjectRoot, const FString& LaunchCommand)
{
	const FString Endpoint = InStatus.Endpoint.IsEmpty() ? TEXT("<not configured>") : InStatus.Endpoint;
	const FString ResolvedProjectRoot = ProjectRoot.IsEmpty() ? FHyperAIStudioService::GetProjectRoot() : ProjectRoot;
	TArray<FString> Commands;
	Commands.Add(TEXT("set TERM=xterm-256color"));
	Commands.Add(FString::Printf(TEXT("cd /d \"%s\""), *ResolvedProjectRoot));
	Commands.Add(TEXT("echo HyperAIStudio Terminal"));
	Commands.Add(FString::Printf(TEXT("echo Status: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(GetReadinessTextForStatus(InStatus, InStatus.bProbeInProgress).ToString())));
	Commands.Add(FString::Printf(TEXT("echo Endpoint: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(Endpoint)));
	if (AgentName.IsEmpty() || LaunchCommand.IsEmpty())
	{
		Commands.Add(TEXT("echo Agent: none usable. Open Setup or Agents, install a CLI agent, then reopen HyperAI Chat."));
	}
	else if (!InStatus.IsReady())
	{
		Commands.Add(FString::Printf(TEXT("echo Agent: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(AgentName)));
		Commands.Add(TEXT("echo Setup is not ready yet. Complete setup before starting the agent CLI."));
	}
	else
	{
		Commands.Add(FString::Printf(TEXT("echo Agent: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(AgentName)));
		Commands.Add(TEXT("echo Starting agent CLI from the Unreal project root..."));
		Commands.Add(FString::Printf(TEXT("( %s ) & echo Agent CLI exited. If no agent prompt is visible, fix the CLI error above or use Copy Prompt."), *LaunchCommand));
	}

	return FString::Printf(TEXT("cmd /d /s /k \"%s\""), *FString::Join(Commands, TEXT(" && ")));
}

FString SHyperAIStudioQuickActionWindow::GetSelectedAgentName() const
{
	return SelectedAgent.IsValid() && *SelectedAgent != HyperAIStudio::QuickAction::NoUsableAgentsLabel()
		? *SelectedAgent
		: FString();
}

bool SHyperAIStudioQuickActionWindow::HasUsableSelectedAgent() const
{
	return !GetSelectedAgentName().IsEmpty();
}

FString SHyperAIStudioQuickActionWindow::GetSelectedAgentStatusLine() const
{
	const FString AgentName = GetSelectedAgentName();
	if (AgentName.IsEmpty())
	{
		return TEXT("Agent: none usable. Open Agents to install or configure Codex, VS Code, Claude Code, Gemini, Cursor, or a custom route.");
	}

	const HyperAIStudio::QuickAction::FAgentRoute* Route = HyperAIStudio::QuickAction::FindBuiltInRoute(AgentName);
	if (!Route)
	{
		return FString::Printf(TEXT("Agent: %s (custom/manual route)."), *AgentName);
	}

	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const bool bConfigured = FPaths::FileExists(FPaths::Combine(ProjectRoot, Route->ConfigPath));
	const bool bInstalled = HyperAIStudio::QuickAction::IsPrerequisiteAvailable(CachedPrerequisites, Route->PrerequisiteId);
	return FString::Printf(TEXT("Agent: %s, app %s, config %s (%s)."),
		*AgentName,
		bInstalled ? TEXT("installed") : TEXT("missing"),
		bConfigured ? TEXT("configured") : TEXT("missing"),
		*Route->ConfigPath);
}

FText SHyperAIStudioQuickActionWindow::GetTranscriptText() const
{
	return FText::FromString(FString::Join(TranscriptLines, LINE_TERMINATOR));
}

FText SHyperAIStudioQuickActionWindow::GetReadinessText() const
{
	return GetReadinessTextForStatus(Status, bRefreshing);
}

FText SHyperAIStudioQuickActionWindow::GetReadinessTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing)
{
	if (InStatus.IsReady())
	{
		return LOCTEXT("ReadyBadge", "Ready");
	}
	if (bIsRefreshing || InStatus.bProbeInProgress)
	{
		return LOCTEXT("CheckingBadge", "Checking");
	}
	return LOCTEXT("SetupBadge", "Setup needed");
}

FSlateColor SHyperAIStudioQuickActionWindow::GetReadinessColor() const
{
	if (Status.IsReady())
	{
		return HyperAIStudio::QuickAction::GoodColor();
	}
	if (bRefreshing || Status.bProbeInProgress)
	{
		return HyperAIStudio::QuickAction::WarningColor();
	}
	return HyperAIStudio::QuickAction::ErrorColor();
}

FText SHyperAIStudioQuickActionWindow::GetNextActionText() const
{
	return GetNextActionTextForStatus(Status, bRefreshing);
}

FText SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing)
{
	if (InStatus.IsReady())
	{
		return LOCTEXT("ReadyNextAction", "Next useful action: message the embedded agent terminal from the box below. Copy Prompt is only for manual handoff fallback.");
	}
	if (bIsRefreshing || InStatus.bProbeInProgress)
	{
		return LOCTEXT("CheckingNextAction", "Next useful action: wait for the MCP readiness probe to finish, then choose a handoff action.");
	}
	if (!InStatus.bUnrealMCPModuleAvailable)
	{
		return LOCTEXT("ModuleNextAction", "Next useful action: enable the required Unreal MCP plugins from the full HyperAIStudio workbench.");
	}
	if (!InStatus.bAgentFilesReady || !InStatus.bUnrealMCPSettingsConfigured)
	{
		return LOCTEXT("SetupNextAction", "Next useful action: start setup to configure Unreal MCP and generate agent files.");
	}
	if (InStatus.ConfiguredAgentCount <= 0)
	{
		return LOCTEXT("AgentConfigNextAction", "Next useful action: enable one installed agent with generated config before using Chat / Terminal.");
	}
	return LOCTEXT("ServerNextAction", "Next useful action: start or refresh Unreal MCP from the full workbench.");
}

#undef LOCTEXT_NAMESPACE
