// Games by Hyper 2026.

#include "SHyperAIStudioPromptComposer.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "HyperAIStudioService.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioPromptComposer"

namespace HyperAIStudio::PromptComposer
{
	struct FExamplePrompt
	{
		FText Label;
		const TCHAR* Prompt;
	};

	TArray<FExamplePrompt> ExamplePrompts()
	{
		return {
			{ LOCTEXT("ExampleLevel", "Inspect current level"), TEXT("Inspect the current level through Unreal MCP. Summarize selected actors, scene structure, and safe next edit options. Do not modify assets yet.") },
			{ LOCTEXT("ExampleSelection", "Use selected assets"), TEXT("Use the selected Content Browser assets as context. Identify what they are, how they are referenced, and one safe improvement path before making changes.") },
			{ LOCTEXT("ExampleBlueprint", "Explain Blueprint nodes"), TEXT("Explain the selected Blueprint nodes using the context pack. Describe execution flow, data flow, side effects, and risky assumptions. Do not modify the Blueprint.") }
		};
	}

	FText RelativeTime(const FDateTime& Utc)
	{
		const FTimespan Age = FDateTime::UtcNow() - Utc;
		if (Age.GetTotalMinutes() < 60.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0}m ago"), FMath::Max(1, FMath::FloorToInt(Age.GetTotalMinutes())));
		}
		if (Age.GetTotalHours() < 24.0)
		{
			return FText::Format(LOCTEXT("HoursAgo", "{0}h ago"), FMath::FloorToInt(Age.GetTotalHours()));
		}
		return FText::Format(LOCTEXT("DaysAgo", "{0}d ago"), FMath::FloorToInt(Age.GetTotalDays()));
	}
}

void SHyperAIStudioPromptComposer::Construct(const FArguments& InArgs)
{
	OnSubmit = InArgs._OnSubmit;
	OnEscape = InArgs._OnEscape;
	OnResumeChat = InArgs._OnResumeChat;
	InputEnabled = InArgs._InputEnabled;

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBox)
			.MaxDesiredHeight(140.0f)
			[
				SAssignNew(InputBox, SMultiLineEditableTextBox)
				.IsEnabled(InputEnabled)
				.Text_Lambda([this]() { return CurrentText; })
				.OnTextChanged_Lambda([this](const FText& NewText) { CurrentText = NewText; })
				.HintText(LOCTEXT("ComposerHint", "Message the agent. Enter sends, Shift+Enter adds a line."))
				.ModiferKeyForNewLine(EModifierKey::Shift) // sic: Epic's spelling.
				.ClearKeyboardFocusOnCommit(false)
				.AutoWrapText(true)
				.OnTextCommitted(this, &SHyperAIStudioPromptComposer::HandleTextCommitted)
				.OnKeyDownHandler(this, &SHyperAIStudioPromptComposer::HandleKeyDown)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Bottom)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SAssignNew(HistoryButton, SComboButton)
			.ToolTipText(LOCTEXT("HistoryTooltip", "Chat history: resume a past agent conversation from this project"))
			.OnGetMenuContent(this, &SHyperAIStudioPromptComposer::BuildHistoryMenu)
			.ButtonContent()
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Recent")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Bottom)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Send", "Send"))
			.ToolTipText(LOCTEXT("SendTooltip", "Send to the agent (Enter)"))
			.IsEnabled_Lambda([this]() { return InputEnabled.Get(true) && !CurrentText.ToString().TrimStartAndEnd().IsEmpty(); })
			.OnClicked_Lambda([this]()
			{
				Submit();
				return FReply::Handled();
			})
		]
	];
}

void SHyperAIStudioPromptComposer::FocusInput() const
{
	if (InputBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(InputBox, EFocusCause::SetDirectly);
	}
}

void SHyperAIStudioPromptComposer::Submit()
{
	FString Text = CurrentText.ToString();
	// Trailing whitespace only: leading indentation can matter in pasted code.
	Text.TrimEndInline();
	if (!InputEnabled.Get(true) || Text.TrimStart().IsEmpty() || !OnSubmit.IsBound() || !OnSubmit.Execute(Text))
	{
		return;
	}
	SetComposerText(FString());
}

void SHyperAIStudioPromptComposer::SetComposerText(const FString& NewText)
{
	CurrentText = FText::FromString(NewText);
	if (InputBox.IsValid())
	{
		InputBox->SetText(CurrentText);
		InputBox->GoTo(ETextLocation::EndOfDocument);
	}
}

void SHyperAIStudioPromptComposer::HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	// Focus loss also commits; only Enter sends.
	if (CommitType == ETextCommit::OnEnter)
	{
		CurrentText = NewText;
		Submit();
	}
}

FReply SHyperAIStudioPromptComposer::HandleKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape && OnEscape.IsBound())
	{
		OnEscape.Execute();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SHyperAIStudioPromptComposer::LoadSessions()
{
	if (bLoadingSessions)
	{
		return;
	}
	bLoadingSessions = true;
	// Session files can be tens of megabytes; read them off the game thread.
	TWeakPtr<SHyperAIStudioPromptComposer> WeakThis = SharedThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Root = FHyperAIStudioService::GetProjectRoot()]()
	{
		TArray<FHyperAIStudioChatSession> Loaded = HyperAIStudio::ChatHistory::ListSessions(Root);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Loaded = MoveTemp(Loaded)]() mutable
		{
			if (const TSharedPtr<SHyperAIStudioPromptComposer> Pinned = WeakThis.Pin())
			{
				Pinned->Sessions = MoveTemp(Loaded);
				Pinned->bLoadingSessions = false;
				Pinned->RebuildFilteredSessions();
			}
		});
	});
}

TSharedRef<SWidget> SHyperAIStudioPromptComposer::BuildHistoryMenu()
{
	SessionFilter.Reset();
	RebuildFilteredSessions();
	LoadSessions();

	TSharedRef<SWrapBox> Examples = SNew(SWrapBox).UseAllottedSize(true);
	for (const HyperAIStudio::PromptComposer::FExamplePrompt& Example : HyperAIStudio::PromptComposer::ExamplePrompts())
	{
		const FString Prompt = Example.Prompt;
		Examples->AddSlot()
		.Padding(0.0f, 0.0f, 6.0f, 4.0f)
		[
			SNew(SButton)
			.Text(Example.Label)
			.ToolTipText(FText::FromString(Prompt))
			.IsEnabled(InputEnabled)
			.OnClicked_Lambda([this, Prompt]()
			{
				SetComposerText(Prompt);
				HistoryButton->SetIsOpen(false);
				FocusInput();
				return FReply::Handled();
			})
		];
	}

	return SNew(SBox)
		.WidthOverride(520.0f)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("HistorySearchHint", "Search agent chats"))
				.OnTextChanged_Lambda([this](const FText& Filter)
				{
					SessionFilter = Filter.ToString();
					RebuildFilteredSessions();
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (bLoadingSessions && Sessions.IsEmpty())
					{
						return LOCTEXT("HistoryLoading", "Reading agent chat history...");
					}
					return SessionFilter.IsEmpty()
						? LOCTEXT("HistoryEmpty", "No Claude Code or Codex chats have run in this project yet.")
						: LOCTEXT("HistoryNoMatch", "No chats match.");
				})
				.Visibility_Lambda([this]() { return FilteredSessions.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.MaxHeight(320.0f)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SAssignNew(SessionList, SListView<FSessionPtr>)
				.ListItemsSource(&FilteredSessions)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SHyperAIStudioPromptComposer::GenerateSessionRow)
				.OnMouseButtonClick_Lambda([this](FSessionPtr Session)
				{
					if (Session.IsValid() && OnResumeChat.IsBound())
					{
						HistoryButton->SetIsOpen(false);
						OnResumeChat.Execute(*Session);
					}
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ExamplesLabel", "Example prompts"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Examples
			]
		];
}

void SHyperAIStudioPromptComposer::RebuildFilteredSessions()
{
	FilteredSessions.Reset();
	for (const FHyperAIStudioChatSession& Session : Sessions)
	{
		if (SessionFilter.IsEmpty() || Session.Title.Contains(SessionFilter) || Session.AgentName.Contains(SessionFilter))
		{
			FilteredSessions.Add(MakeShared<FHyperAIStudioChatSession>(Session));
		}
	}
	if (SessionList.IsValid())
	{
		SessionList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioPromptComposer::GenerateSessionRow(FSessionPtr Session, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FSessionPtr>, OwnerTable)
		.Padding(FMargin(4.0f, 3.0f))
		.ToolTipText(FText::Format(LOCTEXT("SessionRowTooltip", "Resume in {0}\n{1}\nSession {2}"),
			FText::FromString(Session->AgentName), FText::FromString(Session->Title), FText::FromString(Session->SessionId)))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Session->Title))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("SessionRowMeta", "{0}  {1}"),
					FText::FromString(Session->AgentName), HyperAIStudio::PromptComposer::RelativeTime(Session->LastActiveUtc)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
