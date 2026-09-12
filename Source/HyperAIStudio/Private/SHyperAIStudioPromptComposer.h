// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SComboButton;
class SMultiLineEditableTextBox;

/** Return true once the text reached the agent. The composer clears only on true. */
DECLARE_DELEGATE_RetVal_OneParam(bool, FHyperAIStudioOnPromptSubmit, const FString& /*Text*/);
DECLARE_DELEGATE_OneParam(FHyperAIStudioOnResumeChat, const FHyperAIStudioChatSession& /*Session*/);

/**
 * Multi-line prompt box for the chat panel. Enter sends, Shift+Enter inserts a newline, Escape hands focus back to
 * the terminal. The history button lists this project's past agent chats and resumes the one picked.
 */
class SHyperAIStudioPromptComposer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPromptComposer) {}
		SLATE_EVENT(FHyperAIStudioOnPromptSubmit, OnSubmit)
		SLATE_EVENT(FSimpleDelegate, OnEscape)
		SLATE_EVENT(FHyperAIStudioOnResumeChat, OnResumeChat)
		/** Typing needs a running agent; chat history does not. */
		SLATE_ATTRIBUTE(bool, InputEnabled)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void FocusInput() const;

private:
	using FSessionPtr = TSharedPtr<FHyperAIStudioChatSession>;

	void Submit();
	void SetComposerText(const FString& NewText);
	void HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	FReply HandleKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);

	TSharedRef<SWidget> BuildHistoryMenu();
	void LoadSessions();
	void RebuildFilteredSessions();
	TSharedRef<ITableRow> GenerateSessionRow(FSessionPtr Session, const TSharedRef<STableViewBase>& OwnerTable);

	FHyperAIStudioOnPromptSubmit OnSubmit;
	FSimpleDelegate OnEscape;
	FHyperAIStudioOnResumeChat OnResumeChat;
	TAttribute<bool> InputEnabled;

	TSharedPtr<SMultiLineEditableTextBox> InputBox;
	TSharedPtr<SComboButton> HistoryButton;
	TSharedPtr<SListView<FSessionPtr>> SessionList;
	TArray<FHyperAIStudioChatSession> Sessions;
	TArray<FSessionPtr> FilteredSessions;
	FText CurrentText;
	FString SessionFilter;
	bool bLoadingSessions = false;
};
