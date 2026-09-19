// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Containers/Ticker.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgentState.h"
#include "HyperAIStudioService.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class SHyperAIStudioQuickActionWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioQuickActionWindow) {}
		SLATE_EVENT(FSimpleDelegate, OnOpenWorkbench)
		/** Opens another chat tab, so a second agent can work alongside this one. */
		SLATE_EVENT(FSimpleDelegate, OnNewAgentTab)
		/** 1 for the first chat tab; shown in the tab label so tabs can be told apart. */
		SLATE_ARGUMENT(int32, TabIndex)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioQuickActionWindow() override;
	/** The dock tab hosting this panel, so its label can carry the agent's state. */
	void SetOwnerTab(const TSharedRef<class SDockTab>& InTab);
	EHyperAIStudioAgentState GetAgentState() const { return AgentState.State; }
	/** The agent and model this tab's terminal launched, while that session runs. */
	bool GetRunningAgent(FString& OutAgent, FString& OutModel) const;
	/** "HyperAI Chat 2: VFX (Codex)": which tab and agent, for handoff menus and messages. */
	FString GetTabDescription() const;
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
	/** Assets and actors dropped on the panel become object paths in the agent's prompt. */
	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildStatusLine();
	TSharedRef<SWidget> BuildSettingsMenu();
	TSharedRef<SWidget> BuildAgentActions();
	TSharedRef<SWidget> BuildAgentSelector();
	TSharedRef<SWidget> BuildModelSelector();
	TSharedRef<SWidget> BuildModelMenu();
	TSharedRef<SWidget> BuildContextSelector();
	TSharedRef<SWidget> BuildContextMenu();
	TSharedRef<SWidget> BuildMcpStatusSelector();
	TSharedRef<SWidget> BuildMcpStatusMenu();
	/** Paste text into the running agent's prompt without sending it. */
	bool InsertIntoAgentPrompt(const FString& Text, const FString& Label, bool bFocusTerminal = true);
	/** Handoff prompts: sent when the agent is idle and auto-send is on, otherwise typed in for the user to send. */
	bool DeliverHandoffPrompt(const FString& Text, const FString& Label);
	TSharedRef<SWidget> BuildNewTabMenu();
	TSharedRef<SWidget> BuildHandoffMenu();
	void OpenRoutedTab(const FString& Category, const FHyperAIStudioRouteChoice& Choice);
	void StartHandoff(TWeakPtr<SHyperAIStudioQuickActionWindow> Builder);
	/**
	 * Moves a running handoff along: plan file written, then result file written, then review asked for. Runs on
	 * the core ticker and watches files, because Slate paints a hidden tab's terminal, and its timers, not at all.
	 */
	bool TickHandoff(float DeltaTime);
	TArray<FString> GetUsableAgentNames() const;
	/** The model this tab launches AgentName with: the tab's own choice, else the agent's saved one. */
	FString GetModelForAgent(const FString& AgentName) const;
	FText GetTabTitle() const;
	FText GetMcpStatusText() const;
	FSlateColor GetMcpStatusColor() const;
	void SelectModel(FString ModelId);
	bool IsModelSelected(FString ModelId) const;
	FText GetModelButtonText() const;
	TSharedRef<SWidget> BuildTerminalSessionWidget();
	TSharedRef<SWidget> BuildSessionLog();
	void ToggleChatSidebar();
	void ToggleActivitySidebar();

	FReply OnRefreshClicked();
	FReply OnPreparePromptClicked(FString AgentName);
	FReply OnToggleTerminalAgentClicked();
	FReply OnOpenWorkbenchClicked();
	/** bSilent keeps the current status on screen until the probe settles (heartbeat re-probes). */
	void RefreshStatus(bool bSilent = false);
	void RefreshAgentOptions();
	void ResetTranscript();
	void AddTranscriptLine(const FString& Line);
	EActiveTimerReturnType RunDeferredRefresh(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunStatusHeartbeat(double CurrentTime, float DeltaTime);
	/** Re-reads what this tab's agent is showing and updates the tab label when it changes. */
	EActiveTimerReturnType RunAgentStateHeartbeat(double CurrentTime, float DeltaTime);
	/** Smart mode: answers Claude's plan prompt with its auto-mode option, once per prompt. */
	void AutoAcceptSmartPlan(const FString& Tail, double LastOutputTime);
	TSharedRef<SWidget> BuildAgentStateBadge();
	FSlateColor GetAgentStateColor() const;
	FText GetTabLabel() const;
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
	TSharedPtr<class SHyperAIStudioChatActivitySidebar> ActivitySidebar;
	FCurveSequence ActivitySidebarCurve;
	bool bActivitySidebarOpen = false;
	FCurveSequence ChatSidebarCurve;
	bool bChatSidebarOpen = false;
	/** Chat running in the terminal, if it was resumed from history. */
	FString ActiveChatSessionId;
	TArray<FString> TranscriptLines;
	FSimpleDelegate OnOpenWorkbench;
	FText LastMessage;
	bool bRefreshing = false;
	/**
	 * Hint under the header, set only when a probe settles. Every re-probe briefly reports "not ready",
	 * and binding the line to live status made it expand and collapse on each heartbeat.
	 */
	FText StatusHint;
	bool bTerminalStartupSent = false;
	bool bTerminalStartupPaused = false;
	bool bVisibleTerminalCommandPending = false;
	FString PendingVisibleTerminalCommand;
	FString PendingVisibleTerminalLabel;
	FString ActiveTerminalAgentName;
	/** Chat the next agent startup resumes; cleared once that startup is sent. */
	FString ResumeAgentName;
	FString ResumeSessionId;
	FSimpleDelegate OnNewAgentTab;
	TWeakPtr<class SDockTab> OwnerTab;
	int32 TabIndex = 1;
	FHyperAIStudioAgentStateSnapshot AgentState;
	/** The option digit went to the plan prompt now showing; cleared once that prompt leaves the screen. */
	bool bPlanAutoAcceptSent = false;
	bool bPlanAutoAcceptConfirmed = false;
	double PlanAutoAcceptSentTime = 0.0;
	/** Kind of task this tab was opened for from the new-tab menu; shown on the tab. */
	FString RouteCategory;
	/** Model chosen in this tab (or by its route) for TabModelAgent; other tabs keep their own. */
	FString TabModel;
	FString TabModelAgent;
	FString ActiveTerminalModel;

	/** Plan here, build in another tab, then review here. */
	struct FHandoff
	{
		FString PlanPath;
		/** The builder writes what it changed here when it is done. */
		FString ResultPath;
		TWeakPtr<SHyperAIStudioQuickActionWindow> Builder;
		FString BuilderDescription;
		bool bBuilding = false;
		/** Size of the awaited file at the last tick; a file counts as written once its size holds for a tick. */
		int64 LastSize = -1;
		double StartedTime = 0.0;
	};
	TOptional<FHandoff> Handoff;
	FTSTicker::FDelegateHandle HandoffTicker;
};
