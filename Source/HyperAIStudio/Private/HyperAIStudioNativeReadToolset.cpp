// Games by Hyper 2026.

#include "HyperAIStudioNativeReadToolset.h"

#include "BlueprintEditorSettings.h"
#include "CoreGlobals.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDependencyGraphToolset.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioService.h"
#include "IModelContextProtocolModule.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Text.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/WatchedPin.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioNativeReadToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNativeReadTools, Log, All);

namespace HyperAIStudio::NativeReadTools::Private
{
	constexpr int32 MaxGraphFilterCharacters = 256;
	constexpr int32 MaxNodeTitleCharacters = 256;
	constexpr int32 MaxMessageCharacters = 512;
	constexpr int32 MaxWatchValueCharacters = 512;
	constexpr int32 MaxWatchPropertySegments = 64;

	FString NowUtc()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	bool IsLegacyRawSha1(const FString& Value)
	{
		if (Value.Len() != FSHA1::DigestSize * 2)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	FString GuidString(const FGuid& Guid)
	{
		return Guid.IsValid()
			? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
			: FString();
	}

	FString GetPluginVersion(const TCHAR* PluginName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unavailable");
	}

	FString ExpectedPackIdForNativeTool(const FString& ToolName)
	{
		if (ToolName == TEXT("hyper_capability_report") || ToolName == TEXT("hyper_operation_status"))
		{
			return TEXT("shared_foundation");
		}
		if (ToolName == TEXT("hyper_blueprint_debug_inspect"))
		{
			return TEXT("blueprint");
		}
		if (ToolName == TEXT("hyper_pie_query"))
		{
			return TEXT("pie_runtime");
		}
		if (ToolName == TEXT("hyper_asset_dependency_graph"))
		{
			return TEXT("diagnostics");
		}
		return FString();
	}

	bool IsBuiltInCatalogValid()
	{
		static const bool bValid = []()
		{
			TArray<FString> Errors;
			return FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(Errors);
		}();
		return bValid;
	}

	const TArray<const FHyperAIStudioCapabilityToolDefinition*>& SortedCatalogTools()
	{
		static const TArray<const FHyperAIStudioCapabilityToolDefinition*> Sorted = []()
		{
			TArray<const FHyperAIStudioCapabilityToolDefinition*> Result;
			const FHyperAIStudioCapabilityCatalog& Catalog =
				FHyperAIStudioCapabilityPackRegistry::GetCatalog();
			Result.Reserve(Catalog.Tools.Num());
			for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
			{
				Result.Add(&Tool);
			}
			Result.Sort([](
				const FHyperAIStudioCapabilityToolDefinition& A,
				const FHyperAIStudioCapabilityToolDefinition& B)
			{
				return A.Name < B.Name;
			});
			return Result;
		}();
		return Sorted;
	}

	const TSet<FString>& PlanOnlyToolNames()
	{
		static const TSet<FString> Names = {
			TEXT("hyper_actor_modifier_apply_plan"),
			TEXT("hyper_animation_apply_plan"),
			TEXT("hyper_audio_apply_plan"),
			TEXT("hyper_character_apply_plan"),
			TEXT("hyper_cinematics_apply_plan"),
			TEXT("hyper_dynamic_material_apply_plan"),
			TEXT("hyper_game_framework_apply_plan"),
			TEXT("hyper_gameplay_ai_apply_plan"),
			TEXT("hyper_gameplay_systems_apply_plan"),
			TEXT("hyper_gas_apply_plan"),
			TEXT("hyper_geometry_apply_plan"),
			TEXT("hyper_interchange_apply_plan"),
			TEXT("hyper_live_production_apply_plan"),
			TEXT("hyper_material_apply_plan"),
			TEXT("hyper_navigation_apply_plan"),
			TEXT("hyper_network_apply_plan"),
			TEXT("hyper_niagara_apply_plan"),
			TEXT("hyper_pcg_apply_plan"),
			TEXT("hyper_physics_apply_plan"),
			TEXT("hyper_property_animation_apply_plan"),
			TEXT("hyper_scene_apply_plan"),
			TEXT("hyper_ui_apply_plan"),
			TEXT("hyper_worldbuilding_apply_plan")
		};
		return Names;
	}

	const TSet<FString>& LimitedDirectEditToolNames()
	{
		static const TSet<FString> Names = {
			TEXT("hyper_data_apply_plan"),
			TEXT("hyper_input_apply_plan"),
			TEXT("hyper_paper2d_apply_plan")
		};
		return Names;
	}

	FString ExecutionSupportForTool(const FHyperAIStudioCapabilityToolDefinition& Tool)
	{
		if (PlanOnlyToolNames().Contains(Tool.Name))
		{
			return TEXT("PlanOnly");
		}
		if (LimitedDirectEditToolNames().Contains(Tool.Name))
		{
			return TEXT("LimitedDirectEdit");
		}
		if (Tool.Name == TEXT("hyper_plan_validate")
			|| Tool.Name.EndsWith(TEXT("_validate"), ESearchCase::CaseSensitive)
			|| Tool.Name.Contains(TEXT("_validate_"), ESearchCase::CaseSensitive))
		{
			return TEXT("Validate");
		}

		static const TSet<FString> OperationSpecificNames = {
			TEXT("hyper_blueprint_apply_patch"),
			TEXT("hyper_blueprint_build_batch"),
			TEXT("hyper_blueprint_layout"),
			TEXT("hyper_blueprint_repair"),
			TEXT("hyper_build_diagnose"),
			TEXT("hyper_plan_execute"),
			TEXT("hyper_playtest_run"),
			TEXT("hyper_profile_capture"),
			TEXT("hyper_run_audit"),
			TEXT("hyper_test_run"),
			TEXT("hyper_viewport_capture")
		};
		return Tool.bMayCauseExternalEffects || OperationSpecificNames.Contains(Tool.Name)
			? TEXT("OperationSpecific")
			: TEXT("Read");
	}

	TOptional<EHyperAIStudioCapabilityAdmissionState> CatalogAdmissionForTool(const FString& ToolName)
	{
		const FString ExpectedPackId = ExpectedPackIdForNativeTool(ToolName);
		if (ExpectedPackId.IsEmpty())
		{
			return {};
		}
		if (!IsBuiltInCatalogValid())
		{
			return {};
		}
		const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
		TOptional<EHyperAIStudioCapabilityAdmissionState> Result;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name != ToolName)
			{
				continue;
			}
			if (Tool.PackId != ExpectedPackId)
			{
				return {};
			}
			if (Result.IsSet())
			{
				return {};
			}
			Result = Tool.AdmissionState;
		}
		return Result;
	}

	FString DiscoveryModeToString(const EHyperAIStudioToolDiscoveryMode Mode)
	{
		switch (Mode)
		{
		case EHyperAIStudioToolDiscoveryMode::ToolSearch:
			return TEXT("tool_search");
		case EHyperAIStudioToolDiscoveryMode::Eager:
			return TEXT("eager");
		case EHyperAIStudioToolDiscoveryMode::Degraded:
			return TEXT("degraded");
		case EHyperAIStudioToolDiscoveryMode::Unknown:
		default:
			return TEXT("unknown");
		}
	}

	FString BlueprintStatusToString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Dirty:
			return TEXT("dirty");
		case BS_Error:
			return TEXT("error");
		case BS_UpToDate:
			return TEXT("up_to_date");
		case BS_BeingCreated:
			return TEXT("being_created");
		case BS_UpToDateWithWarnings:
			return TEXT("up_to_date_with_warnings");
		case BS_Unknown:
		case BS_MAX:
		default:
			return TEXT("unknown");
		}
	}

	FString WorldTypeToString(const EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::Game:
			return TEXT("game");
		case EWorldType::Editor:
			return TEXT("editor");
		case EWorldType::PIE:
			return TEXT("pie");
		case EWorldType::EditorPreview:
			return TEXT("editor_preview");
		case EWorldType::GamePreview:
			return TEXT("game_preview");
		case EWorldType::GameRPC:
			return TEXT("game_rpc");
		case EWorldType::Inactive:
			return TEXT("inactive");
		case EWorldType::None:
		default:
			return TEXT("none");
		}
	}

	FString NetModeToString(const ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone:
			return TEXT("standalone");
		case NM_DedicatedServer:
			return TEXT("dedicated_server");
		case NM_ListenServer:
			return TEXT("listen_server");
		case NM_Client:
			return TEXT("client");
		case NM_MAX:
		default:
			return TEXT("unknown");
		}
	}

	FString PlayNetModeToString(const EPlayNetMode NetMode)
	{
		switch (NetMode)
		{
		case PIE_Standalone:
			return TEXT("standalone");
		case PIE_ListenServer:
			return TEXT("listen_server");
		case PIE_Client:
			return TEXT("client");
		default:
			return TEXT("unknown");
		}
	}

	bool ReadPieTopologyConfig(
		const ULevelEditorPlaySettings* PlaySettings,
		FHyperAIStudioPIETopologyConfig& OutConfig)
	{
		if (!PlaySettings)
		{
			return false;
		}

		EPlayNetMode ConfiguredNetMode = PIE_Standalone;
		PlaySettings->GetPlayNetMode(ConfiguredNetMode);
		PlaySettings->GetRunUnderOneProcess(OutConfig.bRunUnderOneProcess);
		PlaySettings->GetPlayNumberOfClients(OutConfig.ClientCount);
		OutConfig.PlayNetMode = PlayNetModeToString(ConfiguredNetMode);
		OutConfig.bLaunchSeparateServer = PlaySettings->bLaunchSeparateServer;
		return true;
	}

	FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		switch (Direction)
		{
		case EGPD_Input:
			return TEXT("input");
		case EGPD_Output:
			return TEXT("output");
		case EGPD_MAX:
		default:
			return TEXT("unknown");
		}
	}

	FString MessageSeverityToString(const int32 ErrorType)
	{
		switch (ErrorType)
		{
		case 0: // Legacy critical-error value retained by EMessageSeverity without naming its deprecated enumerator.
			return TEXT("critical_error");
		case static_cast<int32>(EMessageSeverity::Error):
			return TEXT("error");
		case static_cast<int32>(EMessageSeverity::PerformanceWarning):
			return TEXT("performance_warning");
		case static_cast<int32>(EMessageSeverity::Warning):
			return TEXT("warning");
		case static_cast<int32>(EMessageSeverity::Info):
			return TEXT("info");
		default:
			return TEXT("diagnostic");
		}
	}

	FString WatchStateToString(const FKismetDebugUtilities::EWatchTextResult State)
	{
		switch (State)
		{
		case FKismetDebugUtilities::EWTR_Valid:
			return TEXT("valid");
		case FKismetDebugUtilities::EWTR_NotInScope:
			return TEXT("not_in_scope");
		case FKismetDebugUtilities::EWTR_NoDebugObject:
			return TEXT("no_debug_object");
		case FKismetDebugUtilities::EWTR_NoProperty:
		default:
			return TEXT("no_property");
		}
	}

	FKismetDebugUtilities::EWatchTextResult GetBoundedWatchPreview(
		UBlueprint* Blueprint,
		UObject* ActiveObject,
		const UEdGraphPin* WatchPin,
		FString& OutPreview,
		bool& bOutPreviewOmitted)
	{
		OutPreview.Reset();
		bOutPreviewOmitted = false;

		// FindDebuggingData is intentionally protected in UE 5.8. Its public text/debug-info
		// wrappers may materialize an unbounded container/struct representation before the
		// caller can enforce a limit. Resolve only an already-loaded class member through
		// the public compiled-debug map, then read the small allow-listed value directly.
		const FProperty* Property = FKismetDebugUtilities::FindClassPropertyForPin(
			Blueprint,
			WatchPin);
		if (!Property)
		{
			return FKismetDebugUtilities::EWTR_NoProperty;
		}
		if (!IsValid(ActiveObject))
		{
			return FKismetDebugUtilities::EWTR_NoDebugObject;
		}

		const UClass* OwnerClass = Cast<UClass>(Property->GetOwnerStruct());
		if (!OwnerClass || !ActiveObject->IsA(OwnerClass))
		{
			// Function locals, out parameters, anim-node storage, and recursively followed
			// self pins require debugger stack internals. Fail closed instead of invoking an
			// unbounded public text exporter or depending on protected implementation details.
			return FKismetDebugUtilities::EWTR_NotInScope;
		}
		if (Property->ArrayDim != 1)
		{
			bOutPreviewOmitted = true;
			return FKismetDebugUtilities::EWTR_Valid;
		}

		const void* ValueAddress = Property->ContainerPtrToValuePtr<void>(ActiveObject);
		if (!ValueAddress)
		{
			bOutPreviewOmitted = true;
			return FKismetDebugUtilities::EWTR_Valid;
		}

		const FString PropertyPolicy =
			FHyperAIStudioNativeReadContracts::ClassifyWatchPreviewProperty(Property);
		if (PropertyPolicy == TEXT("bounded_scalar"))
		{
			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				OutPreview = BoolProperty->GetPropertyValue(ValueAddress) ? TEXT("true") : TEXT("false");
			}
			else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
			{
				OutPreview = NumericProperty->GetNumericPropertyValueToString(ValueAddress);
			}
			else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				OutPreview = EnumProperty->GetUnderlyingProperty()->GetNumericPropertyValueToString(ValueAddress);
			}
			else if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				OutPreview = NameProperty->GetPropertyValue(ValueAddress).ToString();
			}
			else
			{
				bOutPreviewOmitted = true;
			}
		}
		else if (PropertyPolicy == TEXT("bounded_string"))
		{
			const FString* StringValue = nullptr;
			if (CastField<FStrProperty>(Property))
			{
				StringValue = static_cast<const FString*>(ValueAddress);
			}
			else if (CastField<FTextProperty>(Property))
			{
				StringValue = &FTextInspector::GetDisplayString(*static_cast<const FText*>(ValueAddress));
			}
			if (!StringValue || StringValue->Len() > MaxWatchValueCharacters)
			{
				bOutPreviewOmitted = true;
			}
			else
			{
				OutPreview = *StringValue;
			}
		}
		else
		{
			bOutPreviewOmitted = true;
		}

		if (OutPreview.Len() > MaxWatchValueCharacters)
		{
			OutPreview.Reset();
			bOutPreviewOmitted = true;
		}
		return FKismetDebugUtilities::EWTR_Valid;
	}

	void PopulateGraphNodeIdentity(
		FHyperAIBlueprintDebugItem& Item,
		const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return;
		}

		Item.NodeGuid = GuidString(Node->NodeGuid);
		Item.NodeClass = FHyperAIStudioNativeReadContracts::ClipText(
			Node->GetClass()->GetPathName(),
			FHyperAIStudioNativeReadContracts::MaxPathCharacters);
		Item.NodeTitle = FHyperAIStudioNativeReadContracts::ClipText(
			Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
			MaxNodeTitleCharacters);

		if (const UEdGraph* Graph = Node->GetGraph())
		{
			Item.GraphName = FHyperAIStudioNativeReadContracts::ClipText(Graph->GetName(), MaxGraphFilterCharacters);
			Item.GraphPath = FHyperAIStudioNativeReadContracts::ClipText(
				Graph->GetPathName(),
				FHyperAIStudioNativeReadContracts::MaxPathCharacters);
			Item.GraphGuid = GuidString(Graph->GraphGuid);
		}
	}

	void PopulatePinIdentity(FHyperAIBlueprintDebugItem& Item, const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return;
		}
		PopulateGraphNodeIdentity(Item, Pin->GetOwningNode());
		Item.PinGuid = GuidString(Pin->PinId);
		Item.PinName = FHyperAIStudioNativeReadContracts::ClipText(Pin->PinName.ToString(), MaxNodeTitleCharacters);
		Item.PinDirection = PinDirectionToString(Pin->Direction);
	}

	bool GraphMatchesFilter(const UEdGraph* Graph, const FString& GraphFilter)
	{
		return Graph
			&& (GraphFilter.IsEmpty()
				|| Graph->GetName().Equals(GraphFilter, ESearchCase::CaseSensitive)
				|| Graph->GetPathName().Equals(GraphFilter, ESearchCase::CaseSensitive));
	}

	bool NodeBelongsToBlueprint(const UEdGraphNode* Node, const UBlueprint* Blueprint)
	{
		return Node && Blueprint && FBlueprintEditorUtils::FindBlueprintForNode(Node) == Blueprint;
	}

	FHyperAIPIEWorldIdentity DescribeWorld(
		const UWorld* World,
		const FWorldContext* Context)
	{
		FHyperAIPIEWorldIdentity Result;
		if (!World)
		{
			return Result;
		}

		Result.bPresent = true;
		Result.WorldType = WorldTypeToString(World->WorldType);
		Result.NetMode = NetModeToString(World->GetNetMode());
		Result.WorldPath = FHyperAIStudioNativeReadContracts::ClipText(
			World->GetPathName(),
			FHyperAIStudioNativeReadContracts::MaxPathCharacters);
		Result.bHasAuthority = World->GetNetMode() != NM_Client;
		Result.bBegunPlay = World->HasBegunPlay();
		Result.bPaused = World->IsPaused();
		Result.ActorCount = FMath::Max(0, World->GetActorCount());
		Result.PlayerControllerCount = FMath::Max(0, World->GetNumPlayerControllers());
		Result.LocalPlayerCount = World->GetGameInstance()
			? World->GetGameInstance()->GetLocalPlayers().Num()
			: 0;
		Result.TimeSeconds = static_cast<double>(World->GetTimeSeconds());
		Result.RealTimeSeconds = static_cast<double>(World->GetRealTimeSeconds());
		Result.DeltaTimeSeconds = static_cast<double>(World->GetDeltaSeconds());

		if (const AGameModeBase* GameMode = World->GetAuthGameMode())
		{
			Result.GameModeClass = FHyperAIStudioNativeReadContracts::ClipText(
				GameMode->GetClass()->GetPathName(),
				FHyperAIStudioNativeReadContracts::MaxPathCharacters);
		}
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			Result.GameStateClass = FHyperAIStudioNativeReadContracts::ClipText(
				GameState->GetClass()->GetPathName(),
				FHyperAIStudioNativeReadContracts::MaxPathCharacters);
		}

		if (const UPackage* Package = World->GetPackage())
		{
			Result.WorldPackageName = FHyperAIStudioNativeReadContracts::ClipText(
				Package->GetName(),
				FHyperAIStudioNativeReadContracts::MaxPathCharacters);
			Result.PackagePieInstance = Package->GetPIEInstanceID();
			int32 PrefixInstance = INDEX_NONE;
			Result.SourcePackageName = FHyperAIStudioNativeReadContracts::ClipText(
				UWorld::RemovePIEPrefix(Result.WorldPackageName, &PrefixInstance),
				FHyperAIStudioNativeReadContracts::MaxPathCharacters);
			if (Result.PackagePieInstance == INDEX_NONE)
			{
				Result.PackagePieInstance = PrefixInstance;
			}
		}

		if (Context)
		{
			Result.ContextHandle = FHyperAIStudioNativeReadContracts::ClipText(
				Context->ContextHandle.ToString(),
				MaxNodeTitleCharacters);
			Result.PieInstance = Context->PIEInstance;
			Result.bPrimaryInstance = Context->bIsPrimaryPIEInstance;
			Result.bDedicatedServer = Context->RunAsDedicated || World->GetNetMode() == NM_DedicatedServer;
		}
		else
		{
			Result.PieInstance = Result.PackagePieInstance;
			Result.bDedicatedServer = World->GetNetMode() == NM_DedicatedServer;
		}
		return Result;
	}

	const FWorldContext* FindWorldContext(const UWorld* World)
	{
		if (!GEngine || !World)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() == World)
			{
				return &Context;
			}
		}
		return nullptr;
	}

	bool BlueprintItemLess(
		const FHyperAIBlueprintDebugItem& A,
		const FHyperAIBlueprintDebugItem& B)
	{
		const FString* AFields[] = {
			&A.Kind,
			&A.State,
			&A.GraphName,
			&A.GraphPath,
			&A.GraphGuid,
			&A.NodeGuid,
			&A.PinGuid,
			&A.NodeClass,
			&A.NodeTitle,
			&A.PinName,
			&A.PinDirection,
			&A.Severity,
			&A.Message,
			&A.PropertyPath,
			&A.ValuePreview
		};
		const FString* BFields[] = {
			&B.Kind,
			&B.State,
			&B.GraphName,
			&B.GraphPath,
			&B.GraphGuid,
			&B.NodeGuid,
			&B.PinGuid,
			&B.NodeClass,
			&B.NodeTitle,
			&B.PinName,
			&B.PinDirection,
			&B.Severity,
			&B.Message,
			&B.PropertyPath,
			&B.ValuePreview
		};
		for (int32 FieldIndex = 0; FieldIndex < UE_ARRAY_COUNT(AFields); ++FieldIndex)
		{
			if (*AFields[FieldIndex] != *BFields[FieldIndex])
			{
				return *AFields[FieldIndex] < *BFields[FieldIndex];
			}
		}
		if (A.bEnabled != B.bEnabled)
		{
			return !A.bEnabled;
		}
		if (A.bValid != B.bValid)
		{
			return !A.bValid;
		}
		if (A.bPropertyPathTruncated != B.bPropertyPathTruncated)
		{
			return !A.bPropertyPathTruncated;
		}
		return !A.bValueTruncated && B.bValueTruncated;
	}

	void AppendBlueprintItemFingerprint(
		TArray<FString>& Tokens,
		const FHyperAIBlueprintDebugItem& Item)
	{
		Tokens.Add(Item.Kind);
		Tokens.Add(Item.State);
		Tokens.Add(Item.GraphName);
		Tokens.Add(Item.GraphPath);
		Tokens.Add(Item.GraphGuid);
		Tokens.Add(Item.NodeGuid);
		Tokens.Add(Item.PinGuid);
		Tokens.Add(Item.NodeClass);
		Tokens.Add(Item.NodeTitle);
		Tokens.Add(Item.PinName);
		Tokens.Add(Item.PinDirection);
		Tokens.Add(Item.Severity);
		Tokens.Add(Item.Message);
		Tokens.Add(Item.bEnabled ? TEXT("1") : TEXT("0"));
		Tokens.Add(Item.bValid ? TEXT("1") : TEXT("0"));
		Tokens.Add(Item.PropertyPath);
		Tokens.Add(Item.bPropertyPathTruncated ? TEXT("1") : TEXT("0"));
		Tokens.Add(Item.ValuePreview);
		Tokens.Add(Item.bValueTruncated ? TEXT("1") : TEXT("0"));
	}

	FString DoubleToken(const double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	FHyperAIPIEDebugIdentity MakeDebugPieIdentity(const FHyperAIPIEWorldIdentity& World)
	{
		FHyperAIPIEDebugIdentity Identity;
		Identity.bPresent = World.bPresent;
		Identity.ContextHandle = World.ContextHandle;
		Identity.PieInstance = World.PieInstance;
		Identity.PackagePieInstance = World.PackagePieInstance;
		Identity.bPrimaryInstance = World.bPrimaryInstance;
		Identity.bDedicatedServer = World.bDedicatedServer;
		Identity.WorldType = World.WorldType;
		Identity.NetMode = World.NetMode;
		Identity.WorldPath = World.WorldPath;
		Identity.WorldPackageName = World.WorldPackageName;
		Identity.SourcePackageName = World.SourcePackageName;
		return Identity;
	}

	void AppendDebugPieIdentityFingerprint(
		TArray<FString>& Tokens,
		const FHyperAIPIEDebugIdentity& Identity)
	{
		Tokens.Add(Identity.bPresent ? TEXT("1") : TEXT("0"));
		Tokens.Add(Identity.ContextHandle);
		Tokens.Add(FString::FromInt(Identity.PieInstance));
		Tokens.Add(FString::FromInt(Identity.PackagePieInstance));
		Tokens.Add(Identity.bPrimaryInstance ? TEXT("1") : TEXT("0"));
		Tokens.Add(Identity.bDedicatedServer ? TEXT("1") : TEXT("0"));
		Tokens.Add(Identity.WorldType);
		Tokens.Add(Identity.NetMode);
		Tokens.Add(Identity.WorldPath);
		Tokens.Add(Identity.WorldPackageName);
		Tokens.Add(Identity.SourcePackageName);
	}

	void AppendPieWorldFingerprint(
		TArray<FString>& Tokens,
		const FHyperAIPIEWorldIdentity& World)
	{
		Tokens.Add(World.bPresent ? TEXT("1") : TEXT("0"));
		Tokens.Add(World.ContextHandle);
		Tokens.Add(FString::FromInt(World.PieInstance));
		Tokens.Add(FString::FromInt(World.PackagePieInstance));
		Tokens.Add(World.bPrimaryInstance ? TEXT("1") : TEXT("0"));
		Tokens.Add(World.bDedicatedServer ? TEXT("1") : TEXT("0"));
		Tokens.Add(World.WorldType);
		Tokens.Add(World.NetMode);
		Tokens.Add(World.WorldPath);
		Tokens.Add(World.WorldPackageName);
		Tokens.Add(World.SourcePackageName);
		Tokens.Add(World.bHasAuthority ? TEXT("1") : TEXT("0"));
		Tokens.Add(World.bBegunPlay ? TEXT("1") : TEXT("0"));
		Tokens.Add(World.bPaused ? TEXT("1") : TEXT("0"));
		Tokens.Add(FString::FromInt(World.ActorCount));
		Tokens.Add(FString::FromInt(World.PlayerControllerCount));
		Tokens.Add(FString::FromInt(World.LocalPlayerCount));
		Tokens.Add(World.GameModeClass);
		Tokens.Add(World.GameStateClass);
		Tokens.Add(DoubleToken(World.TimeSeconds));
		Tokens.Add(DoubleToken(World.RealTimeSeconds));
		Tokens.Add(DoubleToken(World.DeltaTimeSeconds));
	}

	FString PieWorldCanonical(const FHyperAIPIEWorldIdentity& World)
	{
		TArray<FString> Tokens;
		Tokens.Reserve(22);
		AppendPieWorldFingerprint(Tokens, World);
		FString Canonical;
		for (const FString& Token : Tokens)
		{
			Canonical += FString::FromInt(Token.Len());
			Canonical += TEXT(":");
			Canonical += Token;
		}
		return Canonical;
	}

	void ApplyPage(
		const TArray<FHyperAIEpicToolsetSummary>& Source,
		const int32 Offset,
		const int32 PageSize,
		const FString& Fingerprint,
		FHyperAICapabilityReport& Report)
	{
		const int32 End = FMath::Min(Source.Num(), Offset + PageSize);
		for (int32 Index = Offset; Index < End; ++Index)
		{
			Report.CachedEpicToolsets.Add(Source[Index]);
		}
		Report.PageOffset = Offset;
		Report.PageSize = PageSize;
		Report.ReturnedEpicToolsetCount = Report.CachedEpicToolsets.Num();
		Report.bTruncated = End < Source.Num() || Report.bCachedEpicInventorySourceTruncated;
		if (End < Source.Num())
		{
			Report.NextCursor = FHyperAIStudioNativeReadContracts::MakeCursor(Fingerprint, End);
		}
	}

	void ApplyPage(
		const TArray<FHyperAIBlueprintDebugItem>& Source,
		const int32 Offset,
		const int32 PageSize,
		const FString& Fingerprint,
		FHyperAIBlueprintDebugReport& Report)
	{
		const int32 End = FMath::Min(Source.Num(), Offset + PageSize);
		for (int32 Index = Offset; Index < End; ++Index)
		{
			Report.Items.Add(Source[Index]);
		}
		Report.PageOffset = Offset;
		Report.PageSize = PageSize;
		Report.ReturnedItemCount = Report.Items.Num();
		Report.bTruncated = End < Source.Num() || Report.bScanTruncated;
		if (End < Source.Num())
		{
			Report.NextCursor = FHyperAIStudioNativeReadContracts::MakeCursor(Fingerprint, End);
		}
	}

}

const TArray<FHyperAIStudioNativeToolManifestEntry>& FHyperAIStudioNativeReadContracts::GetCallableManifest()
{
	static const TArray<FHyperAIStudioNativeToolManifestEntry> Manifest = {
		{
			TEXT("hyper_capability_report"),
			GetQualifiedToolsetName(),
			TEXT("read"),
			TEXT("Installed HyperAI callable tools, current MCP health, and paged cached Epic inventory."),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate
		},
		{
			TEXT("hyper_operation_status"),
			GetQualifiedToolsetName(),
			TEXT("reconciliation_read"),
			TEXT("Exclusive journal load/reconciliation and status lookup for one operation id."),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate
		},
		{
			TEXT("hyper_blueprint_debug_inspect"),
			GetQualifiedToolsetName(),
			TEXT("read"),
			TEXT("Paged Blueprint diagnostics, breakpoints, watches, and debug PIE identity."),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate
		},
		{
			TEXT("hyper_pie_query"),
			GetQualifiedToolsetName(),
			TEXT("read"),
			TEXT("Complete bounded in-process PIE world and frozen-session net-mode snapshot."),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate
		},
		{
			TEXT("hyper_asset_dependency_graph"),
			GetDependencyGraphQualifiedToolsetName(),
			TEXT("read"),
			TEXT("Bounded processing of Asset Registry on-disk dependency/referencer state, with explicit raw-allocation caveat, cycle evidence, and stable pagination."),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate
		}
	};
	return Manifest;
}

FString FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset");
}

FString FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioDependencyGraphToolset");
}

FString FHyperAIStudioNativeReadContracts::AdmissionStateToString(
	const FHyperAIStudioNativeToolManifestEntry::EAdmissionState State)
{
	switch (State)
	{
	case FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Planned:
		return TEXT("planned");
	case FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate:
		return TEXT("source_candidate");
	case FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted:
		return TEXT("admitted");
	default:
		return TEXT("invalid");
	}
}

bool FHyperAIStudioNativeReadContracts::IsPendingNativeToolsTestEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
	const FString& Toolset,
	const bool bAllowPendingForTests)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	bool bFound = false;
	TOptional<EHyperAIStudioCapabilityAdmissionState> CohortState;
	for (const FHyperAIStudioNativeToolManifestEntry& Entry : GetCallableManifest())
	{
		if (Entry.Toolset != Toolset || Entry.AdmissionState == FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Planned)
		{
			continue;
		}
		bFound = true;
		const TOptional<EHyperAIStudioCapabilityAdmissionState> CatalogState = CatalogAdmissionForTool(Entry.Name);
		if (!CatalogState.IsSet())
		{
			return false;
		}
		if (!CohortState.IsSet())
		{
			CohortState = CatalogState;
		}
		else if (CohortState.GetValue() != CatalogState.GetValue())
		{
			return false;
		}
	}
	if (!bFound || !CohortState.IsSet())
	{
		return false;
	}
	return CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::Admitted
		|| (bAllowPendingForTests
			&& CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
}

bool FHyperAIStudioNativeReadContracts::IsAdmissionCohortRegistrationAllowed(
	const TArray<FHyperAIStudioNativeToolManifestEntry>& Manifest,
	const FString& Toolset,
	const bool bAllowPendingForTests)
{
	bool bFoundEntry = false;
	TOptional<FHyperAIStudioNativeToolManifestEntry::EAdmissionState> CohortState;
	for (const FHyperAIStudioNativeToolManifestEntry& Entry : Manifest)
	{
		if (Entry.Toolset != Toolset)
		{
			continue;
		}
		bFoundEntry = true;
		if (!CohortState.IsSet())
		{
			CohortState = Entry.AdmissionState;
		}
		else if (CohortState.GetValue() != Entry.AdmissionState)
		{
			return false;
		}
	}
	if (!bFoundEntry || !CohortState.IsSet())
	{
		return false;
	}
	if (CohortState.GetValue() == FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Planned)
	{
		return false;
	}
	return CohortState.GetValue() == FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted
		|| bAllowPendingForTests;
}

bool FHyperAIStudioNativeReadContracts::IsCapabilityRegistrationExpected(
	const FHyperAINativeToolSummary& Tool,
	const bool bAllowSourceCandidatesForTests)
{
	if (!Tool.bImplementationLoaded)
	{
		return false;
	}
	return Tool.AdmissionState == TEXT("admitted")
		|| (bAllowSourceCandidatesForTests
			&& Tool.AdmissionState == TEXT("source_candidate"));
}

int32 FHyperAIStudioNativeReadContracts::CountCapabilityRegistrationMismatches(
	const TArray<FHyperAINativeToolSummary>& Tools,
	const bool bAllowSourceCandidatesForTests,
	int32& OutExpectedRegisteredToolCount)
{
	OutExpectedRegisteredToolCount = 0;
	int32 MismatchCount = 0;
	for (const FHyperAINativeToolSummary& Tool : Tools)
	{
		const bool bExpected = IsCapabilityRegistrationExpected(
			Tool,
			bAllowSourceCandidatesForTests);
		OutExpectedRegisteredToolCount += bExpected ? 1 : 0;
		MismatchCount += Tool.bToolsetRegistered != bExpected ? 1 : 0;
	}
	return MismatchCount;
}

bool FHyperAIStudioNativeReadContracts::AreCapabilityRuntimeRowsConsistent(
	const TArray<FHyperAINativeToolSummary>& Tools,
	const bool bAllowSourceCandidatesForTests,
	int32& OutExpectedRegisteredToolCount,
	int32& OutRegistrationMismatchCount)
{
	OutRegistrationMismatchCount = CountCapabilityRegistrationMismatches(
		Tools,
		bAllowSourceCandidatesForTests,
		OutExpectedRegisteredToolCount);
	bool bCallableStatesConsistent = true;
	for (const FHyperAINativeToolSummary& Tool : Tools)
	{
		const bool bAdmissionAuthorized = Tool.AdmissionState == TEXT("admitted")
			|| (bAllowSourceCandidatesForTests
				&& Tool.AdmissionState == TEXT("source_candidate"));
		const bool bExpectedCallable = Tool.bImplementationLoaded
			&& Tool.bToolsetRegistered
			&& Tool.bRuntimeEnabled
			&& bAdmissionAuthorized;
		bCallableStatesConsistent &= Tool.bCallable == bExpectedCallable;
	}
	return OutRegistrationMismatchCount == 0 && bCallableStatesConsistent;
}

FHyperAIStudioPIETopologyResolution FHyperAIStudioNativeReadContracts::ResolvePieTopology(
	const bool bActiveSession,
	const bool bFrozenConfigAvailable,
	const FHyperAIStudioPIETopologyConfig& FrozenConfig,
	const FHyperAIStudioPIETopologyConfig& CurrentConfig)
{
	FHyperAIStudioPIETopologyResolution Result;
	if (bActiveSession && !bFrozenConfigAvailable)
	{
		Result.Source = TEXT("active_original_request_unavailable");
		return Result;
	}

	Result.bUsedFrozenActiveSession = bActiveSession;
	Result.Source = bActiveSession ? TEXT("active_original_request") : TEXT("current_editor_settings");
	Result.Effective = bActiveSession ? FrozenConfig : CurrentConfig;
	Result.bValid = Result.Effective.ClientCount >= 1
		&& (Result.Effective.PlayNetMode == TEXT("standalone")
			|| Result.Effective.PlayNetMode == TEXT("listen_server")
			|| Result.Effective.PlayNetMode == TEXT("client"));
	if (!Result.bValid)
	{
		Result.Source += TEXT("_invalid");
		return Result;
	}
	Result.bMultiprocess = Result.Effective.bExternalSessionDestination
		|| Result.Effective.bServerWasLaunched
		|| (!Result.Effective.bRunUnderOneProcess
			&& (Result.Effective.ClientCount > 1
				|| Result.Effective.bLaunchSeparateServer));
	return Result;
}

bool FHyperAIStudioNativeReadContracts::ValidatePageRequest(
	const int32 PageSize,
	const FString& Cursor,
	FString& OutDiagnostic)
{
	OutDiagnostic.Reset();
	if (PageSize < 1 || PageSize > MaxPageSize)
	{
		OutDiagnostic = FString::Printf(TEXT("page_size must be between 1 and %d."), MaxPageSize);
		return false;
	}
	if (Cursor.Len() > MaxCursorCharacters)
	{
		OutDiagnostic = FString::Printf(TEXT("cursor exceeds the %d character limit."), MaxCursorCharacters);
		return false;
	}
	return true;
}

FString FHyperAIStudioNativeReadContracts::MakeCursor(
	const FString& SnapshotFingerprint,
	const int32 Offset)
{
	return SnapshotFingerprint.IsEmpty() || Offset < 0
		? FString()
		: FString::Printf(TEXT("%s@%d"), *SnapshotFingerprint, Offset);
}

bool FHyperAIStudioNativeReadContracts::ParseCursor(
	const FString& Cursor,
	const FString& ExpectedSnapshotFingerprint,
	const int32 TotalCount,
	int32& OutOffset,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	OutOffset = 0;
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (Cursor.IsEmpty())
	{
		return true;
	}
	if (Cursor.Len() > MaxCursorCharacters)
	{
		OutStatus = TEXT("invalid_cursor");
		OutDiagnostic = FString::Printf(TEXT("cursor exceeds the %d character limit."), MaxCursorCharacters);
		return false;
	}

	FString Fingerprint;
	FString OffsetText;
	if (!Cursor.Split(TEXT("@"), &Fingerprint, &OffsetText, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		|| Fingerprint.IsEmpty()
		|| OffsetText.IsEmpty())
	{
		OutStatus = TEXT("invalid_cursor");
		OutDiagnostic = TEXT("cursor has an invalid format.");
		return false;
	}
	if (Fingerprint != ExpectedSnapshotFingerprint)
	{
		OutStatus = TEXT("stale_cursor");
		OutDiagnostic = TEXT("cursor belongs to a different snapshot; restart pagination without a cursor.");
		return false;
	}

	int64 ParsedOffset = INDEX_NONE;
	if (!LexTryParseString(ParsedOffset, *OffsetText)
		|| ParsedOffset < 0
		|| ParsedOffset > TotalCount
		|| ParsedOffset > MAX_int32)
	{
		OutStatus = TEXT("invalid_cursor");
		OutDiagnostic = TEXT("cursor offset is outside the current bounded result set.");
		return false;
	}
	OutOffset = static_cast<int32>(ParsedOffset);
	return true;
}

FString FHyperAIStudioNativeReadContracts::HashTokens(const TArray<FString>& Tokens)
{
	FString Buffer;
	for (const FString& Token : Tokens)
	{
		Buffer += FString::FromInt(Token.Len());
		Buffer += TEXT(":");
		Buffer += Token;
		Buffer += TEXT("|");
	}
	const FTCHARToUTF8 Utf8(*Buffer);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	FString Hex;
	Hex.Reserve(FSHA1::DigestSize * 2 + 5);
	Hex += TEXT("sha1:");
	for (const uint8 Byte : Digest)
	{
		Hex += FString::Printf(TEXT("%02x"), Byte);
	}
	return Hex;
}

FString FHyperAIStudioNativeReadContracts::ClipText(
	const FString& Value,
	const int32 MaxCharacters,
	bool* bOutTruncated)
{
	const int32 SafeLimit = FMath::Max(0, MaxCharacters);
	const bool bWasTruncated = Value.Len() > SafeLimit;
	if (bOutTruncated)
	{
		*bOutTruncated = bWasTruncated;
	}
	return bWasTruncated ? Value.Left(SafeLimit) : Value;
}

FString FHyperAIStudioNativeReadContracts::ClassifyWatchPreviewProperty(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("unavailable");
	}
	if (CastField<FBoolProperty>(Property)
		|| CastField<FNumericProperty>(Property)
		|| CastField<FEnumProperty>(Property)
		|| CastField<FNameProperty>(Property))
	{
		return TEXT("bounded_scalar");
	}
	if (CastField<FStrProperty>(Property) || CastField<FTextProperty>(Property))
	{
		return TEXT("bounded_string");
	}
	return TEXT("complex_or_unsupported");
}

FString FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
	const FHyperAIBlueprintDebugReport& Report,
	const int32 RequestedPageSize,
	const TArray<FHyperAIBlueprintDebugItem>& Items)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	TArray<FString> Tokens = {
		TEXT("hyperai.blueprint-debug-snapshot.v2"),
		Report.AssetPath,
		Report.AssetClass,
		Report.CompileStatus,
		Report.GeneratedClassPath,
		Report.bHasDebuggingData ? TEXT("1") : TEXT("0"),
		Report.DebugObjectPath,
		Report.GraphFilter,
		FString::FromInt(Report.GraphCount),
		FString::FromInt(Report.ScannedGraphCount),
		FString::FromInt(Report.ScannedNodeCount),
		FString::FromInt(Report.BreakpointCount),
		FString::FromInt(Report.WatchCount),
		Report.WatchValuePolicy,
		FString::FromInt(Report.DiagnosticNodeCount),
		FString::FromInt(Report.TotalItemCount),
		FString::FromInt(RequestedPageSize),
		Report.bScanTruncated ? TEXT("1") : TEXT("0")
	};
	AppendDebugPieIdentityFingerprint(Tokens, Report.DebugPieIdentity);
	for (const FHyperAIBlueprintDebugItem& Item : Items)
	{
		AppendBlueprintItemFingerprint(Tokens, Item);
	}
	return HashTokens(Tokens);
}

FString FHyperAIStudioNativeReadContracts::OperationStateToStatus(
	const EHyperAIStudioOperationState State)
{
	switch (State)
	{
	case EHyperAIStudioOperationState::Queued:
		return TEXT("queued");
	case EHyperAIStudioOperationState::Running:
		return TEXT("running");
	case EHyperAIStudioOperationState::CommitStarted:
		return TEXT("commit_started");
	case EHyperAIStudioOperationState::Completed:
		return TEXT("completed");
	case EHyperAIStudioOperationState::Failed:
		return TEXT("failed");
	case EHyperAIStudioOperationState::CancelRequested:
		return TEXT("cancel_requested");
	case EHyperAIStudioOperationState::RolledBack:
		return TEXT("rolled_back");
	case EHyperAIStudioOperationState::Partial:
		return TEXT("partial");
	case EHyperAIStudioOperationState::OutcomeUnknown:
		return TEXT("outcome_unknown");
	default:
		return TEXT("invalid");
	}
}

FString FHyperAIStudioNativeReadContracts::RollbackStateToStatus(
	const EHyperAIStudioRollbackState State)
{
	switch (State)
	{
	case EHyperAIStudioRollbackState::NotNeeded:
		return TEXT("not_needed");
	case EHyperAIStudioRollbackState::Complete:
		return TEXT("complete");
	case EHyperAIStudioRollbackState::Partial:
		return TEXT("partial");
	case EHyperAIStudioRollbackState::Unsupported:
		return TEXT("unsupported");
	case EHyperAIStudioRollbackState::Unknown:
		return TEXT("unknown");
	default:
		return TEXT("invalid");
	}
}

FString FHyperAIStudioNativeReadContracts::ClassifyJournalLoadFailure(const FString& LoadError)
{
	return LoadError.Contains(TEXT("Another HyperAIStudio journal owner is active"), ESearchCase::CaseSensitive)
		? TEXT("busy")
		: TEXT("journal_unavailable");
}

FHyperAICapabilityReport UHyperAIStudioNativeReadToolset::hyper_capability_report(
	const int32 PageSize,
	const FString& Cursor)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	FHyperAICapabilityReport Report;
	Report.SnapshotUtc = NowUtc();
	Report.EngineVersion = FEngineVersion::Current().ToString();
	Report.HyperAIPluginVersion = GetPluginVersion(TEXT("HyperAIStudio"));
	Report.EpicMcpPluginVersion = GetPluginVersion(TEXT("ModelContextProtocol"));
	Report.ProjectName = FHyperAIStudioNativeReadContracts::ClipText(
		FString(FApp::GetProjectName()),
		MaxNodeTitleCharacters);
	Report.ProjectIdentityHash = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(
		FHyperAIStudioService::GetProjectRoot());
	Report.ProjectIdentityScheme = TEXT("hyperai.canonical-project-directory-id.v1");
	Report.ProjectIdentityHashAlgorithm = Report.ProjectIdentityHash.StartsWith(
		TEXT("sha256:"), ESearchCase::CaseSensitive)
		? TEXT("sha256")
		: (Report.ProjectIdentityHash.StartsWith(TEXT("sha1:"), ESearchCase::CaseSensitive)
			|| IsLegacyRawSha1(Report.ProjectIdentityHash)
			? TEXT("sha1")
			: TEXT("unavailable"));
	Report.ProjectIdentityStatus = Report.ProjectIdentityHash.IsEmpty()
		? TEXT("unavailable")
		: (Report.ProjectIdentityHashAlgorithm == TEXT("sha1")
			? TEXT("available_legacy_sha1")
			: TEXT("available"));
	Report.NativeToolset = FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName();
	Report.DependencyGraphToolset = FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName();
	Report.NativeToolChannel = FHyperAIStudioExtensionRuntime::GetNativeToolChannel()
		== EHyperAIStudioNativeToolChannel::Preview ? TEXT("preview") : TEXT("stable_only");
	Report.bExtendedHyperToolsEnabled =
		FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled();
	Report.bSourceCandidateToolsEnabled = Report.bExtendedHyperToolsEnabled;

	FString PageDiagnostic;
	if (!FHyperAIStudioNativeReadContracts::ValidatePageRequest(PageSize, Cursor, PageDiagnostic))
	{
		Report.Status = TEXT("invalid_input");
		Report.Diagnostic = FHyperAIStudioNativeReadContracts::ClipText(
			PageDiagnostic,
			FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
		Report.DiagnosticSummary = TEXT("The capability request is invalid. Check its page size and cursor, then retry.");
		return Report;
	}

	if (!IsBuiltInCatalogValid())
	{
		Report.Status = TEXT("capability_catalog_invalid");
		Report.Diagnostic = TEXT("The immutable generated capability catalog failed semantic validation.");
		Report.DiagnosticSummary = TEXT("The Extended Hyper Tools catalog could not be validated.");
		return Report;
	}
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	Report.CapabilityCatalogFingerprint = Catalog.GeneratedFingerprint;
	Report.TotalKnownHyperAIToolCount = Catalog.Tools.Num();
	Report.IncludedHyperAIToolCount = Report.bExtendedHyperToolsEnabled
		? Report.TotalKnownHyperAIToolCount
		: 0;
	if (Catalog.Tools.Num() > FHyperAIStudioNativeReadContracts::MaxCatalogToolCount)
	{
		Report.Status = TEXT("capability_catalog_limit_exceeded");
		Report.Diagnostic = TEXT("The generated capability catalog exceeds the fixed report output bound.");
		Report.DiagnosticSummary = TEXT("The Extended Hyper Tools catalog is larger than this plugin version supports.");
		return Report;
	}

	TArray<FHyperAIStudioRuntimeToolBinding> RuntimeBindings;
	Report.bRuntimeIndexValid = FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(
		RuntimeBindings,
		Report.RuntimeIndexDiagnostics);
	Report.RuntimeImplementedHyperAIToolCount = RuntimeBindings.Num();
	TMap<FString, const FHyperAIStudioRuntimeToolBinding*> RuntimeByToolName;
	TSet<FString> LoadedToolsets;
	TSet<FString> RegisteredToolsets;
	for (const FHyperAIStudioRuntimeToolBinding& Binding : RuntimeBindings)
	{
		RuntimeByToolName.Add(Binding.ToolName, &Binding);
		LoadedToolsets.Add(Binding.QualifiedToolset);
		if (Binding.bToolsetRegistered)
		{
			RegisteredToolsets.Add(Binding.QualifiedToolset);
			++Report.RegisteredHyperAIToolCount;
			Report.bNativeToolsetRegistered |= Binding.QualifiedToolset == Report.NativeToolset;
			Report.bDependencyGraphToolsetRegistered |=
				Binding.QualifiedToolset == Report.DependencyGraphToolset;
		}
	}
	Report.LoadedHyperAIToolsetCount = LoadedToolsets.Num();
	Report.RegisteredHyperAIToolsetCount = RegisteredToolsets.Num();

	for (const FHyperAIStudioCapabilityToolDefinition* CatalogTool : SortedCatalogTools())
	{
		if (!CatalogTool)
		{
			continue;
		}
		const FHyperAIStudioRuntimeToolBinding* RuntimeBinding =
			RuntimeByToolName.FindRef(CatalogTool->Name);
		const bool bAdmissionAllowsCall =
			CatalogTool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted
				|| (Report.bSourceCandidateToolsEnabled
				&& CatalogTool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate);

		FHyperAINativeToolSummary Item;
		Item.Name = CatalogTool->Name;
		Item.PackId = CatalogTool->PackId;
		Item.AtomicCohortId = CatalogTool->AtomicCohortId;
		Item.Toolset = RuntimeBinding ? RuntimeBinding->QualifiedToolset : FString();
		Item.ExternalEffectPolicy = CatalogTool->bMayCauseExternalEffects
			? TEXT("may_cause_external_effects")
			: TEXT("no_external_effect_flag");
		Item.ExecutionSupport = ExecutionSupportForTool(*CatalogTool);
		Item.AdmissionState = FHyperAIStudioCapabilityPackRegistry::LexToString(
			CatalogTool->AdmissionState);
		Item.SourceArtifactCount = CatalogTool->SourceArtifactCount;
		Item.bImplementationLoaded = RuntimeBinding && RuntimeBinding->bImplementationLoaded;
		Item.bToolsetRegistered = RuntimeBinding && RuntimeBinding->bToolsetRegistered;
		Item.bRuntimeEnabled = RuntimeBinding && RuntimeBinding->bToolEnabled;
		Item.bCallable = Item.bImplementationLoaded && Item.bToolsetRegistered
			&& Item.bRuntimeEnabled && bAdmissionAllowsCall;
		Item.Description = RuntimeBinding
			? (!RuntimeBinding->Description.IsEmpty()
				? FHyperAIStudioNativeReadContracts::ClipText(
					RuntimeBinding->Description, MaxNodeTitleCharacters)
				: FString::Printf(
					TEXT("Loaded runtime implementation bound to generated %s capability contract."),
					*CatalogTool->PackId).Left(MaxNodeTitleCharacters))
			: FString::Printf(
				TEXT("Generated %s capability contract; runtime implementation is not loaded."),
				*CatalogTool->PackId).Left(MaxNodeTitleCharacters);

		switch (CatalogTool->AdmissionState)
		{
		case EHyperAIStudioCapabilityAdmissionState::Planned:
			++Report.PlannedHyperAIToolCount;
			Item.Availability = TEXT("planned");
			break;
		case EHyperAIStudioCapabilityAdmissionState::SourceCandidate:
			++Report.SourceCandidateHyperAIToolCount;
			Item.Availability = Item.bCallable
					? TEXT("callable_preview")
				: (!Item.bImplementationLoaded
					? TEXT("source_candidate_implementation_unloaded")
						: (Report.bSourceCandidateToolsEnabled
						? (Item.bToolsetRegistered && !Item.bRuntimeEnabled
							? TEXT("tool_filtered_or_disabled")
							: TEXT("registration_unavailable"))
							: TEXT("source_candidate_stable_only")));
			break;
		case EHyperAIStudioCapabilityAdmissionState::Admitted:
			++Report.AdmittedHyperAIToolCount;
			Item.Availability = Item.bCallable
				? TEXT("callable_admitted")
				: (Item.bImplementationLoaded
					? (Item.bToolsetRegistered && !Item.bRuntimeEnabled
						? TEXT("tool_filtered_or_disabled")
						: TEXT("registration_unavailable"))
					: TEXT("admitted_implementation_unloaded"));
			break;
		default:
			Item.Availability = TEXT("invalid_admission_state");
			break;
		}
		Report.NativeTools.Add(Item);
		if (Item.bCallable)
		{
			Report.CallableHyperAITools.Add(MoveTemp(Item));
		}
	}
	Report.CallableHyperAIToolCount = Report.CallableHyperAITools.Num();
	const bool bRuntimeRowsConsistent =
		FHyperAIStudioNativeReadContracts::AreCapabilityRuntimeRowsConsistent(
			Report.NativeTools,
				Report.bSourceCandidateToolsEnabled,
			Report.ExpectedRegisteredHyperAIToolCount,
			Report.RegistrationMismatchCount);

	const FHyperAIStudioService Service;
	const FHyperAIStudioStatus Current = Service.GetMcpRuntimeStatusFast();
	const TOptional<FHyperAIStudioCapabilityInventoryResult> CachedInventory =
		FHyperAIStudioCapabilityInventoryClient::GetCached(
			Current.Endpoint,
			FTimespan::MaxValue());
	Report.CurrentStatus = FHyperAIStudioNativeReadContracts::ClipText(Current.Summary, MaxNodeTitleCharacters);
	Report.McpEndpoint = FHyperAIStudioNativeReadContracts::ClipText(
		Current.Endpoint,
		FHyperAIStudioNativeReadContracts::MaxPathCharacters);
	Report.bMcpServerRunning = Current.bServerRunning;
	Report.bMcpPortListening = Current.bPortListening;
	Report.bMcpToolsListReachable = CachedInventory.IsSet() && CachedInventory->bSuccess;
	Report.bMcpToolSearchMode = Report.bMcpToolsListReachable
		? CachedInventory->Snapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::ToolSearch
		: Current.bToolSearchMode;
	Report.McpTopLevelRegisteredToolCount = Current.RegisteredToolCount;

	TArray<FHyperAIEpicToolsetSummary> EpicToolsets;
	TArray<FString> FingerprintTokens;
	FingerprintTokens.Reserve(Report.NativeTools.Num() + 2);
	FingerprintTokens.Add(Report.CapabilityCatalogFingerprint);
	for (const FHyperAINativeToolSummary& Tool : Report.NativeTools)
	{
		FingerprintTokens.Add(FString::Printf(
			TEXT("%s|%s|%s|%s|%d|%d"),
			*Tool.Name,
			*Tool.AdmissionState,
			*Tool.Availability,
			*Tool.Toolset,
			Tool.bImplementationLoaded ? 1 : 0,
			Tool.bToolsetRegistered ? 1 : 0));
	}
	if (CachedInventory.IsSet() && CachedInventory->bSuccess)
	{
		const FHyperAIStudioCapabilitySnapshot& Snapshot = CachedInventory->Snapshot;
		Report.bCachedEpicInventoryAvailable = true;
		Report.bCachedEpicInventoryRefreshing = CachedInventory->bRefreshing || Snapshot.bRefreshing;
		Report.bCachedEpicInventorySourceTruncated = Snapshot.bTruncated;
		Report.CachedEpicInventoryFingerprint = FHyperAIStudioNativeReadContracts::ClipText(
			Snapshot.InventoryFingerprint,
			MaxNodeTitleCharacters);
		Report.CachedEpicInventoryCapturedUtc = Snapshot.CapturedAtUtc.ToIso8601();
		Report.bCachedEpicInventoryStale = Snapshot.bStale
			|| Snapshot.CapturedAtUtc == FDateTime()
			|| FDateTime::UtcNow() - Snapshot.CapturedAtUtc > FTimespan::FromSeconds(30.0);
		Report.CachedEpicDiscoveryMode = DiscoveryModeToString(Snapshot.DiscoveryMode);
		Report.CachedEpicToolsetCount = Snapshot.Toolsets.Num();
		Report.CachedEpicDescribedToolCount = Snapshot.DescribedToolCount;

		TMap<FString, FHyperAIStudioDescribedToolset> DescribedByName;
		for (const FHyperAIStudioDescribedToolset& Described : Snapshot.DescribedToolsets)
		{
			DescribedByName.Add(Described.Name, Described);
		}
		for (const FHyperAIStudioDiscoveredToolset& Discovered : Snapshot.Toolsets)
		{
			FHyperAIEpicToolsetSummary Item;
			Item.Name = FHyperAIStudioNativeReadContracts::ClipText(Discovered.Name, MaxNodeTitleCharacters);
			Item.Description = FHyperAIStudioNativeReadContracts::ClipText(
				Discovered.Description,
				MaxNodeTitleCharacters);
			if (const FHyperAIStudioDescribedToolset* Described = DescribedByName.Find(Discovered.Name))
			{
				Item.Version = FHyperAIStudioNativeReadContracts::ClipText(Described->Version, MaxNodeTitleCharacters);
				Item.SchemaHash = FHyperAIStudioNativeReadContracts::ClipText(Described->SchemaHash, MaxNodeTitleCharacters);
				Item.ToolCount = Described->ToolCount;
				Item.InventoryState = TEXT("described");
			}
			else
			{
				Item.InventoryState = TEXT("discovered");
			}
			EpicToolsets.Add(MoveTemp(Item));
		}
		EpicToolsets.Sort([](const FHyperAIEpicToolsetSummary& A, const FHyperAIEpicToolsetSummary& B)
		{
			return A.Name < B.Name;
		});
		FingerprintTokens.Add(Snapshot.InventoryFingerprint);
	}

	for (const FHyperAIEpicToolsetSummary& Item : EpicToolsets)
	{
		FingerprintTokens.Add(FString::Printf(
			TEXT("%s|%s|%d|%s"),
			*Item.Name,
			*Item.Version,
			Item.ToolCount,
			*Item.SchemaHash));
	}
	const FString SnapshotFingerprint = FHyperAIStudioNativeReadContracts::HashTokens(FingerprintTokens);
	int32 Offset = 0;
	FString CursorStatus;
	FString CursorDiagnostic;
	if (!FHyperAIStudioNativeReadContracts::ParseCursor(
		Cursor,
		SnapshotFingerprint,
		EpicToolsets.Num(),
		Offset,
		CursorStatus,
		CursorDiagnostic))
	{
		Report.Status = CursorStatus;
		Report.Diagnostic = FHyperAIStudioNativeReadContracts::ClipText(
			CursorDiagnostic,
			FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
		Report.DiagnosticSummary = TEXT("The requested tool page is no longer current. Start again without a cursor.");
		return Report;
	}

	ApplyPage(EpicToolsets, Offset, PageSize, SnapshotFingerprint, Report);
	Report.bOk = Report.bRuntimeIndexValid
		&& Report.TotalKnownHyperAIToolCount == FHyperAIStudioNativeReadContracts::MaxCatalogToolCount
		&& Report.PlannedHyperAIToolCount + Report.SourceCandidateHyperAIToolCount
			+ Report.AdmittedHyperAIToolCount == Report.TotalKnownHyperAIToolCount
		&& bRuntimeRowsConsistent;
	if (!Report.bRuntimeIndexValid)
	{
		Report.Status = TEXT("runtime_index_invalid");
		Report.Diagnostic = TEXT("One or more loaded HyperAI implementations could not be bound uniquely to the generated capability catalog.");
		Report.DiagnosticSummary = TEXT("Some Extended Hyper Tools could not be identified reliably.");
	}
	else if (Report.RegistrationMismatchCount > 0)
	{
		Report.Status = TEXT("registration_unavailable");
		Report.Diagnostic = TEXT("One or more loaded HyperAI implementations do not match their generated admission registration state.");
		Report.DiagnosticSummary = TEXT("Some included HyperAI tools are loaded but not registered with Unreal MCP.");
	}
	else if (!Report.bOk)
	{
		Report.Status = TEXT("capability_report_inconsistent");
		Report.Diagnostic = TEXT("The generated catalog aggregates or callable runtime aggregates are inconsistent.");
		Report.DiagnosticSummary = TEXT("The Extended Hyper Tools inventory is internally inconsistent.");
	}
	else if (Report.SourceCandidateHyperAIToolCount > 0 && !Report.bSourceCandidateToolsEnabled)
	{
		Report.Status = Report.AdmittedHyperAIToolCount > 0
			? TEXT("partial_source_candidates_stable_only")
			: TEXT("source_candidates_stable_only");
		Report.Diagnostic = Report.AdmittedHyperAIToolCount > 0
			? TEXT("Stable Only exposes admitted HyperAI tools; SourceCandidate contracts remain visible but non-callable.")
			: TEXT("Stable Only keeps SourceCandidate contracts visible but non-callable; switch to Preview to enable them without changing admission evidence.");
		Report.DiagnosticSummary = TEXT("Unreal MCP Only is selected. Enable Extended Hyper Tools to include the HyperAI tools.");
	}
	else if (!Report.bCachedEpicInventoryAvailable)
	{
		Report.Status = TEXT("ok_no_cached_epic_inventory");
		Report.Diagnostic = TEXT("Every loaded admission-authorized HyperAI implementation is callable; unloaded optional cohorts remain non-callable. No successful cached Epic deep inventory is currently available. This call does not start network I/O.");
		Report.DiagnosticSummary = FString::Printf(
			TEXT("%d Extended Hyper Tools are included; %d are currently registered. Epic Unreal MCP tool details are not cached yet."),
			Report.IncludedHyperAIToolCount,
			Report.RegisteredHyperAIToolCount);
	}
	else if (Report.bTruncated || Report.bCachedEpicInventoryStale)
	{
		Report.Status = TEXT("partial");
		Report.Diagnostic = Report.bCachedEpicInventoryStale
			? TEXT("The installed HyperAI surface is current; the attached Epic inventory cache is stale or paged.")
			: TEXT("The installed HyperAI surface is current; the attached Epic inventory is paged or source-truncated.");
		Report.DiagnosticSummary = TEXT("Extended Hyper Tools are ready; Epic Unreal MCP tool details are still refreshing or paged.");
	}
	else
	{
		Report.Status = TEXT("ok");
		Report.Diagnostic = TEXT("Installed callable HyperAI tools and the cached Epic inventory page are current within the reported snapshot.");
		Report.DiagnosticSummary = TEXT("Extended Hyper Tools and the current Epic Unreal MCP tool page are ready.");
	}
	Report.Diagnostic = FHyperAIStudioNativeReadContracts::ClipText(
		Report.Diagnostic,
		FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
	Report.DiagnosticSummary = FHyperAIStudioNativeReadContracts::ClipText(
		Report.DiagnosticSummary,
		FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
	return Report;
}

FHyperAIOperationStatus UHyperAIStudioNativeReadToolset::hyper_operation_status(
	const FString& OperationId)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	FHyperAIOperationStatus Report;
	Report.SnapshotUtc = NowUtc();
	Report.OperationId = FHyperAIStudioNativeReadContracts::ClipText(OperationId, 128);
	Report.JournalAccessMode = TEXT("read_only_generation_snapshot");
	Report.bJournalLoadMayReconcile = false;
	if (!FHyperAIStudioOperationJournal::IsValidOperationId(OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("operation_id must satisfy the bounded HyperAI operation-id contract.");
		return Report;
	}

	FHyperAIStudioPlanExecutionStatus LiveStatus;
	if (FHyperAIStudioPlanExecutionService::QueryStatus(OperationId, LiveStatus))
	{
		Report.bOk = true;
		Report.bFound = true;
		Report.Status = LiveStatus.Status;
		Report.State = LiveStatus.Status;
		Report.StatusCode = LiveStatus.Status;
		Report.Diagnostic = FHyperAIStudioNativeReadContracts::ClipText(
			LiveStatus.Diagnostic,
			FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
		Report.PlanHash = LiveStatus.PlanHash;
		Report.CapabilityHash = LiveStatus.CapabilityHash;
		Report.bTerminal = LiveStatus.bTerminal;
		Report.bNeedsReconciliation = LiveStatus.bOutcomeUnknown;
		Report.bRetrySafe = false;
		Report.bPartialCommit = LiveStatus.CoordinatorState
			== EHyperAIStudioPlanCoordinatorState::Partial;
		Report.RollbackState = LiveStatus.bOutcomeUnknown
			? TEXT("unknown") : TEXT("not_applicable");
		Report.JournalAccessMode = TEXT("live_execution_registry");
		Report.bJournalLoadMayReconcile = false;
		Report.ProjectIdentityHash = LiveStatus.CanonicalProjectId;
		return Report;
	}

	FHyperAIStudioOperationJournal Journal(FHyperAIStudioService::GetProjectRoot());
	Report.ProjectIdentityHash = Journal.GetCanonicalProjectId();
	FString LoadError;
	if (!Journal.LoadReadOnly(LoadError))
	{
		Report.Status = FHyperAIStudioNativeReadContracts::ClassifyJournalLoadFailure(LoadError);
		Report.Diagnostic = Report.Status == TEXT("busy")
			? TEXT("The durable journal files are temporarily unavailable for a read-only generation snapshot; retry shortly.")
			: FHyperAIStudioNativeReadContracts::ClipText(
				LoadError,
				FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters);
		return Report;
	}

	Report.ReconciledRecordCount = 0;
	Report.JournalGeneration = Journal.GetGeneration();
	const TOptional<FHyperAIStudioOperationRecord> Record = Journal.Find(OperationId);
	if (!Record.IsSet())
	{
		Report.bOk = true;
		Report.Status = TEXT("not_found");
		Report.Diagnostic = TEXT("The read-only journal snapshot contains no record with this operation_id.");
		return Report;
	}

	Report.bOk = true;
	Report.bFound = true;
	Report.State = FHyperAIStudioNativeReadContracts::OperationStateToStatus(Record->State);
	Report.Status = Report.State;
	Report.RollbackState = FHyperAIStudioNativeReadContracts::RollbackStateToStatus(Record->RollbackState);
	Report.StatusCode = Record->StatusCode;
	Report.PlanHash = Record->PlanHash;
	Report.CapabilityHash = Record->CapabilityHash;
	Report.CreatedUtc = Record->CreatedUtc;
	Report.UpdatedUtc = Record->UpdatedUtc;
	Report.bTerminal = Record->IsTerminal();
	Report.bNeedsReconciliation = Record->NeedsReconciliation();
	Report.bRetrySafe = Record->bRetrySafe;
	Report.bPartialCommit = Record->bPartialCommit;
	Report.ResolutionValidatorHash = Record->ResolutionValidatorHash;
	Report.ResolutionPostconditionHash = Record->ResolutionPostconditionHash;
	Report.Diagnostic = TEXT("Operation status loaded from a validated read-only journal generation.");
	return Report;
}

FHyperAIBlueprintDebugReport UHyperAIStudioNativeReadToolset::hyper_blueprint_debug_inspect(
	const FString& BlueprintAssetPath,
	const FString& GraphName,
	const int32 PageSize,
	const FString& Cursor)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	FHyperAIBlueprintDebugReport Report;
	Report.SnapshotUtc = NowUtc();
	Report.GraphFilter = FHyperAIStudioNativeReadContracts::ClipText(GraphName, MaxGraphFilterCharacters);

	FString PageDiagnostic;
	if (!FHyperAIStudioNativeReadContracts::ValidatePageRequest(PageSize, Cursor, PageDiagnostic))
	{
		Report.Status = TEXT("invalid_input");
		Report.Diagnostic = PageDiagnostic;
		return Report;
	}
	if (BlueprintAssetPath.IsEmpty()
		|| BlueprintAssetPath.Len() > FHyperAIStudioNativeReadContracts::MaxPathCharacters
		|| GraphName.Len() > MaxGraphFilterCharacters)
	{
		Report.Status = TEXT("invalid_input");
		Report.Diagnostic = TEXT("blueprint_asset_path and graph_name must satisfy their bounded path/name limits.");
		return Report;
	}

	const FSoftObjectPath AssetReference(BlueprintAssetPath);
	const FString LongPackageName = AssetReference.GetLongPackageName();
	if (!AssetReference.IsValid()
		|| LongPackageName.IsEmpty()
		|| !FPackageName::IsValidLongPackageName(LongPackageName)
		|| LongPackageName.Contains(TEXT("..")))
	{
		Report.Status = TEXT("invalid_asset_path");
		Report.Diagnostic = TEXT("blueprint_asset_path must be a valid Unreal object path, not a filesystem path.");
		return Report;
	}

	UObject* LoadedObject = AssetReference.ResolveObject();
	if (!LoadedObject)
	{
		Report.Status = TEXT("asset_not_loaded");
		Report.Diagnostic = TEXT("The Blueprint is not already loaded. Open it in Unreal Editor or use Epic's asset/editor tools to open it, then retry; HyperAI does not synchronously load assets on the MCP game thread.");
		return Report;
	}
	UBlueprint* Blueprint = Cast<UBlueprint>(LoadedObject);
	if (!Blueprint)
	{
		Report.Status = TEXT("not_a_blueprint");
		Report.Diagnostic = TEXT("The already-loaded Unreal object is not a Blueprint.");
		return Report;
	}

	Report.AssetPath = FHyperAIStudioNativeReadContracts::ClipText(
		Blueprint->GetPathName(),
		FHyperAIStudioNativeReadContracts::MaxPathCharacters);
	Report.AssetClass = FHyperAIStudioNativeReadContracts::ClipText(
		Blueprint->GetClass()->GetPathName(),
		FHyperAIStudioNativeReadContracts::MaxPathCharacters);
	Report.CompileStatus = BlueprintStatusToString(Blueprint->Status);
	Report.GeneratedClassPath = Blueprint->GeneratedClass
		? FHyperAIStudioNativeReadContracts::ClipText(
			Blueprint->GeneratedClass->GetPathName(),
			FHyperAIStudioNativeReadContracts::MaxPathCharacters)
		: FString();
	Report.bHasDebuggingData = FKismetDebugUtilities::HasDebuggingData(Blueprint);
	UObject* DebugObject = Blueprint->GetObjectBeingDebugged();
	if (DebugObject)
	{
		Report.DebugObjectPath = FHyperAIStudioNativeReadContracts::ClipText(
			DebugObject->GetPathName(),
			FHyperAIStudioNativeReadContracts::MaxPathCharacters);
		if (const UWorld* DebugWorld = DebugObject->GetWorld())
		{
				Report.DebugPieIdentity = MakeDebugPieIdentity(
					DescribeWorld(DebugWorld, FindWorldContext(DebugWorld)));
		}
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	Graphs.Remove(nullptr);
	Graphs.Sort([](const UEdGraph& A, const UEdGraph& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	Report.GraphCount = Graphs.Num();
	if (!GraphName.IsEmpty()
		&& !Graphs.ContainsByPredicate([&GraphName](const UEdGraph* Graph)
		{
			return GraphMatchesFilter(Graph, GraphName);
		}))
	{
		Report.Status = TEXT("graph_not_found");
		Report.Diagnostic = TEXT("No graph in this Blueprint exactly matches graph_name or graph path.");
		return Report;
	}

	TArray<FHyperAIBlueprintDebugItem> Items;
	Items.Reserve(FMath::Min(FHyperAIStudioNativeReadContracts::MaxBlueprintDebugItems, 256));
	auto TryAddItem = [&Items, &Report](FHyperAIBlueprintDebugItem&& Item)
	{
		if (Items.Num() >= FHyperAIStudioNativeReadContracts::MaxBlueprintDebugItems)
		{
			Report.bScanTruncated = true;
			return false;
		}
		Items.Add(MoveTemp(Item));
		return true;
	};

	if (UEdGraphNode* CurrentInstruction = FKismetDebugUtilities::GetCurrentInstruction();
		NodeBelongsToBlueprint(CurrentInstruction, Blueprint)
		&& GraphMatchesFilter(CurrentInstruction->GetGraph(), GraphName))
	{
		FHyperAIBlueprintDebugItem Item;
		Item.Kind = TEXT("current_instruction");
		Item.State = TEXT("active");
		Item.bValid = true;
		PopulateGraphNodeIdentity(Item, CurrentInstruction);
		TryAddItem(MoveTemp(Item));
	}
	if (UEdGraphNode* RecentBreakpoint = FKismetDebugUtilities::GetMostRecentBreakpointHit();
		NodeBelongsToBlueprint(RecentBreakpoint, Blueprint)
		&& GraphMatchesFilter(RecentBreakpoint->GetGraph(), GraphName))
	{
		FHyperAIBlueprintDebugItem Item;
		Item.Kind = TEXT("most_recent_breakpoint_hit");
		Item.State = TEXT("hit");
		Item.bValid = true;
		PopulateGraphNodeIdentity(Item, RecentBreakpoint);
		TryAddItem(MoveTemp(Item));
	}

	// UE 5.8 keeps breakpoint/watch records in the public per-Blueprint editor
	// settings map. Read that storage directly so the source arrays can be capped by
	// index; the public Foreach helpers necessarily traverse every record even after
	// our output budget is exhausted.
	const UBlueprintEditorSettings* BlueprintEditorSettings = GetDefault<UBlueprintEditorSettings>();
	const FPerBlueprintSettings* PerBlueprintSettings = BlueprintEditorSettings
		? BlueprintEditorSettings->PerBlueprintSettings.Find(Blueprint->GetPathName())
		: nullptr;
	if (PerBlueprintSettings)
	{
		const int32 BreakpointSourceCount = PerBlueprintSettings->Breakpoints.Num();
		const int32 BreakpointScanCount = FMath::Min(
			BreakpointSourceCount,
			FHyperAIStudioNativeReadContracts::MaxBlueprintBreakpoints);
		Report.bScanTruncated |= BreakpointSourceCount > BreakpointScanCount;
		for (int32 BreakpointIndex = 0; BreakpointIndex < BreakpointScanCount; ++BreakpointIndex)
		{
			const FBlueprintBreakpoint& Breakpoint = PerBlueprintSettings->Breakpoints[BreakpointIndex];
			UEdGraphNode* Node = Breakpoint.GetLocation();
			if (Node && !GraphMatchesFilter(Node->GetGraph(), GraphName))
			{
				continue;
			}
			if (!Node && !GraphName.IsEmpty())
			{
				continue;
			}
			++Report.BreakpointCount;
			FHyperAIBlueprintDebugItem Item;
			Item.Kind = TEXT("breakpoint");
			Item.State = Node ? (Breakpoint.IsEnabled() ? TEXT("enabled") : TEXT("disabled")) : TEXT("unresolved");
			Item.bEnabled = Breakpoint.IsEnabled();
			Item.bValid = Node && FKismetDebugUtilities::IsBreakpointValid(Breakpoint);
			Item.Message = FHyperAIStudioNativeReadContracts::ClipText(
				Breakpoint.GetLocationDescription().ToString(),
				MaxMessageCharacters);
			PopulateGraphNodeIdentity(Item, Node);
			if (!TryAddItem(MoveTemp(Item)))
			{
				break;
			}
		}
	}

	if (PerBlueprintSettings)
	{
		const int32 WatchSourceCount = PerBlueprintSettings->WatchedPins.Num();
		const int32 WatchScanCount = FMath::Min(
			WatchSourceCount,
			FHyperAIStudioNativeReadContracts::MaxBlueprintWatches);
		Report.bScanTruncated |= WatchSourceCount > WatchScanCount;
		for (int32 WatchIndex = 0; WatchIndex < WatchScanCount; ++WatchIndex)
		{
			const FBlueprintWatchedPin& Watch = PerBlueprintSettings->WatchedPins[WatchIndex];
			UEdGraphPin* Pin = Watch.Get();
			if (Pin && !GraphMatchesFilter(Pin->GetOwningNode() ? Pin->GetOwningNode()->GetGraph() : nullptr, GraphName))
			{
				continue;
			}
			if (!Pin && !GraphName.IsEmpty())
			{
				continue;
			}
			++Report.WatchCount;
			FHyperAIBlueprintDebugItem Item;
			Item.Kind = TEXT("watch");
			Item.bEnabled = true;
			Item.bValid = Pin != nullptr;
			PopulatePinIdentity(Item, Pin);
			TArray<FString> PropertySegments;
			const TArray<FName>& PathToProperty = Watch.GetPathToProperty();
			const int32 SegmentCount = FMath::Min(PathToProperty.Num(), MaxWatchPropertySegments);
			for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
			{
				PropertySegments.Add(FHyperAIStudioNativeReadContracts::ClipText(
					PathToProperty[SegmentIndex].ToString(),
					MaxNodeTitleCharacters));
			}
			Item.bPropertyPathTruncated = PathToProperty.Num() > SegmentCount;
			Item.PropertyPath = FHyperAIStudioNativeReadContracts::ClipText(
				FString::Join(PropertySegments, TEXT(".")),
				MaxNodeTitleCharacters);
			if (Pin)
			{
				FString WatchPreview;
				bool bPreviewOmitted = false;
				const FKismetDebugUtilities::EWatchTextResult WatchState =
					GetBoundedWatchPreview(
						Blueprint,
						DebugObject,
						Pin,
						WatchPreview,
						bPreviewOmitted);
				Item.State = bPreviewOmitted
					? TEXT("preview_omitted_complex_or_over_limit")
					: WatchStateToString(WatchState);
				Item.ValuePreview = MoveTemp(WatchPreview);
				Item.bValueTruncated = bPreviewOmitted;
				if (bPreviewOmitted)
				{
					Item.Message = TEXT("Complex/container or oversized watch value preview omitted by the fixed game-thread safety policy.");
				}
			}
			else
			{
				Item.State = TEXT("unresolved");
			}
			if (!TryAddItem(MoveTemp(Item)))
			{
				break;
			}
		}
	}

	for (UEdGraph* Graph : Graphs)
	{
		if (!GraphMatchesFilter(Graph, GraphName))
		{
			continue;
		}
		if (Report.ScannedGraphCount >= FHyperAIStudioNativeReadContracts::MaxBlueprintGraphs)
		{
			Report.bScanTruncated = true;
			break;
		}
		++Report.ScannedGraphCount;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (Report.ScannedNodeCount >= FHyperAIStudioNativeReadContracts::MaxBlueprintNodes)
			{
				Report.bScanTruncated = true;
				break;
			}
			++Report.ScannedNodeCount;
			if (Node->ErrorMsg.IsEmpty())
			{
				continue;
			}
			++Report.DiagnosticNodeCount;
			FHyperAIBlueprintDebugItem Item;
			Item.Kind = TEXT("node_diagnostic");
			Item.Severity = MessageSeverityToString(Node->ErrorType);
			Item.State = Item.Severity;
			Item.bValid = false;
			Item.Message = FHyperAIStudioNativeReadContracts::ClipText(Node->ErrorMsg, MaxMessageCharacters);
			PopulateGraphNodeIdentity(Item, Node);
			if (!TryAddItem(MoveTemp(Item)))
			{
				break;
			}
		}
		if (Report.bScanTruncated)
		{
			break;
		}
	}

	Items.Sort([](const FHyperAIBlueprintDebugItem& A, const FHyperAIBlueprintDebugItem& B)
	{
		return BlueprintItemLess(A, B);
	});
	Report.TotalItemCount = Items.Num();
	const FString Fingerprint = FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
		Report,
		PageSize,
		Items);
	int32 Offset = 0;
	FString CursorStatus;
	FString CursorDiagnostic;
	if (!FHyperAIStudioNativeReadContracts::ParseCursor(
		Cursor,
		Fingerprint,
		Items.Num(),
		Offset,
		CursorStatus,
		CursorDiagnostic))
	{
		Report.Status = CursorStatus;
		Report.Diagnostic = CursorDiagnostic;
		return Report;
	}

	ApplyPage(Items, Offset, PageSize, Fingerprint, Report);
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("partial") : TEXT("ok");
	Report.Diagnostic = Report.bScanTruncated
		? TEXT("Blueprint debugger data hit a hard scan bound; use graph_name to narrow the request.")
		: (Report.NextCursor.IsEmpty()
			? TEXT("Blueprint debugger snapshot completed within all bounds.")
			: TEXT("Blueprint debugger snapshot is paged; continue with next_cursor."));
	return Report;
}

FHyperAIPIEQueryReport UHyperAIStudioNativeReadToolset::hyper_pie_query(const int32 PieInstance)
{
	using namespace HyperAIStudio::NativeReadTools::Private;
	FHyperAIPIEQueryReport Report;
	Report.SnapshotUtc = NowUtc();
	Report.PieInstanceFilter = PieInstance;

	if (PieInstance < INDEX_NONE
		|| PieInstance > 1024)
	{
		Report.Status = TEXT("invalid_input");
		Report.Diagnostic = TEXT("pie_instance must be -1 for all worlds or between 0 and 1024.");
		return Report;
	}

	TArray<FHyperAIPIEWorldIdentity> ObservableWorlds;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE || !Context.World())
			{
				continue;
			}
			++Report.ObservableWorldCount;
			if (ObservableWorlds.Num() >= FHyperAIStudioNativeReadContracts::MaxPieWorlds)
			{
				Report.bSourceTruncated = true;
				continue;
			}
			ObservableWorlds.Add(DescribeWorld(Context.World(), &Context));
		}
	}

	const TOptional<FPlayInEditorSessionInfo> ActiveSessionInfo = GEditor
		? GEditor->GetPlayInEditorSessionInfo()
		: TOptional<FPlayInEditorSessionInfo>();
	Report.bPlaySessionActive = (GEditor && GEditor->IsPlayingSessionInEditor())
		|| ActiveSessionInfo.IsSet()
		|| Report.ObservableWorldCount > 0;

	FHyperAIStudioPIETopologyConfig FrozenConfig;
	FHyperAIStudioPIETopologyConfig CurrentConfig;
	bool bFrozenConfigAvailable = false;
	if (Report.bPlaySessionActive)
	{
		if (ActiveSessionInfo.IsSet()
			&& ActiveSessionInfo->OriginalRequestParams.EditorPlaySettings)
		{
			bFrozenConfigAvailable = ReadPieTopologyConfig(
				ActiveSessionInfo->OriginalRequestParams.EditorPlaySettings,
				FrozenConfig);
			FrozenConfig.bServerWasLaunched = ActiveSessionInfo->bServerWasLaunched;
			FrozenConfig.bExternalSessionDestination =
				ActiveSessionInfo->OriginalRequestParams.SessionDestination
					!= EPlaySessionDestinationType::InProcess;
		}
	}
	else if (!ReadPieTopologyConfig(GetDefault<ULevelEditorPlaySettings>(), CurrentConfig))
	{
		Report.Status = TEXT("settings_unavailable");
		Report.Diagnostic = TEXT("Unreal Level Editor play settings are unavailable.");
		return Report;
	}

	const FHyperAIStudioPIETopologyResolution Topology =
		FHyperAIStudioNativeReadContracts::ResolvePieTopology(
			Report.bPlaySessionActive,
			bFrozenConfigAvailable,
			FrozenConfig,
			CurrentConfig);
	Report.TopologySource = Topology.Source;
	Report.bFrozenTopologyAvailable = Topology.bUsedFrozenActiveSession && Topology.bValid;
	if (!Topology.bValid)
	{
		Report.Status = TEXT("topology_unavailable");
		Report.Diagnostic = Report.bPlaySessionActive
			? TEXT("The active PIE session's frozen OriginalRequestParams topology is unavailable or invalid; the query refuses to substitute mutable current editor settings.")
			: TEXT("Current Unreal Level Editor play settings contain an invalid topology.");
		return Report;
	}
	Report.bRunUnderOneProcess = Topology.Effective.bRunUnderOneProcess;
	Report.bMultiprocessConfiguration = Topology.bMultiprocess;
	Report.bServerWasLaunched = Topology.Effective.bServerWasLaunched;
	Report.ConfiguredPlayNetMode = Topology.Effective.PlayNetMode;
	Report.ConfiguredClientCount = Topology.Effective.ClientCount;
	if (Report.bPlaySessionActive && Report.bMultiprocessConfiguration)
	{
		Report.Status = TEXT("unsupported_multiprocess");
		Report.Diagnostic = TEXT("Active multiprocess PIE is fail-closed because this editor process cannot observe every external PIE world. Use single-process PIE for a complete query.");
		return Report;
	}
	ObservableWorlds.Sort([](const FHyperAIPIEWorldIdentity& A, const FHyperAIPIEWorldIdentity& B)
	{
		return PieWorldCanonical(A) < PieWorldCanonical(B);
	});

	if (!Report.bPlaySessionActive && ObservableWorlds.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("no_active_pie");
		Report.Diagnostic = Report.bMultiprocessConfiguration
			? TEXT("No PIE session is active. The configured multiprocess mode will be unsupported while active.")
			: TEXT("No in-process PIE session is active.");
		return Report;
	}
	if (Report.bPlaySessionActive && ObservableWorlds.IsEmpty())
	{
		Report.Status = TEXT("pie_world_unavailable");
		Report.Diagnostic = TEXT("A play session is active, but no in-process PIE world is observable; the query refuses to infer external-process state.");
		return Report;
	}

	TArray<FHyperAIPIEWorldIdentity> MatchingWorlds;
	for (const FHyperAIPIEWorldIdentity& World : ObservableWorlds)
	{
		if (PieInstance == INDEX_NONE || World.PieInstance == PieInstance)
		{
			MatchingWorlds.Add(World);
		}
	}
	Report.MatchingWorldCount = MatchingWorlds.Num();
	if (MatchingWorlds.IsEmpty())
	{
		if (Report.bSourceTruncated)
		{
			Report.Status = TEXT("source_limit_exceeded");
			Report.Diagnostic = TEXT("The PIE world source bound was reached before this process could prove that pie_instance is absent; narrow or simplify the PIE session.");
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("instance_not_found");
		Report.Diagnostic = TEXT("PIE is active, but no observable in-process world matches pie_instance.");
		return Report;
	}

	Report.Worlds = MoveTemp(MatchingWorlds);
	Report.ReturnedWorldCount = Report.Worlds.Num();
	Report.bOk = true;
	Report.Status = Report.bSourceTruncated ? TEXT("partial") : TEXT("ok");
	Report.Diagnostic = Report.bSourceTruncated
		? TEXT("The in-process PIE world count exceeded the hard source bound; the returned snapshot is explicitly partial.")
		: TEXT("Every matching in-process PIE world is represented explicitly in this single bounded snapshot.");
	return Report;
}

void FHyperAIStudioNativeReadToolRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this,
		&FHyperAIStudioNativeReadToolRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioNativeReadToolRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	bool bRegistryChanged = false;
	const bool bCanUseRegistry = IsInGameThread()
		&& UObjectInitialized()
		&& UToolsetRegistry::IsAvailable();
	if (bCanUseRegistry
		&& bOwnsDependencyGraphRegistration)
	{
		FString Error;
		if (FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioDependencyGraphToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(),
			Error))
		{
			bRegistryChanged = true;
		}
		else
		{
			UE_LOG(LogHyperAIStudioNativeReadTools, Warning,
				TEXT("Could not unregister the owned dependency-graph toolset: %s"), *Error);
		}
	}
	if (bCanUseRegistry
		&& bOwnsNativeReadRegistration)
	{
		FString Error;
		if (FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioNativeReadToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(),
			Error))
		{
			bRegistryChanged = true;
		}
		else
		{
			UE_LOG(LogHyperAIStudioNativeReadTools, Warning,
				TEXT("Could not unregister the owned native-read toolset: %s"), *Error);
		}
	}
	if (bRegistryChanged && !IsEngineExitRequested())
	{
		RefreshMcpToolsIfSafe();
	}
	bOwnsNativeReadRegistration = false;
	bOwnsDependencyGraphRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioNativeReadToolRegistration::IsRegistered() const
{
	if (!UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return false;
	}
	const bool bAllowPending = FHyperAIStudioNativeReadContracts::IsPendingNativeToolsTestEnabled();
	const bool bNativeRequired = FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
		FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(),
		bAllowPending);
	const bool bDependencyRequired = FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
		FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(),
		bAllowPending);
	if (!bNativeRequired && !bDependencyRequired)
	{
		return false;
	}
	return (!bNativeRequired
			|| FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				UHyperAIStudioNativeReadToolset::StaticClass(),
				FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName()))
		&& (!bDependencyRequired
			|| FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				UHyperAIStudioDependencyGraphToolset::StaticClass(),
				FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()));
}

void FHyperAIStudioNativeReadToolRegistration::RegisterAfterEngineInit()
{
	if (!bStarted
		|| !IsInGameThread()
		|| IsEngineExitRequested()
		|| !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable())
	{
		UE_LOG(LogHyperAIStudioNativeReadTools, Warning,
			TEXT("HyperAI native tools could not register safely after engine initialization."));
		return;
	}

	bool bRegistryChanged = false;
	const bool bAllowPending = FHyperAIStudioNativeReadContracts::IsPendingNativeToolsTestEnabled();
	const bool bAllowNativeRead = FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
		FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(),
		bAllowPending);
	const bool bAllowDependencyGraph = FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
		FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(),
		bAllowPending);
	if (bAllowNativeRead
		&& !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioNativeReadToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsNativeReadRegistration =
			FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
				UHyperAIStudioNativeReadToolset::StaticClass(),
				FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(),
				Error);
		bRegistryChanged |= bOwnsNativeReadRegistration;
		if (!bOwnsNativeReadRegistration)
		{
			UE_LOG(LogHyperAIStudioNativeReadTools, Error,
				TEXT("ToolsetRegistry rejected the HyperAI native read toolset: %s"), *Error);
		}
	}
	if (bAllowDependencyGraph
		&& !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioDependencyGraphToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()))
	{
		FString Error;
		bOwnsDependencyGraphRegistration =
			FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
				UHyperAIStudioDependencyGraphToolset::StaticClass(),
				FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(),
				Error);
		bRegistryChanged |= bOwnsDependencyGraphRegistration;
		if (!bOwnsDependencyGraphRegistration)
		{
			UE_LOG(LogHyperAIStudioNativeReadTools, Error,
				TEXT("ToolsetRegistry rejected the HyperAI dependency-graph toolset: %s"), *Error);
		}
	}

	if (bRegistryChanged)
	{
		UE_LOG(LogHyperAIStudioNativeReadTools, Display,
			TEXT("Registered the owned HyperAI toolsets through Epic ToolsetRegistry."));
		RefreshMcpToolsIfSafe();
	}
}

void FHyperAIStudioNativeReadToolRegistration::RefreshMcpToolsIfSafe() const
{
	if (!IsInGameThread() || IsEngineExitRequested())
	{
		return;
	}
	if (IModelContextProtocolModule* ModelContextProtocol =
		FModuleManager::GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol")))
	{
		ModelContextProtocol->RefreshTools();
	}
}
