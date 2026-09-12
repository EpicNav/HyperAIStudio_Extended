// Games by Hyper 2026.

#include "SHyperAIStudioPromptComposer.h"

#include "Framework/Application/SlateApplication.h"
#include "HyperAIStudioPromptHistory.h"
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
}

void SHyperAIStudioPromptComposer::Construct(const FArguments& InArgs)
{
	OnSubmit = InArgs._OnSubmit;
	OnEscape = InArgs._OnEscape;
	AgentName = InArgs._AgentName;

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
				.Text_Lambda([this]() { return CurrentText; })
				.OnTextChanged_Lambda([this](const FText& NewText) { CurrentText = NewText; })
				.HintText(LOCTEXT("ComposerHint", "Message the agent. Enter sends, Shift+Enter adds a line, Up recalls history."))
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
			.ToolTipText(LOCTEXT("HistoryTooltip", "Previous prompts and examples"))
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
			.IsEnabled_Lambda([this]() { return !CurrentText.ToString().TrimStartAndEnd().IsEmpty(); })
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
	if (Text.TrimStart().IsEmpty() || !OnSubmit.IsBound() || !OnSubmit.Execute(Text))
	{
		return;
	}

	UHyperAIStudioPromptHistory::Record(Text, AgentName.Get(FString()));
	RecallIndex = INDEX_NONE;
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
	const FKey Key = KeyEvent.GetKey();
	const bool bNoModifiers = !KeyEvent.IsShiftDown() && !KeyEvent.IsControlDown() && !KeyEvent.IsAltDown() && !KeyEvent.IsCommandDown();
	if (bNoModifiers && (Key == EKeys::Up || Key == EKeys::Down))
	{
		return RecallHistory(Key == EKeys::Up ? 1 : -1) ? FReply::Handled() : FReply::Unhandled();
	}

	if (Key == EKeys::Escape && OnEscape.IsBound())
	{
		OnEscape.Execute();
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

bool SHyperAIStudioPromptComposer::RecallHistory(int32 Direction)
{
	const TArray<FHyperAIStudioPromptHistoryEntry>& Entries = GetDefault<UHyperAIStudioPromptHistory>()->Entries;
	const FString Current = CurrentText.ToString();
	const bool bShowingRecalled = Entries.IsValidIndex(RecallIndex) && Entries[RecallIndex].Text == Current;

	// Once the user edits the text, arrows go back to moving the cursor.
	if (!Current.IsEmpty() && !bShowingRecalled)
	{
		return false;
	}

	const int32 Next = (bShowingRecalled ? RecallIndex : INDEX_NONE) + Direction;
	if (Next < 0)
	{
		RecallIndex = INDEX_NONE;
		SetComposerText(FString());
		return true;
	}
	if (!Entries.IsValidIndex(Next))
	{
		return bShowingRecalled;
	}

	RecallIndex = Next;
	SetComposerText(Entries[Next].Text);
	return true;
}

TSharedRef<SWidget> SHyperAIStudioPromptComposer::BuildHistoryMenu()
{
	HistoryFilter.Reset();
	RebuildFilteredHistory();

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
			.OnClicked_Lambda([this, Prompt]()
			{
				RecallIndex = INDEX_NONE;
				SetComposerText(Prompt);
				HistoryButton->SetIsOpen(false);
				FocusInput();
				return FReply::Handled();
			})
		];
	}

	return SNew(SBox)
		.WidthOverride(460.0f)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("HistorySearchHint", "Search previous prompts"))
				.OnTextChanged_Lambda([this](const FText& Filter)
				{
					HistoryFilter = Filter.ToString();
					RebuildFilteredHistory();
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return HistoryFilter.IsEmpty()
						? LOCTEXT("HistoryEmpty", "No prompts sent yet.")
						: LOCTEXT("HistoryNoMatch", "No prompts match.");
				})
				.Visibility_Lambda([this]() { return FilteredHistory.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.MaxHeight(260.0f)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SAssignNew(HistoryList, SListView<FEntryPtr>)
				.ListItemsSource(&FilteredHistory)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SHyperAIStudioPromptComposer::GenerateHistoryRow)
				.OnMouseButtonClick_Lambda([this](FEntryPtr Entry) { HandleHistoryPicked(Entry, ESelectInfo::OnMouseClick); })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ExamplesLabel", "Examples"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Examples
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearHistory", "Clear History"))
				.ToolTipText(LOCTEXT("ClearHistoryTooltip", "Forget every previous prompt, including any saved to disk."))
				.IsEnabled_Lambda([]() { return !GetDefault<UHyperAIStudioPromptHistory>()->Entries.IsEmpty(); })
				.OnClicked_Lambda([this]()
				{
					UHyperAIStudioPromptHistory::Clear();
					RecallIndex = INDEX_NONE;
					RebuildFilteredHistory();
					return FReply::Handled();
				})
			]
		];
}

void SHyperAIStudioPromptComposer::RebuildFilteredHistory()
{
	FilteredHistory.Reset();
	for (const FHyperAIStudioPromptHistoryEntry& Entry : GetDefault<UHyperAIStudioPromptHistory>()->Entries)
	{
		if (HistoryFilter.IsEmpty() || Entry.Text.Contains(HistoryFilter))
		{
			FilteredHistory.Add(MakeShared<FHyperAIStudioPromptHistoryEntry>(Entry));
		}
	}

	if (HistoryList.IsValid())
	{
		HistoryList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioPromptComposer::GenerateHistoryRow(FEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
	TArray<FString> Lines;
	Entry->Text.ParseIntoArrayLines(Lines, false);
	FString Label = Lines.Num() > 0 ? Lines[0] : Entry->Text;
	if (Lines.Num() > 1)
	{
		Label += FString::Printf(TEXT("  (+%d lines)"), Lines.Num() - 1);
	}

	return SNew(STableRow<FEntryPtr>, OwnerTable)
		.Padding(FMargin(4.0f, 3.0f))
		.ToolTipText(FText::FromString(Entry->Text))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Entry->AgentName))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SHyperAIStudioPromptComposer::HandleHistoryPicked(FEntryPtr Entry, ESelectInfo::Type SelectInfo)
{
	if (!Entry.IsValid())
	{
		return;
	}

	// Load for review; never auto-send a stale prompt to a live agent.
	RecallIndex = INDEX_NONE;
	SetComposerText(Entry->Text);
	HistoryButton->SetIsOpen(false);
	FocusInput();
}

#undef LOCTEXT_NAMESPACE
