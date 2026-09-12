// Games by Hyper 2026.

#include "HyperAIStudioGameplayAIToolset.h"

// Every direct UE Gameplay AI dependency is isolated in this LoadingPhase=None editor module.

#include "AIController.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "CoreGlobals.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioGameplayAIToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGameplayAI, Log, All);

namespace HyperAIStudio::GameplayAI::Private
{
	constexpr int32 MinOutputBytes = 16384;
	constexpr int32 MaxTextCharacters = 512;
	constexpr int32 MaxIssueTextCharacters = 256;
	constexpr int32 MaxGraphTraversalDepth = 128;
	constexpr int32 MaxStagedArtifacts = 32;
	constexpr int64 StageLifetimeMs = 15000;

	FString Clip(const FString& Value, const int32 MaxCharacters = MaxTextCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical += TEXT("|");
	}

	void AppendInt(FString& Canonical, const int64 Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Value));
	}

	void AppendBool(FString& Canonical, const bool bValue)
	{
		AppendToken(Canonical, bValue ? TEXT("1") : TEXT("0"));
	}

	bool IsBaseVariant(const FString& Variant)
	{
		return Variant == TEXT("all") || Variant == TEXT("behavior_tree")
			|| Variant == TEXT("blackboard") || Variant == TEXT("ai_controller_perception");
	}

	bool IsOptionalVariant(const FString& Variant)
	{
		return Variant == TEXT("state_tree") || Variant == TEXT("eqs")
			|| Variant == TEXT("smart_object") || Variant == TEXT("gameplay_behavior")
			|| Variant == TEXT("gameplay_behavior_smart_object")
			|| Variant == TEXT("gameplay_interaction");
	}

	void AddIssue(
		TArray<FHyperAIGameplayAIIssue>& Issues,
		const int32 Maximum,
		bool& bOutTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& AssetPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Maximum)
		{
			bOutTruncated = true;
			return;
		}
		FHyperAIGameplayAIIssue Issue;
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.Variant = Clip(Variant, 96);
		Issue.AssetPath = Clip(AssetPath, MaxIssueTextCharacters);
		Issue.StableId = Clip(StableId, MaxIssueTextCharacters);
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message, MaxIssueTextCharacters);
		Issues.Add(MoveTemp(Issue));
	}

	bool HasError(const TArray<FHyperAIGameplayAIIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIGameplayAIIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	FString BlueprintStatus(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return TEXT("not_blueprint");
		}
		switch (Blueprint->Status)
		{
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_Dirty: return TEXT("dirty_compile_required");
		case BS_Error: return TEXT("compile_error");
		case BS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	FString NodeKind(const UEdGraphNode* GraphNode, const UObject* NodeInstance)
	{
		if (GraphNode && GraphNode->IsA<UBehaviorTreeGraphNode_Root>()) return TEXT("root");
		if (GraphNode && GraphNode->IsA<UBehaviorTreeGraphNode_CompositeDecorator>())
			return TEXT("composite_decorator");
		if (GraphNode && GraphNode->IsA<UEdGraphNode_Comment>()) return TEXT("comment");
		if (!NodeInstance) return TEXT("unknown");
		if (NodeInstance->IsA<UBTTaskNode>()) return TEXT("task");
		if (NodeInstance->IsA<UBTDecorator>()) return TEXT("decorator");
		if (NodeInstance->IsA<UBTService>()) return TEXT("service");
		if (NodeInstance->IsA<UBTCompositeNode>()) return TEXT("composite");
		return TEXT("unknown");
	}

	FString NodeStableId(const FString& AssetPath, const UEdGraphNode* Node, const int32 FallbackIndex)
	{
		const FString Local = Node && Node->NodeGuid.IsValid()
			? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
			: FString::Printf(TEXT("fallback-%d-%s"), FallbackIndex,
				Node ? *Node->GetClass()->GetName() : TEXT("null"));
		return TEXT("bt-node:") + AssetPath + TEXT(":") + Local;
	}

	void CaptureGraphNode(
		const FString& AssetPath,
		const UEdGraphNode* GraphNode,
		const int32 FallbackIndex,
		const int32 Depth,
		TSet<const UEdGraphNode*>& Seen,
		TArray<FHyperAIGameplayAINodeView>& OutNodes,
		bool& bOutComplete)
	{
		if (!GraphNode || Seen.Contains(GraphNode)) return;
		if (Depth > MaxGraphTraversalDepth)
		{
			bOutComplete = false;
			return;
		}
		Seen.Add(GraphNode);
		if (OutNodes.Num() >= FHyperAIStudioGameplayAIContracts::MaxNodesPerTree)
		{
			bOutComplete = false;
			return;
		}
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(GraphNode);
		const UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(GraphNode);
		if (!GraphNode->NodeGuid.IsValid()) bOutComplete = false;
		FHyperAIGameplayAINodeView View;
		View.StableId = NodeStableId(AssetPath, GraphNode, FallbackIndex);
		View.Kind = NodeKind(GraphNode, AINode ? AINode->NodeInstance.Get() : nullptr);
		View.GraphNodeClassPath = GraphNode->GetClass()->GetPathName();
		View.RuntimeNodeClassPath = AINode && AINode->NodeInstance
			? AINode->NodeInstance->GetClass()->GetPathName() : FString();
		View.Error = AINode ? Clip(AINode->ErrorMessage) : FString();
		View.bInjected = BTNode && BTNode->bInjectedNode;
		View.bBreakpointPresent = BTNode && BTNode->bHasBreakpoint;
		View.bBreakpointEnabled = BTNode && BTNode->bIsBreakpointEnabled;
		OutNodes.Add(MoveTemp(View));
		if (AINode)
		{
			for (const UAIGraphNode* SubNode : AINode->SubNodes)
			{
				CaptureGraphNode(AssetPath, SubNode, OutNodes.Num(), Depth + 1,
					Seen, OutNodes, bOutComplete);
			}
		}
	}

	FString NodeCanonical(const FHyperAIGameplayAINodeView& Node)
	{
		FString Canonical;
		AppendToken(Canonical, Node.StableId);
		AppendToken(Canonical, Node.Kind);
		AppendToken(Canonical, Node.GraphNodeClassPath);
		AppendToken(Canonical, Node.RuntimeNodeClassPath);
		AppendToken(Canonical, Node.Error);
		AppendBool(Canonical, Node.bInjected);
		AppendBool(Canonical, Node.bBreakpointPresent);
		AppendBool(Canonical, Node.bBreakpointEnabled);
		return Canonical;
	}

	FString KeyCanonical(const FHyperAIGameplayAIKeyView& Key)
	{
		FString Canonical;
		AppendToken(Canonical, Key.StableId);
		AppendToken(Canonical, Key.Name);
		AppendToken(Canonical, Key.KeyType);
		AppendToken(Canonical, Key.Description);
		AppendToken(Canonical, Key.Category);
		AppendBool(Canonical, Key.bInstanceSynced);
		return Canonical;
	}

	FString SenseCanonical(const FHyperAIGameplayAISenseView& Sense)
	{
		FString Canonical;
		AppendToken(Canonical, Sense.StableId);
		AppendToken(Canonical, Sense.ConfigClassPath);
		AppendToken(Canonical, Sense.SenseClassPath);
		AppendToken(Canonical, Sense.SenseName);
		AppendToken(Canonical, FString::Printf(TEXT("%.9g"), Sense.MaxAgeSeconds));
		AppendBool(Canonical, Sense.bStartsEnabled);
		AppendBool(Canonical, Sense.bDominant);
		return Canonical;
	}

	FString RecordCanonical(FHyperAIGameplayAIAssetRecord Record)
	{
		Record.Nodes.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		Record.Keys.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		Record.Senses.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.gameplay-ai-record.v1"));
		AppendToken(Canonical, Record.Variant);
		AppendToken(Canonical, Record.StableId);
		AppendToken(Canonical, Record.AssetPath);
		AppendBool(Canonical, Record.bPackageDirty);
		AppendToken(Canonical, Record.BlueprintStatus);
		AppendToken(Canonical, Record.GeneratedClassPath);
		AppendToken(Canonical, Record.ParentClassPath);
		AppendToken(Canonical, Record.BlackboardPath);
		AppendToken(Canonical, Record.BehaviorTreeGraphBlackboardPath);
		AppendToken(Canonical, Record.ParentAssetPath);
		AppendToken(Canonical, Record.DominantSenseClassPath);
		AppendInt(Canonical, Record.RootDecoratorCount);
		AppendInt(Canonical, Record.Nodes.Num());
		for (const auto& Node : Record.Nodes) AppendToken(Canonical, NodeCanonical(Node));
		AppendInt(Canonical, Record.Keys.Num());
		for (const auto& Key : Record.Keys) AppendToken(Canonical, KeyCanonical(Key));
		AppendInt(Canonical, Record.Senses.Num());
		for (const auto& Sense : Record.Senses) AppendToken(Canonical, SenseCanonical(Sense));
		return Canonical;
	}

	FString BackendOperationCanonical(const FHyperAIStudioGameplayAIBackendOperation& Operation)
	{
		FString Canonical;
		AppendInt(Canonical, static_cast<uint8>(Operation.Kind));
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.Name);
		AppendToken(Canonical, Operation.NewName);
		AppendToken(Canonical, Operation.KeyType);
		AppendToken(Canonical, Operation.ReferencePath);
		AppendBool(Canonical, Operation.bInstanceSynced);
		return Canonical;
	}

	bool IsClosedBackendName(const FString& Name)
	{
		return !Name.IsEmpty() && Name.Len() <= FHyperAIStudioGameplayAIContracts::MaxNameCharacters
			&& !Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS);
	}

	bool IsClosedBackendOperation(const FHyperAIStudioGameplayAIBackendOperation& Operation)
	{
		if (!FHyperAIStudioGameplayAIContracts::IsCanonicalProjectObjectPath(Operation.TargetPath)
			|| !FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(Operation.ExpectedRevision)
			|| Operation.Name.Len() > FHyperAIStudioGameplayAIContracts::MaxNameCharacters
			|| Operation.NewName.Len() > FHyperAIStudioGameplayAIContracts::MaxNameCharacters
			|| Operation.KeyType.Len() > FHyperAIStudioGameplayAIContracts::MaxNameCharacters
			|| Operation.ReferencePath.Len() > FHyperAIStudioGameplayAIContracts::MaxPathCharacters)
		{
			return false;
		}
		const bool bReferenceValid = Operation.ReferencePath.IsEmpty()
			|| FHyperAIStudioGameplayAIContracts::IsCanonicalProjectObjectPath(Operation.ReferencePath);
		switch (Operation.Kind)
		{
		case EHyperAIStudioGameplayAIOperationKind::SetBehaviorTreeBlackboard:
			return bReferenceValid && Operation.Name.IsEmpty() && Operation.NewName.IsEmpty()
				&& Operation.KeyType.IsEmpty() && !Operation.bInstanceSynced;
		case EHyperAIStudioGameplayAIOperationKind::AddBlackboardKey:
		{
			static const TSet<FString> AllowedTypes = {
				TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("name"), TEXT("string"),
				TEXT("vector"), TEXT("rotator")};
			return IsClosedBackendName(Operation.Name) && Operation.NewName.IsEmpty()
				&& AllowedTypes.Contains(Operation.KeyType) && Operation.ReferencePath.IsEmpty();
		}
		case EHyperAIStudioGameplayAIOperationKind::RemoveBlackboardKey:
			return IsClosedBackendName(Operation.Name) && Operation.NewName.IsEmpty()
				&& Operation.KeyType.IsEmpty() && Operation.ReferencePath.IsEmpty()
				&& !Operation.bInstanceSynced;
		case EHyperAIStudioGameplayAIOperationKind::RenameBlackboardKey:
			return IsClosedBackendName(Operation.Name) && IsClosedBackendName(Operation.NewName)
				&& !Operation.Name.Equals(Operation.NewName, ESearchCase::IgnoreCase)
				&& Operation.KeyType.IsEmpty() && Operation.ReferencePath.IsEmpty()
				&& !Operation.bInstanceSynced;
		case EHyperAIStudioGameplayAIOperationKind::SetBlackboardParent:
			return bReferenceValid
				&& !Operation.ReferencePath.Equals(Operation.TargetPath, ESearchCase::IgnoreCase)
				&& Operation.Name.IsEmpty() && Operation.NewName.IsEmpty()
				&& Operation.KeyType.IsEmpty() && !Operation.bInstanceSynced;
		default:
			return false;
		}
	}

	int32 EstimateRecordBytes(const FHyperAIGameplayAIAssetRecord& Record)
	{
		int64 Chars = Record.Variant.Len() + Record.StableId.Len() + Record.AssetPath.Len()
			+ Record.Revision.Len() + Record.BlueprintStatus.Len() + Record.GeneratedClassPath.Len()
			+ Record.ParentClassPath.Len() + Record.BlackboardPath.Len()
			+ Record.BehaviorTreeGraphBlackboardPath.Len()
			+ Record.ParentAssetPath.Len() + Record.DominantSenseClassPath.Len() + 256;
		for (const auto& Node : Record.Nodes)
			Chars += Node.StableId.Len() + Node.Kind.Len() + Node.GraphNodeClassPath.Len()
				+ Node.RuntimeNodeClassPath.Len() + Node.Error.Len() + 96;
		for (const auto& Key : Record.Keys)
			Chars += Key.StableId.Len() + Key.Name.Len() + Key.KeyType.Len()
				+ Key.Description.Len() + Key.Category.Len() + 64;
		for (const auto& Sense : Record.Senses)
			Chars += Sense.StableId.Len() + Sense.ConfigClassPath.Len() + Sense.SenseClassPath.Len()
				+ Sense.SenseName.Len() + 96;
		return static_cast<int32>(FMath::Min<int64>(MAX_int32, Chars * sizeof(TCHAR)));
	}

	int32 EstimateIssueBytes(const FHyperAIGameplayAIIssue& Issue)
	{
		const int64 Chars = Issue.Code.Len() + Issue.Severity.Len() + Issue.Variant.Len()
			+ Issue.AssetPath.Len() + Issue.StableId.Len() + Issue.Message.Len() + 128;
		return static_cast<int32>(FMath::Min<int64>(MAX_int32, Chars * sizeof(TCHAR)));
	}

	int32 EstimateVariantBytes(const FHyperAIGameplayAIVariantStatus& Variant)
	{
		int64 Chars = Variant.Variant.Len() + Variant.State.Len() + Variant.Remediation.Len() + 192;
		for (const FString& Value : Variant.RequiredPlugins) Chars += Value.Len() + 16;
		for (const FString& Value : Variant.RequiredModules) Chars += Value.Len() + 16;
		for (const FString& Value : Variant.SupportedCases) Chars += Value.Len() + 16;
		for (const FString& Value : Variant.UnsupportedCases) Chars += Value.Len() + 16;
		return static_cast<int32>(FMath::Min<int64>(MAX_int32, Chars * sizeof(TCHAR)));
	}

	void AppendIssueCopy(
		TArray<FHyperAIGameplayAIIssue>& OutIssues,
		const int32 Maximum,
		bool& bOutTruncated,
		const FHyperAIGameplayAIIssue& Issue)
	{
		AddIssue(OutIssues, Maximum, bOutTruncated, *Issue.Code, *Issue.Severity,
			Issue.Variant, Issue.AssetPath, Issue.StableId, Issue.OperationIndex, Issue.Message);
	}

	bool CaptureBehaviorTree(UBehaviorTree* Tree, const bool bDetails,
		FHyperAIGameplayAIAssetRecord& OutRecord, bool& bOutComplete)
	{
		if (!Tree || !Tree->GetPathName().StartsWith(TEXT("/Game/"))) return false;
		OutRecord.Variant = TEXT("behavior_tree");
		OutRecord.AssetPath = Tree->GetPathName();
		OutRecord.StableId = TEXT("behavior-tree:") + OutRecord.AssetPath;
		OutRecord.bPackageDirty = Tree->GetOutermost() && Tree->GetOutermost()->IsDirty();
		OutRecord.BlackboardPath = Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : FString();
		OutRecord.RootDecoratorCount = Tree->RootDecorators.Num();
		if (!bDetails)
		{
			bOutComplete = false;
			return true;
		}
#if WITH_EDITORONLY_DATA
		if (Tree->BTGraph)
		{
			TSet<const UEdGraphNode*> Seen;
			for (int32 Index = 0; Index < Tree->BTGraph->Nodes.Num(); ++Index)
			{
				if (const UBehaviorTreeGraphNode_Root* Root =
					Cast<UBehaviorTreeGraphNode_Root>(Tree->BTGraph->Nodes[Index]))
				{
					OutRecord.BehaviorTreeGraphBlackboardPath = Root->BlackboardAsset
						? Root->BlackboardAsset->GetPathName() : FString();
				}
				CaptureGraphNode(OutRecord.AssetPath, Tree->BTGraph->Nodes[Index], Index, 0,
					Seen, OutRecord.Nodes, bOutComplete);
			}
		}
		else
		{
			bOutComplete = false;
		}
#endif
		return true;
	}

	bool CaptureBlackboard(UBlackboardData* Data, const bool bDetails,
		FHyperAIGameplayAIAssetRecord& OutRecord, bool& bOutComplete)
	{
		if (!Data || !Data->GetPathName().StartsWith(TEXT("/Game/"))) return false;
		OutRecord.Variant = TEXT("blackboard");
		OutRecord.AssetPath = Data->GetPathName();
		OutRecord.StableId = TEXT("blackboard:") + OutRecord.AssetPath;
		OutRecord.bPackageDirty = Data->GetOutermost() && Data->GetOutermost()->IsDirty();
		OutRecord.ParentAssetPath = Data->Parent ? Data->Parent->GetPathName() : FString();
		if (!bDetails)
		{
			bOutComplete = false;
			return true;
		}
		for (int32 Index = 0; Index < Data->Keys.Num(); ++Index)
		{
			if (OutRecord.Keys.Num() >= FHyperAIStudioGameplayAIContracts::MaxKeysPerBlackboard)
			{
				bOutComplete = false;
				break;
			}
			const FBlackboardEntry& Entry = Data->Keys[Index];
			FHyperAIGameplayAIKeyView Key;
			Key.Name = Entry.EntryName.ToString();
			Key.StableId = TEXT("blackboard-key:") + OutRecord.AssetPath + TEXT(":") + Key.Name;
			Key.KeyType = Entry.KeyType ? Entry.KeyType->GetClass()->GetPathName() : FString();
#if WITH_EDITORONLY_DATA
			Key.Description = Clip(Entry.EntryDescription);
			Key.Category = Entry.EntryCategory.ToString();
#endif
			Key.bInstanceSynced = Entry.bInstanceSynced != 0;
			OutRecord.Keys.Add(MoveTemp(Key));
		}
		return true;
	}

	bool CaptureAIController(UBlueprint* Blueprint, const bool bDetails,
		FHyperAIGameplayAIAssetRecord& OutRecord, bool& bOutComplete)
	{
		if (!Blueprint || !Blueprint->GeneratedClass
			|| !Blueprint->GeneratedClass->IsChildOf(AAIController::StaticClass())
			|| !Blueprint->GetPathName().StartsWith(TEXT("/Game/"))) return false;
		OutRecord.Variant = TEXT("ai_controller_perception");
		OutRecord.AssetPath = Blueprint->GetPathName();
		OutRecord.StableId = TEXT("ai-controller:") + OutRecord.AssetPath;
		OutRecord.bPackageDirty = Blueprint->GetOutermost() && Blueprint->GetOutermost()->IsDirty();
		OutRecord.BlueprintStatus = BlueprintStatus(Blueprint);
		OutRecord.GeneratedClassPath = Blueprint->GeneratedClass->GetPathName();
		OutRecord.ParentClassPath = Blueprint->GeneratedClass->GetSuperClass()
			? Blueprint->GeneratedClass->GetSuperClass()->GetPathName() : FString();
		if (!bDetails)
		{
			bOutComplete = false;
			return true;
		}
		const AAIController* CDO = Cast<AAIController>(Blueprint->GeneratedClass->GetDefaultObject(false));
		if (!CDO)
		{
			bOutComplete = false;
			return true;
		}
		const UBlackboardComponent* Blackboard = CDO->GetBlackboardComponent();
		OutRecord.BlackboardPath = Blackboard && Blackboard->GetBlackboardAsset()
			? Blackboard->GetBlackboardAsset()->GetPathName() : FString();
		const UAIPerceptionComponent* Perception = CDO->FindComponentByClass<UAIPerceptionComponent>();
		if (!Perception) return true;
		OutRecord.DominantSenseClassPath = Perception->GetDominantSense()
			? Perception->GetDominantSense()->GetPathName() : FString();
		int32 SenseIndex = 0;
		for (auto It = Perception->GetSensesConfigIterator(); It; ++It, ++SenseIndex)
		{
			if (OutRecord.Senses.Num() >= FHyperAIStudioGameplayAIContracts::MaxSensesPerController)
			{
				bOutComplete = false;
				break;
			}
			const UAISenseConfig* Config = *It;
			if (!Config)
			{
				bOutComplete = false;
				continue;
			}
			FHyperAIGameplayAISenseView Sense;
			Sense.StableId = FString::Printf(TEXT("ai-sense:%s:%d"), *OutRecord.AssetPath, SenseIndex);
			Sense.ConfigClassPath = Config->GetClass()->GetPathName();
			const TSubclassOf<UAISense> SenseClass = Config->GetSenseImplementation();
			Sense.SenseClassPath = SenseClass ? SenseClass->GetPathName() : FString();
			Sense.SenseName = Config->GetSenseName();
			Sense.MaxAgeSeconds = Config->GetMaxAge();
			Sense.bStartsEnabled = Config->GetStartsEnabled();
			Sense.bDominant = !Sense.SenseClassPath.IsEmpty()
				&& Sense.SenseClassPath == OutRecord.DominantSenseClassPath;
			OutRecord.Senses.Add(MoveTemp(Sense));
		}
		return true;
	}

	bool CaptureObject(UObject* Object, const FString& Variant, const bool bDetails,
		FHyperAIGameplayAIAssetRecord& OutRecord, bool& bOutComplete)
	{
		if ((Variant == TEXT("all") || Variant == TEXT("behavior_tree"))
			&& CaptureBehaviorTree(Cast<UBehaviorTree>(Object), bDetails, OutRecord, bOutComplete)) return true;
		if ((Variant == TEXT("all") || Variant == TEXT("blackboard"))
			&& CaptureBlackboard(Cast<UBlackboardData>(Object), bDetails, OutRecord, bOutComplete)) return true;
		if ((Variant == TEXT("all") || Variant == TEXT("ai_controller_perception"))
			&& CaptureAIController(Cast<UBlueprint>(Object), bDetails, OutRecord, bOutComplete)) return true;
		return false;
	}

	bool ParseCursor(const FString& Cursor, const FString& Revision, int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("|"), false);
		return Parts.Num() == 3 && Parts[0] == TEXT("gameplay-ai-v1") && Parts[1] == Revision
			&& LexTryParseString(OutOffset, *Parts[2]) && OutOffset >= 0;
	}

	FString MakeCursor(const FString& Revision, const int32 Offset)
	{
		return FString::Printf(TEXT("gameplay-ai-v1|%s|%d"), *Revision, Offset);
	}

	struct FStagingState
	{
		FCriticalSection Mutex;
		TMap<FString, FHyperAIStudioGameplayAIStagedArtifact> Artifacts;
	};

	FStagingState& GetStagingState()
	{
		static FStagingState State;
		return State;
	}

	int64 NowMonotonicMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}

	void PruneExpired(FStagingState& State, const int64 NowMs)
	{
		for (auto It = State.Artifacts.CreateIterator(); It; ++It)
		{
			if (It.Value().ExpiresMonotonicMs <= NowMs) It.RemoveCurrent();
		}
	}

	FString StageKey(const FString& ProjectId, const FString& OperationId)
	{
		return ProjectId + TEXT("\n") + OperationId;
	}
}

const TArray<FHyperAIStudioGameplayAIVariantDescriptor>&
FHyperAIStudioGameplayAIFacade::GetVariantDescriptors()
{
	static const TArray<FHyperAIStudioGameplayAIVariantDescriptor> Descriptors = {
		{TEXT("behavior_tree"), {},
			{TEXT("AIModule"), TEXT("AIGraph"), TEXT("BehaviorTreeEditor")},
			true, true, false,
			{TEXT("loaded_asset_inspection"), TEXT("graph_node_error_inspection"),
				TEXT("breakpoint_state_inspection"), TEXT("set_blackboard_reference_dry_run_stage")},
			{TEXT("asset_creation_or_disk_loading"), TEXT("graph_structure_authoring"),
				TEXT("task_decorator_service_creation_or_repair"), TEXT("breakpoint_mutation"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("blackboard"), {}, {TEXT("AIModule")}, true, true, false,
			{TEXT("loaded_asset_inspection"), TEXT("primitive_key_add_remove_rename_dry_run_stage"),
				TEXT("parent_reference_dry_run_stage")},
			{TEXT("asset_creation_or_disk_loading"), TEXT("object_class_enum_key_authoring"),
				TEXT("inherited_key_mutation"), TEXT("mutation_execution_until_async_host")}},
		{TEXT("ai_controller_perception"), {}, {TEXT("AIModule")}, true, false, false,
			{TEXT("loaded_blueprint_cdo_inspection"), TEXT("perception_config_inspection")},
			{TEXT("controller_or_perception_mutation"), TEXT("runtime_instance_inspection"),
				TEXT("asset_creation_or_disk_loading")}},
		{TEXT("state_tree"), {TEXT("StateTree")},
			{TEXT("StateTreeModule"), TEXT("StateTreeEditorModule")}, false, false, false, {},
			{TEXT("typed_adapter_not_implemented")}},
		{TEXT("eqs"), {TEXT("EnvironmentQueryEditor")},
			{TEXT("AIModule"), TEXT("EnvironmentQueryEditor")}, false, false, false, {},
			{TEXT("typed_adapter_not_implemented")}},
		{TEXT("smart_object"), {TEXT("SmartObjects"), TEXT("GameplayAbilities")},
			{TEXT("SmartObjectsModule"), TEXT("SmartObjectsEditorModule"), TEXT("GameplayAbilities")},
			false, false, false, {}, {TEXT("typed_adapter_not_implemented")}},
		{TEXT("gameplay_behavior"), {TEXT("GameplayBehaviors"), TEXT("GameplayAbilities")},
			{TEXT("GameplayBehaviorsModule"), TEXT("GameplayBehaviorsEditorModule"), TEXT("GameplayAbilities")},
			false, false, false, {}, {TEXT("typed_adapter_not_implemented")}},
		{TEXT("gameplay_behavior_smart_object"),
			{TEXT("GameplayBehaviorSmartObjects"), TEXT("GameplayBehaviors"), TEXT("SmartObjects"), TEXT("GameplayAbilities")},
			{TEXT("GameplayBehaviorSmartObjectsModule"), TEXT("GameplayBehaviorsModule"),
				TEXT("GameplayBehaviorsEditorModule"), TEXT("SmartObjectsModule"),
				TEXT("SmartObjectsEditorModule"), TEXT("GameplayAbilities")}, false, false, false, {},
			{TEXT("typed_adapter_not_implemented")}},
		{TEXT("gameplay_interaction"),
			{TEXT("GameplayInteractions"), TEXT("StateTree"), TEXT("SmartObjects"),
				TEXT("GameplayStateTree"), TEXT("GameplayAbilities"), TEXT("ContextualAnimation"),
				TEXT("NavCorridor")},
			{TEXT("GameplayInteractionsModule"), TEXT("StateTreeModule"), TEXT("StateTreeEditorModule"),
				TEXT("SmartObjectsModule"), TEXT("SmartObjectsEditorModule"),
				TEXT("GameplayStateTreeModule"), TEXT("GameplayAbilities"),
				TEXT("ContextualAnimation"), TEXT("ContextualAnimationEditor"),
				TEXT("NavCorridor")}, false, false, false, {},
			{TEXT("typed_adapter_not_implemented")}}
	};
	return Descriptors;
}

TArray<FHyperAIGameplayAIVariantStatus> FHyperAIStudioGameplayAIFacade::ResolveVariantStatuses()
{
	TArray<FHyperAIGameplayAIVariantStatus> Result;
	for (const FHyperAIStudioGameplayAIVariantDescriptor& Descriptor : GetVariantDescriptors())
	{
		FHyperAIGameplayAIVariantStatus Status;
		Status.Variant = Descriptor.Variant;
		Status.RequiredPlugins = Descriptor.RequiredPlugins;
		Status.RequiredModules = Descriptor.RequiredModules;
		Status.bInspectImplemented = Descriptor.bInspectImplemented;
		Status.bApplyImplemented = Descriptor.bApplyImplemented;
		Status.bApplyBackendExecutable = Descriptor.bApplyBackendExecutable;
		Status.SupportedCases = Descriptor.SupportedCases;
		Status.UnsupportedCases = Descriptor.UnsupportedCases;
		Status.bPluginsInstalled = true;
		Status.bPluginsEnabled = true;
		for (const FString& PluginId : Descriptor.RequiredPlugins)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginId);
			Status.bPluginsInstalled &= Plugin.IsValid();
			Status.bPluginsEnabled &= Plugin.IsValid() && Plugin->IsEnabled();
		}
		Status.bModulesLoaded = true;
		for (const FString& ModuleId : Descriptor.RequiredModules)
		{
			Status.bModulesLoaded &= FModuleManager::Get().IsModuleLoaded(*ModuleId);
		}

		if (!Descriptor.bInspectImplemented)
		{
			Status.State = TEXT("adapter_unavailable");
			Status.Remediation = TEXT("This typed optional adapter is not implemented; prerequisite observations are informational and no fallback is attempted.");
		}
		else if (!Status.bPluginsInstalled)
		{
			Status.State = TEXT("missing_plugin");
			Status.Remediation = TEXT("Install the exact listed Unreal plugin prerequisites; no fallback is attempted.");
		}
		else if (!Status.bPluginsEnabled)
		{
			Status.State = TEXT("plugin_disabled");
			Status.Remediation = TEXT("Enable the exact listed Unreal plugins and restart when Unreal requests it.");
		}
		else if (Descriptor.bApplyImplemented && !Descriptor.bApplyBackendExecutable)
		{
			// This explicit-load facade directly links its base AI modules. AIGraph can be
			// mapped by the platform loader without FModuleManager reporting it started;
			// the known hard backend blocker is therefore the authoritative state.
			Status.State = TEXT("staged_backend_required");
			Status.Remediation = TEXT("Dry-run and idempotent staging are implemented; mutation remains blocked until the shared async execution host is wired.");
		}
		else if (!Status.bModulesLoaded)
		{
			Status.State = TEXT("enabled_not_loaded");
			Status.Remediation = TEXT("Required modules are not loaded; restart or load only the admitted exact capability module.");
		}
		else if (!Descriptor.bApplyImplemented)
		{
			Status.State = TEXT("inspect_ready_apply_unavailable");
			Status.Remediation = TEXT("Loaded-only inspection is available; mutation remains unavailable until its dedicated typed adapter is proven.");
		}
		else
		{
			Status.State = TEXT("ready");
			Status.Remediation.Reset();
		}
		Result.Add(MoveTemp(Status));
	}
	return Result;
}

bool FHyperAIStudioGameplayAIFacade::CaptureLoaded(
	const TArray<FString>& AssetPaths,
	const FString& Variant,
	const bool bIncludeDetails,
	FHyperAIStudioGameplayAIValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded UObject inspection is allowed only on Unreal's serialized game thread.");
		return false;
	}
	if (!IsBaseVariant(Variant) && !IsOptionalVariant(Variant))
	{
		OutStatus = TEXT("unknown_variant");
		OutDiagnostic = TEXT("Variant is not in the closed Gameplay AI prerequisite matrix.");
		return false;
	}
	if (IsOptionalVariant(Variant))
	{
		OutStatus = TEXT("variant_adapter_unavailable");
		OutDiagnostic = TEXT("The requested optional family is prerequisite-gated and has no typed adapter in this module.");
		return false;
	}
	if (AssetPaths.Num() > FHyperAIStudioGameplayAIContracts::MaxAssetPaths)
	{
		OutStatus = TEXT("too_many_asset_paths");
		OutDiagnostic = TEXT("Loaded-only capture exceeded the exact asset-path bound.");
		return false;
	}
	for (const FString& Path : AssetPaths)
	{
		if (!FHyperAIStudioGameplayAIContracts::IsCanonicalProjectObjectPath(Path))
		{
			OutStatus = TEXT("invalid_asset_path");
			OutDiagnostic = TEXT("Every exact loaded-object path must be canonical and project-contained.");
			return false;
		}
	}
	TArray<FString> CanonicalScopePaths = AssetPaths;
	CanonicalScopePaths.Sort();
	FString ScopeCanonical;
	AppendToken(ScopeCanonical, TEXT("hyperai.gameplay-ai-scope.v1"));
	AppendToken(ScopeCanonical, Variant);
	AppendBool(ScopeCanonical, bIncludeDetails);
	AppendBool(ScopeCanonical, AssetPaths.IsEmpty());
	AppendInt(ScopeCanonical, CanonicalScopePaths.Num());
	for (const FString& Path : CanonicalScopePaths) AppendToken(ScopeCanonical, Path);
	OutSnapshot.ScopeFingerprint =
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ScopeCanonical);
	if (!FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(OutSnapshot.ScopeFingerprint))
	{
		OutStatus = TEXT("scope_identity_unavailable");
		OutDiagnostic = TEXT("Bounded Gameplay AI scope identity could not be produced.");
		return false;
	}

	TSet<const UObject*> Seen;
	auto AddObject = [&](UObject* Object, const FString& RequestedPath)
	{
		if (!IsValid(Object) || Seen.Contains(Object)) return;
		Seen.Add(Object);
		if (OutSnapshot.Records.Num() >= FHyperAIStudioGameplayAIContracts::MaxRecords)
		{
			OutSnapshot.bComplete = false;
			return;
		}
		FHyperAIGameplayAIAssetRecord Record;
		Record.bRevisionComplete = true;
		if (!CaptureObject(Object, Variant, bIncludeDetails, Record, Record.bRevisionComplete))
		{
			bool bIgnored = false;
			AddIssue(OutSnapshot.CaptureIssues, FHyperAIStudioGameplayAIContracts::MaxIssues,
				bIgnored, TEXT("unsupported_loaded_object"), TEXT("error"), Variant,
				RequestedPath, TEXT("capture:") + RequestedPath, -1,
				TEXT("Object is loaded but is not owned by the requested base Gameplay AI variant."));
			OutSnapshot.bComplete = false;
			return;
		}
		if (!Record.bRevisionComplete) OutSnapshot.bComplete = false;
		OutSnapshot.Records.Add(MoveTemp(Record));
	};

	if (!AssetPaths.IsEmpty())
	{
		for (const FString& Path : AssetPaths)
		{
			++OutSnapshot.LoadedObjectsScanned;
			UObject* Object = FindObject<UObject>(nullptr, *Path);
			if (!IsValid(Object))
			{
				bool bIgnored = false;
				AddIssue(OutSnapshot.CaptureIssues, FHyperAIStudioGameplayAIContracts::MaxIssues,
					bIgnored, TEXT("object_not_loaded"), TEXT("error"), Variant, Path,
					TEXT("capture:") + Path, -1,
					TEXT("Exact object is not already loaded; synchronous disk/class loading is prohibited."));
				OutSnapshot.bComplete = false;
				continue;
			}
			AddObject(Object, Path);
		}
	}
	else
	{
		if (Variant == TEXT("all") || Variant == TEXT("behavior_tree"))
		{
			for (TObjectIterator<UBehaviorTree> It; It; ++It)
			{
				if (OutSnapshot.LoadedObjectsScanned >= FHyperAIStudioGameplayAIContracts::MaxLoadedObjectsScanned)
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.LoadedObjectsScanned;
				if (IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
					AddObject(*It, It->GetPathName());
			}
		}
		if (Variant == TEXT("all") || Variant == TEXT("blackboard"))
		{
			for (TObjectIterator<UBlackboardData> It; It; ++It)
			{
				if (OutSnapshot.LoadedObjectsScanned >= FHyperAIStudioGameplayAIContracts::MaxLoadedObjectsScanned)
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.LoadedObjectsScanned;
				if (IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
					AddObject(*It, It->GetPathName());
			}
		}
		if (Variant == TEXT("all") || Variant == TEXT("ai_controller_perception"))
		{
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				if (OutSnapshot.LoadedObjectsScanned >= FHyperAIStudioGameplayAIContracts::MaxLoadedObjectsScanned)
				{
					OutSnapshot.bComplete = false;
					break;
				}
				++OutSnapshot.LoadedObjectsScanned;
				if (IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
					&& It->GeneratedClass && It->GeneratedClass->IsChildOf(AAIController::StaticClass()))
				{
					AddObject(*It, It->GetPathName());
				}
			}
		}
	}

	FHyperAIStudioGameplayAIContracts::ComputeSnapshotRevision(OutSnapshot);
	OutStatus = TEXT("captured_loaded_state");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured a bounded immutable view without loading assets or optional adapters.")
		: TEXT("Captured a bounded partial loaded-state view; revision completeness is false.");
	return true;
}

FString FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioGameplayAI.HyperAIStudioGameplayAIToolset");
}

const TArray<FHyperAIStudioGameplayAIManifestEntry>& FHyperAIStudioGameplayAIContracts::GetManifest()
{
	static const TArray<FHyperAIStudioGameplayAIManifestEntry> Manifest = {
		{TEXT("hyper_gameplay_ai_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_gameplay_ai_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_gameplay_ai_validate"), GetQualifiedToolsetName()}
	};
	return Manifest;
}

bool FHyperAIStudioGameplayAIContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioGameplayAIContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const auto& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
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

bool FHyperAIStudioGameplayAIContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	for (int32 Index = 0; Index < Path.Len(); ++Index)
	{
		if (Path[Index] == TEXT('\0')) return false;
	}
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters
		|| Path.Contains(TEXT("\\")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))
		|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidObjectPath(Path)) return false;
	FString PackageName;
	FString ObjectName;
	return Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd) && !ObjectName.IsEmpty() && !ObjectName.EndsWith(TEXT("_C"));
}

bool FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive)) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioGameplayAIContracts::PayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("gameplay_ai.payload.v1|set_behavior_tree_blackboard|add_blackboard_key|remove_blackboard_key|rename_blackboard_key|set_blackboard_parent|edit|no_client_authorization"));
	return Fingerprint;
}

FString FHyperAIStudioGameplayAIContracts::ResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("gameplay_ai.result.v1|phase|revision|valid|error_count|bounded"));
	return Fingerprint;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioGameplayAIContracts::GetBaseAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.gameplay_ai.base_editor_assets");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_gameplay_ai_apply_plan"), MutationVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), ResultTypeId, ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioGameplayAIContracts::ValidateOperationShape(
	const FHyperAIGameplayAIPlanOperation& Operation,
	FHyperAIStudioGameplayAIBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError)
{
	OutOperation = {};
	OutErrorCode.Reset();
	OutError.Reset();
	auto Fail = [&](const TCHAR* Code, const TCHAR* Message)
	{
		OutErrorCode = Code;
		OutError = Message;
		return false;
	};
	if (!IsCanonicalProjectObjectPath(Operation.TargetPath))
		return Fail(TEXT("invalid_target_path"), TEXT("Target must be one canonical /Game object path."));
	if (!IsCanonicalSha256(Operation.ExpectedRevision))
		return Fail(TEXT("revision_required"), TEXT("Every Gameplay AI edit requires an exact canonical target revision."));
	if (Operation.Type.Len() > MaxNameCharacters || Operation.Name.Len() > MaxNameCharacters
		|| Operation.NewName.Len() > MaxNameCharacters
		|| Operation.KeyType.Len() > MaxNameCharacters || Operation.ReferencePath.Len() > MaxPathCharacters)
		return Fail(TEXT("field_too_long"), TEXT("Operation field exceeds its closed schema bound."));

	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.Name = Operation.Name;
	OutOperation.NewName = Operation.NewName;
	OutOperation.KeyType = Operation.KeyType;
	OutOperation.ReferencePath = Operation.ReferencePath;
	OutOperation.bInstanceSynced = Operation.bInstanceSynced;
	const bool bReferenceValid = Operation.ReferencePath.IsEmpty()
		|| IsCanonicalProjectObjectPath(Operation.ReferencePath);
	const bool bNameValid = !Operation.Name.IsEmpty()
		&& !Operation.Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)
		&& FName::IsValidXName(Operation.Name, INVALID_OBJECTNAME_CHARACTERS);
	const bool bNewNameValid = !Operation.NewName.IsEmpty()
		&& !Operation.NewName.Equals(TEXT("None"), ESearchCase::IgnoreCase)
		&& FName::IsValidXName(Operation.NewName, INVALID_OBJECTNAME_CHARACTERS);

	if (Operation.Type == TEXT("set_behavior_tree_blackboard"))
	{
		OutOperation.Kind = EHyperAIStudioGameplayAIOperationKind::SetBehaviorTreeBlackboard;
		if (!bReferenceValid || !Operation.Name.IsEmpty() || !Operation.NewName.IsEmpty()
			|| !Operation.KeyType.IsEmpty() || Operation.bInstanceSynced)
			return Fail(TEXT("invalid_behavior_tree_shape"),
				TEXT("set_behavior_tree_blackboard accepts only target, CAS, and an optional loaded Blackboard reference."));
	}
	else if (Operation.Type == TEXT("add_blackboard_key"))
	{
		OutOperation.Kind = EHyperAIStudioGameplayAIOperationKind::AddBlackboardKey;
		static const TSet<FString> Allowed = {
			TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("name"), TEXT("string"),
			TEXT("vector"), TEXT("rotator")};
		if (!bNameValid || !Allowed.Contains(Operation.KeyType) || !Operation.NewName.IsEmpty()
			|| !Operation.ReferencePath.IsEmpty())
			return Fail(TEXT("invalid_add_key_shape"),
				TEXT("add_blackboard_key requires one valid key name and one closed key type."));
	}
	else if (Operation.Type == TEXT("remove_blackboard_key"))
	{
		OutOperation.Kind = EHyperAIStudioGameplayAIOperationKind::RemoveBlackboardKey;
		if (!bNameValid || !Operation.NewName.IsEmpty() || !Operation.KeyType.IsEmpty()
			|| !Operation.ReferencePath.IsEmpty() || Operation.bInstanceSynced)
			return Fail(TEXT("invalid_remove_key_shape"),
				TEXT("remove_blackboard_key accepts exactly one existing key name."));
	}
	else if (Operation.Type == TEXT("rename_blackboard_key"))
	{
		OutOperation.Kind = EHyperAIStudioGameplayAIOperationKind::RenameBlackboardKey;
		if (!bNameValid || !bNewNameValid
			|| Operation.Name.Equals(Operation.NewName, ESearchCase::IgnoreCase)
			|| !Operation.KeyType.IsEmpty() || !Operation.ReferencePath.IsEmpty()
			|| Operation.bInstanceSynced)
			return Fail(TEXT("invalid_rename_key_shape"),
				TEXT("rename_blackboard_key requires distinct valid old/new names and no unrelated fields."));
	}
	else if (Operation.Type == TEXT("set_blackboard_parent"))
	{
		OutOperation.Kind = EHyperAIStudioGameplayAIOperationKind::SetBlackboardParent;
		if (!bReferenceValid
			|| Operation.ReferencePath.Equals(Operation.TargetPath, ESearchCase::IgnoreCase)
			|| !Operation.Name.IsEmpty() || !Operation.NewName.IsEmpty()
			|| !Operation.KeyType.IsEmpty() || Operation.bInstanceSynced)
			return Fail(TEXT("invalid_parent_shape"),
				TEXT("set_blackboard_parent accepts only target, CAS, and a distinct optional loaded parent."));
	}
	else if (Operation.Type.StartsWith(TEXT("state_tree_"))
		|| Operation.Type.StartsWith(TEXT("eqs_"))
		|| Operation.Type.StartsWith(TEXT("smart_object_"))
		|| Operation.Type.StartsWith(TEXT("gameplay_behavior_"))
		|| Operation.Type.StartsWith(TEXT("gameplay_interaction_"))
		|| Operation.Type.StartsWith(TEXT("ai_controller_perception_")))
	{
		return Fail(TEXT("variant_adapter_unavailable"),
			TEXT("Operation belongs to a prerequisite-gated variant without an admitted typed adapter."));
	}
	else
	{
		return Fail(TEXT("unknown_operation_type"),
			TEXT("Operation type is not in the closed Gameplay AI edit schema."));
	}
	return true;
}

FString FHyperAIStudioGameplayAIContracts::ComputeSnapshotRevision(
	FHyperAIStudioGameplayAIValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	Snapshot.Records.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
	Snapshot.CaptureIssues.Sort([](const auto& A, const auto& B)
	{
		if (A.Code != B.Code) return A.Code < B.Code;
		if (A.StableId != B.StableId) return A.StableId < B.StableId;
		if (A.AssetPath != B.AssetPath) return A.AssetPath < B.AssetPath;
		return A.Message < B.Message;
	});
	for (FHyperAIGameplayAIAssetRecord& Record : Snapshot.Records)
	{
		Record.Nodes.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		Record.Keys.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		Record.Senses.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
		const bool bCaptureComplete = Record.bRevisionComplete;
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(RecordCanonical(Record));
		Record.bRevisionComplete = bCaptureComplete && IsCanonicalSha256(Record.Revision);
		if (!Record.bRevisionComplete) Snapshot.bComplete = false;
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gameplay-ai-snapshot.v1"));
	AppendToken(Canonical, Snapshot.ScopeFingerprint);
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.LoadedObjectsScanned);
	AppendInt(Canonical, Snapshot.Records.Num());
	for (const FHyperAIGameplayAIAssetRecord& Record : Snapshot.Records)
	{
		AppendToken(Canonical, Record.StableId);
		AppendToken(Canonical, Record.Revision);
	}
	for (const FHyperAIGameplayAIIssue& Issue : Snapshot.CaptureIssues)
	{
		AppendToken(Canonical, Issue.Code);
		AppendToken(Canonical, Issue.Severity);
		AppendToken(Canonical, Issue.Variant);
		AppendToken(Canonical, Issue.AssetPath);
		AppendToken(Canonical, Issue.StableId);
		AppendInt(Canonical, Issue.OperationIndex);
		AppendToken(Canonical, Issue.Message);
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Snapshot.Revision)) Snapshot.bComplete = false;
	return Snapshot.Revision;
}

TArray<FHyperAIGameplayAIIssue> FHyperAIStudioGameplayAIContracts::ValidateValueSnapshot(
	const FHyperAIStudioGameplayAIValueSnapshot& Snapshot,
	const bool bRequirePackagesClean,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	TArray<FHyperAIGameplayAIIssue> Issues;
	if (!Snapshot.bComplete)
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("snapshot_incomplete"), TEXT("error"),
			TEXT("all"), FString(), TEXT("snapshot"), -1,
			TEXT("Independent validation requires a complete bounded loaded-state capture."));
	}
	if (!IsCanonicalSha256(Snapshot.ScopeFingerprint))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("snapshot_scope_invalid"),
			TEXT("error"), TEXT("all"), FString(), TEXT("snapshot"), -1,
			TEXT("Snapshot scope fingerprint is missing or invalid."));
	}
	if (!IsCanonicalSha256(Snapshot.Revision))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("snapshot_revision_invalid"),
			TEXT("error"), TEXT("all"), FString(), TEXT("snapshot"), -1,
			TEXT("Snapshot revision is missing or is not a canonical sha256 identity."));
	}
	for (const auto& CaptureIssue : Snapshot.CaptureIssues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *CaptureIssue.Code, *CaptureIssue.Severity,
			CaptureIssue.Variant, CaptureIssue.AssetPath, CaptureIssue.StableId,
			CaptureIssue.OperationIndex, CaptureIssue.Message);
	}
	TSet<FString> StableIds;
	TMap<FString, FString> ParentByPath;
	for (const FHyperAIGameplayAIAssetRecord& Record : Snapshot.Records)
	{
		auto Add = [&](const TCHAR* Code, const TCHAR* Severity, const FString& StableId,
			const FString& Message)
		{
			AddIssue(Issues, Maximum, bOutTruncated, Code, Severity, Record.Variant,
				Record.AssetPath, StableId.IsEmpty() ? Record.StableId : StableId, -1, Message);
		};
		if (Record.StableId.IsEmpty() || StableIds.Contains(Record.StableId))
			Add(TEXT("duplicate_or_missing_stable_id"), TEXT("error"), Record.StableId,
				TEXT("Every captured asset requires one unique stable identity."));
		StableIds.Add(Record.StableId);
		if (!IsCanonicalProjectObjectPath(Record.AssetPath))
			Add(TEXT("invalid_asset_identity"), TEXT("error"), Record.StableId,
				TEXT("Captured asset identity is not one canonical project object path."));
		if (!Record.bRevisionComplete || !IsCanonicalSha256(Record.Revision))
			Add(TEXT("revision_incomplete"), TEXT("error"), Record.StableId,
				TEXT("Independent validation requires one complete canonical record revision."));
		if (bRequirePackagesClean && Record.bPackageDirty)
			Add(TEXT("package_dirty"), TEXT("error"), Record.StableId,
				TEXT("Validation requires a clean package but the loaded package is dirty."));

		if (Record.Variant == TEXT("behavior_tree"))
		{
			if (Record.StableId != TEXT("behavior-tree:") + Record.AssetPath)
				Add(TEXT("behavior_tree_stable_id_mismatch"), TEXT("error"), Record.StableId,
					TEXT("Behavior Tree stable identity is not derived from its exact asset path."));
			TSet<FString> NodeIds;
			int32 RootNodeCount = 0;
			for (const auto& Node : Record.Nodes)
			{
				if (Node.Kind == TEXT("root")) ++RootNodeCount;
				if (Node.StableId.IsEmpty() || NodeIds.Contains(Node.StableId))
					Add(TEXT("duplicate_behavior_node"), TEXT("error"), Node.StableId,
						TEXT("Behavior Tree graph node identities must be unique."));
				NodeIds.Add(Node.StableId);
				if (!Node.StableId.StartsWith(TEXT("bt-node:") + Record.AssetPath + TEXT(":"),
					ESearchCase::CaseSensitive))
					Add(TEXT("behavior_node_stable_id_mismatch"), TEXT("error"), Node.StableId,
						TEXT("Behavior Tree node identity is not bound to its owning asset."));
				if (!Node.Error.IsEmpty())
					Add(TEXT("behavior_node_error"), TEXT("error"), Node.StableId, Node.Error);
				if (Node.RuntimeNodeClassPath.IsEmpty() && Node.Kind != TEXT("root")
					&& Node.Kind != TEXT("composite_decorator") && Node.Kind != TEXT("comment"))
					Add(TEXT("runtime_node_missing"), TEXT("error"), Node.StableId,
						TEXT("Non-root graph node has no loaded runtime node instance."));
			}
			if (RootNodeCount != 1)
				Add(TEXT("behavior_tree_root_count_invalid"), TEXT("error"), Record.StableId,
					TEXT("Behavior Tree editor graph must contain exactly one root node."));
			if (!Record.BlackboardPath.IsEmpty()
				&& !IsCanonicalProjectObjectPath(Record.BlackboardPath))
				Add(TEXT("invalid_blackboard_reference"), TEXT("error"), Record.StableId,
					TEXT("Behavior Tree blackboard reference is not a canonical project asset path."));
			if (!Record.BehaviorTreeGraphBlackboardPath.IsEmpty()
				&& !IsCanonicalProjectObjectPath(Record.BehaviorTreeGraphBlackboardPath))
				Add(TEXT("invalid_graph_blackboard_reference"), TEXT("error"), Record.StableId,
					TEXT("Behavior Tree graph-root blackboard is not a canonical project asset path."));
			if (Record.BlackboardPath != Record.BehaviorTreeGraphBlackboardPath)
				Add(TEXT("behavior_tree_blackboard_link_mismatch"), TEXT("error"), Record.StableId,
					TEXT("Behavior Tree runtime and editor-root Blackboard references are not linked consistently."));
		}
		else if (Record.Variant == TEXT("blackboard"))
		{
			if (Record.StableId != TEXT("blackboard:") + Record.AssetPath)
				Add(TEXT("blackboard_stable_id_mismatch"), TEXT("error"), Record.StableId,
					TEXT("Blackboard stable identity is not derived from its exact asset path."));
			ParentByPath.Add(Record.AssetPath, Record.ParentAssetPath);
			TSet<FString> Names;
			for (const auto& Key : Record.Keys)
			{
				const FString CanonicalName = Key.Name.ToLower();
				if (Key.Name.IsEmpty() || Key.Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)
					|| !FName::IsValidXName(Key.Name, INVALID_OBJECTNAME_CHARACTERS)
					|| Names.Contains(CanonicalName))
					Add(TEXT("duplicate_or_missing_blackboard_key"), TEXT("error"), Key.StableId,
						TEXT("Local Blackboard key names must be present and unique."));
				Names.Add(CanonicalName);
				if (Key.StableId != TEXT("blackboard-key:") + Record.AssetPath + TEXT(":") + Key.Name)
					Add(TEXT("blackboard_key_stable_id_mismatch"), TEXT("error"), Key.StableId,
						TEXT("Blackboard key identity is not bound to its owning asset and key name."));
				if (Key.KeyType.IsEmpty())
					Add(TEXT("blackboard_key_type_missing"), TEXT("error"), Key.StableId,
						TEXT("Blackboard key has no loaded key type."));
			}
			if (!Record.ParentAssetPath.IsEmpty()
				&& (!IsCanonicalProjectObjectPath(Record.ParentAssetPath)
					|| Record.ParentAssetPath.Equals(Record.AssetPath, ESearchCase::IgnoreCase)))
				Add(TEXT("invalid_blackboard_parent"), TEXT("error"), Record.StableId,
					TEXT("Blackboard parent must be a distinct canonical project asset."));
		}
		else if (Record.Variant == TEXT("ai_controller_perception"))
		{
			if (Record.StableId != TEXT("ai-controller:") + Record.AssetPath)
				Add(TEXT("ai_controller_stable_id_mismatch"), TEXT("error"), Record.StableId,
					TEXT("AI Controller stable identity is not derived from its exact asset path."));
			if (Record.GeneratedClassPath.IsEmpty() || Record.ParentClassPath.IsEmpty())
				Add(TEXT("ai_controller_class_identity_missing"), TEXT("error"), Record.StableId,
					TEXT("AI Controller Blueprint must expose loaded generated and parent class identities."));
			if (!Record.BlackboardPath.IsEmpty()
				&& !IsCanonicalProjectObjectPath(Record.BlackboardPath))
				Add(TEXT("invalid_ai_controller_blackboard_reference"), TEXT("error"),
					Record.StableId,
					TEXT("AI Controller default Blackboard is not a canonical project asset path."));
			if (Record.BlueprintStatus == TEXT("compile_error"))
				Add(TEXT("ai_controller_compile_error"), TEXT("error"), Record.StableId,
					TEXT("AI Controller Blueprint has compiler errors."));
			else if (Record.BlueprintStatus == TEXT("up_to_date_with_warnings"))
				Add(TEXT("ai_controller_compile_warning"), TEXT("warning"), Record.StableId,
					TEXT("AI Controller Blueprint is compiled but still has warnings."));
			else if (Record.BlueprintStatus == TEXT("dirty_compile_required")
				|| Record.BlueprintStatus == TEXT("unknown")
				|| Record.BlueprintStatus == TEXT("being_created"))
				Add(TEXT("ai_controller_compile_state_unproven"), TEXT("warning"), Record.StableId,
					TEXT("AI Controller Blueprint compile state is not final."));
			else if (Record.BlueprintStatus != TEXT("up_to_date"))
				Add(TEXT("ai_controller_compile_state_invalid"), TEXT("error"), Record.StableId,
					TEXT("AI Controller record has an invalid Blueprint compile-state value."));
			TSet<FString> SenseClasses;
			TSet<FString> SenseIds;
			int32 DominantSenseCount = 0;
			for (const auto& Sense : Record.Senses)
			{
				if (Sense.bDominant) ++DominantSenseCount;
				if (Sense.StableId.IsEmpty() || SenseIds.Contains(Sense.StableId))
					Add(TEXT("duplicate_or_missing_perception_sense_id"), TEXT("error"),
						Sense.StableId, TEXT("Perception sense identities must be present and unique."));
				SenseIds.Add(Sense.StableId);
				if (!Sense.StableId.StartsWith(TEXT("ai-sense:") + Record.AssetPath + TEXT(":"),
					ESearchCase::CaseSensitive))
					Add(TEXT("perception_sense_stable_id_mismatch"), TEXT("error"),
						Sense.StableId,
						TEXT("Perception sense identity is not bound to its owning controller."));
				if (Sense.ConfigClassPath.IsEmpty() || Sense.SenseClassPath.IsEmpty())
					Add(TEXT("perception_sense_incomplete"), TEXT("error"), Sense.StableId,
						TEXT("Perception config must resolve to one loaded config and sense class."));
				if (!Sense.SenseClassPath.IsEmpty() && SenseClasses.Contains(Sense.SenseClassPath))
					Add(TEXT("duplicate_perception_sense"), TEXT("warning"), Sense.StableId,
						TEXT("AI Controller config contains duplicate sense implementations."));
				SenseClasses.Add(Sense.SenseClassPath);
				if (!FMath::IsFinite(Sense.MaxAgeSeconds) || Sense.MaxAgeSeconds < 0.0)
					Add(TEXT("perception_max_age_invalid"), TEXT("error"), Sense.StableId,
						TEXT("Perception max age must be finite and non-negative."));
			}
			if (!Record.DominantSenseClassPath.IsEmpty()
				&& !SenseClasses.Contains(Record.DominantSenseClassPath))
				Add(TEXT("dominant_sense_not_configured"), TEXT("error"), Record.StableId,
					TEXT("Dominant perception sense is not present in the configured sense set."));
			if ((Record.DominantSenseClassPath.IsEmpty() && DominantSenseCount != 0)
				|| (!Record.DominantSenseClassPath.IsEmpty() && DominantSenseCount != 1))
				Add(TEXT("dominant_sense_marker_mismatch"), TEXT("error"), Record.StableId,
					TEXT("Exactly one configured sense must match a non-empty dominant-sense identity."));
		}
		else
		{
			Add(TEXT("unknown_record_variant"), TEXT("error"), Record.StableId,
				TEXT("Snapshot contains a record outside the closed base variant set."));
		}
	}

	for (const auto& Pair : ParentByPath)
	{
		TSet<FString> Seen;
		FString Current = Pair.Key;
		while (!Current.IsEmpty())
		{
			if (Seen.Contains(Current))
			{
				const FHyperAIGameplayAIAssetRecord* Record = Snapshot.Records.FindByPredicate(
					[&](const auto& Candidate) { return Candidate.AssetPath == Pair.Key; });
				if (Record)
					AddIssue(Issues, Maximum, bOutTruncated, TEXT("blackboard_parent_cycle"),
						TEXT("error"), TEXT("blackboard"), Pair.Key, Record->StableId, -1,
						TEXT("Captured Blackboard parent chain contains a cycle."));
				break;
			}
			Seen.Add(Current);
			const FString* Parent = ParentByPath.Find(Current);
			Current = Parent ? *Parent : FString();
		}
	}
	return Issues;
}

FString FHyperAIStudioGameplayAIContracts::ComputePayloadSemanticFingerprint(
	const TArray<FHyperAIStudioGameplayAIBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	if (!IsCanonicalSha256(BaseRevision) || Operations.IsEmpty() || Operations.Num() > MaxOperations)
		return FString();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gameplay-ai-payload.v1"));
	AppendToken(Canonical, BaseRevision);
	AppendInt(Canonical, Operations.Num());
	for (const auto& Operation : Operations)
	{
		if (!IsClosedBackendOperation(Operation)) return FString();
		AppendToken(Canonical, BackendOperationCanonical(Operation));
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioGameplayAIContracts::ComputeStageId(
	const FString& CanonicalProjectId,
	const FString& OperationId,
	const FString& PlanHash,
	const FString& EffectFingerprint)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	if (CanonicalProjectId.IsEmpty()
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(OperationId)
		|| !IsCanonicalSha256(PlanHash) || !IsCanonicalSha256(EffectFingerprint))
	{
		return FString();
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gameplay-ai-stage.v1"));
	AppendToken(Canonical, CanonicalProjectId);
	AppendToken(Canonical, OperationId);
	AppendToken(Canonical, PlanHash);
	AppendToken(Canonical, EffectFingerprint);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIGameplayAIApplyPlanReport FHyperAIStudioGameplayAIContracts::BuildPlan(
	const FHyperAIGameplayAIApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	FHyperAIGameplayAIApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Gameplay AI UObject planning is allowed only on Unreal's serialized game thread.");
		return Report;
	}
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 2000
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.OperationId.Len() > FHyperAIStudioDomainLimits::MaxOperationIdChars
		|| Request.ExpectedPlanHash.Len() > 71)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Gameplay AI plan violates operation, deadline, game-thread, or output bounds.");
		return Report;
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		Report.Status = TEXT("invalid_dry_run_shape");
		Report.Diagnostic = TEXT("Dry-run requests must leave operation_id and expected_plan_hash empty.");
		return Report;
	}
	int32 EstimatedOutputBytes = 2048;
	for (const FHyperAIGameplayAIVariantStatus& Status
		: FHyperAIStudioGameplayAIFacade::ResolveVariantStatuses())
	{
		const int32 StatusBytes = EstimateVariantBytes(Status);
		if (EstimatedOutputBytes + StatusBytes + 4096 > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutputBytes += StatusBytes;
		Report.Variants.Add(Status);
	}
	const int32 IssueLimit = FMath::Clamp(
		(Request.MaxOutputBytes - EstimatedOutputBytes) / 3072, 1, MaxIssues);

	TArray<FHyperAIStudioGameplayAIBackendOperation> Operations;
	TArray<FString> TargetPaths;
	TSet<FString> TargetSet;
	TSet<FString> ExactOperations;
	bool bSawUnavailableVariant = false;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioGameplayAIBackendOperation Backend;
		FString ErrorCode;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Backend, ErrorCode, Error))
		{
			bSawUnavailableVariant |= ErrorCode == TEXT("variant_adapter_unavailable");
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, *ErrorCode, TEXT("error"),
				TEXT("apply_plan"), Request.Operations[Index].TargetPath,
				TEXT("gameplay-ai-operation:") + FString::FromInt(Index), Index, Error);
			continue;
		}
		const FString Exact = BackendOperationCanonical(Backend);
		if (ExactOperations.Contains(Exact))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("duplicate_operation"),
				TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
				TEXT("gameplay-ai-operation:") + FString::FromInt(Index), Index,
				TEXT("Exact duplicate operations are rejected."));
			continue;
		}
		ExactOperations.Add(Exact);
		if (!TargetSet.Contains(Backend.TargetPath))
		{
			TargetSet.Add(Backend.TargetPath);
			TargetPaths.Add(Backend.TargetPath);
		}
		Operations.Add(MoveTemp(Backend));
	}
	if (HasError(Report.Issues) || Operations.Num() != Request.Operations.Num())
	{
		Report.Status = bSawUnavailableVariant
			? TEXT("variant_adapter_unavailable") : TEXT("invalid_plan_shape");
		Report.Diagnostic = TEXT("One or more closed Gameplay AI operation variants were invalid or unavailable.");
		return Report;
	}

	FHyperAIStudioGameplayAIValueSnapshot BaseSnapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioGameplayAIFacade::CaptureLoaded(TargetPaths, TEXT("all"), true,
		BaseSnapshot, CaptureStatus, CaptureDiagnostic) || !BaseSnapshot.bComplete)
	{
		Report.Status = TEXT("target_state_incomplete");
		Report.Diagnostic = TEXT("Every target must be already loaded with a complete base-variant revision.");
		bool bCaptureValidationTruncated = false;
		const TArray<FHyperAIGameplayAIIssue> CaptureValidation = ValidateValueSnapshot(
			BaseSnapshot, false, IssueLimit, bCaptureValidationTruncated);
		Report.bTruncated |= bCaptureValidationTruncated;
		for (const FHyperAIGameplayAIIssue& Issue : CaptureValidation)
		{
			AppendIssueCopy(Report.Issues, IssueLimit, Report.bTruncated, Issue);
		}
		return Report;
	}
	TMap<FString, FHyperAIGameplayAIAssetRecord> OriginalByPath;
	TMap<FString, FHyperAIGameplayAIAssetRecord> SimulatedByPath;
	for (const auto& Record : BaseSnapshot.Records)
	{
		OriginalByPath.Add(Record.AssetPath, Record);
		SimulatedByPath.Add(Record.AssetPath, Record);
	}
	if (OriginalByPath.Num() != TargetPaths.Num())
	{
		Report.Status = TEXT("target_state_incomplete");
		Report.Diagnostic = TEXT("One or more exact target paths did not resolve to a base Gameplay AI asset.");
		return Report;
	}

	TSet<FString> BehaviorTreeTargets;
	TSet<FString> BlackboardTargets;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const auto& Operation = Operations[Index];
		FHyperAIGameplayAIAssetRecord* Original = OriginalByPath.Find(Operation.TargetPath);
		FHyperAIGameplayAIAssetRecord* Simulated = SimulatedByPath.Find(Operation.TargetPath);
		if (!Original || !Simulated || Original->Revision != Operation.ExpectedRevision)
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("revision_precondition_failed"),
				TEXT("error"), TEXT("apply_plan"), Operation.TargetPath,
				TEXT("gameplay-ai-operation:") + FString::FromInt(Index), Index,
				TEXT("Target is missing or its fresh loaded revision no longer matches expected_revision."));
			continue;
		}
		const bool bBehaviorOperation = Operation.Kind
			== EHyperAIStudioGameplayAIOperationKind::SetBehaviorTreeBlackboard;
		if (bBehaviorOperation && Original->Variant != TEXT("behavior_tree"))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("target_variant_mismatch"),
				TEXT("error"), Original->Variant, Operation.TargetPath, Original->StableId, Index,
				TEXT("Behavior Tree operation target is not a loaded Behavior Tree."));
			continue;
		}
		if (!bBehaviorOperation && Original->Variant != TEXT("blackboard"))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("target_variant_mismatch"),
				TEXT("error"), Original->Variant, Operation.TargetPath, Original->StableId, Index,
				TEXT("Blackboard operation target is not a loaded Blackboard."));
			continue;
		}

		if (Operation.Kind == EHyperAIStudioGameplayAIOperationKind::SetBehaviorTreeBlackboard)
		{
			UBlackboardData* ReferencedBlackboard = Operation.ReferencePath.IsEmpty()
				? nullptr : FindObject<UBlackboardData>(nullptr, *Operation.ReferencePath);
			if (!Operation.ReferencePath.IsEmpty()
				&& (!IsValid(ReferencedBlackboard)
					|| ReferencedBlackboard->GetPathName() != Operation.ReferencePath))
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
					TEXT("blackboard_reference_not_loaded"), TEXT("error"), Original->Variant,
					Operation.TargetPath, Original->StableId, Index,
					TEXT("Referenced Blackboard is not already loaded; no disk load is allowed."));
				continue;
			}
			Simulated->BlackboardPath = Operation.ReferencePath;
			Simulated->BehaviorTreeGraphBlackboardPath = Operation.ReferencePath;
			BehaviorTreeTargets.Add(Operation.TargetPath);
		}
		else if (Operation.Kind == EHyperAIStudioGameplayAIOperationKind::AddBlackboardKey)
		{
			const FString& Name = Operation.Name;
			if (Simulated->Keys.ContainsByPredicate([&](const auto& Key)
			{
				return Key.Name.Equals(Name, ESearchCase::IgnoreCase);
			}))
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("blackboard_key_exists"),
					TEXT("error"), Original->Variant, Operation.TargetPath, Original->StableId, Index,
					TEXT("Cannot add a duplicate local Blackboard key."));
				continue;
			}
			FHyperAIGameplayAIKeyView Key;
			Key.Name = Name;
			Key.StableId = TEXT("blackboard-key:") + Operation.TargetPath + TEXT(":") + Name;
			Key.KeyType = TEXT("closed-key-type:") + Operation.KeyType;
			Key.bInstanceSynced = Operation.bInstanceSynced;
			Simulated->Keys.Add(MoveTemp(Key));
			BlackboardTargets.Add(Operation.TargetPath);
			++Report.Effects.KeysAdded;
		}
		else if (Operation.Kind == EHyperAIStudioGameplayAIOperationKind::RemoveBlackboardKey)
		{
			const FString& Name = Operation.Name;
			const int32 Removed = Simulated->Keys.RemoveAll([&](const auto& Key)
			{
				return Key.Name.Equals(Name, ESearchCase::IgnoreCase);
			});
			if (Removed != 1)
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("blackboard_key_missing"),
					TEXT("error"), Original->Variant, Operation.TargetPath, Original->StableId, Index,
					TEXT("Exactly one local Blackboard key must match remove_blackboard_key."));
				continue;
			}
			BlackboardTargets.Add(Operation.TargetPath);
			++Report.Effects.KeysRemoved;
		}
		else if (Operation.Kind == EHyperAIStudioGameplayAIOperationKind::RenameBlackboardKey)
		{
			const FString& OldName = Operation.Name;
			const FString& NewName = Operation.NewName;
			FHyperAIGameplayAIKeyView* Key = Simulated->Keys.FindByPredicate(
				[&](const auto& Candidate)
				{
					return Candidate.Name.Equals(OldName, ESearchCase::IgnoreCase);
				});
			if (!Key || Simulated->Keys.ContainsByPredicate(
				[&](const auto& Candidate)
				{
					return Candidate.Name.Equals(NewName, ESearchCase::IgnoreCase);
				}))
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("blackboard_rename_conflict"),
					TEXT("error"), Original->Variant, Operation.TargetPath, Original->StableId, Index,
					TEXT("Rename requires one existing old name and an unused new name."));
				continue;
			}
			Key->Name = NewName;
			Key->StableId = TEXT("blackboard-key:") + Operation.TargetPath + TEXT(":") + NewName;
			BlackboardTargets.Add(Operation.TargetPath);
			++Report.Effects.KeysRenamed;
		}
		else if (Operation.Kind == EHyperAIStudioGameplayAIOperationKind::SetBlackboardParent)
		{
			if (!Operation.ReferencePath.IsEmpty())
			{
				UBlackboardData* Parent = FindObject<UBlackboardData>(nullptr, *Operation.ReferencePath);
				UBlackboardData* Target = FindObject<UBlackboardData>(nullptr, *Operation.TargetPath);
				if (!IsValid(Parent) || !IsValid(Target)
					|| Parent->GetPathName() != Operation.ReferencePath
					|| Target->GetPathName() != Operation.TargetPath || Parent->IsChildOf(*Target))
				{
					AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
						TEXT("blackboard_parent_unavailable_or_cyclic"), TEXT("error"),
						Original->Variant, Operation.TargetPath, Original->StableId, Index,
						TEXT("Parent must be loaded and must not create a Blackboard ancestry cycle."));
					continue;
				}
			}
			Simulated->ParentAssetPath = Operation.ReferencePath;
			BlackboardTargets.Add(Operation.TargetPath);
		}
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("plan_state_invalid");
		Report.Diagnostic = TEXT("Target type, loaded reference, key, or CAS preconditions failed.");
		return Report;
	}

	FHyperAIStudioGameplayAIValueSnapshot SimulatedSnapshot;
	SimulatedSnapshot.bComplete = true;
	SimulatedSnapshot.ScopeFingerprint = BaseSnapshot.ScopeFingerprint;
	SimulatedByPath.GenerateValueArray(SimulatedSnapshot.Records);
	ComputeSnapshotRevision(SimulatedSnapshot);
	bool bValidationTruncated = false;
	const TArray<FHyperAIGameplayAIIssue> ValidationIssues = ValidateValueSnapshot(
		SimulatedSnapshot, false, IssueLimit, bValidationTruncated);
	Report.bTruncated |= bValidationTruncated;
	for (const auto& Issue : ValidationIssues)
	{
		if (Issue.Severity == TEXT("error"))
			AppendIssueCopy(Report.Issues, IssueLimit, Report.bTruncated, Issue);
	}
	if (bValidationTruncated)
	{
		Report.Status = TEXT("postcondition_validation_truncated");
		Report.Diagnostic = TEXT("Pure postcondition validation exceeded the bounded evidence envelope and therefore failed closed.");
		return Report;
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("postcondition_invalid");
		Report.Diagnostic = TEXT("Pure simulation fails the independent Gameplay AI validator.");
		return Report;
	}

	TargetPaths.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.gameplay-ai-base.v1"));
	for (const FString& Path : TargetPaths)
	{
		AppendToken(BaseCanonical, Path);
		AppendToken(BaseCanonical, OriginalByPath.FindChecked(Path).Revision);
	}
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	const FString SemanticFingerprint = ComputePayloadSemanticFingerprint(Operations, Report.BaseRevision);
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (!IsCanonicalSha256(Report.BaseRevision) || !IsCanonicalSha256(SemanticFingerprint)
		|| ProjectId.IsEmpty())
	{
		Report.Status = TEXT("semantic_identity_unavailable");
		Report.Diagnostic = TEXT("Bounded semantic hashes or canonical project identity could not be produced.");
		return Report;
	}

	const auto Payload = MakeShared<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe>();
	Payload->Operations = Operations;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->SemanticFingerprint = SemanticFingerprint;
	const FHyperAIStudioDomainAdapterDescriptor& Adapter = GetBaseAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_gameplay_ai_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Adapter.AdapterFingerprint;
	// Stage-only sealing generation. The future async host must reseal against its live registry pin.
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.AIModule"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.AIGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.BehaviorTreeEditor"), EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = PayloadTypeId;
	Contract.ArtifactSchemaFingerprint = PayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = SemanticFingerprint;
	Contract.EffectTarget = TEXT("gameplay_ai:") + Report.BaseRevision;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = FMath::Clamp(Operations.Num() + 4, 5, 128);
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
	Contract.bCompileOnce = !BehaviorTreeTargets.IsEmpty();
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
	Report.Effects.OperationCount = Operations.Num();
	Report.Effects.TargetCount = TargetPaths.Num();
	Report.Effects.BehaviorTreesUpdated = BehaviorTreeTargets.Num();
	Report.Effects.BlackboardsUpdated = BlackboardTargets.Num();
	Report.Effects.bTransactionOnce = true;
	Report.Effects.bCompileOnce = Contract.bCompileOnce;
	Report.Effects.bSaveOnce = true;
	Report.Effects.bValidateOnce = true;
	Report.Effects.bFreshVerifyOnce = true;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("Closed base Gameplay AI edit plan, loaded-state CAS, simulated postconditions, and typed-artifact hashes are valid without mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Staging requires one journal-safe operation_id.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash || !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Staging must echo the exact dry-run plan hash.");
		return Report;
	}
	FHyperAIStudioGameplayAIStagedArtifact Artifact;
	Artifact.Prepared = Prepared;
	Artifact.Payload = Payload;
	Artifact.CanonicalProjectId = ProjectId;
	Artifact.OperationId = Request.OperationId;
	Artifact.StageId = ComputeStageId(ProjectId, Request.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	Artifact.ExpiresMonotonicMs = NowMonotonicMs() + StageLifetimeMs;
	FString StageError;
	if (!FHyperAIStudioGameplayAIStagingService::Stage(Artifact, Report.bReplay, StageError))
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
	Report.Diagnostic = TEXT("Plan is idempotently staged and side-effect-free; no mutation is submitted until the shared async PlanExecutionService host owns its journal and live adapter lease.");
	return Report;
}

FHyperAIGameplayAIInspectReport UHyperAIStudioGameplayAIToolset::hyper_gameplay_ai_inspect(
	const FHyperAIGameplayAIInspectRequest& Request)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	FHyperAIGameplayAIInspectReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Gameplay AI UObject inspection is allowed only on Unreal's serialized game thread.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioGameplayAIContracts::MaxAssetPaths
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioGameplayAIContracts::MaxPageSize
		|| Request.Variant.Len() > FHyperAIStudioDomainLimits::MaxVariantIdChars
		|| Request.Cursor.Len() > FHyperAIStudioGameplayAIContracts::MaxCursorCharacters
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioGameplayAIContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Inspect request violates path, paging, cursor, or output bounds.");
		return Report;
	}
	const TArray<FHyperAIGameplayAIVariantStatus> VariantStatuses = Request.bIncludePrerequisites
		? FHyperAIStudioGameplayAIFacade::ResolveVariantStatuses()
		: TArray<FHyperAIGameplayAIVariantStatus>();
	FHyperAIStudioGameplayAIValueSnapshot Snapshot;
	if (!FHyperAIStudioGameplayAIFacade::CaptureLoaded(Request.AssetPaths, Request.Variant,
		Request.bIncludeDetails, Snapshot, Report.Status, Report.Diagnostic))
	{
		int32 ErrorBytes = 768;
		for (const FHyperAIGameplayAIVariantStatus& Status : VariantStatuses)
		{
			const int32 StatusBytes = EstimateVariantBytes(Status);
			if (ErrorBytes + StatusBytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			ErrorBytes += StatusBytes;
			Report.Variants.Add(Status);
		}
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
	Report.TotalRecords = Snapshot.Records.Num();
	for (const auto& Record : Snapshot.Records)
	{
		if (Record.Variant == TEXT("behavior_tree")) ++Report.BehaviorTreeCount;
		else if (Record.Variant == TEXT("blackboard")) ++Report.BlackboardCount;
		else if (Record.Variant == TEXT("ai_controller_perception")) ++Report.AIControllerCount;
	}
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Snapshot.Revision, Offset) || Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("cursor_revision_mismatch");
		Report.Diagnostic = TEXT("Cursor is malformed, stale, or outside this immutable snapshot.");
		return Report;
	}
	int32 EstimatedBytes = 768;
	for (const FHyperAIGameplayAIVariantStatus& Status : VariantStatuses)
	{
		const int32 StatusBytes = EstimateVariantBytes(Status);
		if (EstimatedBytes + StatusBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += StatusBytes;
		Report.Variants.Add(Status);
	}
	for (const FHyperAIGameplayAIIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	int32 Index = Offset;
	for (; Index < Snapshot.Records.Num() && Report.Records.Num() < Request.PageSize; ++Index)
	{
		FHyperAIGameplayAIAssetRecord OutputRecord = Snapshot.Records[Index];
		int32 RecordBytes = EstimateRecordBytes(OutputRecord);
		if (EstimatedBytes + RecordBytes > Request.MaxOutputBytes)
		{
			OutputRecord.Nodes.Reset();
			OutputRecord.Keys.Reset();
			OutputRecord.Senses.Reset();
			OutputRecord.bOutputDetailsTruncated = true;
			RecordBytes = EstimateRecordBytes(OutputRecord);
			if (EstimatedBytes + RecordBytes > Request.MaxOutputBytes)
			{
				Report.Status = TEXT("record_exceeds_output_budget");
				Report.Diagnostic = TEXT("Even the bounded record identity projection exceeds max_output_bytes.");
				Report.bTruncated = true;
				return Report;
			}
			Report.bTruncated = true;
		}
		EstimatedBytes += RecordBytes;
		Report.Records.Add(MoveTemp(OutputRecord));
	}
	Report.ReturnedRecords = Report.Records.Num();
	if (Index < Snapshot.Records.Num())
	{
		Report.bTruncated = true;
		Report.NextCursor = MakeCursor(Snapshot.Revision, Index);
	}
	Report.bTruncated |= !Snapshot.bComplete;
	Report.bOk = true;
	Report.Status = Snapshot.bComplete ? TEXT("ok") : TEXT("partial_loaded_state");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a stable bounded page from an immutable loaded-only Gameplay AI capture.")
		: TEXT("Returned bounded loaded-state evidence with explicit incomplete-revision metadata.");
	return Report;
}

FHyperAIGameplayAIValidateReport UHyperAIStudioGameplayAIToolset::hyper_gameplay_ai_validate(
	const FHyperAIGameplayAIValidateRequest& Request)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	FHyperAIGameplayAIValidateReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Gameplay AI UObject validation is allowed only on Unreal's serialized game thread.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioGameplayAIContracts::MaxAssetPaths
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioGameplayAIContracts::MaxIssues
		|| Request.Variant.Len() > FHyperAIStudioDomainLimits::MaxVariantIdChars
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioGameplayAIContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Validate request violates path, issue, output, or expected-revision bounds.");
		return Report;
	}
	const TArray<FHyperAIGameplayAIVariantStatus> VariantStatuses = Request.bIncludePrerequisites
		? FHyperAIStudioGameplayAIFacade::ResolveVariantStatuses()
		: TArray<FHyperAIGameplayAIVariantStatus>();
	FHyperAIStudioGameplayAIValueSnapshot Snapshot;
	if (!FHyperAIStudioGameplayAIFacade::CaptureLoaded(Request.AssetPaths, Request.Variant,
		true, Snapshot, Report.Status, Report.Diagnostic))
	{
		int32 ErrorBytes = 768;
		for (const FHyperAIGameplayAIVariantStatus& Status : VariantStatuses)
		{
			const int32 StatusBytes = EstimateVariantBytes(Status);
			if (ErrorBytes + StatusBytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			ErrorBytes += StatusBytes;
			Report.Variants.Add(Status);
		}
		return Report;
	}
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	bool bValidationTruncated = false;
	TArray<FHyperAIGameplayAIIssue> AllIssues =
		FHyperAIStudioGameplayAIContracts::ValidateValueSnapshot(
			Snapshot, Request.bRequirePackagesClean, Request.MaxIssues, bValidationTruncated);
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(AllIssues, Request.MaxIssues, bValidationTruncated,
			TEXT("expected_revision_mismatch"), TEXT("error"), Request.Variant,
			FString(), TEXT("snapshot"), -1,
			TEXT("Fresh loaded-state revision does not match expected_revision."));
	}
	for (const auto& Issue : AllIssues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	int32 EstimatedBytes = 768;
	bool bOutputTruncated = false;
	for (const FHyperAIGameplayAIVariantStatus& Status : VariantStatuses)
	{
		const int32 StatusBytes = EstimateVariantBytes(Status);
		if (EstimatedBytes + StatusBytes > Request.MaxOutputBytes)
		{
			bOutputTruncated = true;
			break;
		}
		EstimatedBytes += StatusBytes;
		Report.Variants.Add(Status);
	}
	for (const FHyperAIGameplayAIIssue& Issue : AllIssues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			bOutputTruncated = true;
			break;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	Report.bTruncated = bValidationTruncated || bOutputTruncated;
	Report.bOk = true;
	Report.bValid = Snapshot.bComplete && Report.ErrorCount == 0 && !bValidationTruncated;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("validation_failed");
	Report.Diagnostic = Report.bValid
		? TEXT("Independent fresh loaded-state validation passed.")
		: TEXT("Independent validation found errors or incomplete revision evidence.");
	return Report;
}

FHyperAIGameplayAIApplyPlanReport UHyperAIStudioGameplayAIToolset::hyper_gameplay_ai_apply_plan(
	const FHyperAIGameplayAIApplyPlanRequest& Request)
{
	return FHyperAIStudioGameplayAIContracts::BuildPlan(Request);
}

FString FHyperAIStudioGameplayAITypedPayload::GetTypeId() const
{
	return FHyperAIStudioGameplayAIContracts::PayloadTypeId;
}

FString FHyperAIStudioGameplayAITypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGameplayAIContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioGameplayAITypedPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::GameplayAI::Private;
	int64 Bytes = 256 + (BaseRevision.Len() + SemanticFingerprint.Len()) * sizeof(TCHAR);
	for (const auto& Operation : Operations)
	{
		Bytes += BackendOperationCanonical(Operation).Len() * sizeof(TCHAR);
		if (Bytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
			return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	}
	return static_cast<int32>(Bytes);
}

FString FHyperAIStudioGameplayAITypedPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioGameplayAITypedPayload::CloneImmutable() const
{
	TSharedRef<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe>();
	Clone->Operations = Operations;
	Clone->BaseRevision = BaseRevision;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioGameplayAIResultPayload::GetTypeId() const
{
	return FHyperAIStudioGameplayAIContracts::ResultTypeId;
}

FString FHyperAIStudioGameplayAIResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGameplayAIContracts::ResultSchemaFingerprint();
}

int32 FHyperAIStudioGameplayAIResultPayload::GetBoundedByteSize() const
{
	return 96 + (Phase.Len() + Revision.Len()) * sizeof(TCHAR);
}

FHyperAIStudioGameplayAIDomainAdapter::FHyperAIStudioGameplayAIDomainAdapter()
	: Descriptor(FHyperAIStudioGameplayAIContracts::GetBaseAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioGameplayAIDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioGameplayAIDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	if (Context.Binding.PackId != Descriptor.PackId
		|| Context.Binding.ToolName != TEXT("hyper_gameplay_ai_apply_plan")
		|| Context.Binding.VariantId != FHyperAIStudioGameplayAIContracts::MutationVariantId
		|| Context.Safety != EHyperAIStudioDomainSafety::Edit
		|| Payload.GetTypeId() != FHyperAIStudioGameplayAIContracts::PayloadTypeId
		|| Payload.GetSchemaFingerprint()
			!= FHyperAIStudioGameplayAIContracts::PayloadSchemaFingerprint())
	{
		Result.StatusCode = TEXT("typed_binding_mismatch");
		Result.Diagnostic = TEXT("Gameplay AI adapter rejected a non-exact pack, variant, safety, or DTO binding.");
		return Result;
	}
	Result.StatusCode = TEXT("async_host_required");
	Result.Diagnostic = TEXT("Base adapter is side-effect-free until PlanExecutionService owns the live lease, journal, transaction, save, validate, and fresh-verify lifecycle.");
	return Result;
}

bool FHyperAIStudioGameplayAIStagingService::Stage(
	const FHyperAIStudioGameplayAIStagedArtifact& Artifact,
	bool& bOutReplay,
	FString& OutError)
{
	using namespace HyperAIStudio::GameplayAI::Private;
	bOutReplay = false;
	OutError.Reset();
	const int64 NowMs = NowMonotonicMs();
	if (Artifact.CanonicalProjectId.IsEmpty()
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(Artifact.OperationId)
		|| !FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(Artifact.StageId)
		|| !Artifact.Payload.IsValid()
		|| Artifact.Payload->GetTypeId() != FHyperAIStudioGameplayAIContracts::PayloadTypeId
		|| Artifact.Payload->GetSchemaFingerprint()
			!= FHyperAIStudioGameplayAIContracts::PayloadSchemaFingerprint()
		|| FHyperAIStudioGameplayAIContracts::ComputePayloadSemanticFingerprint(
			Artifact.Payload->Operations, Artifact.Payload->BaseRevision)
			!= Artifact.Payload->GetSemanticFingerprint()
		|| Artifact.Payload->GetSemanticFingerprint()
			!= Artifact.Prepared.Contract.ArtifactSemanticFingerprint
		|| Artifact.Prepared.Contract.ArtifactTypeId != Artifact.Payload->GetTypeId()
		|| Artifact.Prepared.Contract.ArtifactSchemaFingerprint
			!= Artifact.Payload->GetSchemaFingerprint()
		|| Artifact.Prepared.Contract.Binding.CanonicalProjectId != Artifact.CanonicalProjectId
		|| Artifact.Prepared.Contract.Binding.PackId != FHyperAIStudioGameplayAIContracts::PackId
		|| Artifact.Prepared.Contract.Binding.ToolName != TEXT("hyper_gameplay_ai_apply_plan")
		|| Artifact.Prepared.Contract.Binding.VariantId
			!= FHyperAIStudioGameplayAIContracts::MutationVariantId
		|| Artifact.Prepared.Contract.Binding.ExpectedSafety != EHyperAIStudioDomainSafety::Edit
		|| Artifact.Prepared.Contract.Binding.ExpectedAdapterFingerprint
			!= FHyperAIStudioGameplayAIContracts::GetBaseAdapterDescriptor().AdapterFingerprint
		|| !FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(Artifact.Prepared.PlanHash)
		|| !FHyperAIStudioGameplayAIContracts::IsCanonicalSha256(Artifact.Prepared.EffectFingerprint)
		|| Artifact.ExpiresMonotonicMs <= NowMs
		|| Artifact.ExpiresMonotonicMs > NowMs + StageLifetimeMs + 1000)
	{
		OutError = TEXT("Staged Gameplay AI artifact identity, DTO, hashes, safety, or lifetime are invalid.");
		return false;
	}
	if (Artifact.StageId != FHyperAIStudioGameplayAIContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId, Artifact.Prepared.PlanHash,
		Artifact.Prepared.EffectFingerprint))
	{
		OutError = TEXT("Staged Gameplay AI artifact has a non-exact stage identity.");
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
		OutError = TEXT("Staged Gameplay AI artifact failed exact shared-contract resealing.");
		return false;
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Artifact.Payload->CloneImmutable();
	if (&Detached.Get() == Artifact.Payload.Get()
		|| Detached->GetTypeId() != Artifact.Payload->GetTypeId()
		|| Detached->GetSchemaFingerprint() != Artifact.Payload->GetSchemaFingerprint()
		|| Detached->GetSemanticFingerprint() != Artifact.Payload->GetSemanticFingerprint()
		|| Detached->GetTypeId() != Artifact.Prepared.Contract.ArtifactTypeId
		|| Detached->GetSchemaFingerprint()
			!= Artifact.Prepared.Contract.ArtifactSchemaFingerprint
		|| Detached->GetSemanticFingerprint()
			!= Artifact.Prepared.Contract.ArtifactSemanticFingerprint
		|| Detached->GetBoundedByteSize() != Artifact.Payload->GetBoundedByteSize()
		|| Detached->GetBoundedByteSize() <= 0
		|| Detached->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		OutError = TEXT("Typed payload clone is aliased, drifted, empty, or outside the shared request bound.");
		return false;
	}
	const TSharedRef<const FHyperAIStudioGameplayAITypedPayload, ESPMode::ThreadSafe>
		DetachedConcrete =
			StaticCastSharedRef<const FHyperAIStudioGameplayAITypedPayload>(Detached);
	if (FHyperAIStudioGameplayAIContracts::ComputePayloadSemanticFingerprint(
		DetachedConcrete->Operations, DetachedConcrete->BaseRevision)
		!= DetachedConcrete->SemanticFingerprint)
	{
		OutError = TEXT("Detached typed payload failed concrete semantic recomputation.");
		return false;
	}
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMs);
	const FString Key = StageKey(Artifact.CanonicalProjectId, Artifact.OperationId);
	if (const FHyperAIStudioGameplayAIStagedArtifact* Existing = State.Artifacts.Find(Key))
	{
		if (Existing->Prepared.PlanHash != Artifact.Prepared.PlanHash
			|| Existing->Prepared.EffectFingerprint != Artifact.Prepared.EffectFingerprint
			|| Existing->Prepared.ContractFingerprint != Artifact.Prepared.ContractFingerprint
			|| Existing->Payload->GetSemanticFingerprint()
				!= DetachedConcrete->GetSemanticFingerprint()
			|| Existing->StageId != Artifact.StageId)
		{
			OutError = TEXT("operation_id is already staged with a different exact plan/effect binding.");
			return false;
		}
		bOutReplay = true;
		return true;
	}
	if (State.Artifacts.Num() >= MaxStagedArtifacts)
	{
		OutError = TEXT("Bounded Gameplay AI stage store is full; wait for expiry or async-host integration.");
		return false;
	}
	FHyperAIStudioGameplayAIStagedArtifact Stored = Artifact;
	Stored.Payload = DetachedConcrete;
	State.Artifacts.Add(Key, MoveTemp(Stored));
	return true;
}

int32 FHyperAIStudioGameplayAIStagingService::NumStaged()
{
	using namespace HyperAIStudio::GameplayAI::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMonotonicMs());
	return State.Artifacts.Num();
}

void FHyperAIStudioGameplayAIStagingService::Reset()
{
	using namespace HyperAIStudio::GameplayAI::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	State.Artifacts.Reset();
}

void FHyperAIStudioGameplayAIRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioGameplayAIRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioGameplayAIRegistration::Shutdown()
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
			UHyperAIStudioGameplayAIToolset::StaticClass(),
			FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioGameplayAI, Warning,
				TEXT("Could not unregister the owned Gameplay AI toolset: %s"), *Error);
		}
	}
	FHyperAIStudioGameplayAIStagingService::Reset();
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioGameplayAIRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioGameplayAIContracts::IsRegistrationAllowed(
			FHyperAIStudioGameplayAIContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGameplayAIToolset::StaticClass(),
			FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioGameplayAIRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioGameplayAIContracts::IsRegistrationAllowed(
			FHyperAIStudioGameplayAIContracts::IsPendingTestRegistrationEnabled())) return;
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioGameplayAIToolset::StaticClass(),
		FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioGameplayAIToolset::StaticClass(),
			FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioGameplayAI, Error,
				TEXT("ToolsetRegistry rejected the owned three-tool Gameplay AI cohort: %s"),
				*Error);
		}
	}
}
