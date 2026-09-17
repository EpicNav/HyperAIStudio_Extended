// Games by Hyper 2026.

#include "Async/Async.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Modules/ModuleManager.h"

#include "ContentBrowserMenuContexts.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "GraphEditorModule.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioBlueprintWorkflowToolset.h"
#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioDiagnosticsRegistration.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioFoundationProbe.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioPlanValidateToolset.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioTrustedExecutionInternal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/HyperAIStudioBenchmarkInstrumentation.h"
#endif
#include "Interfaces/IPluginManager.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Selection.h"
#include "SHyperAIStudioWindow.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "UnrealEdMisc.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "FHyperAIStudioModule"

namespace
{
	const FName HyperAIStudioTabName(TEXT("HyperAIStudio"));
	const FName HyperAIStudioChatTabName(TEXT("HyperAIStudioChat"));

	FString NormalizeMCPServerSelector(const FString& RawValue)
	{
		FString Result;
		const FString Source = RawValue.ToLower();
		for (const TCHAR Ch : Source)
		{
			if ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('-') || Ch == TEXT('_'))
			{
				Result.AppendChar(Ch);
			}
			else if (Ch == TEXT(' ') || Ch == TEXT('.') || Ch == TEXT('/'))
			{
				Result.AppendChar(TEXT('-'));
			}
		}
		return Result;
	}

	struct FHyperAIStudioSavedFile
	{
		FString Path;
		bool bExisted = false;
		bool bCaptured = false;
		TArray<uint8> Contents;
	};

	struct FHyperAIStudioSavedBackupDirectory
	{
		FString Path;
		bool bExisted = false;
		bool bCaptured = false;
		TSet<FString> ExistingFiles;
		TSet<FString> ExistingDirectories;
	};

	struct FHyperAIStudioExpectedMenuEntry
	{
		FName Name;
		FString Label;
		FString ToolTipContains;
	};

	struct FHyperAIStudioBlueprintMenuEntryDefinition
	{
		FName Id;
		FText Label;
		FText ToolTip;
		FName IconName;
		FString AgentName;
		FString Intent;
		bool bOpenTerminal = false;
		bool bQuickAutoComment = false;
	};

	bool IsHyperAIStudioContextAsset(const FAssetData& Asset)
	{
		if (!Asset.IsValid())
		{
			return false;
		}

		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		return ClassName.Contains(TEXT("Blueprint"), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
	}

	bool IsHyperAIStudioContextAssetSelection(const TArray<FAssetData>& Assets)
	{
		if (Assets.IsEmpty())
		{
			return false;
		}

		for (const FAssetData& Asset : Assets)
		{
			if (!IsHyperAIStudioContextAsset(Asset))
			{
				return false;
			}
		}
		return true;
	}

	bool IsHyperAIStudioContextAssetMenuSelection(const FToolMenuContext& MenuContext)
	{
		const UContentBrowserAssetContextMenuContext* AssetContext = MenuContext.FindContext<UContentBrowserAssetContextMenuContext>();
		return AssetContext && IsHyperAIStudioContextAssetSelection(AssetContext->SelectedAssets);
	}

	bool IsHyperAIStudioContextGraphName(const FString& Name)
	{
		return Name.Contains(TEXT("Blueprint"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("K2"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("AnimGraph"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
	}

	bool IsHyperAIStudioContextGraph(const UEdGraph* Graph)
	{
		if (!Graph)
		{
			return false;
		}

		if (IsHyperAIStudioContextGraphName(Graph->GetName()) || IsHyperAIStudioContextGraphName(Graph->GetClass()->GetName()))
		{
			return true;
		}

		if (const UEdGraphSchema* Schema = Graph->GetSchema())
		{
			if (IsHyperAIStudioContextGraphName(Schema->GetClass()->GetName()))
			{
				return true;
			}
		}

		for (const UObject* Outer = Graph->GetOuter(); Outer; Outer = Outer->GetOuter())
		{
			if (IsHyperAIStudioContextGraphName(Outer->GetName()) || IsHyperAIStudioContextGraphName(Outer->GetClass()->GetName()))
			{
				return true;
			}
		}

		return false;
	}

	TArray<FHyperAIStudioBlueprintMenuEntryDefinition> GetBlueprintMenuEntryDefinitions()
	{
		const FString ContextIntent = TEXT("Use the selected Blueprint nodes as context for my next instruction. Read the referenced context files, do not modify the Blueprint yet, and wait for my task.");
		const FString SmartAutoCommentIntent = TEXT("Smart Auto Comment the selected Blueprint nodes. Use only the selected-node context package as the authoritative selection. Goal: add useful native Blueprint comment box(es) around the selected nodes with Unreal MCP. Default to one concise comment around the whole selected flow. Use multiple comments only when the selected nodes clearly form separate independent regions. Do not create nested or overlapping comments. Every node id used for a comment must come exactly from the selected-node context; do not invent ids. Use the available Unreal MCP comment/add-comment tool with node ids and validation when supported. If no comment tool is available, report the exact blocker and the comment text that should be added. Do not move, create, delete, rewire, or compile Blueprint nodes.");
		return {
			{
				TEXT("CopyBlueprintNodeContext"),
				LOCTEXT("CopyBlueprintNodeContext", "Copy Selected Node Context"),
				LOCTEXT("CopyBlueprintNodeContextTooltip", "Copy provider-neutral context for the selected Blueprint nodes to use in any terminal or coding agent."),
				TEXT("Icons.Comment"),
				TEXT("Coding Agent"),
				ContextIntent,
				false,
				false
			},
			{
				TEXT("SmartAutoCommentBlueprintNodes"),
				LOCTEXT("SmartAutoCommentBlueprintNodes", "Smart Auto Comment Selected Nodes"),
				LOCTEXT("SmartAutoCommentBlueprintNodesTooltip", "Open the preferred coding-agent flow and ask AI to understand the selected nodes before adding useful native Blueprint comments."),
				TEXT("Icons.Comment"),
				FString(),
				SmartAutoCommentIntent,
				true,
				false
			},
			{
				TEXT("QuickAutoCommentBlueprintNodes"),
				LOCTEXT("QuickAutoCommentBlueprintNodes", "Quick Auto Comment Selected Nodes"),
				LOCTEXT("QuickAutoCommentBlueprintNodesTooltip", "Add a fast local title-based Unreal comment box around the selected nodes without using AI."),
				TEXT("Icons.Comment"),
				FString(),
				FString(),
				false,
				true
			}
		};
	}

	TArray<FHyperAIStudioSavedFile> SaveFilesForSmoke(const TArray<FString>& FilePaths)
	{
		TArray<FHyperAIStudioSavedFile> Snapshots;
		for (const FString& FilePath : FilePaths)
		{
			FHyperAIStudioSavedFile Snapshot;
			Snapshot.Path = FilePath;
			Snapshot.bExisted = FPaths::FileExists(FilePath);
			Snapshot.bCaptured = !Snapshot.bExisted
				|| FFileHelper::LoadFileToArray(Snapshot.Contents, *FilePath);
			Snapshots.Add(MoveTemp(Snapshot));
		}
		return Snapshots;
	}

	bool RestoreFilesForSmoke(const TArray<FHyperAIStudioSavedFile>& Snapshots)
	{
		bool bOk = true;
		for (const FHyperAIStudioSavedFile& Snapshot : Snapshots)
		{
			if (!Snapshot.bCaptured)
			{
				bOk = false;
				continue;
			}
			if (Snapshot.bExisted)
			{
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Snapshot.Path), true);
				bOk &= FFileHelper::SaveArrayToFile(Snapshot.Contents, *Snapshot.Path);
			}
			else if (FPaths::FileExists(Snapshot.Path))
			{
				bOk &= IFileManager::Get().Delete(*Snapshot.Path, false, true);
			}
		}
		return bOk;
	}

	bool AreFileSnapshotsCompleteForSmoke(const TArray<FHyperAIStudioSavedFile>& Snapshots)
	{
		return !Snapshots.ContainsByPredicate([](const FHyperAIStudioSavedFile& Snapshot)
		{
			return !Snapshot.bCaptured;
		});
	}

	bool EnumerateDirectoryForSmoke(
		const FString& Directory,
		TSet<FString>& OutFiles,
		TSet<FString>& OutDirectories)
	{
		OutFiles.Reset();
		OutDirectories.Reset();
		return IFileManager::Get().IterateDirectory(
			*Directory,
			[&OutFiles, &OutDirectories](const TCHAR* EntryPath, bool bIsDirectory)
			{
				const FString EntryName = FPaths::GetCleanFilename(FString(EntryPath));
				if (bIsDirectory)
				{
					OutDirectories.Add(EntryName);
				}
				else
				{
					OutFiles.Add(EntryName);
				}
				return true;
			});
	}

	TArray<FHyperAIStudioSavedBackupDirectory> SaveBackupDirectoriesForSmoke(
		const TArray<FString>& ManagedFilePaths)
	{
		TSet<FString> BackupDirectories;
		for (const FString& FilePath : ManagedFilePaths)
		{
			const FString ParentDirectory = FPaths::GetPath(FilePath);
			if (!ParentDirectory.IsEmpty())
			{
				BackupDirectories.Add(FPaths::Combine(ParentDirectory, TEXT(".hyperai-backups")));
			}
		}

		TArray<FString> SortedBackupDirectories = BackupDirectories.Array();
		SortedBackupDirectories.Sort();
		TArray<FHyperAIStudioSavedBackupDirectory> Snapshots;
		Snapshots.Reserve(SortedBackupDirectories.Num());
		for (const FString& BackupDirectory : SortedBackupDirectories)
		{
			FHyperAIStudioSavedBackupDirectory Snapshot;
			Snapshot.Path = BackupDirectory;
			Snapshot.bExisted = IFileManager::Get().DirectoryExists(*BackupDirectory);
			Snapshot.bCaptured = !Snapshot.bExisted
				|| EnumerateDirectoryForSmoke(
					BackupDirectory,
					Snapshot.ExistingFiles,
					Snapshot.ExistingDirectories);
			Snapshots.Add(MoveTemp(Snapshot));
		}
		return Snapshots;
	}

	bool AreBackupDirectorySnapshotsCompleteForSmoke(
		const TArray<FHyperAIStudioSavedBackupDirectory>& Snapshots)
	{
		return !Snapshots.ContainsByPredicate([](const FHyperAIStudioSavedBackupDirectory& Snapshot)
		{
			return !Snapshot.bCaptured;
		});
	}

	bool AreBackupDirectoriesUnchangedForSmoke(
		const TArray<FHyperAIStudioSavedBackupDirectory>& Snapshots)
	{
		const auto SetsEqual = [](const TSet<FString>& A, const TSet<FString>& B)
		{
			if (A.Num() != B.Num())
			{
				return false;
			}
			for (const FString& Value : A)
			{
				if (!B.Contains(Value))
				{
					return false;
				}
			}
			return true;
		};

		for (const FHyperAIStudioSavedBackupDirectory& Snapshot : Snapshots)
		{
			if (!Snapshot.bCaptured)
			{
				return false;
			}

			const bool bExistsNow = IFileManager::Get().DirectoryExists(*Snapshot.Path);
			if (bExistsNow != Snapshot.bExisted)
			{
				return false;
			}
			if (!bExistsNow)
			{
				continue;
			}

			TSet<FString> CurrentFiles;
			TSet<FString> CurrentDirectories;
			if (!EnumerateDirectoryForSmoke(Snapshot.Path, CurrentFiles, CurrentDirectories))
			{
				return false;
			}
			if (!SetsEqual(CurrentFiles, Snapshot.ExistingFiles)
				|| !SetsEqual(CurrentDirectories, Snapshot.ExistingDirectories))
			{
				return false;
			}
		}
		return true;
	}

	bool LoadJsonRootForSmoke(const FString& FilePath, TSharedPtr<FJsonObject>& OutRoot)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
	}

	bool FindMCPServerJsonForSmoke(
		const FString& FilePath,
		const FString& ContainerField,
		const FString& ServerId,
		TSharedPtr<FJsonObject>& OutServerObject)
	{
		TSharedPtr<FJsonObject> RootObject;
		if (!LoadJsonRootForSmoke(FilePath, RootObject))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ContainerObject = nullptr;
		if (!RootObject->TryGetObjectField(ContainerField, ContainerObject) || !ContainerObject || !ContainerObject->IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ServerObject = nullptr;
		if (!(*ContainerObject)->TryGetObjectField(ServerId, ServerObject) || !ServerObject || !ServerObject->IsValid())
		{
			return false;
		}

		OutServerObject = *ServerObject;
		return true;
	}

	bool JsonMCPServerStringEqualsForSmoke(
		const FString& FilePath,
		const FString& ContainerField,
		const FString& ServerId,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		TSharedPtr<FJsonObject> ServerObject;
		FString ActualValue;
		return FindMCPServerJsonForSmoke(FilePath, ContainerField, ServerId, ServerObject)
			&& ServerObject->TryGetStringField(FieldName, ActualValue)
			&& ActualValue == ExpectedValue;
	}

	bool JsonMCPServerArrayContainsForSmoke(
		const FString& FilePath,
		const FString& ContainerField,
		const FString& ServerId,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		TSharedPtr<FJsonObject> ServerObject;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!FindMCPServerJsonForSmoke(FilePath, ContainerField, ServerId, ServerObject)
			|| !ServerObject->TryGetArrayField(FieldName, Values)
			|| !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid() && Value->AsString() == ExpectedValue)
			{
				return true;
			}
		}
		return false;
	}
}

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudio, Log, All);

class FHyperAIStudioModule final : public IModuleInterface
{
public:
	virtual bool SupportsDynamicReloading() override
	{
		// Async Epic tool/fresh-read futures are intentionally uncancellable. Never hot-unload
		// this module while one of their completion continuations can still hold its code.
		return false;
	}

	virtual bool SupportsAutomaticShutdown() override
	{
		// This editor module has several intentionally asynchronous engine/HTTP continuations,
		// including callbacks outside PlanExecutionService. Keep its code resident until process
		// teardown; explicit registration objects still provide deterministic test cleanup.
		return false;
	}

	virtual void StartupModule() override
	{
		// First: the tab spawners below reference brushes in this style set.
		FHyperAIStudioStyle::Register();
		EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(
			this, &FHyperAIStudioModule::BeginEnginePreExit);
		if (FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled())
		{
			FString TrustedExecutionError;
			if (!HyperAIStudio::TrustedExecution::Private::StartupCore(
					FPaths::ProjectDir(), TrustedExecutionError))
			{
				UE_LOG(LogHyperAIStudio, Error,
					TEXT("Trusted optional-pack execution host failed closed: %s"),
					*TrustedExecutionError);
			}
			PlanValidateToolRegistration = MakeUnique<FHyperAIStudioPlanValidateToolRegistration>();
			PlanValidateToolRegistration->Startup();
			PlanExecuteToolRegistration = MakeUnique<FHyperAIStudioPlanExecuteToolRegistration>();
			PlanExecuteToolRegistration->Startup();
			NativeReadToolRegistration = MakeUnique<FHyperAIStudioNativeReadToolRegistration>();
			NativeReadToolRegistration->Startup();
			ContextSearchToolRegistration = MakeUnique<FHyperAIStudioContextSearchRegistration>();
			ContextSearchToolRegistration->Startup();
			BlueprintWorkflowRegistration = MakeUnique<FHyperAIStudioBlueprintWorkflowRegistration>();
			BlueprintWorkflowRegistration->Startup();
			DiagnosticsRegistration = MakeUnique<FHyperAIStudioDiagnosticsRegistration>();
			DiagnosticsRegistration->Startup();
			PIEPlaytestRegistration = MakeUnique<FHyperAIStudioPIEPlaytestRegistration>();
			PIEPlaytestRegistration->Startup();
			// Every optional pack's typed mutation requires this probe to be live and fresh.
			FoundationProbe = MakeUnique<FHyperAIStudioFoundationProbe>();
			FoundationProbe->Startup();
			LoadAvailableCapabilityModules();
		}
#if WITH_DEV_AUTOMATION_TESTS
		HyperAIStudio::BenchmarkInstrumentation::Startup();
#endif

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			HyperAIStudioTabName,
			FOnSpawnTab::CreateRaw(this, &FHyperAIStudioModule::SpawnTab))
			.SetDisplayName(LOCTEXT("TabTitle", "HyperAIStudio"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Open HyperAIStudio setup, agent handoff, and MCP readiness."))
			.SetIcon(FSlateIcon(FHyperAIStudioStyle::GetStyleSetName(), "HyperAIStudio.TabIcon"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			HyperAIStudioChatTabName,
			FOnSpawnTab::CreateRaw(this, &FHyperAIStudioModule::SpawnChatTab))
			.SetDisplayName(LOCTEXT("ChatTabTitle", "HyperAI Chat"))
			.SetTooltipText(LOCTEXT("ChatTabTooltip", "Open the lightweight HyperAI chat panel for Unreal context handoff."))
			.SetIcon(FSlateIcon(FHyperAIStudioStyle::GetStyleSetName(), "HyperAIStudio.TabIcon"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FHyperAIStudioModule::RegisterMenus));

		CommandSetUp = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.SetUp"),
			TEXT("Configures Unreal MCP, starts the server, and writes HyperAIStudio agent files."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleSetUp));

		CommandGenerateFiles = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.GenerateProjectFiles"),
			TEXT("Writes HyperAIStudio agent configs and helper scripts for the current project."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleGenerateFiles));

		CommandDocsSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.DocsSmoke"),
			TEXT("Validates the packaged HyperAIStudio HTML documentation for required first-user workflow sections. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleDocsSmoke));

		CommandWelcomeSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.WelcomeSmoke"),
			TEXT("Validates startup welcome copy, actions, unattended suppression, and initial workbench routing. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleWelcomeSmoke));

		CommandWorkbenchSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.WorkbenchSmoke"),
			TEXT("Validates split workbench/chat surface labels, action wording, MCP profile shortcuts, and Terminal fallback guidance. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleWorkbenchSmoke));

		CommandAgentConfigSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.AgentConfigSmoke"),
			TEXT("Temporarily enables every supported agent config writer, regenerates project files, and validates expected config files. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleAgentConfigSmoke));

		CommandMCPRegistrySmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.MCPRegistrySmoke"),
			TEXT("Validates disabled MCP profile imports, secret handling, and enabled extra MCP exports across generated agent configs. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleMCPRegistrySmoke));

		CommandHyperUEMCPSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.HyperUEMCPSmoke"),
			TEXT("Validates migration of a retired built-in profile out of HyperAIStudio-managed configs. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleHyperUEMCPSmoke));

		CommandMCPClipboardSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.MCPClipboardSmoke"),
			TEXT("Validates MCP profile copy/import clipboard routes and restoration. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleMCPClipboardSmoke));

		CommandImportMCPProfile = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.ImportMCPProfileFromClipboard"),
			TEXT("Imports a disabled hyperai-mcp.json profile from the clipboard and regenerates agent files."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleImportMCPProfile));

		CommandImportMCPProfileFromFile = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.ImportMCPProfileFromFile"),
			TEXT("Imports a disabled hyperai-mcp.json profile from a file and regenerates agent files. Usage: HyperAIStudio.ImportMCPProfileFromFile <path>"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleImportMCPProfileFromFile));

		CommandImportExampleMCPProfile = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.ImportExampleMCPProfile"),
			TEXT("Imports the built-in disabled example MCP profile and regenerates agent files."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleImportExampleMCPProfile));

		CommandImportProviderMCPProfile = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.ImportProviderMCPProfile"),
			TEXT("Imports a built-in disabled provider MCP profile and regenerates agent files. Usage: HyperAIStudio.ImportProviderMCPProfile <meshy|tripo|elevenlabs|custom|all>"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleImportProviderMCPProfile));

		CommandTestMCPServer = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.TestMCPServer"),
			TEXT("Tests a configured extra MCP server by index, id, or display name. Add quit-on-complete for unattended smoke runs. Example: HyperAIStudio.TestMCPServer project-docs"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleTestMCPServer));

		CommandCreateTestPrompt = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.CreateTestPrompt"),
			TEXT("Creates a test prompt/context package without opening a terminal. Usage: HyperAIStudio.CreateTestPrompt [test-id] [agent name]"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleCreateTestPrompt));

		CommandAgentPromptSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.AgentPromptSmoke"),
			TEXT("Creates connection-test prompt/context packages for every supported external agent route. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleAgentPromptSmoke));

		CommandHandoffScriptSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.HandoffScriptSmoke"),
			TEXT("Validates generated HyperAIStudio prompt handoff scripts for terminal, clipboard, CLI, and editor-agent branches."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleHandoffScriptSmoke));

		CommandTerminalLaunchPlanSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.TerminalLaunchPlanSmoke"),
			TEXT("Validates prompt handoff launch plans without opening a real terminal. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleTerminalLaunchPlanSmoke));

		CommandBlueprintContextSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.BlueprintContextSmoke"),
			TEXT("Creates a transient Blueprint graph context package and native auto-comment for unattended validation. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleBlueprintContextSmoke));

		CommandBlueprintMenuSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.BlueprintMenuSmoke"),
			TEXT("Validates Blueprint graph context-menu action labels, tooltips, intents, and extender registration. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleBlueprintMenuSmoke));

		CommandSelectionContextSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.SelectionContextSmoke"),
			TEXT("Creates a transient selected actor context package for unattended validation. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleSelectionContextSmoke));

		CommandAssetContextSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.AssetContextSmoke"),
			TEXT("Creates a transient selected asset context package for unattended validation. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleAssetContextSmoke));

		CommandCopyCodexTomlPatch = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.CopyCodexTomlPatch"),
			TEXT("Copies the HyperAIStudio managed Codex TOML block to the clipboard without writing files. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleCopyCodexTomlPatch));

		CommandStartMCP = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.StartMCP"),
			TEXT("Starts Epic Unreal MCP using HyperAIStudio settings."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleStartMCP));

		CommandStatus = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.Status"),
			TEXT("Logs the current HyperAIStudio readiness status."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleStatus));

		CommandStatusProbe = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.StatusProbe"),
			TEXT("Runs the async Unreal MCP tools/list readiness probe. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleStatusProbe));

		CommandPrerequisites = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.Prerequisites"),
			TEXT("Logs HyperAIStudio prerequisite detection states."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsolePrerequisites));

		CommandPrerequisiteGuidanceSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.PrerequisiteGuidanceSmoke"),
			TEXT("Validates missing-tool install/docs guidance and fake installed-tool detection for first-user routes."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsolePrerequisiteGuidanceSmoke));

		CommandOpen = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.Open"),
			TEXT("Opens the HyperAIStudio editor tab."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleOpen));

		CommandOpenChat = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.Chat"),
			TEXT("Opens the lightweight HyperAI chat editor tab. Optional argument selects the preferred agent."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleOpenChat));

		CommandQuickAction = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.QuickAction"),
			TEXT("Opens the lightweight HyperAI chat overlay."),
			FConsoleCommandDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleQuickAction));

		CommandQuickActionSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.QuickActionSmoke"),
			TEXT("Validates HyperAI Chat next-action messaging for setup, checking, and ready states."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleQuickActionSmoke));

		CommandMenuSmoke = MakeUnique<FAutoConsoleCommand>(
			TEXT("HyperAIStudio.MenuSmoke"),
			TEXT("Validates HyperAIStudio Tools menu, status bar, selection context-menu, graph extender registration, and visible menu labels. Add quit-on-complete for unattended smoke runs."),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FHyperAIStudioModule::RunConsoleMenuSmoke));

		RegisterGraphEditorContextMenus();

		WelcomePromptTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FHyperAIStudioModule::RunWelcomePromptTicker),
			2.0f);
	}

	virtual void ShutdownModule() override
	{
		BeginEnginePreExit();
		if (EnginePreExitHandle.IsValid())
		{
			FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
			EnginePreExitHandle.Reset();
		}
		if (PIEPlaytestRegistration)
		{
			PIEPlaytestRegistration->Shutdown();
			PIEPlaytestRegistration.Reset();
		}
		if (DiagnosticsRegistration)
		{
			DiagnosticsRegistration->Shutdown();
			DiagnosticsRegistration.Reset();
		}
		if (BlueprintWorkflowRegistration)
		{
			BlueprintWorkflowRegistration->Shutdown();
			BlueprintWorkflowRegistration.Reset();
		}
		if (ContextSearchToolRegistration)
		{
			ContextSearchToolRegistration->Shutdown();
			ContextSearchToolRegistration.Reset();
		}
		if (NativeReadToolRegistration)
		{
			NativeReadToolRegistration->Shutdown();
			NativeReadToolRegistration.Reset();
		}
		if (PlanExecuteToolRegistration)
		{
			PlanExecuteToolRegistration->Shutdown();
			PlanExecuteToolRegistration.Reset();
		}
		if (PlanValidateToolRegistration)
		{
			PlanValidateToolRegistration->Shutdown();
			PlanValidateToolRegistration.Reset();
		}
		if (WelcomePromptTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(WelcomePromptTickerHandle);
			WelcomePromptTickerHandle.Reset();
		}

		UnregisterGraphEditorContextMenus();

		CommandQuickActionSmoke.Reset();
		CommandQuickAction.Reset();
		CommandOpenChat.Reset();
		CommandMenuSmoke.Reset();
		CommandOpen.Reset();
		CommandPrerequisiteGuidanceSmoke.Reset();
		CommandPrerequisites.Reset();
		CommandStatusProbe.Reset();
		CommandStatus.Reset();
		CommandStartMCP.Reset();
		CommandCopyCodexTomlPatch.Reset();
		CommandAssetContextSmoke.Reset();
		CommandSelectionContextSmoke.Reset();
		CommandBlueprintMenuSmoke.Reset();
		CommandBlueprintContextSmoke.Reset();
		CommandTerminalLaunchPlanSmoke.Reset();
		CommandHandoffScriptSmoke.Reset();
		CommandAgentPromptSmoke.Reset();
		CommandCreateTestPrompt.Reset();
		CommandTestMCPServer.Reset();
		CommandImportProviderMCPProfile.Reset();
		CommandImportExampleMCPProfile.Reset();
		CommandImportMCPProfileFromFile.Reset();
		CommandImportMCPProfile.Reset();
		CommandMCPClipboardSmoke.Reset();
		CommandHyperUEMCPSmoke.Reset();
		CommandMCPRegistrySmoke.Reset();
		CommandAgentConfigSmoke.Reset();
		CommandWorkbenchSmoke.Reset();
		CommandWelcomeSmoke.Reset();
		CommandDocsSmoke.Reset();
		CommandGenerateFiles.Reset();
		CommandSetUp.Reset();

		if (UToolMenus::IsToolMenuUIEnabled())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}

		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(HyperAIStudioTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(HyperAIStudioChatTabName);
		// Jobs hold pack callbacks and pending approvals hold prepared plans; neither may outlive the host.
		FHyperAIStudioAsyncJobHost::Shutdown();
		FHyperAIStudioApprovalGate::Clear();
		if (FoundationProbe)
		{
			FoundationProbe->Shutdown();
			FoundationProbe.Reset();
		}
		HyperAIStudio::TrustedExecution::Private::ShutdownCore();
		// Last: the spawners unregistered above held raw brush pointers into the style set.
		FHyperAIStudioStyle::Unregister();
	}

private:
	void LoadAvailableCapabilityModules()
	{
		struct FOptionalCapabilityModule
		{
			FName ModuleName;
			FString PackId;
			FString CohortId;
			TArray<FString> ToolNames;
			TArray<FString> RequiredPlugins;
		};
		const TArray<FOptionalCapabilityModule> Modules = {
			{
				TEXT("HyperAIStudioEnhancedInput"),
				TEXT("enhanced_input"),
				TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1"),
				{TEXT("hyper_input_inspect"), TEXT("hyper_input_apply_plan"), TEXT("hyper_input_validate")},
				{TEXT("EnhancedInput")}
			},
			{
				TEXT("HyperAIStudioGameplayAI"),
				TEXT("gameplay_ai"),
				TEXT("cohort.source.hyperaistudiogameplayaitoolset.v1"),
				{TEXT("hyper_gameplay_ai_inspect"), TEXT("hyper_gameplay_ai_apply_plan"),
					TEXT("hyper_gameplay_ai_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioGAS"),
				TEXT("gas"),
				TEXT("cohort.source.hyperaistudiogastoolset.v1"),
				{TEXT("hyper_gas_inspect"), TEXT("hyper_gas_apply_plan"), TEXT("hyper_gas_validate")},
				{TEXT("GameplayAbilities")}
			},
			{
				TEXT("HyperAIStudioAnimation"),
				TEXT("animation_rigging"),
				TEXT("cohort.source.hyperaistudioanimationtoolset.v1"),
				{TEXT("hyper_animation_inspect"), TEXT("hyper_animation_apply_plan"),
					TEXT("hyper_animation_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioWorldbuilding"),
				TEXT("worldbuilding_navigation"),
				TEXT("cohort.source.hyperaistudioworldbuildingtoolset.v1"),
				{TEXT("hyper_worldbuilding_inspect"), TEXT("hyper_worldbuilding_apply_plan"),
					TEXT("hyper_worldbuilding_validate"), TEXT("hyper_navigation_inspect"),
					TEXT("hyper_navigation_apply_plan"), TEXT("hyper_navigation_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioNetworking"),
				TEXT("networking_game_framework"),
				TEXT("cohort.source.hyperaistudionetworkingtoolset.v1"),
				{TEXT("hyper_network_inspect"), TEXT("hyper_network_apply_plan"),
					TEXT("hyper_network_validate"), TEXT("hyper_game_framework_inspect"),
					TEXT("hyper_game_framework_apply_plan"), TEXT("hyper_game_framework_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioAudio"),
				TEXT("audio_metasound"),
				TEXT("cohort.source.hyperaistudioaudiotoolset.v1"),
				{TEXT("hyper_audio_inspect"), TEXT("hyper_audio_apply_plan"),
					TEXT("hyper_audio_validate")},
				{TEXT("Metasound")}
			},
			{
				TEXT("HyperAIStudioMaterials"),
				TEXT("materials_dynamic_material"),
				TEXT("cohort.source.hyperaistudiomaterialstoolset.v1"),
				{TEXT("hyper_material_inspect"), TEXT("hyper_material_apply_plan"),
					TEXT("hyper_material_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioDynamicMaterial"),
				TEXT("materials_dynamic_material"),
				TEXT("cohort.source.hyperaistudiodynamicmaterialtoolset.v1"),
				{TEXT("hyper_dynamic_material_inspect"), TEXT("hyper_dynamic_material_apply_plan"),
					TEXT("hyper_dynamic_material_validate")},
				{TEXT("DynamicMaterial")}
			},
			{
				TEXT("HyperAIStudioNiagara"),
				TEXT("niagara_vfx"),
				TEXT("cohort.source.hyperaistudioniagaratoolset.v1"),
				{TEXT("hyper_niagara_inspect"), TEXT("hyper_niagara_apply_plan"),
					TEXT("hyper_niagara_validate")},
				{TEXT("Niagara")}
			},
			{
				TEXT("HyperAIStudioTextureGraph"),
				TEXT("texture_graph"),
				TEXT("cohort.source.hyperaistudiotexturegraphtoolset.v1"),
				{TEXT("hyper_texture_graph_inspect"), TEXT("hyper_texture_graph_apply_plan"),
					TEXT("hyper_texture_graph_validate")},
				{TEXT("TextureGraph")}
			},
			{
				TEXT("HyperAIStudioPCG"),
				TEXT("pcg"),
				TEXT("cohort.source.hyperaistudiopcgtoolset.v1"),
				{TEXT("hyper_pcg_inspect"), TEXT("hyper_pcg_apply_plan"),
					TEXT("hyper_pcg_validate")},
				{TEXT("PCG")}
			},
			{
				TEXT("HyperAIStudioUI"),
				TEXT("ui_slate_mvvm"),
				TEXT("cohort.source.hyperaistudiouitoolset.v1"),
				{TEXT("hyper_ui_inspect"), TEXT("hyper_ui_apply_plan"),
					TEXT("hyper_ui_validate")},
				{TEXT("ModelViewViewModel")}
			},
			{
				TEXT("HyperAIStudioData"),
				TEXT("data_config_localization"),
				TEXT("cohort.source.hyperaistudiodatatoolset.v1"),
				{TEXT("hyper_data_inspect"), TEXT("hyper_data_apply_plan"),
					TEXT("hyper_data_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioPhysics"),
				TEXT("physics_chaos"),
				TEXT("cohort.source.hyperaistudiophysicstoolset.v1"),
				{TEXT("hyper_physics_inspect"), TEXT("hyper_physics_apply_plan"),
					TEXT("hyper_physics_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioCinematics"),
				TEXT("cinematics"),
				TEXT("cohort.source.hyperaistudiocinematicstoolset.v1"),
				{TEXT("hyper_cinematics_inspect"), TEXT("hyper_cinematics_apply_plan"),
					TEXT("hyper_cinematics_validate")},
				{TEXT("MovieRenderPipeline")}
			},
			{
				TEXT("HyperAIStudioGeometry"),
				TEXT("geometry_interchange"),
				TEXT("cohort.source.hyperaistudiogeometrytoolset.v1"),
				{TEXT("hyper_geometry_inspect"), TEXT("hyper_geometry_apply_plan"),
					TEXT("hyper_geometry_validate")},
				{TEXT("GeometryScripting")}
			},
			{
				TEXT("HyperAIStudioInterchange"),
				TEXT("geometry_interchange"),
				TEXT("cohort.source.hyperaistudiointerchangetoolset.v1"),
				{TEXT("hyper_interchange_inspect"), TEXT("hyper_interchange_apply_plan"),
					TEXT("hyper_interchange_validate")},
				{TEXT("Interchange")}
			},
			{
				TEXT("HyperAIStudioLiveProduction"),
				TEXT("live_production"),
				TEXT("cohort.source.hyperaistudioliveproductiontoolset.v1"),
				{TEXT("hyper_live_production_inspect"), TEXT("hyper_live_production_apply_plan"),
					TEXT("hyper_live_production_validate")},
				{}
			},
			{
				TEXT("HyperAIStudioGameplaySystems"),
				TEXT("gameplay_systems"),
				TEXT("cohort.source.hyperaistudiogameplaysystemstoolset.v1"),
				{TEXT("hyper_gameplay_systems_inspect"), TEXT("hyper_gameplay_systems_apply_plan"),
					TEXT("hyper_gameplay_systems_validate")},
				{TEXT("MassAI"), TEXT("MassGameplay"), TEXT("GameFeatures"), TEXT("WorldConditions")}
			},
			{
				TEXT("HyperAIStudioCharacter"),
				TEXT("character"),
				TEXT("cohort.source.hyperaistudiocharactertoolset.v1"),
				{TEXT("hyper_character_inspect"), TEXT("hyper_character_apply_plan"),
					TEXT("hyper_character_validate")},
				{TEXT("MetaHumanGenerator")}
			},
			{
				TEXT("HyperAIStudioPaper2D"),
				TEXT("paper2d"),
				TEXT("cohort.source.hyperaistudiopaper2dtoolset.v1"),
				{TEXT("hyper_paper2d_inspect"), TEXT("hyper_paper2d_apply_plan"),
					TEXT("hyper_paper2d_validate")},
				{TEXT("Paper2D")}
			},
			{
				TEXT("HyperAIStudioActorModifier"),
				TEXT("actor_modifier_property_animation"),
				TEXT("cohort.source.hyperaistudioactormodifiertoolset.v1"),
				{TEXT("hyper_actor_modifier_inspect"), TEXT("hyper_actor_modifier_apply_plan"),
					TEXT("hyper_actor_modifier_validate")},
				{TEXT("ActorModifier")}
			},
			{
				TEXT("HyperAIStudioPropertyAnimation"),
				TEXT("actor_modifier_property_animation"),
				TEXT("cohort.source.hyperaistudiopropertyanimationtoolset.v1"),
				{TEXT("hyper_property_animation_inspect"), TEXT("hyper_property_animation_apply_plan"),
					TEXT("hyper_property_animation_validate")},
				{TEXT("PropertyAnimator")}
			},
			{
				TEXT("HyperAIStudioAutomation"),
				TEXT("automation_profiling_build"),
				TEXT("cohort.source.hyperaistudioautomationtoolset.v1"),
				{TEXT("hyper_test_inspect"), TEXT("hyper_test_run"),
					TEXT("hyper_test_validate"), TEXT("hyper_profile_capture"),
					TEXT("hyper_build_diagnose")},
				{}
			}
		};
		const bool bAllowSourceCandidate =
			FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
		for (const FOptionalCapabilityModule& Module : Modules)
		{
			FHyperAIStudioExtensionCohortAdmission Admission;
			if (!FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
					Module.PackId, Module.CohortId, Module.ToolNames, Admission)
				|| !Admission.bExactCohortMatch)
			{
				continue;
			}
			const bool bMayLoad = Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted
				|| (Admission.State == EHyperAIStudioExtensionAdmissionState::SourceCandidate
					&& bAllowSourceCandidate);
			if (!bMayLoad)
			{
				continue;
			}
			bool bPrerequisitesReady = true;
			for (const FString& PluginName : Module.RequiredPlugins)
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
				if (!Plugin.IsValid() || !Plugin->IsEnabled())
				{
					bPrerequisitesReady = false;
					break;
				}
			}
			if (!bPrerequisitesReady)
			{
				UE_LOG(LogHyperAIStudio, Display,
					TEXT("Optional capability module %s remains unavailable until its project plugin prerequisites are enabled."),
					*Module.ModuleName.ToString());
				continue;
			}
			if (!FModuleManager::Get().LoadModule(Module.ModuleName))
			{
				UE_LOG(LogHyperAIStudio, Error,
					TEXT("Admission-authorized optional capability module %s failed to load."),
					*Module.ModuleName.ToString());
			}
		}
	}

	void BeginEnginePreExit()
	{
		if (bModuleShuttingDown)
		{
			return;
		}
		bModuleShuttingDown = true;
#if WITH_DEV_AUTOMATION_TESTS
		HyperAIStudio::BenchmarkInstrumentation::Shutdown();
#endif
		HyperAIStudio::TrustedExecution::Private::BeginEnginePreExit();
		FHyperAIStudioPlanExecutionService::Shutdown();
		FHyperAIStudioPIEPlaytestService::Shutdown();
		if (WelcomePromptTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(WelcomePromptTickerHandle);
			WelcomePromptTickerHandle.Reset();
		}
	}

	void RequestSmokeExit() const
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			FPlatformMisc::RequestExit(false);
			return false;
		}), 0.25f);
	}

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("DockedTabLabel", "HyperAIStudio"))
			[
				SNew(SBox)
				.MinDesiredWidth(420.0f)
				.MinDesiredHeight(520.0f)
				[
					SNew(SHyperAIStudioWindow)
				]
			];
	}

	TSharedRef<SDockTab> CreateChatDockTab()
	{
		TSharedPtr<SHyperAIStudioQuickActionWindow> ChatWidget;
		TSharedRef<SDockTab> ChatTab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("DockedChatTabLabel", "HyperAI Chat"))
			[
				SNew(SBox)
				.MinDesiredWidth(420.0f)
				.MinDesiredHeight(520.0f)
				[
					SAssignNew(ChatWidget, SHyperAIStudioQuickActionWindow)
					.OnOpenWorkbench(FSimpleDelegate::CreateRaw(this, &FHyperAIStudioModule::OpenTab))
				]
			];
		ChatTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &FHyperAIStudioModule::OnChatTabClosed));
		ActiveChatTab = ChatTab;
		ActiveChatWidget = ChatWidget;
		return ChatTab;
	}

	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args)
	{
		return CreateChatDockTab();
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
		{
			FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("HyperAIStudio"));
			Section.AddMenuEntry(
				TEXT("OpenHyperAIStudio"),
				LOCTEXT("OpenHyperAIStudio", "HyperAIStudio"),
				LOCTEXT("OpenHyperAIStudioTooltip", "Open HyperAIStudio MCP setup and readiness checks."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::OpenTab)));
			Section.AddMenuEntry(
				TEXT("OpenHyperAIStudioChat"),
				LOCTEXT("OpenHyperAIStudioChat", "HyperAI Chat"),
				LOCTEXT("OpenHyperAIStudioChatTooltip", "Open the lightweight HyperAI chat panel."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::OpenChatTab)));
			Section.AddMenuEntry(
				TEXT("OpenHyperAIStudioQuickAction"),
				LOCTEXT("OpenHyperAIStudioQuickAction", "HyperAI Chat Overlay"),
				LOCTEXT("OpenHyperAIStudioQuickActionTooltip", "Open the lightweight chat overlay without changing the editor layout."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::OpenQuickActionWindowFromMenu)));
		}

		if (UToolMenu* StatusBarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar")))
		{
			FToolMenuSection& Section = StatusBarMenu->AddSection(
				TEXT("HyperAIStudioStatusBar"),
				FText::GetEmpty(),
				FToolMenuInsert(TEXT("SourceControl"), EToolMenuInsertType::Before));

			const TSharedRef<SWidget> StatusWidget =
				SNew(SButton)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("StatusBar.StatusBarButton")))
				.ContentPadding(FMargin(6.0f, 0.0f))
				.ToolTipText(LOCTEXT("StatusBarTooltip", "Open HyperAIStudio setup, agent handoff, and MCP readiness."))
				.OnClicked_Lambda([this]()
				{
					OpenTab();
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.ColorAndOpacity(FSlateColor::UseForeground())
						.Image(FAppStyle::Get().GetBrush(TEXT("Icons.Comment")))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(5.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("StatusBarLabel", "HyperAIStudio"))
						.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText")))
					]
				];

			FToolMenuEntry StatusBarEntry = FToolMenuEntry::InitWidget(
				TEXT("OpenHyperAIStudioStatus"),
				StatusWidget,
				FText::GetEmpty(),
				true,
				false);
			Section.AddEntry(StatusBarEntry);
		}

		RegisterContentBrowserContextMenu();
	}

	bool HasToolMenuEntries(const FName MenuName, const TArray<FName>& ExpectedEntries, int32& OutFoundCount, FString& OutMissing) const
	{
		OutFoundCount = 0;
		OutMissing.Reset();

		UToolMenus* ToolMenus = UToolMenus::Get();
		const UToolMenu* Menu = ToolMenus ? ToolMenus->FindMenu(MenuName) : nullptr;
		if (!Menu)
		{
			OutMissing = FString::Printf(TEXT("menu `%s` not found"), *MenuName.ToString());
			return false;
		}

		TSet<FName> EntryNames;
		for (const FToolMenuSection& Section : Menu->Sections)
		{
			for (const FToolMenuEntry& Entry : Section.Blocks)
			{
				EntryNames.Add(Entry.Name);
			}
		}

		TArray<FString> MissingEntries;
		for (const FName& ExpectedEntry : ExpectedEntries)
		{
			if (EntryNames.Contains(ExpectedEntry))
			{
				++OutFoundCount;
			}
			else
			{
				MissingEntries.Add(ExpectedEntry.ToString());
			}
		}

		if (MissingEntries.Num() > 0)
		{
			OutMissing = FString::Printf(TEXT("%s missing: %s"), *MenuName.ToString(), *FString::Join(MissingEntries, TEXT(", ")));
			return false;
		}

		return true;
	}

	bool HasToolMenuEntriesWithText(
		const FName MenuName,
		const TArray<FHyperAIStudioExpectedMenuEntry>& ExpectedEntries,
		int32& OutFoundCount,
		int32& OutTextOkCount,
		FString& OutMissing) const
	{
		OutFoundCount = 0;
		OutTextOkCount = 0;
		OutMissing.Reset();

		UToolMenus* ToolMenus = UToolMenus::Get();
		const UToolMenu* Menu = ToolMenus ? ToolMenus->FindMenu(MenuName) : nullptr;
		if (!Menu)
		{
			OutMissing = FString::Printf(TEXT("menu `%s` not found"), *MenuName.ToString());
			return false;
		}

		TArray<FString> MissingEntries;
		for (const FHyperAIStudioExpectedMenuEntry& ExpectedEntry : ExpectedEntries)
		{
			const FToolMenuEntry* FoundEntry = nullptr;
			for (const FToolMenuSection& Section : Menu->Sections)
			{
				for (const FToolMenuEntry& Entry : Section.Blocks)
				{
					if (Entry.Name == ExpectedEntry.Name)
					{
						FoundEntry = &Entry;
						break;
					}
				}

				if (FoundEntry)
				{
					break;
				}
			}

			if (!FoundEntry)
			{
				MissingEntries.Add(ExpectedEntry.Name.ToString());
				continue;
			}

			++OutFoundCount;
			const FString Label = FoundEntry->Label.Get(FText::GetEmpty()).ToString();
			const FString ToolTip = FoundEntry->ToolTip.Get(FText::GetEmpty()).ToString();
			const bool bLabelOk = Label == ExpectedEntry.Label;
			const bool bToolTipOk = ExpectedEntry.ToolTipContains.IsEmpty() || ToolTip.Contains(ExpectedEntry.ToolTipContains);
			const bool bInternalTextOk = !Label.Contains(TEXT("prepare prompt"), ESearchCase::IgnoreCase)
				&& !ToolTip.Contains(TEXT("prepare prompt"), ESearchCase::IgnoreCase);
			if (bLabelOk && bToolTipOk && bInternalTextOk)
			{
				++OutTextOkCount;
			}
			else
			{
				MissingEntries.Add(FString::Printf(TEXT("%s text label=`%s` tooltip=`%s`"),
					*ExpectedEntry.Name.ToString(),
					*Label,
					*ToolTip));
			}
		}

		if (MissingEntries.Num() > 0)
		{
			OutMissing = FString::Printf(TEXT("%s missing/text: %s"), *MenuName.ToString(), *FString::Join(MissingEntries, TEXT(", ")));
			return false;
		}

		return true;
	}

	bool HasNoToolMenuEntries(
		const FName MenuName,
		const TArray<FName>& UnexpectedEntries,
		int32& OutFoundCount,
		FString& OutUnexpected) const
	{
		OutFoundCount = 0;
		OutUnexpected.Reset();

		UToolMenus* ToolMenus = UToolMenus::Get();
		const UToolMenu* Menu = ToolMenus ? ToolMenus->FindMenu(MenuName) : nullptr;
		if (!Menu)
		{
			OutUnexpected = FString::Printf(TEXT("menu `%s` not found"), *MenuName.ToString());
			return false;
		}

		TSet<FName> EntryNames;
		for (const FToolMenuSection& Section : Menu->Sections)
		{
			for (const FToolMenuEntry& Entry : Section.Blocks)
			{
				EntryNames.Add(Entry.Name);
			}
		}

		TArray<FString> FoundEntries;
		for (const FName& UnexpectedEntry : UnexpectedEntries)
		{
			if (EntryNames.Contains(UnexpectedEntry))
			{
				++OutFoundCount;
				FoundEntries.Add(UnexpectedEntry.ToString());
			}
		}

		if (FoundEntries.Num() > 0)
		{
			OutUnexpected = FString::Printf(TEXT("%s unexpected entries: %s"), *MenuName.ToString(), *FString::Join(FoundEntries, TEXT(", ")));
			return false;
		}

		return true;
	}

	bool IsGraphEditorExtenderRegistered() const
	{
		if (!GraphEditorContextMenuExtenderHandle.IsValid() || !FModuleManager::Get().IsModuleLoaded(TEXT("GraphEditor")))
		{
			return false;
		}

		const FDelegateHandle Handle = GraphEditorContextMenuExtenderHandle;
		FGraphEditorModule& GraphEditorModule = FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
		const TArray<FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode>& Extenders = GraphEditorModule.GetAllGraphEditorContextMenuExtender();
		return Extenders.ContainsByPredicate([Handle](const FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode& Delegate)
		{
			return Delegate.GetHandle() == Handle;
		});
	}

	bool IsBuiltInContextAgentAvailable(const FString& AgentName) const
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (Settings && !Settings->IsBuiltInAgentEnabled(AgentName))
		{
			return false;
		}

		FString PrerequisiteId;
		if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
		{
			PrerequisiteId = TEXT("codex");
		}
		else if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
		{
			PrerequisiteId = TEXT("claude");
		}
		else if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
		{
			PrerequisiteId = TEXT("gemini");
		}
		else if (AgentName.Contains(TEXT("Cursor"), ESearchCase::IgnoreCase))
		{
			PrerequisiteId = TEXT("cursor");
		}
		else if (AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase)
			|| AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase))
		{
			PrerequisiteId = TEXT("vscode");
		}
		if (PrerequisiteId.IsEmpty())
		{
			return false;
		}

		FHyperAIStudioService Service;
		const TArray<FHyperAIStudioPrerequisiteStatus> Prerequisites = Service.GetPrerequisites();
		const FHyperAIStudioPrerequisiteStatus* Match = Prerequisites.FindByPredicate([&PrerequisiteId](const FHyperAIStudioPrerequisiteStatus& Entry)
		{
			return Entry.Id.Equals(PrerequisiteId, ESearchCase::IgnoreCase);
		});
		return Match && Match->bAvailable;
	}

	bool IsPreferredContextAgentAvailable() const
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		return Settings && IsBuiltInContextAgentAvailable(Settings->PreferredAgent);
	}

	bool ShouldShowBlueprintContextEntry(const FHyperAIStudioBlueprintMenuEntryDefinition& Entry) const
	{
		if (Entry.bQuickAutoComment)
		{
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			return Settings && Settings->bShowQuickAutoCommentInBlueprintMenu;
		}
		return true;
	}

	bool ShouldShowBuiltInAssetContextEntry(const FToolMenuContext& MenuContext, FString AgentName)
	{
		return IsHyperAIStudioContextAssetMenuSelection(MenuContext)
			&& IsBuiltInContextAgentAvailable(AgentName);
	}

	bool ShouldShowPreferredAssetContextEntry(const FToolMenuContext& MenuContext)
	{
		return IsHyperAIStudioContextAssetMenuSelection(MenuContext)
			&& IsPreferredContextAgentAvailable();
	}

	void ApplyBuiltInAgentVisibility(FToolMenuEntry& Entry, const FString& AgentName)
	{
		Entry.Visibility = [this, AgentName]()
		{
			return IsBuiltInContextAgentAvailable(AgentName);
		};
	}

	void ApplyPreferredAgentVisibility(FToolMenuEntry& Entry)
	{
		Entry.Visibility = [this]()
		{
			return IsPreferredContextAgentAvailable();
		};
	}

	void RegisterContextMenu(const FName MenuName)
	{
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(MenuName))
		{
			FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("HyperAIStudio"));
			FToolMenuEntry& OpenCodexEntry = Section.AddMenuEntry(
				TEXT("HyperAIStudioOpenCodexContext"),
				LOCTEXT("OpenCodexContext", "Open Codex With Selection"),
				LOCTEXT("OpenCodexContextTooltip", "Copy a prompt with the current selection and open Codex from the project root."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareContextForAgent, FString(TEXT("Codex")), true)));
			ApplyBuiltInAgentVisibility(OpenCodexEntry, TEXT("Codex"));

			FToolMenuEntry& OpenClaudeEntry = Section.AddMenuEntry(
				TEXT("HyperAIStudioOpenClaudeContext"),
				LOCTEXT("OpenClaudeContext", "Open Claude Code With Selection"),
				LOCTEXT("OpenClaudeContextTooltip", "Copy a prompt with the current selection and open Claude Code from the project root."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareContextForAgent, FString(TEXT("Claude Code")), true)));
			ApplyBuiltInAgentVisibility(OpenClaudeEntry, TEXT("Claude Code"));

			FToolMenuEntry& CopyCodexEntry = Section.AddMenuEntry(
				TEXT("HyperAIStudioCreateCodexTaskPack"),
				LOCTEXT("CreateCodexTaskPack", "Copy Codex Prompt"),
				LOCTEXT("CreateCodexTaskPackTooltip", "Copy a Codex-ready prompt with context for the current Unreal selection."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareContextForAgent, FString(TEXT("Codex")), false)));
			ApplyBuiltInAgentVisibility(CopyCodexEntry, TEXT("Codex"));

			FToolMenuEntry& CopyClaudeEntry = Section.AddMenuEntry(
				TEXT("HyperAIStudioCreateClaudeTaskPack"),
				LOCTEXT("CreateClaudeTaskPack", "Copy Claude Code Prompt"),
				LOCTEXT("CreateClaudeTaskPackTooltip", "Copy a Claude Code-ready prompt with context for the current Unreal selection."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareContextForAgent, FString(TEXT("Claude Code")), false)));
			ApplyBuiltInAgentVisibility(CopyClaudeEntry, TEXT("Claude Code"));

			FToolMenuEntry& PreferredEntry = Section.AddMenuEntry(
				TEXT("HyperAIStudioAddSelectionContext"),
				LOCTEXT("AddSelectionContext", "Copy Selection Prompt"),
				LOCTEXT("AddSelectionContextTooltip", "Copy a prompt for the preferred external agent with the current selection."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareContextForAgent, FString(), false)));
			ApplyPreferredAgentVisibility(PreferredEntry);
		}
	}

	void RegisterContentBrowserContextMenu()
	{
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu")))
		{
			FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("HyperAIStudio"));

			FToolUIAction OpenCodexAction(
				FToolMenuExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareAssetContextForAgentFromMenu, FString(TEXT("Codex")), true),
				FToolMenuCanExecuteAction(),
				FToolMenuGetActionCheckState());
			OpenCodexAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateRaw(this, &FHyperAIStudioModule::ShouldShowBuiltInAssetContextEntry, FString(TEXT("Codex")));
			Section.AddMenuEntry(
				TEXT("HyperAIStudioOpenCodexContext"),
				LOCTEXT("OpenCodexContext", "Open Codex With Selection"),
				LOCTEXT("OpenCodexContextTooltip", "Copy a prompt with the selected assets and open Codex from the project root."),
				FSlateIcon(),
				OpenCodexAction);

			FToolUIAction OpenClaudeAction(
				FToolMenuExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareAssetContextForAgentFromMenu, FString(TEXT("Claude Code")), true),
				FToolMenuCanExecuteAction(),
				FToolMenuGetActionCheckState());
			OpenClaudeAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateRaw(this, &FHyperAIStudioModule::ShouldShowBuiltInAssetContextEntry, FString(TEXT("Claude Code")));
			Section.AddMenuEntry(
				TEXT("HyperAIStudioOpenClaudeContext"),
				LOCTEXT("OpenClaudeContext", "Open Claude Code With Selection"),
				LOCTEXT("OpenClaudeContextTooltip", "Copy a prompt with the selected assets and open Claude Code from the project root."),
				FSlateIcon(),
				OpenClaudeAction);

			FToolUIAction CopyCodexAction(
				FToolMenuExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareAssetContextForAgentFromMenu, FString(TEXT("Codex")), false),
				FToolMenuCanExecuteAction(),
				FToolMenuGetActionCheckState());
			CopyCodexAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateRaw(this, &FHyperAIStudioModule::ShouldShowBuiltInAssetContextEntry, FString(TEXT("Codex")));
			Section.AddMenuEntry(
				TEXT("HyperAIStudioCreateCodexTaskPack"),
				LOCTEXT("CreateCodexTaskPack", "Copy Codex Prompt"),
				LOCTEXT("CreateCodexTaskPackTooltip", "Copy a Codex-ready prompt with context for the selected assets."),
				FSlateIcon(),
				CopyCodexAction);

			FToolUIAction CopyClaudeAction(
				FToolMenuExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareAssetContextForAgentFromMenu, FString(TEXT("Claude Code")), false),
				FToolMenuCanExecuteAction(),
				FToolMenuGetActionCheckState());
			CopyClaudeAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateRaw(this, &FHyperAIStudioModule::ShouldShowBuiltInAssetContextEntry, FString(TEXT("Claude Code")));
			Section.AddMenuEntry(
				TEXT("HyperAIStudioCreateClaudeTaskPack"),
				LOCTEXT("CreateClaudeTaskPack", "Copy Claude Code Prompt"),
				LOCTEXT("CreateClaudeTaskPackTooltip", "Copy a Claude Code-ready prompt with context for the selected assets."),
				FSlateIcon(),
				CopyClaudeAction);

			FToolUIAction PreferredAction(
				FToolMenuExecuteAction::CreateRaw(this, &FHyperAIStudioModule::PrepareAssetContextForAgentFromMenu, FString(), false),
				FToolMenuCanExecuteAction(),
				FToolMenuGetActionCheckState());
			PreferredAction.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateRaw(this, &FHyperAIStudioModule::ShouldShowPreferredAssetContextEntry);
			Section.AddMenuEntry(
				TEXT("HyperAIStudioAddSelectionContext"),
				LOCTEXT("AddSelectionContext", "Copy Selection Prompt"),
				LOCTEXT("AddSelectionContextTooltip", "Copy a prompt for the preferred external agent with the selected assets."),
				FSlateIcon(),
				PreferredAction);
		}
	}

	void RegisterGraphEditorContextMenus()
	{
		FGraphEditorModule& GraphEditorModule = FModuleManager::LoadModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
		FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode Delegate =
			FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode::CreateRaw(this, &FHyperAIStudioModule::ExtendBlueprintNodeContextMenu);
		GraphEditorContextMenuExtenderHandle = Delegate.GetHandle();
		GraphEditorModule.GetAllGraphEditorContextMenuExtender().Add(Delegate);
	}

	void UnregisterGraphEditorContextMenus()
	{
		if (!GraphEditorContextMenuExtenderHandle.IsValid() || !FModuleManager::Get().IsModuleLoaded(TEXT("GraphEditor")))
		{
			GraphEditorContextMenuExtenderHandle.Reset();
			return;
		}

		FGraphEditorModule& GraphEditorModule = FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
		TArray<FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode>& Extenders = GraphEditorModule.GetAllGraphEditorContextMenuExtender();
		const FDelegateHandle Handle = GraphEditorContextMenuExtenderHandle;
		Extenders.RemoveAll([Handle](const FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode& Delegate)
		{
			return Delegate.GetHandle() == Handle;
		});
		GraphEditorContextMenuExtenderHandle.Reset();
	}

	TSharedRef<FExtender> ExtendBlueprintNodeContextMenu(const TSharedRef<FUICommandList> CommandList, const UEdGraph* Graph, const UEdGraphNode* Node, const UEdGraphPin* Pin, bool bIsReadOnly)
	{
		TSharedRef<FExtender> Extender = MakeShared<FExtender>();
		if (bIsReadOnly || Pin || !Graph || !Node || !IsHyperAIStudioContextGraph(Graph))
		{
			return Extender;
		}

		Extender->AddMenuExtension(
			TEXT("EdGraphSchemaNodeActions"),
			EExtensionHook::After,
			CommandList,
			FMenuExtensionDelegate::CreateRaw(this, &FHyperAIStudioModule::AddBlueprintContextMenuEntries, Graph, Node));

		return Extender;
	}

	void AddBlueprintContextMenuEntries(FMenuBuilder& MenuBuilder, const UEdGraph* Graph, const UEdGraphNode* Node)
	{
		MenuBuilder.BeginSection(TEXT("HyperAIStudio"), LOCTEXT("BlueprintContextSection", "HyperAIStudio"));
		for (const FHyperAIStudioBlueprintMenuEntryDefinition& Entry : GetBlueprintMenuEntryDefinitions())
		{
			if (!ShouldShowBlueprintContextEntry(Entry))
			{
				continue;
			}

			const FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), Entry.IconName);
			if (Entry.bQuickAutoComment)
			{
				MenuBuilder.AddMenuEntry(
					Entry.Label,
					Entry.ToolTip,
					Icon,
					FUIAction(FExecuteAction::CreateRaw(this, &FHyperAIStudioModule::AutoCommentBlueprintContext, Graph, Node)));
				continue;
			}

			MenuBuilder.AddMenuEntry(
				Entry.Label,
				Entry.ToolTip,
				Icon,
				FUIAction(FExecuteAction::CreateRaw(
					this,
					&FHyperAIStudioModule::PrepareBlueprintContextForAgent,
					Entry.Id,
					Entry.AgentName,
					Graph,
					Node,
					Entry.Intent,
					Entry.bOpenTerminal)));
		}
		MenuBuilder.EndSection();
	}

	void OpenTab()
	{
		if (FApp::IsUnattended() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			if (const TSharedPtr<FTabManager> LevelEditorTabManager = LevelEditorModule.GetLevelEditorTabManager())
			{
				if (LevelEditorTabManager->TryInvokeTab(HyperAIStudioTabName).IsValid())
				{
					return;
				}
			}
		}

		FGlobalTabmanager::Get()->TryInvokeTab(HyperAIStudioTabName);
	}

	void OpenChatTab()
	{
		OpenChatTabForAgent(FString());
	}

	void OpenChatTabForAgent(const FString& AgentName)
	{
		if (FApp::IsUnattended() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		const FString TrimmedAgentName = AgentName.TrimStartAndEnd();
		if (!TrimmedAgentName.IsEmpty())
		{
			if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
			{
				Settings->PreferredAgent = TrimmedAgentName;
				Settings->SaveConfig();
			}
		}

		auto ActivateExistingChatTab = [this, &TrimmedAgentName](const TSharedPtr<SDockTab>& ExistingChatTab)
		{
			if (!ExistingChatTab.IsValid())
			{
				return false;
			}

			ActiveChatTab = ExistingChatTab;
			if (!TrimmedAgentName.IsEmpty())
			{
				if (const TSharedPtr<SHyperAIStudioQuickActionWindow> ExistingChatWidget = ActiveChatWidget.Pin())
				{
					ExistingChatWidget->SelectAgentByName(TrimmedAgentName);
				}
			}

			ExistingChatTab->ActivateInParent(ETabActivationCause::SetDirectly);
			return true;
		};

		if (ActivateExistingChatTab(ActiveChatTab.Pin()))
		{
			return;
		}

		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			if (const TSharedPtr<FTabManager> LevelEditorTabManager = LevelEditorModule.GetLevelEditorTabManager())
			{
				if (ActivateExistingChatTab(LevelEditorTabManager->FindExistingLiveTab(FTabId(HyperAIStudioChatTabName))))
				{
					return;
				}

				if (const TSharedPtr<SDockTab> InvokedChatTab = LevelEditorTabManager->TryInvokeTab(HyperAIStudioChatTabName))
				{
					ActiveChatTab = InvokedChatTab;
					ActivateExistingChatTab(InvokedChatTab);
					return;
				}
			}
		}

		if (const TSharedPtr<SDockTab> InvokedChatTab = FGlobalTabmanager::Get()->TryInvokeTab(HyperAIStudioChatTabName))
		{
			ActiveChatTab = InvokedChatTab;
			ActivateExistingChatTab(InvokedChatTab);
		}
	}

	void OnChatTabClosed(TSharedRef<SDockTab> ClosedTab)
	{
		if (ActiveChatTab.Pin() == ClosedTab)
		{
			ActiveChatTab.Reset();
			ActiveChatWidget.Reset();
		}
	}

	bool OpenQuickActionWindow()
	{
		if (FApp::IsUnattended() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		const TSharedRef<SWindow> QuickActionWindow = SNew(SWindow)
			.Title(LOCTEXT("QuickActionWindowTitle", "HyperAI Chat"))
			.ClientSize(FVector2D(640.0f, 430.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false);

		QuickActionWindow->SetContent(
			SNew(SHyperAIStudioQuickActionWindow)
			.OnOpenWorkbench(FSimpleDelegate::CreateRaw(this, &FHyperAIStudioModule::OpenTab)));

		FSlateApplication::Get().AddWindow(QuickActionWindow);
		return true;
	}

	void OpenQuickActionWindowFromMenu()
	{
		OpenQuickActionWindow();
	}

	FText GetWelcomeWindowTitleText() const
	{
		return LOCTEXT("WelcomeWindowTitle", "HyperAIStudio Setup");
	}

	FText GetWelcomeTitleText() const
	{
		return LOCTEXT("WelcomeTitle", "Welcome to HyperAIStudio");
	}

	FText GetWelcomeBodyText() const
	{
		return LOCTEXT("WelcomeBody", "HyperAIStudio is the Unreal-to-agent connection layer. It is not an in-editor replacement for Codex, Claude Code, Cursor, VS Code, or Gemini. Run setup once, verify Unreal MCP and agent configs, then continue the real chat in your external agent.");
	}

	FText GetWelcomeRouteHintText() const
	{
		return LOCTEXT("WelcomeRouteHint", "Intended workflow: compose a prompt or attach Unreal context in HyperAIStudio, then paste/open that handoff in the external agent from the project root.");
	}

	FText GetWelcomeStartSetupText() const
	{
		return LOCTEXT("WelcomeStartSetup", "Start Setup");
	}

	FText GetWelcomeStartSetupTooltipText() const
	{
		return LOCTEXT("WelcomeStartSetupTooltip", "Configure Unreal MCP, generate agent files, and open HyperAIStudio.");
	}

	FText GetWelcomeOpenPanelText() const
	{
		return LOCTEXT("WelcomeOpenPanel", "Open HyperAIStudio");
	}

	FText GetWelcomeOpenPanelTooltipText() const
	{
		return LOCTEXT("WelcomeOpenPanelTooltip", "Open the main HyperAIStudio workbench.");
	}

	FText GetWelcomeOpenChatText() const
	{
		return LOCTEXT("WelcomeOpenChat", "Open Chat / Terminal");
	}

	FText GetWelcomeOpenChatTooltipText() const
	{
		return LOCTEXT("WelcomeOpenChatTooltip", "Open the separate daily prompt and terminal handoff panel.");
	}

	FText GetWelcomeOpenDocsText() const
	{
		return LOCTEXT("WelcomeOpenDocs", "Open Docs");
	}

	FText GetWelcomeOpenDocsTooltipText() const
	{
		return LOCTEXT("WelcomeOpenDocsTooltip", "Open the HyperAIStudio documentation site.");
	}

	FText GetWelcomeDontShowText() const
	{
		return LOCTEXT("WelcomeDontShow", "Don't show again");
	}

	FText GetWelcomeDontShowTooltipText() const
	{
		return LOCTEXT("WelcomeDontShowTooltip", "Hide this startup prompt. You can still open HyperAIStudio from Tools or the status bar.");
	}

	FVector2D GetWelcomeWindowSize() const
	{
		return FVector2D(680.0f, 390.0f);
	}

	FVector2D GetWelcomeContentSize() const
	{
		return FVector2D(620.0f, 330.0f);
	}

	bool RunWelcomePromptTicker(float DeltaTime)
	{
		if (bModuleShuttingDown || FApp::IsUnattended() || IsRunningCommandlet())
		{
			return false;
		}

		if (!FSlateApplication::IsInitialized())
		{
			return true;
		}

		if (UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>();
			MutableSettings && MutableSettings->bResumeSetupAfterRestart)
		{
			TSharedRef<FHyperAIStudioService> ResumeService = MakeShared<FHyperAIStudioService>();
			FText ResumeMessage;
			const EHyperAIStudioSetupOutcome ResumeOutcome = ResumeService->SetUpHyperAIStudio(ResumeMessage);
			if (ResumeOutcome == EHyperAIStudioSetupOutcome::Complete)
			{
				TSharedRef<bool> bValidationHandled = MakeShared<bool>(false);
				ResumeService->RefreshStatusAsync(
					[this, ResumeService, ResumeMessage, bValidationHandled](const FHyperAIStudioStatus& ResumeStatus)
					{
						if (*bValidationHandled || ResumeStatus.bProbeInProgress)
						{
							return;
						}

						*bValidationHandled = true;
						if (ResumeStatus.IsReady())
						{
							UE_LOG(LogHyperAIStudio, Display, TEXT("Automatic post-restart setup completed and readiness was verified: %s"), *ResumeMessage.ToString());
							NotifyAction(LOCTEXT("PostRestartSetupNotification", "HyperAIStudio setup"),
								LOCTEXT("PostRestartSetupReady", "Setup finished: Unreal MCP and tools/list are ready."), true);
							return;
						}

						const FString WarningText = ResumeStatus.Warnings.IsEmpty()
							? ResumeStatus.ProbeMessage
							: FString::Join(ResumeStatus.Warnings, TEXT(" "));
						const FText AttentionMessage = FText::FromString(FString::Printf(
							TEXT("Setup resumed, but readiness still needs attention: %s"),
							WarningText.IsEmpty() ? TEXT("Open Setup for details.") : *WarningText));
						UE_LOG(LogHyperAIStudio, Warning, TEXT("Automatic post-restart setup did not verify readiness: %s"), *AttentionMessage.ToString());
						NotifyAction(LOCTEXT("PostRestartSetupNotification", "HyperAIStudio setup"), AttentionMessage, false);
						OpenTab();
					});
				return false;
			}
			if (ResumeOutcome == EHyperAIStudioSetupOutcome::RestartRequired)
			{
				NotifyAction(LOCTEXT("PostRestartSetupNotification", "HyperAIStudio setup"), ResumeMessage, true);
				OpenTab();
				return false;
			}

			UE_LOG(LogHyperAIStudio, Warning, TEXT("Automatic post-restart setup failed: %s"), *ResumeMessage.ToString());
			NotifyAction(LOCTEXT("PostRestartSetupNotification", "HyperAIStudio setup"), ResumeMessage, false);
			OpenTab();
			return false;
		}

		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (!Settings || !Settings->bShowWelcomeOnStartup)
		{
			return false;
		}

		if (bWelcomePromptProbeActive || bWelcomePromptShown)
		{
			return false;
		}

		bWelcomePromptProbeActive = true;
		TSharedRef<FHyperAIStudioService> StatusService = MakeShared<FHyperAIStudioService>();
		StatusService->RefreshStatusAsync([this, StatusService](const FHyperAIStudioStatus& StartupStatus)
		{
			if (StartupStatus.bProbeInProgress)
			{
				return;
			}

			AsyncTask(ENamedThreads::GameThread, [this, StartupStatus]()
			{
				bWelcomePromptProbeActive = false;
				if (bModuleShuttingDown
					|| bWelcomePromptShown
					|| FApp::IsUnattended()
					|| IsRunningCommandlet()
					|| !FSlateApplication::IsInitialized())
				{
					return;
				}

				const UHyperAIStudioSettings* CurrentSettings = GetDefault<UHyperAIStudioSettings>();
				if (!CurrentSettings || !CurrentSettings->bShowWelcomeOnStartup)
				{
					return;
				}

				ShowWelcomePrompt(StartupStatus);
			});
		});
		return false;
	}

	void DisableWelcomePrompt() const
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			return;
		}

		Settings->bShowWelcomeOnStartup = false;
		Settings->SaveConfig();
	}

	static bool ShouldShowWelcomeReadyActions(const FHyperAIStudioStatus& StartupStatus)
	{
		return StartupStatus.IsReady();
	}

	void ShowWelcomePrompt(const FHyperAIStudioStatus& StartupStatus)
	{
		bWelcomePromptShown = true;
		const bool bReady = ShouldShowWelcomeReadyActions(StartupStatus);
		const TSharedRef<SWindow> WelcomeWindow = SNew(SWindow)
			.Title(GetWelcomeWindowTitleText())
			.ClientSize(GetWelcomeWindowSize())
			.AutoCenter(EAutoCenter::PreferredWorkArea)
			.SizingRule(ESizingRule::FixedSize)
			.SupportsMaximize(false)
			.SupportsMinimize(false);
		const TWeakPtr<SWindow> WeakWelcomeWindow = WelcomeWindow;

		WelcomeWindow->SetContent(
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(GetWelcomeContentSize().X)
				.HeightOverride(GetWelcomeContentSize().Y)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
					.Padding(20.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(GetWelcomeTitleText())
							.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("DetailsView.CategoryTextStyle")))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
						[
							SNew(STextBlock)
							.Text(GetWelcomeBodyText())
							.AutoWrapText(true)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FMargin(0.0f, 14.0f, 0.0f, 0.0f))
						[
							SNew(STextBlock)
							.Text(GetWelcomeRouteHintText())
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor(FLinearColor(0.62f, 0.66f, 0.70f)))
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SBox)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SBox)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
							[
								SNew(SButton)
								.Text(GetWelcomeStartSetupText())
								.ToolTipText(GetWelcomeStartSetupTooltipText())
								.Visibility(bReady ? EVisibility::Collapsed : EVisibility::Visible)
								.OnClicked_Lambda([this, WeakWelcomeWindow]()
								{
									FHyperAIStudioService Service;
									FText Message;
									const EHyperAIStudioSetupOutcome Outcome = Service.SetUpHyperAIStudio(Message);
									const bool bOk = Outcome != EHyperAIStudioSetupOutcome::Failed;
									NotifyAction(LOCTEXT("WelcomeSetupNotification", "HyperAIStudio setup"), Message, bOk);
									if (const TSharedPtr<SWindow> PinnedWindow = WeakWelcomeWindow.Pin())
									{
										PinnedWindow->RequestDestroyWindow();
									}
									OpenTab();
									if (Outcome == EHyperAIStudioSetupOutcome::RestartRequired)
									{
										FUnrealEdMisc::Get().RestartEditor(true);
									}
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
							[
								SNew(SButton)
								.Text(GetWelcomeOpenPanelText())
								.ToolTipText(GetWelcomeOpenPanelTooltipText())
								.Visibility(bReady ? EVisibility::Visible : EVisibility::Collapsed)
								.OnClicked_Lambda([this, WeakWelcomeWindow]()
								{
									if (const TSharedPtr<SWindow> PinnedWindow = WeakWelcomeWindow.Pin())
									{
										PinnedWindow->RequestDestroyWindow();
									}
									OpenTab();
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
							[
								SNew(SButton)
								.Text(GetWelcomeOpenChatText())
								.ToolTipText(GetWelcomeOpenChatTooltipText())
								.Visibility(bReady ? EVisibility::Visible : EVisibility::Collapsed)
								.OnClicked_Lambda([this, WeakWelcomeWindow]()
								{
									if (const TSharedPtr<SWindow> PinnedWindow = WeakWelcomeWindow.Pin())
									{
										PinnedWindow->RequestDestroyWindow();
									}
									OpenChatTab();
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
							[
								SNew(SButton)
								.Text(GetWelcomeOpenDocsText())
								.ToolTipText(GetWelcomeOpenDocsTooltipText())
								.OnClicked_Lambda([]()
								{
									FHyperAIStudioService Service;
									FText Message;
									if (!Service.OpenIncludedDocs(Message))
									{
										FPlatformProcess::LaunchURL(*FHyperAIStudioService::GetDocumentationUrl(), nullptr, nullptr);
									}
									return FReply::Handled();
								})
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SCheckBox)
								.Visibility(bReady ? EVisibility::Visible : EVisibility::Collapsed)
								.ToolTipText(GetWelcomeDontShowTooltipText())
								.OnCheckStateChanged_Lambda([this, WeakWelcomeWindow](ECheckBoxState State)
								{
									if (State == ECheckBoxState::Checked)
									{
										DisableWelcomePrompt();
									}
									if (const TSharedPtr<SWindow> PinnedWindow = WeakWelcomeWindow.Pin())
									{
										PinnedWindow->RequestDestroyWindow();
									}
								})
								[
									SNew(STextBlock).Text(GetWelcomeDontShowText())
								]
							]
						]
					]
				]
			]);

		FSlateApplication::Get().AddWindow(WelcomeWindow);
	}

	void NotifyAction(const FText& Title, const FText& Message, bool bSuccess) const
	{
		FNotificationInfo Info(Message);
		Info.SubText = Title;
		Info.ExpireDuration = bSuccess ? 4.0f : 8.0f;
		Info.bUseSuccessFailIcons = true;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	void PrepareContextForAgent(FString AgentName, bool bOpenTerminal)
	{
		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		bool bOk = Service.CreatePromptPackage(
			TEXT("Use the current Unreal selection as context. Inspect what is selected, then wait for the user's next concrete instruction."),
			AgentName,
			Package,
			Message);
		if (bOk && bOpenTerminal)
		{
			bOk = Service.OpenTerminalForPrompt(Package, Message);
		}
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio context handoff for %s %s: %s"),
			AgentName.IsEmpty() ? TEXT("preferred agent") : *AgentName,
			bOk ? TEXT("succeeded") : TEXT("failed"),
			*Message.ToString());
		NotifyAction(
			bOpenTerminal ? LOCTEXT("ContextOpenNotification", "HyperAIStudio sent selection context") : LOCTEXT("ContextPackNotification", "HyperAIStudio copied a selection prompt"),
			Message,
			bOk);
		OpenTab();
	}

	void PrepareAssetContextForAgentFromMenu(const FToolMenuContext& MenuContext, FString AgentName, bool bOpenTerminal)
	{
		const UContentBrowserAssetContextMenuContext* AssetContext = MenuContext.FindContext<UContentBrowserAssetContextMenuContext>();
		if (!AssetContext || AssetContext->SelectedAssets.IsEmpty())
		{
			PrepareContextForAgent(AgentName, bOpenTerminal);
			return;
		}

		PrepareAssetContextForAgent(AgentName, bOpenTerminal, AssetContext->SelectedAssets);
	}

	void PrepareAssetContextForAgent(FString AgentName, bool bOpenTerminal, const TArray<FAssetData>& SelectedAssets)
	{
		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		bool bOk = Service.CreatePromptPackageForAssets(
			TEXT("Use the selected Content Browser assets as context. Identify what they are, how they are referenced, and one safe improvement path before making changes."),
			AgentName,
			SelectedAssets,
			Package,
			Message);
		if (bOk && bOpenTerminal)
		{
			bOk = Service.OpenTerminalForPrompt(Package, Message);
		}
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio asset handoff for %s assets=%d %s: %s"),
			AgentName.IsEmpty() ? TEXT("preferred agent") : *AgentName,
			SelectedAssets.Num(),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			*Message.ToString());
		NotifyAction(
			bOpenTerminal ? LOCTEXT("AssetContextOpenNotification", "HyperAIStudio sent asset context") : LOCTEXT("AssetContextPackNotification", "HyperAIStudio copied an asset prompt"),
			Message,
			bOk);
		OpenTab();
	}

	void PrepareBlueprintContextForAgent(FName ActionId, FString AgentName, const UEdGraph* Graph, const UEdGraphNode* Node, FString Intent, bool bOpenTerminal)
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		const bool bCopyContextAction = ActionId == TEXT("CopyBlueprintNodeContext");
		if (bCopyContextAction && Settings && !Settings->bAllowClipboardPromptCopy)
		{
			NotifyAction(
				LOCTEXT("BlueprintContextCopyDisabledTitle", "Selected node context was not copied"),
				LOCTEXT("BlueprintContextCopyDisabledMessage", "Clipboard copying is disabled. Enable it in HyperAIStudio under Files And Permissions, then try again."),
				false);
			return;
		}

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		bool bOk = Service.CreateBlueprintPromptPackage(Intent, AgentName, Graph, Node, Package, Message);
		if (bOk && bOpenTerminal)
		{
			bOk = Service.OpenTerminalForPrompt(Package, Message);
		}
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio Blueprint handoff for %s %s: %s"),
			AgentName.IsEmpty() ? TEXT("preferred agent") : *AgentName,
			bOk ? TEXT("succeeded") : TEXT("failed"),
			*Message.ToString());

		if (!bOk)
		{
			NotifyAction(
				bOpenTerminal ? LOCTEXT("SmartAutoCommentFailedTitle", "Smart Auto Comment could not start") : LOCTEXT("BlueprintContextCopyFailedTitle", "Selected node context could not be copied"),
				Message,
				false);
			return;
		}

		if (bCopyContextAction)
		{
			NotifyAction(
				LOCTEXT("BlueprintContextCopiedTitle", "Selected node context copied"),
				LOCTEXT("BlueprintContextCopiedMessage", "Blueprint node context copied successfully. Paste it into your terminal or coding agent."),
				true);
			return;
		}

		const bool bClipboardCopied = !Settings || Settings->bAllowClipboardPromptCopy;
		const FString AgentDisplayName = Package.AgentName.IsEmpty() ? FString(TEXT("your coding agent")) : Package.AgentName;
		const FText SmartMessage = FText::FromString(bClipboardCopied
			? FString::Printf(TEXT("Smart Auto Comment is ready in %s. The prompt was copied; paste it there if it was not sent automatically."), *AgentDisplayName)
			: FString::Printf(TEXT("Smart Auto Comment is ready in %s. Continue there to let AI understand and comment the selected nodes."), *AgentDisplayName));
		NotifyAction(LOCTEXT("SmartAutoCommentReadyTitle", "Smart Auto Comment ready"), SmartMessage, true);
	}

	void AutoCommentBlueprintContext(const UEdGraph* Graph, const UEdGraphNode* Node)
	{
		FHyperAIStudioService Service;
		FText Message;
		const bool bOk = Service.AutoCommentBlueprintNodes(Graph, Node, Message);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio Quick Auto Comment Selected Nodes %s: %s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			*Message.ToString());
		NotifyAction(
			bOk ? LOCTEXT("QuickAutoCommentNotification", "Quick comment added") : LOCTEXT("QuickAutoCommentFailedNotification", "Quick comment could not be added"),
			Message,
			bOk);
	}

	void RunConsoleSetUp()
	{
		FHyperAIStudioService Service;
		FText Message;
		const EHyperAIStudioSetupOutcome Outcome = Service.SetUpHyperAIStudio(Message);
		const bool bOk = Outcome != EHyperAIStudioSetupOutcome::Failed;
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.SetUp %s: %s"), bOk ? TEXT("succeeded") : TEXT("failed"), *Message.ToString());
	}

	void RunConsoleGenerateFiles()
	{
		FHyperAIStudioService Service;
		FText Message;
		const bool bOk = Service.GenerateProjectFiles(Message);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.GenerateProjectFiles %s: %s"), bOk ? TEXT("succeeded") : TEXT("failed"), *Message.ToString());
	}

	void RunConsoleDocsSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		const bool bPluginOk = Plugin.IsValid();
		const FString DocsPath = bPluginOk
			? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Docs"), TEXT("HyperAIStudio.html"))
			: FString();
		const bool bDocsExists = !DocsPath.IsEmpty() && FPaths::FileExists(DocsPath);

		FString DocsText;
		const bool bDocsRead = bDocsExists && FFileHelper::LoadFileToString(DocsText, *DocsPath);
		const bool bFirstRunOk = DocsText.Contains(TEXT("First Run"));
		const bool bGeneratedOk = DocsText.Contains(TEXT("What HyperAIStudio Generates"))
			&& DocsText.Contains(TEXT("Shared Project Workspace"))
			&& DocsText.Contains(TEXT(".hyperai/work/YYYY-MM-DD-&lt;clear-context-name&gt;/"));
		const bool bDailyOk = DocsText.Contains(TEXT("Daily Use"));
		const bool bContextMenusOk = DocsText.Contains(TEXT("From editor context menus"));
		const bool bPrerequisitesOk = DocsText.Contains(TEXT("Prerequisites"));
		const bool bMultiMcpOk = DocsText.Contains(TEXT("MCP Profiles"))
			&& DocsText.Contains(TEXT("single execution server"))
			&& DocsText.Contains(TEXT("Toolset Registry"))
			&& DocsText.Contains(TEXT("MCP</code> tab"));
		const bool bProvidersOk = !DocsText.Contains(TEXT("Model Providers"))
			&& !DocsText.Contains(TEXT("Add OpenAI Provider"))
			&& !DocsText.Contains(TEXT("Copy Codex Provider Patch"))
			&& !DocsText.Contains(TEXT("Optional Provider Integrations"))
			&& !DocsText.Contains(TEXT("Provider Templates"));
		const bool bNoBridgeOk = DocsText.Contains(TEXT("does not provide licensing, accounts, cloud services, an execution bridge"))
			&& DocsText.Contains(TEXT("retired Hyper UE TCP/stdio/HTTP runtime is never started or used as fallback"))
			&& !DocsText.Contains(TEXT("<h2>Automation Bridge</h2>"))
			&& !DocsText.Contains(TEXT("534-tool MCP runtime"))
			&& !DocsText.Contains(TEXT("127.0.0.1:55557"))
			&& !DocsText.Contains(TEXT("127.0.0.1:8765"));
		const bool bSafetyOk = DocsText.Contains(TEXT("Safety And Limits"));
		const bool bTroubleshootingOk = DocsText.Contains(TEXT("Troubleshooting"));
		const bool bAgentActionsOk = DocsText.Contains(TEXT("Use the HyperAIStudio workbench tabs for setup"))
			&& DocsText.Contains(TEXT("HyperAI Chat"))
			&& DocsText.Contains(TEXT("embedded terminal"))
			&& DocsText.Contains(TEXT("Stop Agent"))
			&& DocsText.Contains(TEXT("Copy Prompt"))
			&& DocsText.Contains(TEXT("does not interpret terminal output"))
			&& DocsText.Contains(TEXT("own agent approvals/history"))
			&& DocsText.Contains(TEXT("Setup</code>, <code>Agents</code>, <code>MCP</code>, and <code>Docs"));
		const bool bOk = bPluginOk
			&& bDocsExists
			&& bDocsRead
			&& bFirstRunOk
			&& bGeneratedOk
			&& bDailyOk
			&& bContextMenusOk
			&& bPrerequisitesOk
			&& bMultiMcpOk
			&& bProvidersOk
			&& bNoBridgeOk
			&& bSafetyOk
			&& bTroubleshootingOk
			&& bAgentActionsOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.DocsSmoke %s path=%s plugin=%s docs=%s read=%s firstRun=%s generated=%s daily=%s contextMenus=%s prerequisites=%s multiMcp=%s providers=%s noBridge=%s safety=%s troubleshooting=%s agentActions=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			DocsPath.IsEmpty() ? TEXT("<missing>") : *DocsPath,
			bPluginOk ? TEXT("true") : TEXT("false"),
			bDocsExists ? TEXT("true") : TEXT("false"),
			bDocsRead ? TEXT("true") : TEXT("false"),
			bFirstRunOk ? TEXT("true") : TEXT("false"),
			bGeneratedOk ? TEXT("true") : TEXT("false"),
			bDailyOk ? TEXT("true") : TEXT("false"),
			bContextMenusOk ? TEXT("true") : TEXT("false"),
			bPrerequisitesOk ? TEXT("true") : TEXT("false"),
			bMultiMcpOk ? TEXT("true") : TEXT("false"),
			bProvidersOk ? TEXT("true") : TEXT("false"),
			bNoBridgeOk ? TEXT("true") : TEXT("false"),
			bSafetyOk ? TEXT("true") : TEXT("false"),
			bTroubleshootingOk ? TEXT("true") : TEXT("false"),
			bAgentActionsOk ? TEXT("true") : TEXT("false"));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleWelcomeSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bSettingsOk = Settings != nullptr;
		const bool bOriginalShowWelcome = Settings ? Settings->bShowWelcomeOnStartup : false;

		bool bShowSettingOk = false;
		bool bDontShowOk = false;
		bool bRestoredOk = false;
		if (Settings)
		{
			Settings->bShowWelcomeOnStartup = true;
			Settings->SaveConfig();
			bShowSettingOk = Settings->bShowWelcomeOnStartup;

			DisableWelcomePrompt();
			bDontShowOk = !Settings->bShowWelcomeOnStartup;

			Settings->bShowWelcomeOnStartup = bOriginalShowWelcome;
			Settings->SaveConfig();
			bRestoredOk = Settings->bShowWelcomeOnStartup == bOriginalShowWelcome;
		}

		const bool bUnattended = FApp::IsUnattended() || IsRunningCommandlet();
		const bool bUnattendedSuppressed = bUnattended ? !RunWelcomePromptTicker(0.0f) : true;

		FHyperAIStudioStatus SyntheticSetupStatus;
		const bool bSetupRouteOk = SHyperAIStudioWindow::GetInitialTabIndexForStatus(SyntheticSetupStatus) == 0;

		FHyperAIStudioStatus SyntheticReadyStatus;
		SyntheticReadyStatus.bUnrealMCPModuleAvailable = true;
		SyntheticReadyStatus.bUnrealMCPSettingsConfigured = true;
		SyntheticReadyStatus.bRequiredPluginsReady = true;
		SyntheticReadyStatus.bDesiredCapabilityPluginsReady = true;
		SyntheticReadyStatus.bServerRunning = true;
		SyntheticReadyStatus.bPortListening = true;
		SyntheticReadyStatus.bToolsListReachable = true;
		SyntheticReadyStatus.bAgentFilesReady = true;
		SyntheticReadyStatus.ConfiguredAgentCount = 1;
		const bool bAgentRouteOk = SHyperAIStudioWindow::GetInitialTabIndexForStatus(SyntheticReadyStatus) == 1;
		const bool bWelcomeSetupVisibilityOk = !ShouldShowWelcomeReadyActions(SyntheticSetupStatus);
		const bool bWelcomeReadyVisibilityOk = ShouldShowWelcomeReadyActions(SyntheticReadyStatus);
		const bool bPostSetupRouteOk = SHyperAIStudioWindow::GetPostRefreshTabIndex(0, SyntheticSetupStatus, false, true) == 0;
		const bool bPostReadyRouteOk = SHyperAIStudioWindow::GetPostRefreshTabIndex(0, SyntheticReadyStatus, false, true) == 1;
		const bool bPostManualRouteOk = SHyperAIStudioWindow::GetPostRefreshTabIndex(0, SyntheticReadyStatus, true, true) == 0;

		FHyperAIStudioService Service;
		const FHyperAIStudioStatus CurrentStatus = Service.GetStatusSync();
		const int32 CurrentInitialTab = SHyperAIStudioWindow::GetInitialTabIndexForStatus(CurrentStatus);
		const bool bCurrentRouteOk = CurrentInitialTab == (CurrentStatus.IsReady() ? 1 : 0);

		const FString WelcomeBody = GetWelcomeBodyText().ToString();
		const FString RouteHint = GetWelcomeRouteHintText().ToString();
		const bool bWelcomeCopyOk = GetWelcomeTitleText().ToString().Equals(TEXT("Welcome to HyperAIStudio"))
			&& WelcomeBody.Contains(TEXT("Unreal-to-agent connection layer"))
			&& WelcomeBody.Contains(TEXT("not an in-editor replacement"))
			&& WelcomeBody.Contains(TEXT("Run setup once"))
			&& WelcomeBody.Contains(TEXT("Unreal MCP"))
			&& WelcomeBody.Contains(TEXT("agent configs"))
			&& WelcomeBody.Contains(TEXT("Codex"))
			&& WelcomeBody.Contains(TEXT("Claude Code"))
			&& WelcomeBody.Contains(TEXT("Gemini"))
			&& WelcomeBody.Contains(TEXT("Cursor"))
			&& WelcomeBody.Contains(TEXT("VS Code"))
			&& RouteHint.Contains(TEXT("Intended workflow"))
			&& RouteHint.Contains(TEXT("external agent"));
		const bool bWelcomeActionsOk = GetWelcomeStartSetupText().ToString().Equals(TEXT("Start Setup"))
			&& GetWelcomeOpenPanelText().ToString().Equals(TEXT("Open HyperAIStudio"))
			&& GetWelcomeOpenChatText().ToString().Equals(TEXT("Open Chat / Terminal"))
			&& GetWelcomeOpenDocsText().ToString().Equals(TEXT("Open Docs"))
			&& GetWelcomeDontShowText().ToString().Equals(TEXT("Don't show again"))
			&& GetWelcomeStartSetupTooltipText().ToString().Contains(TEXT("Configure Unreal MCP"))
			&& GetWelcomeOpenPanelTooltipText().ToString().Contains(TEXT("workbench"))
			&& GetWelcomeOpenChatTooltipText().ToString().Contains(TEXT("prompt and terminal"))
			&& GetWelcomeOpenDocsTooltipText().ToString().Contains(TEXT("documentation site"))
			&& GetWelcomeDontShowTooltipText().ToString().Contains(TEXT("Tools or the status bar"));
		const FVector2D WelcomeWindowSize = GetWelcomeWindowSize();
		const FVector2D WelcomeContentSize = GetWelcomeContentSize();
		const bool bWelcomeLayoutOk = WelcomeWindowSize.X >= 640.0f
			&& WelcomeWindowSize.Y >= 360.0f
			&& WelcomeContentSize.X > 0.0f
			&& WelcomeContentSize.Y > 0.0f
			&& WelcomeContentSize.X < WelcomeWindowSize.X
			&& WelcomeContentSize.Y < WelcomeWindowSize.Y;

		const FString DocsUrl = FHyperAIStudioService::GetDocumentationUrl();
		const bool bDocsActionOk = DocsUrl.Equals(TEXT("https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/"), ESearchCase::IgnoreCase);

		const bool bOk = bSettingsOk
			&& bShowSettingOk
			&& bDontShowOk
			&& bRestoredOk
			&& bUnattendedSuppressed
			&& bSetupRouteOk
			&& bAgentRouteOk
			&& bWelcomeSetupVisibilityOk
			&& bWelcomeReadyVisibilityOk
			&& bPostSetupRouteOk
			&& bPostReadyRouteOk
			&& bPostManualRouteOk
			&& bCurrentRouteOk
			&& bWelcomeCopyOk
			&& bWelcomeActionsOk
			&& bWelcomeLayoutOk
			&& bDocsActionOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.WelcomeSmoke %s settings=%s showSetting=%s dontShow=%s restored=%s unattended=%s unattendedSuppressed=%s setupRoute=%s agentRoute=%s welcomeSetup=%s welcomeReady=%s postSetup=%s postReady=%s postManual=%s currentReady=%s currentInitialTab=%d currentRoute=%s copy=%s actions=%s layout=%s docsAction=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bSettingsOk ? TEXT("true") : TEXT("false"),
			bShowSettingOk ? TEXT("true") : TEXT("false"),
			bDontShowOk ? TEXT("true") : TEXT("false"),
			bRestoredOk ? TEXT("true") : TEXT("false"),
			bUnattended ? TEXT("true") : TEXT("false"),
			bUnattendedSuppressed ? TEXT("true") : TEXT("false"),
			bSetupRouteOk ? TEXT("true") : TEXT("false"),
			bAgentRouteOk ? TEXT("true") : TEXT("false"),
			bWelcomeSetupVisibilityOk ? TEXT("true") : TEXT("false"),
			bWelcomeReadyVisibilityOk ? TEXT("true") : TEXT("false"),
			bPostSetupRouteOk ? TEXT("true") : TEXT("false"),
			bPostReadyRouteOk ? TEXT("true") : TEXT("false"),
			bPostManualRouteOk ? TEXT("true") : TEXT("false"),
			CurrentStatus.IsReady() ? TEXT("true") : TEXT("false"),
			CurrentInitialTab,
			bCurrentRouteOk ? TEXT("true") : TEXT("false"),
			bWelcomeCopyOk ? TEXT("true") : TEXT("false"),
			bWelcomeActionsOk ? TEXT("true") : TEXT("false"),
			bWelcomeLayoutOk ? TEXT("true") : TEXT("false"),
			bDocsActionOk ? TEXT("true") : TEXT("false"));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleWorkbenchSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const FHyperAIStudioWorkbenchSurfaceSmoke Surface = SHyperAIStudioWindow::GetWorkbenchSurfaceSmoke();
		TArray<FString> Problems;
		const auto CountExpected = [&Problems](const TCHAR* GroupName, const TArray<FString>& Values, const TArray<FString>& ExpectedValues)
		{
			int32 Count = 0;
			for (const FString& ExpectedValue : ExpectedValues)
			{
				if (Values.Contains(ExpectedValue))
				{
					++Count;
				}
				else
				{
					Problems.Add(FString::Printf(TEXT("%s missing `%s`"), GroupName, *ExpectedValue));
				}
			}
			return Count;
		};

		const TArray<FString> ExpectedTabs = {
			TEXT("Setup"),
			TEXT("Agents"),
			TEXT("MCP"),
			TEXT("Docs")
		};
		const TArray<FString> ExpectedFirstRun = {
			TEXT("HyperAIStudio"),
			TEXT("Start Setup"),
			TEXT("Verify"),
			TEXT("Sync Toolsets"),
			TEXT("Configure Agents"),
			TEXT("View Prerequisites"),
			TEXT("Open Chat / Terminal"),
			TEXT("Agent Files"),
			TEXT("Open")
		};
		const TArray<FString> ExpectedTargets = {
			TEXT("Enabled"),
			TEXT("Use in Chat"),
			TEXT("Copy Codex Prompt"),
			TEXT("Copy Claude Prompt"),
			TEXT("Copy Gemini Prompt"),
			TEXT("Copy Cursor Prompt"),
			TEXT("Copy VS Code Prompt"),
			TEXT("Generate Config"),
			TEXT("Files"),
			TEXT("Install")
		};
		const TArray<FString> ExpectedHandoff = {
			TEXT("HyperAI Chat"),
			TEXT("Ready"),
			TEXT("Needs setup"),
			TEXT("Agent"),
			TEXT("Model"),
			TEXT("Context"),
			TEXT("Copy Prompt"),
			TEXT("Stop Agent"),
			TEXT("Open Setup")
		};
		const TArray<FString> ExpectedTests = {};
		const TArray<FString> ExpectedAdvanced = {
			TEXT("Prerequisites"),
			TEXT("Agent Files"),
			TEXT("MCP Tools")
		};
		const TArray<FString> ExpectedConfigs = {
			TEXT("AGENTS.md"),
			TEXT(".codex/config.toml"),
			TEXT(".mcp.json"),
			TEXT(".cursor/mcp.json"),
			TEXT(".vscode/mcp.json"),
			TEXT(".gemini/settings.json")
		};
		const TArray<FString> ExpectedMCP = {
			TEXT("Unreal MCP"),
			TEXT("Extended Hyper Tools"),
			TEXT("Primary"),
			TEXT("Tools"),
			TEXT("Epic + 109 Hyper"),
			TEXT("3 discovery dispatchers"),
			TEXT("Optional UE 5.8 source catalog"),
			TEXT("Search tool name, description, or toolset"),
			TEXT("Unreal MCP + Extended Hyper Tools"),
			TEXT("Unreal MCP Only"),
			TEXT("Fast"),
			TEXT("Strict Safety"),
			TEXT("Add"),
			TEXT("Import Profile"),
			TEXT("Copy URL"),
			TEXT("Edit"),
			TEXT("Test")
		};
		const int32 TabsOk = CountExpected(TEXT("tabs"), Surface.NavigationTabs, ExpectedTabs);
		const int32 FirstRunOk = CountExpected(TEXT("firstRun"), Surface.FirstRunLabels, ExpectedFirstRun);
		const int32 TargetsOk = CountExpected(TEXT("agentTargets"), Surface.AgentTargetActions, ExpectedTargets);
		const int32 HandoffOk = CountExpected(TEXT("handoff"), Surface.HandoffActions, ExpectedHandoff);
		const int32 TestsOk = CountExpected(TEXT("agentTests"), Surface.AgentTestActions, ExpectedTests);
		const int32 AdvancedOk = CountExpected(TEXT("advanced"), Surface.AdvancedLabels, ExpectedAdvanced);
		const int32 ConfigsOk = CountExpected(TEXT("configs"), Surface.AgentConfigLabels, ExpectedConfigs);
		const int32 MCPOk = CountExpected(TEXT("mcp"), Surface.MCPServerActions, ExpectedMCP);

		const bool bTopTabsOk = Surface.NavigationTabs.Num() == ExpectedTabs.Num()
			&& Surface.NavigationTabs.Contains(TEXT("Setup"))
			&& Surface.NavigationTabs.Contains(TEXT("Agents"))
			&& Surface.NavigationTabs.Contains(TEXT("MCP"))
			&& Surface.NavigationTabs.Contains(TEXT("Docs"))
			&& !Surface.NavigationTabs.Contains(TEXT("Terminal"))
			&& !Surface.NavigationTabs.Contains(TEXT("Status"))
			&& !Surface.NavigationTabs.Contains(TEXT("Tests"))
			&& !Surface.NavigationTabs.Contains(TEXT("MCP Servers"));

		const FString TerminalHint = SHyperAIStudioWindow::GetTerminalPanelHintText().ToString();
		const FString TerminalRecipe = SHyperAIStudioWindow::GetTerminalStartupRecipeText(TEXT("D:/HyperAIStudioSmokeProject"));
		const bool bTerminalFallbackOk = TerminalHint.Contains(TEXT("enable the Terminal plugin"))
			&& TerminalHint.Contains(TEXT("Copy Startup Commands"))
			&& TerminalHint.Contains(TEXT("Windows Terminal"))
			&& TerminalRecipe.Contains(TEXT("TERM=xterm-256color"))
			&& TerminalRecipe.Contains(TEXT("codex"))
			&& TerminalRecipe.Contains(TEXT("claude"))
			&& TerminalRecipe.Contains(TEXT("gemini"))
			&& TerminalRecipe.Contains(TEXT("cursor ."))
			&& TerminalRecipe.Contains(TEXT("code ."));
		const bool bDockableWorkbenchOk = FGlobalTabmanager::Get()->HasTabSpawner(HyperAIStudioTabName)
			&& FGlobalTabmanager::Get()->HasTabSpawner(HyperAIStudioChatTabName);
		FHyperAIStudioMCPServerEntry ExactRetiredEntry;
		ExactRetiredEntry.Id = TEXT("hyper-ue-mcp");
		ExactRetiredEntry.DisplayName = TEXT("Retired built-in profile");
		ExactRetiredEntry.Transport = EHyperAIStudioMCPTransport::Command;
		ExactRetiredEntry.Command = TEXT("powershell");
		ExactRetiredEntry.Arguments = {
			TEXT("-NoProfile"), TEXT("-ExecutionPolicy"), TEXT("Bypass"), TEXT("-File"),
			TEXT("D:\\Plugin\\Resources\\MCPServers\\HyperUEMCP\\Start-HyperUEMCP.ps1")};
		FHyperAIStudioMCPServerEntry CustomizedReservedEntry = ExactRetiredEntry;
		CustomizedReservedEntry.DisplayName = TEXT("User Custom MCP");
		CustomizedReservedEntry.Command = TEXT("user-custom-mcp-server.exe");
		CustomizedReservedEntry.Arguments.Reset();
		const bool bRetiredMCPDisplayFilterOk =
			SHyperAIStudioWindow::IsRetiredExactOwnedMCPServerEntryForWorkbench(ExactRetiredEntry)
			&& !SHyperAIStudioWindow::IsRetiredExactOwnedMCPServerEntryForWorkbench(CustomizedReservedEntry);

		TArray<FString> AllSurfaceStrings;
		const auto AppendStrings = [&AllSurfaceStrings](const TArray<FString>& Values)
		{
			AllSurfaceStrings.Append(Values);
		};
		AppendStrings(Surface.NavigationTabs);
		AppendStrings(Surface.FirstRunLabels);
		AppendStrings(Surface.AgentTargetActions);
		AppendStrings(Surface.HandoffActions);
		AppendStrings(Surface.AgentTestActions);
		AppendStrings(Surface.AdvancedLabels);
		AppendStrings(Surface.AgentConfigLabels);
		AppendStrings(Surface.MCPServerActions);
		AllSurfaceStrings.Add(TerminalHint);
		AllSurfaceStrings.Add(TerminalRecipe);

		bool bBannedOk = true;
		for (const FString& BannedSubstring : Surface.BannedSubstrings)
		{
			for (const FString& SurfaceString : AllSurfaceStrings)
			{
				if (SurfaceString.Contains(BannedSubstring, ESearchCase::IgnoreCase))
				{
					bBannedOk = false;
					Problems.Add(FString::Printf(TEXT("banned wording `%s` in `%s`"), *BannedSubstring, *SurfaceString));
				}
			}
		}

		if (!bTopTabsOk)
		{
			Problems.Add(TEXT("expected workbench top tabs are missing"));
		}
		if (!bTerminalFallbackOk)
		{
			Problems.Add(TEXT("terminal fallback guidance incomplete"));
		}
		if (!bDockableWorkbenchOk)
		{
			Problems.Add(TEXT("workbench or chat dockable tab spawner is not registered"));
		}
		if (!bRetiredMCPDisplayFilterOk)
		{
			Problems.Add(TEXT("exact retired MCP display filtering is not bounded to owned fingerprints"));
		}

		const bool bOk = TabsOk == ExpectedTabs.Num()
			&& FirstRunOk == ExpectedFirstRun.Num()
			&& TargetsOk == ExpectedTargets.Num()
			&& HandoffOk == ExpectedHandoff.Num()
			&& TestsOk == ExpectedTests.Num()
			&& AdvancedOk == ExpectedAdvanced.Num()
			&& ConfigsOk == ExpectedConfigs.Num()
			&& MCPOk == ExpectedMCP.Num()
			&& bTopTabsOk
			&& bTerminalFallbackOk
			&& bDockableWorkbenchOk
			&& bRetiredMCPDisplayFilterOk
			&& bBannedOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.WorkbenchSmoke %s tabs=%d/%d firstRun=%d/%d targets=%d/%d handoff=%d/%d tests=%d/%d advanced=%d/%d configs=%d/%d mcp=%d/%d terminalFallback=%s topTabs=%s dockableTab=%s retiredMCPFilter=%s banned=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			TabsOk,
			ExpectedTabs.Num(),
			FirstRunOk,
			ExpectedFirstRun.Num(),
			TargetsOk,
			ExpectedTargets.Num(),
			HandoffOk,
			ExpectedHandoff.Num(),
			TestsOk,
			ExpectedTests.Num(),
			AdvancedOk,
			ExpectedAdvanced.Num(),
			ConfigsOk,
			ExpectedConfigs.Num(),
			MCPOk,
			ExpectedMCP.Num(),
			bTerminalFallbackOk ? TEXT("true") : TEXT("false"),
			bTopTabsOk ? TEXT("true") : TEXT("false"),
			bDockableWorkbenchOk ? TEXT("true") : TEXT("false"),
			bRetiredMCPDisplayFilterOk ? TEXT("true") : TEXT("false"),
			bBannedOk ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT("; ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleAgentConfigSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.AgentConfigSmoke failed: settings unavailable."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		const bool bOriginalCodex = Settings->bGenerateCodexConfig;
		const TArray<FHyperAIStudioMCPServerEntry> OriginalServers = Settings->ExtraServers;
		const int32 OriginalMigrationVersion = Settings->NativeToolArchitectureMigrationVersion;
		const bool bOriginalClaude = Settings->bGenerateClaudeConfig;
		const bool bOriginalClaudeMarkdown = Settings->bGenerateClaudeMarkdown;
		const bool bOriginalCursor = Settings->bGenerateCursorConfig;
		const bool bOriginalVSCode = Settings->bGenerateVSCodeConfig;
		const bool bOriginalGemini = Settings->bGenerateGeminiConfig;
		const bool bOriginalEnableCodex = Settings->bEnableCodexAgent;
		const bool bOriginalEnableClaude = Settings->bEnableClaudeAgent;
		const bool bOriginalEnableCursor = Settings->bEnableCursorAgent;
		const bool bOriginalEnableVSCode = Settings->bEnableVSCodeAgent;
		const bool bOriginalEnableGemini = Settings->bEnableGeminiAgent;

		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
		const FString AgentsPath = FPaths::Combine(ProjectRoot, TEXT("AGENTS.md"));
		const FString ClaudeMarkdownPath = FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md"));
		const FString CodexPath = FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml"));
		const FString ClaudeMcpPath = FPaths::Combine(ProjectRoot, TEXT(".mcp.json"));
		const FString CursorPath = FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json"));
		const FString VSCodePath = FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json"));
		const FString GeminiPath = FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json"));
		const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
		const FString WorkspaceReadmePath = FPaths::Combine(HyperDir, TEXT("README.md"));
		const FString WorkspaceIndexPath = FPaths::Combine(HyperDir, TEXT("INDEX.md"));
		const FString ManagedConfigLedgerPath = FPaths::Combine(HyperDir, TEXT("state"), TEXT("managed-config-state.json"));
		const FString ManagedConfigPendingPath = FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-pending.json"));
		const TArray<FString> ManagedFilePaths = {
			FPaths::Combine(ProjectRoot, TEXT(".gitignore")),
			AgentsPath,
			ClaudeMarkdownPath,
			CodexPath,
			ClaudeMcpPath,
			CursorPath,
			VSCodePath,
			GeminiPath,
			WorkspaceReadmePath,
			WorkspaceIndexPath,
			ManagedConfigLedgerPath,
			ManagedConfigPendingPath,
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-settings-pending.json")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Start-HyperAIStudioEditor.ps1")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1")),
			FPaths::Combine(HyperDir, TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("hyperai-status.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.a.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.b.json"))
		};
		const TArray<FHyperAIStudioSavedFile> SavedFiles = SaveFilesForSmoke(ManagedFilePaths);
		const auto WriteSeedFile = [](const FString& FilePath, const FString& Contents)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
			return FFileHelper::SaveStringToFile(Contents, *FilePath);
		};
		const bool bSeedOk =
			WriteSeedFile(AgentsPath, TEXT("# Existing Project Agent Notes\n\nHYPERAI_USER_AGENTS_SENTINEL\n")) &&
			WriteSeedFile(ClaudeMarkdownPath, TEXT("# Existing Claude Notes\n\nHYPERAI_USER_CLAUDE_SENTINEL\n")) &&
			WriteSeedFile(CodexPath, TEXT("# HYPERAI_USER_CODEX_SENTINEL\n")) &&
			WriteSeedFile(ClaudeMcpPath, TEXT("{\"userSentinel\":\"claude\",\"mcpServers\":{\"user-server\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:9999/mcp\"}}}\n")) &&
			WriteSeedFile(CursorPath, TEXT("{\"userSentinel\":\"cursor\",\"mcpServers\":{\"user-server\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:9999/mcp\"}}}\n")) &&
			WriteSeedFile(VSCodePath, TEXT("{\"userSentinel\":\"vscode\",\"servers\":{\"user-server\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:9999/mcp\"}}}\n")) &&
			WriteSeedFile(GeminiPath, TEXT("{\"userSentinel\":\"gemini\",\"mcpServers\":{\"user-server\":{\"httpUrl\":\"http://127.0.0.1:9999/mcp\"}}}\n")) &&
			WriteSeedFile(WorkspaceReadmePath, TEXT("HYPERAI_USER_WORKSPACE_README_SENTINEL\n")) &&
			WriteSeedFile(WorkspaceIndexPath, TEXT("HYPERAI_USER_WORKSPACE_INDEX_SENTINEL\n"));

		Settings->bGenerateCodexConfig = true;
		Settings->bGenerateClaudeConfig = true;
		Settings->bGenerateClaudeMarkdown = true;
		Settings->bGenerateCursorConfig = true;
		Settings->bGenerateVSCodeConfig = true;
		Settings->bGenerateGeminiConfig = true;
		Settings->bEnableCodexAgent = true;
		Settings->bEnableClaudeAgent = true;
		Settings->bEnableCursorAgent = true;
		Settings->bEnableVSCodeAgent = true;
		Settings->bEnableGeminiAgent = true;
		Settings->SaveConfig();

		FHyperAIStudioService Service;
		FText Message;
		const bool bGenerateOk = Service.GenerateProjectFiles(Message);

		const bool bAgentsOk = FPaths::FileExists(AgentsPath);
		const bool bClaudeMdOk = FPaths::FileExists(ClaudeMarkdownPath);
		const bool bCodexOk = FPaths::FileExists(CodexPath);
		const bool bClaudeOk = FPaths::FileExists(ClaudeMcpPath);
		const bool bCursorOk = FPaths::FileExists(CursorPath);
		const bool bVSCodeOk = FPaths::FileExists(VSCodePath);
		const bool bGeminiOk = FPaths::FileExists(GeminiPath);
		FString AgentsText;
		FString ClaudeText;
		FString CodexText;
		FString ClaudeJsonText;
		FString CursorJsonText;
		FString VSCodeJsonText;
		FString GeminiJsonText;
		FString WorkspaceReadmeText;
		FString WorkspaceIndexText;
		FString GitIgnoreText;
		const bool bAgentsTextOk = FFileHelper::LoadFileToString(AgentsText, *AgentsPath);
		FFileHelper::LoadFileToString(ClaudeText, *ClaudeMarkdownPath);
		FFileHelper::LoadFileToString(CodexText, *CodexPath);
		FFileHelper::LoadFileToString(ClaudeJsonText, *ClaudeMcpPath);
		FFileHelper::LoadFileToString(CursorJsonText, *CursorPath);
		FFileHelper::LoadFileToString(VSCodeJsonText, *VSCodePath);
		FFileHelper::LoadFileToString(GeminiJsonText, *GeminiPath);
		FFileHelper::LoadFileToString(WorkspaceReadmeText, *WorkspaceReadmePath);
		FFileHelper::LoadFileToString(WorkspaceIndexText, *WorkspaceIndexPath);
		FFileHelper::LoadFileToString(GitIgnoreText, *FPaths::Combine(ProjectRoot, TEXT(".gitignore")));
		const bool bGeneratedCopyOk = bAgentsTextOk
			&& AgentsText.Contains(TEXT("Native MCP"))
			&& AgentsText.Contains(TEXT("Follow the user's requested intent first"))
			&& AgentsText.Contains(TEXT("Missing native MCP tools do not prove"))
			&& AgentsText.Contains(TEXT("Start and verify Unreal first"))
			&& AgentsText.Contains(TEXT("only ask for an MCP reload or project-root reopen"))
			&& AgentsText.Contains(TEXT("Never ask for a new agent session merely to start Unreal Editor"))
			&& AgentsText.Contains(TEXT("do not hand-roll MCP protocol calls"))
			&& AgentsText.Contains(TEXT("Fast Paths"))
			&& AgentsText.Contains(TEXT("get_current_level"))
			&& AgentsText.Contains(TEXT("find_actors"))
			&& AgentsText.Contains(TEXT("Do not overlap Unreal MCP calls"))
			&& AgentsText.Contains(TEXT("Context Packs"))
			&& AgentsText.Contains(TEXT("Shared Project Workspace"))
			&& AgentsText.Contains(TEXT(".hyperai/work/YYYY-MM-DD-<clear-context-name>/"))
			&& AgentsText.Contains(TEXT("Do not create generic agent-work folders"))
			&& AgentsText.Contains(TEXT("decisions/YYYY-MM-DD-<system>-<decision>.md"))
			&& AgentsText.Contains(TEXT("Consult `.hyperai/INDEX.md` only when prior project context is relevant"))
			&& AgentsText.Contains(TEXT("Extra MCP Servers"))
			&& WorkspaceReadmeText.Contains(TEXT("HyperAIStudio Shared Workspace"))
			&& WorkspaceReadmeText.Contains(TEXT(".hyperai/work/YYYY-MM-DD-<clear-context-name>/"))
			&& WorkspaceReadmeText.Contains(TEXT("`scripts/`"))
			&& WorkspaceReadmeText.Contains(TEXT("`state/`"))
			&& WorkspaceReadmeText.Contains(TEXT("`runtime/`"))
			&& WorkspaceIndexText.Contains(TEXT("HyperAIStudio Project Knowledge Index"))
			&& !AgentsText.Contains(TEXT("Ask before destructive"))
			&& !AgentsText.Contains(TEXT("tools/list"))
			&& !AgentsText.Contains(TEXT("list_toolsets"))
			&& !AgentsText.Contains(TEXT("describe_toolset"))
			&& !AgentsText.Contains(TEXT("first `data:` event"))
			&& !AgentsText.Contains(TEXT("probably not opened"))
			&& !AgentsText.Contains(TEXT("Then wait"))
			&& !AgentsText.Contains(TEXT("If you are Claude Code"))
			&& !AgentsText.Contains(TEXT("prepare prompt"))
			&& !AgentsText.Contains(TEXT("prepare selected nodes"));
		const bool bPreserveOk = AgentsText.Contains(TEXT("HYPERAI_USER_AGENTS_SENTINEL"))
			&& ClaudeText.Contains(TEXT("HYPERAI_USER_CLAUDE_SENTINEL"))
			&& WorkspaceReadmeText.Contains(TEXT("HYPERAI_USER_WORKSPACE_README_SENTINEL"))
			&& WorkspaceIndexText.Contains(TEXT("HYPERAI_USER_WORKSPACE_INDEX_SENTINEL"))
			&& ClaudeText.Contains(TEXT("Claude Code HyperAIStudio Notes"))
			&& ClaudeText.Contains(TEXT("@AGENTS.md"))
			&& CodexText.Contains(TEXT("HYPERAI_USER_CODEX_SENTINEL"))
			&& ClaudeJsonText.Contains(TEXT("\"userSentinel\""))
			&& ClaudeJsonText.Contains(TEXT("\"user-server\""))
			&& CursorJsonText.Contains(TEXT("\"userSentinel\""))
			&& CursorJsonText.Contains(TEXT("\"user-server\""))
			&& VSCodeJsonText.Contains(TEXT("\"userSentinel\""))
			&& VSCodeJsonText.Contains(TEXT("\"user-server\""))
			&& GeminiJsonText.Contains(TEXT("\"userSentinel\""))
			&& GeminiJsonText.Contains(TEXT("\"user-server\""));
		const bool bLayoutOk = FPaths::FileExists(ManagedConfigLedgerPath)
			&& FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Start-HyperAIStudioEditor.ps1")))
			&& FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1")))
			&& FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1")))
			&& FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md")))
			&& GitIgnoreText.Contains(TEXT("# BEGIN HYPERAISTUDIO MANAGED RUNTIME IGNORE"))
			&& GitIgnoreText.Contains(TEXT(".hyperai/runtime/"))
			&& !FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("managed-config-state.json")))
			&& !FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("Start-HyperAIStudioEditor.ps1")))
			&& !FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("Wait-HyperAIStudioMCP.ps1")))
			&& !FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("Send-HyperAIStudioPrompt.ps1")))
			&& !FPaths::FileExists(FPaths::Combine(HyperDir, TEXT("HyperAIStudio-TestCommands.md")));
		const bool bFilesRestored = RestoreFilesForSmoke(SavedFiles);

		Settings->ExtraServers = OriginalServers;
		Settings->NativeToolArchitectureMigrationVersion = OriginalMigrationVersion;
		Settings->bGenerateCodexConfig = bOriginalCodex;
		Settings->bGenerateClaudeConfig = bOriginalClaude;
		Settings->bGenerateClaudeMarkdown = bOriginalClaudeMarkdown;
		Settings->bGenerateCursorConfig = bOriginalCursor;
		Settings->bGenerateVSCodeConfig = bOriginalVSCode;
		Settings->bGenerateGeminiConfig = bOriginalGemini;
		Settings->bEnableCodexAgent = bOriginalEnableCodex;
		Settings->bEnableClaudeAgent = bOriginalEnableClaude;
		Settings->bEnableCursorAgent = bOriginalEnableCursor;
		Settings->bEnableVSCodeAgent = bOriginalEnableVSCode;
		Settings->bEnableGeminiAgent = bOriginalEnableGemini;
		Settings->SaveConfig();
		const bool bSettingsRestored = Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion
			&& Settings->bGenerateCodexConfig == bOriginalCodex
			&& Settings->bGenerateClaudeConfig == bOriginalClaude
			&& Settings->bGenerateClaudeMarkdown == bOriginalClaudeMarkdown
			&& Settings->bGenerateCursorConfig == bOriginalCursor
			&& Settings->bGenerateVSCodeConfig == bOriginalVSCode
			&& Settings->bGenerateGeminiConfig == bOriginalGemini
			&& Settings->bEnableCodexAgent == bOriginalEnableCodex
			&& Settings->bEnableClaudeAgent == bOriginalEnableClaude
			&& Settings->bEnableCursorAgent == bOriginalEnableCursor
			&& Settings->bEnableVSCodeAgent == bOriginalEnableVSCode
			&& Settings->bEnableGeminiAgent == bOriginalEnableGemini;

		const bool bOk = bGenerateOk
			&& bSeedOk
			&& bAgentsOk
			&& bClaudeMdOk
			&& bCodexOk
			&& bClaudeOk
			&& bCursorOk
			&& bVSCodeOk
			&& bGeminiOk
			&& bGeneratedCopyOk
			&& bPreserveOk
			&& bLayoutOk
			&& bFilesRestored
			&& bSettingsRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.AgentConfigSmoke %s agents=%s claudeMd=%s codex=%s claude=%s cursor=%s vscode=%s gemini=%s copy=%s preserve=%s layout=%s filesRestored=%s settingsRestored=%s message=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bAgentsOk ? TEXT("true") : TEXT("false"),
			bClaudeMdOk ? TEXT("true") : TEXT("false"),
			bCodexOk ? TEXT("true") : TEXT("false"),
			bClaudeOk ? TEXT("true") : TEXT("false"),
			bCursorOk ? TEXT("true") : TEXT("false"),
			bVSCodeOk ? TEXT("true") : TEXT("false"),
			bGeminiOk ? TEXT("true") : TEXT("false"),
			bGeneratedCopyOk ? TEXT("true") : TEXT("false"),
			bPreserveOk ? TEXT("true") : TEXT("false"),
			bLayoutOk ? TEXT("true") : TEXT("false"),
			bFilesRestored ? TEXT("true") : TEXT("false"),
			bSettingsRestored ? TEXT("true") : TEXT("false"),
			*Message.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleMCPRegistrySmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPRegistrySmoke failed: settings unavailable."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		const TArray<FHyperAIStudioMCPServerEntry> OriginalServers = Settings->ExtraServers;
		const int32 OriginalMigrationVersion = Settings->NativeToolArchitectureMigrationVersion;
		const bool bOriginalCodex = Settings->bGenerateCodexConfig;
		const bool bOriginalClaude = Settings->bGenerateClaudeConfig;
		const bool bOriginalClaudeMarkdown = Settings->bGenerateClaudeMarkdown;
		const bool bOriginalCursor = Settings->bGenerateCursorConfig;
		const bool bOriginalVSCode = Settings->bGenerateVSCodeConfig;
		const bool bOriginalGemini = Settings->bGenerateGeminiConfig;
		const bool bOriginalEnableCodex = Settings->bEnableCodexAgent;
		const bool bOriginalEnableClaude = Settings->bEnableClaudeAgent;
		const bool bOriginalEnableCursor = Settings->bEnableCursorAgent;
		const bool bOriginalEnableVSCode = Settings->bEnableVSCodeAgent;
		const bool bOriginalEnableGemini = Settings->bEnableGeminiAgent;

		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
		const FString AgentsPath = FPaths::Combine(ProjectRoot, TEXT("AGENTS.md"));
		const FString ClaudeMarkdownPath = FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md"));
		const FString CodexPath = FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml"));
		const FString ClaudeMcpPath = FPaths::Combine(ProjectRoot, TEXT(".mcp.json"));
		const FString CursorPath = FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json"));
		const FString VSCodePath = FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json"));
		const FString GeminiPath = FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json"));
		const FString ManagedConfigLedgerPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("state"), TEXT("managed-config-state.json"));
		const FString ManagedConfigPendingPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("runtime"), TEXT("managed-config-pending.json"));
		const FString ManagedConfigSettingsPendingPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("runtime"), TEXT("managed-config-settings-pending.json"));
		const TArray<FString> ManagedFilePaths = {
			FPaths::Combine(ProjectRoot, TEXT(".gitignore")),
			AgentsPath,
			ClaudeMarkdownPath,
			CodexPath,
			ClaudeMcpPath,
			CursorPath,
			VSCodePath,
			GeminiPath,
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("README.md")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("INDEX.md")),
			ManagedConfigLedgerPath,
			ManagedConfigPendingPath,
			ManagedConfigSettingsPendingPath,
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("managed-config-state.json")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("managed-config-pending.json")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("managed-config-settings-pending.json")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("Start-HyperAIStudioEditor.ps1")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("Wait-HyperAIStudioMCP.ps1")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("Send-HyperAIStudioPrompt.ps1")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("HyperAIStudio-TestCommands.md")),
			FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("hyperai-status.json"))
		};
		const TArray<FHyperAIStudioSavedFile> SavedFiles = SaveFilesForSmoke(ManagedFilePaths);

		FHyperAIStudioService Service;
		TArray<FString> Problems;
		TArray<FString> ProviderProblems;

		Settings->ExtraServers.Reset();
		Settings->bGenerateCodexConfig = true;
		Settings->bGenerateClaudeConfig = true;
		Settings->bGenerateClaudeMarkdown = true;
		Settings->bGenerateCursorConfig = true;
		Settings->bGenerateVSCodeConfig = true;
		Settings->bGenerateGeminiConfig = true;
		Settings->bEnableCodexAgent = true;
		Settings->bEnableClaudeAgent = true;
		Settings->bEnableCursorAgent = true;
		Settings->bEnableVSCodeAgent = true;
		Settings->bEnableGeminiAgent = true;
		Settings->SaveConfig();

		FText ExampleImportMessage;
		const bool bExampleImportOk = Service.ImportExampleMCPProfile(ExampleImportMessage);
		const int32 CountAfterExampleImport = Settings->ExtraServers.Num();
		FText ExampleUpdateMessage;
		const bool bExampleUpdateOk = Service.ImportExampleMCPProfile(ExampleUpdateMessage);
		const bool bExampleUpdateNoDuplicate = bExampleUpdateOk && Settings->ExtraServers.Num() == CountAfterExampleImport;

		int32 ProviderImports = 0;
		int32 ProviderDocs = 0;
		int32 ProviderNotes = 0;
		int32 ProviderEnvMetadata = 0;
		int32 ProviderTargets = 0;
		const TArray<FString> ProviderIds = {
			TEXT("meshy"),
			TEXT("tripo"),
			TEXT("elevenlabs"),
			TEXT("custom")
		};
		for (const FString& ProviderId : ProviderIds)
		{
			const FString ProfileJson = Service.GetProviderMCPProfileJson(ProviderId);
			TSharedPtr<FJsonObject> ProviderProfileObject;
			const bool bProviderProfileParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ProfileJson), ProviderProfileObject)
				&& ProviderProfileObject.IsValid();
			if (bProviderProfileParsed)
			{
				FString DocumentationUrl;
				FString SetupNotes;
				const bool bProviderDocsOk = ProviderProfileObject->TryGetStringField(TEXT("documentationUrl"), DocumentationUrl)
					&& DocumentationUrl.StartsWith(TEXT("https://"));
				const bool bProviderNotesOk = ProviderProfileObject->TryGetStringField(TEXT("setupNotes"), SetupNotes)
					&& SetupNotes.Contains(TEXT("Disabled template"))
					&& SetupNotes.Contains(TEXT("server"));

				const TArray<TSharedPtr<FJsonValue>>* EnvValues = nullptr;
				bool bEnvOk = false;
				if (ProviderProfileObject->TryGetArrayField(TEXT("env"), EnvValues) && EnvValues && EnvValues->Num() > 0)
				{
					const TSharedPtr<FJsonObject> EnvObject = (*EnvValues)[0].IsValid() ? (*EnvValues)[0]->AsObject() : nullptr;
					FString EnvName;
					FString EnvDescription;
					bool bSecret = false;
					bEnvOk = EnvObject.IsValid()
						&& EnvObject->TryGetStringField(TEXT("name"), EnvName)
						&& !EnvName.IsEmpty()
						&& EnvObject->TryGetBoolField(TEXT("secret"), bSecret)
						&& bSecret
						&& EnvObject->TryGetStringField(TEXT("description"), EnvDescription)
						&& !EnvDescription.IsEmpty()
						&& !EnvObject->HasField(TEXT("value"));
				}

				const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
				bool bTargetsOk = false;
				if (ProviderProfileObject->TryGetArrayField(TEXT("targetClients"), TargetValues) && TargetValues)
				{
					TSet<FString> Targets;
					for (const TSharedPtr<FJsonValue>& TargetValue : *TargetValues)
					{
						if (TargetValue.IsValid())
						{
							Targets.Add(TargetValue->AsString());
						}
					}
					bTargetsOk = Targets.Contains(TEXT("codex"))
						&& Targets.Contains(TEXT("claude"))
						&& Targets.Contains(TEXT("cursor"))
						&& Targets.Contains(TEXT("vscode"))
						&& Targets.Contains(TEXT("gemini"));
				}

				ProviderDocs += bProviderDocsOk ? 1 : 0;
				ProviderNotes += bProviderNotesOk ? 1 : 0;
				ProviderEnvMetadata += bEnvOk ? 1 : 0;
				ProviderTargets += bTargetsOk ? 1 : 0;
			}

			FText ProviderMessage;
			if (!ProfileJson.IsEmpty() && bProviderProfileParsed && Service.ImportMCPProfileFromJson(ProfileJson, ProviderMessage))
			{
				++ProviderImports;
			}
			else
			{
				ProviderProblems.Add(FString::Printf(TEXT("%s: %s"), *ProviderId, *ProviderMessage.ToString()));
			}
		}

		const FString DisabledSecretServerId = TEXT("hyperai-disabled-secret-smoke");
		const FString SecretValue = TEXT("SHOULD_NOT_IMPORT_SECRET_VALUE");
		const FString DisabledSecretProfileJson =
			TEXT("{\n")
			TEXT("  \"schema\": \"hyperai.mcp.v1\",\n")
			TEXT("  \"id\": \"hyperai-disabled-secret-smoke\",\n")
			TEXT("  \"name\": \"HyperAIStudio Disabled Secret Smoke MCP\",\n")
			TEXT("  \"description\": \"Disabled profile used to verify secret-free imports.\",\n")
			TEXT("  \"transport\": \"http\",\n")
			TEXT("  \"url\": \"http://127.0.0.1:9199/mcp\",\n")
			TEXT("  \"documentationUrl\": \"https://modelcontextprotocol.io/docs/getting-started/intro\",\n")
			TEXT("  \"domains\": [\"smoke\", \"secret-check\"],\n")
			TEXT("  \"priority\": 55,\n")
			TEXT("  \"targetClients\": [\"codex\", \"claude\", \"cursor\", \"vscode\", \"gemini\"],\n")
			TEXT("  \"env\": [\n")
			TEXT("    { \"name\": \"HYPERAI_SMOKE_SECRET\", \"required\": true, \"secret\": true, \"value\": \"SHOULD_NOT_IMPORT_SECRET_VALUE\" }\n")
			TEXT("  ],\n")
			TEXT("  \"agentInstructions\": [\"Do not export this disabled smoke profile to generated client configs.\"]\n")
			TEXT("}\n");
		FText SecretImportMessage;
		const bool bSecretImportOk = Service.ImportMCPProfileFromJson(DisabledSecretProfileJson, SecretImportMessage);

		const int32 DisabledImportedCount = Settings->ExtraServers.FilterByPredicate([](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return !Entry.bEnabled;
		}).Num();

		const FHyperAIStudioMCPServerEntry* DisabledSecretEntry = Settings->ExtraServers.FindByPredicate([&DisabledSecretServerId](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return Entry.Id == DisabledSecretServerId;
		});
		const bool bSecretNotesOk = DisabledSecretEntry
			&& !DisabledSecretEntry->bEnabled
			&& DisabledSecretEntry->SetupNotes.Contains(TEXT("HYPERAI_SMOKE_SECRET"))
			&& DisabledSecretEntry->SetupNotes.Contains(TEXT("did not import secret values"))
			&& !DisabledSecretEntry->SetupNotes.Contains(SecretValue);

		const FString HttpServerId = TEXT("hyperai-config-smoke");
		const FString HttpServerUrl = TEXT("http://127.0.0.1:9017/mcp");
		FHyperAIStudioMCPServerEntry HttpEntry;
		HttpEntry.Id = HttpServerId;
		HttpEntry.DisplayName = TEXT("HyperAIStudio Config Smoke MCP");
		HttpEntry.Description = TEXT("Validates generated multi-MCP client exports.");
		HttpEntry.Transport = EHyperAIStudioMCPTransport::StreamableHttp;
		HttpEntry.Url = HttpServerUrl;
		HttpEntry.Priority = 42;
		HttpEntry.Domains = { TEXT("automation"), TEXT("smoke") };
		HttpEntry.TargetClients = { TEXT("codex"), TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini") };
		HttpEntry.AgentInstructions = { TEXT("Use hyperai-config-smoke only for HyperAIStudio export smoke coverage.") };
		HttpEntry.bEnabled = true;
		Settings->ExtraServers.Add(HttpEntry);

		const FString CommandServerId = TEXT("hyperai-command-smoke");
		const FString CommandServerArg = TEXT("@modelcontextprotocol/server-everything");
		FHyperAIStudioMCPServerEntry CommandEntry;
		CommandEntry.Id = CommandServerId;
		CommandEntry.DisplayName = TEXT("HyperAIStudio Command Smoke MCP");
		CommandEntry.Description = TEXT("Validates stdio-style MCP command config exports.");
		CommandEntry.Transport = EHyperAIStudioMCPTransport::Command;
		CommandEntry.Command = TEXT("npx");
		CommandEntry.Arguments = { TEXT("-y"), CommandServerArg };
		CommandEntry.Priority = 43;
		CommandEntry.Domains = { TEXT("automation"), TEXT("command-smoke") };
		CommandEntry.TargetClients = { TEXT("codex"), TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini") };
		CommandEntry.AgentInstructions = { TEXT("Use hyperai-command-smoke only for HyperAIStudio command export smoke coverage.") };
		CommandEntry.bEnabled = true;
		Settings->ExtraServers.Add(CommandEntry);
		Settings->SaveConfig();

		FText FilesMessage;
		const bool bGenerateOk = Service.GenerateProjectFiles(FilesMessage);

		FString AgentsText;
		const bool bAgentsLoaded = FFileHelper::LoadFileToString(AgentsText, *AgentsPath);
		const bool bAgentsOk = bAgentsLoaded
			&& AgentsText.Contains(HttpServerId)
			&& AgentsText.Contains(CommandServerId)
			&& AgentsText.Contains(TEXT("priority 42"))
			&& AgentsText.Contains(TEXT("Use hyperai-config-smoke only"))
			&& !AgentsText.Contains(DisabledSecretServerId)
			&& !AgentsText.Contains(SecretValue);

		FString CodexText;
		const bool bCodexLoaded = FFileHelper::LoadFileToString(CodexText, *CodexPath);
		const bool bCodexOk = bCodexLoaded
			&& CodexText.Contains(FString::Printf(TEXT("[mcp_servers.\"%s\"]"), *HttpServerId))
			&& CodexText.Contains(FString::Printf(TEXT("url = \"%s\""), *HttpServerUrl))
			&& CodexText.Contains(FString::Printf(TEXT("[mcp_servers.\"%s\"]"), *CommandServerId))
			&& CodexText.Contains(TEXT("command = \"npx\""))
			&& CodexText.Contains(CommandServerArg)
			&& !CodexText.Contains(DisabledSecretServerId)
			&& !CodexText.Contains(SecretValue);

		const auto ValidateJsonConfig = [&HttpServerId, &HttpServerUrl, &CommandServerId, &CommandServerArg](
			const FString& FilePath,
			const FString& ContainerField,
			const FString& HttpUrlField)
		{
			return JsonMCPServerStringEqualsForSmoke(FilePath, ContainerField, HttpServerId, HttpUrlField, HttpServerUrl)
				&& JsonMCPServerStringEqualsForSmoke(FilePath, ContainerField, CommandServerId, TEXT("command"), TEXT("npx"))
				&& JsonMCPServerArrayContainsForSmoke(FilePath, ContainerField, CommandServerId, TEXT("args"), CommandServerArg);
		};

		const bool bClaudeOk = ValidateJsonConfig(ClaudeMcpPath, TEXT("mcpServers"), TEXT("url"));
		const bool bCursorOk = ValidateJsonConfig(CursorPath, TEXT("mcpServers"), TEXT("url"));
		const bool bVSCodeOk = ValidateJsonConfig(VSCodePath, TEXT("servers"), TEXT("url"));
		const bool bGeminiOk = ValidateJsonConfig(GeminiPath, TEXT("mcpServers"), TEXT("httpUrl"));
		const int32 ConfigsOk = (bCodexOk ? 1 : 0)
			+ (bClaudeOk ? 1 : 0)
			+ (bCursorOk ? 1 : 0)
			+ (bVSCodeOk ? 1 : 0)
			+ (bGeminiOk ? 1 : 0);

		bool bSecretValueAbsent = true;
		for (const FString& FilePath : ManagedFilePaths)
		{
			FString FileText;
			if (FFileHelper::LoadFileToString(FileText, *FilePath) && FileText.Contains(SecretValue))
			{
				bSecretValueAbsent = false;
				break;
			}
		}

		const int32 EnabledCount = Settings->ExtraServers.FilterByPredicate([](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return Entry.bEnabled;
		}).Num();
		const bool bImportOk = bExampleImportOk
			&& bExampleUpdateNoDuplicate
			&& ProviderImports == ProviderIds.Num()
			&& ProviderDocs == ProviderIds.Num()
			&& ProviderNotes == ProviderIds.Num()
			&& ProviderEnvMetadata == ProviderIds.Num()
			&& ProviderTargets == ProviderIds.Num()
			&& bSecretImportOk
			&& DisabledImportedCount == 6
			&& bSecretNotesOk;
		const bool bEnabledExportOk = EnabledCount == 2 && bAgentsOk && ConfigsOk == 5;
		const bool bFilesRestored = RestoreFilesForSmoke(SavedFiles);

		Settings->ExtraServers = OriginalServers;
		Settings->NativeToolArchitectureMigrationVersion = OriginalMigrationVersion;
		Settings->bGenerateCodexConfig = bOriginalCodex;
		Settings->bGenerateClaudeConfig = bOriginalClaude;
		Settings->bGenerateClaudeMarkdown = bOriginalClaudeMarkdown;
		Settings->bGenerateCursorConfig = bOriginalCursor;
		Settings->bGenerateVSCodeConfig = bOriginalVSCode;
		Settings->bGenerateGeminiConfig = bOriginalGemini;
		Settings->bEnableCodexAgent = bOriginalEnableCodex;
		Settings->bEnableClaudeAgent = bOriginalEnableClaude;
		Settings->bEnableCursorAgent = bOriginalEnableCursor;
		Settings->bEnableVSCodeAgent = bOriginalEnableVSCode;
		Settings->bEnableGeminiAgent = bOriginalEnableGemini;
		Settings->SaveConfig();

		const bool bSettingsRestored = Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion
			&& Settings->bGenerateCodexConfig == bOriginalCodex
			&& Settings->bGenerateClaudeConfig == bOriginalClaude
			&& Settings->bGenerateClaudeMarkdown == bOriginalClaudeMarkdown
			&& Settings->bGenerateCursorConfig == bOriginalCursor
			&& Settings->bGenerateVSCodeConfig == bOriginalVSCode
			&& Settings->bGenerateGeminiConfig == bOriginalGemini
			&& Settings->bEnableCodexAgent == bOriginalEnableCodex
			&& Settings->bEnableClaudeAgent == bOriginalEnableClaude
			&& Settings->bEnableCursorAgent == bOriginalEnableCursor
			&& Settings->bEnableVSCodeAgent == bOriginalEnableVSCode
			&& Settings->bEnableGeminiAgent == bOriginalEnableGemini;

		if (!bGenerateOk)
		{
			Problems.Add(FString::Printf(TEXT("generate files: %s"), *FilesMessage.ToString()));
		}
		if (!bImportOk)
		{
			Problems.Add(FString::Printf(TEXT("imports disabled=%d example=%s duplicate=%s providers=%d/%d secret=%s notes=%s"),
				DisabledImportedCount,
				bExampleImportOk ? TEXT("true") : TEXT("false"),
				bExampleUpdateNoDuplicate ? TEXT("true") : TEXT("false"),
				ProviderImports,
				ProviderIds.Num(),
				bSecretImportOk ? TEXT("true") : TEXT("false"),
				bSecretNotesOk ? TEXT("true") : TEXT("false")));
		}
		if (ProviderProblems.Num() > 0)
		{
			Problems.Add(FString::Printf(TEXT("provider problems: %s"), *FString::Join(ProviderProblems, TEXT("; "))));
		}
		if (ProviderDocs != ProviderIds.Num()
			|| ProviderNotes != ProviderIds.Num()
			|| ProviderEnvMetadata != ProviderIds.Num()
			|| ProviderTargets != ProviderIds.Num())
		{
			Problems.Add(FString::Printf(TEXT("provider metadata docs=%d/%d notes=%d/%d env=%d/%d targets=%d/%d"),
				ProviderDocs,
				ProviderIds.Num(),
				ProviderNotes,
				ProviderIds.Num(),
				ProviderEnvMetadata,
				ProviderIds.Num(),
				ProviderTargets,
				ProviderIds.Num()));
		}
		if (!bEnabledExportOk)
		{
			Problems.Add(FString::Printf(TEXT("exports enabled=%d agents=%s configs=%d/5"),
				EnabledCount,
				bAgentsOk ? TEXT("true") : TEXT("false"),
				ConfigsOk));
		}
		if (!bSecretValueAbsent)
		{
			Problems.Add(TEXT("secret value leaked to generated files"));
		}
		if (!bFilesRestored)
		{
			Problems.Add(TEXT("managed project files were not fully restored"));
		}
		if (!bSettingsRestored)
		{
			Problems.Add(TEXT("settings were not fully restored"));
		}

		const bool bOk = bGenerateOk
			&& bImportOk
			&& bEnabledExportOk
			&& bSecretValueAbsent
			&& bFilesRestored
			&& bSettingsRestored
			&& ProviderProblems.Num() == 0;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPRegistrySmoke %s imports=%d/6 providers=%d/4 providerDocs=%d/4 providerNotes=%d/4 providerEnv=%d/4 providerTargets=%d/4 duplicateUpdate=%s enabled=%d/2 configs=%d/5 agents=%s secrets=%s filesRestored=%s settingsRestored=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			DisabledImportedCount,
			ProviderImports,
			ProviderDocs,
			ProviderNotes,
			ProviderEnvMetadata,
			ProviderTargets,
			bExampleUpdateNoDuplicate ? TEXT("true") : TEXT("false"),
			EnabledCount,
			ConfigsOk,
			bAgentsOk ? TEXT("true") : TEXT("false"),
			(bSecretNotesOk && bSecretValueAbsent) ? TEXT("true") : TEXT("false"),
			bFilesRestored ? TEXT("true") : TEXT("false"),
			bSettingsRestored ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT(" | ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleHyperUEMCPSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.HyperUEMCPSmoke failed settings=false"));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		const TArray<FHyperAIStudioMCPServerEntry> OriginalServers = Settings->ExtraServers;
		const int32 OriginalMigrationVersion = Settings->NativeToolArchitectureMigrationVersion;
		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
		const TArray<FString> ManagedFiles = {
			FPaths::Combine(ProjectRoot, TEXT("AGENTS.md")),
			FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml")),
			FPaths::Combine(ProjectRoot, TEXT(".mcp.json")),
			FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json")),
			FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json")),
			FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json"))
		};
		const TArray<FHyperAIStudioSavedFile> FileSnapshots = SaveFilesForSmoke(ManagedFiles);

		FHyperAIStudioService Service;
		FText ImportMessage;
		// The retired built-in fixture is now a negative contract: the generic
		// importer must reject its reserved id without changing settings/files.
		const FString RetiredReservedProfileJson = TEXT(
			"{\"schema\":\"hyperai.mcp.v1\",\"id\":\"hyper-ue-mcp\","
			"\"name\":\"Retired built-in fixture\",\"transport\":\"stdio\","
			"\"command\":\"retired-profile-must-never-run\",\"args\":[],\"url\":\"\"}");
		const bool bUnrealReservedImportRejected = !Service.ImportMCPProfileFromJson(RetiredReservedProfileJson, ImportMessage);
		FText KnowledgeImportMessage;
		const FString RetiredKnowledgeReservedProfileJson = TEXT(
			"{\"schema\":\"hyperai.mcp.v1\",\"id\":\"hyper-knowledge-mcp\","
			"\"name\":\"Customized Knowledge fixture\",\"transport\":\"stdio\","
			"\"command\":\"customized-knowledge-must-be-preserved\",\"args\":[],\"url\":\"\"}");
		const bool bKnowledgeReservedImportRejected = !Service.ImportMCPProfileFromJson(
			RetiredKnowledgeReservedProfileJson,
			KnowledgeImportMessage);
		const bool bImportOk = bUnrealReservedImportRejected && bKnowledgeReservedImportRejected;
		Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bProfileOk = Settings
			&& Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		const FString PluginBaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
		const bool bLegacyRuntimeAbsent = Plugin.IsValid()
			&& !IFileManager::Get().DirectoryExists(*FPaths::Combine(
				PluginBaseDir,
				TEXT("Resources"),
				TEXT("MCPServers")));
		const FString PrivateSourceDir = FPaths::Combine(
			PluginBaseDir,
			TEXT("Source"),
			TEXT("HyperAIStudio"),
			TEXT("Private"));
		const bool bLegacyBridgeSourceAbsent = Plugin.IsValid()
			&& !IFileManager::Get().FileExists(*FPaths::Combine(PrivateSourceDir, TEXT("HyperAIStudioTcpBridge.cpp")))
			&& !IFileManager::Get().FileExists(*FPaths::Combine(PrivateSourceDir, TEXT("HyperAIStudioTcpBridge.h")));

		FText DisableMessage;
		const bool bDisableRejected = !Service.DisableHyperUEMCPProfile(DisableMessage);
		Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bLegacyEntryPreserved = Settings
			&& Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion;

		FText ReenableMessage;
		const bool bReenableRejected = bDisableRejected && !Service.ImportHyperUEMCPProfile(ReenableMessage);
		Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bLegacyEntryStillPreserved = Settings
			&& Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion;

		const bool bNoUnsafeMigrationWrite = bLegacyEntryPreserved && bLegacyEntryStillPreserved;

		if (Settings)
		{
			Settings->ExtraServers = OriginalServers;
			Settings->NativeToolArchitectureMigrationVersion = OriginalMigrationVersion;
			Settings->SaveConfig();
		}
		const bool bFilesRestored = RestoreFilesForSmoke(FileSnapshots);
		const bool bSettingsRestored = Settings
			&& Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion;

		TArray<FString> Problems;
		if (!bImportOk)
		{
			Problems.Add(FString::Printf(
				TEXT("reserved import guard: unreal=%s (%s), knowledge=%s (%s)"),
				bUnrealReservedImportRejected ? TEXT("rejected") : TEXT("accepted"),
				*ImportMessage.ToString(),
				bKnowledgeReservedImportRejected ? TEXT("rejected") : TEXT("accepted"),
				*KnowledgeImportMessage.ToString()));
		}
		if (!bProfileOk)
		{
			Problems.Add(TEXT("reserved-profile rejection changed settings or migration state"));
		}
		if (!bLegacyRuntimeAbsent)
		{
			Problems.Add(TEXT("retired MCPServers runtime directory is still packaged"));
		}
		if (!bLegacyBridgeSourceAbsent)
		{
			Problems.Add(TEXT("retired HyperAIStudioTcpBridge source is still packaged"));
		}
		if (!bDisableRejected || !bLegacyEntryPreserved || !bReenableRejected || !bLegacyEntryStillPreserved)
		{
			Problems.Add(FString::Printf(TEXT("retirement guard failed cleanupRejected=%s entryPreserved=%s installRejected=%s stillPreserved=%s disableMessage=%s reenableMessage=%s"),
				bDisableRejected ? TEXT("true") : TEXT("false"),
				bLegacyEntryPreserved ? TEXT("true") : TEXT("false"),
				bReenableRejected ? TEXT("true") : TEXT("false"),
				bLegacyEntryStillPreserved ? TEXT("true") : TEXT("false"),
				*DisableMessage.ToString(),
				*ReenableMessage.ToString()));
		}
		if (!bNoUnsafeMigrationWrite)
		{
			Problems.Add(TEXT("retirement guard unexpectedly changed the legacy settings fixture"));
		}
		if (!bFilesRestored)
		{
			Problems.Add(TEXT("managed files were not restored"));
		}
		if (!bSettingsRestored)
		{
			Problems.Add(TEXT("settings were not restored"));
		}

		const bool bOk = bImportOk
			&& bProfileOk
			&& bLegacyRuntimeAbsent
			&& bLegacyBridgeSourceAbsent
			&& bDisableRejected
			&& bLegacyEntryPreserved
			&& bReenableRejected
			&& bLegacyEntryStillPreserved
			&& bNoUnsafeMigrationWrite
			&& bFilesRestored
			&& bSettingsRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.HyperUEMCPSmoke %s reservedImportRejected=%s knowledgeReservedImportRejected=%s settingsUnchanged=%s runtimeAbsent=%s bridgeSourceAbsent=%s cleanupRejected=%s installRejected=%s noUnsafeWrite=%s filesRestored=%s settingsRestored=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bUnrealReservedImportRejected ? TEXT("true") : TEXT("false"),
			bKnowledgeReservedImportRejected ? TEXT("true") : TEXT("false"),
			bProfileOk ? TEXT("true") : TEXT("false"),
			bLegacyRuntimeAbsent ? TEXT("true") : TEXT("false"),
			bLegacyBridgeSourceAbsent ? TEXT("true") : TEXT("false"),
			bDisableRejected && bLegacyEntryPreserved ? TEXT("true") : TEXT("false"),
			bReenableRejected && bLegacyEntryStillPreserved ? TEXT("true") : TEXT("false"),
			bNoUnsafeMigrationWrite ? TEXT("true") : TEXT("false"),
			bFilesRestored ? TEXT("true") : TEXT("false"),
			bSettingsRestored ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT(" | ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleMCPClipboardSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPClipboardSmoke failed: settings unavailable."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		FString OriginalClipboard;
		FPlatformApplicationMisc::ClipboardPaste(OriginalClipboard);
		const TArray<FHyperAIStudioMCPServerEntry> OriginalServers = Settings->ExtraServers;
		const int32 OriginalMigrationVersion = Settings->NativeToolArchitectureMigrationVersion;
		const bool bOriginalCodex = Settings->bGenerateCodexConfig;
		const bool bOriginalClaude = Settings->bGenerateClaudeConfig;
		const bool bOriginalClaudeMarkdown = Settings->bGenerateClaudeMarkdown;
		const bool bOriginalCursor = Settings->bGenerateCursorConfig;
		const bool bOriginalVSCode = Settings->bGenerateVSCodeConfig;
		const bool bOriginalGemini = Settings->bGenerateGeminiConfig;

		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
		const FString AgentsPath = FPaths::Combine(ProjectRoot, TEXT("AGENTS.md"));
		const FString ClaudeMarkdownPath = FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md"));
		const FString CodexPath = FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml"));
		const FString ClaudeMcpPath = FPaths::Combine(ProjectRoot, TEXT(".mcp.json"));
		const FString CursorPath = FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json"));
		const FString VSCodePath = FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json"));
		const FString GeminiPath = FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json"));
		const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
		const TArray<FString> ManagedFilePaths = {
			FPaths::Combine(ProjectRoot, TEXT(".gitignore")),
			AgentsPath,
			ClaudeMarkdownPath,
			CodexPath,
			ClaudeMcpPath,
			CursorPath,
			VSCodePath,
			GeminiPath,
			FPaths::Combine(HyperDir, TEXT("state"), TEXT("managed-config-state.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-pending.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-settings-pending.json")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Start-HyperAIStudioEditor.ps1")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1")),
			FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1")),
			FPaths::Combine(HyperDir, TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md")),
			FPaths::Combine(HyperDir, TEXT("README.md")),
			FPaths::Combine(HyperDir, TEXT("INDEX.md")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("hyperai-status.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.a.json")),
			FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.b.json")),
			FPaths::Combine(HyperDir, TEXT("managed-config-state.json")),
			FPaths::Combine(HyperDir, TEXT("managed-config-pending.json")),
			FPaths::Combine(HyperDir, TEXT("managed-config-settings-pending.json")),
			FPaths::Combine(HyperDir, TEXT("Start-HyperAIStudioEditor.ps1")),
			FPaths::Combine(HyperDir, TEXT("Wait-HyperAIStudioMCP.ps1")),
			FPaths::Combine(HyperDir, TEXT("Send-HyperAIStudioPrompt.ps1")),
			FPaths::Combine(HyperDir, TEXT("HyperAIStudio-TestCommands.md")),
			FPaths::Combine(HyperDir, TEXT("hyperai-status.json"))
		};
		const TArray<FHyperAIStudioSavedFile> SavedFiles = SaveFilesForSmoke(ManagedFilePaths);
		if (!AreFileSnapshotsCompleteForSmoke(SavedFiles))
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPClipboardSmoke failed: a managed-file snapshot could not be captured; no smoke mutations were made."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}
		TArray<FString> BackupSourcePaths = ManagedFilePaths;
		if (!GEditorPerProjectIni.IsEmpty())
		{
			BackupSourcePaths.Add(GEditorPerProjectIni);
		}
		const TArray<FHyperAIStudioSavedBackupDirectory> SavedBackupDirectories =
			SaveBackupDirectoriesForSmoke(BackupSourcePaths);
		if (!AreBackupDirectorySnapshotsCompleteForSmoke(SavedBackupDirectories))
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPClipboardSmoke failed: backup-artifact state could not be captured; no smoke mutations were made."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		FHyperAIStudioService Service;
		TArray<FString> Problems;
		bool bClipboardFallbackUsed = false;

		const auto ReadClipboardForSmoke = [](const FString& ExpectedText, FString& OutClipboard)
		{
			if (FApp::IsUnattended())
			{
				OutClipboard = ExpectedText;
				return true;
			}

			FPlatformApplicationMisc::ClipboardPaste(OutClipboard);
			return false;
		};

		const auto ImportClipboardPayloadForSmoke = [&Service](const FString& EffectiveClipboard, bool bUsedFallback, FText& OutMessage)
		{
			if (bUsedFallback)
			{
				return Service.ImportMCPProfileFromJson(EffectiveClipboard, OutMessage);
			}
			return Service.ImportMCPProfileFromClipboard(OutMessage);
		};

		Settings->ExtraServers.Reset();
		Settings->SaveConfig();

		const FString ExampleJson = Service.GetExampleMCPProfileJson();
		Service.CopyTextToClipboard(ExampleJson);
		FString ExampleClipboard;
		const bool bExampleClipboardFallback = ReadClipboardForSmoke(ExampleJson, ExampleClipboard);
		bClipboardFallbackUsed = bClipboardFallbackUsed || bExampleClipboardFallback;
		const bool bExampleCopyOk = ExampleClipboard == ExampleJson
			&& ExampleClipboard.Contains(TEXT("\"schema\": \"hyperai.mcp.v1\""))
			&& ExampleClipboard.Contains(TEXT("\"id\": \"project-docs\""));

		FText ExampleMessage;
		const bool bExampleImportOk = ImportClipboardPayloadForSmoke(ExampleClipboard, bExampleClipboardFallback, ExampleMessage);
		FText ExampleFilesMessage;
		const bool bExampleFilesOk = bExampleImportOk && Service.GenerateProjectFiles(ExampleFilesMessage);
		const int32 CountAfterExampleImport = Settings->ExtraServers.Num();
		const FHyperAIStudioMCPServerEntry* ExampleEntry = Settings->ExtraServers.FindByPredicate([](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return Entry.Id == TEXT("project-docs");
		});
		const bool bExampleEntryOk = ExampleEntry
			&& !ExampleEntry->bEnabled
			&& ExampleEntry->Transport == EHyperAIStudioMCPTransport::Command
			&& ExampleEntry->Command == TEXT("npx")
			&& ExampleEntry->Arguments.Contains(TEXT("@modelcontextprotocol/server-filesystem"))
			&& ExampleEntry->SetupNotes.Contains(TEXT("Install command"))
			&& ExampleEntry->SetupNotes.Contains(TEXT("Docs:"));

		FText ExampleUpdateMessage;
		const bool bExampleUpdateOk = ImportClipboardPayloadForSmoke(ExampleClipboard, bExampleClipboardFallback, ExampleUpdateMessage);
		const bool bExampleUpdateNoDuplicate = bExampleUpdateOk && Settings->ExtraServers.Num() == CountAfterExampleImport;

		const FString ProviderJson = Service.GetProviderMCPProfileJson(TEXT("meshy"));
		Service.CopyTextToClipboard(ProviderJson);
		FString ProviderClipboard;
		const bool bProviderClipboardFallback = ReadClipboardForSmoke(ProviderJson, ProviderClipboard);
		bClipboardFallbackUsed = bClipboardFallbackUsed || bProviderClipboardFallback;
		TSharedPtr<FJsonObject> ProviderClipboardObject;
		const bool bProviderClipboardParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ProviderClipboard), ProviderClipboardObject)
			&& ProviderClipboardObject.IsValid();
		FString ProviderClipboardId;
		const bool bProviderCopyOk = ProviderClipboard == ProviderJson
			&& bProviderClipboardParsed
			&& ProviderClipboardObject->TryGetStringField(TEXT("id"), ProviderClipboardId)
			&& ProviderClipboardId == TEXT("meshy-provider")
			&& ProviderClipboard.Contains(TEXT("MESHY_API_KEY"))
			&& !ProviderClipboard.Contains(TEXT("\"value\""));

		FText ProviderMessage;
		const bool bProviderImportOk = ImportClipboardPayloadForSmoke(ProviderClipboard, bProviderClipboardFallback, ProviderMessage);
		FText ProviderFilesMessage;
		const bool bProviderFilesOk = bProviderImportOk && Service.GenerateProjectFiles(ProviderFilesMessage);
		const FHyperAIStudioMCPServerEntry* ProviderEntry = Settings->ExtraServers.FindByPredicate([](const FHyperAIStudioMCPServerEntry& Entry)
		{
			return Entry.Id == TEXT("meshy-provider");
		});
		const bool bProviderEntryOk = ProviderEntry
			&& !ProviderEntry->bEnabled
			&& ProviderEntry->Transport == EHyperAIStudioMCPTransport::StreamableHttp
			&& ProviderEntry->Url == TEXT("http://127.0.0.1:9101/mcp")
			&& ProviderEntry->SetupNotes.Contains(TEXT("MESHY_API_KEY"))
			&& ProviderEntry->SetupNotes.Contains(TEXT("did not import secret values"))
			&& !ProviderEntry->SetupNotes.Contains(TEXT("\"value\""))
			&& ProviderEntry->TargetClients.Contains(TEXT("codex"))
			&& ProviderEntry->TargetClients.Contains(TEXT("gemini"));

		const int32 CountBeforeInvalidImport = Settings->ExtraServers.Num();
		const FString InvalidProfileJson = TEXT("{ invalid hyperai mcp profile");
		Service.CopyTextToClipboard(InvalidProfileJson);
		FString InvalidClipboard;
		const bool bInvalidClipboardFallback = ReadClipboardForSmoke(InvalidProfileJson, InvalidClipboard);
		bClipboardFallbackUsed = bClipboardFallbackUsed || bInvalidClipboardFallback;
		FText InvalidMessage;
		const bool bInvalidRejected = !ImportClipboardPayloadForSmoke(InvalidClipboard, bInvalidClipboardFallback, InvalidMessage)
			&& InvalidMessage.ToString().Contains(TEXT("valid JSON"))
			&& Settings->ExtraServers.Num() == CountBeforeInvalidImport;

		if (!bExampleCopyOk)
		{
			Problems.Add(TEXT("example profile copy/paste mismatch"));
		}
		if (!bExampleImportOk || !bExampleFilesOk || !bExampleEntryOk)
		{
			Problems.Add(FString::Printf(TEXT("example import failed: %s / %s"), *ExampleMessage.ToString(), *ExampleFilesMessage.ToString()));
		}
		if (!bExampleUpdateNoDuplicate)
		{
			Problems.Add(TEXT("example clipboard re-import duplicated entry"));
		}
		if (!bProviderCopyOk)
		{
			Problems.Add(TEXT("provider profile copy/paste mismatch"));
		}
		if (!bProviderImportOk || !bProviderFilesOk || !bProviderEntryOk)
		{
			Problems.Add(FString::Printf(TEXT("provider import failed: %s / %s"), *ProviderMessage.ToString(), *ProviderFilesMessage.ToString()));
		}
		if (!bInvalidRejected)
		{
			Problems.Add(FString::Printf(TEXT("invalid clipboard profile was not rejected: %s"), *InvalidMessage.ToString()));
		}

		const bool bFilesRestored = RestoreFilesForSmoke(SavedFiles);
		Settings->ExtraServers = OriginalServers;
		Settings->NativeToolArchitectureMigrationVersion = OriginalMigrationVersion;
		Settings->bGenerateCodexConfig = bOriginalCodex;
		Settings->bGenerateClaudeConfig = bOriginalClaude;
		Settings->bGenerateClaudeMarkdown = bOriginalClaudeMarkdown;
		Settings->bGenerateCursorConfig = bOriginalCursor;
		Settings->bGenerateVSCodeConfig = bOriginalVSCode;
		Settings->bGenerateGeminiConfig = bOriginalGemini;
		Settings->SaveConfig();
		const bool bNoBackupArtifacts = AreBackupDirectoriesUnchangedForSmoke(SavedBackupDirectories);
		const auto EntriesMatch = [](const FHyperAIStudioMCPServerEntry& A, const FHyperAIStudioMCPServerEntry& B)
		{
			return A.Id == B.Id
				&& A.DisplayName == B.DisplayName
				&& A.Description == B.Description
				&& A.Transport == B.Transport
				&& A.Url == B.Url
				&& A.Command == B.Command
				&& A.Arguments == B.Arguments
				&& A.Priority == B.Priority
				&& A.Domains == B.Domains
				&& A.TargetClients == B.TargetClients
				&& A.AgentInstructions == B.AgentInstructions
				&& A.SetupNotes == B.SetupNotes
				&& A.bEnabled == B.bEnabled;
		};
		bool bSettingsRestored = Settings->ExtraServers.Num() == OriginalServers.Num()
			&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion
			&& Settings->bGenerateCodexConfig == bOriginalCodex
			&& Settings->bGenerateClaudeConfig == bOriginalClaude
			&& Settings->bGenerateClaudeMarkdown == bOriginalClaudeMarkdown
			&& Settings->bGenerateCursorConfig == bOriginalCursor
			&& Settings->bGenerateVSCodeConfig == bOriginalVSCode
			&& Settings->bGenerateGeminiConfig == bOriginalGemini;
		for (int32 Index = 0; bSettingsRestored && Index < OriginalServers.Num(); ++Index)
		{
			bSettingsRestored = EntriesMatch(Settings->ExtraServers[Index], OriginalServers[Index]);
		}

		Service.CopyTextToClipboard(OriginalClipboard);
		FString RestoredClipboard;
		FPlatformApplicationMisc::ClipboardPaste(RestoredClipboard);
		const bool bClipboardRestored = RestoredClipboard == OriginalClipboard || bClipboardFallbackUsed;

		const bool bOk = bExampleCopyOk
			&& bExampleImportOk
			&& bExampleFilesOk
			&& bExampleEntryOk
			&& bExampleUpdateNoDuplicate
			&& bProviderCopyOk
			&& bProviderImportOk
			&& bProviderFilesOk
			&& bProviderEntryOk
			&& bInvalidRejected
			&& bFilesRestored
			&& bNoBackupArtifacts
			&& bSettingsRestored
			&& bClipboardRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MCPClipboardSmoke %s exampleCopy=%s exampleImport=%s exampleEntry=%s duplicateUpdate=%s providerCopy=%s providerImport=%s providerEntry=%s invalidRejected=%s filesRestored=%s noBackupArtifacts=%s settingsRestored=%s clipboardRestored=%s clipboardFallback=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bExampleCopyOk ? TEXT("true") : TEXT("false"),
			(bExampleImportOk && bExampleFilesOk) ? TEXT("true") : TEXT("false"),
			bExampleEntryOk ? TEXT("true") : TEXT("false"),
			bExampleUpdateNoDuplicate ? TEXT("true") : TEXT("false"),
			bProviderCopyOk ? TEXT("true") : TEXT("false"),
			(bProviderImportOk && bProviderFilesOk) ? TEXT("true") : TEXT("false"),
			bProviderEntryOk ? TEXT("true") : TEXT("false"),
			bInvalidRejected ? TEXT("true") : TEXT("false"),
			bFilesRestored ? TEXT("true") : TEXT("false"),
			bNoBackupArtifacts ? TEXT("true") : TEXT("false"),
			bSettingsRestored ? TEXT("true") : TEXT("false"),
			bClipboardRestored ? TEXT("true") : TEXT("false"),
			bClipboardFallbackUsed ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT(" | ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleImportMCPProfile()
	{
		FHyperAIStudioService Service;
		FText Message;
		const bool bImportOk = Service.ImportMCPProfileFromClipboard(Message);
		if (bImportOk)
		{
			FText FilesMessage;
			const bool bFilesOk = Service.GenerateProjectFiles(FilesMessage);
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromClipboard %s: %s %s"),
				bFilesOk ? TEXT("succeeded") : TEXT("failed"),
				*Message.ToString(),
				*FilesMessage.ToString());
			return;
		}

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromClipboard failed: %s"), *Message.ToString());
	}

	void RunConsoleImportMCPProfileFromFile(const TArray<FString>& Args)
	{
		const FString RawPath = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")).TrimStartAndEnd() : FString();
		if (RawPath.IsEmpty())
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromFile failed: pass a hyperai-mcp.json path."));
			return;
		}

		const FString ProfilePath = FPaths::ConvertRelativePathToFull(RawPath);
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ProfilePath))
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromFile failed: could not read `%s`."), *ProfilePath);
			return;
		}

		FHyperAIStudioService Service;
		FText Message;
		const bool bImportOk = Service.ImportMCPProfileFromJson(JsonText, Message);
		if (bImportOk)
		{
			FText FilesMessage;
			const bool bFilesOk = Service.GenerateProjectFiles(FilesMessage);
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromFile %s path=%s: %s %s"),
				bFilesOk ? TEXT("succeeded") : TEXT("failed"),
				*ProfilePath,
				*Message.ToString(),
				*FilesMessage.ToString());
			return;
		}

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportMCPProfileFromFile failed path=%s: %s"), *ProfilePath, *Message.ToString());
	}

	void RunConsoleImportExampleMCPProfile()
	{
		FHyperAIStudioService Service;
		FText Message;
		const bool bImportOk = Service.ImportExampleMCPProfile(Message);
		if (bImportOk)
		{
			FText FilesMessage;
			const bool bFilesOk = Service.GenerateProjectFiles(FilesMessage);
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportExampleMCPProfile %s: %s %s"),
				bFilesOk ? TEXT("succeeded") : TEXT("failed"),
				*Message.ToString(),
				*FilesMessage.ToString());
			return;
		}

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportExampleMCPProfile failed: %s"), *Message.ToString());
	}

	void RunConsoleImportProviderMCPProfile(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportProviderMCPProfile failed: pass meshy, tripo, elevenlabs, custom, or all."));
			return;
		}

		TArray<FString> ProviderIds;
		if (Args[0].Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			ProviderIds = { TEXT("meshy"), TEXT("tripo"), TEXT("elevenlabs"), TEXT("custom") };
		}
		else
		{
			ProviderIds.Add(Args[0]);
		}

		FHyperAIStudioService Service;
		TArray<FString> ImportedProviders;
		TArray<FString> Problems;
		for (const FString& ProviderId : ProviderIds)
		{
			const FString ProfileJson = Service.GetProviderMCPProfileJson(ProviderId);
			if (ProfileJson.IsEmpty())
			{
				Problems.Add(FString::Printf(TEXT("unknown provider `%s`"), *ProviderId));
				continue;
			}

			FText Message;
			if (Service.ImportMCPProfileFromJson(ProfileJson, Message))
			{
				ImportedProviders.Add(FString::Printf(TEXT("%s: %s"), *ProviderId, *Message.ToString()));
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s: %s"), *ProviderId, *Message.ToString()));
			}
		}

		if (ImportedProviders.Num() == 0)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportProviderMCPProfile failed: %s"),
				Problems.Num() == 0 ? TEXT("no providers imported") : *FString::Join(Problems, TEXT("; ")));
			return;
		}

		FText FilesMessage;
		const bool bFilesOk = Service.GenerateProjectFiles(FilesMessage);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.ImportProviderMCPProfile %s imported=%s problems=%s files=%s"),
			bFilesOk ? TEXT("succeeded") : TEXT("failed"),
			*FString::Join(ImportedProviders, TEXT("; ")),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT("; ")),
			*FilesMessage.ToString());
	}

	void RunConsoleTestMCPServer(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		TArray<FString> SelectorParts;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				continue;
			}
			SelectorParts.Add(Arg);
		}

		const FString Selector = SelectorParts.Num() > 0 ? FString::Join(SelectorParts, TEXT(" ")).TrimStartAndEnd() : FString();
		if (Selector.IsEmpty())
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TestMCPServer failed: pass a server index, id, or display name."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (!Settings)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TestMCPServer failed: settings unavailable."));
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		int32 ServerIndex = INDEX_NONE;
		if (Selector.IsNumeric())
		{
			ServerIndex = FCString::Atoi(*Selector);
		}
		if (!Settings->ExtraServers.IsValidIndex(ServerIndex))
		{
			const FString NormalizedSelector = NormalizeMCPServerSelector(Selector);
			ServerIndex = Settings->ExtraServers.IndexOfByPredicate([&NormalizedSelector](const FHyperAIStudioMCPServerEntry& Entry)
			{
				return NormalizeMCPServerSelector(Entry.Id) == NormalizedSelector
					|| NormalizeMCPServerSelector(Entry.DisplayName) == NormalizedSelector;
			});
		}

		if (!Settings->ExtraServers.IsValidIndex(ServerIndex))
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TestMCPServer failed: no extra MCP server matched `%s`."), *Selector);
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
			return;
		}

		const FString DisplayName = Settings->ExtraServers[ServerIndex].DisplayName;
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TestMCPServer queued `%s` quitOnComplete=%s"),
			*DisplayName,
			bQuitOnComplete ? TEXT("true") : TEXT("false"));

		FHyperAIStudioService Service;
		Service.TestExtraMCPServerAsync(ServerIndex, [this, DisplayName, bQuitOnComplete](bool bOk, const FString& Message)
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TestMCPServer %s `%s`: %s"),
				bOk ? TEXT("succeeded") : TEXT("failed"),
				*DisplayName,
				*Message);
			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
		});
	}

	void RunConsoleBlueprintMenuSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const TArray<FHyperAIStudioBlueprintMenuEntryDefinition> Entries = GetBlueprintMenuEntryDefinitions();
		const TArray<FHyperAIStudioExpectedMenuEntry> ExpectedEntries = {
			{ TEXT("CopyBlueprintNodeContext"), TEXT("Copy Selected Node Context"), TEXT("provider-neutral context") },
			{ TEXT("SmartAutoCommentBlueprintNodes"), TEXT("Smart Auto Comment Selected Nodes"), TEXT("ask AI") },
			{ TEXT("QuickAutoCommentBlueprintNodes"), TEXT("Quick Auto Comment Selected Nodes"), TEXT("without using AI") }
		};

		int32 LabelsOk = 0;
		int32 ToolTipsOk = 0;
		int32 IntentsOk = 0;
		int32 CopyContextOk = 0;
		int32 SmartAutoCommentOk = 0;
		int32 QuickAutoCommentOk = 0;
		bool bBannedOk = true;
		TArray<FString> Problems;

		for (const FHyperAIStudioExpectedMenuEntry& ExpectedEntry : ExpectedEntries)
		{
			const FHyperAIStudioBlueprintMenuEntryDefinition* Entry = Entries.FindByPredicate([&ExpectedEntry](const FHyperAIStudioBlueprintMenuEntryDefinition& Candidate)
			{
				return Candidate.Id == ExpectedEntry.Name;
			});

			if (!Entry)
			{
				Problems.Add(FString::Printf(TEXT("missing `%s`"), *ExpectedEntry.Name.ToString()));
				continue;
			}

			const FString Label = Entry->Label.ToString();
			const FString ToolTip = Entry->ToolTip.ToString();
			if (Label == ExpectedEntry.Label)
			{
				++LabelsOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s label `%s`"), *ExpectedEntry.Name.ToString(), *Label));
			}

			if (ToolTip.Contains(ExpectedEntry.ToolTipContains))
			{
				++ToolTipsOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s tooltip `%s`"), *ExpectedEntry.Name.ToString(), *ToolTip));
			}

			if (Label.Contains(TEXT("Codex"), ESearchCase::IgnoreCase)
				|| Label.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
				|| ToolTip.Contains(TEXT("Codex"), ESearchCase::IgnoreCase)
				|| ToolTip.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
				|| Entry->Intent.Contains(TEXT("Codex"), ESearchCase::IgnoreCase)
				|| Entry->Intent.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
				|| Label.Contains(TEXT("prepare prompt"), ESearchCase::IgnoreCase)
				|| ToolTip.Contains(TEXT("prepare prompt"), ESearchCase::IgnoreCase)
				|| Entry->Intent.Contains(TEXT("prepare prompt"), ESearchCase::IgnoreCase)
				|| Label.Contains(TEXT("debug endpoint"), ESearchCase::IgnoreCase)
				|| ToolTip.Contains(TEXT("debug endpoint"), ESearchCase::IgnoreCase)
				|| Entry->Intent.Contains(TEXT("debug endpoint"), ESearchCase::IgnoreCase))
			{
				bBannedOk = false;
				Problems.Add(FString::Printf(TEXT("%s contains banned internal wording"), *ExpectedEntry.Name.ToString()));
			}

			if (Entry->bQuickAutoComment)
			{
				if (ExpectedEntry.Name == TEXT("QuickAutoCommentBlueprintNodes")
					&& Entry->AgentName.IsEmpty()
					&& Entry->Intent.IsEmpty()
					&& !Entry->bOpenTerminal)
				{
					++QuickAutoCommentOk;
				}
				continue;
			}

			if (Entry->Intent.Contains(TEXT("selected Blueprint nodes")))
			{
				++IntentsOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s intent incomplete"), *ExpectedEntry.Name.ToString()));
			}

			if (ExpectedEntry.Name == TEXT("CopyBlueprintNodeContext")
				&& Entry->AgentName == TEXT("Coding Agent")
				&& !Entry->bOpenTerminal
				&& Entry->Intent.Contains(TEXT("wait for my task")))
			{
				++CopyContextOk;
			}
			if (ExpectedEntry.Name == TEXT("SmartAutoCommentBlueprintNodes")
				&& Entry->AgentName.IsEmpty()
				&& Entry->bOpenTerminal
				&& Entry->Intent.Contains(TEXT("Smart Auto Comment"))
				&& Entry->Intent.Contains(TEXT("Do not move, create, delete, rewire, or compile Blueprint nodes")))
			{
				++SmartAutoCommentOk;
			}
		}

		const bool bDefinitionsOk = Entries.Num() == ExpectedEntries.Num();
		const bool bGraphExtenderOk = IsGraphEditorExtenderRegistered();
		bool bQuickSettingOk = false;
		if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
		{
			const FHyperAIStudioBlueprintMenuEntryDefinition* QuickEntry = Entries.FindByPredicate([](const FHyperAIStudioBlueprintMenuEntryDefinition& Candidate)
			{
				return Candidate.Id == TEXT("QuickAutoCommentBlueprintNodes");
			});
			const bool bOriginalQuickSetting = Settings->bShowQuickAutoCommentInBlueprintMenu;
			if (QuickEntry)
			{
				Settings->bShowQuickAutoCommentInBlueprintMenu = false;
				const bool bHiddenWhenOff = !ShouldShowBlueprintContextEntry(*QuickEntry);
				Settings->bShowQuickAutoCommentInBlueprintMenu = true;
				const bool bVisibleWhenOn = ShouldShowBlueprintContextEntry(*QuickEntry);
				bQuickSettingOk = bHiddenWhenOff && bVisibleWhenOn;
			}
			Settings->bShowQuickAutoCommentInBlueprintMenu = bOriginalQuickSetting;
		}
		if (!bDefinitionsOk)
		{
			Problems.Add(FString::Printf(TEXT("definitions=%d/%d"), Entries.Num(), ExpectedEntries.Num()));
		}
		if (!bGraphExtenderOk)
		{
			Problems.Add(TEXT("Blueprint graph context-menu extender not registered"));
		}

		const bool bOk = bDefinitionsOk
			&& LabelsOk == ExpectedEntries.Num()
			&& ToolTipsOk == ExpectedEntries.Num()
			&& IntentsOk == 2
			&& CopyContextOk == 1
			&& SmartAutoCommentOk == 1
			&& QuickAutoCommentOk == 1
			&& bQuickSettingOk
			&& bGraphExtenderOk
			&& bBannedOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.BlueprintMenuSmoke %s definitions=%d/3 labels=%d/3 tooltips=%d/3 intents=%d/2 copyContext=%d/1 smartAutoComment=%d/1 quickAutoComment=%d/1 quickSetting=%s graphExtender=%s banned=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			Entries.Num(),
			LabelsOk,
			ToolTipsOk,
			IntentsOk,
			CopyContextOk,
			SmartAutoCommentOk,
			QuickAutoCommentOk,
			bQuickSettingOk ? TEXT("true") : TEXT("false"),
			bGraphExtenderOk ? TEXT("true") : TEXT("false"),
			bBannedOk ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT("; ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleBlueprintContextSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage(), TEXT("HyperAIStudioTransientBlueprintSmokeGraph"), RF_Transient);
		Graph->Schema = UEdGraphSchema::StaticClass();

		auto AddSmokeNode = [Graph](const TCHAR* Name, int32 X, int32 Y, const TCHAR* Comment)
		{
			UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, Name, RF_Transient | RF_Transactional);
			NewNode->CreateNewGuid();
			NewNode->NodePosX = X;
			NewNode->NodePosY = Y;
			NewNode->NodeWidth = 260;
			NewNode->NodeHeight = 140;
			NewNode->NodeComment = Comment;
			Graph->AddNode(NewNode, false, false);
			return NewNode;
		};

		UEdGraphNode* FirstNode = AddSmokeNode(
			TEXT("HyperAIStudioTransientBlueprintSmokeNodeA"),
			128,
			96,
			TEXT("HyperAIStudio transient Blueprint smoke node A"));
		UEdGraphNode* SecondNode = AddSmokeNode(
			TEXT("HyperAIStudioTransientBlueprintSmokeNodeB"),
			520,
			180,
			TEXT("HyperAIStudio transient Blueprint smoke node B"));

		TSharedRef<SGraphEditor> GraphEditor = SNew(SGraphEditor)
			.GraphToEdit(Graph);
		GraphEditor->ClearSelectionSet();
		GraphEditor->SetNodeSelection(FirstNode, true);
		GraphEditor->SetNodeSelection(SecondNode, true);
		const int32 SelectedGraphNodeCount = GraphEditor->GetSelectedNodes().Num();

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText PromptMessage;
		const bool bPromptOk = Service.CreateBlueprintPromptPackage(
			TEXT("Validate HyperAIStudio Blueprint multi-node context capture in an unattended smoke run."),
			TEXT("Codex"),
			Graph,
			FirstNode,
			Package,
			PromptMessage);

		int32 ContextNodeCount = INDEX_NONE;
		bool bContextParsed = false;
		if (bPromptOk && !Package.ContextJsonPath.IsEmpty())
		{
			FString ContextJson;
			TSharedPtr<FJsonObject> RootObject;
			if (FFileHelper::LoadFileToString(ContextJson, *Package.ContextJsonPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContextJson), RootObject)
				&& RootObject.IsValid())
			{
				double NodeCount = 0.0;
				if (RootObject->TryGetNumberField(TEXT("selectedBlueprintNodeCount"), NodeCount))
				{
					ContextNodeCount = FMath::RoundToInt(NodeCount);
					bContextParsed = true;
				}
			}
		}

		FText CommentMessage;
		const bool bCommentOk = Service.AutoCommentBlueprintNodes(Graph, FirstNode, CommentMessage);
		int32 CommentNodeCount = 0;
		for (const UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (GraphNode && GraphNode->IsA<UEdGraphNode_Comment>())
			{
				++CommentNodeCount;
			}
		}

		const bool bOk = bPromptOk
			&& SelectedGraphNodeCount == 2
			&& bContextParsed
			&& ContextNodeCount == 2
			&& bCommentOk
			&& CommentNodeCount == 1
			&& CommentMessage.ToString().Contains(TEXT("2 selected Blueprint node"))
			&& FPaths::FileExists(Package.PromptPath)
			&& FPaths::FileExists(Package.ContextJsonPath);

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.BlueprintContextSmoke %s prompt=%s context=%s selectedNodes=%d contextNodes=%d comments=%d promptMessage=%s commentMessage=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			Package.PromptPath.IsEmpty() ? TEXT("(none)") : *Package.PromptPath,
			Package.ContextJsonPath.IsEmpty() ? TEXT("(none)") : *Package.ContextJsonPath,
			SelectedGraphNodeCount,
			ContextNodeCount,
			CommentNodeCount,
			*PromptMessage.ToString(),
			*CommentMessage.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleSelectionContextSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		USelection* SelectedActors = GEditor ? GEditor->GetSelectedActors() : nullptr;
		TArray<TWeakObjectPtr<AActor>> PreviousActors;
		if (SelectedActors)
		{
			for (FSelectionIterator It(*SelectedActors); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					PreviousActors.Add(Actor);
				}
			}
		}

		AActor* SmokeActor = nullptr;
		if (World)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.Name = MakeUniqueObjectName(World, AActor::StaticClass(), TEXT("HyperAIStudioSmokeActor"));
			SpawnParams.ObjectFlags = RF_Transient;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SmokeActor = World->SpawnActor<AActor>(
				AActor::StaticClass(),
				FVector(12.0, 34.0, 56.0),
				FRotator::ZeroRotator,
				SpawnParams);
		}

		const FString SmokeLabel = TEXT("HyperAIStudio Smoke Actor");
		if (SmokeActor)
		{
			SmokeActor->SetActorLabel(SmokeLabel);
		}

		const bool bSelected = SelectedActors && SmokeActor;
		if (bSelected)
		{
			SelectedActors->DeselectAll(AActor::StaticClass());
			SelectedActors->Select(SmokeActor);
		}

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		const bool bPromptOk = bSelected && Service.CreatePromptPackage(
			TEXT("Validate HyperAIStudio selected actor context capture in an unattended smoke run."),
			TEXT("Codex"),
			Package,
			Message);

		int32 ActorCount = INDEX_NONE;
		bool bContextParsed = false;
		bool bActorMatched = false;
		if (bPromptOk && !Package.ContextJsonPath.IsEmpty())
		{
			FString ContextJson;
			TSharedPtr<FJsonObject> RootObject;
			if (FFileHelper::LoadFileToString(ContextJson, *Package.ContextJsonPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContextJson), RootObject)
				&& RootObject.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
				if (RootObject->TryGetArrayField(TEXT("selectedActors"), ActorValues) && ActorValues)
				{
					ActorCount = ActorValues->Num();
					bContextParsed = true;
					for (const TSharedPtr<FJsonValue>& ActorValue : *ActorValues)
					{
						const TSharedPtr<FJsonObject> ActorObject = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
						if (!ActorObject.IsValid())
						{
							continue;
						}

						FString ActorLabel;
						FString ActorName;
						ActorObject->TryGetStringField(TEXT("label"), ActorLabel);
						ActorObject->TryGetStringField(TEXT("name"), ActorName);
						if (ActorLabel == SmokeLabel || ActorName.Contains(TEXT("HyperAIStudioSmokeActor")))
						{
							bActorMatched = true;
							break;
						}
					}
				}
			}
		}

		if (SelectedActors)
		{
			SelectedActors->DeselectAll(AActor::StaticClass());
			for (const TWeakObjectPtr<AActor>& Actor : PreviousActors)
			{
				if (Actor.IsValid())
				{
					SelectedActors->Select(Actor.Get());
				}
			}
		}
		if (World && SmokeActor && !SmokeActor->IsActorBeingDestroyed())
		{
			World->DestroyActor(SmokeActor);
		}

		const bool bPromptFilesOk = FPaths::FileExists(Package.PromptPath) && FPaths::FileExists(Package.ContextJsonPath);
		const bool bOk = World
			&& SmokeActor
			&& bSelected
			&& bPromptOk
			&& bContextParsed
			&& ActorCount == 1
			&& bActorMatched
			&& bPromptFilesOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.SelectionContextSmoke %s selected=%s prompt=%s context=%s actors=%d actorMatched=%s restored=%d message=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bSelected ? TEXT("true") : TEXT("false"),
			Package.PromptPath.IsEmpty() ? TEXT("(none)") : *Package.PromptPath,
			Package.ContextJsonPath.IsEmpty() ? TEXT("(none)") : *Package.ContextJsonPath,
			ActorCount,
			bActorMatched ? TEXT("true") : TEXT("false"),
			PreviousActors.Num(),
			*Message.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleAssetContextSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const FAssetData SmokeAssetData(
			FName(TEXT("/Temp/HyperAIStudioSmokeAssetPackage")),
			FName(TEXT("/Temp")),
			FName(TEXT("HyperAIStudioSmokeAsset")),
			FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Texture2D")));
		const TArray<FAssetData> SelectedAssets = { SmokeAssetData };

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		const bool bPromptOk = SmokeAssetData.IsValid() && Service.CreatePromptPackageForAssets(
			TEXT("Validate HyperAIStudio selected Content Browser asset context capture in an unattended smoke run."),
			TEXT("Codex"),
			SelectedAssets,
			Package,
			Message);

		int32 AssetCount = INDEX_NONE;
		bool bContextParsed = false;
		bool bAssetMatched = false;
		if (bPromptOk && !Package.ContextJsonPath.IsEmpty())
		{
			FString ContextJson;
			TSharedPtr<FJsonObject> RootObject;
			if (FFileHelper::LoadFileToString(ContextJson, *Package.ContextJsonPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContextJson), RootObject)
				&& RootObject.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* AssetValues = nullptr;
				if (RootObject->TryGetArrayField(TEXT("selectedAssets"), AssetValues) && AssetValues)
				{
					AssetCount = AssetValues->Num();
					bContextParsed = true;
					for (const TSharedPtr<FJsonValue>& AssetValue : *AssetValues)
					{
						const TSharedPtr<FJsonObject> AssetObject = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
						if (!AssetObject.IsValid())
						{
							continue;
						}

						FString AssetName;
						FString ObjectPath;
						AssetObject->TryGetStringField(TEXT("assetName"), AssetName);
						AssetObject->TryGetStringField(TEXT("objectPath"), ObjectPath);
						if (AssetName == TEXT("HyperAIStudioSmokeAsset") || ObjectPath.Contains(TEXT("HyperAIStudioSmokeAsset")))
						{
							bAssetMatched = true;
							break;
						}
					}
				}
			}
		}

		const bool bPromptFilesOk = FPaths::FileExists(Package.PromptPath) && FPaths::FileExists(Package.ContextJsonPath);
		const bool bOk = SmokeAssetData.IsValid()
			&& bPromptOk
			&& bContextParsed
			&& AssetCount == 1
			&& bAssetMatched
			&& bPromptFilesOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.AssetContextSmoke %s assetData=%s prompt=%s context=%s assets=%d assetMatched=%s message=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			SmokeAssetData.IsValid() ? TEXT("true") : TEXT("false"),
			Package.PromptPath.IsEmpty() ? TEXT("(none)") : *Package.PromptPath,
			Package.ContextJsonPath.IsEmpty() ? TEXT("(none)") : *Package.ContextJsonPath,
			AssetCount,
			bAssetMatched ? TEXT("true") : TEXT("false"),
			*Message.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleCreateTestPrompt(const TArray<FString>& Args)
	{
		const FString TestId = Args.Num() > 0 && !Args[0].TrimStartAndEnd().IsEmpty()
			? Args[0].TrimStartAndEnd()
			: FString(TEXT("agent-connection"));

		FString AgentName;
		if (Args.Num() > 1)
		{
			TArray<FString> AgentParts = Args;
			AgentParts.RemoveAt(0);
			AgentName = FString::Join(AgentParts, TEXT(" ")).TrimStartAndEnd();
		}

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage Package;
		FText Message;
		const bool bOk = Service.CreateTestPromptPackage(TestId, AgentName, Package, Message);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.CreateTestPrompt %s testId=%s agent=%s prompt=%s context=%s message=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			*TestId,
			Package.AgentName.IsEmpty() ? TEXT("(none)") : *Package.AgentName,
			Package.PromptPath.IsEmpty() ? TEXT("(none)") : *Package.PromptPath,
			Package.ContextJsonPath.IsEmpty() ? TEXT("(none)") : *Package.ContextJsonPath,
			*Message.ToString());
	}

	void RunConsoleAgentPromptSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const TArray<FString> AgentNames = {
			TEXT("Codex"),
			TEXT("Claude Code"),
			TEXT("Gemini"),
			TEXT("Cursor"),
			TEXT("VS Code/Copilot")
		};

		FHyperAIStudioService Service;
		int32 PackagesOk = 0;
		int32 FilesOk = 0;
		int32 PromptTextOk = 0;
		int32 TerminalOk = 0;
		TArray<FString> Problems;
		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();

		for (const FString& AgentName : AgentNames)
		{
			FHyperAIStudioPromptPackage Package;
			FText Message;
			const bool bCreated = Service.CreateQuickConnectPromptPackage(AgentName, Package, Message);
			const bool bPackageOk = bCreated
				&& Package.bSuccess
				&& Package.AgentName == AgentName
				&& Package.Message.Contains(TEXT("Quick prompt ready"));
			if (bPackageOk)
			{
				++PackagesOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s package: %s"), *AgentName, *Message.ToString()));
			}

			const bool bFilesOk = FPaths::FileExists(Package.PromptPath)
				&& Package.ContextJsonPath.IsEmpty()
				&& Package.ContextMarkdownPath.IsEmpty();
			if (bFilesOk)
			{
				++FilesOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s files missing"), *AgentName));
			}

			FString PromptText;
			FFileHelper::LoadFileToString(PromptText, *Package.PromptPath);
			const bool bPromptOk = PromptText.Contains(TEXT("Connect to this project's configured `unreal-mcp`"))
				&& PromptText.Contains(TEXT("Verify the connection with `list_toolsets`"))
				&& PromptText.Contains(TEXT("If Unreal is offline"))
				&& PromptText.Contains(TEXT("Start-HyperAIStudioEditor.ps1"))
				&& PromptText.Contains(TEXT("Say `Unreal MCP ready` only after that succeeds"))
				&& PromptText.Contains(TEXT("then wait for my task"))
				&& PromptText.Len() <= 420
				&& !PromptText.Contains(TEXT("must be reopened"))
				&& !PromptText.Contains(TEXT("Project:"))
				&& !PromptText.Contains(ProjectRoot)
				&& !PromptText.Contains(TEXT("AGENTS.md"))
				&& !PromptText.Contains(TEXT("CLAUDE.md"))
				&& !PromptText.Contains(TEXT("Unreal MCP:"))
				&& !PromptText.Contains(TEXT("tool-search mode"))
				&& !PromptText.Contains(TEXT("3 discovery dispatchers"))
				&& !PromptText.Contains(TEXT("project MCP config was not loaded into this task"))
				&& !PromptText.Contains(TEXT("fresh agent task from this exact project root"))
				&& !PromptText.Contains(TEXT("Paste this prompt only"))
				&& !PromptText.Contains(TEXT("Codex uses `AGENTS.md`"))
				&& !PromptText.Contains(TEXT("Claude Code uses `CLAUDE.md`"))
				&& !PromptText.Contains(TEXT("Context markdown"))
				&& !PromptText.Contains(TEXT("Context JSON"))
				&& !PromptText.Contains(TEXT("Ask before destructive"))
				&& !PromptText.Contains(TEXT("Reply exactly"))
				&& !PromptText.Contains(TEXT("Confirm the configured `unreal-mcp` server is reachable"))
				&& !PromptText.Contains(TEXT("tools/list"))
				&& !PromptText.Contains(TEXT("If you are Claude Code"))
				&& !PromptText.Contains(TEXT("first `data:` event"))
				&& !PromptText.Contains(TEXT("describe_toolset"));
			if (bPromptOk)
			{
				++PromptTextOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s prompt text incomplete"), *AgentName));
			}

			const bool bTerminalOk = Package.TerminalCommand.Contains(TEXT("Send-HyperAIStudioPrompt.ps1"))
				&& Package.TerminalCommand.Contains(AgentName)
				&& Package.TerminalCommand.Contains(Package.PromptPath);
			if (bTerminalOk)
			{
				++TerminalOk;
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("%s terminal command incomplete"), *AgentName));
			}
		}

		const bool bOk = PackagesOk == AgentNames.Num()
			&& FilesOk == AgentNames.Num()
			&& PromptTextOk == AgentNames.Num()
			&& TerminalOk == AgentNames.Num();
		const FString ProblemText = Problems.Num() == 0 ? FString(TEXT("none")) : FString::Join(Problems, TEXT(" | "));

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.AgentPromptSmoke %s packages=%d/%d files=%d/%d promptText=%d/%d terminal=%d/%d agents=Codex,Claude Code,Gemini,Cursor,VS Code/Copilot problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			PackagesOk,
			AgentNames.Num(),
			FilesOk,
			AgentNames.Num(),
			PromptTextOk,
			AgentNames.Num(),
			TerminalOk,
			AgentNames.Num(),
			*ProblemText);

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleHandoffScriptSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		FHyperAIStudioService Service;
		FText FilesMessage;
		const bool bFilesOk = Service.GenerateProjectFiles(FilesMessage);

		const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
		const FString SendScriptPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1"));
		const FString WaitScriptPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1"));
		const FString TestCommandsPath = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md"));

		FString SendScript;
		FString WaitScript;
		FString TestCommands;
		const bool bSendScriptLoaded = FFileHelper::LoadFileToString(SendScript, *SendScriptPath);
		const bool bWaitScriptLoaded = FFileHelper::LoadFileToString(WaitScript, *WaitScriptPath);
		const bool bTestCommandsLoaded = FFileHelper::LoadFileToString(TestCommands, *TestCommandsPath);

		const bool bTermOk = SendScript.Contains(TEXT("$env:TERM = 'xterm-256color'"));
		const bool bProjectRootOk = SendScript.Contains(TEXT("$HyperAIStudioRoot = Split-Path -Parent $PSScriptRoot"))
			&& SendScript.Contains(TEXT("$ProjectRoot = Split-Path -Parent $HyperAIStudioRoot"))
			&& SendScript.Contains(TEXT("Set-Location -LiteralPath $ProjectRoot"));
		const bool bClipboardOk = SendScript.Contains(TEXT("Set-Clipboard -Value $Prompt"));
		const bool bResolverOk = SendScript.Contains(TEXT("function Find-HyperAIStudioCommand"))
			&& SendScript.Contains(TEXT("function Invoke-HyperAIStudioCommand"));
		const bool bCodexOk = SendScript.Contains(TEXT("$CodexCommand = Find-HyperAIStudioCommand 'codex'"))
			&& SendScript.Contains(TEXT("Invoke-HyperAIStudioCommand $CodexCommand $CodexArgs"))
			&& SendScript.Contains(TEXT("service_tier=fast"))
			&& SendScript.Contains(TEXT("Starting a fresh Codex CLI session from the project root"))
			&& !SendScript.Contains(TEXT("@('app', $ProjectRoot)"))
			&& !SendScript.Contains(TEXT("codex app"));
		const bool bClaudeOk = SendScript.Contains(TEXT("$ClaudeCommand = Find-HyperAIStudioCommand 'claude'"))
			&& SendScript.Contains(TEXT("Invoke-HyperAIStudioCommand $ClaudeCommand"))
			&& SendScript.Contains(TEXT("Opening Claude Code from the project root"))
			&& !SendScript.Contains(TEXT("claude $Prompt"));
		const bool bGeminiOk = SendScript.Contains(TEXT("$GeminiCommand = Find-HyperAIStudioCommand 'gemini'"))
			&& SendScript.Contains(TEXT("Invoke-HyperAIStudioCommand $GeminiCommand"))
			&& SendScript.Contains(TEXT("Opening Gemini from the project root"))
			&& !SendScript.Contains(TEXT("gemini $Prompt"));
		const bool bCursorOk = SendScript.Contains(TEXT("$CursorCommand = Find-HyperAIStudioCommand 'cursor'"))
			&& SendScript.Contains(TEXT("Invoke-HyperAIStudioCommand $CursorCommand @('.')"));
		const bool bVSCodeOk = SendScript.Contains(TEXT("$CodeCommand = Find-HyperAIStudioCommand 'code'"))
			&& SendScript.Contains(TEXT("Invoke-HyperAIStudioCommand $CodeCommand @('.')"));
		const bool bFallbackOk = SendScript.Contains(TEXT("paste the copied prompt"));
		const bool bWaitScriptSafeOk = WaitScript.Contains(TEXT("3 discovery dispatchers"))
			&& WaitScript.Contains(TEXT("Epic and HyperAI tools are available behind those dispatchers"))
			&& WaitScript.Contains(TEXT("agent task must separately load its project MCP config"))
			&& WaitScript.Contains(TEXT("runtime\\hyperai-status.json"))
			&& !WaitScript.Contains(TEXT("\"method\":\"tools/call\""))
			&& !WaitScript.Contains(TEXT("\"method\":\"list_toolsets\""))
			&& !WaitScript.Contains(TEXT("\"method\":\"describe_toolset\""));
		const int32 WaitInvocationIndex = SendScript.Find(TEXT("-File $WaitScript -Port $McpPort"));
		const int32 CodexLaunchIndex = SendScript.Find(TEXT("$CodexCommand = Find-HyperAIStudioCommand 'codex'"));
		const bool bReadinessGateOk = WaitInvocationIndex != INDEX_NONE
			&& CodexLaunchIndex != INDEX_NONE
			&& WaitInvocationIndex < CodexLaunchIndex
			&& SendScript.Contains(TEXT("HyperAIStudio handoff v2"))
			&& SendScript.Contains(TEXT("if ($LASTEXITCODE -ne 0)"))
			&& SendScript.Contains(TEXT("the agent was not started"));
		const bool bTestTitleOk = TestCommands.Contains(TEXT("Test in agent app"));
		const bool bOk = bFilesOk
			&& bSendScriptLoaded
			&& bWaitScriptLoaded
			&& bTestCommandsLoaded
			&& bTermOk
			&& bProjectRootOk
			&& bClipboardOk
			&& bResolverOk
			&& bCodexOk
			&& bClaudeOk
			&& bGeminiOk
			&& bCursorOk
			&& bVSCodeOk
			&& bFallbackOk
			&& bWaitScriptSafeOk
			&& bReadinessGateOk
			&& bTestTitleOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.HandoffScriptSmoke %s sendScript=%s waitScript=%s testCommands=%s term=%s projectRoot=%s clipboard=%s codex=%s claude=%s gemini=%s cursor=%s vscode=%s fallback=%s waitSafe=%s readinessGate=%s testTitle=%s files=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bSendScriptLoaded ? *SendScriptPath : TEXT("(missing)"),
			bWaitScriptLoaded ? *WaitScriptPath : TEXT("(missing)"),
			bTestCommandsLoaded ? *TestCommandsPath : TEXT("(missing)"),
			bTermOk ? TEXT("true") : TEXT("false"),
			bProjectRootOk ? TEXT("true") : TEXT("false"),
			bClipboardOk ? TEXT("true") : TEXT("false"),
			bCodexOk ? TEXT("true") : TEXT("false"),
			bClaudeOk ? TEXT("true") : TEXT("false"),
			bGeminiOk ? TEXT("true") : TEXT("false"),
			bCursorOk ? TEXT("true") : TEXT("false"),
			bVSCodeOk ? TEXT("true") : TEXT("false"),
			bFallbackOk ? TEXT("true") : TEXT("false"),
			bWaitScriptSafeOk ? TEXT("true") : TEXT("false"),
			bReadinessGateOk ? TEXT("true") : TEXT("false"),
			bTestTitleOk ? TEXT("true") : TEXT("false"),
			*FilesMessage.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleTerminalLaunchPlanSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const FString OriginalTestPath = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_TEST_PATH"));
		const FString SmokeRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HyperAIStudio"), TEXT("TerminalLaunchPlanSmoke"));
		const FString FakeWtDir = FPaths::Combine(SmokeRoot, TEXT("fake-wt"));
		const FString FakeNoWtDir = FPaths::Combine(SmokeRoot, TEXT("fake-no-wt"));
		const FString FakeCodexDir = FPaths::Combine(SmokeRoot, TEXT("fake-codex"));
		const bool bDirsOk = IFileManager::Get().MakeDirectory(*FakeWtDir, true)
			&& IFileManager::Get().MakeDirectory(*FakeNoWtDir, true)
			&& IFileManager::Get().MakeDirectory(*FakeCodexDir, true);
		const FString FakeWtPath = FPaths::Combine(FakeWtDir, TEXT("wt.exe"));
		const bool bFakeWtOk = FFileHelper::SaveStringToFile(TEXT(""), *FakeWtPath);
		const FString FakeCodexPath = FPaths::Combine(FakeCodexDir, TEXT("codex.cmd"));
		const bool bFakeCodexOk = FFileHelper::SaveStringToFile(TEXT("@echo off\r\n"), *FakeCodexPath);
		const FString FakeClaudePath = FPaths::Combine(FakeCodexDir, TEXT("claude.cmd"));
		const bool bFakeClaudeOk = FFileHelper::SaveStringToFile(TEXT("@echo off\r\n"), *FakeClaudePath);

		FHyperAIStudioService Service;
		FHyperAIStudioPromptPackage TerminalPackage;
		FText TerminalPackageMessage;
		const bool bTerminalPackageOk = Service.CreateTestPromptPackage(TEXT("agent-connection"), TEXT("Claude Code"), TerminalPackage, TerminalPackageMessage);
		FHyperAIStudioPromptPackage CodexPackage;
		FText CodexPackageMessage;
		const bool bCodexPackageOk = Service.CreateTestPromptPackage(TEXT("agent-connection"), TEXT("Codex"), CodexPackage, CodexPackageMessage);
		FHyperAIStudioPromptPackage MissingGeminiPackage;
		FText MissingGeminiPackageMessage;
		const bool bMissingGeminiPackageOk = Service.CreateTestPromptPackage(TEXT("agent-connection"), TEXT("Gemini"), MissingGeminiPackage, MissingGeminiPackageMessage);

		auto PlanHasNoCmdWrapper = [](const FString& ExecutablePath, const FString& Arguments)
		{
			return !ExecutablePath.EndsWith(TEXT("cmd.exe"), ESearchCase::IgnoreCase)
				&& !Arguments.Contains(TEXT("/c start"), ESearchCase::IgnoreCase)
				&& !Arguments.Contains(TEXT("cmd.exe"), ESearchCase::IgnoreCase);
		};
		auto TerminalPlanHasPromptScript = [](const FHyperAIStudioTerminalLaunchPlan& Plan, const FHyperAIStudioPromptPackage& Package)
		{
			return Plan.Arguments.Contains(TEXT("Send-HyperAIStudioPrompt.ps1"))
				&& Plan.Arguments.Contains(FString::Printf(TEXT("-Agent \"%s\""), *Package.AgentName))
				&& Plan.Arguments.Contains(Package.PromptPath);
		};

		FHyperAIStudioTerminalLaunchPlan WtPlan;
		FText WtMessage;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakeWtDir);
		const bool bWtPlanOk = Service.BuildTerminalLaunchPlanForPrompt(TerminalPackage, WtPlan, WtMessage);
		const bool bWtRouteOk = bWtPlanOk
			&& WtPlan.bSuccess
			&& WtPlan.bUsesWindowsTerminal
			&& !WtPlan.bUsesPowerShell
			&& WtPlan.RouteName == TEXT("Windows Terminal")
			&& FPaths::GetCleanFilename(WtPlan.ExecutablePath).StartsWith(TEXT("wt"), ESearchCase::IgnoreCase)
			&& WtPlan.Arguments.Contains(TEXT("-d "))
			&& WtPlan.Arguments.Contains(TEXT("powershell "))
			&& WtPlan.WorkingDirectory == FHyperAIStudioService::GetProjectRoot()
			&& TerminalPlanHasPromptScript(WtPlan, TerminalPackage)
			&& PlanHasNoCmdWrapper(WtPlan.ExecutablePath, WtPlan.Arguments);

		FHyperAIStudioTerminalLaunchPlan PowerShellPlan;
		FText PowerShellMessage;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakeNoWtDir);
		const bool bPowerShellPlanOk = Service.BuildTerminalLaunchPlanForPrompt(TerminalPackage, PowerShellPlan, PowerShellMessage);
		const bool bPowerShellRouteOk = bPowerShellPlanOk
			&& PowerShellPlan.bSuccess
			&& !PowerShellPlan.bUsesWindowsTerminal
			&& PowerShellPlan.bUsesPowerShell
			&& PowerShellPlan.RouteName == TEXT("PowerShell")
			&& FPaths::GetCleanFilename(PowerShellPlan.ExecutablePath).Contains(TEXT("powershell"), ESearchCase::IgnoreCase)
			&& !PowerShellPlan.Arguments.Contains(TEXT("-d "))
			&& PowerShellPlan.WorkingDirectory == FHyperAIStudioService::GetProjectRoot()
			&& TerminalPlanHasPromptScript(PowerShellPlan, TerminalPackage)
			&& PlanHasNoCmdWrapper(PowerShellPlan.ExecutablePath, PowerShellPlan.Arguments);

		FHyperAIStudioTerminalLaunchPlan CodexPlan;
		FText CodexMessage;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakeCodexDir);
		const bool bCodexPlanOk = Service.BuildTerminalLaunchPlanForPrompt(CodexPackage, CodexPlan, CodexMessage);
		const FString ResolvedCodexLaunch = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Codex"));
		const FString ResolvedClaudeLaunch = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Claude Code"));
		const bool bCodexCliRouteOk = bCodexPlanOk
			&& CodexPlan.bSuccess
			&& CodexPlan.bUsesPowerShell
			&& CodexPlan.RouteName == TEXT("PowerShell")
			&& FPaths::GetCleanFilename(CodexPlan.ExecutablePath).Contains(TEXT("powershell"), ESearchCase::IgnoreCase)
			&& TerminalPlanHasPromptScript(CodexPlan, CodexPackage)
			&& !CodexPlan.Arguments.Contains(TEXT(" app "))
			&& CodexPlan.WorkingDirectory == FHyperAIStudioService::GetProjectRoot()
			&& PlanHasNoCmdWrapper(CodexPlan.ExecutablePath, CodexPlan.Arguments);
		const bool bResolvedAgentLaunchOk = ResolvedCodexLaunch.Contains(TEXT("codex.cmd"))
			&& ResolvedCodexLaunch.Contains(TEXT("-c tui.alternate_screen=never"))
			&& ResolvedClaudeLaunch.Contains(TEXT("claude.cmd"))
			&& !ResolvedClaudeLaunch.Contains(TEXT("tui.alternate_screen"));

		FText MissingGeminiMessage;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakeNoWtDir);
		const bool bMissingGeminiHandled = bMissingGeminiPackageOk
			&& Service.OpenTerminalForPrompt(MissingGeminiPackage, MissingGeminiMessage)
			&& MissingGeminiMessage.ToString().Contains(TEXT("Gemini CLI was not found"))
			&& MissingGeminiMessage.ToString().Contains(TEXT("Prompt copied"));

		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *OriginalTestPath);
		const bool bRestored = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_TEST_PATH")) == OriginalTestPath;
		const bool bNoCmdOk = PlanHasNoCmdWrapper(WtPlan.ExecutablePath, WtPlan.Arguments)
			&& PlanHasNoCmdWrapper(PowerShellPlan.ExecutablePath, PowerShellPlan.Arguments)
			&& PlanHasNoCmdWrapper(CodexPlan.ExecutablePath, CodexPlan.Arguments);
		const bool bOk = bDirsOk
			&& bFakeWtOk
			&& bFakeCodexOk
			&& bFakeClaudeOk
			&& bTerminalPackageOk
			&& bCodexPackageOk
			&& bWtRouteOk
			&& bPowerShellRouteOk
			&& bCodexCliRouteOk
			&& bResolvedAgentLaunchOk
			&& bMissingGeminiHandled
			&& bNoCmdOk
			&& bRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.TerminalLaunchPlanSmoke %s terminalPackage=%s codexPackage=%s missingGeminiPackage=%s wt=%s powershell=%s codexCli=%s resolvedAgents=%s missingGemini=%s noCmd=%s restored=%s wtRoute=%s wtExe=%s powerRoute=%s powerExe=%s codexRoute=%s codexExe=%s codexLaunch=%s claudeLaunch=%s terminalPackageMessage=%s codexPackageMessage=%s missingGeminiMessage=%s wtMessage=%s powerMessage=%s codexMessage=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bTerminalPackageOk ? TEXT("true") : TEXT("false"),
			bCodexPackageOk ? TEXT("true") : TEXT("false"),
			bMissingGeminiPackageOk ? TEXT("true") : TEXT("false"),
			bWtRouteOk ? TEXT("true") : TEXT("false"),
			bPowerShellRouteOk ? TEXT("true") : TEXT("false"),
			bCodexCliRouteOk ? TEXT("true") : TEXT("false"),
			bResolvedAgentLaunchOk ? TEXT("true") : TEXT("false"),
			bMissingGeminiHandled ? TEXT("true") : TEXT("false"),
			bNoCmdOk ? TEXT("true") : TEXT("false"),
			bRestored ? TEXT("true") : TEXT("false"),
			*WtPlan.RouteName,
			*WtPlan.ExecutablePath,
			*PowerShellPlan.RouteName,
			*PowerShellPlan.ExecutablePath,
			*CodexPlan.RouteName,
			*CodexPlan.ExecutablePath,
			*ResolvedCodexLaunch,
			*ResolvedClaudeLaunch,
			*TerminalPackageMessage.ToString(),
			*CodexPackageMessage.ToString(),
			*MissingGeminiMessage.ToString(),
			*WtMessage.ToString(),
			*PowerShellMessage.ToString(),
			*CodexMessage.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleCopyCodexTomlPatch(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		FHyperAIStudioService Service;
		const FString Patch = Service.GetCodexTomlPatch();
		Service.CopyTextToClipboard(Patch);

		const bool bBeginOk = Patch.Contains(TEXT("# BEGIN HYPERAISTUDIO MANAGED MCP"));
		const bool bEndOk = Patch.Contains(TEXT("# END HYPERAISTUDIO MANAGED MCP"));
		const bool bUnrealMcpOk = Patch.Contains(TEXT("[mcp_servers.\"unreal-mcp\"]"));

		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const TArray<FHyperAIStudioMCPServerEntry> OriginalServers = Settings
			? Settings->ExtraServers
			: TArray<FHyperAIStudioMCPServerEntry>();
		const int32 OriginalMigrationVersion = Settings ? Settings->NativeToolArchitectureMigrationVersion : 0;
		const bool bOriginalCodex = Settings ? Settings->bGenerateCodexConfig : false;
		const bool bOriginalClaude = Settings ? Settings->bGenerateClaudeConfig : false;
		const bool bOriginalClaudeMarkdown = Settings ? Settings->bGenerateClaudeMarkdown : false;
		const bool bOriginalCursor = Settings ? Settings->bGenerateCursorConfig : false;
		const bool bOriginalVSCode = Settings ? Settings->bGenerateVSCodeConfig : false;
		const bool bOriginalGemini = Settings ? Settings->bGenerateGeminiConfig : false;
		const bool bOriginalEnableCodex = Settings ? Settings->bEnableCodexAgent : false;
		bool bSeedOk = false;
		bool bGenerateOk = false;
		bool bGeneratedLoaded = false;
		bool bMatchesGenerated = false;
		bool bPreserveOk = false;
		bool bFilesRestored = false;
		bool bNoBackupArtifacts = false;
		bool bSettingsRestored = false;
		FText GenerateMessage = FText::FromString(TEXT("settings unavailable"));

		if (Settings)
		{
			const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
			const FString AgentsPath = FPaths::Combine(ProjectRoot, TEXT("AGENTS.md"));
			const FString ClaudeMarkdownPath = FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md"));
			const FString CodexPath = FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml"));
			const FString ClaudeMcpPath = FPaths::Combine(ProjectRoot, TEXT(".mcp.json"));
			const FString CursorPath = FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json"));
			const FString VSCodePath = FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json"));
			const FString GeminiPath = FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json"));
			const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
			const TArray<FString> ManagedFilePaths = {
				FPaths::Combine(ProjectRoot, TEXT(".gitignore")),
				AgentsPath,
				ClaudeMarkdownPath,
				CodexPath,
				ClaudeMcpPath,
				CursorPath,
				VSCodePath,
				GeminiPath,
				FPaths::Combine(HyperDir, TEXT("state"), TEXT("managed-config-state.json")),
				FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-pending.json")),
				FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("managed-config-settings-pending.json")),
				FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Start-HyperAIStudioEditor.ps1")),
				FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1")),
				FPaths::Combine(HyperDir, TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1")),
				FPaths::Combine(HyperDir, TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md")),
				FPaths::Combine(HyperDir, TEXT("README.md")),
				FPaths::Combine(HyperDir, TEXT("INDEX.md")),
				FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("hyperai-status.json")),
				FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.a.json")),
				FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("operation-journal.b.json")),
				FPaths::Combine(HyperDir, TEXT("managed-config-state.json")),
				FPaths::Combine(HyperDir, TEXT("managed-config-pending.json")),
				FPaths::Combine(HyperDir, TEXT("managed-config-settings-pending.json")),
				FPaths::Combine(HyperDir, TEXT("Start-HyperAIStudioEditor.ps1")),
				FPaths::Combine(HyperDir, TEXT("Wait-HyperAIStudioMCP.ps1")),
				FPaths::Combine(HyperDir, TEXT("Send-HyperAIStudioPrompt.ps1")),
				FPaths::Combine(HyperDir, TEXT("HyperAIStudio-TestCommands.md")),
				FPaths::Combine(HyperDir, TEXT("hyperai-status.json"))
			};
			const TArray<FHyperAIStudioSavedFile> SavedFiles = SaveFilesForSmoke(ManagedFilePaths);
			if (!AreFileSnapshotsCompleteForSmoke(SavedFiles))
			{
				UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.CopyCodexTomlPatch failed: a managed-file snapshot could not be captured; no file or settings mutations were made."));
				if (bQuitOnComplete)
				{
					RequestSmokeExit();
				}
				return;
			}
			TArray<FString> BackupSourcePaths = ManagedFilePaths;
			if (!GEditorPerProjectIni.IsEmpty())
			{
				BackupSourcePaths.Add(GEditorPerProjectIni);
			}
			const TArray<FHyperAIStudioSavedBackupDirectory> SavedBackupDirectories =
				SaveBackupDirectoriesForSmoke(BackupSourcePaths);
			if (!AreBackupDirectorySnapshotsCompleteForSmoke(SavedBackupDirectories))
			{
				UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.CopyCodexTomlPatch failed: backup-artifact state could not be captured; no file or settings mutations were made."));
				if (bQuitOnComplete)
				{
					RequestSmokeExit();
				}
				return;
			}

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(CodexPath), true);
			bSeedOk = FFileHelper::SaveStringToFile(TEXT("# HYPERAI_USER_CODEX_PATCH_SENTINEL\n"), *CodexPath);

			Settings->bGenerateCodexConfig = true;
			Settings->bEnableCodexAgent = true;
			Settings->SaveConfig();
			bGenerateOk = Service.GenerateProjectFiles(GenerateMessage);

			FString GeneratedCodexText;
			bGeneratedLoaded = FFileHelper::LoadFileToString(GeneratedCodexText, *CodexPath);
			bMatchesGenerated = bGeneratedLoaded && GeneratedCodexText.Contains(Patch);
			bPreserveOk = bGeneratedLoaded && GeneratedCodexText.Contains(TEXT("HYPERAI_USER_CODEX_PATCH_SENTINEL"));
			bFilesRestored = RestoreFilesForSmoke(SavedFiles);

			Settings->ExtraServers = OriginalServers;
			Settings->NativeToolArchitectureMigrationVersion = OriginalMigrationVersion;
			Settings->bGenerateCodexConfig = bOriginalCodex;
			Settings->bGenerateClaudeConfig = bOriginalClaude;
			Settings->bGenerateClaudeMarkdown = bOriginalClaudeMarkdown;
			Settings->bGenerateCursorConfig = bOriginalCursor;
			Settings->bGenerateVSCodeConfig = bOriginalVSCode;
			Settings->bGenerateGeminiConfig = bOriginalGemini;
			Settings->bEnableCodexAgent = bOriginalEnableCodex;
			Settings->SaveConfig();
			bNoBackupArtifacts = AreBackupDirectoriesUnchangedForSmoke(SavedBackupDirectories);
			const auto EntriesMatch = [](const FHyperAIStudioMCPServerEntry& A, const FHyperAIStudioMCPServerEntry& B)
			{
				return A.Id == B.Id
					&& A.DisplayName == B.DisplayName
					&& A.Description == B.Description
					&& A.Transport == B.Transport
					&& A.Url == B.Url
					&& A.Command == B.Command
					&& A.Arguments == B.Arguments
					&& A.Priority == B.Priority
					&& A.Domains == B.Domains
					&& A.TargetClients == B.TargetClients
					&& A.AgentInstructions == B.AgentInstructions
					&& A.SetupNotes == B.SetupNotes
					&& A.bEnabled == B.bEnabled;
			};
			bSettingsRestored = Settings->ExtraServers.Num() == OriginalServers.Num()
				&& Settings->NativeToolArchitectureMigrationVersion == OriginalMigrationVersion
				&& Settings->bGenerateCodexConfig == bOriginalCodex
				&& Settings->bGenerateClaudeConfig == bOriginalClaude
				&& Settings->bGenerateClaudeMarkdown == bOriginalClaudeMarkdown
				&& Settings->bGenerateCursorConfig == bOriginalCursor
				&& Settings->bGenerateVSCodeConfig == bOriginalVSCode
				&& Settings->bGenerateGeminiConfig == bOriginalGemini
				&& Settings->bEnableCodexAgent == bOriginalEnableCodex;
			for (int32 Index = 0; bSettingsRestored && Index < OriginalServers.Num(); ++Index)
			{
				bSettingsRestored = EntriesMatch(Settings->ExtraServers[Index], OriginalServers[Index]);
			}
		}

		const bool bOk = bBeginOk
			&& bEndOk
			&& bUnrealMcpOk
			&& bSeedOk
			&& bGenerateOk
			&& bMatchesGenerated
			&& bPreserveOk
			&& bFilesRestored
			&& bNoBackupArtifacts
			&& bSettingsRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.CopyCodexTomlPatch %s copied %d characters begin=%s end=%s unrealMcp=%s generated=%s matchesGenerated=%s preserve=%s filesRestored=%s noBackupArtifacts=%s settingsRestored=%s message=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			Patch.Len(),
			bBeginOk ? TEXT("true") : TEXT("false"),
			bEndOk ? TEXT("true") : TEXT("false"),
			bUnrealMcpOk ? TEXT("true") : TEXT("false"),
			bGenerateOk ? TEXT("true") : TEXT("false"),
			bMatchesGenerated ? TEXT("true") : TEXT("false"),
			bPreserveOk ? TEXT("true") : TEXT("false"),
			bFilesRestored ? TEXT("true") : TEXT("false"),
			bNoBackupArtifacts ? TEXT("true") : TEXT("false"),
			bSettingsRestored ? TEXT("true") : TEXT("false"),
			*GenerateMessage.ToString());

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleStartMCP()
	{
		FHyperAIStudioService Service;
		FText Message;
		const bool bOk = Service.StartUnrealMCP(Message);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.StartMCP %s: %s"), bOk ? TEXT("succeeded") : TEXT("failed"), *Message.ToString());
	}

	void RunConsoleStatus()
	{
		FHyperAIStudioService Service;
		const FHyperAIStudioStatus Status = Service.GetStatusSync();
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.Status ready=%s endpoint=%s module=%s settings=%s requiredPlugins=%s server=%s port=%s toolsList=%s probeRun=%s agentFiles=%s tools=%d probeTools=%d"),
			Status.IsReady() ? TEXT("true") : TEXT("false"),
			*Status.Endpoint,
			Status.bUnrealMCPModuleAvailable ? TEXT("true") : TEXT("false"),
			Status.bUnrealMCPSettingsConfigured ? TEXT("true") : TEXT("false"),
			Status.bRequiredPluginsReady ? TEXT("true") : TEXT("false"),
			Status.bServerRunning ? TEXT("true") : TEXT("false"),
			Status.bPortListening ? TEXT("true") : TEXT("false"),
			Status.bToolsListReachable ? TEXT("true") : TEXT("false"),
			Status.bHasProbeRun ? TEXT("true") : TEXT("false"),
			Status.bAgentFilesReady ? TEXT("true") : TEXT("false"),
			Status.RegisteredToolCount,
			Status.ProbeToolCount);
	}

	void RunConsoleStatusProbe(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.StatusProbe queued quitOnComplete=%s"),
			bQuitOnComplete ? TEXT("true") : TEXT("false"));

		const TSharedRef<FHyperAIStudioService> Service = MakeShared<FHyperAIStudioService>();
		Service->RefreshStatusAsync([this, Service, bQuitOnComplete](const FHyperAIStudioStatus& Status)
		{
			if (Status.bProbeInProgress || Status.bCapabilityInventoryInProgress)
			{
				UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.StatusProbe checking endpoint=%s module=%s settings=%s requiredPlugins=%s server=%s port=%s agentFiles=%s tools=%d shallowPending=%s inventoryPending=%s"),
					*Status.Endpoint,
					Status.bUnrealMCPModuleAvailable ? TEXT("true") : TEXT("false"),
					Status.bUnrealMCPSettingsConfigured ? TEXT("true") : TEXT("false"),
					Status.bRequiredPluginsReady ? TEXT("true") : TEXT("false"),
					Status.bServerRunning ? TEXT("true") : TEXT("false"),
					Status.bPortListening ? TEXT("true") : TEXT("false"),
					Status.bAgentFilesReady ? TEXT("true") : TEXT("false"),
					Status.RegisteredToolCount,
					Status.bProbeInProgress ? TEXT("true") : TEXT("false"),
					Status.bCapabilityInventoryInProgress ? TEXT("true") : TEXT("false"));
				return;
			}

			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.StatusProbe %s ready=%s endpoint=%s module=%s settings=%s requiredPlugins=%s server=%s port=%s toolsList=%s agentFiles=%s tools=%d probeTools=%d inventory=%s toolSearch=%s truncated=%s toolsets=%d described=%d inventoryTools=%d fingerprint=%s message=%s inventoryMessage=%s"),
				Status.IsReady() ? TEXT("succeeded") : TEXT("failed"),
				Status.IsReady() ? TEXT("true") : TEXT("false"),
				*Status.Endpoint,
				Status.bUnrealMCPModuleAvailable ? TEXT("true") : TEXT("false"),
				Status.bUnrealMCPSettingsConfigured ? TEXT("true") : TEXT("false"),
				Status.bRequiredPluginsReady ? TEXT("true") : TEXT("false"),
				Status.bServerRunning ? TEXT("true") : TEXT("false"),
				Status.bPortListening ? TEXT("true") : TEXT("false"),
				Status.bToolsListReachable ? TEXT("true") : TEXT("false"),
				Status.bAgentFilesReady ? TEXT("true") : TEXT("false"),
				Status.RegisteredToolCount,
				Status.ProbeToolCount,
				Status.bCapabilityInventoryAvailable ? TEXT("true") : TEXT("false"),
				Status.bToolSearchMode ? TEXT("true") : TEXT("false"),
				Status.bCapabilityInventoryTruncated ? TEXT("true") : TEXT("false"),
				Status.DiscoverableToolsetCount,
				Status.DescribedToolsetCount,
				Status.InventoryToolCount,
				Status.CapabilityInventoryFingerprint.IsEmpty() ? TEXT("(none)") : *Status.CapabilityInventoryFingerprint,
				Status.ProbeMessage.IsEmpty() ? TEXT("(none)") : *Status.ProbeMessage,
				Status.CapabilityInventoryMessage.IsEmpty() ? TEXT("(none)") : *Status.CapabilityInventoryMessage);

			if (bQuitOnComplete)
			{
				RequestSmokeExit();
			}
		});
	}

	void RunConsolePrerequisites()
	{
		FHyperAIStudioService Service;
		for (const FHyperAIStudioPrerequisiteStatus& Entry : Service.GetPrerequisites())
		{
			UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.Prerequisite id=%s required=%s available=%s enabled=%s canEnable=%s restart=%s action=%s"),
				*Entry.Id,
				Entry.bRequired ? TEXT("true") : TEXT("false"),
				Entry.bAvailable ? TEXT("true") : TEXT("false"),
				Entry.bEnabled ? TEXT("true") : TEXT("false"),
				Entry.bCanEnable ? TEXT("true") : TEXT("false"),
				Entry.bRestartRequired ? TEXT("true") : TEXT("false"),
				*Entry.ActionHint);
		}
	}

	void RunConsolePrerequisiteGuidanceSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		const FString OriginalTestPath = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_TEST_PATH"));
		const FString OriginalIgnoreCodexConfig = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"));
		const FString SmokeRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HyperAIStudio"), TEXT("PrerequisiteGuidanceSmoke"));
		const FString EmptyPath = FPaths::Combine(SmokeRoot, TEXT("empty"));
		const FString FakePath = FPaths::Combine(SmokeRoot, TEXT("fake-bin"));
		const bool bEmptyDirOk = IFileManager::Get().MakeDirectory(*EmptyPath, true);
		const bool bFakeDirOk = IFileManager::Get().MakeDirectory(*FakePath, true);

		const TMap<FString, FString> ExternalTools = {
			{ TEXT("codex"), TEXT("codex") },
			{ TEXT("claude"), TEXT("claude") },
			{ TEXT("gemini"), TEXT("gemini") },
			{ TEXT("cursor"), TEXT("cursor") },
			{ TEXT("vscode"), TEXT("code") },
			{ TEXT("git"), TEXT("git") },
			{ TEXT("node"), TEXT("node") },
			{ TEXT("npm"), TEXT("npm") },
			{ TEXT("npx"), TEXT("npx") },
			{ TEXT("uv"), TEXT("uv") }
		};
		const TArray<FString> RequiredPluginIds = {
			TEXT("ModelContextProtocol"),
			TEXT("ToolsetRegistry"),
			TEXT("EditorToolset"),
			TEXT("MCPClientToolset"),
			TEXT("Terminal")
		};
		const TArray<FString> AgentIds = {
			TEXT("codex"),
			TEXT("claude"),
			TEXT("gemini"),
			TEXT("cursor"),
			TEXT("vscode")
		};

		bool bFakeFilesOk = bFakeDirOk;
		for (const TPair<FString, FString>& Tool : ExternalTools)
		{
			const FString StubPath = FPaths::Combine(FakePath, Tool.Value + TEXT(".cmd"));
			bFakeFilesOk = FFileHelper::SaveStringToFile(TEXT("@echo off\r\n"), *StubPath) && bFakeFilesOk;
		}

		auto FindPrerequisite = [](const TArray<FHyperAIStudioPrerequisiteStatus>& Entries, const FString& Id) -> const FHyperAIStudioPrerequisiteStatus*
		{
			return Entries.FindByPredicate([&Id](const FHyperAIStudioPrerequisiteStatus& Entry)
			{
				return Entry.Id == Id;
			});
		};

		FHyperAIStudioService Service;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"), TEXT("1"));
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *EmptyPath);
		const TArray<FHyperAIStudioPrerequisiteStatus> MissingEntries = Service.GetPrerequisites();

		int32 RequiredPluginCount = 0;
		bool bRequiredPluginGuidanceOk = true;
		for (const FString& Id : RequiredPluginIds)
		{
			const FHyperAIStudioPrerequisiteStatus* Entry = FindPrerequisite(MissingEntries, Id);
			if (Entry)
			{
				++RequiredPluginCount;
				bRequiredPluginGuidanceOk = bRequiredPluginGuidanceOk
					&& Entry->bRequired
					&& !Entry->DisplayName.IsEmpty()
					&& !Entry->ActionHint.IsEmpty()
					&& Entry->DocumentationUrl.StartsWith(TEXT("http"));
			}
			else
			{
				bRequiredPluginGuidanceOk = false;
			}
		}

		int32 MissingToolCount = 0;
		bool bMissingGuidanceOk = true;
		for (const TPair<FString, FString>& Tool : ExternalTools)
		{
			const FHyperAIStudioPrerequisiteStatus* Entry = FindPrerequisite(MissingEntries, Tool.Key);
			if (Entry)
			{
				if (!Entry->bAvailable && !Entry->bEnabled)
				{
					++MissingToolCount;
				}
				bMissingGuidanceOk = bMissingGuidanceOk
					&& !Entry->bRequired
					&& !Entry->bAvailable
					&& !Entry->bEnabled
					&& !Entry->DisplayName.IsEmpty()
					&& !Entry->ActionHint.IsEmpty()
					&& !Entry->InstallCommand.IsEmpty()
					&& Entry->DocumentationUrl.StartsWith(TEXT("http"));
			}
			else
			{
				bMissingGuidanceOk = false;
			}
		}

		bool bAgentGuidanceOk = true;
		for (const FString& Id : AgentIds)
		{
			const FHyperAIStudioPrerequisiteStatus* Entry = FindPrerequisite(MissingEntries, Id);
			bAgentGuidanceOk = bAgentGuidanceOk
				&& Entry
				&& Entry->Detail.Contains(TEXT("External"))
				&& Entry->ActionHint.Contains(TEXT("Install"))
				&& !Entry->InstallCommand.IsEmpty()
				&& Entry->DocumentationUrl.StartsWith(TEXT("http"));
		}

		const FHyperAIStudioPrerequisiteStatus* MissingGeminiEntry = FindPrerequisite(MissingEntries, TEXT("gemini"));
		const FString GeminiInstallerCommand = MissingGeminiEntry
			? SHyperAIStudioWindow::BuildPrerequisiteInstallTerminalCommandForSmoke(MissingGeminiEntry->Id, MissingGeminiEntry->DisplayName, MissingGeminiEntry->InstallCommand, TEXT("D:/HyperAIStudioSmokeProject"))
			: FString();
		const bool bGeminiInstallerGuidanceOk = MissingGeminiEntry
			&& MissingGeminiEntry->ActionHint.Contains(TEXT("Node.js 20+"))
			&& !GeminiInstallerCommand.StartsWith(TEXT("@echo off"))
			&& !GeminiInstallerCommand.Contains(TEXT("cmd /d /s /k"))
			&& !GeminiInstallerCommand.Contains(TEXT("TERM=xterm-256color"))
			&& !GeminiInstallerCommand.Contains(TEXT("cd /d"))
			&& GeminiInstallerCommand.Contains(TEXT("where npm >nul 2>nul"))
			&& GeminiInstallerCommand.Contains(TEXT("npm install -g @google/gemini-cli"))
			&& GeminiInstallerCommand.Contains(TEXT("Gemini CLI install needs Node.js 20+ and npm"))
			&& !GeminiInstallerCommand.Contains(TEXT("InstallScripts"))
			&& !GeminiInstallerCommand.Contains(TEXT("-File"));

		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakePath);
		const TArray<FHyperAIStudioPrerequisiteStatus> InstalledEntries = Service.GetPrerequisites();

		int32 InstalledToolCount = 0;
		bool bInstalledRoutesOk = true;
		for (const TPair<FString, FString>& Tool : ExternalTools)
		{
			const FHyperAIStudioPrerequisiteStatus* Entry = FindPrerequisite(InstalledEntries, Tool.Key);
			if (Entry)
			{
				if (Entry->bAvailable && Entry->bEnabled)
				{
					++InstalledToolCount;
				}
				bInstalledRoutesOk = bInstalledRoutesOk
					&& Entry->bAvailable
					&& Entry->bEnabled
					&& Entry->ActionHint.Contains(TEXT("Available on PATH"))
					&& !Entry->InstallCommand.IsEmpty()
					&& Entry->DocumentationUrl.StartsWith(TEXT("http"));
			}
			else
			{
				bInstalledRoutesOk = false;
			}
		}

		const FString CodexConfigPath = FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), TEXT(".codex"), TEXT("config.toml"));
		const TArray<FHyperAIStudioSavedFile> CodexConfigSnapshot = SaveFilesForSmoke({ CodexConfigPath });
		const bool bCodexConfigDirOk = IFileManager::Get().MakeDirectory(*FPaths::GetPath(CodexConfigPath), true);
		const bool bInvalidCodexConfigFileOk = bCodexConfigDirOk
			&& FFileHelper::SaveStringToFile(TEXT("service_tier = \"priority\"\n"), *CodexConfigPath);
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"), TEXT(""));
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *FakePath);
		const TArray<FHyperAIStudioPrerequisiteStatus> InvalidCodexConfigEntries = Service.GetPrerequisites();
		const FHyperAIStudioPrerequisiteStatus* InvalidCodexEntry = FindPrerequisite(InvalidCodexConfigEntries, TEXT("codex"));
		const bool bInvalidCodexConfigGuidanceOk = bInvalidCodexConfigFileOk
			&& InvalidCodexEntry
			&& InvalidCodexEntry->bAvailable
			&& InvalidCodexEntry->bEnabled
			&& InvalidCodexEntry->ActionHint.Contains(TEXT("unsupported service_tier"))
			&& InvalidCodexEntry->ActionHint.Contains(TEXT("service_tier=fast"))
			&& InvalidCodexEntry->ActionHint.Contains(TEXT("config"));
		const bool bCodexConfigRestored = RestoreFilesForSmoke(CodexConfigSnapshot);
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"), TEXT("1"));

		const FString TerminalHint = SHyperAIStudioWindow::GetTerminalPanelHintText().ToString();
		const FString TerminalRecipe = SHyperAIStudioWindow::GetTerminalStartupRecipeText(TEXT("D:/HyperAIStudioSmokeProject"));
		const bool bTerminalFallbackGuidanceOk = TerminalHint.Contains(TEXT("enable the Terminal plugin"))
			&& TerminalHint.Contains(TEXT("restart the editor"))
			&& TerminalHint.Contains(TEXT("Copy Startup Commands"))
			&& TerminalHint.Contains(TEXT("Windows Terminal"))
			&& TerminalHint.Contains(TEXT("PowerShell"))
			&& TerminalRecipe.Contains(TEXT("TERM=xterm-256color"))
			&& TerminalRecipe.Contains(TEXT("cd /d \"D:/HyperAIStudioSmokeProject\""))
			&& TerminalRecipe.Contains(TEXT("codex"))
			&& TerminalRecipe.Contains(TEXT("claude"))
			&& TerminalRecipe.Contains(TEXT("gemini"))
			&& TerminalRecipe.Contains(TEXT("cursor ."))
			&& TerminalRecipe.Contains(TEXT("code ."));

		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_TEST_PATH"), *OriginalTestPath);
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"), *OriginalIgnoreCodexConfig);
		const bool bRestored = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_TEST_PATH")) == OriginalTestPath;
		const bool bIgnoreRestored = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG")) == OriginalIgnoreCodexConfig;
		const bool bOk = bEmptyDirOk
			&& bFakeDirOk
			&& bFakeFilesOk
			&& RequiredPluginCount == RequiredPluginIds.Num()
			&& bRequiredPluginGuidanceOk
			&& MissingToolCount == ExternalTools.Num()
			&& bMissingGuidanceOk
			&& bAgentGuidanceOk
			&& bGeminiInstallerGuidanceOk
			&& bTerminalFallbackGuidanceOk
			&& bInvalidCodexConfigGuidanceOk
			&& bCodexConfigRestored
			&& InstalledToolCount == ExternalTools.Num()
			&& bInstalledRoutesOk
			&& bRestored
			&& bIgnoreRestored;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.PrerequisiteGuidanceSmoke %s requiredPlugins=%d/%d requiredGuidance=%s missingTools=%d/%d missingGuidance=%s agentGuidance=%s geminiInstaller=%s terminalFallback=%s codexConfig=%s codexConfigRestored=%s installedTools=%d/%d installedRoutes=%s fakeFiles=%s restored=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			RequiredPluginCount,
			RequiredPluginIds.Num(),
			bRequiredPluginGuidanceOk ? TEXT("true") : TEXT("false"),
			MissingToolCount,
			ExternalTools.Num(),
			bMissingGuidanceOk ? TEXT("true") : TEXT("false"),
			bAgentGuidanceOk ? TEXT("true") : TEXT("false"),
			bGeminiInstallerGuidanceOk ? TEXT("true") : TEXT("false"),
			bTerminalFallbackGuidanceOk ? TEXT("true") : TEXT("false"),
			bInvalidCodexConfigGuidanceOk ? TEXT("true") : TEXT("false"),
			bCodexConfigRestored ? TEXT("true") : TEXT("false"),
			InstalledToolCount,
			ExternalTools.Num(),
			bInstalledRoutesOk ? TEXT("true") : TEXT("false"),
			bFakeFilesOk ? TEXT("true") : TEXT("false"),
			(bRestored && bIgnoreRestored) ? TEXT("true") : TEXT("false"));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleOpen()
	{
		OpenTab();
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.Open invoked."));
	}

	void RunConsoleOpenChat(const TArray<FString>& Args)
	{
		const FString AgentName = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
		OpenChatTabForAgent(AgentName);
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.Chat invoked%s%s."),
			AgentName.IsEmpty() ? TEXT("") : TEXT(" for "),
			AgentName.IsEmpty() ? TEXT("") : *AgentName);
	}

	void RunConsoleQuickAction()
	{
		const bool bOpened = OpenQuickActionWindow();
		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.QuickAction chat overlay %s."),
			bOpened ? TEXT("opened") : TEXT("skipped: Slate unavailable or unattended"));
	}

	void RunConsoleQuickActionSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
			}
		}

		FHyperAIStudioStatus ReadyStatus;
		ReadyStatus.bUnrealMCPModuleAvailable = true;
		ReadyStatus.bUnrealMCPSettingsConfigured = true;
		ReadyStatus.bRequiredPluginsReady = true;
		ReadyStatus.bDesiredCapabilityPluginsReady = true;
		ReadyStatus.bServerRunning = true;
		ReadyStatus.bPortListening = true;
		ReadyStatus.bToolsListReachable = true;
		ReadyStatus.bAgentFilesReady = true;
		ReadyStatus.ConfiguredAgentCount = 1;
		ReadyStatus.Endpoint = TEXT("http://127.0.0.1:8000/mcp");

		FHyperAIStudioStatus CheckingStatus;
		CheckingStatus.bUnrealMCPModuleAvailable = true;
		CheckingStatus.bUnrealMCPSettingsConfigured = true;
		CheckingStatus.bServerRunning = true;
		CheckingStatus.bPortListening = true;
		CheckingStatus.bAgentFilesReady = true;
		CheckingStatus.bProbeInProgress = true;
		CheckingStatus.ConfiguredAgentCount = 1;

		FHyperAIStudioStatus MissingModuleStatus;

		FHyperAIStudioStatus SetupNeededStatus;
		SetupNeededStatus.bUnrealMCPModuleAvailable = true;

		FHyperAIStudioStatus ServerNeededStatus;
		ServerNeededStatus.bUnrealMCPModuleAvailable = true;
		ServerNeededStatus.bUnrealMCPSettingsConfigured = true;
		ServerNeededStatus.bAgentFilesReady = true;
		ServerNeededStatus.ConfiguredAgentCount = 1;

		const FString SmokeProjectRoot = TEXT("D:/HyperAIStudioSmokeProject");
		const FString ReadyBadge = SHyperAIStudioQuickActionWindow::GetReadinessTextForStatus(ReadyStatus, false).ToString();
		const FString CheckingBadge = SHyperAIStudioQuickActionWindow::GetReadinessTextForStatus(CheckingStatus, false).ToString();
		const FString SetupBadge = SHyperAIStudioQuickActionWindow::GetReadinessTextForStatus(MissingModuleStatus, false).ToString();
		const FString ReadyNext = SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(ReadyStatus, false).ToString();
		const FString CheckingNext = SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(CheckingStatus, false).ToString();
		const FString ModuleNext = SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(MissingModuleStatus, false).ToString();
		const FString SetupNext = SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(SetupNeededStatus, false).ToString();
		const FString ServerNext = SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(ServerNeededStatus, false).ToString();

		const bool bReadyOk = ReadyBadge == TEXT("Ready") && ReadyNext.Contains(TEXT("embedded agent terminal"));
		const bool bCheckingOk = CheckingBadge == TEXT("Checking") && CheckingNext.Contains(TEXT("wait for the MCP readiness probe"));
		const bool bModuleOk = SetupBadge == TEXT("Setup needed") && ModuleNext.Contains(TEXT("enable the required Unreal MCP plugins"));
		const bool bSetupOk = SetupNext.Contains(TEXT("start setup to configure Unreal MCP"));
		const bool bServerOk = ServerNext.Contains(TEXT("start or refresh Unreal MCP"));
		const FString ReadyTerminalCommand = SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(ReadyStatus, TEXT("Codex"), SmokeProjectRoot, TEXT("codex"));
		const FString SetupTerminalCommand = SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(SetupNeededStatus, TEXT("Codex"), SmokeProjectRoot, TEXT("codex"));
		const FString NoAgentTerminalCommand = SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(ReadyStatus, FString(), SmokeProjectRoot, FString());
		const bool bTerminalOk = ReadyTerminalCommand.Contains(TEXT("cmd /d /s /k"))
			&& ReadyTerminalCommand.Contains(TEXT("TERM=xterm-256color"))
			&& ReadyTerminalCommand.Contains(TEXT("cd /d \"D:/HyperAIStudioSmokeProject\""))
			&& ReadyTerminalCommand.Contains(TEXT("Endpoint: http://127.0.0.1:8000/mcp"))
			&& ReadyTerminalCommand.Contains(TEXT("Starting agent CLI"))
			&& ReadyTerminalCommand.Contains(TEXT("( codex )"))
			&& ReadyTerminalCommand.Contains(TEXT("Agent CLI exited"))
			&& ReadyTerminalCommand.Contains(TEXT("use Copy Prompt"))
			&& !ReadyTerminalCommand.Contains(TEXT("cursor ."))
			&& !ReadyTerminalCommand.Contains(TEXT("code ."))
			&& SetupTerminalCommand.Contains(TEXT("Setup is not ready yet"))
			&& !SetupTerminalCommand.Contains(TEXT("Starting agent CLI"))
			&& NoAgentTerminalCommand.Contains(TEXT("Agent: none usable"));
		const bool bOk = bReadyOk && bCheckingOk && bModuleOk && bSetupOk && bServerOk && bTerminalOk;

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.QuickActionSmoke %s ready=%s checking=%s module=%s setup=%s server=%s terminal=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			bReadyOk ? TEXT("true") : TEXT("false"),
			bCheckingOk ? TEXT("true") : TEXT("false"),
			bModuleOk ? TEXT("true") : TEXT("false"),
			bSetupOk ? TEXT("true") : TEXT("false"),
			bServerOk ? TEXT("true") : TEXT("false"),
			bTerminalOk ? TEXT("true") : TEXT("false"));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	void RunConsoleMenuSmoke(const TArray<FString>& Args)
	{
		bool bQuitOnComplete = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("-quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("--quit-on-complete"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitOnComplete = true;
				break;
			}
		}

		int32 ToolsFound = 0;
		int32 ToolsTextOk = 0;
		int32 StatusFound = 0;
		int32 AssetFound = 0;
		int32 AssetTextOk = 0;
		int32 ActorUnexpectedFound = 0;
		FString ToolsMissing;
		FString StatusMissing;
		FString AssetMissing;
		FString ActorUnexpected;

		const bool bToolsOk = HasToolMenuEntriesWithText(
			TEXT("LevelEditor.MainMenu.Tools"),
			{
				{ TEXT("OpenHyperAIStudio"), TEXT("HyperAIStudio"), TEXT("Open HyperAIStudio") },
				{ TEXT("OpenHyperAIStudioChat"), TEXT("HyperAI Chat"), TEXT("lightweight HyperAI chat panel") },
				{ TEXT("OpenHyperAIStudioQuickAction"), TEXT("HyperAI Chat Overlay"), TEXT("lightweight chat overlay") }
			},
			ToolsFound,
			ToolsTextOk,
			ToolsMissing);

		const bool bStatusOk = HasToolMenuEntries(
			TEXT("LevelEditor.StatusBar.ToolBar"),
			{
				TEXT("OpenHyperAIStudioStatus")
			},
			StatusFound,
			StatusMissing);

		const TArray<FHyperAIStudioExpectedMenuEntry> SelectionEntries = {
			{ TEXT("HyperAIStudioOpenCodexContext"), TEXT("Open Codex With Selection"), TEXT("open Codex") },
			{ TEXT("HyperAIStudioOpenClaudeContext"), TEXT("Open Claude Code With Selection"), TEXT("open Claude Code") },
			{ TEXT("HyperAIStudioCreateCodexTaskPack"), TEXT("Copy Codex Prompt"), TEXT("Codex-ready prompt") },
			{ TEXT("HyperAIStudioCreateClaudeTaskPack"), TEXT("Copy Claude Code Prompt"), TEXT("Claude Code-ready prompt") },
			{ TEXT("HyperAIStudioAddSelectionContext"), TEXT("Copy Selection Prompt"), TEXT("preferred external agent") }
		};
		const bool bAssetOk = HasToolMenuEntriesWithText(TEXT("ContentBrowser.AssetContextMenu"), SelectionEntries, AssetFound, AssetTextOk, AssetMissing);
		const bool bActorOk = HasNoToolMenuEntries(
			TEXT("LevelEditor.ActorContextMenu"),
			{
				TEXT("HyperAIStudioOpenCodexContext"),
				TEXT("HyperAIStudioOpenClaudeContext"),
				TEXT("HyperAIStudioCreateCodexTaskPack"),
				TEXT("HyperAIStudioCreateClaudeTaskPack"),
				TEXT("HyperAIStudioAddSelectionContext")
			},
			ActorUnexpectedFound,
			ActorUnexpected);
		const bool bGraphExtenderOk = IsGraphEditorExtenderRegistered();
		const int32 MenuTextOk = ToolsTextOk + AssetTextOk;
		const bool bTextOk = MenuTextOk == 8;
		const bool bOk = bToolsOk && bStatusOk && bAssetOk && bActorOk && bGraphExtenderOk && bTextOk;

		TArray<FString> Problems;
		if (!ToolsMissing.IsEmpty())
		{
			Problems.Add(ToolsMissing);
		}
		if (!StatusMissing.IsEmpty())
		{
			Problems.Add(StatusMissing);
		}
		if (!AssetMissing.IsEmpty())
		{
			Problems.Add(AssetMissing);
		}
		if (!ActorUnexpected.IsEmpty())
		{
			Problems.Add(ActorUnexpected);
		}
		if (!bGraphExtenderOk)
		{
			Problems.Add(TEXT("Blueprint graph context-menu extender not registered"));
		}
		if (!bTextOk)
		{
			Problems.Add(FString::Printf(TEXT("visible menu text validated %d/8"), MenuTextOk));
		}

		UE_LOG(LogHyperAIStudio, Display, TEXT("HyperAIStudio.MenuSmoke %s tools=%d/3 status=%d/1 asset=%d/5 actorUnexpected=%d/0 menuText=%d/8 graphExtender=%s problems=%s"),
			bOk ? TEXT("succeeded") : TEXT("failed"),
			ToolsFound,
			StatusFound,
			AssetFound,
			ActorUnexpectedFound,
			MenuTextOk,
			bGraphExtenderOk ? TEXT("true") : TEXT("false"),
			Problems.Num() == 0 ? TEXT("none") : *FString::Join(Problems, TEXT("; ")));

		if (bQuitOnComplete)
		{
			RequestSmokeExit();
		}
	}

	TUniquePtr<FHyperAIStudioPlanValidateToolRegistration> PlanValidateToolRegistration;
	TUniquePtr<FHyperAIStudioPlanExecuteToolRegistration> PlanExecuteToolRegistration;
	TUniquePtr<FHyperAIStudioNativeReadToolRegistration> NativeReadToolRegistration;
	TUniquePtr<FHyperAIStudioContextSearchRegistration> ContextSearchToolRegistration;
	TUniquePtr<FHyperAIStudioBlueprintWorkflowRegistration> BlueprintWorkflowRegistration;
	TUniquePtr<FHyperAIStudioDiagnosticsRegistration> DiagnosticsRegistration;
	TUniquePtr<FHyperAIStudioPIEPlaytestRegistration> PIEPlaytestRegistration;
	TUniquePtr<FHyperAIStudioFoundationProbe> FoundationProbe;
	FDelegateHandle EnginePreExitHandle;
	TUniquePtr<FAutoConsoleCommand> CommandSetUp;
	TUniquePtr<FAutoConsoleCommand> CommandGenerateFiles;
	TUniquePtr<FAutoConsoleCommand> CommandDocsSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandWelcomeSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandWorkbenchSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandAgentConfigSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandMCPRegistrySmoke;
	TUniquePtr<FAutoConsoleCommand> CommandHyperUEMCPSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandMCPClipboardSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandImportMCPProfile;
	TUniquePtr<FAutoConsoleCommand> CommandImportMCPProfileFromFile;
	TUniquePtr<FAutoConsoleCommand> CommandImportExampleMCPProfile;
	TUniquePtr<FAutoConsoleCommand> CommandImportProviderMCPProfile;
	TUniquePtr<FAutoConsoleCommand> CommandTestMCPServer;
	TUniquePtr<FAutoConsoleCommand> CommandCreateTestPrompt;
	TUniquePtr<FAutoConsoleCommand> CommandAgentPromptSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandHandoffScriptSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandTerminalLaunchPlanSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandBlueprintContextSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandBlueprintMenuSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandSelectionContextSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandAssetContextSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandCopyCodexTomlPatch;
	TUniquePtr<FAutoConsoleCommand> CommandStartMCP;
	TUniquePtr<FAutoConsoleCommand> CommandStatus;
	TUniquePtr<FAutoConsoleCommand> CommandStatusProbe;
	TUniquePtr<FAutoConsoleCommand> CommandPrerequisites;
	TUniquePtr<FAutoConsoleCommand> CommandPrerequisiteGuidanceSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandOpen;
	TUniquePtr<FAutoConsoleCommand> CommandOpenChat;
	TUniquePtr<FAutoConsoleCommand> CommandQuickAction;
	TUniquePtr<FAutoConsoleCommand> CommandQuickActionSmoke;
	TUniquePtr<FAutoConsoleCommand> CommandMenuSmoke;
	TWeakPtr<SDockTab> ActiveChatTab;
	TWeakPtr<SHyperAIStudioQuickActionWindow> ActiveChatWidget;
	FDelegateHandle GraphEditorContextMenuExtenderHandle;
	FTSTicker::FDelegateHandle WelcomePromptTickerHandle;
	bool bWelcomePromptProbeActive = false;
	bool bWelcomePromptShown = false;
	bool bModuleShuttingDown = false;
};

IMPLEMENT_MODULE(FHyperAIStudioModule, HyperAIStudio)

#undef LOCTEXT_NAMESPACE
