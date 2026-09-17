// Games by Hyper 2026.

#include "SHyperAIStudioQuickActionWindow.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioTerminalRawInput.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "IContentBrowserSingleton.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "HyperAIStudioApprovalGate.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioChatHistorySidebar.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

	constexpr float ChatSidebarWidth = 280.0f;

	/**
	 * The terminal paints its colour scheme's background; the frame around it takes the same colour so the padding
	 * blends in. The Terminal plugin keeps its parsed schemes private, so read the selected scheme's JSON directly.
	 */
	FLinearColor TerminalBackgroundColor()
	{
		static FString CachedSchemeName;
		static FColor CachedBackground(0x1E, 0x1E, 0x1E);
		const UTerminalSettings* Settings = GetDefault<UTerminalSettings>();
		const FString SchemeName = Settings ? Settings->ColorSchemeName : FString(TEXT("Default"));
		if (SchemeName != CachedSchemeName)
		{
			CachedSchemeName = SchemeName;
			CachedBackground = FColor(0x1E, 0x1E, 0x1E);
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Terminal"));
			const FString Directory = Plugin ? Plugin->GetBaseDir() / TEXT("Config") / TEXT("ColorSchemes") : FString();
			TArray<FString> Files;
			if (!Directory.IsEmpty() && SchemeName != TEXT("Default"))
			{
				IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.json")), true, false);
			}
			for (const FString& File : Files)
			{
				FString Json;
				TSharedPtr<FJsonObject> Scheme;
				FString Name;
				FString Hex;
				if (FFileHelper::LoadFileToString(Json, *(Directory / File))
					&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Scheme) && Scheme.IsValid()
					&& Scheme->TryGetStringField(TEXT("name"), Name) && Name == SchemeName
					&& Scheme->TryGetStringField(TEXT("defaultBackground"), Hex))
				{
					CachedBackground = FColor::FromHex(Hex);
					break;
				}
			}
		}
		return FLinearColor(CachedBackground);
	}

	FText InitialReadyMessage()
	{
		return LOCTEXT("InitialReadyMessage", "Type to the agent in the terminal: Enter sends, Shift+Enter adds a line. The Chats button resumes a past conversation. Copy Prompt is only for manual handoff/context fallback.");
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
		.AlwaysShowScrollbar(false)
		.Thickness(FVector2D(5.0f, 5.0f));
	ChatSidebarCurve = FCurveSequence(0.0f, 0.2f, ECurveEaseFunction::CubicOut);
	ActivitySidebarCurve = FCurveSequence(0.0f, 0.2f, ECurveEaseFunction::CubicOut);

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
			.FillHeight(1.0f)
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					// Width and opacity follow the curve; the clip hides content while it slides in or out.
					SNew(SBox)
					.WidthOverride_Lambda([this]() { return HyperAIStudio::QuickAction::ChatSidebarWidth * ChatSidebarCurve.GetLerp(); })
					.Visibility_Lambda([this]() { return ChatSidebarCurve.GetLerp() > 0.0f ? EVisibility::Visible : EVisibility::Collapsed; })
					.Clipping(EWidgetClipping::ClipToBounds)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush(TEXT("NoBorder")))
						.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
						.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.0f, 1.0f, 1.0f, ChatSidebarCurve.GetLerp()); })
						[
							SAssignNew(ChatSidebar, SHyperAIStudioChatHistorySidebar)
							.OnResumeChat(this, &SHyperAIStudioQuickActionWindow::ResumeChat)
							.OnClose(this, &SHyperAIStudioQuickActionWindow::ToggleChatSidebar)
							.OnNewChat_Lambda([this]()
							{
								ResumeAgentName.Reset();
								ResumeSessionId.Reset();
								ActiveChatSessionId.Reset();
								LastMessage = LOCTEXT("NewChatStarted", "Starting a new chat.");
								AddTranscriptLine(LastMessage.ToString());
								QueueTerminalStartup(true, true);
							})
							.ActiveSessionId_Lambda([this]() { return ActiveChatSessionId; })
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildStatusLine()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildAgentActions()
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.TerminalFrame"))
						.BorderBackgroundColor_Lambda([]() { return FSlateColor(HyperAIStudio::QuickAction::TerminalBackgroundColor()); })
						.Padding(FMargin(12.0f, 10.0f, 4.0f, 10.0f))
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
							.Padding(4.0f, 0.0f, 0.0f, 0.0f)
							[
								TerminalScrollBar.ToSharedRef()
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						BuildSessionLog()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return LastMessage; })
						.AutoWrapText(true)
						.ColorAndOpacity(HyperAIStudio::QuickAction::MutedColor())
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride_Lambda([this]() { return HyperAIStudio::QuickAction::ChatSidebarWidth * ActivitySidebarCurve.GetLerp(); })
					.Visibility_Lambda([this]() { return ActivitySidebarCurve.GetLerp() > 0.0f ? EVisibility::Visible : EVisibility::Collapsed; })
					.Clipping(EWidgetClipping::ClipToBounds)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush(TEXT("NoBorder")))
						.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
						.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.0f, 1.0f, 1.0f, ActivitySidebarCurve.GetLerp()); })
						[
							SAssignNew(ActivitySidebar, SHyperAIStudioChatActivitySidebar)
							.OnClose(this, &SHyperAIStudioQuickActionWindow::ToggleActivitySidebar)
							.OnApprovalResolved_Lambda([this]()
							{
								Invalidate(EInvalidateWidgetReason::Paint);
							})
						]
					]
				]
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
	// The MCP pill is only honest if it is re-probed; the probe is async and idles when nothing changed.
	RegisterActiveTimer(15.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunStatusHeartbeat));
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

void SHyperAIStudioQuickActionWindow::ResumeChat(const FHyperAIStudioChatSession& Session)
{
	if (HyperAIStudio::ChatHistory::BuildResumeArguments(Session.AgentName, Session.SessionId).IsEmpty())
	{
		return;
	}
	RefreshAgentOptions();
	const bool bAgentUsable = AgentOptions.ContainsByPredicate([&Session](const TSharedPtr<FString>& Agent)
	{
		return Agent.IsValid() && Agent->Equals(Session.AgentName, ESearchCase::IgnoreCase);
	});
	if (!bAgentUsable)
	{
		LastMessage = FText::Format(LOCTEXT("ResumeAgentUnavailable", "{0} is not set up for HyperAI Chat, so that chat cannot be resumed here."),
			FText::FromString(Session.AgentName));
		AddTranscriptLine(LastMessage.ToString());
		return;
	}

	ResumeAgentName = Session.AgentName;
	ResumeSessionId = Session.SessionId;
	LastMessage = FText::Format(LOCTEXT("ResumingChat", "Resuming {0} chat: {1}"), FText::FromString(Session.AgentName), FText::FromString(Session.Title));
	AddTranscriptLine(LastMessage.ToString());
	if (GetSelectedAgentName().Equals(Session.AgentName, ESearchCase::IgnoreCase))
	{
		QueueTerminalStartup(true, true);
	}
	else
	{
		SelectAgentByName(Session.AgentName);
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

void SHyperAIStudioQuickActionWindow::ToggleActivitySidebar()
{
	bActivitySidebarOpen = !bActivitySidebarOpen;
	if (ActivitySidebarCurve.IsPlaying())
	{
		ActivitySidebarCurve.Reverse();
	}
	else if (bActivitySidebarOpen)
	{
		ActivitySidebarCurve.Play(AsShared());
	}
	else
	{
		ActivitySidebarCurve.PlayReverse(AsShared());
	}
}

void SHyperAIStudioQuickActionWindow::ToggleChatSidebar()
{
	bChatSidebarOpen = !bChatSidebarOpen;
	if (bChatSidebarOpen && ChatSidebar.IsValid())
	{
		ChatSidebar->Refresh();
	}
	if (ChatSidebarCurve.IsPlaying())
	{
		ChatSidebarCurve.Reverse();
	}
	else if (bChatSidebarOpen)
	{
		ChatSidebarCurve.Play(AsShared());
	}
	else
	{
		ChatSidebarCurve.PlayReverse(AsShared());
	}
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("ChatsToggleTooltip", "Show or hide this project's past agent chats."))
			.OnClicked_Lambda([this]()
			{
				ToggleChatSidebar();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Recent")))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ChatsToggle", "Chats"))
				]
			]
		]
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
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildMcpStatusSelector()
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
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("ActivityToggleTooltip", "Plans waiting for your approval, work in progress, and what the agent changed."))
			.OnClicked_Lambda([this]()
			{
				ToggleActivitySidebar();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Visibility")))
					.ColorAndOpacity_Lambda([]()
					{
						// Amber while something is waiting on the user.
						return SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount() > 0
							? FSlateColor(FStyleColors::AccentYellow) : FSlateColor::UseForeground();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						const int32 Pending = SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount();
						return Pending > 0
							? FText::Format(LOCTEXT("ActivityTogglePending", "Activity ({0})"), Pending)
							: LOCTEXT("ActivityToggle", "Activity");
					})
				]
			]
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
		LOCTEXT("ChatSettingsEntryTooltip", "Agent models and agent options."),
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
			BuildContextSelector()
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

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildContextSelector()
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("ContextTooltip", "Put what you have selected in Unreal into the agent's prompt. You can also drag assets or actors onto this panel."))
		.IsEnabled_Lambda([this]() { return TerminalWidget.IsValid() && TerminalWidget->IsSessionRunning(); })
		.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildContextMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ContextButton", "Context"))
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildContextMenu()
{
	TArray<FAssetData> SelectedAssets;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(SelectedAssets);
	TArray<FString> ActorPaths;
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (const AActor* Actor = Cast<AActor>(*It))
			{
				ActorPaths.Add(Actor->GetPathName());
			}
		}
	}

	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("ContextMenuHeading", "Insert into the prompt"));
	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("InsertAssets", "Selected assets ({0})"), SelectedAssets.Num()),
		LOCTEXT("InsertAssetsTooltip", "Paste the object path of each selected Content Browser asset."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Package"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this, SelectedAssets]()
			{
				TArray<FString> Paths;
				for (const FAssetData& Asset : SelectedAssets)
				{
					Paths.Add(Asset.GetSoftObjectPath().ToString());
				}
				InsertIntoAgentPrompt(FString::Join(Paths, TEXT("\n")), TEXT("selected assets"));
			}),
			FCanExecuteAction::CreateLambda([SelectedAssets]() { return SelectedAssets.Num() > 0; })));
	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("InsertActors", "Selected actors ({0})"), ActorPaths.Num()),
		LOCTEXT("InsertActorsTooltip", "Paste the path of each selected level actor."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Level"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this, ActorPaths]()
			{
				InsertIntoAgentPrompt(FString::Join(ActorPaths, TEXT("\n")), TEXT("selected actors"));
			}),
			FCanExecuteAction::CreateLambda([ActorPaths]() { return ActorPaths.Num() > 0; })));
	Menu.AddMenuEntry(
		LOCTEXT("InsertSummary", "Context summary"),
		LOCTEXT("InsertSummaryTooltip", "Paste a sentence describing the current selection and map."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Info"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
		{
			if (Service.IsValid())
			{
				InsertIntoAgentPrompt(Service->GetCurrentContextSummary(), TEXT("context summary"));
			}
		})));
	Menu.EndSection();
	Menu.AddMenuSeparator();
	Menu.AddWidget(
		SNew(SBox)
		.WidthOverride(240.0f)
		.Padding(FMargin(12.0f, 4.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ContextDropHint", "Dragging assets or actors onto this panel does the same thing."))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		],
		FText::GetEmpty());
	return Menu.MakeWidget();
}

bool SHyperAIStudioQuickActionWindow::InsertIntoAgentPrompt(const FString& Text, const FString& Label)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return false;
	}
	// Bracketed paste is on only while an agent TUI owns the prompt; at a shell prompt this would run as commands.
	if (!HyperAIStudio::TerminalRawInput::IsAvailable()
		|| !HyperAIStudio::TerminalRawInput::IsBracketedPasteEnabled(*TerminalWidget))
	{
		LastMessage = LOCTEXT("ContextNoAgentPrompt", "Nothing inserted: no agent prompt is accepting input. Start the agent first.");
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}
	// Never sends: the text lands in the prompt for the user to finish writing around.
	if (!HyperAIStudio::TerminalRawInput::WriteText(*TerminalWidget, Trimmed, /*bAppendCarriageReturn=*/false))
	{
		LastMessage = LOCTEXT("ContextInsertFailed", "Nothing inserted: the terminal refused the paste.");
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}
	LastMessage = FText::Format(LOCTEXT("ContextInserted", "Inserted {0} into the prompt."), FText::FromString(Label));
	AddTranscriptLine(LastMessage.ToString());
	FSlateApplication::Get().SetKeyboardFocus(TerminalWidget, EFocusCause::SetDirectly);
	return true;
}

void SHyperAIStudioQuickActionWindow::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	SCompoundWidget::OnDragEnter(MyGeometry, DragDropEvent);
}

FReply SHyperAIStudioQuickActionWindow::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const bool bUnderstood = DragDropEvent.GetOperationAs<FAssetDragDropOp>().IsValid()
		|| DragDropEvent.GetOperationAs<FActorDragDropOp>().IsValid();
	return bUnderstood ? FReply::Handled() : FReply::Unhandled();
}

FReply SHyperAIStudioQuickActionWindow::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	TArray<FString> Paths;
	FString Label;
	if (const TSharedPtr<FAssetDragDropOp> AssetDrop = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		for (const FAssetData& Asset : AssetDrop->GetAssets())
		{
			Paths.Add(Asset.GetSoftObjectPath().ToString());
		}
		Label = TEXT("dropped assets");
	}
	else if (const TSharedPtr<FActorDragDropOp> ActorDrop = DragDropEvent.GetOperationAs<FActorDragDropOp>())
	{
		for (const TWeakObjectPtr<AActor>& Actor : ActorDrop->Actors)
		{
			if (Actor.IsValid())
			{
				Paths.Add(Actor->GetPathName());
			}
		}
		Label = TEXT("dropped actors");
	}
	if (Paths.IsEmpty())
	{
		return FReply::Unhandled();
	}
	InsertIntoAgentPrompt(FString::Join(Paths, TEXT("\n")), Label);
	return FReply::Handled();
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildMcpStatusSelector()
{
	return SNew(SComboButton)
		.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
		.HasDownArrow(false)
		.ToolTipText_Lambda([this]()
		{
			return FText::Format(
				LOCTEXT("McpStatusTooltip", "Unreal MCP at {0}\nServer running: {1}\nPort listening: {2}\nTool list reachable: {3}\nRegistered tools: {4}"),
				FText::FromString(Status.Endpoint.IsEmpty() ? FString(TEXT("<not configured>")) : Status.Endpoint),
				Status.bServerRunning ? LOCTEXT("Yes", "yes") : LOCTEXT("No", "no"),
				Status.bPortListening ? LOCTEXT("Yes", "yes") : LOCTEXT("No", "no"),
				Status.bToolsListReachable ? LOCTEXT("Yes", "yes") : LOCTEXT("No", "no"),
				Status.RegisteredToolCount);
		})
		.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildMcpStatusMenu)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.BulletPoint")))
				.ColorAndOpacity(this, &SHyperAIStudioQuickActionWindow::GetMcpStatusColor)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(3.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SHyperAIStudioQuickActionWindow::GetMcpStatusText)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildMcpStatusMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("McpMenuHeading", "Unreal MCP"));
	Menu.AddMenuEntry(
		LOCTEXT("McpReconnect", "Reconnect"),
		LOCTEXT("McpReconnectTooltip", "Restart the Unreal MCP server, then re-probe it. Running agents reconnect on their next call."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
		{
			if (!Service.IsValid())
			{
				return;
			}
			FText Message;
			const bool bRestarted = Service->RestartUnrealMCP(Message);
			LastMessage = Message.IsEmpty()
				? (bRestarted ? LOCTEXT("McpRestarted", "Unreal MCP restarted.") : LOCTEXT("McpRestartFailed", "Unreal MCP did not restart."))
				: Message;
			AddTranscriptLine(LastMessage.ToString());
			RefreshStatus();
		})));
	Menu.AddMenuEntry(
		LOCTEXT("McpReprobe", "Re-probe"),
		LOCTEXT("McpReprobeTooltip", "Check the endpoint again without restarting anything."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { RefreshStatus(); })));
	Menu.AddMenuEntry(
		LOCTEXT("McpCopyEndpoint", "Copy endpoint"),
		LOCTEXT("McpCopyEndpointTooltip", "Copy the MCP endpoint URL to the clipboard."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Duplicate"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this]() { FPlatformApplicationMisc::ClipboardCopy(*Status.Endpoint); }),
			FCanExecuteAction::CreateLambda([this]() { return !Status.Endpoint.IsEmpty(); })));
	Menu.EndSection();
	return Menu.MakeWidget();
}

FText SHyperAIStudioQuickActionWindow::GetMcpStatusText() const
{
	if (!Status.bUnrealMCPModuleAvailable)
	{
		return LOCTEXT("McpMissing", "MCP: unavailable");
	}
	if (!Status.bServerRunning)
	{
		return LOCTEXT("McpStopped", "MCP: stopped");
	}
	if (!Status.bPortListening)
	{
		return FText::Format(LOCTEXT("McpNotListening", "MCP: port {0} closed"), FText::AsNumber(Status.ActivePort, &FNumberFormattingOptions::DefaultNoGrouping()));
	}
	if (!Status.bToolsListReachable)
	{
		return LOCTEXT("McpUnreachable", "MCP: no tool list");
	}
	return FText::Format(LOCTEXT("McpReady", "MCP: {0} tools"), Status.RegisteredToolCount);
}

FSlateColor SHyperAIStudioQuickActionWindow::GetMcpStatusColor() const
{
	if (!Status.bServerRunning || !Status.bUnrealMCPModuleAvailable)
	{
		return FSlateColor(FStyleColors::AccentRed);
	}
	if (!Status.bPortListening || !Status.bToolsListReachable)
	{
		return FSlateColor(FStyleColors::AccentYellow);
	}
	return FSlateColor(FStyleColors::AccentGreen);
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
	AddTranscriptLine(TEXT("Context: manual. Use the Context button, or drag assets and actors onto this panel, to put paths in the prompt."));
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

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunStatusHeartbeat(double CurrentTime, float DeltaTime)
{
	// Only re-probe when the previous one finished, so a slow endpoint cannot queue probes on top of each other.
	if (!bRefreshing && !Status.bProbeInProgress)
	{
		RefreshStatus();
	}
	return EActiveTimerReturnType::Continue;
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
	ActiveChatSessionId = ResumeAgentName.Equals(AgentName, ESearchCase::IgnoreCase) ? ResumeSessionId : FString();
	ResumeAgentName.Reset();
	ResumeSessionId.Reset();
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
		const FString ResumeArguments = ResumeAgentName.Equals(Route->Name, ESearchCase::IgnoreCase)
			? HyperAIStudio::ChatHistory::BuildResumeArguments(Route->Name, ResumeSessionId)
			: FString();
		if (!LaunchCommand.IsEmpty() && !ResumeArguments.IsEmpty())
		{
			LaunchCommand += TEXT(" ") + ResumeArguments;
		}
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
		return LOCTEXT("ReadyNextAction", "Next useful action: type to the embedded agent terminal. Copy Prompt is only for manual handoff fallback.");
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
