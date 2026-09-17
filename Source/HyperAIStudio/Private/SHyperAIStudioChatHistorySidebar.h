// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

DECLARE_DELEGATE_OneParam(FHyperAIStudioOnResumeChat, const FHyperAIStudioChatSession& /*Session*/);

/** This project's past agent chats, newest first. Clicking one resumes it. */
class SHyperAIStudioChatHistorySidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioChatHistorySidebar) {}
		SLATE_EVENT(FHyperAIStudioOnResumeChat, OnResumeChat)
		SLATE_EVENT(FSimpleDelegate, OnClose)
		/** Start a fresh conversation with the selected agent. */
		SLATE_EVENT(FSimpleDelegate, OnNewChat)
		/** Session id of the chat running in the terminal, highlighted in the list. */
		SLATE_ATTRIBUTE(FString, ActiveSessionId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** Re-read the agents' session stores off the game thread. */
	void Refresh();

private:
	/** A date heading when Session is null, otherwise a chat. */
	struct FRow
	{
		FText Heading;
		TSharedPtr<FHyperAIStudioChatSession> Session;
	};
	using FRowPtr = TSharedPtr<FRow>;

	void RebuildFiltered();
	TSharedRef<ITableRow> GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);

	FHyperAIStudioOnResumeChat OnResumeChat;
	FSimpleDelegate OnClose;
	FSimpleDelegate OnNewChat;
	/** Session whose title is being edited inline, if any. */
	FString RenamingSessionId;
	TAttribute<FString> ActiveSessionId;

	TSharedPtr<SListView<FRowPtr>> List;
	TArray<FHyperAIStudioChatSession> Sessions;
	TArray<FRowPtr> Filtered;
	FString Filter;
	bool bLoading = false;
};
