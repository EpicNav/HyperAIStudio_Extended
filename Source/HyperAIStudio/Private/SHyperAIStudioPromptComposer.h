// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SComboButton;
class SMultiLineEditableTextBox;
struct FHyperAIStudioPromptHistoryEntry;

/** Return true once the text reached the agent. The composer clears and records history only on true. */
DECLARE_DELEGATE_RetVal_OneParam(bool, FHyperAIStudioOnPromptSubmit, const FString& /*Text*/);

/**
 * Multi-line prompt box for the chat panel. Enter sends, Shift+Enter inserts a newline, Up/Down on an empty or
 * unedited box walks prompt history, Escape hands focus back to the terminal.
 */
class SHyperAIStudioPromptComposer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPromptComposer) {}
		SLATE_EVENT(FHyperAIStudioOnPromptSubmit, OnSubmit)
		SLATE_EVENT(FSimpleDelegate, OnEscape)
		/** Recorded with each history entry. */
		SLATE_ATTRIBUTE(FString, AgentName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void FocusInput() const;

private:
	using FEntryPtr = TSharedPtr<FHyperAIStudioPromptHistoryEntry>;

	void Submit();
	void SetComposerText(const FString& NewText);
	void HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	FReply HandleKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	bool RecallHistory(int32 Direction);

	TSharedRef<SWidget> BuildHistoryMenu();
	void RebuildFilteredHistory();
	TSharedRef<ITableRow> GenerateHistoryRow(FEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable);
	void HandleHistoryPicked(FEntryPtr Entry, ESelectInfo::Type SelectInfo);

	FHyperAIStudioOnPromptSubmit OnSubmit;
	FSimpleDelegate OnEscape;
	TAttribute<FString> AgentName;

	TSharedPtr<SMultiLineEditableTextBox> InputBox;
	TSharedPtr<SComboButton> HistoryButton;
	TSharedPtr<SListView<FEntryPtr>> HistoryList;
	TArray<FEntryPtr> FilteredHistory;
	FText CurrentText;
	FString HistoryFilter;

	/** Index into history of the entry currently shown by Up/Down recall, or INDEX_NONE. */
	int32 RecallIndex = INDEX_NONE;
};
