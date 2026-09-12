// Games by Hyper 2026.

#include "HyperAIStudioNetworkingToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EdGraph/EdGraph.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "K2Node_CustomEvent.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Net/OnlineEngineInterface.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNetworking, Log, All);

namespace HyperAIStudio::Networking::Private
{
	constexpr int32 MinOutputBytes = 4096;
	constexpr double MaxFrequency = 1000000.0;
	constexpr double MaxPriority = 1000000.0;
	constexpr int32 MaxSessionPlayers = 1000000;

	void AppendToken(FString& Out, const FString& Value)
	{
		Out += FString::FromInt(Value.Len());
		Out += TEXT(":");
		Out += Value;
		Out += TEXT("|");
	}

	void AppendInt(FString& Out, const int64 Value)
	{
		AppendToken(Out, FString::Printf(TEXT("%lld"), static_cast<long long>(Value)));
	}

	void AppendBool(FString& Out, const bool bValue)
	{
		AppendToken(Out, bValue ? TEXT("1") : TEXT("0"));
	}

	void AppendDouble(FString& Out, const double Value)
	{
		if (FMath::IsNaN(Value)) AppendToken(Out, TEXT("nan"));
		else if (!FMath::IsFinite(Value))
			AppendToken(Out, Value < 0.0 ? TEXT("-inf") : TEXT("+inf"));
		else AppendToken(Out, Value == 0.0 ? TEXT("0") : FString::Printf(TEXT("%.17g"), Value));
	}

	FString Clip(const FString& Value, const int32 Limit = 2048)
	{
		return Value.Left(FMath::Max(0, Limit));
	}

	bool IsDeadlineExpired(const double DeadlineSeconds)
	{
		return DeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= DeadlineSeconds;
	}

	bool IsNameToken(const FString& Value)
	{
		return !Value.IsEmpty()
			&& Value.Len() <= FHyperAIStudioNetworkingContracts::MaxNameCharacters
			&& !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& FName::IsValidXName(Value, INVALID_OBJECTNAME_CHARACTERS);
	}

	bool IsTriState(const int32 Value)
	{
		return Value == 0 || Value == 1;
	}

	bool IsDefaultNetworkFields(const FHyperAINetworkPlanOperation& Value)
	{
		return Value.MemberName.IsEmpty() && Value.Replicates == -1
			&& Value.AlwaysRelevant == -1 && Value.OnlyRelevantToOwner == -1
			&& Value.UseOwnerRelevancy == -1 && Value.NetUpdateFrequency == -1.0
			&& Value.MinNetUpdateFrequency == -1.0 && Value.NetPriority == -1.0
			&& Value.Dormancy.IsEmpty() && Value.ReplicationCondition.IsEmpty()
			&& Value.RepNotifyFunction.IsEmpty() && Value.RpcMode.IsEmpty()
			&& Value.Reliable == -1 && Value.WithValidation == -1;
	}

	bool IsDefaultFrameworkClassRefs(const FHyperAIGameFrameworkPlanOperation& Value)
	{
		return Value.GameModeClassPath.IsEmpty() && Value.GameStateClassPath.IsEmpty()
			&& Value.PlayerControllerClassPath.IsEmpty() && Value.PlayerStateClassPath.IsEmpty()
			&& Value.PawnClassPath.IsEmpty() && Value.HUDClassPath.IsEmpty()
			&& Value.GameSessionClassPath.IsEmpty() && Value.SpectatorClassPath.IsEmpty()
			&& Value.GameInstanceClassPath.IsEmpty();
	}

	bool IsDefaultFrameworkLimits(const FHyperAIGameFrameworkPlanOperation& Value)
	{
		return Value.MaxPlayers == -1 && Value.MaxSpectators == -1
			&& Value.MaxSplitscreensPerConnection == -1 && Value.RequiresPushToTalk == -1;
	}

	void AddIssue(
		TArray<FHyperAINetworkingIssue>& Issues,
		const int32 Limit,
		bool& bTruncated,
		const TCHAR* Domain,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& TargetPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Limit)
		{
			bTruncated = true;
			return;
		}
		FHyperAINetworkingIssue Issue;
		Issue.Domain = Domain;
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.TargetPath = TargetPath;
		Issue.StableId = StableId;
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message);
		Issues.Add(MoveTemp(Issue));
	}

	void AddCaptureIssue(
		TArray<FHyperAINetworkingIssue>& Issues,
		const TCHAR* Domain,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& TargetPath,
		const FString& Message)
	{
		bool bIgnored = false;
		AddIssue(Issues, FHyperAIStudioNetworkingContracts::MaxIssues, bIgnored,
			Domain, Code, Severity, TargetPath, FString(), -1, Message);
	}

	FString BlueprintStatus(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Error: return TEXT("error");
		case BS_Dirty: return TEXT("dirty");
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	FString NetRole(const ENetRole Role)
	{
		switch (Role)
		{
		case ROLE_None: return TEXT("none");
		case ROLE_SimulatedProxy: return TEXT("simulated_proxy");
		case ROLE_AutonomousProxy: return TEXT("autonomous_proxy");
		case ROLE_Authority: return TEXT("authority");
		default: return TEXT("unrecognized");
		}
	}

	FString NetMode(const ENetMode Mode)
	{
		switch (Mode)
		{
		case NM_Standalone: return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated_server");
		case NM_ListenServer: return TEXT("listen_server");
		case NM_Client: return TEXT("client");
		default: return TEXT("unrecognized");
		}
	}

	FString Dormancy(const ENetDormancy Value)
	{
		switch (Value)
		{
		case DORM_Never: return TEXT("never");
		case DORM_Awake: return TEXT("awake");
		case DORM_DormantAll: return TEXT("dormant_all");
		case DORM_DormantPartial: return TEXT("dormant_partial");
		case DORM_Initial: return TEXT("initial");
		default: return TEXT("unrecognized");
		}
	}

	FString LifetimeCondition(const ELifetimeCondition Value)
	{
		const UEnum* Enum = StaticEnum<ELifetimeCondition>();
		FString EnumToken = Enum
			? Enum->GetNameStringByValue(static_cast<int64>(Value))
			: FString();
		if (!EnumToken.RemoveFromStart(TEXT("COND_"), ESearchCase::CaseSensitive)
			|| EnumToken.IsEmpty() || EnumToken == TEXT("Max"))
		{
			return TEXT("unrecognized");
		}

		FString Result;
		Result.Reserve(EnumToken.Len() + 4);
		for (int32 Index = 0; Index < EnumToken.Len(); ++Index)
		{
			const TCHAR Character = EnumToken[Index];
			if (Index > 0 && FChar::IsUpper(Character)
				&& (FChar::IsLower(EnumToken[Index - 1])
					|| FChar::IsDigit(EnumToken[Index - 1])))
			{
				Result.AppendChar(TEXT('_'));
			}
			Result.AppendChar(FChar::ToLower(Character));
		}
		return Result;
	}

	bool IsLifetimeConditionToken(const FString& Value)
	{
		static const TSet<FString> Values = {
			TEXT("none"), TEXT("initial_only"), TEXT("owner_only"), TEXT("skip_owner"),
			TEXT("simulated_only"), TEXT("autonomous_only"), TEXT("simulated_or_physics"),
			TEXT("initial_or_owner"), TEXT("custom"), TEXT("replay_or_owner"),
			TEXT("replay_only"), TEXT("simulated_only_no_replay"),
			TEXT("simulated_or_physics_no_replay"), TEXT("skip_replay"),
			TEXT("dynamic"), TEXT("never"), TEXT("net_group")};
		return Values.Contains(Value);
	}

	bool IsDormancyToken(const FString& Value)
	{
		return Value == TEXT("never") || Value == TEXT("awake")
			|| Value == TEXT("dormant_all") || Value == TEXT("dormant_partial")
			|| Value == TEXT("initial");
	}

	FString RpcMode(const UFunction* Function)
	{
		if (!Function || !Function->HasAnyFunctionFlags(FUNC_Net)) return FString();
		const int32 DirectionCount =
			(Function->HasAnyFunctionFlags(FUNC_NetServer) ? 1 : 0)
			+ (Function->HasAnyFunctionFlags(FUNC_NetClient) ? 1 : 0)
			+ (Function->HasAnyFunctionFlags(FUNC_NetMulticast) ? 1 : 0);
		if (DirectionCount > 1) return TEXT("conflicting");
		if (Function->HasAnyFunctionFlags(FUNC_NetServer)) return TEXT("server");
		if (Function->HasAnyFunctionFlags(FUNC_NetClient)) return TEXT("client");
		if (Function->HasAnyFunctionFlags(FUNC_NetMulticast)) return TEXT("multicast");
		return TEXT("unspecified");
	}

	FString ObjectPath(const UObject* Object)
	{
		return IsValid(Object) ? Object->GetPathName() : FString();
	}

	FString ClassPath(const UClass* Class)
	{
		return IsValid(Class) ? Class->GetPathName() : FString();
	}

	FString FrameworkRole(const UClass* Class)
	{
		if (!Class) return FString();
		if (Class->IsChildOf(AGameModeBase::StaticClass())) return TEXT("game_mode");
		if (Class->IsChildOf(AGameStateBase::StaticClass())) return TEXT("game_state");
		if (Class->IsChildOf(APlayerController::StaticClass())) return TEXT("player_controller");
		if (Class->IsChildOf(APlayerState::StaticClass())) return TEXT("player_state");
		if (Class->IsChildOf(ASpectatorPawn::StaticClass())) return TEXT("spectator_pawn");
		if (Class->IsChildOf(APawn::StaticClass())) return TEXT("pawn");
		if (Class->IsChildOf(AHUD::StaticClass())) return TEXT("hud");
		if (Class->IsChildOf(AGameSession::StaticClass())) return TEXT("game_session");
		if (Class->IsChildOf(UGameInstance::StaticClass())) return TEXT("game_instance");
		return FString();
	}

	bool IsFrameworkRoleToken(const FString& Role)
	{
		return Role == TEXT("game_mode") || Role == TEXT("game_state")
			|| Role == TEXT("player_controller") || Role == TEXT("player_state")
			|| Role == TEXT("pawn") || Role == TEXT("spectator_pawn")
			|| Role == TEXT("hud") || Role == TEXT("game_session")
			|| Role == TEXT("game_instance");
	}

	UClass* ExpectedBaseClass(const FString& Role)
	{
		if (Role == TEXT("game_mode")) return AGameModeBase::StaticClass();
		if (Role == TEXT("game_state")) return AGameStateBase::StaticClass();
		if (Role == TEXT("player_controller")) return APlayerController::StaticClass();
		if (Role == TEXT("player_state")) return APlayerState::StaticClass();
		if (Role == TEXT("pawn")) return APawn::StaticClass();
		if (Role == TEXT("spectator_pawn")) return ASpectatorPawn::StaticClass();
		if (Role == TEXT("hud")) return AHUD::StaticClass();
		if (Role == TEXT("game_session")) return AGameSession::StaticClass();
		if (Role == TEXT("game_instance")) return UGameInstance::StaticClass();
		return nullptr;
	}

	UClass* FindLoadedClassNoLoad(const FString& Path);
	bool IsLoadedRoleReference(const FString& Path, const FString& ExpectedRole);

	FString AssetPathForClass(const UClass* Class)
	{
		if (const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class))
		{
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Generated->ClassGeneratedBy))
				return Blueprint->GetPathName();
		}
		return FString();
	}

	FString NormalizeExportObjectPath(const FString& Value)
	{
		if (Value.IsEmpty()
			|| Value.Len() > FHyperAIStudioNetworkingContracts::MaxPathCharacters)
		{
			return FString();
		}
		return Value.Contains(TEXT("'")) ? FPackageName::ExportTextPathToObjectPath(Value) : Value;
	}

	bool TryGetProjectAssetPackageName(
		const FString& AssetPath,
		const bool bRequirePrimaryAssetName,
		FName& OutPackageName)
	{
		OutPackageName = NAME_None;
		if (!FHyperAIStudioNetworkingContracts::IsCanonicalProjectAssetPath(AssetPath))
		{
			return false;
		}
		FString PackageName;
		FString ObjectName;
		if (!AssetPath.Split(TEXT("."), &PackageName, &ObjectName,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| !FPackageName::IsValidLongPackageName(PackageName)
			|| (bRequirePrimaryAssetName
				&& ObjectName != FPackageName::GetShortName(PackageName)))
		{
			return false;
		}
		OutPackageName = FName(*PackageName);
		return !OutPackageName.IsNone();
	}

	bool IsInspectableBlueprintClass(const UBlueprint* Blueprint, const UClass* Class)
	{
		return FHyperAIStudioNetworkingContracts::IsCurrentGeneratedClass(Blueprint, Class)
			&& !Class->HasAnyFlags(RF_Transient)
			&& !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
			&& !Class->GetName().StartsWith(TEXT("REINST_"), ESearchCase::CaseSensitive)
			&& !Class->GetName().StartsWith(TEXT("SKEL_"), ESearchCase::CaseSensitive);
	}

	bool IsInspectableClass(const UClass* Class)
	{
		if (!Class || Class->HasAnyFlags(RF_Transient)
			|| Class->HasAnyClassFlags(CLASS_NewerVersionExists)
			|| Class->GetName().StartsWith(TEXT("REINST_"), ESearchCase::CaseSensitive)
			|| Class->GetName().StartsWith(TEXT("SKEL_"), ESearchCase::CaseSensitive)) return false;
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
			return IsInspectableBlueprintClass(Blueprint, Class);
		return Class->ClassGeneratedBy == nullptr;
	}

	bool IsStructurallyCurrentClass(const UClass* Class)
	{
		if (!IsInspectableClass(Class)) return false;
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
			return FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
				BlueprintStatus(Blueprint->Status));
		return true;
	}

	bool IsRpcCompatibleSignature(const UFunction* Function)
	{
		if (!Function || Function->GetReturnProperty()
			|| Function->HasAnyFunctionFlags(FUNC_Native | FUNC_Static | FUNC_Delegate)
			|| !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)) return false;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Parameter = *It;
			if (!Parameter->HasAnyPropertyFlags(CPF_Parm)) continue;
			if (Parameter->HasAnyPropertyFlags(CPF_ReturnParm)
				|| (Parameter->HasAnyPropertyFlags(CPF_OutParm)
					&& !Parameter->HasAnyPropertyFlags(CPF_ConstParm))) return false;
		}
		return true;
	}

	bool IsRepNotifyCompatibleSignature(const UFunction* Function)
	{
		return Function && Function->GetReturnProperty() == nullptr
			&& Function->NumParms == 0
			&& !Function->HasAnyFunctionFlags(FUNC_Native | FUNC_Static | FUNC_Delegate);
	}

	struct FBlueprintDeclarationSet
	{
		TSet<FName> Variables;
		TMap<FName, uint64> VariableFlags;
		TMap<FName, ELifetimeCondition> VariableConditions;
		TMap<FName, FName> VariableRepNotifies;
		TSet<FName> FunctionGraphs;
		TSet<FName> CustomEvents;
	};

	FBlueprintDeclarationSet CollectBlueprintDeclarations(
		const UBlueprint* Blueprint,
		FHyperAIStudioNetworkValueSnapshot& Snapshot,
		bool& bOutComplete)
	{
		FBlueprintDeclarationSet Result;
		bOutComplete = true;
		if (!Blueprint) return Result;
		int32 Traversed = 0;
		auto CanTraverse = [&]()
		{
			if (Traversed >= FHyperAIStudioNetworkingContracts::MaxElementsPerRecord
				|| Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
			{
				bOutComplete = false;
				Snapshot.bComplete = false;
				return false;
			}
			++Traversed;
			++Snapshot.ObjectsScanned;
			return true;
		};
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			if (!CanTraverse()) return Result;
			if (Variable.VarName.IsNone()) continue;
			Result.Variables.Add(Variable.VarName);
			Result.VariableFlags.Add(Variable.VarName, Variable.PropertyFlags);
			Result.VariableConditions.Add(Variable.VarName, Variable.ReplicationCondition);
			Result.VariableRepNotifies.Add(Variable.VarName, Variable.RepNotifyFunc);
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!CanTraverse()) return Result;
			if (Graph && Graph->GetOuter() == Blueprint) Result.FunctionGraphs.Add(Graph->GetFName());
		}
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!CanTraverse()) return Result;
			if (!Graph || Graph->GetOuter() != Blueprint) continue;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!CanTraverse()) return Result;
				const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
				if (Event && !Event->IsOverride() && !Event->CustomFunctionName.IsNone())
					Result.CustomEvents.Add(Event->CustomFunctionName);
			}
		}
		return Result;
	}

	struct FCachedLineageProof
	{
		bool bComplete = false;
		bool bActor = false;
		FString FrameworkRole;
		FString Diagnostic;
	};

	FCachedLineageProof ProveCachedBlueprintLineage(
		const FAssetData& InitialData,
		int32& ObjectsScanned,
		const int32 RecordCount,
		const double DeadlineSeconds)
	{
		FCachedLineageProof Result;
		FString GeneratedClassPath;
		if (InitialData.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassPath))
		{
			GeneratedClassPath = NormalizeExportObjectPath(GeneratedClassPath);
			if (const UClass* LoadedGenerated = FindObject<UClass>(nullptr, *GeneratedClassPath))
			{
				if (!IsStructurallyCurrentClass(LoadedGenerated))
				{
					Result.Diagnostic = TEXT("Loaded GeneratedClass is dirty/error/being-created, transient, skeleton, REINST, superseded, or not current.");
					return Result;
				}
				Result.bComplete = true;
				Result.bActor = LoadedGenerated->IsChildOf(AActor::StaticClass());
				Result.FrameworkRole = FrameworkRole(LoadedGenerated);
				return Result;
			}
		}
		if (!FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(
			ObjectsScanned, RecordCount, FPlatformTime::Seconds(), DeadlineSeconds))
		{
			Result.Diagnostic = TEXT("Cached Blueprint lineage exceeded the bounded object/deadline budget.");
			return Result;
		}
		++ObjectsScanned;
		FString NativeParent;
		FString Parent;
		InitialData.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParent);
		InitialData.GetTagValue(FBlueprintTags::ParentClassPath, Parent);
		NativeParent = NormalizeExportObjectPath(NativeParent);
		Parent = NormalizeExportObjectPath(Parent);
		const FString Candidate = !NativeParent.IsEmpty() ? NativeParent : Parent;
		if (Candidate.IsEmpty())
		{
			Result.Diagnostic = TEXT("Cached Blueprint lineage has no bounded authoritative parent-class tag.");
			return Result;
		}
		if (const UClass* LoadedParent = FindObject<UClass>(nullptr, *Candidate))
		{
			if (!IsStructurallyCurrentClass(LoadedParent))
			{
				Result.Diagnostic = TEXT("Cached lineage resolved only to a dirty/error/being-created, transient, skeleton, REINST, or superseded class.");
				return Result;
			}
			Result.bComplete = true;
			Result.bActor = LoadedParent->IsChildOf(AActor::StaticClass());
			Result.FrameworkRole = FrameworkRole(LoadedParent);
			return Result;
		}
		Result.Diagnostic = TEXT("Parent class is not already loaded. Exact cached lineage needs a bounded generation-bound asynchronous asset inventory; synchronous parent lookup is prohibited.");
		return Result;
	}

	bool AddNetworkElement(
		FHyperAINetworkRecord& Record,
		FHyperAIStudioNetworkValueSnapshot& Snapshot,
		FHyperAINetworkElementView&& Element)
	{
		if (IsDeadlineExpired(Snapshot.WorkDeadlineSeconds)
			|| Record.Elements.Num() >= FHyperAIStudioNetworkingContracts::MaxElementsPerRecord)
		{
			Record.bRevisionComplete = false;
			Record.bDetailsComplete = false;
			Snapshot.bComplete = false;
			return false;
		}
		Record.Elements.Add(MoveTemp(Element));
		return true;
	}

	void CaptureNetworkClass(
		UClass* Class,
		UBlueprint* Blueprint,
		const bool bIncludeDetails,
		FHyperAIStudioNetworkValueSnapshot& Snapshot)
	{
		if (!Class || Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAINetworkRecord Record;
		Record.Kind = TEXT("class_default");
		Record.AssetPath = Blueprint ? Blueprint->GetPathName() : AssetPathForClass(Class);
		Record.ClassPath = Class->GetPathName();
		Record.TargetPath = Record.AssetPath.IsEmpty() ? Record.ClassPath : Record.AssetPath;
		Record.ParentClassPath = ClassPath(Class->GetSuperClass());
		Record.StableId = TEXT("network.class:") + Record.TargetPath;
		Record.bLoaded = true;
		Record.bOnDiskMetadata = false;
		Record.bRevisionComplete = true;
		Record.bPackageDirty = Blueprint && Blueprint->GetOutermost()->IsDirty();
		Record.BlueprintCompileStatus = Blueprint ? BlueprintStatus(Blueprint->Status) : TEXT("native");
		Record.bBlueprintCompileError = Blueprint && Blueprint->Status == BS_Error;
		Record.bGeneratedClassCurrent = Blueprint
			? IsInspectableBlueprintClass(Blueprint, Class) : IsInspectableClass(Class);
		const bool bBlueprintStateEditable = !Blueprint
			|| (Record.bGeneratedClassCurrent
				&& FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
					Record.BlueprintCompileStatus));
		if (!bBlueprintStateEditable)
		{
			Record.bRevisionComplete = false;
			Record.bDetailsComplete = false;
			Snapshot.bComplete = false;
		}

		AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject(false));
		Record.bDetailsComplete = bBlueprintStateEditable && ActorCDO != nullptr;
		if (ActorCDO)
		{
			Record.bReplicates = ActorCDO->GetIsReplicated();
			Record.bAlwaysRelevant = ActorCDO->bAlwaysRelevant;
			Record.bOnlyRelevantToOwner = ActorCDO->bOnlyRelevantToOwner;
			Record.bUseOwnerRelevancy = ActorCDO->bNetUseOwnerRelevancy;
			Record.Dormancy = Dormancy(ActorCDO->NetDormancy);
			Record.NetUpdateFrequency = ActorCDO->GetNetUpdateFrequency();
			Record.MinNetUpdateFrequency = ActorCDO->GetMinNetUpdateFrequency();
			Record.NetPriority = ActorCDO->NetPriority;
		}
		else
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}

		if (bIncludeDetails && bBlueprintStateEditable && Blueprint)
		{
			bool bDeclarationsComplete = true;
			const FBlueprintDeclarationSet Declarations = CollectBlueprintDeclarations(
				Blueprint, Snapshot, bDeclarationsComplete);
			if (!bDeclarationsComplete)
			{
				Record.bRevisionComplete = false;
				Record.bDetailsComplete = false;
			}
			TSet<FName> RepNotifyNames;
			for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
				{
					Record.bRevisionComplete = false;
					Record.bDetailsComplete = false;
					Snapshot.bComplete = false;
					break;
				}
				++Snapshot.ObjectsScanned;
				const FProperty* Property = *It;
				if (!Property || Property->GetOwnerClass() != Class
					|| !Declarations.Variables.Contains(Property->GetFName())) continue;
				const uint64 DeclaredFlags = Declarations.VariableFlags.FindRef(Property->GetFName());
				const bool bPropertyReplicated = (DeclaredFlags & CPF_Net) != 0;
				const FName DeclaredRepNotify =
					Declarations.VariableRepNotifies.FindRef(Property->GetFName());
				FHyperAINetworkElementView Element;
				Element.Kind = TEXT("property");
				Element.Name = Property->GetName();
				Element.TypePath = Property->GetClass()->GetName();
				Element.Condition = bPropertyReplicated
					? LifetimeCondition(Declarations.VariableConditions.FindRef(Property->GetFName()))
					: FString();
				Element.RepNotifyFunction = DeclaredRepNotify.ToString();
				Element.DeclarationSource = TEXT("new_variable");
				Element.OwnerClassPath = Record.ClassPath;
				Element.bAuthoritativeDeclaration = true;
				Element.bSignatureValid = true;
				if (!DeclaredRepNotify.IsNone()) RepNotifyNames.Add(DeclaredRepNotify);
				Element.bReplicated = bPropertyReplicated;
				Element.StableId = TEXT("property:") + Record.TargetPath + TEXT(":") + Element.Name;
				if (!AddNetworkElement(Record, Snapshot, MoveTemp(Element))) break;
				if (bPropertyReplicated) ++Record.ReplicatedPropertyCount;
			}
			for (const FName RepNotifyName : RepNotifyNames)
			{
				const UFunction* Function = Class->FindFunctionByName(RepNotifyName);
				FHyperAINetworkElementView Element;
				Element.Kind = TEXT("rep_notify_function");
				Element.Name = RepNotifyName.ToString();
				Element.TypePath = Function ? Function->GetClass()->GetPathName() : FString();
				Element.DeclarationSource = TEXT("function_graph");
				Element.OwnerClassPath = Function ? ClassPath(Function->GetOwnerClass()) : FString();
				Element.bAuthoritativeDeclaration = Function
					&& Function->GetOwnerClass() == Class
					&& Declarations.FunctionGraphs.Contains(RepNotifyName);
				Element.bSignatureValid = Element.bAuthoritativeDeclaration
					&& IsRepNotifyCompatibleSignature(Function);
				Element.bReplicated = Function != nullptr;
				Element.StableId = TEXT("rep_notify:") + Record.TargetPath + TEXT(":") + Element.Name;
				if (!AddNetworkElement(Record, Snapshot, MoveTemp(Element))) break;
			}

			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
				{
					Record.bRevisionComplete = false;
					Record.bDetailsComplete = false;
					Snapshot.bComplete = false;
					break;
				}
				++Snapshot.ObjectsScanned;
				const UFunction* Function = *It;
				if (!Function || Function->GetOwnerClass() != Class) continue;
				const bool bFunctionGraph = Declarations.FunctionGraphs.Contains(Function->GetFName());
				const bool bCustomEvent = Declarations.CustomEvents.Contains(Function->GetFName());
				if (!bFunctionGraph && !bCustomEvent) continue;
				const bool bFunctionReplicated = Function->HasAnyFunctionFlags(FUNC_Net);
				FHyperAINetworkElementView Element;
				Element.Kind = bFunctionReplicated ? TEXT("rpc") : TEXT("function");
				Element.Name = Function->GetName();
				Element.TypePath = Function->GetClass()->GetPathName();
				Element.RpcMode = RpcMode(Function);
				Element.DeclarationSource = bCustomEvent
					? TEXT("custom_event") : TEXT("function_graph");
				Element.OwnerClassPath = Record.ClassPath;
				Element.bAuthoritativeDeclaration = true;
				Element.bSignatureValid = bCustomEvent
					? IsRpcCompatibleSignature(Function)
					: IsRepNotifyCompatibleSignature(Function);
				Element.bReplicated = bFunctionReplicated;
				Element.bReliable = Function->HasAnyFunctionFlags(FUNC_NetReliable);
				Element.bWithValidation = Function->HasAnyFunctionFlags(FUNC_NetValidate);
				Element.StableId = (bFunctionReplicated ? TEXT("rpc:") : TEXT("function:"))
					+ Record.TargetPath + TEXT(":") + Element.Name;
				if (!AddNetworkElement(Record, Snapshot, MoveTemp(Element))) break;
				if (bFunctionReplicated) ++Record.RpcCount;
			}

			if (Blueprint->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& Nodes =
					Blueprint->SimpleConstructionScript->GetAllNodes();
				for (USCS_Node* Node : Nodes)
				{
					if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
						|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
					{
						Record.bRevisionComplete = false;
						Record.bDetailsComplete = false;
						Snapshot.bComplete = false;
						break;
					}
					++Snapshot.ObjectsScanned;
					if (!Node || Node->GetOuter() != Blueprint->SimpleConstructionScript
						|| !Node->ComponentTemplate) continue;
					UActorComponent* Component = Node->ComponentTemplate;
					FHyperAINetworkElementView Element;
					Element.Kind = TEXT("component");
					Element.Name = Node->GetVariableName().ToString();
					Element.TypePath = Component->GetClass()->GetPathName();
					Element.DeclarationSource = TEXT("scs");
					Element.OwnerClassPath = Record.ClassPath;
					Element.bAuthoritativeDeclaration = true;
					Element.bSignatureValid = true;
					Element.bReplicated = Component->GetIsReplicated();
					Element.Condition = LifetimeCondition(Component->GetReplicationCondition());
					Element.StableId = TEXT("component:") + Record.TargetPath + TEXT(":") + Element.Name;
					if (!AddNetworkElement(Record, Snapshot, MoveTemp(Element))) break;
					if (Component->GetIsReplicated()) ++Record.ReplicatedComponentCount;
				}
			}
		}
		Snapshot.Records.Add(MoveTemp(Record));
	}

	bool CaptureNetworkAssetMetadata(
		const FAssetData& Data,
		FHyperAIStudioNetworkValueSnapshot& Snapshot,
		TSet<FString>& SeenTargets)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords
			|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
		{
			Snapshot.bComplete = false;
			return false;
		}
		FString GeneratedClass;
		if (!Data.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClass)
			|| GeneratedClass.IsEmpty()) return false;
		const FString AssetPath = Data.GetObjectPathString();
		if (Data.IsAssetLoaded())
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("network"),
				TEXT("loaded_asset_identity_unresolved"), TEXT("error"), AssetPath,
				TEXT("Asset Registry reports the Blueprint loaded, but its exact UObject/GeneratedClass identity was not resolved by the loaded-only path."));
			return false;
		}
		FString GeneratedAssetPath;
		if (!FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			GeneratedClass, GeneratedAssetPath) || GeneratedAssetPath != AssetPath)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("network"),
				TEXT("generated_class_asset_binding_unproven"), TEXT("error"), AssetPath,
				TEXT("Cached GeneratedClassPath does not resolve exactly back to this Blueprint asset."));
			return false;
		}
		if (SeenTargets.Contains(AssetPath)) return true;
		FString NativeParent;
		FString Parent;
		Data.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParent);
		Data.GetTagValue(FBlueprintTags::ParentClassPath, Parent);
		NativeParent = NormalizeExportObjectPath(NativeParent);
		Parent = NormalizeExportObjectPath(Parent);
		const FCachedLineageProof Proof = ProveCachedBlueprintLineage(
			Data, Snapshot.ObjectsScanned, Snapshot.Records.Num(),
			Snapshot.WorkDeadlineSeconds);
		if (!Proof.bComplete)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("network"),
				TEXT("cached_lineage_unproven"), TEXT("error"), AssetPath,
				Proof.Diagnostic + TEXT(" Supply a later bounded generation-bound asynchronous inventory observation; no synchronous retry path is claimed."));
			return false;
		}
		if (!Proof.bActor) return false;

		FHyperAINetworkRecord Record;
		Record.Kind = TEXT("blueprint_asset_metadata");
		Record.StableId = TEXT("network.asset:") + AssetPath;
		Record.TargetPath = AssetPath;
		Record.AssetPath = AssetPath;
		Record.ClassPath = NormalizeExportObjectPath(GeneratedClass);
		Record.ParentClassPath = Parent.IsEmpty() ? NativeParent : Parent;
		Record.bLoaded = Data.IsAssetLoaded();
		Record.bOnDiskMetadata = true;
		Record.bDetailsComplete = false;
		Record.bRevisionComplete = false;
		Record.BlueprintCompileStatus = TEXT("cached_not_loaded");
		Record.bGeneratedClassCurrent = false;
		Data.GetTagValue(FBlueprintTags::NumReplicatedProperties, Record.ReplicatedPropertyCount);
		Record.RpcCount = -1;
		Record.ReplicatedComponentCount = -1;
		Snapshot.bComplete = false;
		AddCaptureIssue(Snapshot.CaptureIssues, TEXT("network"),
			TEXT("cached_revision_requires_async_inventory"), TEXT("warning"), AssetPath,
			TEXT("Cached class tags can prove only loaded structural lineage; exact class/declaration revision requires a bounded generation-bound asynchronous inventory."));
		SeenTargets.Add(AssetPath);
		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	void CaptureWorldNetworkState(
		UWorld* World,
		const bool bIncludeActors,
		FHyperAIStudioNetworkValueSnapshot& Snapshot)
	{
		if (!World) return;
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAINetworkRecord WorldRecord;
		WorldRecord.Kind = TEXT("world");
		WorldRecord.WorldPath = World->GetPathName();
		WorldRecord.TargetPath = WorldRecord.WorldPath;
		WorldRecord.StableId = TEXT("network.world:") + WorldRecord.WorldPath;
		WorldRecord.ClassPath = World->GetClass()->GetPathName();
		WorldRecord.NetMode = NetMode(World->GetNetMode());
		WorldRecord.bLoaded = true;
		WorldRecord.bDetailsComplete = true;
		WorldRecord.bRevisionComplete = true;
		Snapshot.Records.Add(MoveTemp(WorldRecord));

		if (UNetDriver* Driver = World->GetNetDriver())
		{
			if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
			{
				Snapshot.bComplete = false;
				return;
			}
			FHyperAINetworkRecord DriverRecord;
			DriverRecord.Kind = TEXT("net_driver");
			DriverRecord.TargetPath = Driver->GetPathName();
			DriverRecord.StableId = TEXT("network.driver:") + World->GetPathName()
				+ TEXT(":") + Driver->NetDriverName.ToString();
			DriverRecord.WorldPath = World->GetPathName();
			DriverRecord.ClassPath = Driver->GetClass()->GetPathName();
			DriverRecord.NetMode = NetMode(Driver->GetNetMode());
			DriverRecord.NetDriverName = Driver->NetDriverName.ToString();
			DriverRecord.ClientConnectionCount = Driver->ClientConnections.Num();
			DriverRecord.bHasServerConnection = Driver->ServerConnection != nullptr;
			DriverRecord.bLoaded = true;
			DriverRecord.bDetailsComplete = true;
			DriverRecord.bRevisionComplete = true;
			Snapshot.Records.Add(MoveTemp(DriverRecord));
		}

		if (!bIncludeActors) return;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords
				|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
			{
				Snapshot.bComplete = false;
				break;
			}
			++Snapshot.ObjectsScanned;
			AActor* Actor = *It;
			if (!Actor || !Actor->GetIsReplicated()) continue;
			FHyperAINetworkRecord Record;
			Record.Kind = TEXT("actor_instance");
			Record.TargetPath = Actor->GetPathName();
			Record.StableId = TEXT("network.actor:") + Record.TargetPath;
			Record.ClassPath = Actor->GetClass()->GetPathName();
			Record.AssetPath = AssetPathForClass(Actor->GetClass());
			Record.WorldPath = World->GetPathName();
			Record.bLoaded = true;
			Record.bDetailsComplete = true;
			Record.bRevisionComplete = true;
			Record.bReplicates = Actor->GetIsReplicated();
			Record.bAlwaysRelevant = Actor->bAlwaysRelevant;
			Record.bOnlyRelevantToOwner = Actor->bOnlyRelevantToOwner;
			Record.bUseOwnerRelevancy = Actor->bNetUseOwnerRelevancy;
			Record.Dormancy = Dormancy(Actor->NetDormancy);
			Record.NetUpdateFrequency = Actor->GetNetUpdateFrequency();
			Record.MinNetUpdateFrequency = Actor->GetMinNetUpdateFrequency();
			Record.NetPriority = Actor->NetPriority;
			Record.bHasAuthority = Actor->HasAuthority();
			Record.LocalRole = NetRole(Actor->GetLocalRole());
			Record.RemoteRole = NetRole(Actor->GetRemoteRole());
			Record.OwnerPath = ObjectPath(Actor->GetOwner());
			Record.NetMode = NetMode(World->GetNetMode());
			Snapshot.Records.Add(MoveTemp(Record));
		}
	}

	void CaptureFrameworkClass(
		UClass* Class,
		UBlueprint* Blueprint,
		FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
	{
		const FString Role = FrameworkRole(Class);
		if (!Class || Role.IsEmpty()) return;
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAIGameFrameworkRecord Record;
		Record.Kind = TEXT("class_default");
		Record.Role = Role;
		Record.AssetPath = Blueprint ? Blueprint->GetPathName() : AssetPathForClass(Class);
		Record.ClassPath = Class->GetPathName();
		Record.ParentClassPath = ClassPath(Class->GetSuperClass());
		Record.TargetPath = Record.AssetPath.IsEmpty() ? Record.ClassPath : Record.AssetPath;
		Record.StableId = TEXT("framework.class:") + Role + TEXT(":") + Record.TargetPath;
		Record.bLoaded = true;
		Record.bOnDiskMetadata = false;
		Record.bPackageDirty = Blueprint && Blueprint->GetOutermost()->IsDirty();
		Record.BlueprintCompileStatus = Blueprint ? BlueprintStatus(Blueprint->Status) : TEXT("native");
		Record.bBlueprintCompileError = Blueprint && Blueprint->Status == BS_Error;
		Record.bGeneratedClassCurrent = Blueprint
			? IsInspectableBlueprintClass(Blueprint, Class) : IsInspectableClass(Class);
		Record.bRevisionComplete = true;
		if ((Blueprint && !FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
				Record.BlueprintCompileStatus)) || !Record.bGeneratedClassCurrent)
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		UObject* CDO = Class->GetDefaultObject(false);
		Record.bDetailsComplete = CDO != nullptr && Record.bRevisionComplete;
		if (!CDO)
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		if (const AGameModeBase* GameMode = Cast<AGameModeBase>(CDO))
		{
			Record.GameStateClassPath = ClassPath(GameMode->GameStateClass.Get());
			Record.PlayerControllerClassPath = ClassPath(GameMode->PlayerControllerClass.Get());
			Record.PlayerStateClassPath = ClassPath(GameMode->PlayerStateClass.Get());
			Record.PawnClassPath = ClassPath(GameMode->DefaultPawnClass.Get());
			Record.HUDClassPath = ClassPath(GameMode->HUDClass.Get());
			Record.GameSessionClassPath = ClassPath(GameMode->GameSessionClass.Get());
			Record.SpectatorClassPath = ClassPath(GameMode->SpectatorClass.Get());
		}
		if (const AGameSession* Session = Cast<AGameSession>(CDO))
		{
			Record.MaxPlayers = Session->MaxPlayers;
			Record.MaxSpectators = Session->MaxSpectators;
			Record.MaxSplitscreensPerConnection = Session->MaxSplitscreensPerConnection;
			Record.bRequiresPushToTalk = Session->bRequiresPushToTalk;
			Record.SessionName = Session->SessionName.ToString();
		}
		Snapshot.Records.Add(MoveTemp(Record));
	}

	bool CaptureFrameworkAssetMetadata(
		const FAssetData& Data,
		FHyperAIStudioGameFrameworkValueSnapshot& Snapshot,
		TSet<FString>& SeenTargets)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords
			|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
		{
			Snapshot.bComplete = false;
			return false;
		}
		FString GeneratedClass;
		if (!Data.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClass)
			|| GeneratedClass.IsEmpty()) return false;
		const FString AssetPath = Data.GetObjectPathString();
		if (Data.IsAssetLoaded())
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("game_framework"),
				TEXT("loaded_asset_identity_unresolved"), TEXT("error"), AssetPath,
				TEXT("Asset Registry reports the Framework Blueprint loaded, but its exact UObject/GeneratedClass identity was not resolved by the loaded-only path."));
			return false;
		}
		FString GeneratedAssetPath;
		if (!FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			GeneratedClass, GeneratedAssetPath) || GeneratedAssetPath != AssetPath)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("game_framework"),
				TEXT("generated_class_asset_binding_unproven"), TEXT("error"), AssetPath,
				TEXT("Cached GeneratedClassPath does not resolve exactly back to this Blueprint asset."));
			return false;
		}
		if (SeenTargets.Contains(AssetPath)) return true;
		FString NativeParent;
		FString Parent;
		Data.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParent);
		Data.GetTagValue(FBlueprintTags::ParentClassPath, Parent);
		NativeParent = NormalizeExportObjectPath(NativeParent);
		Parent = NormalizeExportObjectPath(Parent);
		const FCachedLineageProof Proof = ProveCachedBlueprintLineage(
			Data, Snapshot.ObjectsScanned, Snapshot.Records.Num(),
			Snapshot.WorkDeadlineSeconds);
		if (!Proof.bComplete)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot.CaptureIssues, TEXT("game_framework"),
				TEXT("cached_lineage_unproven"), TEXT("error"), AssetPath,
				Proof.Diagnostic + TEXT(" Supply a later bounded generation-bound asynchronous inventory observation; no synchronous retry path is claimed."));
			return false;
		}
		const FString Role = Proof.FrameworkRole;
		if (Role.IsEmpty()) return false;
		FHyperAIGameFrameworkRecord Record;
		Record.Kind = TEXT("blueprint_asset_metadata");
		Record.Role = Role;
		Record.AssetPath = AssetPath;
		Record.TargetPath = AssetPath;
		Record.StableId = TEXT("framework.asset:") + Role + TEXT(":") + AssetPath;
		Record.ClassPath = NormalizeExportObjectPath(GeneratedClass);
		Record.ParentClassPath = Parent.IsEmpty() ? NativeParent : Parent;
		Record.bLoaded = Data.IsAssetLoaded();
		Record.bOnDiskMetadata = true;
		Record.bDetailsComplete = false;
		Record.bRevisionComplete = false;
		Record.BlueprintCompileStatus = TEXT("cached_not_loaded");
		Record.bGeneratedClassCurrent = false;
		Snapshot.bComplete = false;
		AddCaptureIssue(Snapshot.CaptureIssues, TEXT("game_framework"),
			TEXT("cached_revision_requires_async_inventory"), TEXT("warning"), AssetPath,
			TEXT("Cached class tags can prove only loaded structural lineage; exact Framework defaults revision requires a bounded generation-bound asynchronous inventory."));
		SeenTargets.Add(AssetPath);
		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	void CaptureProjectSettings(FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
	{
		UGameMapsSettings* Settings = UGameMapsSettings::GetGameMapsSettings();
		FHyperAIGameFrameworkRecord Record;
		Record.Kind = TEXT("project_settings");
		Record.Role = TEXT("project");
		Record.StableId = TEXT("project:game_maps_settings");
		Record.TargetPath = Record.StableId;
		Record.ClassPath = UGameMapsSettings::StaticClass()->GetPathName();
		Record.bLoaded = Settings != nullptr;
		Record.bDetailsComplete = Settings != nullptr;
		Record.bRevisionComplete = Settings != nullptr;
		if (Settings)
		{
			Record.GameDefaultMap = UGameMapsSettings::GetGameDefaultMap();
			Record.TransitionMap = Settings->TransitionMap.ToString();
			Record.GameModeClassPath = UGameMapsSettings::GetGlobalDefaultGameMode();
			Record.GameInstanceClassPath = Settings->GameInstanceClass.ToString();
		}
		else Snapshot.bComplete = false;
		Snapshot.Records.Add(MoveTemp(Record));
	}

	void CaptureFrameworkWorldSettings(
		UWorld* World,
		FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
	{
		if (!World) return;
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAIGameFrameworkRecord SettingsRecord;
		SettingsRecord.Kind = TEXT("world_settings");
		SettingsRecord.Role = TEXT("world");
		SettingsRecord.WorldPath = World->GetPathName();
		SettingsRecord.TargetPath = SettingsRecord.WorldPath;
		SettingsRecord.StableId = TEXT("framework.world_settings:") + SettingsRecord.WorldPath;
		SettingsRecord.PackageName = World->GetOutermost()->GetName();
		SettingsRecord.bLoaded = true;
		SettingsRecord.bRevisionComplete = true;
		if (const AWorldSettings* WorldSettings = World->GetWorldSettings(false, false))
		{
			SettingsRecord.WorldSettingsPath = WorldSettings->GetPathName();
			SettingsRecord.ClassPath = WorldSettings->GetClass()->GetPathName();
			SettingsRecord.GameModeClassPath = ClassPath(WorldSettings->DefaultGameMode.Get());
			SettingsRecord.bPackageDirty = World->GetOutermost()->IsDirty()
				|| WorldSettings->GetOutermost()->IsDirty();
			SettingsRecord.bDetailsComplete = true;
		}
		else
		{
			SettingsRecord.bDetailsComplete = false;
			SettingsRecord.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		Snapshot.Records.Add(MoveTemp(SettingsRecord));
	}

	void CaptureFrameworkWorldRuntime(
		UWorld* World,
		FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
	{
		if (!World) return;
		CaptureFrameworkWorldSettings(World, Snapshot);
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAIGameFrameworkRecord Record;
		Record.Kind = TEXT("world_runtime");
		Record.Role = TEXT("world");
		Record.WorldPath = World->GetPathName();
		Record.TargetPath = Record.WorldPath;
		Record.StableId = TEXT("framework.world:") + Record.WorldPath;
		Record.ClassPath = World->GetClass()->GetPathName();
		Record.PackageName = World->GetOutermost()->GetName();
		Record.bLoaded = true;
		Record.bDetailsComplete = true;
		Record.bRevisionComplete = true;
		if (const AGameModeBase* GameMode = World->GetAuthGameMode())
			Record.GameModeClassPath = GameMode->GetClass()->GetPathName();
		const bool bHasGameState = World->GetGameState() != nullptr;
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			Record.GameStateClassPath = GameState->GetClass()->GetPathName();
			Record.PlayerStateCount = GameState->PlayerArray.Num();
		}
		if (const UGameInstance* GameInstance = World->GetGameInstance())
			Record.GameInstanceClassPath = GameInstance->GetClass()->GetPathName();
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
			{
				Record.bRevisionComplete = false;
				Snapshot.bComplete = false;
				break;
			}
			++Snapshot.ObjectsScanned;
			const APlayerController* Controller = It->Get();
			if (!Controller) continue;
			++Record.PlayerControllerCount;
			if (!bHasGameState && Controller->GetPlayerState<APlayerState>())
				++Record.PlayerStateCount;
			if (Controller->GetPawn()) ++Record.PawnCount;
		}
		Snapshot.Records.Add(MoveTemp(Record));
	}

	void CaptureOnlineObservation(FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		FHyperAIGameFrameworkRecord Record;
		Record.Kind = TEXT("online_subsystem");
		Record.Role = TEXT("online");
		Record.TargetPath = TEXT("engine:online_subsystem");
		Record.StableId = Record.TargetPath;
		Record.bRevisionComplete = true;
		Record.bDetailsComplete = true;
		for (TObjectIterator<UOnlineEngineInterface> It; It; ++It)
		{
			if (Snapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
			{
				Record.bRevisionComplete = false;
				Record.bDetailsComplete = false;
				Snapshot.bComplete = false;
				break;
			}
			++Snapshot.ObjectsScanned;
			UOnlineEngineInterface* Interface = *It;
			if (!Interface || Interface->HasAnyFlags(RF_ClassDefaultObject)) continue;
			Record.bOnlineInterfaceObserved = true;
			Record.bLoaded = true;
			const FName SubsystemName = Interface->GetDefaultOnlineSubsystemName();
			Record.OnlineSubsystemName = SubsystemName.ToString();
			Record.bOnlineSubsystemLoaded = SubsystemName.IsNone()
				? false : Interface->IsLoaded(SubsystemName);
			break;
		}
		Snapshot.Records.Add(MoveTemp(Record));
	}

	void CanonicalNetworkElement(FString& Out, const FHyperAINetworkElementView& Element)
	{
		AppendToken(Out, Element.Kind);
		AppendToken(Out, Element.StableId);
		AppendToken(Out, Element.Name);
		AppendToken(Out, Element.TypePath);
		AppendToken(Out, Element.Condition);
		AppendToken(Out, Element.RepNotifyFunction);
		AppendToken(Out, Element.RpcMode);
		AppendToken(Out, Element.DeclarationSource);
		AppendToken(Out, Element.OwnerClassPath);
		AppendInt(Out, Element.Index);
		AppendBool(Out, Element.bReplicated);
		AppendBool(Out, Element.bReliable);
		AppendBool(Out, Element.bWithValidation);
		AppendBool(Out, Element.bAuthoritativeDeclaration);
		AppendBool(Out, Element.bSignatureValid);
	}

	FString CanonicalNetworkRecord(const FHyperAINetworkRecord& Record)
	{
		FString Out;
		AppendToken(Out, Record.Kind); AppendToken(Out, Record.StableId);
		AppendToken(Out, Record.TargetPath); AppendToken(Out, Record.AssetPath);
		AppendToken(Out, Record.ClassPath); AppendToken(Out, Record.ParentClassPath);
		AppendToken(Out, Record.WorldPath); AppendBool(Out, Record.bRevisionComplete);
		AppendBool(Out, Record.bLoaded); AppendBool(Out, Record.bOnDiskMetadata);
		AppendBool(Out, Record.bDetailsComplete); AppendBool(Out, Record.bPackageDirty);
		AppendBool(Out, Record.bBlueprintCompileError);
		AppendToken(Out, Record.BlueprintCompileStatus);
		AppendBool(Out, Record.bGeneratedClassCurrent); AppendBool(Out, Record.bReplicates);
		AppendBool(Out, Record.bAlwaysRelevant); AppendBool(Out, Record.bOnlyRelevantToOwner);
		AppendBool(Out, Record.bUseOwnerRelevancy); AppendToken(Out, Record.Dormancy);
		AppendDouble(Out, Record.NetUpdateFrequency); AppendDouble(Out, Record.MinNetUpdateFrequency);
		AppendDouble(Out, Record.NetPriority); AppendBool(Out, Record.bHasAuthority);
		AppendToken(Out, Record.LocalRole); AppendToken(Out, Record.RemoteRole);
		AppendToken(Out, Record.OwnerPath); AppendToken(Out, Record.NetMode);
		AppendToken(Out, Record.NetDriverName); AppendInt(Out, Record.ClientConnectionCount);
		AppendBool(Out, Record.bHasServerConnection); AppendInt(Out, Record.ReplicatedPropertyCount);
		AppendInt(Out, Record.RpcCount); AppendInt(Out, Record.ReplicatedComponentCount);
		TArray<FHyperAINetworkElementView> Elements = Record.Elements;
		Elements.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		AppendInt(Out, Elements.Num());
		for (const auto& Element : Elements) CanonicalNetworkElement(Out, Element);
		return Out;
	}

	FString CanonicalFrameworkRecord(const FHyperAIGameFrameworkRecord& Record)
	{
		FString Out;
		AppendToken(Out, Record.Kind); AppendToken(Out, Record.Role);
		AppendToken(Out, Record.StableId); AppendToken(Out, Record.TargetPath);
		AppendToken(Out, Record.AssetPath); AppendToken(Out, Record.ClassPath);
		AppendToken(Out, Record.ParentClassPath); AppendBool(Out, Record.bRevisionComplete);
		AppendBool(Out, Record.bLoaded); AppendBool(Out, Record.bOnDiskMetadata);
		AppendBool(Out, Record.bDetailsComplete); AppendBool(Out, Record.bPackageDirty);
		AppendBool(Out, Record.bBlueprintCompileError);
		AppendToken(Out, Record.BlueprintCompileStatus);
		AppendBool(Out, Record.bGeneratedClassCurrent); AppendToken(Out, Record.WorldPath);
		AppendToken(Out, Record.WorldSettingsPath); AppendToken(Out, Record.PackageName);
		AppendToken(Out, Record.GameDefaultMap); AppendToken(Out, Record.TransitionMap);
		AppendToken(Out, Record.GameModeClassPath); AppendToken(Out, Record.GameStateClassPath);
		AppendToken(Out, Record.PlayerControllerClassPath); AppendToken(Out, Record.PlayerStateClassPath);
		AppendToken(Out, Record.PawnClassPath); AppendToken(Out, Record.HUDClassPath);
		AppendToken(Out, Record.GameSessionClassPath); AppendToken(Out, Record.SpectatorClassPath);
		AppendToken(Out, Record.GameInstanceClassPath); AppendInt(Out, Record.MaxPlayers);
		AppendInt(Out, Record.MaxSpectators); AppendInt(Out, Record.MaxSplitscreensPerConnection);
		AppendBool(Out, Record.bRequiresPushToTalk); AppendToken(Out, Record.SessionName);
		AppendToken(Out, Record.OnlineSubsystemName); AppendBool(Out, Record.bOnlineInterfaceObserved);
		AppendBool(Out, Record.bOnlineSubsystemLoaded); AppendInt(Out, Record.PlayerControllerCount);
		AppendInt(Out, Record.PlayerStateCount); AppendInt(Out, Record.PawnCount);
		return Out;
	}

	int64 EscapedStringUpperBound(const FString& Value)
	{
		// JSON UTF-8 escaping is at most six bytes for every UTF-16 code unit.
		return 6ll * Value.Len() + 8;
	}

	int64 EstimateNetworkRecordBytes(const FHyperAINetworkRecord& Record, const bool bDetails)
	{
		int64 Bytes = 1024;
		Bytes += EscapedStringUpperBound(Record.Kind) + EscapedStringUpperBound(Record.StableId)
			+ EscapedStringUpperBound(Record.TargetPath) + EscapedStringUpperBound(Record.AssetPath)
			+ EscapedStringUpperBound(Record.ClassPath) + EscapedStringUpperBound(Record.ParentClassPath)
			+ EscapedStringUpperBound(Record.WorldPath) + EscapedStringUpperBound(Record.Dormancy)
			+ EscapedStringUpperBound(Record.LocalRole) + EscapedStringUpperBound(Record.RemoteRole)
			+ EscapedStringUpperBound(Record.OwnerPath) + EscapedStringUpperBound(Record.NetMode)
			+ EscapedStringUpperBound(Record.NetDriverName) + EscapedStringUpperBound(Record.Revision)
			+ EscapedStringUpperBound(Record.BlueprintCompileStatus);
		if (bDetails)
		{
			for (const auto& Element : Record.Elements)
			{
				Bytes += 512 + EscapedStringUpperBound(Element.Kind)
					+ EscapedStringUpperBound(Element.StableId) + EscapedStringUpperBound(Element.Name)
					+ EscapedStringUpperBound(Element.TypePath) + EscapedStringUpperBound(Element.Condition)
					+ EscapedStringUpperBound(Element.RepNotifyFunction)
					+ EscapedStringUpperBound(Element.RpcMode)
					+ EscapedStringUpperBound(Element.DeclarationSource)
					+ EscapedStringUpperBound(Element.OwnerClassPath);
			}
		}
		return Bytes;
	}

	int64 EstimateFrameworkRecordBytes(const FHyperAIGameFrameworkRecord& Record)
	{
		int64 Bytes = 1536;
		const FString* Values[] = {&Record.Kind, &Record.Role, &Record.StableId, &Record.TargetPath,
			&Record.AssetPath, &Record.ClassPath, &Record.ParentClassPath, &Record.Revision,
			&Record.BlueprintCompileStatus, &Record.WorldPath, &Record.WorldSettingsPath,
			&Record.PackageName, &Record.GameDefaultMap, &Record.TransitionMap, &Record.GameModeClassPath,
			&Record.GameStateClassPath, &Record.PlayerControllerClassPath, &Record.PlayerStateClassPath,
			&Record.PawnClassPath, &Record.HUDClassPath, &Record.GameSessionClassPath,
			&Record.SpectatorClassPath, &Record.GameInstanceClassPath, &Record.SessionName,
			&Record.OnlineSubsystemName};
		for (const FString* Value : Values) Bytes += EscapedStringUpperBound(*Value);
		return Bytes;
	}

	bool ParseCursor(const FString& Cursor, FString& OutRevision, int32& OutOffset)
	{
		OutRevision.Reset();
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		if (!Cursor.StartsWith(TEXT("revision:"), ESearchCase::CaseSensitive)) return false;
		FString RevisionPart;
		FString OffsetPart;
		if (!Cursor.Mid(9).Split(TEXT("|offset:"), &RevisionPart, &OffsetPart,
			ESearchCase::CaseSensitive, ESearchDir::FromEnd)) return false;
		if (!FHyperAIStudioNetworkingContracts::IsCanonicalSha256(RevisionPart)) return false;
		if (OffsetPart.IsEmpty()) return false;
		for (const TCHAR Character : OffsetPart)
			if (Character < TEXT('0') || Character > TEXT('9')) return false;
		const int64 Parsed = FCString::Atoi64(*OffsetPart);
		if (Parsed < 0 || Parsed > MAX_int32) return false;
		OutRevision = RevisionPart;
		OutOffset = static_cast<int32>(Parsed);
		return true;
	}

	FString MakeCursor(const FString& Revision, const int32 Offset)
	{
		return TEXT("revision:") + Revision + TEXT("|offset:") + FString::FromInt(Offset);
	}

	int64 NowMonotonicMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}

	struct FStagingState
	{
		FCriticalSection Mutex;
		TMap<FString, FHyperAIStudioNetworkingStagedArtifact> Artifacts;
	};

	FStagingState& GetStagingState()
	{
		static FStagingState State;
		return State;
	}

	void PruneExpired(FStagingState& State, const int64 NowMs)
	{
		for (auto It = State.Artifacts.CreateIterator(); It; ++It)
			if (It.Value().ExpiresMonotonicMs <= NowMs) It.RemoveCurrent();
	}

	FString StageKey(const FString& ProjectId, const FString& OperationId)
	{
		return ProjectId + TEXT("\n") + OperationId;
	}
}

const TArray<FHyperAIStudioNetworkingVariantDescriptor>&
FHyperAIStudioNetworkingFacade::GetVariantDescriptors()
{
	static const TArray<FHyperAIStudioNetworkingVariantDescriptor> Descriptors = {
		{TEXT("network"), TEXT("class_defaults"), {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_actor_replication_defaults"), TEXT("dormancy_relevancy_update_policy"),
				TEXT("closed_cas_plan_stage")},
			{TEXT("ordinary_actor_and_property_crud")},
			{TEXT("implicit_class_or_asset_loading"), TEXT("native_CDO_mutation"),
				TEXT("runtime_replication_events"), TEXT("skeleton_reinst_or_stale_class_claims"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("network"), TEXT("blueprint_replication_rpc"),
			{TEXT("Engine"), TEXT("BlueprintGraph")}, true, true, false,
			{TEXT("compiled_replicated_properties"), TEXT("blueprint_replication_conditions"),
				TEXT("rpc_direction_reliability_validation"), TEXT("replicated_default_components")},
			{TEXT("blueprint_function_graph_and_variable_crud")},
			{TEXT("unrestricted_reflection_mutation"), TEXT("arbitrary_rpc_function_creation"),
				TEXT("inherited_member_mutation"), TEXT("mutation_execution_until_async_host")}},
		{TEXT("network"), TEXT("world_net_driver"), {TEXT("Engine")}, true, false, false,
			{TEXT("loaded_world_net_mode"), TEXT("driver_identity_and_connection_counts"),
				TEXT("authority_role_owner_diagnostics")}, {},
			{TEXT("console_commands"), TEXT("connection_addresses_or_credentials"),
				TEXT("network_event_dispatch"), TEXT("pie_session_control")}},
		{TEXT("network"), TEXT("on_disk_asset_metadata"), {TEXT("AssetRegistry")}, true, false, false,
			{TEXT("nonblocking_exact_target_package_state")}, {},
			{TEXT("find_in_blueprints_blob_decoding"), TEXT("implicit_asset_loading"),
				TEXT("synchronous_broad_asset_inventory"),
				TEXT("synchronous_cached_blueprint_identity_or_lineage"),
				TEXT("cached_revision_without_async_inventory"),
				TEXT("unloaded_rpc_or_default_detail_claims")}},
		{TEXT("game_framework"), TEXT("framework_class_defaults"), {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_game_mode_class_references"),
				TEXT("typed_framework_class_lifecycle_stage")},
			{TEXT("ordinary_blueprint_class_and_property_crud")},
			{TEXT("implicit_class_loading"), TEXT("native_CDO_mutation"),
				TEXT("synchronous_broad_asset_inventory"),
				TEXT("synchronous_cached_framework_identity_or_lineage"),
				TEXT("cached_revision_without_async_inventory"),
				TEXT("skeleton_reinst_or_stale_class_claims"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("game_framework"), TEXT("project_maps_modes"), {TEXT("EngineSettings")}, true, true, false,
			{TEXT("game_maps_settings_api_inspection"), TEXT("closed_project_default_stage")}, {},
			{TEXT("raw_ini_or_file_access"), TEXT("implicit_map_or_class_loading"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("game_framework"), TEXT("world_runtime"), {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_game_mode_state_instance_relationships"),
				TEXT("controller_playerstate_pawn_counts"),
				TEXT("world_settings_default_game_mode_cas")}, {},
			{TEXT("runtime_spawn_possession_or_travel"), TEXT("pie_session_control"),
				TEXT("synchronous_broad_asset_inventory")}},
		{TEXT("game_framework"), TEXT("session_config"), {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_game_session_class_defaults"), TEXT("closed_session_limit_stage")}, {},
			{TEXT("session_start_end_join_or_registration"), TEXT("oss_login_or_cloud"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("game_framework"), TEXT("online_subsystem_observation"), {TEXT("Engine")}, true, false, false,
			{TEXT("already_instantiated_engine_interface"),
				TEXT("default_provider_name_and_loaded_state")}, {},
			{TEXT("online_interface_creation"), TEXT("oss_module_loading"),
				TEXT("login_cloud_or_session_calls")}}
	};
	return Descriptors;
}

TArray<FHyperAINetworkingVariantStatus>
FHyperAIStudioNetworkingFacade::ResolveVariantStatuses(const FString& Domain)
{
	TArray<FHyperAINetworkingVariantStatus> Result;
	for (const auto& Descriptor : GetVariantDescriptors())
	{
		if (!Domain.IsEmpty() && Descriptor.Domain != Domain) continue;
		FHyperAINetworkingVariantStatus Status;
		Status.Domain = Descriptor.Domain;
		Status.Variant = Descriptor.Variant;
		Status.RequiredModules = Descriptor.RequiredModules;
		Status.bInspectImplemented = Descriptor.bInspectImplemented;
		Status.bPlanSchemaImplemented = Descriptor.bPlanSchemaImplemented;
		Status.bApplyBackendExecutable = Descriptor.bApplyBackendExecutable;
		Status.SupportedCases = Descriptor.SupportedCases;
		Status.DelegatedEpicCases = Descriptor.DelegatedEpicCases;
		Status.UnsupportedCases = Descriptor.UnsupportedCases;
		Status.bModulesLoaded = true;
		for (const FString& Module : Descriptor.RequiredModules)
			Status.bModulesLoaded &= FModuleManager::Get().IsModuleLoaded(*Module);
		if (!Descriptor.bInspectImplemented)
		{
			Status.State = TEXT("adapter_unavailable");
			Status.Remediation = TEXT("The exact typed inspection adapter is unavailable; no fallback is attempted.");
		}
		else if (!Status.bModulesLoaded)
		{
			Status.State = TEXT("enabled_not_loaded");
			Status.Remediation = TEXT("Required engine modules are not loaded; no module is loaded implicitly by this tool.");
		}
		else if (!Descriptor.bPlanSchemaImplemented)
		{
			Status.State = TEXT("inspect_ready_apply_unavailable");
			Status.Remediation = TEXT("Bounded inspection/validation is available; this variant intentionally exposes no edit plan.");
		}
		else if (!Descriptor.bApplyBackendExecutable)
		{
			Status.State = TEXT("staged_backend_required");
			Status.Remediation = TEXT("Dry-run and idempotent staging are available; the shared async host still needs an independent typed mutator and validator backend.");
		}
		else Status.State = TEXT("ready");
		Result.Add(MoveTemp(Status));
	}
	return Result;
}

bool FHyperAIStudioNetworkingFacade::CaptureNetwork(
	const TArray<FString>& TargetPaths,
	const FString& Scope,
	const bool bIncludeDetails,
	const bool bIncludeOnDisk,
	FHyperAIStudioNetworkValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic,
	const int32 MaxWorkMs)
{
	using namespace HyperAIStudio::Networking::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded UObject and world inspection must run on Unreal's serialized game thread.");
		return false;
	}
	if (MaxWorkMs < 1 || MaxWorkMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs)
	{
		OutStatus = TEXT("invalid_work_budget");
		OutDiagnostic = TEXT("Network capture requires a 1..250 ms game-thread budget.");
		return false;
	}
	if (TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths)
	{
		OutStatus = TEXT("too_many_targets");
		OutDiagnostic = TEXT("Target path count exceeds the closed request bound.");
		return false;
	}
	static const TSet<FString> Scopes = {TEXT("all"), TEXT("class_defaults"),
		TEXT("blueprint_declarations"), TEXT("world_instances"), TEXT("net_drivers"),
		TEXT("on_disk_assets")};
	if (Scope.IsEmpty() || Scope.Len() > FHyperAIStudioNetworkingContracts::MaxNameCharacters
		|| !Scopes.Contains(Scope))
	{
		OutStatus = TEXT("invalid_scope");
		OutDiagnostic = TEXT("Network scope is outside the closed vocabulary.");
		return false;
	}
	if (Scope == TEXT("on_disk_assets") && !bIncludeOnDisk)
	{
		OutStatus = TEXT("on_disk_scope_requires_metadata");
		OutDiagnostic = TEXT("The on_disk_assets scope requires bIncludeOnDiskMetadata=true.");
		return false;
	}
	for (const FString& Path : TargetPaths)
	{
		if (!FHyperAIStudioNetworkingContracts::IsCanonicalTargetPath(Path))
		{
			OutStatus = TEXT("invalid_target_path");
			OutDiagnostic = TEXT("Every target must be one canonical /Game object/generated-class path or /Script class path.");
			return false;
		}
	}
	OutSnapshot.WorkDeadlineSeconds = FPlatformTime::Seconds()
		+ static_cast<double>(MaxWorkMs) / 1000.0;
	TArray<FString> SortedTargets = TargetPaths;
	SortedTargets.Sort();
	for (int32 Index = 1; Index < SortedTargets.Num(); ++Index)
	{
		if (SortedTargets[Index] == SortedTargets[Index - 1])
		{
			OutStatus = TEXT("duplicate_target_path");
			OutDiagnostic = TEXT("A target path may occur only once in one network capture.");
			return false;
		}
	}
	FString ScopeCanonical;
	AppendToken(ScopeCanonical, Scope);
	AppendBool(ScopeCanonical, bIncludeDetails);
	AppendBool(ScopeCanonical, bIncludeOnDisk);
	TSet<FString> CanonicalRequestedTargets;
	for (const FString& Path : SortedTargets)
	{
		FString CanonicalRequestTarget = Path;
		FString ResolvedAssetPath;
		if (FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			Path, ResolvedAssetPath)) CanonicalRequestTarget = ResolvedAssetPath;
		if (CanonicalRequestedTargets.Contains(CanonicalRequestTarget))
		{
			OutStatus = TEXT("duplicate_semantic_target");
			OutDiagnostic = TEXT("Blueprint asset and GeneratedClass aliases may not both occur in one capture.");
			return false;
		}
		CanonicalRequestedTargets.Add(CanonicalRequestTarget);
		AppendToken(ScopeCanonical, CanonicalRequestTarget);
	}
	OutSnapshot.ScopeFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ScopeCanonical);
	if (!FHyperAIStudioNetworkingContracts::IsCanonicalSha256(OutSnapshot.ScopeFingerprint))
	{
		OutStatus = TEXT("scope_hash_failed");
		OutDiagnostic = TEXT("Canonical network scope exceeded the shared hash bound.");
		return false;
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (bIncludeOnDisk && !AssetRegistry)
	{
		OutStatus = TEXT("asset_registry_unavailable");
		OutDiagnostic = TEXT("Asset Registry is not initialized; no module or asset is loaded implicitly.");
		return false;
	}
	TSet<FString> SeenTargets;
	const bool bRegistryDiscoveryIncomplete = bIncludeOnDisk
		&& (AssetRegistry->IsGathering() || AssetRegistry->IsLoadingAssets());
	if (bRegistryDiscoveryIncomplete)
	{
		OutSnapshot.bComplete = false;
		AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
			TEXT("asset_registry_gathering"), TEXT("warning"), FString(),
			TEXT("Asset Registry discovery is still gathering; on-disk coverage is incomplete."));
	}

	if (!SortedTargets.IsEmpty())
	{
		for (const FString& Path : SortedTargets)
		{
			if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
			{
				OutSnapshot.bComplete = false;
				break;
			}
			++OutSnapshot.ObjectsScanned;
			if (UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Path))
			{
				if (IsInspectableBlueprintClass(Blueprint, Blueprint->GeneratedClass)
					&& Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
				{
					CaptureNetworkClass(Blueprint->GeneratedClass, Blueprint, bIncludeDetails, OutSnapshot);
					SeenTargets.Add(Path);
					continue;
				}
				OutSnapshot.bComplete = false;
				AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
					TEXT("blueprint_generated_class_not_current"), TEXT("error"), Path,
					TEXT("Loaded Blueprint is not bound to its one current non-transient GeneratedClass; compile/repair it first."));
				continue;
			}
			if (UClass* Class = FindObject<UClass>(nullptr, *Path))
			{
				if (IsInspectableClass(Class) && Class->IsChildOf(AActor::StaticClass()))
				{
					CaptureNetworkClass(Class, Cast<UBlueprint>(Class->ClassGeneratedBy),
						bIncludeDetails, OutSnapshot);
					SeenTargets.Add(AssetPathForClass(Class).IsEmpty()
						? Path : AssetPathForClass(Class));
					continue;
				}
				OutSnapshot.bComplete = false;
				AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
					TEXT("class_not_current_or_supported"), TEXT("error"), Path,
					TEXT("Loaded class is transient/stale or does not derive from AActor."));
				continue;
			}
			if (UWorld* World = FindObject<UWorld>(nullptr, *Path))
			{
				CaptureWorldNetworkState(World, Scope != TEXT("net_drivers"), OutSnapshot);
				continue;
			}
			if (bIncludeOnDisk && Path.StartsWith(TEXT("/Game/")))
			{
				if (bRegistryDiscoveryIncomplete)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
						TEXT("target_existence_unknown"), TEXT("error"), Path,
						TEXT("Asset Registry discovery is incomplete; exact cached target identity is unknown and must come from a later bounded asynchronous inventory."));
					continue;
				}
				FString QueryPath = Path;
				FString ResolvedAssetPath;
				if (FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
					Path, ResolvedAssetPath)) QueryPath = ResolvedAssetPath;
				FName PackageName;
				FAssetPackageData PackageData;
				const UE::AssetRegistry::EExists Exists =
					TryGetProjectAssetPackageName(QueryPath, false, PackageName)
					? AssetRegistry->TryGetAssetPackageData(
						PackageName, PackageData, true /* bFailIfLockHeld */)
					: UE::AssetRegistry::EExists::Unknown;
				if (Exists == UE::AssetRegistry::EExists::Exists)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
						TEXT("cached_metadata_requires_async_inventory"), TEXT("error"), Path,
						TEXT("The package exists, but package data has no authoritative Blueprint class tags. Exact object identity, lineage, and revision require a bounded generation-bound asynchronous inventory."));
					continue;
				}
				else if (Exists == UE::AssetRegistry::EExists::Unknown)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
						TEXT("target_existence_unknown"), TEXT("error"), Path,
						TEXT("Asset Registry could not prove target-package state without waiting; the no-load capture fails closed."));
					continue;
				}
			}
			OutSnapshot.bComplete = false;
			AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
				TEXT("target_not_found_no_load"), TEXT("error"), Path,
				TEXT("Target is neither already loaded nor a supported cached Blueprint asset."));
		}
	}
	else
	{
		if (Scope == TEXT("all") || Scope == TEXT("class_defaults")
			|| Scope == TEXT("blueprint_declarations"))
		{
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| OutSnapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords
					|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.ObjectsScanned;
				UBlueprint* Blueprint = *It;
				if (!Blueprint || !Blueprint->GetPathName().StartsWith(TEXT("/Game/"))
					|| !IsInspectableBlueprintClass(Blueprint, Blueprint->GeneratedClass)
					|| !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass())) continue;
				if (SeenTargets.Contains(Blueprint->GetPathName())) continue;
				CaptureNetworkClass(Blueprint->GeneratedClass, Blueprint, bIncludeDetails, OutSnapshot);
				SeenTargets.Add(Blueprint->GetPathName());
			}
		}
		if (bIncludeOnDisk && (Scope == TEXT("all") || Scope == TEXT("on_disk_assets")))
		{
			OutSnapshot.bComplete = false;
			AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("network"),
				TEXT("broad_inventory_requires_async_cache"), TEXT("warning"), FString(),
				TEXT("Synchronous broad Asset Registry enumeration is disabled. Supply exact target paths, or use a future generation-bound asynchronous inventory cache."));
		}
		if (GEngine && (Scope == TEXT("all") || Scope == TEXT("world_instances")
			|| Scope == TEXT("net_drivers")))
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.ObjectsScanned;
				CaptureWorldNetworkState(Context.World(), Scope != TEXT("net_drivers"), OutSnapshot);
			}
		}
	}

	OutSnapshot.Records.Sort([](const auto& A, const auto& B)
	{
		return A.StableId < B.StableId;
	});
	FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(OutSnapshot);
	OutStatus = OutSnapshot.bComplete ? TEXT("captured_no_load_state")
		: TEXT("captured_partial_state");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured a bounded deterministic network view from loaded objects and cached Asset Registry metadata without loading assets.")
		: TEXT("Captured a partial no-load network view; completeness and CAS freshness remain false.");
	return true;
}

bool FHyperAIStudioNetworkingFacade::CaptureGameFramework(
	const TArray<FString>& TargetPaths,
	const FString& Scope,
	const bool bIncludeDetails,
	const bool bIncludeOnDisk,
	FHyperAIStudioGameFrameworkValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic,
	const int32 MaxWorkMs)
{
	using namespace HyperAIStudio::Networking::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Framework UObject/config inspection must run on Unreal's serialized game thread.");
		return false;
	}
	if (MaxWorkMs < 1 || MaxWorkMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs)
	{
		OutStatus = TEXT("invalid_work_budget");
		OutDiagnostic = TEXT("Framework capture requires a 1..250 ms game-thread budget.");
		return false;
	}
	if (TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths)
	{
		OutStatus = TEXT("too_many_targets");
		OutDiagnostic = TEXT("Target path count exceeds the closed request bound.");
		return false;
	}
	static const TSet<FString> Scopes = {TEXT("all"), TEXT("project_settings"),
		TEXT("class_defaults"), TEXT("world_runtime"), TEXT("world_settings"),
		TEXT("session"), TEXT("online"),
		TEXT("on_disk_assets")};
	if (Scope.IsEmpty() || Scope.Len() > FHyperAIStudioNetworkingContracts::MaxNameCharacters
		|| !Scopes.Contains(Scope))
	{
		OutStatus = TEXT("invalid_scope");
		OutDiagnostic = TEXT("Game Framework scope is outside the closed vocabulary.");
		return false;
	}
	if (Scope == TEXT("on_disk_assets") && !bIncludeOnDisk)
	{
		OutStatus = TEXT("on_disk_scope_requires_metadata");
		OutDiagnostic = TEXT("The on_disk_assets scope requires bIncludeOnDiskMetadata=true.");
		return false;
	}
	for (const FString& Path : TargetPaths)
	{
		if (Path != TEXT("project:game_maps_settings")
			&& !FHyperAIStudioNetworkingContracts::IsCanonicalTargetPath(Path))
		{
			OutStatus = TEXT("invalid_target_path");
			OutDiagnostic = TEXT("Every Framework target must be the project settings token or one canonical /Game or /Script object path.");
			return false;
		}
	}
	OutSnapshot.WorkDeadlineSeconds = FPlatformTime::Seconds()
		+ static_cast<double>(MaxWorkMs) / 1000.0;
	TArray<FString> SortedTargets = TargetPaths;
	SortedTargets.Sort();
	for (int32 Index = 1; Index < SortedTargets.Num(); ++Index)
	{
		if (SortedTargets[Index] == SortedTargets[Index - 1])
		{
			OutStatus = TEXT("duplicate_target_path");
			OutDiagnostic = TEXT("A target path may occur only once in one Framework capture.");
			return false;
		}
	}
	FString ScopeCanonical;
	AppendToken(ScopeCanonical, Scope);
	AppendBool(ScopeCanonical, bIncludeDetails);
	AppendBool(ScopeCanonical, bIncludeOnDisk);
	TSet<FString> CanonicalRequestedTargets;
	for (const FString& Path : SortedTargets)
	{
		FString CanonicalRequestTarget = Path;
		FString ResolvedAssetPath;
		if (FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			Path, ResolvedAssetPath)) CanonicalRequestTarget = ResolvedAssetPath;
		if (CanonicalRequestedTargets.Contains(CanonicalRequestTarget))
		{
			OutStatus = TEXT("duplicate_semantic_target");
			OutDiagnostic = TEXT("Framework asset and GeneratedClass aliases may not both occur in one capture.");
			return false;
		}
		CanonicalRequestedTargets.Add(CanonicalRequestTarget);
		AppendToken(ScopeCanonical, CanonicalRequestTarget);
	}
	OutSnapshot.ScopeFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ScopeCanonical);
	if (!FHyperAIStudioNetworkingContracts::IsCanonicalSha256(OutSnapshot.ScopeFingerprint))
	{
		OutStatus = TEXT("scope_hash_failed");
		OutDiagnostic = TEXT("Canonical Framework scope exceeded the shared hash bound.");
		return false;
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (bIncludeOnDisk && !AssetRegistry)
	{
		OutStatus = TEXT("asset_registry_unavailable");
		OutDiagnostic = TEXT("Asset Registry is not initialized; no module or asset is loaded implicitly.");
		return false;
	}
	TSet<FString> SeenTargets;
	const bool bRegistryDiscoveryIncomplete = bIncludeOnDisk
		&& (AssetRegistry->IsGathering() || AssetRegistry->IsLoadingAssets());
	if (bRegistryDiscoveryIncomplete)
	{
		OutSnapshot.bComplete = false;
		AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
			TEXT("asset_registry_gathering"), TEXT("warning"), FString(),
			TEXT("Asset Registry discovery is still gathering; on-disk Framework coverage is incomplete."));
	}

	if (!SortedTargets.IsEmpty())
	{
		for (const FString& Path : SortedTargets)
		{
			if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
				|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
			{
				OutSnapshot.bComplete = false;
				break;
			}
			++OutSnapshot.ObjectsScanned;
			if (Path == TEXT("project:game_maps_settings"))
			{
				CaptureProjectSettings(OutSnapshot);
				continue;
			}
			if (UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Path))
			{
				if (IsInspectableBlueprintClass(Blueprint, Blueprint->GeneratedClass)
					&& !FrameworkRole(Blueprint->GeneratedClass).IsEmpty())
				{
					CaptureFrameworkClass(Blueprint->GeneratedClass, Blueprint, OutSnapshot);
					SeenTargets.Add(Path);
					continue;
				}
				OutSnapshot.bComplete = false;
				AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
					TEXT("blueprint_generated_class_not_current"), TEXT("error"), Path,
					TEXT("Loaded Framework Blueprint is not bound to its one current non-transient GeneratedClass; compile/repair it first."));
				continue;
			}
			if (UClass* Class = FindObject<UClass>(nullptr, *Path))
			{
				if (IsInspectableClass(Class) && !FrameworkRole(Class).IsEmpty())
				{
					CaptureFrameworkClass(Class, Cast<UBlueprint>(Class->ClassGeneratedBy), OutSnapshot);
					const FString AssetPath = AssetPathForClass(Class);
					SeenTargets.Add(AssetPath.IsEmpty() ? Path : AssetPath);
					continue;
				}
				OutSnapshot.bComplete = false;
				AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
					TEXT("class_not_current_or_supported"), TEXT("error"), Path,
					TEXT("Loaded class is transient/stale or outside the closed Framework-role hierarchy."));
				continue;
			}
			if (UWorld* World = FindObject<UWorld>(nullptr, *Path))
			{
				if (Scope == TEXT("world_settings"))
					CaptureFrameworkWorldSettings(World, OutSnapshot);
				else CaptureFrameworkWorldRuntime(World, OutSnapshot);
				continue;
			}
			if (bIncludeOnDisk && Path.StartsWith(TEXT("/Game/")))
			{
				if (bRegistryDiscoveryIncomplete)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
						TEXT("target_existence_unknown"), TEXT("error"), Path,
						TEXT("Asset Registry discovery is incomplete; exact cached Framework identity is unknown and must come from a later bounded asynchronous inventory."));
					continue;
				}
				FString QueryPath = Path;
				FString ResolvedAssetPath;
				if (FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
					Path, ResolvedAssetPath)) QueryPath = ResolvedAssetPath;
				FName PackageName;
				FAssetPackageData PackageData;
				const UE::AssetRegistry::EExists Exists =
					TryGetProjectAssetPackageName(QueryPath, false, PackageName)
					? AssetRegistry->TryGetAssetPackageData(
						PackageName, PackageData, true /* bFailIfLockHeld */)
					: UE::AssetRegistry::EExists::Unknown;
				if (Exists == UE::AssetRegistry::EExists::Exists)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
						TEXT("cached_metadata_requires_async_inventory"), TEXT("error"), Path,
						TEXT("The package exists, but package data has no authoritative Blueprint class tags. Exact object identity, Framework lineage, and revision require a bounded generation-bound asynchronous inventory."));
					continue;
				}
				else if (Exists == UE::AssetRegistry::EExists::Unknown)
				{
					OutSnapshot.bComplete = false;
					AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
						TEXT("target_existence_unknown"), TEXT("error"), Path,
						TEXT("Asset Registry could not prove target-package state without waiting; the no-load capture fails closed."));
					continue;
				}
			}
			OutSnapshot.bComplete = false;
			AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
				TEXT("target_not_found_no_load"), TEXT("error"), Path,
				TEXT("Framework target is neither already loaded nor supported cached metadata."));
		}
	}
	else
	{
		if (Scope == TEXT("all") || Scope == TEXT("project_settings"))
			CaptureProjectSettings(OutSnapshot);
		if (Scope == TEXT("all") || Scope == TEXT("class_defaults") || Scope == TEXT("session"))
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| OutSnapshot.Records.Num() >= FHyperAIStudioNetworkingContracts::MaxRecords
					|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.ObjectsScanned;
				UClass* Class = *It;
				if (!IsInspectableClass(Class)) continue;
				const FString Role = FrameworkRole(Class);
				if (Role.IsEmpty()) continue;
				if (Scope == TEXT("session") && Role != TEXT("game_session")) continue;
				if (!Class->GetPathName().StartsWith(TEXT("/Game/"))
					&& !Class->GetPathName().StartsWith(TEXT("/Script/"))) continue;
				const FString AssetPath = AssetPathForClass(Class);
				const FString CanonicalTarget = AssetPath.IsEmpty() ? Class->GetPathName() : AssetPath;
				if (SeenTargets.Contains(CanonicalTarget)) continue;
				CaptureFrameworkClass(Class, Cast<UBlueprint>(Class->ClassGeneratedBy), OutSnapshot);
				SeenTargets.Add(CanonicalTarget);
			}
		}
		if (bIncludeOnDisk && (Scope == TEXT("all") || Scope == TEXT("on_disk_assets")))
		{
			OutSnapshot.bComplete = false;
			AddCaptureIssue(OutSnapshot.CaptureIssues, TEXT("game_framework"),
				TEXT("broad_inventory_requires_async_cache"), TEXT("warning"), FString(),
				TEXT("Synchronous broad Asset Registry enumeration is disabled. Supply exact target paths, or use a future generation-bound asynchronous inventory cache."));
		}
		if (GEngine && (Scope == TEXT("all") || Scope == TEXT("world_runtime")
			|| Scope == TEXT("world_settings")))
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (OutSnapshot.ObjectsScanned >= FHyperAIStudioNetworkingContracts::MaxObjectsScanned
					|| IsDeadlineExpired(OutSnapshot.WorkDeadlineSeconds))
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.ObjectsScanned;
				if (Scope == TEXT("world_settings"))
					CaptureFrameworkWorldSettings(Context.World(), OutSnapshot);
				else CaptureFrameworkWorldRuntime(Context.World(), OutSnapshot);
			}
		}
		if (Scope == TEXT("all") || Scope == TEXT("online"))
			CaptureOnlineObservation(OutSnapshot);
	}

	OutSnapshot.Records.Sort([](const auto& A, const auto& B)
	{
		return A.StableId < B.StableId;
	});
	FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(OutSnapshot);
	OutStatus = OutSnapshot.bComplete ? TEXT("captured_no_load_state")
		: TEXT("captured_partial_state");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured bounded Framework defaults, settings, runtime relationships, session, and loaded online-interface state without loading assets or providers.")
		: TEXT("Captured a partial no-load Framework view; completeness and CAS freshness remain false.");
	return true;
}

FString FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioNetworking.HyperAIStudioNetworkingToolset");
}

const TArray<FHyperAIStudioNetworkingManifestEntry>&
FHyperAIStudioNetworkingContracts::GetManifest()
{
	static const TArray<FHyperAIStudioNetworkingManifestEntry> Manifest = {
		{TEXT("hyper_network_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_network_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_network_validate"), GetQualifiedToolsetName()},
		{TEXT("hyper_game_framework_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_game_framework_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_game_framework_validate"), GetQualifiedToolsetName()}
	};
	return Manifest;
}

bool FHyperAIStudioNetworkingContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioNetworkingContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const auto& Manifest = GetManifest();
	if (Manifest.Num() != 6) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const auto& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioNetworkingContracts::IsCanonicalTargetPath(
	const FString& Path,
	const bool bAllowScriptClass)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || Path.Contains(TEXT("\\"))
		|| Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))) return false;
	for (const TCHAR Character : Path) if (Character == TEXT('\0')) return false;
	if (!Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		&& !(bAllowScriptClass && Path.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive)))
		return false;
	return FPackageName::IsValidObjectPath(Path);
}

bool FHyperAIStudioNetworkingContracts::IsCanonicalProjectAssetPath(const FString& Path)
{
	if (!IsCanonicalTargetPath(Path, false) || Path.EndsWith(TEXT("_C"))) return false;
	FString PackageName;
	FString ObjectName;
	return Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd) && !ObjectName.IsEmpty();
}

bool FHyperAIStudioNetworkingContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
	const FString& GeneratedClassPath,
	FString& OutAssetPath)
{
	using namespace HyperAIStudio::Networking::Private;
	OutAssetPath.Reset();
	const FString Normalized = NormalizeExportObjectPath(GeneratedClassPath);
	if (!Normalized.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| Normalized.Contains(TEXT("\\")) || Normalized.Contains(TEXT(".."))) return false;
	FString PackageName;
	FString ObjectName;
	if (!Normalized.Split(TEXT("."), &PackageName, &ObjectName,
		ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		|| !ObjectName.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive)) return false;
	ObjectName = ObjectName.LeftChop(2);
	if (ObjectName.IsEmpty()) return false;
	OutAssetPath = PackageName + TEXT(".") + ObjectName;
	if (!IsCanonicalProjectAssetPath(OutAssetPath))
	{
		OutAssetPath.Reset();
		return false;
	}
	return true;
}

bool FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
	const FString& Status)
{
	return Status == TEXT("up_to_date") || Status == TEXT("up_to_date_with_warnings");
}

bool FHyperAIStudioNetworkingContracts::IsCurrentGeneratedClass(
	const UBlueprint* Blueprint,
	const UClass* Class)
{
	return Blueprint && Class && Blueprint->GeneratedClass == Class
		&& Blueprint->SkeletonGeneratedClass != Class
		&& Class->ClassGeneratedBy == Blueprint
		&& Cast<UBlueprintGeneratedClass>(Class) != nullptr
		&& !Class->HasAnyFlags(RF_Transient)
		&& !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
		&& !Class->GetName().StartsWith(TEXT("REINST_"), ESearchCase::CaseSensitive)
		&& !Class->GetName().StartsWith(TEXT("SKEL_"), ESearchCase::CaseSensitive);
}

FString FHyperAIStudioNetworkingContracts::ClassifyLoadedFrameworkRole(const UClass* Class)
{
	using namespace HyperAIStudio::Networking::Private;
	return FrameworkRole(Class);
}

bool FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(
	const int32 ObjectsScanned,
	const int32 RecordCount,
	const double NowSeconds,
	const double DeadlineSeconds)
{
	return ObjectsScanned >= 0 && ObjectsScanned < MaxObjectsScanned
		&& RecordCount >= 0 && RecordCount < MaxRecords
		&& FMath::IsFinite(NowSeconds) && FMath::IsFinite(DeadlineSeconds)
		&& DeadlineSeconds > 0.0 && NowSeconds < DeadlineSeconds;
}

bool FHyperAIStudioNetworkingContracts::HasAuthoritativeDeclaredSelector(
	const FHyperAINetworkRecord& Record,
	const FString& KindA,
	const FString& KindB,
	const FString& Name,
	const FString& DeclarationSource,
	const bool bRequireValidSignature)
{
	return Record.Elements.ContainsByPredicate([&](const auto& Element)
	{
		return Element.Name == Name && (Element.Kind == KindA || Element.Kind == KindB)
			&& Element.bAuthoritativeDeclaration
			&& Element.OwnerClassPath == Record.ClassPath
			&& (DeclarationSource.IsEmpty() || Element.DeclarationSource == DeclarationSource)
			&& (!bRequireValidSignature || Element.bSignatureValid);
	});
}

FString FHyperAIStudioNetworkingContracts::PayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("networking_game_framework.payload.v1|two_closed_domains|typed_kind_binding|loaded_no_load_cas|exact_cached_lineage|current_blueprint_declarations|world_settings_cas|symbolic_create_lifecycle|edit_only|no_client_authorization|no_script_console_ini_process_event_or_runtime_network_effect"));
	return Fingerprint;
}

FString FHyperAIStudioNetworkingContracts::ResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("networking_game_framework.result.v1|domain|phase|revision|compile_state|generated_class_identity|world_settings_identity|valid|error_count|bounded"));
	return Fingerprint;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("hyperaistudio.networking_game_framework.base");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		FHyperAIStudioDomainVariantDescriptor Network;
		Network.ToolName = TEXT("hyper_network_apply_plan");
		Network.VariantId = NetworkMutationVariantId;
		Network.RequestTypeId = PayloadTypeId;
		Network.RequestSchemaFingerprint = PayloadSchemaFingerprint();
		Network.ResultTypeId = ResultTypeId;
		Network.ResultSchemaFingerprint = ResultSchemaFingerprint();
		Network.Safety = EHyperAIStudioDomainSafety::Edit;
		FHyperAIStudioDomainVariantDescriptor Framework = Network;
		Framework.ToolName = TEXT("hyper_game_framework_apply_plan");
		Framework.VariantId = FrameworkMutationVariantId;
		Value.Variants = {Network, Framework};
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
	const FHyperAINetworkPlanOperation& Operation,
	FHyperAIStudioNetworkingBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Networking::Private;
	OutOperation = {};
	OutErrorCode.Reset();
	OutError.Reset();
	if (!IsCanonicalProjectAssetPath(Operation.TargetPath)
		|| !IsCanonicalSha256(Operation.ExpectedRevision))
	{
		OutErrorCode = TEXT("invalid_target_or_revision");
		OutError = TEXT("Network edits require one canonical /Game asset target and its exact inspector revision.");
		return false;
	}
	OutOperation.Domain = TEXT("network");
	OutOperation.Type = Operation.Type;
	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.MemberName = Operation.MemberName;
	OutOperation.Replicates = Operation.Replicates;
	OutOperation.AlwaysRelevant = Operation.AlwaysRelevant;
	OutOperation.OnlyRelevantToOwner = Operation.OnlyRelevantToOwner;
	OutOperation.UseOwnerRelevancy = Operation.UseOwnerRelevancy;
	OutOperation.NetUpdateFrequency = Operation.NetUpdateFrequency;
	OutOperation.MinNetUpdateFrequency = Operation.MinNetUpdateFrequency;
	OutOperation.NetPriority = Operation.NetPriority;
	OutOperation.Dormancy = Operation.Dormancy;
	OutOperation.ReplicationCondition = Operation.ReplicationCondition;
	OutOperation.RepNotifyFunction = Operation.RepNotifyFunction;
	OutOperation.RpcMode = Operation.RpcMode;
	OutOperation.Reliable = Operation.Reliable;
	OutOperation.WithValidation = Operation.WithValidation;

	auto NoMember = [&]() { return Operation.MemberName.IsEmpty(); };
	auto NoReplicationFlags = [&]()
	{
		return Operation.Replicates == -1 && Operation.AlwaysRelevant == -1
			&& Operation.OnlyRelevantToOwner == -1 && Operation.UseOwnerRelevancy == -1;
	};
	auto NoRates = [&]()
	{
		return Operation.NetUpdateFrequency == -1.0
			&& Operation.MinNetUpdateFrequency == -1.0 && Operation.NetPriority == -1.0;
	};
	auto NoStrings = [&]()
	{
		return Operation.Dormancy.IsEmpty() && Operation.ReplicationCondition.IsEmpty()
			&& Operation.RepNotifyFunction.IsEmpty() && Operation.RpcMode.IsEmpty();
	};
	auto NoRpcFlags = [&]()
	{
		return Operation.Reliable == -1 && Operation.WithValidation == -1;
	};

	if (Operation.Type == TEXT("class.set_replication"))
	{
		if (!NoMember() || !IsTriState(Operation.Replicates)
			|| Operation.AlwaysRelevant != -1 || Operation.OnlyRelevantToOwner != -1
			|| Operation.UseOwnerRelevancy != -1 || !NoRates() || !NoStrings() || !NoRpcFlags())
			OutErrorCode = TEXT("invalid_class_replication_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication;
	}
	else if (Operation.Type == TEXT("class.set_relevancy"))
	{
		if (!NoMember() || Operation.Replicates != -1
			|| !IsTriState(Operation.AlwaysRelevant)
			|| !IsTriState(Operation.OnlyRelevantToOwner)
			|| !IsTriState(Operation.UseOwnerRelevancy)
			|| !NoRates() || !NoStrings() || !NoRpcFlags())
			OutErrorCode = TEXT("invalid_class_relevancy_shape");
		else if (Operation.AlwaysRelevant == 1 && Operation.OnlyRelevantToOwner == 1)
			OutErrorCode = TEXT("contradictory_relevancy_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetRelevancy;
	}
	else if (Operation.Type == TEXT("class.set_update_policy"))
	{
		if (!NoMember() || !NoReplicationFlags() || !NoStrings() || !NoRpcFlags()
			|| !FMath::IsFinite(Operation.NetUpdateFrequency)
			|| !FMath::IsFinite(Operation.MinNetUpdateFrequency)
			|| !FMath::IsFinite(Operation.NetPriority)
			|| Operation.NetUpdateFrequency <= 0.0 || Operation.NetUpdateFrequency > MaxFrequency
			|| Operation.MinNetUpdateFrequency < 0.0
			|| Operation.MinNetUpdateFrequency > Operation.NetUpdateFrequency
			|| Operation.NetPriority <= 0.0 || Operation.NetPriority > MaxPriority)
			OutErrorCode = TEXT("invalid_class_update_policy_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetUpdatePolicy;
	}
	else if (Operation.Type == TEXT("class.set_dormancy"))
	{
		if (!NoMember() || !NoReplicationFlags() || !NoRates()
			|| !IsDormancyToken(Operation.Dormancy)
			|| !Operation.ReplicationCondition.IsEmpty() || !Operation.RepNotifyFunction.IsEmpty()
			|| !Operation.RpcMode.IsEmpty() || !NoRpcFlags())
			OutErrorCode = TEXT("invalid_class_dormancy_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetDormancy;
	}
	else if (Operation.Type == TEXT("component.set_replicated"))
	{
		if (!IsNameToken(Operation.MemberName) || !IsTriState(Operation.Replicates)
			|| Operation.AlwaysRelevant != -1 || Operation.OnlyRelevantToOwner != -1
			|| Operation.UseOwnerRelevancy != -1 || !NoRates() || !NoStrings() || !NoRpcFlags())
			OutErrorCode = TEXT("invalid_component_replication_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkComponentSetReplicated;
	}
	else if (Operation.Type == TEXT("property.set_replication"))
	{
		const bool bNotifyValid = Operation.RepNotifyFunction.IsEmpty()
			|| IsNameToken(Operation.RepNotifyFunction);
		if (!IsNameToken(Operation.MemberName) || !IsTriState(Operation.Replicates)
			|| Operation.AlwaysRelevant != -1 || Operation.OnlyRelevantToOwner != -1
			|| Operation.UseOwnerRelevancy != -1 || !NoRates() || !Operation.Dormancy.IsEmpty()
			|| !Operation.RpcMode.IsEmpty() || !NoRpcFlags() || !bNotifyValid
			|| (Operation.Replicates == 1 && !IsLifetimeConditionToken(Operation.ReplicationCondition))
			|| (Operation.Replicates == 0
				&& (!Operation.ReplicationCondition.IsEmpty() || !Operation.RepNotifyFunction.IsEmpty())))
			OutErrorCode = TEXT("invalid_property_replication_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkPropertySetReplication;
	}
	else if (Operation.Type == TEXT("rpc.configure_existing"))
	{
		const bool bRpcMode = Operation.RpcMode == TEXT("server")
			|| Operation.RpcMode == TEXT("client") || Operation.RpcMode == TEXT("multicast");
		if (!IsNameToken(Operation.MemberName) || !NoReplicationFlags() || !NoRates()
			|| !Operation.Dormancy.IsEmpty() || !Operation.ReplicationCondition.IsEmpty()
			|| !Operation.RepNotifyFunction.IsEmpty() || !bRpcMode
			|| !IsTriState(Operation.Reliable) || !IsTriState(Operation.WithValidation)
			|| (Operation.WithValidation == 1 && Operation.RpcMode != TEXT("server")))
			OutErrorCode = TEXT("invalid_rpc_configuration_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkRpcConfigureExisting;
	}
	else
	{
		OutErrorCode = TEXT("unknown_operation_type");
	}
	if (!OutErrorCode.IsEmpty())
	{
		OutError = TEXT("Network operation violates its exact closed discriminated schema.");
		return false;
	}
	return true;
}

bool FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
	const FHyperAIGameFrameworkPlanOperation& Operation,
	FHyperAIStudioNetworkingBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Networking::Private;
	OutOperation = {};
	OutErrorCode.Reset();
	OutError.Reset();
	OutOperation.Domain = TEXT("game_framework");
	OutOperation.Type = Operation.Type;
	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.Role = Operation.Role;
	OutOperation.ParentClassPath = Operation.ParentClassPath;
	OutOperation.GameModeClassPath = Operation.GameModeClassPath;
	OutOperation.GameStateClassPath = Operation.GameStateClassPath;
	OutOperation.PlayerControllerClassPath = Operation.PlayerControllerClassPath;
	OutOperation.PlayerStateClassPath = Operation.PlayerStateClassPath;
	OutOperation.PawnClassPath = Operation.PawnClassPath;
	OutOperation.HUDClassPath = Operation.HUDClassPath;
	OutOperation.GameSessionClassPath = Operation.GameSessionClassPath;
	OutOperation.SpectatorClassPath = Operation.SpectatorClassPath;
	OutOperation.GameInstanceClassPath = Operation.GameInstanceClassPath;
	OutOperation.MaxPlayers = Operation.MaxPlayers;
	OutOperation.MaxSpectators = Operation.MaxSpectators;
	OutOperation.MaxSplitscreensPerConnection = Operation.MaxSplitscreensPerConnection;
	OutOperation.RequiresPushToTalk = Operation.RequiresPushToTalk;

	if (Operation.Type == TEXT("framework.create_class"))
	{
		FName CreatePackageName;
		if (!IsCanonicalProjectAssetPath(Operation.TargetPath) || !Operation.ExpectedRevision.IsEmpty()
			|| !TryGetProjectAssetPackageName(Operation.TargetPath, true, CreatePackageName)
			|| !IsFrameworkRoleToken(Operation.Role)
			|| !IsCanonicalTargetPath(Operation.ParentClassPath)
			|| !IsDefaultFrameworkClassRefs(Operation) || !IsDefaultFrameworkLimits(Operation))
			OutErrorCode = TEXT("invalid_framework_create_shape");
		else
		{
			OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::FrameworkCreateClass;
			OutOperation.bCreatesTarget = true;
		}
	}
	else if (Operation.Type == TEXT("project.set_defaults"))
	{
		const bool bHasChange = !Operation.GameModeClassPath.IsEmpty()
			|| !Operation.GameInstanceClassPath.IsEmpty();
		const bool bOnlyAllowedRefs = Operation.GameStateClassPath.IsEmpty()
			&& Operation.PlayerControllerClassPath.IsEmpty() && Operation.PlayerStateClassPath.IsEmpty()
			&& Operation.PawnClassPath.IsEmpty() && Operation.HUDClassPath.IsEmpty()
			&& Operation.GameSessionClassPath.IsEmpty() && Operation.SpectatorClassPath.IsEmpty();
		if (Operation.TargetPath != TEXT("project:game_maps_settings")
			|| !IsCanonicalSha256(Operation.ExpectedRevision) || !Operation.Role.IsEmpty()
			|| !Operation.ParentClassPath.IsEmpty() || !bHasChange || !bOnlyAllowedRefs
			|| (!Operation.GameModeClassPath.IsEmpty()
				&& !IsCanonicalTargetPath(Operation.GameModeClassPath))
			|| (!Operation.GameInstanceClassPath.IsEmpty()
				&& !IsCanonicalTargetPath(Operation.GameInstanceClassPath))
			|| !IsDefaultFrameworkLimits(Operation))
			OutErrorCode = TEXT("invalid_project_defaults_shape");
		else OutOperation.Kind = EHyperAIStudioNetworkingOperationKind::FrameworkProjectSetDefaults;
	}
	else if (Operation.Type == TEXT("world.set_game_mode_override"))
	{
		FHyperAIGameFrameworkPlanOperation Copy = Operation;
		Copy.GameModeClassPath.Reset();
		if (!IsCanonicalTargetPath(Operation.TargetPath, false)
			|| !IsCanonicalSha256(Operation.ExpectedRevision) || !Operation.Role.IsEmpty()
			|| !Operation.ParentClassPath.IsEmpty()
			|| !IsCanonicalTargetPath(Operation.GameModeClassPath)
			|| !IsDefaultFrameworkClassRefs(Copy) || !IsDefaultFrameworkLimits(Operation))
			OutErrorCode = TEXT("invalid_world_game_mode_shape");
		else OutOperation.Kind =
			EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride;
	}
	else if (Operation.Type == TEXT("game_mode.set_class_defaults"))
	{
		const bool bHasChange = !Operation.GameStateClassPath.IsEmpty()
			|| !Operation.PlayerControllerClassPath.IsEmpty() || !Operation.PlayerStateClassPath.IsEmpty()
			|| !Operation.PawnClassPath.IsEmpty() || !Operation.HUDClassPath.IsEmpty()
			|| !Operation.GameSessionClassPath.IsEmpty() || !Operation.SpectatorClassPath.IsEmpty();
		const bool bRefsValid = [&]()
		{
			const FString* Paths[] = {&Operation.GameStateClassPath, &Operation.PlayerControllerClassPath,
				&Operation.PlayerStateClassPath, &Operation.PawnClassPath, &Operation.HUDClassPath,
				&Operation.GameSessionClassPath, &Operation.SpectatorClassPath};
			for (const FString* Path : Paths)
				if (!Path->IsEmpty() && !IsCanonicalTargetPath(*Path)) return false;
			return true;
		}();
		if (!IsCanonicalProjectAssetPath(Operation.TargetPath)
			|| (!Operation.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Operation.ExpectedRevision))
			|| !Operation.Role.IsEmpty() || !Operation.ParentClassPath.IsEmpty()
			|| !Operation.GameModeClassPath.IsEmpty() || !Operation.GameInstanceClassPath.IsEmpty()
			|| !bHasChange || !bRefsValid || !IsDefaultFrameworkLimits(Operation))
			OutErrorCode = TEXT("invalid_game_mode_defaults_shape");
		else OutOperation.Kind =
			EHyperAIStudioNetworkingOperationKind::FrameworkGameModeSetClassDefaults;
	}
	else if (Operation.Type == TEXT("game_session.set_limits"))
	{
		const bool bAnyLimit = Operation.MaxPlayers >= 0 || Operation.MaxSpectators >= 0
			|| Operation.MaxSplitscreensPerConnection >= 0 || Operation.RequiresPushToTalk >= 0;
		if (!IsCanonicalProjectAssetPath(Operation.TargetPath)
			|| (!Operation.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Operation.ExpectedRevision))
			|| !Operation.Role.IsEmpty() || !Operation.ParentClassPath.IsEmpty()
			|| !IsDefaultFrameworkClassRefs(Operation) || !bAnyLimit
			|| Operation.MaxPlayers < -1 || Operation.MaxPlayers > MaxSessionPlayers
			|| Operation.MaxSpectators < -1 || Operation.MaxSpectators > MaxSessionPlayers
			|| Operation.MaxSplitscreensPerConnection < -1
			|| Operation.MaxSplitscreensPerConnection > 255
			|| (Operation.RequiresPushToTalk != -1
				&& !IsTriState(Operation.RequiresPushToTalk)))
			OutErrorCode = TEXT("invalid_game_session_limits_shape");
		else OutOperation.Kind =
			EHyperAIStudioNetworkingOperationKind::FrameworkGameSessionSetLimits;
	}
	else OutErrorCode = TEXT("unknown_operation_type");

	if (!OutErrorCode.IsEmpty())
	{
		OutError = TEXT("Game Framework operation violates its exact closed discriminated schema.");
		return false;
	}
	return true;
}

FString FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(
	FHyperAIStudioNetworkValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Networking::Private;
	if (!IsCanonicalSha256(Snapshot.ScopeFingerprint)) Snapshot.bComplete = false;
	Snapshot.Records.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
	for (int32 Index = 1; Index < Snapshot.Records.Num(); ++Index)
	{
		if (Snapshot.Records[Index].StableId == Snapshot.Records[Index - 1].StableId)
		{
			Snapshot.bComplete = false;
			Snapshot.Records[Index].bRevisionComplete = false;
			Snapshot.Records[Index - 1].bRevisionComplete = false;
		}
	}
	FString Aggregate;
	AppendToken(Aggregate, Snapshot.ScopeFingerprint);
	AppendBool(Aggregate, Snapshot.bComplete);
	AppendInt(Aggregate, Snapshot.Records.Num());
	for (FHyperAINetworkRecord& Record : Snapshot.Records)
	{
		if (IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
		{
			Snapshot.bComplete = false;
			Record.bRevisionComplete = false;
			break;
		}
		if (!Record.bRevisionComplete || Record.StableId.IsEmpty()
			|| Record.TargetPath.IsEmpty())
			Snapshot.bComplete = false;
		TSet<FString> ElementIds;
		for (const FHyperAINetworkElementView& Element : Record.Elements)
		{
			if (Element.StableId.IsEmpty() || Element.Name.IsEmpty()
				|| ElementIds.Contains(Element.StableId))
			{
				Record.bRevisionComplete = false;
				Snapshot.bComplete = false;
			}
			ElementIds.Add(Element.StableId);
		}
		if (!FMath::IsFinite(Record.NetUpdateFrequency)
			|| !FMath::IsFinite(Record.MinNetUpdateFrequency)
			|| !FMath::IsFinite(Record.NetPriority))
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			CanonicalNetworkRecord(Record));
		if (!IsCanonicalSha256(Record.Revision))
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		AppendToken(Aggregate, Record.StableId);
		AppendToken(Aggregate, Record.Revision);
		AppendBool(Aggregate, Record.bRevisionComplete);
	}
	AppendBool(Aggregate, Snapshot.bComplete);
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Aggregate);
	if (!IsCanonicalSha256(Snapshot.Revision)) Snapshot.bComplete = false;
	return Snapshot.Revision;
}

FString FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(
	FHyperAIStudioGameFrameworkValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Networking::Private;
	if (!IsCanonicalSha256(Snapshot.ScopeFingerprint)) Snapshot.bComplete = false;
	Snapshot.Records.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
	for (int32 Index = 1; Index < Snapshot.Records.Num(); ++Index)
	{
		if (Snapshot.Records[Index].StableId == Snapshot.Records[Index - 1].StableId)
		{
			Snapshot.bComplete = false;
			Snapshot.Records[Index].bRevisionComplete = false;
			Snapshot.Records[Index - 1].bRevisionComplete = false;
		}
	}
	FString Aggregate;
	AppendToken(Aggregate, Snapshot.ScopeFingerprint);
	AppendBool(Aggregate, Snapshot.bComplete);
	AppendInt(Aggregate, Snapshot.Records.Num());
	for (FHyperAIGameFrameworkRecord& Record : Snapshot.Records)
	{
		if (IsDeadlineExpired(Snapshot.WorkDeadlineSeconds))
		{
			Snapshot.bComplete = false;
			Record.bRevisionComplete = false;
			break;
		}
		if (!Record.bRevisionComplete || Record.StableId.IsEmpty()
			|| Record.TargetPath.IsEmpty())
			Snapshot.bComplete = false;
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			CanonicalFrameworkRecord(Record));
		if (!IsCanonicalSha256(Record.Revision))
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		AppendToken(Aggregate, Record.StableId);
		AppendToken(Aggregate, Record.Revision);
		AppendBool(Aggregate, Record.bRevisionComplete);
	}
	AppendBool(Aggregate, Snapshot.bComplete);
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Aggregate);
	if (!IsCanonicalSha256(Snapshot.Revision)) Snapshot.bComplete = false;
	return Snapshot.Revision;
}

TArray<FHyperAINetworkingIssue> FHyperAIStudioNetworkingContracts::ValidateNetworkSnapshot(
	const FHyperAIStudioNetworkValueSnapshot& Snapshot,
	const bool bRequirePackagesClean,
	const bool bRequirePIEMultiplayerEvidence,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Networking::Private;
	bOutTruncated = false;
	TArray<FHyperAINetworkingIssue> Issues;
	for (const auto& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (Issues.Num() >= MaxIssueCount) { bOutTruncated = true; break; }
		Issues.Add(CaptureIssue);
	}
	if (!Snapshot.bComplete || !IsCanonicalSha256(Snapshot.Revision))
		AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
			TEXT("incomplete_fresh_capture"), TEXT("error"), FString(), FString(), -1,
			TEXT("Fresh network capture or its deterministic revision is incomplete."));

	bool bHasPIENetworkWorld = false;
	bool bHasNetworkDriver = false;
	for (const FHyperAINetworkRecord& Record : Snapshot.Records)
	{
		if (Record.WorldPath.Contains(TEXT("UEDPIE_")) && Record.NetMode != TEXT("standalone"))
			bHasPIENetworkWorld = true;
		if (Record.Kind == TEXT("net_driver")) bHasNetworkDriver = true;
		if (!FMath::IsFinite(Record.NetUpdateFrequency)
			|| !FMath::IsFinite(Record.MinNetUpdateFrequency)
			|| !FMath::IsFinite(Record.NetPriority))
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
				TEXT("non_finite_network_numeric"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1, TEXT("Network frequency or priority is NaN/Inf."));
		if (bRequirePackagesClean && Record.bPackageDirty)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
				TEXT("dirty_package"), TEXT("error"), Record.TargetPath, Record.StableId, -1,
				TEXT("Target package is dirty while clean-package validation is required."));
		if (Record.bBlueprintCompileError)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
				TEXT("blueprint_compile_error"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1, TEXT("Blueprint compilation state is Error."));
		if (Record.Kind == TEXT("class_default") && !Record.AssetPath.IsEmpty()
			&& (!Record.bGeneratedClassCurrent
				|| !IsBlueprintCompileStatusEditable(Record.BlueprintCompileStatus)))
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
				TEXT("blueprint_not_current_or_compiled"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1,
				TEXT("Blueprint class projection is stale/transient or its compile state blocks edits."));
		if (Record.bOnDiskMetadata && !Record.bDetailsComplete)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
				TEXT("unloaded_metadata_only"), TEXT("info"), Record.TargetPath,
				Record.StableId, -1,
				TEXT("Cached on-disk metadata cannot prove unloaded RPC/default/member details."));
		if ((Record.Kind == TEXT("class_default") || Record.Kind == TEXT("actor_instance"))
			&& Record.bDetailsComplete)
		{
			if (Record.NetUpdateFrequency <= 0.0 || Record.NetUpdateFrequency > MaxFrequency
				|| Record.MinNetUpdateFrequency < 0.0
				|| Record.MinNetUpdateFrequency > Record.NetUpdateFrequency
				|| Record.NetPriority <= 0.0 || Record.NetPriority > MaxPriority)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("invalid_update_policy"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("Net update frequency/minimum/priority violates bounded invariants."));
			if (Record.bAlwaysRelevant && Record.bOnlyRelevantToOwner)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("contradictory_relevancy"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("AlwaysRelevant and OnlyRelevantToOwner are both enabled."));
			if (!Record.bReplicates && !Record.Dormancy.IsEmpty()
				&& Record.Dormancy != TEXT("never") && Record.Dormancy != TEXT("awake"))
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("dormancy_without_replication"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("Dormancy is configured on a non-replicating actor class/instance."));
			if (!Record.bReplicates && Record.ReplicatedComponentCount > 0)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("replicated_component_without_actor_replication"), TEXT("warning"),
					Record.TargetPath, Record.StableId, -1,
					TEXT("One or more default components replicate while the owner actor default does not."));
			if ((Record.bOnlyRelevantToOwner || Record.bUseOwnerRelevancy)
				&& Record.Kind == TEXT("actor_instance") && Record.OwnerPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("owner_relevancy_without_owner"), TEXT("warning"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("Owner-based relevancy is enabled but the live actor currently has no owner."));
		}
		TSet<FString> RepNotifyFunctions;
		for (const auto& Element : Record.Elements)
			if (Element.Kind == TEXT("rep_notify_function") && Element.bReplicated
				&& Element.bAuthoritativeDeclaration && Element.bSignatureValid
				&& Element.DeclarationSource == TEXT("function_graph")
				&& Element.OwnerClassPath == Record.ClassPath)
				RepNotifyFunctions.Add(Element.Name);
		for (const FHyperAINetworkElementView& Element : Record.Elements)
		{
			if (Element.Kind == TEXT("property") && Element.bReplicated)
			{
				if (Element.Condition == TEXT("unrecognized"))
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
						TEXT("invalid_property_replication_condition"), TEXT("error"),
						Record.TargetPath, Element.StableId, -1,
						TEXT("Replicated property has an invalid/unrecognized property condition."));
				if (!Element.RepNotifyFunction.IsEmpty()
					&& !RepNotifyFunctions.Contains(Element.RepNotifyFunction))
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
						TEXT("missing_rep_notify_function"), TEXT("error"), Record.TargetPath,
						Element.StableId, -1,
						TEXT("RepNotify references a function absent from the captured class hierarchy."));
			}
			else if (Element.Kind == TEXT("rpc"))
			{
				if (!Element.bAuthoritativeDeclaration || !Element.bSignatureValid
					|| Element.DeclarationSource != TEXT("custom_event")
					|| Element.OwnerClassPath != Record.ClassPath)
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
						TEXT("rpc_declaration_or_signature_invalid"), TEXT("error"),
						Record.TargetPath, Element.StableId, -1,
						TEXT("RPC must be an owner-bound Blueprint custom-event declaration with a valid no-return/no-out signature."));
				if (Element.RpcMode == TEXT("unspecified")
					|| Element.RpcMode == TEXT("conflicting") || Element.RpcMode.IsEmpty())
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
						TEXT("rpc_direction_missing"), TEXT("error"), Record.TargetPath,
						Element.StableId, -1,
						TEXT("Network function lacks exactly one server/client/multicast direction."));
				if (Element.bWithValidation && Element.RpcMode != TEXT("server"))
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
						TEXT("rpc_validation_direction_invalid"), TEXT("error"), Record.TargetPath,
						Element.StableId, -1,
						TEXT("RPC validation is only valid for a client-to-server RPC."));
			}
		}
		if (Record.Kind == TEXT("net_driver"))
		{
			if (Record.NetMode == TEXT("client") && !Record.bHasServerConnection)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("client_driver_without_server_connection"), TEXT("warning"),
					Record.TargetPath, Record.StableId, -1,
					TEXT("Client net driver has no observed server connection."));
			if ((Record.NetMode == TEXT("listen_server")
				|| Record.NetMode == TEXT("dedicated_server")) && Record.bHasServerConnection)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
					TEXT("server_driver_has_server_connection"), TEXT("error"),
					Record.TargetPath, Record.StableId, -1,
					TEXT("Server net driver unexpectedly reports a server connection."));
		}
	}
	if (bRequirePIEMultiplayerEvidence && (!bHasPIENetworkWorld || !bHasNetworkDriver))
		AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("network"),
			TEXT("pie_multiplayer_evidence_missing"), TEXT("error"), FString(), FString(), -1,
			TEXT("Required loaded PIE multiplayer world and net-driver evidence were not observed."));
	return Issues;
}

TArray<FHyperAINetworkingIssue>
FHyperAIStudioNetworkingContracts::ValidateGameFrameworkSnapshot(
	const FHyperAIStudioGameFrameworkValueSnapshot& Snapshot,
	const bool bRequirePackagesClean,
	const bool bRequirePIEWorldEvidence,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Networking::Private;
	bOutTruncated = false;
	TArray<FHyperAINetworkingIssue> Issues;
	for (const auto& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (Issues.Num() >= MaxIssueCount) { bOutTruncated = true; break; }
		Issues.Add(CaptureIssue);
	}
	if (!Snapshot.bComplete || !IsCanonicalSha256(Snapshot.Revision))
		AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
			TEXT("incomplete_fresh_capture"), TEXT("error"), FString(), FString(), -1,
			TEXT("Fresh Framework capture or its deterministic revision is incomplete."));
	bool bHasPIEWorld = false;
	for (const FHyperAIGameFrameworkRecord& Record : Snapshot.Records)
	{
		bHasPIEWorld |= Record.Kind == TEXT("world_runtime")
			&& Record.WorldPath.Contains(TEXT("UEDPIE_"));
		if (bRequirePackagesClean && Record.bPackageDirty)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
				TEXT("dirty_package"), TEXT("error"), Record.TargetPath, Record.StableId, -1,
				TEXT("Framework Blueprint package is dirty while clean-package validation is required."));
		if (Record.bBlueprintCompileError)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
				TEXT("blueprint_compile_error"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1, TEXT("Framework Blueprint compilation state is Error."));
		if (Record.Kind == TEXT("class_default") && !Record.AssetPath.IsEmpty()
			&& (!Record.bGeneratedClassCurrent
				|| !IsBlueprintCompileStatusEditable(Record.BlueprintCompileStatus)))
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
				TEXT("blueprint_not_current_or_compiled"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1,
				TEXT("Framework Blueprint projection is stale/transient or its compile state blocks edits."));
		if (Record.Kind == TEXT("world_settings")
			&& (Record.WorldPath.IsEmpty() || Record.WorldSettingsPath.IsEmpty()
				|| Record.PackageName.IsEmpty()))
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
				TEXT("world_settings_identity_incomplete"), TEXT("error"), Record.TargetPath,
				Record.StableId, -1,
				TEXT("WorldSettings CAS requires exact world, package, and WorldSettings object identity."));
		if (Record.bOnDiskMetadata && !Record.bDetailsComplete)
			AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
				TEXT("unloaded_metadata_only"), TEXT("info"), Record.TargetPath,
				Record.StableId, -1,
				TEXT("Cached class lineage cannot prove unloaded Framework class defaults."));
		if (Record.Kind == TEXT("project_settings"))
		{
			if (Record.GameDefaultMap.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("game_default_map_not_configured"), TEXT("warning"), Record.TargetPath,
					Record.StableId, -1, TEXT("Project settings have no explicit game default map."));
			else if (!FPackageName::IsValidLongPackageName(Record.GameDefaultMap))
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("invalid_game_default_map_path"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("Configured game default map is not one canonical long package name."));
			if (!Record.TransitionMap.IsEmpty()
				&& !FSoftObjectPath(Record.TransitionMap).IsValid())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("invalid_transition_map_path"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("Configured transition map is not one valid soft object path."));
			if (Record.GameModeClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("missing_global_game_mode"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1, TEXT("Project settings have no global default GameMode."));
			if (Record.GameInstanceClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("missing_game_instance_class"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1, TEXT("Project settings have no GameInstance class."));
			const FString ReferencePaths[] = {Record.GameModeClassPath,
				Record.GameInstanceClassPath};
			const FString ExpectedRoles[] = {TEXT("game_mode"), TEXT("game_instance")};
			for (int32 ReferenceIndex = 0; ReferenceIndex < 2; ++ReferenceIndex)
			{
				if (ReferencePaths[ReferenceIndex].IsEmpty()) continue;
				if (UClass* LoadedClass = FindLoadedClassNoLoad(ReferencePaths[ReferenceIndex]))
				{
					if (!IsLoadedRoleReference(ReferencePaths[ReferenceIndex],
						ExpectedRoles[ReferenceIndex]))
						AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
							TEXT("project_default_role_mismatch"), TEXT("error"),
							Record.TargetPath, Record.StableId, -1,
							TEXT("A loaded project default class does not derive from its required Framework role."));
					(void)LoadedClass;
				}
				else
					AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
						TEXT("project_default_role_unproven_no_load"), TEXT("warning"),
						Record.TargetPath, Record.StableId, -1,
						TEXT("An unloaded project default is reported by soft path only; no class was loaded to prove its role."));
			}
		}
		else if (Record.Kind == TEXT("class_default") && Record.Role == TEXT("game_mode")
			&& Record.bDetailsComplete)
		{
			if (Record.GameStateClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("game_mode_missing_game_state"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1, TEXT("GameMode default has no GameState class."));
			if (Record.PlayerControllerClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("game_mode_missing_player_controller"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1, TEXT("GameMode default has no PlayerController class."));
			if (Record.PlayerStateClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("game_mode_missing_player_state"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1, TEXT("GameMode default has no PlayerState class."));
			if (Record.PawnClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("game_mode_missing_default_pawn"), TEXT("warning"), Record.TargetPath,
					Record.StableId, -1, TEXT("GameMode default has no Pawn class."));
		}
		else if (Record.Kind == TEXT("class_default") && Record.Role == TEXT("game_session")
			&& Record.bDetailsComplete)
		{
			if (Record.MaxPlayers < 0 || Record.MaxPlayers > MaxSessionPlayers
				|| Record.MaxSpectators < 0 || Record.MaxSpectators > MaxSessionPlayers
				|| Record.MaxSplitscreensPerConnection < 0
				|| Record.MaxSplitscreensPerConnection > 255)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("invalid_game_session_limits"), TEXT("error"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("GameSession player/spectator/splitscreen limits are outside bounded ranges."));
		}
		else if (Record.Kind == TEXT("world_runtime"))
		{
			if (!Record.GameModeClassPath.IsEmpty() && Record.GameStateClassPath.IsEmpty())
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("runtime_game_state_missing"), TEXT("warning"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("World has an authority GameMode but no observed GameState."));
			if (Record.PlayerControllerCount > 0 && Record.PlayerStateCount == 0)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("runtime_player_states_missing"), TEXT("warning"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("World has PlayerControllers but no observed PlayerStates."));
		}
		else if (Record.Kind == TEXT("online_subsystem"))
		{
			if (!Record.bOnlineInterfaceObserved)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("online_interface_not_loaded"), TEXT("info"), Record.TargetPath,
					Record.StableId, -1,
					TEXT("No already-instantiated online engine interface was observed; none was created or loaded."));
			else if (!Record.OnlineSubsystemName.IsEmpty()
				&& Record.OnlineSubsystemName != TEXT("None") && !Record.bOnlineSubsystemLoaded)
				AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
					TEXT("configured_online_subsystem_not_loaded"), TEXT("warning"),
					Record.TargetPath, Record.StableId, -1,
					TEXT("An online provider name is configured but its already-loaded state is false."));
		}
	}
	if (bRequirePIEWorldEvidence && !bHasPIEWorld)
		AddIssue(Issues, MaxIssueCount, bOutTruncated, TEXT("game_framework"),
			TEXT("pie_framework_evidence_missing"), TEXT("error"), FString(), FString(), -1,
			TEXT("Required loaded PIE Framework world evidence was not observed."));
	return Issues;
}

namespace HyperAIStudio::Networking::Private
{
	void CanonicalBackendOperation(
		FString& Out,
		const FHyperAIStudioNetworkingBackendOperation& Operation)
	{
		AppendInt(Out, static_cast<uint8>(Operation.Kind));
		AppendToken(Out, Operation.Domain); AppendToken(Out, Operation.Type);
		AppendToken(Out, Operation.TargetPath); AppendToken(Out, Operation.ExpectedRevision);
		AppendToken(Out, Operation.MemberName); AppendToken(Out, Operation.Role);
		AppendToken(Out, Operation.ParentClassPath); AppendInt(Out, Operation.Replicates);
		AppendInt(Out, Operation.AlwaysRelevant); AppendInt(Out, Operation.OnlyRelevantToOwner);
		AppendInt(Out, Operation.UseOwnerRelevancy); AppendDouble(Out, Operation.NetUpdateFrequency);
		AppendDouble(Out, Operation.MinNetUpdateFrequency); AppendDouble(Out, Operation.NetPriority);
		AppendToken(Out, Operation.Dormancy); AppendToken(Out, Operation.ReplicationCondition);
		AppendToken(Out, Operation.RepNotifyFunction); AppendToken(Out, Operation.RpcMode);
		AppendInt(Out, Operation.Reliable); AppendInt(Out, Operation.WithValidation);
		AppendToken(Out, Operation.GameModeClassPath); AppendToken(Out, Operation.GameStateClassPath);
		AppendToken(Out, Operation.PlayerControllerClassPath);
		AppendToken(Out, Operation.PlayerStateClassPath); AppendToken(Out, Operation.PawnClassPath);
		AppendToken(Out, Operation.HUDClassPath); AppendToken(Out, Operation.GameSessionClassPath);
		AppendToken(Out, Operation.SpectatorClassPath); AppendToken(Out, Operation.GameInstanceClassPath);
		AppendInt(Out, Operation.MaxPlayers); AppendInt(Out, Operation.MaxSpectators);
		AppendInt(Out, Operation.MaxSplitscreensPerConnection);
		AppendInt(Out, Operation.RequiresPushToTalk); AppendBool(Out, Operation.bCreatesTarget);
	}

	bool HasExactBackendDiscriminant(const FHyperAIStudioNetworkingBackendOperation& Operation)
	{
		FHyperAIStudioNetworkingBackendOperation Rebuilt;
		FString Code;
		FString Error;
		bool bValid = false;
		if (Operation.Domain == TEXT("network"))
		{
			FHyperAINetworkPlanOperation Public;
			Public.Type = Operation.Type;
			Public.TargetPath = Operation.TargetPath;
			Public.ExpectedRevision = Operation.ExpectedRevision;
			Public.MemberName = Operation.MemberName;
			Public.Replicates = Operation.Replicates;
			Public.AlwaysRelevant = Operation.AlwaysRelevant;
			Public.OnlyRelevantToOwner = Operation.OnlyRelevantToOwner;
			Public.UseOwnerRelevancy = Operation.UseOwnerRelevancy;
			Public.NetUpdateFrequency = Operation.NetUpdateFrequency;
			Public.MinNetUpdateFrequency = Operation.MinNetUpdateFrequency;
			Public.NetPriority = Operation.NetPriority;
			Public.Dormancy = Operation.Dormancy;
			Public.ReplicationCondition = Operation.ReplicationCondition;
			Public.RepNotifyFunction = Operation.RepNotifyFunction;
			Public.RpcMode = Operation.RpcMode;
			Public.Reliable = Operation.Reliable;
			Public.WithValidation = Operation.WithValidation;
			bValid = FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
				Public, Rebuilt, Code, Error);
		}
		else if (Operation.Domain == TEXT("game_framework"))
		{
			FHyperAIGameFrameworkPlanOperation Public;
			Public.Type = Operation.Type;
			Public.TargetPath = Operation.TargetPath;
			Public.ExpectedRevision = Operation.ExpectedRevision;
			Public.Role = Operation.Role;
			Public.ParentClassPath = Operation.ParentClassPath;
			Public.GameModeClassPath = Operation.GameModeClassPath;
			Public.GameStateClassPath = Operation.GameStateClassPath;
			Public.PlayerControllerClassPath = Operation.PlayerControllerClassPath;
			Public.PlayerStateClassPath = Operation.PlayerStateClassPath;
			Public.PawnClassPath = Operation.PawnClassPath;
			Public.HUDClassPath = Operation.HUDClassPath;
			Public.GameSessionClassPath = Operation.GameSessionClassPath;
			Public.SpectatorClassPath = Operation.SpectatorClassPath;
			Public.GameInstanceClassPath = Operation.GameInstanceClassPath;
			Public.MaxPlayers = Operation.MaxPlayers;
			Public.MaxSpectators = Operation.MaxSpectators;
			Public.MaxSplitscreensPerConnection = Operation.MaxSplitscreensPerConnection;
			Public.RequiresPushToTalk = Operation.RequiresPushToTalk;
			bValid = FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
				Public, Rebuilt, Code, Error);
		}
		if (!bValid) return false;
		FString Expected;
		FString Actual;
		CanonicalBackendOperation(Expected, Rebuilt);
		CanonicalBackendOperation(Actual, Operation);
		return Expected == Actual;
	}

	const FHyperAINetworkRecord* FindNetworkRecord(
		const FHyperAIStudioNetworkValueSnapshot& Snapshot,
		const FString& Path)
	{
		return Snapshot.Records.FindByPredicate([&](const auto& Record)
		{
			return Record.TargetPath == Path || Record.AssetPath == Path || Record.ClassPath == Path;
		});
	}

	const FHyperAIGameFrameworkRecord* FindFrameworkRecord(
		const FHyperAIStudioGameFrameworkValueSnapshot& Snapshot,
		const FString& Path)
	{
		return Snapshot.Records.FindByPredicate([&](const auto& Record)
		{
			return Record.TargetPath == Path || Record.AssetPath == Path || Record.ClassPath == Path;
		});
	}

	const FHyperAIGameFrameworkRecord* FindFrameworkRecordByKind(
		const FHyperAIStudioGameFrameworkValueSnapshot& Snapshot,
		const FString& Path,
		const FString& Kind)
	{
		return Snapshot.Records.FindByPredicate([&](const auto& Record)
		{
			return Record.Kind == Kind
				&& (Record.TargetPath == Path || Record.AssetPath == Path || Record.ClassPath == Path);
		});
	}

	UClass* FindLoadedClassNoLoad(const FString& Path)
	{
		if (UClass* Class = FindObject<UClass>(nullptr, *Path))
		{
			if (!IsInspectableClass(Class)) return nullptr;
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
				return FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
					BlueprintStatus(Blueprint->Status)) ? Class : nullptr;
			return Class;
		}
		if (UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Path))
			return IsInspectableBlueprintClass(Blueprint, Blueprint->GeneratedClass)
				&& FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
					BlueprintStatus(Blueprint->Status))
				? Blueprint->GeneratedClass : nullptr;
		return nullptr;
	}

	bool IsLoadedRoleReference(const FString& Path, const FString& ExpectedRole)
	{
		UClass* Class = FindLoadedClassNoLoad(Path);
		if (!Class) return false;
		if (ExpectedRole == TEXT("pawn"))
			return Class->IsChildOf(APawn::StaticClass());
		return FrameworkRole(Class) == ExpectedRole;
	}

	struct FFinalizePlanInput
	{
		FString Domain;
		FString ToolName;
		FString VariantId;
		FString BaseRevision;
		bool bDryRun = true;
		FString OperationId;
		FString ExpectedPlanHash;
		int32 DeadlineMs = 2000;
		int32 MaxGameThreadMs = 250;
		int32 MaxOutputBytes = 65536;
		bool bCompileOnce = true;
		TArray<FHyperAIStudioNetworkingBackendOperation> Operations;
		FHyperAINetworkingPlanEffects Effects;
	};

	FHyperAINetworkingApplyPlanReport FinalizePlan(const FFinalizePlanInput& Input)
	{
		FHyperAINetworkingApplyPlanReport Report;
		Report.Domain = Input.Domain;
		Report.bDryRun = Input.bDryRun;
		Report.OperationId = Input.OperationId;
		Report.BaseRevision = Input.BaseRevision;
		Report.Effects = Input.Effects;

		const TSharedRef<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe>();
		Payload->Domain = Input.Domain;
		Payload->ToolName = Input.ToolName;
		Payload->Operations = Input.Operations;
		Payload->BaseRevision = Input.BaseRevision;
		Payload->SemanticFingerprint =
			FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
				Payload->Domain, Payload->ToolName, Payload->Operations, Payload->BaseRevision);
		if (!FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Payload->SemanticFingerprint))
		{
			Report.Status = TEXT("semantic_fingerprint_failed");
			Report.Diagnostic = TEXT("Closed typed plan exceeded the shared deterministic hash bound.");
			return Report;
		}
		const int32 PayloadBytes = Payload->GetBoundedByteSize();
		if (PayloadBytes <= 0 || PayloadBytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
		{
			Report.Status = TEXT("payload_exceeds_shared_bound");
			Report.Diagnostic = TEXT("Aggregate typed payload cannot be staged inside the shared 1 MiB request bound.");
			return Report;
		}
		const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		if (ProjectId.IsEmpty())
		{
			Report.Status = TEXT("canonical_project_identity_unavailable");
			Report.Diagnostic = TEXT("Shared typed-artifact preparation requires a proven current-project identity.");
			return Report;
		}

		const auto& Descriptor = FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioNetworkingContracts::PackId;
		Binding.ToolName = Input.ToolName;
		Binding.VariantId = Input.VariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = ProjectId;
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.Engine"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.EngineSettings"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.AssetRegistry"), EHyperAIStudioDomainPrerequisiteState::Available}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.bPackAdmitted = false;
		Binding.Admission.bEditAdmitted = false;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = FHyperAIStudioNetworkingContracts::PayloadTypeId;
		Contract.ArtifactSchemaFingerprint =
			FHyperAIStudioNetworkingContracts::PayloadSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
		Contract.EffectTarget = Input.Domain + TEXT(":") + Input.BaseRevision;
		Contract.DeadlineMs = Input.DeadlineMs;
		Contract.MaxNativeOperations = FMath::Clamp(Input.Operations.Num() + 4, 5, 128);
		Contract.MaxGameThreadMs = Input.MaxGameThreadMs;
		Contract.MaxOutputBytes = Input.MaxOutputBytes;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = FHyperAIStudioNetworkingContracts::StageLifetimeMs;
		Contract.bCompileOnce = Input.bCompileOnce;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		FHyperAIStudioPreparedTypedArtifact Prepared;
		FString PrepareError;
		if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
		{
			Report.Status = TEXT("typed_artifact_prepare_failed");
			Report.Diagnostic = Clip(PrepareError);
			return Report;
		}
		Report.PlanHash = Prepared.PlanHash;
		Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
		Report.CapabilityHash = Prepared.CapabilityHash;
		Report.EffectFingerprint = Prepared.EffectFingerprint;
		if (Input.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("valid_dry_run");
			Report.Diagnostic = TEXT("Closed typed plan, no-load references, member selectors, CAS, and shared artifact hashes are valid without mutation.");
			return Report;
		}
		if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Input.OperationId))
		{
			Report.Status = TEXT("invalid_operation_id");
			Report.Diagnostic = TEXT("Idempotent staging requires one journal-safe operation_id.");
			return Report;
		}
		if (Input.ExpectedPlanHash != Prepared.PlanHash
			|| !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Input.ExpectedPlanHash))
		{
			Report.Status = TEXT("expected_plan_hash_mismatch");
			Report.Diagnostic = TEXT("Staging must echo the exact dry-run plan hash.");
			return Report;
		}
		FHyperAIStudioNetworkingStagedArtifact Artifact;
		Artifact.Prepared = Prepared;
		Artifact.Payload = Payload;
		Artifact.CanonicalProjectId = ProjectId;
		Artifact.OperationId = Input.OperationId;
		Artifact.StageId = FHyperAIStudioNetworkingContracts::ComputeStageId(
			ProjectId, Input.OperationId, Prepared.PlanHash, Prepared.EffectFingerprint);
		Artifact.ExpiresMonotonicMs = NowMonotonicMs()
			+ FHyperAIStudioNetworkingContracts::StageLifetimeMs;
		FString StageError;
		if (!FHyperAIStudioNetworkingStagingService::Stage(Artifact, Report.bReplay, StageError))
		{
			Report.Status = TEXT("stage_rejected");
			Report.Diagnostic = Clip(StageError);
			return Report;
		}
		Report.bOk = false;
		Report.bStaged = true;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		Report.StageId = Artifact.StageId;
		Report.Status = TEXT("staged_backend_required");
		Report.Diagnostic = TEXT("Plan is idempotently staged and side-effect-free; no mutation is submitted until the shared async host owns an independent typed mutator, validator, journal, and fresh verification.");
		return Report;
	}

	int32 RemainingBudgetMs(const double DeadlineSeconds, const int32 RequestedMaximum)
	{
		const double Remaining = DeadlineSeconds - FPlatformTime::Seconds();
		if (Remaining <= 0.0) return 0;
		return FMath::Clamp(static_cast<int32>(Remaining * 1000.0), 1, RequestedMaximum);
	}

	bool AddPlanIssue(
		FHyperAINetworkingApplyPlanReport& Report,
		const TCHAR* Domain,
		const TCHAR* Code,
		const FString& Target,
		const int32 OperationIndex,
		const FString& Message)
	{
		bool bTruncated = Report.bTruncated;
		AddIssue(Report.Issues, FHyperAIStudioNetworkingContracts::MaxIssues, bTruncated,
			Domain, Code, TEXT("error"), Target, FString(), OperationIndex, Message);
		Report.bTruncated = bTruncated;
		Report.Status = Code;
		Report.Diagnostic = Clip(Message);
		return false;
	}
}

FString FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
	const FString& Domain,
	const FString& ToolName,
	const TArray<FHyperAIStudioNetworkingBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::Networking::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("networking_game_framework.payload.v1"));
	AppendToken(Canonical, Domain);
	AppendToken(Canonical, ToolName);
	AppendToken(Canonical, BaseRevision);
	AppendInt(Canonical, Operations.Num());
	for (const auto& Operation : Operations) CanonicalBackendOperation(Canonical, Operation);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioNetworkingContracts::ComputeStageId(
	const FString& CanonicalProjectId,
	const FString& OperationId,
	const FString& PlanHash,
	const FString& EffectFingerprint)
{
	using namespace HyperAIStudio::Networking::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("networking_game_framework.stage.v1"));
	AppendToken(Canonical, CanonicalProjectId);
	AppendToken(Canonical, OperationId);
	AppendToken(Canonical, PlanHash);
	AppendToken(Canonical, EffectFingerprint);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAINetworkingApplyPlanReport FHyperAIStudioNetworkingContracts::BuildNetworkPlan(
	const FHyperAINetworkingApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAINetworkingApplyPlanReport Failure;
	Failure.Domain = TEXT("network");
	Failure.bDryRun = Request.bDryRun;
	Failure.OperationId = Request.OperationId;
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 1
		|| Request.DeadlineMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousDeadlineMs
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.OperationId.Len() > MaxNameCharacters
		|| Request.ExpectedPlanHash.Len() > 71
		|| (!Request.OperationId.IsEmpty()
			&& !FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
		|| (!Request.ExpectedPlanHash.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPlanHash))
		|| (!Request.bDryRun
			&& (Request.OperationId.IsEmpty() || Request.ExpectedPlanHash.IsEmpty())))
	{
		Failure.Status = TEXT("invalid_request_bounds");
		Failure.Diagnostic = TEXT("Plan requires 1..64 operations, shared work/output bounds, and bounded valid staging identifiers when supplied.");
		return Failure;
	}
	const double PlanDeadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FHyperAIStudioNetworkingBackendOperation> Operations;
	TSet<FString> TargetSet;
	TSet<FString> OperationKeys;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioNetworkingBackendOperation Backend;
		FString Code;
		FString Error;
		if (!ValidateNetworkOperationShape(Request.Operations[Index], Backend, Code, Error))
		{
			AddPlanIssue(Failure, TEXT("network"), *Code,
				Request.Operations[Index].TargetPath, Index, Error);
			return Failure;
		}
		const FString Key = Backend.TargetPath + TEXT("|") + Backend.Type
			+ TEXT("|") + Backend.MemberName;
		if (OperationKeys.Contains(Key))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("duplicate_plan_operation"),
				Backend.TargetPath, Index,
				TEXT("The exact target/type/member operation occurs more than once."));
			return Failure;
		}
		OperationKeys.Add(Key);
		TargetSet.Add(Backend.TargetPath);
		Operations.Add(MoveTemp(Backend));
	}
	const int32 WorkMs = RemainingBudgetMs(PlanDeadline, Request.MaxGameThreadMs);
	if (WorkMs <= 0)
	{
		Failure.Status = TEXT("deadline_expired_before_capture");
		Failure.Diagnostic = TEXT("Plan deadline expired before fresh CAS capture.");
		return Failure;
	}
	TArray<FString> Targets = TargetSet.Array();
	Targets.Sort();
	FHyperAIStudioNetworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioNetworkingFacade::CaptureNetwork(
		Targets, TEXT("class_defaults"), true, false, Snapshot,
		CaptureStatus, CaptureDiagnostic, WorkMs))
	{
		Failure.Status = CaptureStatus;
		Failure.Diagnostic = CaptureDiagnostic;
		return Failure;
	}
	if (!Snapshot.bComplete || !IsCanonicalSha256(Snapshot.Revision))
	{
		Failure.Status = TEXT("incomplete_cas_capture");
		Failure.Diagnostic = TEXT("Every existing network target and member selector must be captured completely without loading.");
		return Failure;
	}

	TMap<FString, bool> FinalReplicates;
	TSet<FString> EnablingNetworkMembers;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const auto& Operation = Operations[Index];
		const FHyperAINetworkRecord* Record = FindNetworkRecord(Snapshot, Operation.TargetPath);
		if (!Record || Record->Kind != TEXT("class_default") || !Record->bDetailsComplete)
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("loaded_class_default_required"),
				Operation.TargetPath, Index,
				TEXT("Network mutation planning requires the exact already-loaded actor class default."));
			return Failure;
		}
		if (!Record->bGeneratedClassCurrent
			|| !IsBlueprintCompileStatusEditable(Record->BlueprintCompileStatus))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("blueprint_not_current_or_compiled"),
				Operation.TargetPath, Index,
				TEXT("Network edits require the Blueprint's current GeneratedClass in UpToDate or UpToDateWithWarnings state."));
			return Failure;
		}
		if (Operation.ExpectedRevision != Record->Revision)
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("target_revision_mismatch"),
				Operation.TargetPath, Index,
				TEXT("Expected target revision does not match the fresh no-load class snapshot."));
			return Failure;
		}
		FinalReplicates.FindOrAdd(Operation.TargetPath, Record->bReplicates);
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkComponentSetReplicated
			&& !HasAuthoritativeDeclaredSelector(*Record, TEXT("component"), TEXT("component"),
				Operation.MemberName, TEXT("scs"), true))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("component_selector_missing"),
				Operation.TargetPath, Index,
				TEXT("Named default component was not present in the exact inspector projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkPropertySetReplication
			&& !HasAuthoritativeDeclaredSelector(*Record, TEXT("property"), TEXT("property"),
				Operation.MemberName, TEXT("new_variable"), true))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("property_selector_missing"),
				Operation.TargetPath, Index,
				TEXT("Named declared property was not present in the exact inspector projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkPropertySetReplication
			&& !Operation.RepNotifyFunction.IsEmpty()
			&& !HasAuthoritativeDeclaredSelector(*Record, TEXT("rep_notify_function"),
				TEXT("rep_notify_function"), Operation.RepNotifyFunction,
				TEXT("function_graph"), true))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("rep_notify_selector_missing"),
				Operation.TargetPath, Index,
				TEXT("Named RepNotify function was not present in the exact declared-function projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkRpcConfigureExisting
			&& !HasAuthoritativeDeclaredSelector(*Record, TEXT("function"), TEXT("rpc"),
				Operation.MemberName, TEXT("custom_event"), true))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("function_selector_missing"),
				Operation.TargetPath, Index,
				TEXT("Named existing function was not present in the exact inspector projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication)
			FinalReplicates[Operation.TargetPath] = Operation.Replicates == 1;
		if ((Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkComponentSetReplicated
				|| Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkPropertySetReplication)
			&& Operation.Replicates == 1)
			EnablingNetworkMembers.Add(Operation.TargetPath);
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::NetworkRpcConfigureExisting)
			EnablingNetworkMembers.Add(Operation.TargetPath);
	}
	TArray<FString> SortedEnablingTargets = EnablingNetworkMembers.Array();
	SortedEnablingTargets.Sort();
	for (const FString& Target : SortedEnablingTargets)
	{
		if (!FinalReplicates.FindRef(Target))
		{
			AddPlanIssue(Failure, TEXT("network"), TEXT("network_member_without_actor_replication"),
				Target, -1,
				TEXT("Final symbolic plan enables an RPC/property/component while actor replication is disabled."));
			return Failure;
		}
	}
	if (IsDeadlineExpired(PlanDeadline))
	{
		Failure.Status = TEXT("deadline_expired_during_validation");
		Failure.Diagnostic = TEXT("Plan deadline expired during selector/precondition validation.");
		return Failure;
	}

	FHyperAINetworkingPlanEffects Effects;
	Effects.OperationCount = Operations.Num();
	Effects.TargetCount = TargetSet.Num();
	for (const auto& Operation : Operations)
	{
		switch (Operation.Kind)
		{
		case EHyperAIStudioNetworkingOperationKind::NetworkPropertySetReplication:
			++Effects.PropertyChanges; break;
		case EHyperAIStudioNetworkingOperationKind::NetworkRpcConfigureExisting:
			++Effects.RpcChanges; break;
		case EHyperAIStudioNetworkingOperationKind::NetworkComponentSetReplicated:
			++Effects.ComponentChanges; break;
		default: ++Effects.ClassDefaultChanges; break;
		}
	}
	Effects.bTransactionOnce = true;
	Effects.bCompileOnce = true;
	Effects.bSaveOnce = true;
	Effects.bValidateOnce = true;
	Effects.bFreshVerifyOnce = true;
	FFinalizePlanInput Input;
	Input.Domain = TEXT("network");
	Input.ToolName = TEXT("hyper_network_apply_plan");
	Input.VariantId = NetworkMutationVariantId;
	Input.BaseRevision = Snapshot.Revision;
	Input.bDryRun = Request.bDryRun;
	Input.OperationId = Request.OperationId;
	Input.ExpectedPlanHash = Request.ExpectedPlanHash;
	Input.DeadlineMs = Request.DeadlineMs;
	Input.MaxGameThreadMs = Request.MaxGameThreadMs;
	Input.MaxOutputBytes = Request.MaxOutputBytes;
	Input.bCompileOnce = true;
	Input.Operations = MoveTemp(Operations);
	Input.Effects = Effects;
	return FinalizePlan(Input);
}

FHyperAINetworkingApplyPlanReport FHyperAIStudioNetworkingContracts::BuildGameFrameworkPlan(
	const FHyperAIGameFrameworkApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAINetworkingApplyPlanReport Failure;
	Failure.Domain = TEXT("game_framework");
	Failure.bDryRun = Request.bDryRun;
	Failure.OperationId = Request.OperationId;
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 1
		|| Request.DeadlineMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousDeadlineMs
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.OperationId.Len() > MaxNameCharacters
		|| Request.ExpectedPlanHash.Len() > 71
		|| (!Request.OperationId.IsEmpty()
			&& !FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
		|| (!Request.ExpectedPlanHash.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPlanHash))
		|| (!Request.bDryRun
			&& (Request.OperationId.IsEmpty() || Request.ExpectedPlanHash.IsEmpty())))
	{
		Failure.Status = TEXT("invalid_request_bounds");
		Failure.Diagnostic = TEXT("Plan requires 1..64 operations, shared work/output bounds, and bounded valid staging identifiers when supplied.");
		return Failure;
	}
	const double PlanDeadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	TArray<FHyperAIStudioNetworkingBackendOperation> Operations;
	TSet<FString> TargetSet;
	TSet<FString> OperationKeys;
	TMap<FString, FString> CreatedRoles;
	TMap<FString, int32> CreatedAt;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioNetworkingBackendOperation Backend;
		FString Code;
		FString Error;
		if (!ValidateGameFrameworkOperationShape(Request.Operations[Index], Backend, Code, Error))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), *Code,
				Request.Operations[Index].TargetPath, Index, Error);
			return Failure;
		}
		const FString Key = Backend.TargetPath + TEXT("|") + Backend.Type;
		if (OperationKeys.Contains(Key))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("duplicate_plan_operation"),
				Backend.TargetPath, Index,
				TEXT("The exact target/type operation occurs more than once."));
			return Failure;
		}
		OperationKeys.Add(Key);
		TargetSet.Add(Backend.TargetPath);
		if (Backend.bCreatesTarget)
		{
			if (CreatedRoles.Contains(Backend.TargetPath))
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("duplicate_create_target"),
					Backend.TargetPath, Index, TEXT("A class target may be created only once."));
				return Failure;
			}
			CreatedRoles.Add(Backend.TargetPath, Backend.Role);
			CreatedAt.Add(Backend.TargetPath, Index);
		}
		Operations.Add(MoveTemp(Backend));
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (!CreatedRoles.IsEmpty() && !AssetRegistry)
	{
		AddPlanIssue(Failure, TEXT("game_framework"), TEXT("asset_registry_unavailable"),
			FString(), -1,
			TEXT("Create-target absence requires an already initialized Asset Registry."));
		return Failure;
	}
	if (!CreatedRoles.IsEmpty()
		&& (AssetRegistry->IsGathering() || AssetRegistry->IsLoadingAssets()))
	{
		AddPlanIssue(Failure, TEXT("game_framework"), TEXT("asset_registry_incomplete"),
			FString(), -1,
			TEXT("Create-target absence cannot be proven while Asset Registry discovery is incomplete."));
		return Failure;
	}
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const auto& Operation = Operations[Index];
		if (!Operation.bCreatesTarget) continue;
		if (IsDeadlineExpired(PlanDeadline))
		{
			AddPlanIssue(Failure, TEXT("game_framework"),
				TEXT("deadline_expired_before_absence_check"), Operation.TargetPath, Index,
				TEXT("Plan deadline expired before nonblocking create-target absence proof."));
			return Failure;
		}
		if (FindObject<UObject>(nullptr, *Operation.TargetPath)
			|| FindObject<UClass>(nullptr, *(Operation.TargetPath + TEXT("_C"))))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("create_target_already_loaded"),
				Operation.TargetPath, Index, TEXT("Create target already exists in loaded memory."));
			return Failure;
		}
		FName PackageName;
		if (!TryGetProjectAssetPackageName(Operation.TargetPath, true, PackageName))
		{
			AddPlanIssue(Failure, TEXT("game_framework"),
				TEXT("invalid_create_primary_asset_name"), Operation.TargetPath, Index,
				TEXT("Create target object name must match its package short name."));
			return Failure;
		}
		FAssetPackageData ExistingData;
		const UE::AssetRegistry::EExists Exists = AssetRegistry->TryGetAssetPackageData(
			PackageName, ExistingData, true /* bFailIfLockHeld */);
		if (Exists != UE::AssetRegistry::EExists::DoesNotExist)
		{
			AddPlanIssue(Failure, TEXT("game_framework"),
				Exists == UE::AssetRegistry::EExists::Exists
					? TEXT("create_target_exists_on_disk") : TEXT("create_target_absence_unknown"),
				Operation.TargetPath, Index,
				TEXT("Create target package must be proven absent by a nonblocking initialized Asset Registry query without loading it."));
			return Failure;
		}
		if (const FString* SamePlanParentRole = CreatedRoles.Find(Operation.ParentClassPath))
		{
			if (CreatedAt.FindRef(Operation.ParentClassPath) >= Index
				|| *SamePlanParentRole != Operation.Role)
			{
				AddPlanIssue(Failure, TEXT("game_framework"),
					TEXT("same_plan_parent_role_or_order_mismatch"), Operation.TargetPath, Index,
					TEXT("A same-plan parent must be created earlier with the exact same Framework role."));
				return Failure;
			}
		}
		else
		{
			UClass* ParentClass = FindLoadedClassNoLoad(Operation.ParentClassPath);
			UClass* ExpectedBase = ExpectedBaseClass(Operation.Role);
			if (!ParentClass || !ExpectedBase || !ParentClass->IsChildOf(ExpectedBase)
				|| FrameworkRole(ParentClass) != Operation.Role)
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("loaded_parent_role_mismatch"),
					Operation.TargetPath, Index,
					TEXT("Create parent must already be loaded and derive from the exact closed Framework role base."));
				return Failure;
			}
		}
	}

	TMap<FString, FString> RequiredReferences;
	FString ConflictingReferencePath;
	auto AddReference = [&](const FString& Path, const TCHAR* Role)
	{
		if (Path.IsEmpty()) return;
		if (const FString* ExistingRole = RequiredReferences.Find(Path))
		{
			if (*ExistingRole != Role) ConflictingReferencePath = Path;
			return;
		}
		RequiredReferences.Add(Path, Role);
	};
	for (const auto& Operation : Operations)
	{
		if (Operation.bCreatesTarget) AddReference(Operation.ParentClassPath, *Operation.Role);
		AddReference(Operation.GameModeClassPath, TEXT("game_mode"));
		AddReference(Operation.GameStateClassPath, TEXT("game_state"));
		AddReference(Operation.PlayerControllerClassPath, TEXT("player_controller"));
		AddReference(Operation.PlayerStateClassPath, TEXT("player_state"));
		AddReference(Operation.PawnClassPath, TEXT("pawn"));
		AddReference(Operation.HUDClassPath, TEXT("hud"));
		AddReference(Operation.GameSessionClassPath, TEXT("game_session"));
		AddReference(Operation.SpectatorClassPath, TEXT("spectator_pawn"));
		AddReference(Operation.GameInstanceClassPath, TEXT("game_instance"));
	}
	if (!ConflictingReferencePath.IsEmpty())
	{
		AddPlanIssue(Failure, TEXT("game_framework"), TEXT("reference_role_conflict"),
			ConflictingReferencePath, -1,
			TEXT("One class path cannot satisfy two different closed Framework roles in the same plan."));
		return Failure;
	}
	TArray<FString> SortedReferencePaths;
	RequiredReferences.GetKeys(SortedReferencePaths);
	SortedReferencePaths.Sort();
	for (const FString& ReferencePath : SortedReferencePaths)
	{
		const FString& RequiredRole = RequiredReferences.FindChecked(ReferencePath);
		if (const FString* CreatedRole = CreatedRoles.Find(ReferencePath))
		{
			if (*CreatedRole != RequiredRole && !(RequiredRole == TEXT("pawn")
				&& *CreatedRole == TEXT("spectator_pawn")))
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("same_plan_reference_role_mismatch"),
					ReferencePath, -1, TEXT("Same-plan class reference does not match the required role."));
				return Failure;
			}
		}
		else if (!IsLoadedRoleReference(ReferencePath, RequiredRole))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("loaded_reference_required"),
				ReferencePath, -1,
				TEXT("Referenced Framework classes must already be loaded, or be created earlier in the same closed plan."));
			return Failure;
		}
	}

	TArray<FString> CapturePaths;
	for (const auto& Operation : Operations)
	{
		if (!Operation.bCreatesTarget && !CreatedRoles.Contains(Operation.TargetPath))
			CapturePaths.AddUnique(Operation.TargetPath);
	}
	for (const FString& ReferencePath : SortedReferencePaths)
		if (!CreatedRoles.Contains(ReferencePath)) CapturePaths.AddUnique(ReferencePath);
	FHyperAIStudioGameFrameworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	const bool bOnlyWorldSettingsEdits = Operations.ContainsByPredicate([](const auto& Operation)
	{
		return Operation.Kind ==
			EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride;
	}) && !Operations.ContainsByPredicate([](const auto& Operation)
	{
		return Operation.Kind !=
			EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride;
	});
	const FString FreshCaptureScope = bOnlyWorldSettingsEdits
		? TEXT("world_settings") : TEXT("all");
	const int32 Remaining = RemainingBudgetMs(PlanDeadline, Request.MaxGameThreadMs);
	if (Remaining <= 0 || !FHyperAIStudioNetworkingFacade::CaptureGameFramework(
		CapturePaths, FreshCaptureScope, true, false, Snapshot,
		CaptureStatus, CaptureDiagnostic, Remaining) || !Snapshot.bComplete)
	{
		Failure.Status = Remaining <= 0 ? TEXT("deadline_expired_before_capture")
			: TEXT("fresh_capture_incomplete");
		Failure.Diagnostic = Remaining <= 0
			? TEXT("Plan deadline expired before the required fresh Framework capture.")
			: Clip(CaptureDiagnostic);
		Failure.BaseRevision = Snapshot.Revision;
		return Failure;
	}
	Failure.BaseRevision = Snapshot.Revision;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const auto& Operation = Operations[Index];
		const FString* OperationReferences[] = {&Operation.GameModeClassPath,
			&Operation.GameStateClassPath, &Operation.PlayerControllerClassPath,
			&Operation.PlayerStateClassPath, &Operation.PawnClassPath, &Operation.HUDClassPath,
			&Operation.GameSessionClassPath, &Operation.SpectatorClassPath,
			&Operation.GameInstanceClassPath};
		for (const FString* Reference : OperationReferences)
		{
			if (!Reference->IsEmpty() && CreatedAt.Contains(*Reference)
				&& CreatedAt.FindRef(*Reference) >= Index)
			{
				AddPlanIssue(Failure, TEXT("game_framework"),
					TEXT("same_plan_reference_order_invalid"), *Reference, Index,
					TEXT("A same-plan Framework class must be created before it is referenced."));
				return Failure;
			}
		}
		if (Operation.bCreatesTarget) continue;
		if (CreatedRoles.Contains(Operation.TargetPath))
		{
			if (CreatedAt.FindRef(Operation.TargetPath) >= Index)
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("same_plan_create_order_invalid"),
					Operation.TargetPath, Index,
					TEXT("A same-plan Framework class must be created before it is configured."));
				return Failure;
			}
			if (!Operation.ExpectedRevision.IsEmpty())
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("same_plan_target_revision_must_be_empty"),
					Operation.TargetPath, Index,
					TEXT("A same-plan created class has no pre-existing revision."));
				return Failure;
			}
			const FString CreatedRole = CreatedRoles.FindRef(Operation.TargetPath);
			const bool bCompatible =
				(Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameModeSetClassDefaults
					&& CreatedRole == TEXT("game_mode"))
				|| (Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameSessionSetLimits
					&& CreatedRole == TEXT("game_session"));
			if (!bCompatible)
			{
				AddPlanIssue(Failure, TEXT("game_framework"), TEXT("same_plan_target_role_mismatch"),
					Operation.TargetPath, Index,
					TEXT("Same-plan configuration target does not match the operation role."));
				return Failure;
			}
			continue;
		}
		const FHyperAIGameFrameworkRecord* Record =
			Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride
			? FindFrameworkRecordByKind(Snapshot, Operation.TargetPath, TEXT("world_settings"))
			: FindFrameworkRecord(Snapshot, Operation.TargetPath);
		if (!Record || !Record->bLoaded || !Record->bDetailsComplete || !Record->bRevisionComplete)
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("loaded_framework_target_required"),
				Operation.TargetPath, Index,
				TEXT("Mutating an existing Framework target requires exact loaded defaults and a complete record revision."));
			return Failure;
		}
		if (Operation.ExpectedRevision != Record->Revision)
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("target_revision_mismatch"),
				Operation.TargetPath, Index,
				TEXT("Operation expected_revision does not match the fresh target record revision."));
			return Failure;
		}
		if ((Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameModeSetClassDefaults
				|| Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameSessionSetLimits)
			&& (!Record->bGeneratedClassCurrent
				|| !IsBlueprintCompileStatusEditable(Record->BlueprintCompileStatus)))
		{
			AddPlanIssue(Failure, TEXT("game_framework"),
				TEXT("blueprint_not_current_or_compiled"), Operation.TargetPath, Index,
				TEXT("Framework Blueprint edits require the current GeneratedClass in UpToDate or UpToDateWithWarnings state."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameModeSetClassDefaults
			&& Record->Role != TEXT("game_mode"))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("game_mode_target_role_mismatch"),
				Operation.TargetPath, Index, TEXT("Target is not a loaded GameMode class default."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkProjectSetDefaults
			&& Record->Kind != TEXT("project_settings"))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("project_settings_target_mismatch"),
				Operation.TargetPath, Index,
				TEXT("Target is not the exact loaded GameMapsSettings projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride
			&& Record->Kind != TEXT("world_settings"))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("world_target_mismatch"),
				Operation.TargetPath, Index,
				TEXT("Target is not the exact loaded WorldSettings override projection."));
			return Failure;
		}
		if (Operation.Kind == EHyperAIStudioNetworkingOperationKind::FrameworkGameSessionSetLimits
			&& Record->Role != TEXT("game_session"))
		{
			AddPlanIssue(Failure, TEXT("game_framework"), TEXT("game_session_target_role_mismatch"),
				Operation.TargetPath, Index, TEXT("Target is not a loaded GameSession class default."));
			return Failure;
		}
	}
	if (IsDeadlineExpired(PlanDeadline))
	{
		Failure.Status = TEXT("deadline_expired_during_validation");
		Failure.Diagnostic = TEXT("Plan deadline expired during Framework precondition validation.");
		return Failure;
	}

	FHyperAINetworkingPlanEffects Effects;
	Effects.OperationCount = Operations.Num();
	Effects.TargetCount = TargetSet.Num();
	for (const auto& Operation : Operations)
	{
		switch (Operation.Kind)
		{
		case EHyperAIStudioNetworkingOperationKind::FrameworkCreateClass:
			++Effects.ClassesCreated; break;
		case EHyperAIStudioNetworkingOperationKind::FrameworkProjectSetDefaults:
			++Effects.ProjectSettingChanges; break;
		case EHyperAIStudioNetworkingOperationKind::FrameworkWorldSetGameModeOverride:
			++Effects.WorldSettingChanges; break;
		case EHyperAIStudioNetworkingOperationKind::FrameworkGameSessionSetLimits:
			++Effects.SessionChanges; break;
		default: ++Effects.ClassDefaultChanges; break;
		}
	}
	Effects.bTransactionOnce = true;
	Effects.bCompileOnce = true;
	Effects.bSaveOnce = true;
	Effects.bValidateOnce = true;
	Effects.bFreshVerifyOnce = true;
	FFinalizePlanInput Input;
	Input.Domain = TEXT("game_framework");
	Input.ToolName = TEXT("hyper_game_framework_apply_plan");
	Input.VariantId = FrameworkMutationVariantId;
	Input.BaseRevision = Snapshot.Revision;
	Input.bDryRun = Request.bDryRun;
	Input.OperationId = Request.OperationId;
	Input.ExpectedPlanHash = Request.ExpectedPlanHash;
	Input.DeadlineMs = Request.DeadlineMs;
	Input.MaxGameThreadMs = Request.MaxGameThreadMs;
	Input.MaxOutputBytes = Request.MaxOutputBytes;
	Input.bCompileOnce = true;
	Input.Operations = MoveTemp(Operations);
	Input.Effects = Effects;
	return FinalizePlan(Input);
}

namespace HyperAIStudio::Networking::Private
{
	int64 EstimateIssueBytes(const FHyperAINetworkingIssue& Issue)
	{
		return 512 + EscapedStringUpperBound(Issue.Domain) + EscapedStringUpperBound(Issue.Code)
			+ EscapedStringUpperBound(Issue.Severity) + EscapedStringUpperBound(Issue.TargetPath)
			+ EscapedStringUpperBound(Issue.StableId) + EscapedStringUpperBound(Issue.Message);
	}

	int64 EstimateVariantBytes(const FHyperAINetworkingVariantStatus& Status)
	{
		int64 Bytes = 1024 + EscapedStringUpperBound(Status.Domain)
			+ EscapedStringUpperBound(Status.Variant) + EscapedStringUpperBound(Status.State)
			+ EscapedStringUpperBound(Status.Remediation);
		for (const FString& Value : Status.RequiredModules) Bytes += EscapedStringUpperBound(Value) + 32;
		for (const FString& Value : Status.SupportedCases) Bytes += EscapedStringUpperBound(Value) + 32;
		for (const FString& Value : Status.DelegatedEpicCases) Bytes += EscapedStringUpperBound(Value) + 32;
		for (const FString& Value : Status.UnsupportedCases) Bytes += EscapedStringUpperBound(Value) + 32;
		return Bytes;
	}

	bool AddBoundedVariants(
		const FString& Domain,
		const int32 MaxOutputBytes,
		int64& InOutBytes,
		bool& bOutTruncated,
		TArray<FHyperAINetworkingVariantStatus>& OutVariants)
	{
		for (const auto& Status : FHyperAIStudioNetworkingFacade::ResolveVariantStatuses(Domain))
		{
			const int64 Bytes = EstimateVariantBytes(Status);
			if (InOutBytes + Bytes > MaxOutputBytes)
			{
				bOutTruncated = true;
				return false;
			}
			InOutBytes += Bytes;
			OutVariants.Add(Status);
		}
		return true;
	}
}

FHyperAINetworkInspectReport UHyperAIStudioNetworkingToolset::hyper_network_inspect(
	const FHyperAINetworkInspectRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAINetworkInspectReport Report;
	if (Request.TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths
		|| Request.Scope.Len() > 32 || Request.Projection.Len() > 16
		|| (Request.Projection != TEXT("summary") && Request.Projection != TEXT("details"))
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioNetworkingContracts::MaxPageSize
		|| Request.Cursor.Len() > FHyperAIStudioNetworkingContracts::MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioNetworkingContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Network inspect request exceeds projection, target, page, cursor, work, or output bounds.");
		return Report;
	}
	int32 Offset = 0;
	FString CursorRevision;
	if (!ParseCursor(Request.Cursor, CursorRevision, Offset))
	{
		Report.Status = TEXT("invalid_cursor");
		Report.Diagnostic = TEXT("Cursor must be empty or bind one exact snapshot revision and offset.");
		return Report;
	}
	FHyperAIStudioNetworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	// Projection is output-only: capture the same semantic member set so summary and details
	// pages share record/snapshot CAS identities.
	if (!FHyperAIStudioNetworkingFacade::CaptureNetwork(Request.TargetPaths, Request.Scope,
		true, Request.bIncludeOnDiskMetadata,
		Snapshot, CaptureStatus, CaptureDiagnostic, Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		if (Request.bIncludeVariants)
		{
			int64 Bytes = 1024;
			AddBoundedVariants(TEXT("network"), Request.MaxOutputBytes, Bytes,
				Report.bTruncated, Report.Variants);
		}
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.ObjectsScanned = Snapshot.ObjectsScanned;
	Report.TotalRecords = Snapshot.Records.Num();
	if (!CursorRevision.IsEmpty() && CursorRevision != Snapshot.Revision)
	{
		Report.Status = TEXT("stale_cursor_revision");
		Report.Diagnostic = TEXT("Network state changed between pages; restart with an empty cursor.");
		return Report;
	}
	if (Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("cursor_out_of_range");
		Report.Diagnostic = TEXT("Cursor offset is beyond the deterministic result set.");
		return Report;
	}
	int64 EstimatedBytes = 2048;
	for (const auto& Issue : Snapshot.CaptureIssues)
	{
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	bool bRecordExceeded = false;
	for (int32 Index = Offset; Index < End; ++Index)
	{
		FHyperAINetworkRecord Record = Snapshot.Records[Index];
		bool bDetails = Request.Projection == TEXT("details");
		if (!bDetails)
		{
			Record.bOutputDetailsTruncated = !Record.Elements.IsEmpty();
			Record.Elements.Reset();
		}
		int64 Bytes = EstimateNetworkRecordBytes(Record, bDetails);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes && !Record.Elements.IsEmpty())
		{
			Record.bOutputDetailsTruncated = true;
			Record.Elements.Reset();
			bDetails = false;
			Bytes = EstimateNetworkRecordBytes(Record, false);
			Report.bTruncated = true;
		}
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			bRecordExceeded = true;
			break;
		}
		EstimatedBytes += Bytes;
		Report.Records.Add(MoveTemp(Record));
	}
	Report.ReturnedRecords = Report.Records.Num();
	if (bRecordExceeded && Report.ReturnedRecords == 0 && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("record_exceeds_output_budget");
		Report.Diagnostic = TEXT("The next base network record cannot fit the output budget after detail trimming.");
		Report.NextCursor.Reset();
		return Report;
	}
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	if (NextOffset < Snapshot.Records.Num())
	{
		Report.bTruncated = true;
		if (Snapshot.bComplete) Report.NextCursor = MakeCursor(Snapshot.Revision, NextOffset);
	}
	if (Request.bIncludeVariants)
		AddBoundedVariants(TEXT("network"), Request.MaxOutputBytes, EstimatedBytes,
			Report.bTruncated, Report.Variants);
	Report.bOk = Snapshot.bComplete;
	Report.Status = Snapshot.bComplete ? TEXT("captured_no_load_state") : TEXT("captured_partial_state");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a deterministic revision-bound page from bounded loaded and Asset Registry network state.")
		: TEXT("Network capture is partial; revision completeness is false and mutation CAS remains blocked.");
	return Report;
}

FHyperAINetworkValidateReport UHyperAIStudioNetworkingToolset::hyper_network_validate(
	const FHyperAINetworkValidateRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAINetworkValidateReport Report;
	if (Request.TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths
		|| Request.Scope.Len() > 32
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioNetworkingContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioNetworkingContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Network validate request exceeds target, issue, work, output, or revision bounds.");
		return Report;
	}
	FHyperAIStudioNetworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioNetworkingFacade::CaptureNetwork(Request.TargetPaths, Request.Scope, true,
		Request.bIncludeOnDiskMetadata, Snapshot, CaptureStatus, CaptureDiagnostic,
		Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		return Report;
	}
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	bool bValidationTruncated = false;
	TArray<FHyperAINetworkingIssue> AllIssues =
		FHyperAIStudioNetworkingContracts::ValidateNetworkSnapshot(Snapshot,
			Request.bRequirePackagesClean, Request.bRequirePIEMultiplayerEvidence,
			Request.MaxIssues, bValidationTruncated);
	Report.bTruncated = bValidationTruncated;
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(AllIssues, Request.MaxIssues, Report.bTruncated, TEXT("network"),
			TEXT("expected_revision_mismatch"), TEXT("error"), TEXT("snapshot"),
			FString(), -1, TEXT("Fresh network revision no longer matches expected_revision."));
	}
	int64 EstimatedBytes = 1536;
	for (const auto& Issue : AllIssues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	if (Request.bIncludeVariants)
		AddBoundedVariants(TEXT("network"), Request.MaxOutputBytes, EstimatedBytes,
			Report.bTruncated, Report.Variants);
	Report.bOk = true;
	Report.bValid = Snapshot.bComplete && Report.ErrorCount == 0 && !Report.bTruncated;
	Report.Status = Report.bValid ? TEXT("valid")
		: Report.bTruncated ? TEXT("validation_truncated") : TEXT("validation_failed");
	Report.Diagnostic = Report.bValid
		? TEXT("Fresh no-load network state satisfies the independent bounded validator.")
		: TEXT("Fresh network state failed validation, freshness, completeness, or output requirements.");
	return Report;
}

FHyperAINetworkingApplyPlanReport UHyperAIStudioNetworkingToolset::hyper_network_apply_plan(
	const FHyperAINetworkingApplyPlanRequest& Request)
{
	return FHyperAIStudioNetworkingContracts::BuildNetworkPlan(Request);
}

FHyperAIGameFrameworkInspectReport UHyperAIStudioNetworkingToolset::hyper_game_framework_inspect(
	const FHyperAIGameFrameworkInspectRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAIGameFrameworkInspectReport Report;
	if (Request.TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths
		|| Request.Scope.Len() > 32 || Request.Projection.Len() > 16
		|| (Request.Projection != TEXT("summary") && Request.Projection != TEXT("details"))
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioNetworkingContracts::MaxPageSize
		|| Request.Cursor.Len() > FHyperAIStudioNetworkingContracts::MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioNetworkingContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Framework inspect request exceeds projection, target, page, cursor, work, or output bounds.");
		return Report;
	}
	int32 Offset = 0;
	FString CursorRevision;
	if (!ParseCursor(Request.Cursor, CursorRevision, Offset))
	{
		Report.Status = TEXT("invalid_cursor");
		Report.Diagnostic = TEXT("Cursor must be empty or bind one exact snapshot revision and offset.");
		return Report;
	}
	FHyperAIStudioGameFrameworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioNetworkingFacade::CaptureGameFramework(Request.TargetPaths, Request.Scope,
		true, Request.bIncludeOnDiskMetadata,
		Snapshot, CaptureStatus, CaptureDiagnostic, Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.ObjectsScanned = Snapshot.ObjectsScanned;
	Report.TotalRecords = Snapshot.Records.Num();
	if (!CursorRevision.IsEmpty() && CursorRevision != Snapshot.Revision)
	{
		Report.Status = TEXT("stale_cursor_revision");
		Report.Diagnostic = TEXT("Framework state changed between pages; restart with an empty cursor.");
		return Report;
	}
	if (Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("cursor_out_of_range");
		Report.Diagnostic = TEXT("Cursor offset is beyond the deterministic result set.");
		return Report;
	}
	int64 EstimatedBytes = 2048;
	for (const auto& Issue : Snapshot.CaptureIssues)
	{
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	bool bRecordExceeded = false;
	for (int32 Index = Offset; Index < End; ++Index)
	{
		const FHyperAIGameFrameworkRecord& Record = Snapshot.Records[Index];
		const int64 Bytes = EstimateFrameworkRecordBytes(Record);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			bRecordExceeded = true;
			break;
		}
		EstimatedBytes += Bytes;
		Report.Records.Add(Record);
	}
	Report.ReturnedRecords = Report.Records.Num();
	if (bRecordExceeded && Report.ReturnedRecords == 0 && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("record_exceeds_output_budget");
		Report.Diagnostic = TEXT("The next base Framework record cannot fit the output budget.");
		Report.NextCursor.Reset();
		return Report;
	}
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	if (NextOffset < Snapshot.Records.Num())
	{
		Report.bTruncated = true;
		if (Snapshot.bComplete) Report.NextCursor = MakeCursor(Snapshot.Revision, NextOffset);
	}
	if (Request.bIncludeVariants)
		AddBoundedVariants(TEXT("game_framework"), Request.MaxOutputBytes, EstimatedBytes,
			Report.bTruncated, Report.Variants);
	Report.bOk = Snapshot.bComplete;
	Report.Status = Snapshot.bComplete ? TEXT("captured_no_load_state") : TEXT("captured_partial_state");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a deterministic revision-bound page from bounded Framework state without loading providers or assets.")
		: TEXT("Framework capture is partial; revision completeness is false and mutation CAS remains blocked.");
	return Report;
}

FHyperAIGameFrameworkValidateReport UHyperAIStudioNetworkingToolset::hyper_game_framework_validate(
	const FHyperAIGameFrameworkValidateRequest& Request)
{
	using namespace HyperAIStudio::Networking::Private;
	FHyperAIGameFrameworkValidateReport Report;
	if (Request.TargetPaths.Num() > FHyperAIStudioNetworkingContracts::MaxTargetPaths
		|| Request.Scope.Len() > 32
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioNetworkingContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioNetworkingContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Framework validate request exceeds target, issue, work, output, or revision bounds.");
		return Report;
	}
	FHyperAIStudioGameFrameworkValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioNetworkingFacade::CaptureGameFramework(Request.TargetPaths, Request.Scope,
		true, Request.bIncludeOnDiskMetadata, Snapshot, CaptureStatus, CaptureDiagnostic,
		Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		return Report;
	}
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	bool bValidationTruncated = false;
	TArray<FHyperAINetworkingIssue> AllIssues =
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkSnapshot(Snapshot,
			Request.bRequirePackagesClean, Request.bRequirePIEWorldEvidence,
			Request.MaxIssues, bValidationTruncated);
	Report.bTruncated = bValidationTruncated;
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(AllIssues, Request.MaxIssues, Report.bTruncated, TEXT("game_framework"),
			TEXT("expected_revision_mismatch"), TEXT("error"), TEXT("snapshot"),
			FString(), -1, TEXT("Fresh Framework revision no longer matches expected_revision."));
	}
	int64 EstimatedBytes = 1536;
	for (const auto& Issue : AllIssues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	if (Request.bIncludeVariants)
		AddBoundedVariants(TEXT("game_framework"), Request.MaxOutputBytes, EstimatedBytes,
			Report.bTruncated, Report.Variants);
	Report.bOk = true;
	Report.bValid = Snapshot.bComplete && Report.ErrorCount == 0 && !Report.bTruncated;
	Report.Status = Report.bValid ? TEXT("valid")
		: Report.bTruncated ? TEXT("validation_truncated") : TEXT("validation_failed");
	Report.Diagnostic = Report.bValid
		? TEXT("Fresh no-load Framework state satisfies the independent bounded validator.")
		: TEXT("Fresh Framework state failed validation, freshness, completeness, or output requirements.");
	return Report;
}

FHyperAINetworkingApplyPlanReport UHyperAIStudioNetworkingToolset::hyper_game_framework_apply_plan(
	const FHyperAIGameFrameworkApplyPlanRequest& Request)
{
	return FHyperAIStudioNetworkingContracts::BuildGameFrameworkPlan(Request);
}

FString FHyperAIStudioNetworkingTypedPayload::GetTypeId() const
{
	return FHyperAIStudioNetworkingContracts::PayloadTypeId;
}

FString FHyperAIStudioNetworkingTypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNetworkingContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioNetworkingTypedPayload::GetBoundedByteSize() const
{
	int64 Bytes = 512 + 6ll * (Domain.Len() + ToolName.Len() + BaseRevision.Len()
		+ SemanticFingerprint.Len());
	for (const auto& Operation : Operations)
	{
		const FString* Strings[] = {&Operation.Domain, &Operation.Type, &Operation.TargetPath,
			&Operation.ExpectedRevision, &Operation.MemberName, &Operation.Role,
			&Operation.ParentClassPath, &Operation.Dormancy, &Operation.ReplicationCondition,
			&Operation.RepNotifyFunction, &Operation.RpcMode, &Operation.GameModeClassPath,
			&Operation.GameStateClassPath, &Operation.PlayerControllerClassPath,
			&Operation.PlayerStateClassPath, &Operation.PawnClassPath, &Operation.HUDClassPath,
			&Operation.GameSessionClassPath, &Operation.SpectatorClassPath,
			&Operation.GameInstanceClassPath};
		Bytes += 1024;
		for (const FString* Value : Strings) Bytes += 6ll * Value->Len() + 16;
		if (Bytes > MAX_int32) return MAX_int32;
	}
	return static_cast<int32>(Bytes);
}

FString FHyperAIStudioNetworkingTypedPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioNetworkingTypedPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe>();
	Clone->Domain = Domain;
	Clone->ToolName = ToolName;
	Clone->Operations = Operations;
	Clone->BaseRevision = BaseRevision;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioNetworkingResultPayload::GetTypeId() const
{
	return FHyperAIStudioNetworkingContracts::ResultTypeId;
}

FString FHyperAIStudioNetworkingResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioNetworkingContracts::ResultSchemaFingerprint();
}

int32 FHyperAIStudioNetworkingResultPayload::GetBoundedByteSize() const
{
	return 192 + 6 * (Domain.Len() + Phase.Len() + Revision.Len());
}

FHyperAIStudioNetworkingDomainAdapter::FHyperAIStudioNetworkingDomainAdapter()
	: Descriptor(FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioNetworkingDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioNetworkingDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	const bool bNetwork = Context.Binding.ToolName == TEXT("hyper_network_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioNetworkingContracts::NetworkMutationVariantId;
	const bool bFramework = Context.Binding.ToolName == TEXT("hyper_game_framework_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioNetworkingContracts::FrameworkMutationVariantId;
	if (Context.Binding.PackId != Descriptor.PackId || (!bNetwork && !bFramework)
		|| Context.Safety != EHyperAIStudioDomainSafety::Edit
		|| Context.Binding.ExpectedSafety != EHyperAIStudioDomainSafety::Edit
		|| Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint
		|| Context.ActionKind != EHyperAIStudioDomainExecutionActionKind::Apply
		|| Payload.GetTypeId() != FHyperAIStudioNetworkingContracts::PayloadTypeId
		|| Payload.GetSchemaFingerprint()
			!= FHyperAIStudioNetworkingContracts::PayloadSchemaFingerprint())
	{
		Result.StatusCode = TEXT("typed_binding_mismatch");
		Result.Diagnostic = TEXT("Networking adapter rejected a non-exact pack, tool, variant, safety, or DTO binding.");
		return Result;
	}
	Result.StatusCode = TEXT("staged_backend_required");
	Result.Diagnostic = TEXT("Networking/Game Framework plans remain side-effect-free until the shared async host owns a typed mutator, journal, transaction, save, validator, and fresh verification.");
	return Result;
}

bool FHyperAIStudioNetworkingStagingService::Stage(
	const FHyperAIStudioNetworkingStagedArtifact& Artifact,
	bool& bOutReplay,
	FString& OutError)
{
	using namespace HyperAIStudio::Networking::Private;
	bOutReplay = false;
	OutError.Reset();
	const int64 NowMs = NowMonotonicMs();
	const bool bNetworkBinding =
		Artifact.Prepared.Contract.Binding.ToolName == TEXT("hyper_network_apply_plan")
		&& Artifact.Prepared.Contract.Binding.VariantId
			== FHyperAIStudioNetworkingContracts::NetworkMutationVariantId
		&& Artifact.Payload.IsValid() && Artifact.Payload->Domain == TEXT("network")
		&& Artifact.Payload->ToolName == TEXT("hyper_network_apply_plan");
	const bool bFrameworkBinding =
		Artifact.Prepared.Contract.Binding.ToolName == TEXT("hyper_game_framework_apply_plan")
		&& Artifact.Prepared.Contract.Binding.VariantId
			== FHyperAIStudioNetworkingContracts::FrameworkMutationVariantId
		&& Artifact.Payload.IsValid() && Artifact.Payload->Domain == TEXT("game_framework")
		&& Artifact.Payload->ToolName == TEXT("hyper_game_framework_apply_plan");
	if (Artifact.CanonicalProjectId.IsEmpty()
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(Artifact.OperationId)
		|| !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Artifact.StageId)
		|| !Artifact.Payload.IsValid() || (!bNetworkBinding && !bFrameworkBinding)
		|| Artifact.Payload->Operations.IsEmpty()
		|| Artifact.Payload->Operations.Num() > FHyperAIStudioNetworkingContracts::MaxOperations
		|| Artifact.Payload->Operations.ContainsByPredicate([](const auto& Operation)
		{
			return !HasExactBackendDiscriminant(Operation);
		})
		|| !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(
			Artifact.Payload->BaseRevision)
		|| Artifact.Payload->GetTypeId() != FHyperAIStudioNetworkingContracts::PayloadTypeId
		|| Artifact.Payload->GetSchemaFingerprint()
			!= FHyperAIStudioNetworkingContracts::PayloadSchemaFingerprint()
		|| FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
			Artifact.Payload->Domain, Artifact.Payload->ToolName,
			Artifact.Payload->Operations, Artifact.Payload->BaseRevision)
			!= Artifact.Payload->GetSemanticFingerprint()
		|| Artifact.Payload->GetSemanticFingerprint()
			!= Artifact.Prepared.Contract.ArtifactSemanticFingerprint
		|| Artifact.Prepared.Contract.ArtifactTypeId != Artifact.Payload->GetTypeId()
		|| Artifact.Prepared.Contract.ArtifactSchemaFingerprint
			!= Artifact.Payload->GetSchemaFingerprint()
		|| Artifact.Prepared.Contract.EffectTarget
			!= Artifact.Payload->Domain + TEXT(":") + Artifact.Payload->BaseRevision
		|| Artifact.Prepared.Contract.Binding.CanonicalProjectId != Artifact.CanonicalProjectId
		|| Artifact.Prepared.Contract.Binding.PackId
			!= FHyperAIStudioNetworkingContracts::PackId
		|| Artifact.Prepared.Contract.Binding.ExpectedSafety != EHyperAIStudioDomainSafety::Edit
		|| Artifact.Prepared.Contract.Binding.ExpectedAdapterFingerprint
			!= FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor().AdapterFingerprint
		|| !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(Artifact.Prepared.PlanHash)
		|| !FHyperAIStudioNetworkingContracts::IsCanonicalSha256(
			Artifact.Prepared.EffectFingerprint)
		|| Artifact.ExpiresMonotonicMs <= NowMs
		|| Artifact.ExpiresMonotonicMs > NowMs
			+ FHyperAIStudioNetworkingContracts::StageLifetimeMs + 1000)
	{
		OutError = TEXT("Staged Networking artifact identity, DTO, hashes, safety, or lifetime are invalid.");
		return false;
	}
	if (Artifact.StageId != FHyperAIStudioNetworkingContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId, Artifact.Prepared.PlanHash,
		Artifact.Prepared.EffectFingerprint))
	{
		OutError = TEXT("Staged Networking artifact has a non-exact stage identity.");
		return false;
	}
	FHyperAIStudioPreparedTypedArtifact Resealed;
	FString ResealError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(
		Artifact.Prepared.Contract, Resealed, ResealError)
		|| Resealed.ContractFingerprint != Artifact.Prepared.ContractFingerprint
		|| Resealed.BindingFingerprint != Artifact.Prepared.BindingFingerprint
		|| Resealed.CapabilityHash != Artifact.Prepared.CapabilityHash
		|| Resealed.PlanHash != Artifact.Prepared.PlanHash
		|| Resealed.AuthorizationPlanHash != Artifact.Prepared.AuthorizationPlanHash
		|| Resealed.EffectFingerprint != Artifact.Prepared.EffectFingerprint)
	{
		OutError = TEXT("Staged Networking artifact failed exact shared-contract resealing.");
		return false;
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Artifact.Payload->CloneImmutable();
	if (&Detached.Get() == Artifact.Payload.Get()
		|| Detached->GetTypeId() != Artifact.Payload->GetTypeId()
		|| Detached->GetSchemaFingerprint() != Artifact.Payload->GetSchemaFingerprint()
		|| Detached->GetSemanticFingerprint() != Artifact.Payload->GetSemanticFingerprint()
		|| Detached->GetBoundedByteSize() != Artifact.Payload->GetBoundedByteSize()
		|| Detached->GetBoundedByteSize() <= 0
		|| Detached->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		OutError = TEXT("Typed payload clone is aliased, drifted, empty, or outside the shared request bound.");
		return false;
	}
	const TSharedRef<const FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe> Concrete =
		StaticCastSharedRef<const FHyperAIStudioNetworkingTypedPayload>(Detached);
	if (FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
		Concrete->Domain, Concrete->ToolName, Concrete->Operations, Concrete->BaseRevision)
		!= Concrete->SemanticFingerprint)
	{
		OutError = TEXT("Detached Networking payload failed concrete semantic recomputation.");
		return false;
	}
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMs);
	const FString Key = StageKey(Artifact.CanonicalProjectId, Artifact.OperationId);
	if (const FHyperAIStudioNetworkingStagedArtifact* Existing = State.Artifacts.Find(Key))
	{
		if (Existing->Prepared.PlanHash != Artifact.Prepared.PlanHash
			|| Existing->Prepared.EffectFingerprint != Artifact.Prepared.EffectFingerprint
			|| Existing->Prepared.ContractFingerprint != Artifact.Prepared.ContractFingerprint
			|| Existing->Payload->GetSemanticFingerprint() != Concrete->GetSemanticFingerprint()
			|| Existing->StageId != Artifact.StageId)
		{
			OutError = TEXT("operation_id is already staged with a different exact plan/effect binding.");
			return false;
		}
		bOutReplay = true;
		return true;
	}
	if (State.Artifacts.Num() >= FHyperAIStudioTypedArtifactLimits::MaxStagedArtifacts)
	{
		OutError = TEXT("Bounded Networking stage store is full; wait for expiry or async-host integration.");
		return false;
	}
	FHyperAIStudioNetworkingStagedArtifact Stored = Artifact;
	Stored.Payload = Concrete;
	State.Artifacts.Add(Key, MoveTemp(Stored));
	return true;
}

int32 FHyperAIStudioNetworkingStagingService::NumStaged()
{
	using namespace HyperAIStudio::Networking::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMonotonicMs());
	return State.Artifacts.Num();
}

void FHyperAIStudioNetworkingStagingService::Reset()
{
	using namespace HyperAIStudio::Networking::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	State.Artifacts.Reset();
}

void FHyperAIStudioNetworkingRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioNetworkingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioNetworkingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioNetworkingToolset::StaticClass(),
			FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioNetworking, Warning,
				TEXT("Could not unregister the owned Networking/Game Framework toolset: %s"),
				*Error);
		}
	}
	FHyperAIStudioNetworkingStagingService::Reset();
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioNetworkingRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioNetworkingContracts::IsRegistrationAllowed(
			FHyperAIStudioNetworkingContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioNetworkingToolset::StaticClass(),
			FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioNetworkingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioNetworkingContracts::IsRegistrationAllowed(
			FHyperAIStudioNetworkingContracts::IsPendingTestRegistrationEnabled())) return;
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioNetworkingToolset::StaticClass(),
		FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioNetworkingToolset::StaticClass(),
			FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName(), Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioNetworking, Error,
				TEXT("ToolsetRegistry rejected the owned six-tool Networking/Game Framework cohort: %s"),
				*Error);
		}
	}
}
