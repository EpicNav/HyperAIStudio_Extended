// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "HyperAIStudioService.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class SHyperAIStudioQuickActionWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioQuickActionWindow) {}
		SLATE_EVENT(FSimpleDelegate, OnOpenWorkbench)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	bool IsActiveOrSelectedAgent(const FString& AgentName) const;
	void SelectAgentByName(const FString& AgentName);
	/** Restart the terminal on that chat's agent, resuming the conversation. */
	void ResumeChat(const struct FHyperAIStudioChatSession& Session);
	void RunVisibleTerminalCommand(const FString& Command, const FString& Label);
	static void EnqueueVisibleTerminalCommand(const FString& Command, const FString& Label);
	static FText GetReadinessTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing);
	static FText GetNextActionTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing);
	static FString BuildTerminalBootstrapCommandForStatus(const FHyperAIStudioStatus& InStatus, const FString& AgentName, const FString& ProjectRoot, const FString& LaunchCommand);

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildStatusLine();
	TSharedRef<SWidget> BuildSettingsMenu();
	TSharedRef<SWidget> BuildAgentActions();
	TSharedRef<SWidget> BuildAgentSelector();
	TSharedRef<SWidget> BuildModelSelector();
	TSharedRef<SWidget> BuildModelMenu();
	void SelectModel(FString ModelId);
	bool IsModelSelected(FString ModelId) const;
	FText GetModelButtonText() const;
	TSharedRef<SWidget> BuildTerminalSessionWidget();
	TSharedRef<SWidget> BuildSessionLog();
	void ToggleChatSidebar();

	FReply OnRefreshClicked();
	FReply OnPreparePromptClicked(FString AgentName);
	FReply OnToggleTerminalAgentClicked();
	FReply OnOpenWorkbenchClicked();
	void RefreshStatus();
	void RefreshAgentOptions();
	void ResetTranscript();
	void AddTranscriptLine(const FString& Line);
	EActiveTimerReturnType RunDeferredRefresh(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunDeferredTerminalStartup(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunDeferredVisibleTerminalCommand(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunQueuedVisibleTerminalCommandPoll(double CurrentTime, float DeltaTime);
	void QueueTerminalStartup(bool bForce, bool bResetTerminal = false);
	void RecreateTerminalSession();
	FString GetTerminalLaunchCommandForAgent(const FString& AgentName) const;
	FString BuildTerminalBootstrapCommand(const FString& AgentName) const;

	FString GetSelectedAgentName() const;
	bool HasUsableSelectedAgent() const;
	FString GetSelectedAgentStatusLine() const;
	FText GetTranscriptText() const;
	FText GetReadinessText() const;
	FSlateColor GetReadinessColor() const;
	FText GetNextActionText() const;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Status;
	FHyperAIStudioPromptPackage LastPromptPackage;
	TArray<FHyperAIStudioPrerequisiteStatus> CachedPrerequisites;
	TArray<TSharedPtr<FString>> AgentOptions;
	TSharedPtr<FString> SelectedAgent;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> AgentComboBox;
	TSharedPtr<class SBox> TerminalHost;
	TSharedPtr<class STerminal> TerminalWidget;
	TSharedPtr<class SScrollBar> TerminalScrollBar;
	TSharedPtr<class SHyperAIStudioChatHistorySidebar> ChatSidebar;
	FCurveSequence ChatSidebarCurve;
	bool bChatSidebarOpen = false;
	/** Chat running in the terminal, if it was resumed from history. */
	FString ActiveChatSessionId;
	TArray<FString> TranscriptLines;
	FSimpleDelegate OnOpenWorkbench;
	FText LastMessage;
	bool bRefreshing = false;
	bool bTerminalStartupSent = false;
	bool bTerminalStartupPaused = false;
	bool bVisibleTerminalCommandPending = false;
	FString PendingVisibleTerminalCommand;
	FString PendingVisibleTerminalLabel;
	FString ActiveTerminalAgentName;
	/** Chat the next agent startup resumes; cleared once that startup is sent. */
	FString ResumeAgentName;
	FString ResumeSessionId;
};
