// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioService.h"
#include "Widgets/SCompoundWidget.h"

enum class EHyperAIStudioNativeExecutionMode : uint8;
enum class EHyperAIStudioNativeToolChannel : uint8;

struct FHyperAIStudioWorkbenchSurfaceSmoke
{
	TArray<FString> NavigationTabs;
	TArray<FString> FirstRunLabels;
	TArray<FString> AgentTargetActions;
	TArray<FString> HandoffActions;
	TArray<FString> AgentTestActions;
	TArray<FString> AdvancedLabels;
	TArray<FString> AgentConfigLabels;
	TArray<FString> MCPServerActions;
	TArray<FString> BannedSubstrings;
};

class SHyperAIStudioWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	static int32 GetInitialTabIndexForStatus(const FHyperAIStudioStatus& InStatus);
	static int32 GetPostRefreshTabIndex(int32 CurrentTabIndex, const FHyperAIStudioStatus& NewStatus, bool bUserSelectedTab, bool bAutoRoutePending);
	static FText GetTerminalPanelHintText();
	static FString GetTerminalStartupRecipeText(const FString& ProjectRoot);
	static FHyperAIStudioWorkbenchSurfaceSmoke GetWorkbenchSurfaceSmoke();
	static bool IsRetiredExactOwnedMCPServerEntryForWorkbench(const FHyperAIStudioMCPServerEntry& Entry);
	static FString BuildPrerequisiteInstallTerminalCommandForSmoke(const FString& PrerequisiteId, const FString& DisplayName, const FString& InstallCommand, const FString& ProjectRoot);

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildTabBar();
	TSharedRef<SWidget> BuildTabButton(const FText& Label, const FName IconName, int32 TabIndex);
	TSharedRef<SWidget> BuildIconTextButton(const FText& Label, const FName IconName, const FText& ToolTip, FOnClicked OnClicked) const;
	TSharedRef<SWidget> BuildSetupTab();
	TSharedRef<SWidget> BuildAgentsTab();
	TSharedRef<SWidget> BuildTerminalTab();
	TSharedRef<SWidget> BuildStatusTab();
	TSharedRef<SWidget> BuildTestsTab();
	TSharedRef<SWidget> BuildMCPServersTab();
	TSharedRef<SWidget> BuildNativeToolSettingsBar();
	TSharedRef<SWidget> BuildHelpTab();
	TSharedRef<SWidget> BuildSetupSummaryPanel();
	TSharedRef<SWidget> BuildSetupSummaryRow(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter, const FText& ActionLabel, FOnClicked Action) const;
	TSharedRef<SWidget> BuildAgentTableHeader() const;
	TSharedRef<SWidget> BuildAgentRow(const FString& PrerequisiteId, const FString& AgentName, const FString& ConfigRelativePath);
	TSharedRef<SWidget> BuildDocsShortcutRow(const FText& Title, const FText& Body, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const;
	TSharedRef<SWidget> BuildMCPStaticRow(const FText& Name, const FText& Type, TFunction<FText()> StatusGetter, TFunction<FSlateColor()> ColorGetter, const FText& Priority, TFunction<FText()> ToolsGetter, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const;
	TSharedRef<SWidget> BuildFirstRunGuidePanel();
	TSharedRef<SWidget> BuildPrerequisitesPanel();
	TSharedRef<SWidget> BuildAgentStatusPanel();
	TSharedRef<SWidget> BuildAgentStatusRow(const FString& PrerequisiteId, const FString& AgentName, const FText& OpenLabel);
	TSharedRef<SWidget> BuildContextChipsPanel();
	TSharedRef<SWidget> BuildContextChip(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter, const FText& ToolTip) const;
	TSharedRef<SWidget> BuildSessionLogPanel();
	TSharedRef<SWidget> BuildTerminalPanel();
	TSharedRef<SWidget> BuildTestCommandsPanel();
	TSharedRef<SWidget> BuildStatusPanel();
	TSharedRef<SWidget> BuildAgentConfigPanel();
	TSharedRef<SWidget> BuildFilesAndPermissionsPanel();
	TSharedRef<SWidget> BuildMCPRegistryPanel();
	TSharedRef<SWidget> BuildActionBar();
	TSharedRef<SWidget> BuildHeaderMetric(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter) const;
	TSharedRef<SWidget> BuildStatusRow(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter) const;
	TSharedRef<SWidget> BuildPrerequisiteRow(const FHyperAIStudioPrerequisiteStatus& Prerequisite);
	TSharedRef<SWidget> BuildTestCommandRow(const FHyperAIStudioTestCommand& Command);
	TSharedRef<SWidget> BuildMCPServerRow(int32 ServerIndex);
	void ResetContentScroll();

	FReply OnSetupClicked();
	FReply OnEnablePluginsClicked();
	FReply OnRefreshClicked();
	FReply OnStartClicked();
	FReply OnStopClicked();
	FReply OnAddServerClicked();
	FReply OnEditMCPServerClicked(int32 ServerIndex);
	FReply OnOpenChatClicked();
	FReply OnConfigureAgentsClicked();
	FReply OnViewPrerequisitesClicked();
	FReply OnOpenAgentFilesDialogClicked();
	FReply OnOpenAgentFilesForAgentClicked(FString AgentName);
	FReply OnCreateAgentFileClicked(FString RelativePath);
	FReply OnRegenerateAgentFilesClicked();
	FReply OnGenerateAgentConfigClicked(FString AgentName);
	FReply OnUseAgentInChatClicked(FString AgentName);
	FReply OnPromptInAgentClicked(FString AgentName);
	FReply OnOpenMCPToolsClicked(FString ServerName, FString ToolsSummary);
	FReply OnSetNativeExecutionModeClicked(EHyperAIStudioNativeExecutionMode Mode);
	FReply OnSetNativeToolChannelClicked(EHyperAIStudioNativeToolChannel Channel);
	FReply OnOpenProjectPathClicked(FString RelativePath);
	FReply OnRevealProjectPathClicked(FString RelativePath);
	FReply OnInstallPrerequisiteClicked(FString PrerequisiteId, FString DisplayName, FString InstallCommand);
	FReply OnInstallMissingPrerequisitesClicked();
	FReply OnCopyExampleMCPProfileClicked();
	FReply OnImportExampleMCPProfileClicked();
	FReply OnImportMCPProfileClicked();
	FReply OnRemoveMCPServerClicked(int32 ServerIndex);
	FReply OnTestMCPServerClicked(int32 ServerIndex);
	void OnMCPServerEnabledChanged(ECheckBoxState State, int32 ServerIndex);
	void OnMCPServerNameCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex);
	void OnMCPServerDescriptionCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex);
	void OnMCPServerEndpointCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex);
	void OnMCPServerPriorityCommitted(const FText& Text, ETextCommit::Type CommitType, int32 ServerIndex);
	void SaveMCPServerChange(const FString& ChangeSummary);
	FReply OnOpenAgentTestClicked(const FString AgentName);
	FReply OnOpenUnrealTerminalClicked();
	FReply OnCopyTerminalSetupClicked();
	FReply OnCopyInspectorCommandClicked();
	FReply OnOpenAgentsClicked();
	FReply OnOpenClaudeClicked();
	FReply OnOpenIncludedDocsClicked();
	FReply OnCopyFirstRunPromptClicked();
	FReply OnCopyCodexTomlPatchClicked();
	FReply OnOpenOfficialMCPDocsClicked();
	FReply OnOpenProjectFolderClicked();
	FReply OnCopyPrerequisiteCommandClicked(FString DisplayName, FString InstallCommand);
	FReply OnOpenPrerequisiteDocsClicked(FString DisplayName, FString DocumentationUrl);
	FReply OnCopyTestPromptClicked(FString TestId);
	FReply OnOpenTestTerminalClicked(FString TestId);
	bool OpenChatTerminalInstallCommand(const FString& TerminalCommand, const FString& Label, FText& OutMessage);
	void RefreshPrerequisitesCache();
	const FHyperAIStudioPrerequisiteStatus* FindCachedPrerequisite(const FString& PrerequisiteId) const;
	bool HasCachedMissingInstallablePrerequisites() const;
	void RefreshStatus();
	EActiveTimerReturnType RunDeferredRefresh(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunReadinessRetry(double CurrentTime, float DeltaTime);
	void AddSessionLogEntry(const FString& Entry);

	FText GetHeadlineText() const;
	FSlateColor GetHeadlineColor() const;
	FText GetMessageText() const;
	FText GetSessionLogText() const;
	FText BoolText(bool bValue, const FString& TrueText, const FString& FalseText) const;
	FSlateColor BoolColor(bool bValue) const;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Status;
	FHyperAIStudioPromptPackage LastPromptPackage;
	TArray<FHyperAIStudioPrerequisiteStatus> CachedPrerequisites;
	TSharedPtr<class SScrollBox> MainScrollBox;
	TSharedPtr<class SWidgetSwitcher> MainTabSwitcher;
	FText LastMessage;
	TArray<FString> SessionLog;
	FString ActiveInstallingPrerequisiteId;
	int32 ActiveTabIndex = 0;
	bool bRefreshing = false;
	bool bReportNextRefreshResult = false;
	bool bUserSelectedTab = false;
	bool bAutoRouteAfterFirstRefresh = true;
	int32 PendingReadinessRetryCount = 0;
	int32 CachedLiveEpicToolCount = INDEX_NONE;
	int32 CachedLiveHyperToolCount = INDEX_NONE;
};
