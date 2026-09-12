// Games by Hyper 2026.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "HyperAIStudioSettings.h"

class UEdGraph;
class UEdGraphNode;

enum class EHyperAIStudioSetupOutcome : uint8
{
	Failed,
	Complete,
	RestartRequired
};

struct FHyperAIStudioPluginSyncDecision
{
	bool bWriteProjectDescriptor = false;
	bool bRestartRequired = false;
};

struct FHyperAIStudioStatus
{
	bool bUnrealMCPModuleAvailable = false;
	bool bUnrealMCPSettingsConfigured = false;
	bool bRequiredPluginsReady = false;
	bool bServerRunning = false;
	bool bPortListening = false;
	bool bToolsListReachable = false;
	bool bAgentFilesReady = false;
	bool bProbeInProgress = false;
	bool bHasProbeRun = false;
	bool bCapabilityInventoryAvailable = false;
	bool bCapabilityInventoryInProgress = false;
	bool bToolSearchMode = false;
	bool bCapabilityInventoryTruncated = false;
	bool bDesiredCapabilityPluginsReady = false;
	int32 ConfiguredAgentCount = 0;
	int32 SupportedAgentCount = 5;
	uint32 ConfiguredPort = 0;
	uint32 ActivePort = 0;
	int32 RegisteredToolCount = 0;
	int32 ProbeToolCount = 0;
	int32 DiscoverableToolsetCount = 0;
	int32 DescribedToolsetCount = 0;
	int32 InventoryToolCount = 0;
	int32 DesiredCapabilityPluginCount = 0;
	int32 EnabledCapabilityPluginCount = 0;
	FString UrlPath;
	FString Endpoint;
	FString Summary;
	FString ProbeMessage;
	FString CapabilityInventoryMessage;
	FString CapabilityInventoryFingerprint;
	FString CapabilityPresetName;
	TArray<FString> Warnings;

	bool IsReady() const
	{
		return bUnrealMCPModuleAvailable
			&& bUnrealMCPSettingsConfigured
			&& bRequiredPluginsReady
			&& bDesiredCapabilityPluginsReady
			&& bServerRunning
			&& bPortListening
			&& bToolsListReachable
			&& bAgentFilesReady
			&& ConfiguredAgentCount > 0;
	}
};

struct FHyperAIStudioPrerequisiteStatus
{
	FString Id;
	FString DisplayName;
	FString Detail;
	FString ActionHint;
	FString InstallCommand;
	FString DocumentationUrl;
	bool bRequired = true;
	bool bAvailable = false;
	bool bEnabled = false;
	bool bCanEnable = false;
	bool bRestartRequired = false;
};

struct FHyperAIStudioTestCommand
{
	FString Id;
	FString Title;
	FString Description;
	FString Intent;
	FString AgentName;
	FString TerminalCommand;
};

struct FHyperAIStudioPromptPackage
{
	bool bSuccess = false;
	FString AgentName;
	FString PromptText;
	FString PromptPath;
	FString ContextJsonPath;
	FString ContextMarkdownPath;
	FString TerminalCommand;
	FString Message;
};

struct FHyperAIStudioTerminalLaunchPlan
{
	bool bSuccess = false;
	bool bUsesWindowsTerminal = false;
	bool bUsesPowerShell = false;
	FString RouteName;
	FString ExecutablePath;
	FString Arguments;
	FString WorkingDirectory;
};

struct FHyperAIStudioContextSnapshot
{
	FString ProjectRoot;
	FString ProjectFile;
	FString CurrentMap;
	FString Endpoint;
	int32 SelectedActorCount = 0;
	int32 SelectedAssetCount = 0;
	int32 SelectedBlueprintNodeCount = 0;
	FString BlueprintContextHint;
};

class FHyperAIStudioService : public TSharedFromThis<FHyperAIStudioService>
{
public:
	FHyperAIStudioStatus GetStatusSync() const;
	/**
	 * Samples only the already-loaded MCP module/settings/server state. This never loads a module,
	 * opens a socket, scans plugins, checks agent files, or searches the executable path.
	 */
	FHyperAIStudioStatus GetMcpRuntimeStatusFast() const;
	void RefreshStatusAsync(TFunction<void(const FHyperAIStudioStatus&)> OnComplete) const;
	TArray<FHyperAIStudioPrerequisiteStatus> GetPrerequisites() const;
	TArray<FHyperAIStudioTestCommand> GetTestCommands() const;
	EHyperAIStudioSetupOutcome SetUpHyperAIStudio(FText& OutMessage);
	bool EnableRequiredPlugins(FText& OutMessage, bool* bOutRestartRequired = nullptr) const;
	bool StartUnrealMCP(FText& OutMessage) const;
	bool StopUnrealMCP(FText& OutMessage) const;
	bool RestartUnrealMCP(FText& OutMessage) const;
	bool GenerateProjectFiles(FText& OutMessage) const;
	bool CreateQuickConnectPromptPackage(const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const;
	bool CreatePromptPackage(const FString& UserIntent, const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const;
	bool CreatePromptPackageForAssets(const FString& UserIntent, const FString& AgentName, const TArray<FAssetData>& SelectedAssets, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const;
	bool CreateBlueprintPromptPackage(const FString& UserIntent, const FString& AgentName, const UEdGraph* Graph, const UEdGraphNode* ContextNode, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const;
	bool CreateTestPromptPackage(const FString& TestId, const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const;
	bool AutoCommentBlueprintNodes(const UEdGraph* Graph, const UEdGraphNode* ContextNode, FText& OutMessage) const;
	bool AddDefaultCustomServer(FText& OutMessage) const;
	bool ImportMCPProfileFromClipboard(FText& OutMessage) const;
	bool ImportExampleMCPProfile(FText& OutMessage) const;
	bool ImportHyperUEMCPProfile(FText& OutMessage) const;
	bool DisableHyperUEMCPProfile(FText& OutMessage) const;
	bool ImportMCPProfileFromJson(const FString& JsonText, FText& OutMessage) const;
	void TestExtraMCPServerAsync(int32 ServerIndex, TFunction<void(bool bOk, const FString& Message)> OnComplete) const;
	void DescribeUnrealMCPToolsAsync(TFunction<void(const FString& Details)> OnComplete) const;
	FString GetExampleMCPProfileJson() const;
	FString GetProviderMCPProfileJson(const FString& ProviderId) const;
	FString GetCurrentContextSummary() const;
	FHyperAIStudioContextSnapshot GetCurrentContextSnapshot() const;
	FString GetFirstRunPrompt() const;
	FString GetCodexTomlPatch() const;
	bool OpenAgentsFile(FText& OutMessage) const;
	bool OpenClaudeFile(FText& OutMessage) const;
	bool OpenIncludedDocs(FText& OutMessage) const;
	bool OpenProjectFolder(FText& OutMessage) const;
	bool BuildTerminalLaunchPlanForPrompt(const FHyperAIStudioPromptPackage& Package, FHyperAIStudioTerminalLaunchPlan& OutPlan, FText& OutMessage) const;
	bool OpenTerminalForPrompt(const FHyperAIStudioPromptPackage& Package, FText& OutMessage) const;
	bool OpenUnrealTerminal(FText& OutMessage) const;
	void CopyTextToClipboard(const FString& Text) const;

	static FString GetProjectRoot();
	static FString GetEndpoint(uint32 Port, const FString& UrlPath);
	static FString GetDocumentationUrl();
	static FHyperAIStudioPluginSyncDecision GetPluginSyncDecision(
		bool bRuntimeEnabled,
		bool bProjectDescriptorEnabled);
	static FString FindUnsupportedCodexServiceTierConfigPath();
	static bool FindExecutableOnPath(const FString& ExecutableName, FString& OutPath);
	static FString GetCodexTerminalLaunchCommand();
	static FString GetAgentTerminalLaunchCommand(const FString& AgentName);

private:
	static TArray<FString> GetRequiredPluginNames();
	static bool IsExecutableOnPath(const FString& ExecutableName);
	static bool ConfigureUnrealMCPSettings(FText& OutMessage);
	static bool GenerateManagedProjectFilesTransactional(const FString& ProjectRoot, const FString& Endpoint, UHyperAIStudioSettings* Settings, FText& OutMessage);
	static bool GenerateHelperScripts(const FString& ProjectRoot, const FString& Endpoint, const UHyperAIStudioSettings* Settings, FText& OutMessage);
	static bool WriteStatusJson(const FString& ProjectRoot, const FHyperAIStudioStatus& Status);
	static bool AreAgentFilesReady(const FString& ProjectRoot, const UHyperAIStudioSettings* Settings);
	static int32 CountConfiguredAgents(const FString& ProjectRoot, const UHyperAIStudioSettings* Settings);
	static bool IsPortListening(uint32 Port);
	static void ProbeToolsListAsync(const FString& Endpoint, TFunction<void(bool bReachable, const FString& Message, int32 ToolCount)> OnComplete);
};
