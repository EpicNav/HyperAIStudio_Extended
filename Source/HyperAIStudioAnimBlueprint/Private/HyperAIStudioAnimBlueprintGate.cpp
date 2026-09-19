// Games by Hyper 2026.

#include "HyperAIStudioAnimBlueprintGate.h"

#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Factories/AnimBlueprintFactory.h"
#include "GameFramework/NavMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace HyperAIStudio::AnimBlueprint::Gate
{
	namespace
	{
		const FString DriverTag = TEXT("HyperAI driver: ");
		const FString DriverBlockTag = TEXT("HyperAI drivers");
		const FString PawnTag = TEXT("HyperAI pawn");
		const FName UpdateEvent(TEXT("BlueprintUpdateAnimation"));

		bool IsIdentifier(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > 64 || !(FChar::IsAlpha(Name[0]) || Name[0] == TEXT('_'))) return false;
			for (const TCHAR Char : Name)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
			}
			return true;
		}

		FString DriverType(const FString& Driver)
		{
			if (Driver == TEXT("speed")) return TEXT("float");
			if (Driver == TEXT("is_falling") || Driver == TEXT("is_crouching")) return TEXT("bool");
			return FString();
		}

		FString TypeOf(const FEdGraphPinType& Type)
		{
			if (Type.PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
			if (Type.PinCategory == UEdGraphSchema_K2::PC_Real || Type.PinCategory == UEdGraphSchema_K2::PC_Float
				|| Type.PinCategory == UEdGraphSchema_K2::PC_Double) return TEXT("float");
			return TEXT("other");
		}

		FEdGraphPinType MakeType(const FString& Type)
		{
			FEdGraphPinType PinType;
			if (Type == TEXT("bool"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			}
			else
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			}
			return PinType;
		}

		// ---------------------------------------------------------------- rules

		struct FCondition
		{
			FString Variable;
			bool bNegate = false;
			/** Empty for a bool test; otherwise >, <, >= or <=. */
			FString Op;
			double Number = 0.0;
		};

		struct FRule
		{
			bool bTimeRemaining = false;
			double TimeRemaining = 0.0;
			TArray<FCondition> Conditions;
		};

		bool ParseRule(const FString& Text, FRule& Out, FString& OutError)
		{
			Out = FRule();
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT("&&"));
			if (Parts.IsEmpty())
			{
				OutError = TEXT("a transition needs a rule, e.g. Speed > 10, IsFalling, !IsFalling, or time_remaining < 0.2.");
				return false;
			}
			for (FString Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (Part.StartsWith(TEXT("time_remaining")))
				{
					FString Number = Part.RightChop(14).TrimStartAndEnd();
					if (Parts.Num() != 1 || !Number.StartsWith(TEXT("<")) || !LexTryParseString(Out.TimeRemaining, *Number.RightChop(1).TrimStartAndEnd())
						|| Out.TimeRemaining < 0.0 || Out.TimeRemaining > 10.0)
					{
						OutError = TEXT("time_remaining must stand alone as time_remaining < seconds (0-10).");
						return false;
					}
					Out.bTimeRemaining = true;
					return true;
				}
				FCondition Condition;
				if (Part.StartsWith(TEXT("!")))
				{
					Condition.bNegate = true;
					Condition.Variable = Part.RightChop(1).TrimStartAndEnd();
				}
				else
				{
					for (const TCHAR* Op : {TEXT(">="), TEXT("<="), TEXT(">"), TEXT("<")})
					{
						int32 At = INDEX_NONE;
						if (Part.FindChar(Op[0], At) && Part.Mid(At, FCString::Strlen(Op)) == Op)
						{
							Condition.Op = Op;
							Condition.Variable = Part.Left(At).TrimStartAndEnd();
							if (!LexTryParseString(Condition.Number, *Part.RightChop(At + Condition.Op.Len()).TrimStartAndEnd()) || !FMath::IsFinite(Condition.Number))
							{
								OutError = FString::Printf(TEXT("'%s' needs a number after %s."), *Part, Op);
								return false;
							}
							break;
						}
					}
					if (Condition.Op.IsEmpty()) Condition.Variable = Part;
				}
				if (!IsIdentifier(Condition.Variable))
				{
					OutError = FString::Printf(TEXT("'%s' is not Var, !Var or Var > n."), *Part);
					return false;
				}
				Out.Conditions.Add(Condition);
			}
			return true;
		}

		const TCHAR* CompareFunction(const FString& Op)
		{
			if (Op == TEXT(">")) return TEXT("Greater_DoubleDouble");
			if (Op == TEXT("<")) return TEXT("Less_DoubleDouble");
			if (Op == TEXT(">=")) return TEXT("GreaterEqual_DoubleDouble");
			return TEXT("LessEqual_DoubleDouble");
		}

		// ---------------------------------------------------------------- graph lookups

		UEdGraph* FindAnimGraph(const UAnimBlueprint& Blueprint)
		{
			for (UEdGraph* Graph : Blueprint.FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph) return Graph;
			}
			return nullptr;
		}

		template <typename TNode>
		TNode* FindFirst(const UEdGraph& Graph)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (TNode* Typed = Cast<TNode>(Node)) return Typed;
			}
			return nullptr;
		}

		UEdGraphPin* PosePin(const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
		{
			for (UEdGraphPin* Pin : Node.Pins)
			{
				const UObject* Struct = Pin ? Pin->PinType.PinSubCategoryObject.Get() : nullptr;
				if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && Struct
					&& (Struct->GetFName() == TEXT("PoseLink") || Struct->GetFName() == TEXT("ComponentSpacePoseLink")))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		TArray<UAnimGraphNode_StateMachine*> Machines(const UAnimBlueprint& Blueprint)
		{
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			TArray<UAnimGraphNode_StateMachine*> Found;
			for (const UEdGraph* Graph : Graphs)
			{
				for (UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
				{
					UAnimGraphNode_StateMachine* Machine = Cast<UAnimGraphNode_StateMachine>(Node);
					if (Machine && Machine->EditorStateMachineGraph) Found.Add(Machine);
				}
			}
			return Found;
		}

		UAnimGraphNode_StateMachine* FindMachine(const UAnimBlueprint& Blueprint, const FString& Name)
		{
			for (UAnimGraphNode_StateMachine* Machine : Machines(Blueprint))
			{
				if (Machine->GetStateMachineName().Equals(Name, ESearchCase::IgnoreCase)) return Machine;
			}
			return nullptr;
		}

		TArray<UAnimStateNode*> States(const UAnimationStateMachineGraph& Graph)
		{
			TArray<UAnimStateNode*> Found;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (UAnimStateNode* State = Cast<UAnimStateNode>(Node)) Found.Add(State);
			}
			return Found;
		}

		UAnimStateNode* FindState(const UAnimationStateMachineGraph& Graph, const FString& Name)
		{
			for (UAnimStateNode* State : States(Graph))
			{
				if (State->GetStateName().Equals(Name, ESearchCase::IgnoreCase)) return State;
			}
			return nullptr;
		}

		TArray<UAnimStateTransitionNode*> Transitions(const UAnimationStateMachineGraph& Graph)
		{
			TArray<UAnimStateTransitionNode*> Found;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node)) Found.Add(Transition);
			}
			return Found;
		}

		FString StateNameOf(const UAnimStateNodeBase* State)
		{
			return State ? State->GetStateName() : FString();
		}

		UAnimStateNodeBase* EntryState(const UAnimationStateMachineGraph& Graph)
		{
			const UEdGraphPin* Out = Graph.EntryNode && Graph.EntryNode->Pins.Num() > 0 ? Graph.EntryNode->Pins[0] : nullptr;
			return Out && Out->LinkedTo.Num() > 0 ? Cast<UAnimStateNodeBase>(Out->LinkedTo[0]->GetOwningNode()) : nullptr;
		}

		UEdGraphNode* LinkedNode(const UEdGraphPin* Pin)
		{
			return Pin && Pin->LinkedTo.Num() > 0 ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
		}

		FString VariableOnPin(const UEdGraphNode& Node, const TCHAR* PinName)
		{
			const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(LinkedNode(Node.FindPin(PinName, EGPD_Input)));
			return Get ? Get->VariableReference.GetMemberName().ToString() : FString();
		}

		/** The sequence player's loop flag is an editor-side property of its node struct, set the way the Details panel sets it. */
		FBoolProperty* LoopProperty(UAnimGraphNode_SequencePlayer& Player, void*& OutContainer)
		{
			FStructProperty* NodeProperty = FindFProperty<FStructProperty>(UAnimGraphNode_SequencePlayer::StaticClass(), TEXT("Node"));
			OutContainer = NodeProperty ? NodeProperty->ContainerPtrToValuePtr<void>(&Player) : nullptr;
			return NodeProperty ? FindFProperty<FBoolProperty>(NodeProperty->Struct, TEXT("bLoopAnimation")) : nullptr;
		}

		void ReadState(const UAnimStateNode& Node, FHyperAIAnimBlueprintState& Out)
		{
			Out.Name = Node.GetStateName();
			Out.Player = TEXT("empty");
			UAnimationStateGraph* Graph = Cast<UAnimationStateGraph>(Node.BoundGraph);
			UAnimGraphNode_StateResult* Result = Graph ? Graph->GetResultNode() : nullptr;
			UEdGraphNode* Source = Result ? LinkedNode(PosePin(*Result, EGPD_Input)) : nullptr;
			if (UAnimGraphNode_SequencePlayer* Sequence = Cast<UAnimGraphNode_SequencePlayer>(Source))
			{
				Out.Player = TEXT("sequence");
				Out.Asset = Sequence->GetAnimationAsset() ? Sequence->GetAnimationAsset()->GetPathName() : FString();
				void* Container = nullptr;
				if (FBoolProperty* Loop = LoopProperty(*Sequence, Container)) Out.bLooping = Loop->GetPropertyValue_InContainer(Container);
			}
			else if (UAnimGraphNode_BlendSpacePlayer* Blend = Cast<UAnimGraphNode_BlendSpacePlayer>(Source))
			{
				Out.Player = TEXT("blend_space");
				Out.Asset = Blend->GetAnimationAsset() ? Blend->GetAnimationAsset()->GetPathName() : FString();
				Out.XVariable = VariableOnPin(*Blend, TEXT("X"));
				Out.YVariable = VariableOnPin(*Blend, TEXT("Y"));
			}
			else if (Source)
			{
				Out.Player = TEXT("custom");
			}
		}

		FString RuleOf(const UAnimStateTransitionNode& Transition)
		{
			if (!Transition.NodeComment.IsEmpty()) return Transition.NodeComment;
			if (Transition.bAutomaticRuleBasedOnSequencePlayerInState)
			{
				return FString::Printf(TEXT("time_remaining < %g"), Transition.AutomaticRuleTriggerTime);
			}
			const UAnimationTransitionGraph* Graph = Cast<UAnimationTransitionGraph>(Transition.BoundGraph);
			UAnimGraphNode_TransitionResult* Result = Graph ? const_cast<UAnimationTransitionGraph*>(Graph)->GetResultNode() : nullptr;
			const UEdGraphPin* Can = Result ? Result->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;
			if (Can && Can->LinkedTo.Num() > 0) return TEXT("(custom)");
			return Can && Can->DefaultValue == TEXT("true") ? TEXT("(always)") : TEXT("(none)");
		}

		// ---------------------------------------------------------------- node building

		template <typename TNode>
		TNode* Place(UEdGraph& Graph, const int32 X, const int32 Y, TFunctionRef<void(TNode&)> Setup)
		{
			FGraphNodeCreator<TNode> Creator(Graph);
			TNode* Node = Creator.CreateNode(/*bSelectNewNode=*/false);
			Node->NodePosX = X;
			Node->NodePosY = Y;
			Setup(*Node);
			Creator.Finalize();
			return Node;
		}

		UK2Node_CallFunction* PlaceCall(UEdGraph& Graph, UClass* Owner, const TCHAR* Function, const int32 X, const int32 Y)
		{
			UFunction* Found = Owner ? Owner->FindFunctionByName(Function) : nullptr;
			check(Found);
			return Place<UK2Node_CallFunction>(Graph, X, Y, [Found](UK2Node_CallFunction& Node) { Node.SetFromFunction(Found); });
		}

		UEdGraphPin* PlaceGet(UEdGraph& Graph, const FString& Variable, const int32 X, const int32 Y)
		{
			UK2Node_VariableGet* Get = Place<UK2Node_VariableGet>(Graph, X, Y,
				[&Variable](UK2Node_VariableGet& Node) { Node.VariableReference.SetSelfMember(FName(*Variable)); });
			return Get->FindPin(FName(*Variable), EGPD_Output);
		}

		UEdGraphPin* Pin(UEdGraphNode& Node, const FName Name, const EEdGraphPinDirection Direction)
		{
			return Node.FindPin(Name, Direction);
		}

		void BuildRule(UAnimationTransitionGraph& Graph, const FRule& Rule)
		{
			UEdGraphPin* Can = Graph.GetResultNode()->FindPin(TEXT("bCanEnterTransition"), EGPD_Input);
			UEdGraphPin* Combined = nullptr;
			int32 Row = 0;
			for (const FCondition& Condition : Rule.Conditions)
			{
				const int32 Y = Row++ * 140;
				UEdGraphPin* Value = PlaceGet(Graph, Condition.Variable, -700, Y);
				if (Condition.bNegate || !Condition.Op.IsEmpty())
				{
					UK2Node_CallFunction* Test = PlaceCall(Graph, UKismetMathLibrary::StaticClass(),
						Condition.bNegate ? TEXT("Not_PreBool") : CompareFunction(Condition.Op), -420, Y);
					Value->MakeLinkTo(Pin(*Test, TEXT("A"), EGPD_Input));
					if (!Condition.Op.IsEmpty()) Pin(*Test, TEXT("B"), EGPD_Input)->DefaultValue = FString::SanitizeFloat(Condition.Number);
					Value = Pin(*Test, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
				}
				if (Combined)
				{
					UK2Node_CallFunction* And = PlaceCall(Graph, UKismetMathLibrary::StaticClass(), TEXT("BooleanAND"), -200, Y);
					Combined->MakeLinkTo(Pin(*And, TEXT("A"), EGPD_Input));
					Value->MakeLinkTo(Pin(*And, TEXT("B"), EGPD_Input));
					Value = Pin(*And, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
				}
				Combined = Value;
			}
			Combined->MakeLinkTo(Can);
		}

		void RemoveNodes(UAnimBlueprint& Blueprint, const TArray<UEdGraphNode*>& Nodes)
		{
			for (UEdGraphNode* Node : Nodes)
			{
				if (Node) FBlueprintEditorUtils::RemoveNode(&Blueprint, Node, /*bDontRecompile=*/true);
			}
		}

		void SetPlayer(UAnimBlueprint& Blueprint, UAnimStateNode& State, UAnimationAsset* Asset, const FString& X, const FString& Y, const bool bOnce)
		{
			UAnimationStateGraph* Graph = CastChecked<UAnimationStateGraph>(State.BoundGraph);
			UEdGraphPin* In = PosePin(*Graph->GetResultNode(), EGPD_Input);
			// Replace whatever feeds the state, and the variable reads that fed its axes.
			TArray<UEdGraphNode*> Old;
			for (UEdGraphPin* Linked : In->LinkedTo)
			{
				UEdGraphNode* Player = Linked->GetOwningNode();
				Old.Add(Player);
				for (UEdGraphPin* Input : Player->Pins)
				{
					if (Input->Direction == EGPD_Input && Cast<UK2Node_VariableGet>(LinkedNode(Input))) Old.AddUnique(LinkedNode(Input));
				}
			}
			In->BreakAllPinLinks();
			RemoveNodes(Blueprint, Old);
			if (!Asset) return;
			UEdGraphNode* Player = nullptr;
			if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
			{
				UAnimGraphNode_BlendSpacePlayer* Blend = Place<UAnimGraphNode_BlendSpacePlayer>(*Graph, -400, 0,
					[BlendSpace](UAnimGraphNode_BlendSpacePlayer& Node) { Node.SetAnimationAsset(BlendSpace); });
				if (!X.IsEmpty()) PlaceGet(*Graph, X, -700, 0)->MakeLinkTo(Blend->FindPin(TEXT("X"), EGPD_Input));
				if (!Y.IsEmpty()) PlaceGet(*Graph, Y, -700, 120)->MakeLinkTo(Blend->FindPin(TEXT("Y"), EGPD_Input));
				Player = Blend;
			}
			else
			{
				UAnimGraphNode_SequencePlayer* Sequence = Place<UAnimGraphNode_SequencePlayer>(*Graph, -400, 0,
					[Asset](UAnimGraphNode_SequencePlayer& Node) { Node.SetAnimationAsset(Asset); });
				void* Container = nullptr;
				if (FBoolProperty* Loop = LoopProperty(*Sequence, Container)) Loop->SetPropertyValue_InContainer(Container, !bOnce);
				Player = Sequence;
			}
			PosePin(*Player, EGPD_Output)->MakeLinkTo(In);
		}

		UK2Node_Event* FindUpdateEvent(UEdGraph& Graph)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (Event && Event->EventReference.GetMemberName() == UpdateEvent) return Event;
			}
			return nullptr;
		}

		template <typename TNode>
		TNode* FindTagged(const UEdGraph& Graph, const FString& Tag)
		{
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				TNode* Typed = Cast<TNode>(Node);
				if (Typed && Typed->NodeComment == Tag) return Typed;
			}
			return nullptr;
		}

		/** Splices Node's exec in after Pin: Pin -> Node -> whatever Pin led to before. */
		void SpliceAfter(UEdGraphPin* Pin, UEdGraphNode& Node)
		{
			const TArray<UEdGraphPin*> Previous = Pin->LinkedTo;
			Pin->BreakAllPinLinks();
			Pin->MakeLinkTo(Node.FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
			UEdGraphPin* Then = Node.FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			for (UEdGraphPin* Next : Previous) Then->MakeLinkTo(Next);
		}

		/** Every frame: if the pawn is valid, set each driven variable from it; the user's own update logic runs after. */
		void BindDriver(UAnimBlueprint& Blueprint, const FString& Variable, const FString& Driver)
		{
			UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(&Blueprint);
			UK2Node_Event* Event = FindUpdateEvent(*Graph);
			if (!Event)
			{
				int32 PosY = 0;
				Event = FKismetEditorUtilities::AddDefaultEventNode(&Blueprint, Graph, UpdateEvent, UAnimInstance::StaticClass(), PosY);
			}
			if (Event->IsAutomaticallyPlacedGhostNode() || !Event->IsNodeEnabled())
			{
				// A default event starts as a disabled placeholder until something is wired to it.
				Event->SetEnabledState(ENodeEnabledState::Enabled, false);
				Event->NodeComment.Reset();
				Event->bCommentBubbleVisible = false;
			}
			const int32 X = Event->NodePosX;
			const int32 Y = Event->NodePosY;
			UK2Node_CallFunction* Pawn = FindTagged<UK2Node_CallFunction>(*Graph, PawnTag);
			UK2Node_IfThenElse* Branch = FindTagged<UK2Node_IfThenElse>(*Graph, DriverBlockTag);
			if (!Branch)
			{
				Pawn = PlaceCall(*Graph, UAnimInstance::StaticClass(), TEXT("TryGetPawnOwner"), X, Y + 220);
				Pawn->NodeComment = PawnTag;
				UK2Node_CallFunction* Valid = PlaceCall(*Graph, UKismetSystemLibrary::StaticClass(), TEXT("IsValid"), X + 260, Y + 220);
				Pin(*Pawn, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output)->MakeLinkTo(Pin(*Valid, TEXT("Object"), EGPD_Input));
				Branch = Place<UK2Node_IfThenElse>(*Graph, X + 300, Y, [](UK2Node_IfThenElse&) {});
				Branch->NodeComment = DriverBlockTag;
				Pin(*Valid, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output)->MakeLinkTo(Pin(*Branch, UEdGraphSchema_K2::PN_Condition, EGPD_Input));
				UEdGraphPin* EventThen = Pin(*Event, UEdGraphSchema_K2::PN_Then, EGPD_Output);
				const TArray<UEdGraphPin*> Previous = EventThen->LinkedTo;
				SpliceAfter(EventThen, *Branch);
				for (UEdGraphPin* Next : Previous) Pin(*Branch, UEdGraphSchema_K2::PN_Else, EGPD_Output)->MakeLinkTo(Next);
			}
			const int32 Column = X + 600 + 40 * Graph->Nodes.Num() % 400;
			UEdGraphPin* PawnOut = Pin(*Pawn, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
			UEdGraphPin* Value = nullptr;
			if (Driver == TEXT("speed"))
			{
				UK2Node_CallFunction* Velocity = PlaceCall(*Graph, AActor::StaticClass(), TEXT("GetVelocity"), Column, Y + 360);
				PawnOut->MakeLinkTo(Pin(*Velocity, UEdGraphSchema_K2::PN_Self, EGPD_Input));
				// Ground speed: a fall or jump must not read as running.
				UK2Node_CallFunction* Length = PlaceCall(*Graph, UKismetMathLibrary::StaticClass(), TEXT("VSizeXY"), Column + 260, Y + 360);
				Pin(*Velocity, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output)->MakeLinkTo(Pin(*Length, TEXT("A"), EGPD_Input));
				Value = Pin(*Length, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
			}
			else
			{
				UK2Node_CallFunction* Movement = PlaceCall(*Graph, APawn::StaticClass(), TEXT("GetMovementComponent"), Column, Y + 360);
				PawnOut->MakeLinkTo(Pin(*Movement, UEdGraphSchema_K2::PN_Self, EGPD_Input));
				UK2Node_CallFunction* Query = PlaceCall(*Graph, UNavMovementComponent::StaticClass(),
					Driver == TEXT("is_falling") ? TEXT("IsFalling") : TEXT("IsCrouching"), Column + 260, Y + 360);
				Pin(*Movement, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output)->MakeLinkTo(Pin(*Query, UEdGraphSchema_K2::PN_Self, EGPD_Input));
				Value = Pin(*Query, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
			}
			UK2Node_VariableSet* Set = Place<UK2Node_VariableSet>(*Graph, Column + 520, Y,
				[&Variable](UK2Node_VariableSet& Node) { Node.VariableReference.SetSelfMember(FName(*Variable)); });
			Set->NodeComment = DriverTag + Driver;
			Value->MakeLinkTo(Set->FindPin(FName(*Variable), EGPD_Input));
			SpliceAfter(Pin(*Branch, UEdGraphSchema_K2::PN_Then, EGPD_Output), *Set);
		}

		// ---------------------------------------------------------------- plan model

		struct FStateModel
		{
			bool bPlaysSequence = false;
		};

		struct FMachineModel
		{
			TMap<FString, FStateModel> States;
			TSet<FString> Transitions;
		};

		struct FModel
		{
			TMap<FString, FString> Variables;
			TSet<FString> Driven;
			TMap<FString, FMachineModel> Machines;
		};

		FModel ModelOf(const UAnimBlueprint* Blueprint)
		{
			FModel Model;
			if (!Blueprint) return Model;
			TArray<FHyperAIAnimBlueprintVariable> Variables;
			TArray<FHyperAIAnimBlueprintMachine> Machines;
			Read(*Blueprint, Variables, Machines);
			for (const FHyperAIAnimBlueprintVariable& Variable : Variables)
			{
				Model.Variables.Add(Variable.Name.ToLower(), Variable.Type);
				if (!Variable.Driver.IsEmpty()) Model.Driven.Add(Variable.Name.ToLower());
			}
			for (const FHyperAIAnimBlueprintMachine& Machine : Machines)
			{
				FMachineModel& Entry = Model.Machines.Add(Machine.Name.ToLower());
				for (const FHyperAIAnimBlueprintState& State : Machine.States)
				{
					Entry.States.Add(State.Name.ToLower(), {State.Player == TEXT("sequence")});
				}
				for (const FHyperAIAnimBlueprintTransition& Transition : Machine.Transitions)
				{
					Entry.Transitions.Add(Transition.From.ToLower() + TEXT(">") + Transition.To.ToLower());
				}
			}
			return Model;
		}

		/** Loads a state's asset and checks it can play here; null Asset with no error means an empty state. */
		bool CheckAsset(const FHyperAIAnimBlueprintOp& Op, const USkeleton* Skeleton, const FModel& Model, UAnimationAsset*& OutAsset, FString& OutError)
		{
			OutAsset = nullptr;
			if (Op.Asset.IsEmpty())
			{
				if (!Op.XVariable.IsEmpty() || !Op.YVariable.IsEmpty() || !Op.Value.IsEmpty())
				{
					OutError = TEXT("x_variable, y_variable and value need an asset.");
					return false;
				}
				return true;
			}
			OutAsset = LoadObject<UAnimationAsset>(nullptr, *Op.Asset);
			const bool bSequence = OutAsset && OutAsset->IsA<UAnimSequenceBase>() && !OutAsset->IsA<UAnimMontage>();
			const UBlendSpace* BlendSpace = Cast<UBlendSpace>(OutAsset);
			if (!bSequence && !BlendSpace)
			{
				OutError = FString::Printf(TEXT("%s is not an animation sequence or blend space (montages play from slots, not states)."), *Op.Asset);
				return false;
			}
			const USkeleton* AssetSkeleton = OutAsset->GetSkeleton();
			if (Skeleton && AssetSkeleton != Skeleton && !(AssetSkeleton && Skeleton->IsCompatibleForEditor(AssetSkeleton)))
			{
				OutError = FString::Printf(TEXT("%s uses skeleton %s, not the Blueprint's %s."), *Op.Asset,
					AssetSkeleton ? *AssetSkeleton->GetName() : TEXT("none"), *Skeleton->GetName());
				return false;
			}
			auto IsFloat = [&Model](const FString& Variable) { return Model.Variables.FindRef(Variable.ToLower()) == TEXT("float"); };
			if (bSequence)
			{
				if (!Op.XVariable.IsEmpty() || !Op.YVariable.IsEmpty() || !(Op.Value.IsEmpty() || Op.Value == TEXT("once")))
				{
					OutError = TEXT("a sequence takes no axis variables; value may only be once.");
					return false;
				}
				return true;
			}
			const bool b1D = BlendSpace->IsA<UBlendSpace1D>();
			if (!IsFloat(Op.XVariable) || (b1D ? !Op.YVariable.IsEmpty() : !IsFloat(Op.YVariable)) || !Op.Value.IsEmpty())
			{
				OutError = b1D ? TEXT("a 1D blend space needs x_variable naming a float variable, and no y_variable.")
					: TEXT("a blend space needs x_variable and y_variable naming float variables.");
				return false;
			}
			return true;
		}

		bool RunOps(UAnimBlueprint* Blueprint, const USkeleton* Skeleton, const TArray<FHyperAIAnimBlueprintOp>& Ops, const bool bWrite, FString& OutError)
		{
			FModel Model = ModelOf(Blueprint);
			for (int32 Index = 0; Index < Ops.Num(); ++Index)
			{
				const FHyperAIAnimBlueprintOp& Op = Ops[Index];
				auto Fail = [&](const FString& Why)
				{
					OutError = FString::Printf(TEXT("Op %d (%s %s): %s"), Index, *Op.Kind, *Op.Name, *Why);
					return false;
				};
				if (!IsIdentifier(Op.Name)) return Fail(TEXT("name must be an identifier: letters, digits and _, up to 64."));
				const FString Name = Op.Name.ToLower();
				const FString MachineKey = Op.Machine.ToLower();

				if (Op.Kind == TEXT("add_variable") || Op.Kind == TEXT("bind_variable"))
				{
					const bool bBind = Op.Kind == TEXT("bind_variable");
					const FString Type = bBind ? DriverType(Op.Value) : Op.Value;
					if (Type != TEXT("bool") && Type != TEXT("float"))
					{
						return Fail(bBind ? TEXT("value must be a driver: speed, is_falling or is_crouching.") : TEXT("value must be bool or float."));
					}
					const FString* Existing = Model.Variables.Find(Name);
					if (!bBind && Existing) return Fail(TEXT("a variable already has that name."));
					if (bBind && Existing && *Existing != Type) return Fail(FString::Printf(TEXT("%s drives a %s, but the variable is %s."), *Op.Value, *Type, **Existing));
					if (bBind && Model.Driven.Contains(Name)) return Fail(TEXT("that variable already has a driver."));
					if (bWrite && !Existing && !FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Op.Name), MakeType(Type),
						Type == TEXT("bool") ? TEXT("false") : TEXT("0.0")))
					{
						return Fail(TEXT("the Blueprint refused the variable."));
					}
					Model.Variables.Add(Name, Type);
					if (bBind)
					{
						Model.Driven.Add(Name);
						if (bWrite) BindDriver(*Blueprint, Op.Name, Op.Value);
					}
					continue;
				}
				if (Op.Kind == TEXT("add_state_machine"))
				{
					if (Model.Machines.Contains(Name)) return Fail(TEXT("a state machine already has that name."));
					if (!(Op.Value.IsEmpty() || Op.Value == TEXT("output"))) return Fail(TEXT("value may only be output."));
					Model.Machines.Add(Name);
					if (!bWrite) continue;
					UEdGraph* AnimGraph = FindAnimGraph(*Blueprint);
					UAnimGraphNode_Root* Root = AnimGraph ? FindFirst<UAnimGraphNode_Root>(*AnimGraph) : nullptr;
					if (!Root) return Fail(TEXT("the Blueprint has no AnimGraph output."));
					UAnimGraphNode_StateMachine* Machine = Place<UAnimGraphNode_StateMachine>(*AnimGraph,
						Root->NodePosX - 450, Root->NodePosY + 200 * (Model.Machines.Num() - 1), [](UAnimGraphNode_StateMachine&) {});
					FBlueprintEditorUtils::RenameGraph(Machine->EditorStateMachineGraph, Op.Name);
					UEdGraphPin* Output = PosePin(*Root, EGPD_Input);
					if (Output && (Output->LinkedTo.IsEmpty() || Op.Value == TEXT("output")))
					{
						Output->BreakAllPinLinks();
						PosePin(*Machine, EGPD_Output)->MakeLinkTo(Output);
					}
					continue;
				}

				FMachineModel* Machine = Model.Machines.Find(MachineKey);
				if (!Machine) return Fail(FString::Printf(TEXT("no state machine is named '%s'."), *Op.Machine));
				UAnimationStateMachineGraph* Graph = bWrite ? FindMachine(*Blueprint, Op.Machine)->EditorStateMachineGraph.Get() : nullptr;
				const bool bStateExists = Machine->States.Contains(Name);

				if (Op.Kind == TEXT("add_state") || Op.Kind == TEXT("set_state_asset"))
				{
					const bool bAdd = Op.Kind == TEXT("add_state");
					if (bAdd == bStateExists) return Fail(bAdd ? TEXT("a state already has that name.") : TEXT("no state has that name."));
					if (!bAdd && Op.Asset.IsEmpty()) return Fail(TEXT("set_state_asset needs an asset."));
					UAnimationAsset* Asset = nullptr;
					FString Error;
					if (!CheckAsset(Op, Skeleton, Model, Asset, Error)) return Fail(Error);
					Machine->States.Add(Name, {Asset && !Asset->IsA<UBlendSpace>()});
					if (!bWrite) continue;
					UAnimStateNode* State = bAdd ? nullptr : FindState(*Graph, Op.Name);
					if (bAdd)
					{
						const int32 Count = States(*Graph).Num();
						State = Place<UAnimStateNode>(*Graph, 300 + 320 * (Count % 4), 200 * (Count / 4), [](UAnimStateNode&) {});
						State->OnRenameNode(Op.Name);
						if (!EntryState(*Graph) && Graph->EntryNode && Graph->EntryNode->Pins.Num() > 0)
						{
							Graph->EntryNode->Pins[0]->MakeLinkTo(State->GetInputPin());
						}
					}
					SetPlayer(*Blueprint, *State, Asset, Op.XVariable, Op.YVariable, Op.Value == TEXT("once"));
					continue;
				}
				if (Op.Kind == TEXT("set_entry_state"))
				{
					if (!bStateExists) return Fail(TEXT("no state has that name."));
					if (!bWrite) continue;
					UEdGraphPin* Entry = Graph->EntryNode->Pins[0];
					Entry->BreakAllPinLinks();
					Entry->MakeLinkTo(FindState(*Graph, Op.Name)->GetInputPin());
					continue;
				}
				if (Op.Kind == TEXT("remove_state"))
				{
					if (!bStateExists) return Fail(TEXT("no state has that name."));
					Machine->States.Remove(Name);
					for (auto It = Machine->Transitions.CreateIterator(); It; ++It)
					{
						if (It->StartsWith(Name + TEXT(">")) || It->EndsWith(TEXT(">") + Name)) It.RemoveCurrent();
					}
					if (!bWrite) continue;
					UAnimStateNode* State = FindState(*Graph, Op.Name);
					TArray<UEdGraphNode*> Doomed;
					for (UAnimStateTransitionNode* Transition : Transitions(*Graph))
					{
						if (Transition->GetPreviousState() == State || Transition->GetNextState() == State) Doomed.Add(Transition);
					}
					Doomed.Add(State);
					RemoveNodes(*Blueprint, Doomed);
					continue;
				}
				if (Op.Kind == TEXT("add_transition") || Op.Kind == TEXT("remove_transition"))
				{
					const FString To = Op.To.ToLower();
					const FString Key = Name + TEXT(">") + To;
					if (!bStateExists || !Machine->States.Contains(To)) return Fail(FString::Printf(TEXT("both states must exist in %s."), *Op.Machine));
					const bool bAdd = Op.Kind == TEXT("add_transition");
					if (bAdd == Machine->Transitions.Contains(Key))
					{
						return Fail(bAdd ? TEXT("that transition already exists; remove it first to change it.") : TEXT("no such transition."));
					}
					if (!bAdd)
					{
						Machine->Transitions.Remove(Key);
						if (!bWrite) continue;
						for (UAnimStateTransitionNode* Transition : Transitions(*Graph))
						{
							if (StateNameOf(Transition->GetPreviousState()).Equals(Op.Name, ESearchCase::IgnoreCase)
								&& StateNameOf(Transition->GetNextState()).Equals(Op.To, ESearchCase::IgnoreCase))
							{
								RemoveNodes(*Blueprint, {Transition});
								break;
							}
						}
						continue;
					}
					if (Name == To) return Fail(TEXT("a state cannot transition to itself here."));
					FRule Rule;
					FString Error;
					if (!ParseRule(Op.Rule, Rule, Error)) return Fail(Error);
					for (const FCondition& Condition : Rule.Conditions)
					{
						const FString Type = Model.Variables.FindRef(Condition.Variable.ToLower());
						if (Type != (Condition.Op.IsEmpty() ? TEXT("bool") : TEXT("float")))
						{
							return Fail(FString::Printf(TEXT("%s must be a %s variable."), *Condition.Variable, Condition.Op.IsEmpty() ? TEXT("bool") : TEXT("float")));
						}
					}
					if (Rule.bTimeRemaining && !Machine->States[Name].bPlaysSequence)
					{
						return Fail(TEXT("time_remaining needs the source state to play a sequence."));
					}
					double Blend = 0.2;
					if (!Op.Value.IsEmpty() && (!LexTryParseString(Blend, *Op.Value) || Blend < 0.0 || Blend > 2.0))
					{
						return Fail(TEXT("value is the blend time in seconds, 0 to 2."));
					}
					Machine->Transitions.Add(Key);
					if (!bWrite) continue;
					UAnimStateNode* From = FindState(*Graph, Op.Name);
					UAnimStateNode* ToState = FindState(*Graph, Op.To);
					UAnimStateTransitionNode* Transition = Place<UAnimStateTransitionNode>(*Graph,
						(From->NodePosX + ToState->NodePosX) / 2, (From->NodePosY + ToState->NodePosY) / 2 + 60, [](UAnimStateTransitionNode&) {});
					Transition->CreateConnections(From, ToState);
					Transition->CrossfadeDuration = static_cast<float>(Blend);
					if (Rule.bTimeRemaining)
					{
						Transition->bAutomaticRuleBasedOnSequencePlayerInState = true;
						Transition->AutomaticRuleTriggerTime = static_cast<float>(Rule.TimeRemaining);
					}
					else
					{
						BuildRule(*CastChecked<UAnimationTransitionGraph>(Transition->BoundGraph), Rule);
					}
					// The rule as written, readable on the graph and read back by inspect.
					Transition->NodeComment = Op.Rule.TrimStartAndEnd();
					Transition->bCommentBubbleVisible = true;
					continue;
				}
				return Fail(TEXT("unknown kind: use add_variable, bind_variable, add_state_machine, add_state, set_state_asset, set_entry_state, add_transition, remove_transition or remove_state."));
			}
			if (bWrite)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
			return true;
		}
	}

	UAnimBlueprint* Resolve(const FString& Path, const bool bLoad)
	{
		UAnimBlueprint* Blueprint = FindObject<UAnimBlueprint>(nullptr, *Path);
		return Blueprint || !bLoad ? Blueprint : LoadObject<UAnimBlueprint>(nullptr, *Path);
	}

	USkeleton* ResolveSkeleton(const FString& Path)
	{
		return Path.IsEmpty() ? nullptr : LoadObject<USkeleton>(nullptr, *Path);
	}

	UClass* ResolveParentClass(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty()) return UAnimInstance::StaticClass();
		UClass* Class = LoadClass<UAnimInstance>(nullptr, *Path);
		if (!Class) OutError = FString::Printf(TEXT("%s is not an AnimInstance class."), *Path);
		return Class;
	}

	bool IsOpenInEditor(const UAnimBlueprint& Blueprint)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		return Editors && Editors->FindEditorForAsset(const_cast<UAnimBlueprint*>(&Blueprint), /*bFocusIfOpen=*/false) != nullptr;
	}

	FString CompileStatusOf(const UAnimBlueprint& Blueprint)
	{
		switch (Blueprint.Status)
		{
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("warnings");
		case BS_Error: return TEXT("error");
		default: return TEXT("dirty");
		}
	}

	FString ComputeRevision(const UAnimBlueprint& Blueprint)
	{
		FString Canonical = TEXT("hyperai.anim_blueprint.revision.v1\n");
		Canonical += (Blueprint.TargetSkeleton ? Blueprint.TargetSkeleton->GetPathName() : FString()) + TEXT("\n");
		Canonical += (Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString()) + TEXT("\n");
		for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
		{
			Canonical += FString::Printf(TEXT("var|%s|%s|%s|%s\n"), *Variable.VarName.ToString(), *Variable.VarType.PinCategory.ToString(),
				*Variable.VarType.PinSubCategory.ToString(), *Variable.DefaultValue);
		}
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GetPathName() < B.GetPathName(); });
		for (const UEdGraph* Graph : Graphs)
		{
			Canonical += TEXT("graph|") + Graph->GetPathName() + TEXT("\n");
			TArray<UEdGraphNode*> Nodes(Graph->Nodes);
			Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid.ToString() < B.NodeGuid.ToString(); });
			for (const UEdGraphNode* Node : Nodes)
			{
				if (!Node) continue;
				Canonical += FString::Printf(TEXT("node|%s|%s|%s|%d\n"), *Node->GetClass()->GetName(), *Node->NodeGuid.ToString(),
					*Node->NodeComment, static_cast<int32>(Node->GetDesiredEnabledState()));
				if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
				{
					Canonical += FString::Printf(TEXT("transition|%g|%d|%g|%d\n"), Transition->CrossfadeDuration,
						Transition->bAutomaticRuleBasedOnSequencePlayerInState ? 1 : 0, Transition->AutomaticRuleTriggerTime, Transition->PriorityOrder);
				}
				if (const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
				{
					if (const UAnimationAsset* Asset = AnimNode->GetAnimationAsset()) Canonical += TEXT("asset|") + Asset->GetPathName() + TEXT("\n");
				}
				if (UAnimGraphNode_SequencePlayer* Sequence = Cast<UAnimGraphNode_SequencePlayer>(const_cast<UEdGraphNode*>(Node)))
				{
					void* Container = nullptr;
					if (FBoolProperty* Loop = LoopProperty(*Sequence, Container)) Canonical += Loop->GetPropertyValue_InContainer(Container) ? TEXT("loop\n") : TEXT("once\n");
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					TArray<FString> Links;
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						Links.Add(Linked->GetOwningNode()->NodeGuid.ToString() + TEXT(".") + Linked->PinName.ToString());
					}
					Links.Sort();
					Canonical += FString::Printf(TEXT("pin|%s|%s|%s|%s\n"), *Pin->PinName.ToString(), *Pin->DefaultValue,
						Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT(""), *FString::Join(Links, TEXT(",")));
				}
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	void Read(const UAnimBlueprint& Blueprint, TArray<FHyperAIAnimBlueprintVariable>& OutVariables, TArray<FHyperAIAnimBlueprintMachine>& OutMachines)
	{
		OutVariables.Reset();
		OutMachines.Reset();
		const UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(&Blueprint);
		for (const FBPVariableDescription& Description : Blueprint.NewVariables)
		{
			FHyperAIAnimBlueprintVariable& Variable = OutVariables.AddDefaulted_GetRef();
			Variable.Name = Description.VarName.ToString();
			Variable.Type = TypeOf(Description.VarType);
			for (const UEdGraphNode* Node : EventGraph ? EventGraph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
			{
				const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node);
				if (Set && Set->NodeComment.StartsWith(DriverTag) && Set->VariableReference.GetMemberName() == Description.VarName)
				{
					Variable.Driver = Set->NodeComment.RightChop(DriverTag.Len());
				}
			}
		}
		const UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
		const UAnimGraphNode_Root* Root = AnimGraph ? FindFirst<UAnimGraphNode_Root>(*AnimGraph) : nullptr;
		const UEdGraphNode* OutputSource = Root ? LinkedNode(PosePin(*Root, EGPD_Input)) : nullptr;
		for (UAnimGraphNode_StateMachine* MachineNode : Machines(Blueprint))
		{
			const UAnimationStateMachineGraph& Graph = *MachineNode->EditorStateMachineGraph;
			FHyperAIAnimBlueprintMachine& Machine = OutMachines.AddDefaulted_GetRef();
			Machine.Name = MachineNode->GetStateMachineName();
			Machine.bDrivesOutput = OutputSource == MachineNode;
			const UAnimStateNodeBase* Entry = EntryState(Graph);
			for (const UAnimStateNode* StateNode : States(Graph))
			{
				FHyperAIAnimBlueprintState& State = Machine.States.AddDefaulted_GetRef();
				ReadState(*StateNode, State);
				State.bEntry = StateNode == Entry;
			}
			for (const UAnimStateTransitionNode* TransitionNode : Transitions(Graph))
			{
				FHyperAIAnimBlueprintTransition& Transition = Machine.Transitions.AddDefaulted_GetRef();
				Transition.From = StateNameOf(TransitionNode->GetPreviousState());
				Transition.To = StateNameOf(TransitionNode->GetNextState());
				Transition.Rule = RuleOf(*TransitionNode);
				Transition.BlendTime = TransitionNode->CrossfadeDuration;
			}
		}
	}

	bool ValidateOps(const UAnimBlueprint* Blueprint, const USkeleton* Skeleton, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError)
	{
		return RunOps(const_cast<UAnimBlueprint*>(Blueprint), Skeleton, Ops, /*bWrite=*/false, OutError);
	}

	UAnimBlueprint* Create(const FString& Path, USkeleton& Skeleton, UClass& ParentClass, FString& OutError)
	{
		UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(Path));
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->TargetSkeleton = &Skeleton;
		Factory->ParentClass = &ParentClass;
		Factory->BlueprintType = BPTYPE_Normal;
		UAnimBlueprint* Blueprint = Package ? Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
			FName(*FPackageName::ObjectPathToObjectName(Path)), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)) : nullptr;
		if (!Blueprint)
		{
			OutError = TEXT("Could not create the Animation Blueprint.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
		return Blueprint;
	}

	bool ApplyOps(UAnimBlueprint& Blueprint, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError)
	{
		if (!RunOps(&Blueprint, Blueprint.TargetSkeleton, Ops, /*bWrite=*/false, OutError))
		{
			return false;
		}
		Blueprint.Modify();
		return RunOps(&Blueprint, Blueprint.TargetSkeleton, Ops, /*bWrite=*/true, OutError);
	}

	bool Compile(UAnimBlueprint& Blueprint, FString& OutFirstError, int32& OutErrors, int32& OutWarnings)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		OutErrors = Results.NumErrors;
		OutWarnings = Results.NumWarnings;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				OutFirstError = Message->ToText().ToString();
				break;
			}
		}
		return OutErrors == 0 && Blueprint.Status != BS_Error;
	}

	TArray<FHyperAIAnimBlueprintIssue> Check(const UAnimBlueprint& Blueprint)
	{
		TArray<FHyperAIAnimBlueprintIssue> Issues;
		auto Add = [&Issues](const TCHAR* Code, const bool bError, const FString& Subject, const FString& Message)
		{
			Issues.Add({Code, bError ? TEXT("error") : TEXT("warning"), Subject, Message});
		};
		if (Blueprint.Status == BS_Error)
		{
			Add(TEXT("compile_error"), true, Blueprint.GetName(), TEXT("The last compile failed; open the Blueprint's compiler results, or fix the graph and apply again."));
		}
		const UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
		const UAnimGraphNode_Root* Root = AnimGraph ? FindFirst<UAnimGraphNode_Root>(*AnimGraph) : nullptr;
		if (!Root || !LinkedNode(PosePin(*Root, EGPD_Input)))
		{
			Add(TEXT("no_output_pose"), true, TEXT("AnimGraph"), TEXT("Nothing feeds the output pose, so the character holds its reference pose."));
		}

		TArray<FHyperAIAnimBlueprintVariable> Variables;
		TArray<FHyperAIAnimBlueprintMachine> Machines;
		Read(Blueprint, Variables, Machines);
		TSet<FString> Driven;
		for (const FHyperAIAnimBlueprintVariable& Variable : Variables)
		{
			if (!Variable.Driver.IsEmpty()) Driven.Add(Variable.Name.ToLower());
		}
		// Anything else that sets a variable counts as a driver too.
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node)) Driven.Add(Set->VariableReference.GetMemberName().ToString().ToLower());
			}
		}
		TSet<FString> ReadVariables;

		for (const FHyperAIAnimBlueprintMachine& Machine : Machines)
		{
			const FHyperAIAnimBlueprintState* Entry = Machine.States.FindByPredicate([](const FHyperAIAnimBlueprintState& State) { return State.bEntry; });
			if (!Entry && !Machine.States.IsEmpty())
			{
				Add(TEXT("no_entry"), true, Machine.Name, TEXT("No entry state; set_entry_state picks where the machine starts."));
			}
			// Reachable from the entry along transitions.
			TSet<FString> Reached;
			TArray<FString> Frontier;
			if (Entry) Frontier.Add(Entry->Name);
			while (!Frontier.IsEmpty())
			{
				const FString Current = Frontier.Pop();
				if (Reached.Contains(Current)) continue;
				Reached.Add(Current);
				for (const FHyperAIAnimBlueprintTransition& Transition : Machine.Transitions)
				{
					if (Transition.From == Current && Transition.Rule != TEXT("(none)")) Frontier.Add(Transition.To);
				}
			}
			for (const FHyperAIAnimBlueprintState& State : Machine.States)
			{
				const FString Subject = Machine.Name + TEXT(".") + State.Name;
				if (Entry && !Reached.Contains(State.Name))
				{
					Add(TEXT("unreachable_state"), false, Subject, TEXT("No transition leads here from the entry state."));
				}
				const bool bLeaves = Machine.Transitions.ContainsByPredicate([&State](const FHyperAIAnimBlueprintTransition& T) { return T.From == State.Name; });
				if (!bLeaves && Machine.States.Num() > 1)
				{
					Add(TEXT("dead_end"), false, Subject, TEXT("No transition leaves this state, so the machine stays here once it arrives."));
				}
				if (State.Player == TEXT("empty"))
				{
					Add(TEXT("empty_state"), true, Subject, TEXT("The state outputs no pose; give it a sequence or blend space."));
				}
				if (State.Player == TEXT("blend_space") && State.XVariable.IsEmpty())
				{
					Add(TEXT("unbound_axis"), false, Subject, TEXT("The blend space's axes read no variable, so it always plays its origin sample."));
				}
				if (State.Player == TEXT("sequence") && !State.bLooping
					&& !Machine.Transitions.ContainsByPredicate([&State](const FHyperAIAnimBlueprintTransition& T) { return T.From == State.Name && T.Rule.StartsWith(TEXT("time_remaining")); }))
				{
					Add(TEXT("frozen_pose"), false, Subject, TEXT("It plays once and nothing leaves it as the animation ends, so the last frame holds. Add a time_remaining transition."));
				}
				if (!State.Asset.IsEmpty() && Blueprint.TargetSkeleton)
				{
					const UAnimationAsset* Asset = FindObject<UAnimationAsset>(nullptr, *State.Asset);
					const USkeleton* AssetSkeleton = Asset ? Asset->GetSkeleton() : nullptr;
					if (AssetSkeleton && AssetSkeleton != Blueprint.TargetSkeleton && !Blueprint.TargetSkeleton->IsCompatibleForEditor(AssetSkeleton))
					{
						Add(TEXT("skeleton_mismatch"), true, Subject, FString::Printf(TEXT("%s is for skeleton %s."), *Asset->GetName(), *AssetSkeleton->GetName()));
					}
				}
				if (!State.XVariable.IsEmpty()) ReadVariables.Add(State.XVariable.ToLower());
				if (!State.YVariable.IsEmpty()) ReadVariables.Add(State.YVariable.ToLower());
			}
			for (const FHyperAIAnimBlueprintTransition& Transition : Machine.Transitions)
			{
				const FString Subject = FString::Printf(TEXT("%s.%s->%s"), *Machine.Name, *Transition.From, *Transition.To);
				if (Transition.Rule == TEXT("(none)"))
				{
					Add(TEXT("never_fires"), false, Subject, TEXT("The rule is unset, so this transition never happens."));
				}
				if (Transition.Rule == TEXT("(always)"))
				{
					Add(TEXT("always_fires"), true, Subject, TEXT("The rule is always true, so the machine leaves the state the moment it enters."));
				}
				if (Transition.BlendTime <= 0.0f)
				{
					Add(TEXT("instant_blend"), false, Subject, TEXT("A zero blend snaps between poses and reads as a pop; 0.1-0.25 s suits most locomotion."));
				}
				else if (Transition.BlendTime > 0.6f)
				{
					Add(TEXT("slow_blend"), false, Subject, FString::Printf(TEXT("A %.2f s blend makes the change feel late."), Transition.BlendTime));
				}
				FRule Rule;
				FString Ignored;
				if (!ParseRule(Transition.Rule, Rule, Ignored)) continue;
				for (const FCondition& Condition : Rule.Conditions) ReadVariables.Add(Condition.Variable.ToLower());
				// Hysteresis: A->B on V > x and B->A on V < y need y below x, or the machine flickers at the threshold.
				if (Rule.Conditions.Num() != 1 || Rule.Conditions[0].Op.IsEmpty()) continue;
				const FCondition& Out = Rule.Conditions[0];
				for (const FHyperAIAnimBlueprintTransition& Back : Machine.Transitions)
				{
					FRule BackRule;
					if (Back.From != Transition.To || Back.To != Transition.From || !ParseRule(Back.Rule, BackRule, Ignored)
						|| BackRule.Conditions.Num() != 1 || !BackRule.Conditions[0].Variable.Equals(Out.Variable, ESearchCase::IgnoreCase))
					{
						continue;
					}
					const FCondition& In = BackRule.Conditions[0];
					const bool bUp = Out.Op.StartsWith(TEXT(">")) && In.Op.StartsWith(TEXT("<"));
					if (bUp && In.Number >= Out.Number)
					{
						Add(TEXT("no_hysteresis"), false, Subject, FString::Printf(TEXT("%s enters above %g and leaves below %g; leave below a lower value (for example %g) so it cannot flicker at the threshold."),
							*Out.Variable, Out.Number, In.Number, Out.Number * 0.5));
					}
				}
			}
		}
		for (const FString& Variable : ReadVariables)
		{
			if (!Driven.Contains(Variable))
			{
				Add(TEXT("never_set"), false, Variable, TEXT("States or rules read this variable but nothing sets it; bind_variable it or set it in the event graph."));
			}
		}
		return Issues;
	}
}
