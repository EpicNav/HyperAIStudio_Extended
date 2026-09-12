// Games by Hyper 2026.

#include "HyperAIStudioBlueprintPatch.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ITransaction.h"
#include "ScopedTransaction.h"

namespace HyperAIStudio::BlueprintPatch::Private
{
	constexpr const TCHAR* TransactionContext = TEXT("HyperAIStudio.BlueprintPatch");

	FString GuidToken(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT("invalid");
	}

	FString CompileStatus(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return TEXT("unavailable");
		}
		switch (Blueprint->Status)
		{
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_Dirty: return TEXT("dirty");
		case BS_Error: return TEXT("error");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	void AppendToken(FString& Buffer, const FString& Value)
	{
		Buffer += FString::FromInt(Value.Len());
		Buffer += TEXT(":");
		Buffer += Value;
		Buffer += TEXT("|");
	}

	FString Sha256(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		const int64 ByteCount = Utf8.Length();
		if (ByteCount < 0 || ByteCount > MAX_int32 - 72)
		{
			return {};
		}
		static constexpr uint32 Constants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
		uint32 State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

		TArray<uint8> Padded;
		Padded.Append(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<int32>(ByteCount));
		Padded.Add(0x80u);
		while ((Padded.Num() % 64) != 56)
		{
			Padded.Add(0u);
		}
		const uint64 BitCount = static_cast<uint64>(ByteCount) * 8u;
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Padded.Add(static_cast<uint8>((BitCount >> Shift) & 0xffu));
		}

		auto RotateRight = [](const uint32 Input, const uint32 Shift)
		{
			return (Input >> Shift) | (Input << (32u - Shift));
		};
		for (int32 Block = 0; Block < Padded.Num(); Block += 64)
		{
			uint32 Words[64]{};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = Block + Index * 4;
				Words[Index] = (static_cast<uint32>(Padded[Offset]) << 24)
					| (static_cast<uint32>(Padded[Offset + 1]) << 16)
					| (static_cast<uint32>(Padded[Offset + 2]) << 8)
					| static_cast<uint32>(Padded[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 Small0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const uint32 Small1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + Small0 + Words[Index - 7] + Small1;
			}

			uint32 A = State[0];
			uint32 B = State[1];
			uint32 C = State[2];
			uint32 D = State[3];
			uint32 E = State[4];
			uint32 F = State[5];
			uint32 G = State[6];
			uint32 H = State[7];
			for (int32 Index = 0; Index < 64; ++Index)
			{
				const uint32 Big1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ ((~E) & G);
				const uint32 Temp1 = H + Big1 + Choice + Constants[Index] + Words[Index];
				const uint32 Big0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = Big0 + Majority;
				H = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}
			State[0] += A;
			State[1] += B;
			State[2] += C;
			State[3] += D;
			State[4] += E;
			State[5] += F;
			State[6] += G;
			State[7] += H;
		}

		FString Hex = TEXT("sha256:");
		Hex.Reserve(71);
		for (const uint32 Word : State)
		{
			Hex += FString::Printf(TEXT("%08x"), Word);
		}
		return Hex;
	}

	bool IsCanonicalSha256(const FString& Value)
	{
		if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	FString CanonicalLink(const FString& A, const FString& B)
	{
		return A < B ? A + TEXT("<->") + B : B + TEXT("<->") + A;
	}

	bool IsBoundedPath(const FString& Value)
	{
		return !Value.IsEmpty() && Value.Len() <= FHyperAIStudioBlueprintPatch::MaxPathCharacters;
	}

	bool IsValidPinDirection(const EHyperAIBlueprintPinDirection Direction)
	{
		return Direction == EHyperAIBlueprintPinDirection::Input
			|| Direction == EHyperAIBlueprintPinDirection::Output;
	}

	bool IsValidDeleteMode(const EHyperAIBlueprintDeleteMode Mode)
	{
		return Mode == EHyperAIBlueprintDeleteMode::OrphanOnly
			|| Mode == EHyperAIBlueprintDeleteMode::ExplicitConfirmed;
	}

	bool IsValidPatchSafety(const EHyperAIBlueprintPatchSafety Safety)
	{
		return Safety == EHyperAIBlueprintPatchSafety::Edit
			|| Safety == EHyperAIBlueprintPatchSafety::Destructive;
	}

	bool IsValidCreateNodeKind(const EHyperAIBlueprintCreateNodeKind Kind)
	{
		return Kind == EHyperAIBlueprintCreateNodeKind::Branch
			|| Kind == EHyperAIBlueprintCreateNodeKind::Sequence
			|| Kind == EHyperAIBlueprintCreateNodeKind::Reroute;
	}

	bool TryConvertPinDirection(
		const EEdGraphPinDirection Direction,
		EHyperAIBlueprintPinDirection& OutDirection)
	{
		switch (Direction)
		{
		case EGPD_Input:
			OutDirection = EHyperAIBlueprintPinDirection::Input;
			return true;
		case EGPD_Output:
			OutDirection = EHyperAIBlueprintPinDirection::Output;
			return true;
		default:
			return false;
		}
	}

	bool ContainsEmbeddedNull(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] == TEXT('\0'))
			{
				return true;
			}
		}
		return false;
	}

	FString PinTypeToken(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return {};
		}
		FString Result = Pin->PinType.PinCategory.ToString();
		Result += TEXT("/") + Pin->PinType.PinSubCategory.ToString();
		Result += FString::Printf(TEXT("/%d/%d/%d"),
			static_cast<int32>(Pin->PinType.ContainerType),
			Pin->PinType.bIsReference ? 1 : 0,
			Pin->PinType.bIsConst ? 1 : 0);
		if (const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get())
		{
			Result += TEXT("/") + TypeObject->GetPathName();
		}
		return Result;
	}

	bool GatherAuthoringGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs, FString& OutError)
	{
		OutGraphs.Reset();
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_unavailable");
			return false;
		}

		TArray<UEdGraph*> Queue;
		auto AddTopLevel = [&Queue](const auto& Source)
		{
			for (UEdGraph* Graph : Source)
			{
				if (Graph)
				{
					Queue.Add(Graph);
				}
			}
		};
		AddTopLevel(Blueprint->UbergraphPages);
		AddTopLevel(Blueprint->FunctionGraphs);
		AddTopLevel(Blueprint->MacroGraphs);
		AddTopLevel(Blueprint->DelegateSignatureGraphs);

		TSet<const UEdGraph*> Seen;
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			UEdGraph* Graph = Queue[Index];
			if (!Graph || Seen.Contains(Graph))
			{
				continue;
			}
			if (Seen.Num() >= FHyperAIStudioBlueprintPatch::MaxGraphs)
			{
				OutError = TEXT("graph_source_bound_exceeded");
				return false;
			}
			Seen.Add(Graph);
			OutGraphs.Add(Graph);
			if (Queue.Num() + Graph->SubGraphs.Num() > FHyperAIStudioBlueprintPatch::MaxGraphs * 2)
			{
				OutError = TEXT("graph_queue_bound_exceeded");
				return false;
			}
			for (UEdGraph* SubGraph : Graph->SubGraphs)
			{
				if (SubGraph && !Seen.Contains(SubGraph))
				{
					Queue.Add(SubGraph);
				}
			}
		}
		return true;
	}

	struct FLiveIndex
	{
		TMap<FString, UEdGraph*> Graphs;
		TMap<FString, UEdGraphNode*> Nodes;
		TMap<FString, UEdGraphPin*> Pins;
	};

	bool BuildLiveIndex(UBlueprint* Blueprint, FLiveIndex& OutIndex, FString& OutError)
	{
		OutIndex = {};
		TArray<UEdGraph*> Graphs;
		if (!GatherAuthoringGraphs(Blueprint, Graphs, OutError))
		{
			return false;
		}
		int32 NodeCount = 0;
		int32 PinCount = 0;
		for (UEdGraph* Graph : Graphs)
		{
			FHyperAIBlueprintGraphId GraphId{ Graph->GraphGuid, Graph->GetPathName() };
			if (!GraphId.IsValid() || OutIndex.Graphs.Contains(GraphId.StableKey()))
			{
				OutError = TEXT("unsupported_or_duplicate_graph_identity");
				return false;
			}
			OutIndex.Graphs.Add(GraphId.StableKey(), Graph);
			if (NodeCount + Graph->Nodes.Num() > FHyperAIStudioBlueprintPatch::MaxNodes)
			{
				OutError = TEXT("node_source_bound_exceeded");
				return false;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					OutError = TEXT("null_node_unsupported");
					return false;
				}
				++NodeCount;
				FHyperAIBlueprintNodeId NodeId{ GraphId, Node->NodeGuid };
				if (!NodeId.IsValid() || OutIndex.Nodes.Contains(NodeId.StableKey()))
				{
					OutError = TEXT("unsupported_or_duplicate_node_identity");
					return false;
				}
				OutIndex.Nodes.Add(NodeId.StableKey(), Node);
				if (PinCount + Node->Pins.Num() > FHyperAIStudioBlueprintPatch::MaxPins)
				{
					OutError = TEXT("pin_source_bound_exceeded");
					return false;
				}
				for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
				{
					UEdGraphPin* Pin = Node->Pins[PinIndex];
					if (!Pin)
					{
						OutError = TEXT("null_pin_unsupported");
						return false;
					}
					++PinCount;
					FHyperAIBlueprintPinId PinId;
					PinId.Node = NodeId;
					PinId.PersistentGuid = Pin->PersistentGuid;
					PinId.PinName = Pin->PinName;
					if (!TryConvertPinDirection(Pin->Direction, PinId.Direction))
					{
						OutError = TEXT("unsupported_live_pin_direction");
						return false;
					}
					PinId.PinIndex = PinIndex;
					if (!PinId.IsValid() || OutIndex.Pins.Contains(PinId.StableKey()))
					{
						OutError = TEXT("unsupported_or_duplicate_pin_identity");
						return false;
					}
					OutIndex.Pins.Add(PinId.StableKey(), Pin);
				}
			}
		}
		return true;
	}

	void AddIssue(
		FHyperAIBlueprintPatchPlan& Plan,
		const FString& Code,
		const int32 OperationIndex,
		const FString& Target,
		const FString& Message)
	{
		Plan.bValid = false;
		if (Plan.Issues.Num() >= FHyperAIStudioBlueprintPatch::MaxIssues)
		{
			return;
		}
		FHyperAIBlueprintPatchIssue& Issue = Plan.Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = TEXT("error");
		Issue.OperationIndex = OperationIndex;
		Issue.Target = Target.Left(FHyperAIStudioBlueprintPatch::MaxPathCharacters);
		Issue.Message = Message.Left(512);
	}

	bool CoordinateValid(const int32 Value)
	{
		return FMath::Abs(static_cast<int64>(Value)) <= FHyperAIStudioBlueprintPatch::MaxCoordinate;
	}

	FString OperationCanonical(const FHyperAIBlueprintPatchOperation& Operation)
	{
		FString Result = Operation.Kind();
				if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
				{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
			Result += TEXT("|") + Value.Node.StableKey() + FString::Printf(TEXT("|%d|%d"), Value.X, Value.Y);
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
			Result += TEXT("|") + Value.Graph.StableKey()
				+ TEXT("|") + GuidToken(Value.NewNodeGuid)
				+ FString::Printf(TEXT("|%d|%d|%d"), static_cast<int32>(Value.Kind), Value.X, Value.Y);
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintCompileOnly>())
		{
			Result += TEXT("|compile-once-validate-once");
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
			Result += TEXT("|") + CanonicalLink(Value.A.StableKey(), Value.B.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
			Result += TEXT("|") + CanonicalLink(Value.A.StableKey(), Value.B.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
			Result += TEXT("|") + Value.Pin.StableKey() + TEXT("|") + Value.Value;
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
			Result += TEXT("|") + Value.Node.StableKey() + FString::Printf(TEXT("|%d"), static_cast<int32>(Value.Mode));
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
			Result += TEXT("|") + Value.Graph.StableKey();
			TArray<FHyperAIBlueprintNodeId> CanonicalNodes = Value.Nodes;
			CanonicalNodes.Sort([](const auto& A, const auto& B) { return A.StableKey() < B.StableKey(); });
			for (const FHyperAIBlueprintNodeId& Node : CanonicalNodes)
			{
				Result += TEXT("|") + Node.StableKey();
			}
			Result += FString::Printf(TEXT("|%d|%d|%d|%d|%d"), Value.OriginX, Value.OriginY,
				Value.Columns, Value.HorizontalSpacing, Value.VerticalSpacing);
		}
		return Result;
	}

	bool IsUnsafeLiteralPin(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->PinType.IsContainer())
		{
			return true;
		}
		const FName Category = Pin->PinType.PinCategory;
		return Category == UEdGraphSchema_K2::PC_Exec
			|| Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass
			|| Category == UEdGraphSchema_K2::PC_Interface
			|| Category == UEdGraphSchema_K2::PC_Delegate
			|| Category == UEdGraphSchema_K2::PC_MCDelegate;
	}

	bool ValidateOperationEnums(
		const FHyperAIBlueprintPatch& Patch,
		TArray<FHyperAIBlueprintPatchIssue>& OutIssues)
	{
		bool bValid = true;
		auto Fail = [&OutIssues, &bValid](
			const FString& Code,
			const int32 OperationIndex,
			const FString& Target,
			const FString& Message)
		{
			bValid = false;
			if (OutIssues.Num() < FHyperAIStudioBlueprintPatch::MaxIssues)
			{
				OutIssues.Add({Code, TEXT("error"), OperationIndex, Target.Left(1024), Message.Left(512)});
			}
		};
		auto CheckPin = [&Fail](const FHyperAIBlueprintPinId& Pin, const int32 OperationIndex)
		{
			if (!IsValidPinDirection(Pin.Direction))
			{
				Fail(TEXT("invalid_pin_direction"), OperationIndex, Pin.StableKey(),
					TEXT("Pin direction must be exactly input or output."));
			}
		};

		for (int32 OperationIndex = 0; OperationIndex < Patch.Operations.Num(); ++OperationIndex)
		{
			const FHyperAIBlueprintPatchOperation& Operation = Patch.Operations[OperationIndex];
			if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
				CheckPin(Value.A, OperationIndex);
				CheckPin(Value.B, OperationIndex);
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
				if (!IsValidCreateNodeKind(Value.Kind))
				{
					Fail(TEXT("invalid_create_node_kind"), OperationIndex, Value.NodeId().StableKey(),
						TEXT("Create kind must be exactly branch, sequence, or reroute."));
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
				CheckPin(Value.A, OperationIndex);
				CheckPin(Value.B, OperationIndex);
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
			{
				CheckPin(Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>().Pin, OperationIndex);
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				if (!IsValidDeleteMode(Value.Mode))
				{
					Fail(TEXT("invalid_delete_mode"), OperationIndex, Value.Node.StableKey(),
						TEXT("Delete mode must be exactly orphan_only or explicit_confirmed."));
				}
			}
		}
		return bValid;
	}

	bool ValidateLiveSchema(
		const FHyperAIBlueprintPatch& Patch,
		const FHyperAIBlueprintPatchPlan& Plan,
		const FLiveIndex& Index,
		TArray<FHyperAIBlueprintPatchIssue>& OutIssues)
	{
		OutIssues = Plan.Issues;
		if (!ValidateOperationEnums(Patch, OutIssues))
		{
			return false;
		}
			auto Fail = [&OutIssues](const FString& Code, const int32 OperationIndex, const FString& Target, const FString& Message)
		{
			if (OutIssues.Num() < FHyperAIStudioBlueprintPatch::MaxIssues)
			{
				OutIssues.Add({ Code, TEXT("error"), OperationIndex, Target.Left(1024), Message.Left(512) });
			}
			};

			TArray<FHyperAIBlueprintPatchNodeSnapshot> CreatedTemplates;
			CreatedTemplates.Reserve(FMath::Min(Patch.Operations.Num(), FHyperAIStudioBlueprintPatch::MaxOperations));
			for (const FHyperAIBlueprintPatchOperation& Operation : Patch.Operations)
			{
				if (!Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
				{
					continue;
				}
				FString TemplateError;
				FHyperAIBlueprintPatchNodeSnapshot Template;
				if (FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(
					Operation.Value.Get<FHyperAIBlueprintCreateNode>(), Template, TemplateError))
				{
					CreatedTemplates.Add(MoveTemp(Template));
				}
			}
			TMap<FString, const FHyperAIBlueprintPatchPinSnapshot*> CreatedPins;
			for (const FHyperAIBlueprintPatchNodeSnapshot& Node : CreatedTemplates)
			{
				for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
				{
					CreatedPins.Add(Pin.Id.StableKey(), &Pin);
				}
			}

			for (int32 OperationIndex = 0; OperationIndex < Patch.Operations.Num(); ++OperationIndex)
			{
				const FHyperAIBlueprintPatchOperation& Operation = Patch.Operations[OperationIndex];
				if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
				{
					const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
					UEdGraph* const* GraphPtr = Index.Graphs.Find(Value.Graph.StableKey());
					if (!GraphPtr || !Cast<UEdGraphSchema_K2>((*GraphPtr)->GetSchema()))
					{
						Fail(TEXT("unsupported_create_graph"), OperationIndex, Value.Graph.StableKey(),
							TEXT("Allowlisted nodes can be created only in an already-loaded K2 graph."));
					}
					if (Index.Nodes.Contains(Value.NodeId().StableKey()))
					{
						Fail(TEXT("create_node_guid_collision"), OperationIndex, Value.NodeId().StableKey(),
							TEXT("The requested stable node GUID already exists in the loaded Blueprint."));
					}
				}
				else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
				{
					const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
					UEdGraphPin* const* APtr = Index.Pins.Find(Value.A.StableKey());
					UEdGraphPin* const* BPtr = Index.Pins.Find(Value.B.StableKey());
					const FHyperAIBlueprintPatchPinSnapshot* const* CreatedA = CreatedPins.Find(Value.A.StableKey());
					const FHyperAIBlueprintPatchPinSnapshot* const* CreatedB = CreatedPins.Find(Value.B.StableKey());
					if ((!APtr && !CreatedA) || (!BPtr && !CreatedB))
					{
						Fail(TEXT("live_pin_missing"), OperationIndex, Value.A.StableKey(), TEXT("A pin disappeared after snapshot validation."));
						continue;
					}
					if (APtr && BPtr && (*APtr)->LinkedTo.Contains(*BPtr))
					{
						continue;
					}
					if (APtr && BPtr)
					{
						const UEdGraphSchema* Schema = (*APtr)->GetSchema();
						if (!Cast<UEdGraphSchema_K2>(Schema) || Schema != (*BPtr)->GetSchema())
						{
							Fail(TEXT("unsupported_graph_schema"), OperationIndex, Value.A.StableKey(), TEXT("Only loaded K2 Blueprint pins in one schema are supported."));
							continue;
						}
						const FPinConnectionResponse Response = Schema->CanCreateConnection(*APtr, *BPtr);
						if (Response.Response != CONNECT_RESPONSE_MAKE)
						{
							Fail(TEXT("connection_requires_implicit_effect"), OperationIndex, Value.A.StableKey(),
								TEXT("The schema did not approve a direct connection without conversion or implicit link breaking: ") + Response.Message.ToString());
						}
					}
					else
					{
						const FString AType = APtr ? PinTypeToken(*APtr) : (*CreatedA)->PinType;
						const FString BType = BPtr ? PinTypeToken(*BPtr) : (*CreatedB)->PinType;
						const bool bWildcard = AType.StartsWith(TEXT("wildcard/")) || BType.StartsWith(TEXT("wildcard/"));
						if (!bWildcard && AType != BType)
						{
							Fail(TEXT("created_pin_type_mismatch"), OperationIndex, Value.A.StableKey(),
								TEXT("A newly created fixed-type pin cannot connect to the requested incompatible pin type."));
						}
					}
				}
				else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
				{
					const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
					UEdGraphPin* const* PinPtr = Index.Pins.Find(Value.Pin.StableKey());
					const FHyperAIBlueprintPatchPinSnapshot* const* CreatedPin = CreatedPins.Find(Value.Pin.StableKey());
					if (!PinPtr && !CreatedPin)
					{
						Fail(TEXT("live_pin_missing"), OperationIndex, Value.Pin.StableKey(), TEXT("The target pin disappeared after snapshot validation."));
						continue;
					}
					if (!PinPtr)
					{
						if ((*CreatedPin)->bDefaultIgnored || (*CreatedPin)->bDefaultReadOnly
							|| !(*CreatedPin)->PinType.StartsWith(TEXT("bool/"))
							|| (Value.Value != TEXT("true") && Value.Value != TEXT("false")))
						{
							Fail(TEXT("unsupported_created_literal_pin"), OperationIndex, Value.Pin.StableKey(),
								TEXT("Only true/false on the allowlisted Branch Condition pin is accepted before creation."));
						}
						continue;
					}
					UEdGraphPin* Pin = *PinPtr;
				const UEdGraphSchema* Schema = Pin->GetSchema();
				if (!Cast<UEdGraphSchema_K2>(Schema) || IsUnsafeLiteralPin(Pin))
				{
					Fail(TEXT("unsupported_literal_pin"), OperationIndex, Value.Pin.StableKey(), TEXT("Object, class, interface, delegate, exec, and container defaults are not accepted by this literal-only core."));
					continue;
				}
				const FString DefaultError = Schema->IsPinDefaultValid(Pin, Value.Value, nullptr, FText::GetEmpty());
				if (!DefaultError.IsEmpty())
				{
					Fail(TEXT("invalid_literal_default"), OperationIndex, Value.Pin.StableKey(), DefaultError);
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				UEdGraphNode* const* NodePtr = Index.Nodes.Find(Value.Node.StableKey());
				if (!NodePtr || !Cast<UK2Node>(*NodePtr) || !Cast<UEdGraphSchema_K2>((*NodePtr)->GetSchema()))
				{
					Fail(TEXT("unsupported_delete_node"), OperationIndex, Value.Node.StableKey(), TEXT("Deletion is limited to user-deletable UK2Node instances in a K2 graph."));
				}
			}
		}
		return OutIssues.IsEmpty();
	}

	bool ValidatePostconditions(const FHyperAIBlueprintPatchSnapshot& Snapshot, const FHyperAIBlueprintPatch& Patch)
	{
		TMap<FString, const FHyperAIBlueprintPatchNodeSnapshot*> Nodes;
		TMap<FString, const FHyperAIBlueprintPatchPinSnapshot*> Pins;
		for (const auto& Graph : Snapshot.Graphs)
		{
			for (const auto& Node : Graph.Nodes)
			{
				Nodes.Add(Node.Id.StableKey(), &Node);
				for (const auto& Pin : Node.Pins)
				{
					Pins.Add(Pin.Id.StableKey(), &Pin);
				}
			}
		}

		for (const FHyperAIBlueprintPatchOperation& Operation : Patch.Operations)
		{
			if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
				const auto* const* Node = Nodes.Find(Value.Node.StableKey());
				if (!Node || (*Node)->X != Value.X || (*Node)->Y != Value.Y) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
				const auto* const* Node = Nodes.Find(Value.NodeId().StableKey());
				FHyperAIBlueprintPatchNodeSnapshot Expected;
				FString Error;
				if (!Node || !FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(Value, Expected, Error)
					|| (*Node)->NodeClassPath != Expected.NodeClassPath
					|| (*Node)->X != Value.X || (*Node)->Y != Value.Y
					|| (*Node)->Pins.Num() != Expected.Pins.Num()) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
				const auto* const* A = Pins.Find(Value.A.StableKey());
				if (!A || !(*A)->LinkedPinKeys.Contains(Value.B.StableKey())) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
				const auto* const* A = Pins.Find(Value.A.StableKey());
				if (A && (*A)->LinkedPinKeys.Contains(Value.B.StableKey())) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
				const auto* const* Pin = Pins.Find(Value.Pin.StableKey());
				if (!Pin || (*Pin)->DefaultValue != Value.Value) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				if (Nodes.Contains(Value.Node.StableKey())) return false;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
				FString Error;
				const TMap<FString, FIntPoint> Positions = FHyperAIStudioBlueprintPatch::ComputeDeterministicLayout(Value, Error);
				if (!Error.IsEmpty()) return false;
				for (const TPair<FString, FIntPoint>& Pair : Positions)
				{
					const auto* const* Node = Nodes.Find(Pair.Key);
					if (!Node || (*Node)->X != Pair.Value.X || (*Node)->Y != Pair.Value.Y) return false;
				}
			}
		}
		return true;
	}
}

bool FHyperAIBlueprintGraphId::IsValid() const
{
	return GraphGuid.IsValid() && HyperAIStudio::BlueprintPatch::Private::IsBoundedPath(GraphPath);
}

FString FHyperAIBlueprintGraphId::StableKey() const
{
	return TEXT("graph:") + HyperAIStudio::BlueprintPatch::Private::GuidToken(GraphGuid) + TEXT("|") + GraphPath;
}

bool FHyperAIBlueprintNodeId::IsValid() const
{
	return Graph.IsValid() && NodeGuid.IsValid();
}

FString FHyperAIBlueprintNodeId::StableKey() const
{
	return Graph.StableKey() + TEXT("|node:") + HyperAIStudio::BlueprintPatch::Private::GuidToken(NodeGuid);
}

bool FHyperAIBlueprintPinId::IsValid() const
{
	return Node.IsValid() && PinIndex >= 0 && PinIndex < FHyperAIStudioBlueprintPatch::MaxPins
		&& HyperAIStudio::BlueprintPatch::Private::IsValidPinDirection(Direction)
		&& (PersistentGuid.IsValid() || !PinName.IsNone());
}

FString FHyperAIBlueprintPinId::StableKey() const
{
	FString DirectionToken;
	if (Direction == EHyperAIBlueprintPinDirection::Input)
	{
		DirectionToken = TEXT("in");
	}
	else if (Direction == EHyperAIBlueprintPinDirection::Output)
	{
		DirectionToken = TEXT("out");
	}
	else
	{
		DirectionToken = FString::Printf(TEXT("invalid-%u"), static_cast<uint8>(Direction));
	}
	const FString Local = PersistentGuid.IsValid()
		? TEXT("guid:") + HyperAIStudio::BlueprintPatch::Private::GuidToken(PersistentGuid)
		: FString::Printf(TEXT("fallback:%s:%s:%d"), *PinName.ToString(),
			*DirectionToken, PinIndex);
	return Node.StableKey() + TEXT("|pin:") + Local;
}

FString FHyperAIBlueprintPatchOperation::Kind() const
{
	if (Value.IsType<FHyperAIBlueprintMoveNode>()) return TEXT("move_node");
	if (Value.IsType<FHyperAIBlueprintCreateNode>()) return TEXT("create_node");
	if (Value.IsType<FHyperAIBlueprintCompileOnly>()) return TEXT("compile_only");
	if (Value.IsType<FHyperAIBlueprintConnectPins>()) return TEXT("connect_pins");
	if (Value.IsType<FHyperAIBlueprintBreakPinLink>()) return TEXT("break_pin_link");
	if (Value.IsType<FHyperAIBlueprintSetLiteralDefault>()) return TEXT("set_literal_default");
	if (Value.IsType<FHyperAIBlueprintDeleteNode>()) return TEXT("delete_node");
	if (Value.IsType<FHyperAIBlueprintLayoutNodes>()) return TEXT("layout_nodes");
	return TEXT("unsupported");
}

const TCHAR* FHyperAIStudioBlueprintPatch::LexToString(const EHyperAIBlueprintCreateNodeKind Kind)
{
	switch (Kind)
	{
	case EHyperAIBlueprintCreateNodeKind::Branch: return TEXT("branch");
	case EHyperAIBlueprintCreateNodeKind::Sequence: return TEXT("sequence");
	case EHyperAIBlueprintCreateNodeKind::Reroute: return TEXT("reroute");
	default: return TEXT("invalid");
	}
}

bool FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(
	const FHyperAIBlueprintCreateNode& Create,
	FHyperAIBlueprintPatchNodeSnapshot& OutNode,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	OutNode = {};
	OutError.Reset();
	if (!Create.Graph.IsValid() || !Create.NewNodeGuid.IsValid()
		|| !IsValidCreateNodeKind(Create.Kind)
		|| !CoordinateValid(Create.X) || !CoordinateValid(Create.Y))
	{
		OutError = TEXT("invalid_create_node_request");
		return false;
	}

	OutNode.Id = Create.NodeId();
	OutNode.X = Create.X;
	OutNode.Y = Create.Y;
	OutNode.bCanUserDelete = true;
	auto AddPin = [&OutNode](
		const FName Name,
		const EHyperAIBlueprintPinDirection Direction,
		const TCHAR* Type,
		const TCHAR* DefaultValue = TEXT(""),
		const bool bDefaultIgnored = false)
	{
		FHyperAIBlueprintPatchPinSnapshot& Pin = OutNode.Pins.AddDefaulted_GetRef();
		Pin.Id.Node = OutNode.Id;
		Pin.Id.PinName = Name;
		Pin.Id.Direction = Direction;
		Pin.Id.PinIndex = OutNode.Pins.Num() - 1;
		Pin.PinType = Type;
		Pin.DefaultValue = DefaultValue;
		Pin.bDefaultIgnored = bDefaultIgnored;
	};

	switch (Create.Kind)
	{
	case EHyperAIBlueprintCreateNodeKind::Branch:
		OutNode.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_IfThenElse");
		AddPin(UEdGraphSchema_K2::PN_Execute, EHyperAIBlueprintPinDirection::Input, TEXT("exec//0/0/0"));
		AddPin(UEdGraphSchema_K2::PN_Condition, EHyperAIBlueprintPinDirection::Input, TEXT("bool//0/0/0"), TEXT("true"));
		AddPin(UEdGraphSchema_K2::PN_Then, EHyperAIBlueprintPinDirection::Output, TEXT("exec//0/0/0"));
		AddPin(UEdGraphSchema_K2::PN_Else, EHyperAIBlueprintPinDirection::Output, TEXT("exec//0/0/0"));
		break;
	case EHyperAIBlueprintCreateNodeKind::Sequence:
		OutNode.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_ExecutionSequence");
		AddPin(UEdGraphSchema_K2::PN_Execute, EHyperAIBlueprintPinDirection::Input, TEXT("exec//0/0/0"));
		AddPin(FName(TEXT("then_0")), EHyperAIBlueprintPinDirection::Output, TEXT("exec//0/0/0"));
		AddPin(FName(TEXT("then_1")), EHyperAIBlueprintPinDirection::Output, TEXT("exec//0/0/0"));
		break;
	case EHyperAIBlueprintCreateNodeKind::Reroute:
		OutNode.NodeClassPath = TEXT("/Script/BlueprintGraph.K2Node_Knot");
		AddPin(FName(TEXT("InputPin")), EHyperAIBlueprintPinDirection::Input, TEXT("wildcard//0/0/0"), TEXT(""), true);
		AddPin(FName(TEXT("OutputPin")), EHyperAIBlueprintPinDirection::Output, TEXT("wildcard//0/0/0"));
		break;
	default:
		OutError = TEXT("invalid_create_node_kind");
		OutNode = {};
		return false;
	}
	return true;
}

FString FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(const FHyperAIBlueprintPatchPlan& Plan)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	if (!IsCanonicalSha256(Plan.PlanHash))
	{
		return {};
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.blueprint-effect.v1"));
	AppendToken(Canonical, Plan.PlanHash);
	AppendToken(Canonical, Plan.BaseRevision);
	AppendToken(Canonical, Plan.Safety == EHyperAIBlueprintPatchSafety::Destructive
		? TEXT("destructive") : TEXT("edit"));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.OperationCount));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.NoOpCount));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.NodesCreated));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.CompileRequests));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.NodesMoved));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.LinksAdded));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.LinksBroken));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.DefaultsChanged));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.NodesDeleted));
	AppendToken(Canonical, FString::FromInt(Plan.Effects.LayoutNodesMoved));
	AppendToken(Canonical, Plan.Effects.bRequiresCompile ? TEXT("1") : TEXT("0"));
	AppendToken(Canonical, Plan.Effects.bRequiresCallerSave ? TEXT("1") : TEXT("0"));
	return Sha256(Canonical);
}

FHyperAIBlueprintPatchSnapshot FHyperAIStudioBlueprintPatch::CaptureLoaded(UBlueprint* Blueprint)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	FHyperAIBlueprintPatchSnapshot Result;
	if (!IsInGameThread())
	{
		Result.Status = TEXT("game_thread_required");
		Result.Diagnostic = TEXT("Loaded Blueprint capture requires the Unreal game thread.");
		return Result;
	}
	if (!IsValid(Blueprint))
	{
		Result.Status = TEXT("blueprint_unavailable");
		Result.Diagnostic = TEXT("An already-loaded valid UBlueprint is required; this backend never loads assets.");
		return Result;
	}

	Result.BlueprintAssetPath = Blueprint->GetPathName();
	Result.CompileStatus = CompileStatus(Blueprint);
	Result.GeneratedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();
	if (!IsBoundedPath(Result.BlueprintAssetPath)
		|| Result.GeneratedClassPath.Len() > MaxPathCharacters)
	{
		Result.Status = TEXT("path_bound_exceeded");
		Result.Diagnostic = TEXT("Blueprint or generated-class path exceeds the fixed authoring bound.");
		return Result;
	}

	TArray<UEdGraph*> Graphs;
	FString GatherError;
	if (!GatherAuthoringGraphs(Blueprint, Graphs, GatherError))
	{
		Result.Status = GatherError;
		Result.Diagnostic = TEXT("Blueprint authoring graph capture exceeded a fixed source bound or encountered unsupported state.");
		return Result;
	}

	struct FLivePinLocation
	{
		UEdGraphPin* Pin = nullptr;
		int32 Graph = INDEX_NONE;
		int32 Node = INDEX_NONE;
		int32 PinIndex = INDEX_NONE;
	};
	TArray<FLivePinLocation> LivePins;
	TMap<const UEdGraphPin*, FString> PinKeys;
	TSet<FString> GraphKeys;
	TSet<FString> NodeKeys;
	TSet<FString> AllPinKeys;

	for (UEdGraph* Graph : Graphs)
	{
		FHyperAIBlueprintPatchGraphSnapshot& GraphSnapshot = Result.Graphs.AddDefaulted_GetRef();
		GraphSnapshot.Id = { Graph->GraphGuid, Graph->GetPathName() };
		GraphSnapshot.SchemaClassPath = Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString();
		if (!GraphSnapshot.Id.IsValid() || !IsBoundedPath(GraphSnapshot.SchemaClassPath)
			|| GraphKeys.Contains(GraphSnapshot.Id.StableKey()))
		{
			Result.Status = TEXT("unsupported_graph_identity");
			Result.Diagnostic = TEXT("Every authoring graph requires a unique valid GUID, bounded path, and loaded schema.");
			Result.Graphs.Reset();
			return Result;
		}
		GraphKeys.Add(GraphSnapshot.Id.StableKey());
		if (Result.NodeCount + Graph->Nodes.Num() > MaxNodes)
		{
			Result.Status = TEXT("node_source_bound_exceeded");
			Result.Graphs.Reset();
			return Result;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				Result.Status = TEXT("null_node_unsupported");
				Result.Graphs.Reset();
				return Result;
			}
			FHyperAIBlueprintPatchNodeSnapshot& NodeSnapshot = GraphSnapshot.Nodes.AddDefaulted_GetRef();
			NodeSnapshot.Id = { GraphSnapshot.Id, Node->NodeGuid };
			NodeSnapshot.NodeClassPath = Node->GetClass()->GetPathName();
			NodeSnapshot.X = Node->NodePosX;
			NodeSnapshot.Y = Node->NodePosY;
			NodeSnapshot.bCanUserDelete = Node->CanUserDeleteNode();
			++Result.NodeCount;
			if (!NodeSnapshot.Id.IsValid() || !IsBoundedPath(NodeSnapshot.NodeClassPath)
				|| NodeKeys.Contains(NodeSnapshot.Id.StableKey()))
			{
				Result.Status = TEXT("unsupported_node_identity");
				Result.Graphs.Reset();
				return Result;
			}
			NodeKeys.Add(NodeSnapshot.Id.StableKey());
			if (Result.PinCount + Node->Pins.Num() > MaxPins)
			{
				Result.Status = TEXT("pin_source_bound_exceeded");
				Result.Graphs.Reset();
				return Result;
			}
			for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
			{
				UEdGraphPin* Pin = Node->Pins[PinIndex];
				if (!Pin)
				{
					Result.Status = TEXT("null_pin_unsupported");
					Result.Graphs.Reset();
					return Result;
				}
				FHyperAIBlueprintPatchPinSnapshot& PinSnapshot = NodeSnapshot.Pins.AddDefaulted_GetRef();
				PinSnapshot.Id.Node = NodeSnapshot.Id;
				PinSnapshot.Id.PersistentGuid = Pin->PersistentGuid;
				PinSnapshot.Id.PinName = Pin->PinName;
				if (!TryConvertPinDirection(Pin->Direction, PinSnapshot.Id.Direction))
				{
					Result.Status = TEXT("unsupported_live_pin_direction");
					Result.Diagnostic = TEXT("A loaded graph pin has a direction outside the exhaustive input/output set.");
					Result.Graphs.Reset();
					return Result;
				}
				PinSnapshot.Id.PinIndex = PinIndex;
				PinSnapshot.PinType = PinTypeToken(Pin);
				PinSnapshot.DefaultValue = Pin->GetDefaultAsString();
				PinSnapshot.bDefaultReadOnly = Pin->bDefaultValueIsReadOnly;
				PinSnapshot.bDefaultIgnored = Pin->bDefaultValueIsIgnored;
				PinSnapshot.bNotConnectable = Pin->bNotConnectable;
				PinSnapshot.bOrphaned = Pin->bOrphanedPin;
				++Result.PinCount;
				const FString PinKey = PinSnapshot.Id.StableKey();
				if (!PinSnapshot.Id.IsValid() || PinSnapshot.PinType.Len() > MaxPathCharacters
					|| PinSnapshot.DefaultValue.Len() > MaxLiteralCharacters || AllPinKeys.Contains(PinKey))
				{
					Result.Status = TEXT("unsupported_pin_identity_or_value");
					Result.Diagnostic = TEXT("A pin has no revision-stable identity, duplicates an identity, or exceeds a fixed value/type bound.");
					Result.Graphs.Reset();
					return Result;
				}
				AllPinKeys.Add(PinKey);
				PinKeys.Add(Pin, PinKey);
				LivePins.Add({ Pin, Result.Graphs.Num() - 1, GraphSnapshot.Nodes.Num() - 1, NodeSnapshot.Pins.Num() - 1 });
			}
		}
	}

	TSet<FString> UniqueLinks;
	for (const FLivePinLocation& Location : LivePins)
	{
		auto& PinSnapshot = Result.Graphs[Location.Graph].Nodes[Location.Node].Pins[Location.PinIndex];
		for (UEdGraphPin* LinkedPin : Location.Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->LinkedTo.Contains(Location.Pin))
			{
				Result.Status = TEXT("asymmetric_pin_link");
				Result.Diagnostic = TEXT("A captured pin link is null or is not represented symmetrically by both endpoints.");
				Result.Graphs.Reset();
				return Result;
			}
			const FString* LinkedKey = PinKeys.Find(LinkedPin);
			if (!LinkedKey)
			{
				Result.Status = TEXT("external_or_unbounded_pin_link");
				Result.Diagnostic = TEXT("A graph pin link points outside the bounded captured authoring graph set.");
				Result.Graphs.Reset();
				return Result;
			}
			PinSnapshot.LinkedPinKeys.Add(*LinkedKey);
			UniqueLinks.Add(CanonicalLink(PinSnapshot.Id.StableKey(), *LinkedKey));
			if (UniqueLinks.Num() > MaxLinks)
			{
				Result.Status = TEXT("link_source_bound_exceeded");
				Result.Graphs.Reset();
				return Result;
			}
		}
		PinSnapshot.LinkedPinKeys.Sort();
	}

	for (auto& Graph : Result.Graphs)
	{
		for (auto& Node : Graph.Nodes)
		{
			Node.Pins.Sort([](const auto& A, const auto& B) { return A.Id.StableKey() < B.Id.StableKey(); });
		}
		Graph.Nodes.Sort([](const auto& A, const auto& B) { return A.Id.StableKey() < B.Id.StableKey(); });
	}
	Result.Graphs.Sort([](const auto& A, const auto& B) { return A.Id.StableKey() < B.Id.StableKey(); });
	Result.GraphCount = Result.Graphs.Num();
	Result.LinkCount = UniqueLinks.Num();

	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.blueprint-authoring-snapshot.v1"));
	AppendToken(Canonical, Result.BlueprintAssetPath);
	AppendToken(Canonical, Result.CompileStatus);
	AppendToken(Canonical, Result.GeneratedClassPath);
	for (const auto& Graph : Result.Graphs)
	{
		AppendToken(Canonical, Graph.Id.StableKey());
		AppendToken(Canonical, Graph.SchemaClassPath);
		for (const auto& Node : Graph.Nodes)
		{
			AppendToken(Canonical, Node.Id.StableKey());
			AppendToken(Canonical, Node.NodeClassPath);
			AppendToken(Canonical, FString::FromInt(Node.X));
			AppendToken(Canonical, FString::FromInt(Node.Y));
			AppendToken(Canonical, Node.bCanUserDelete ? TEXT("1") : TEXT("0"));
			for (const auto& Pin : Node.Pins)
			{
				AppendToken(Canonical, Pin.Id.StableKey());
				AppendToken(Canonical, Pin.PinType);
				AppendToken(Canonical, Pin.DefaultValue);
				AppendToken(Canonical, Pin.bDefaultReadOnly ? TEXT("1") : TEXT("0"));
				AppendToken(Canonical, Pin.bDefaultIgnored ? TEXT("1") : TEXT("0"));
				AppendToken(Canonical, Pin.bNotConnectable ? TEXT("1") : TEXT("0"));
				AppendToken(Canonical, Pin.bOrphaned ? TEXT("1") : TEXT("0"));
				for (const FString& Link : Pin.LinkedPinKeys) AppendToken(Canonical, Link);
			}
		}
	}
	Result.Revision = Sha256(Canonical);
	if (!IsCanonicalSha256(Result.Revision))
	{
		Result.Status = TEXT("revision_hash_unavailable");
		Result.Diagnostic = TEXT("The platform SHA-256 implementation did not produce a canonical revision.");
		Result.Graphs.Reset();
		Result.Revision.Reset();
		return Result;
	}
	Result.bComplete = true;
	Result.Status = TEXT("complete_loaded_revision");
	Result.Diagnostic = TEXT("Complete bounded loaded-Blueprint authoring snapshot; no asset load or save was performed.");
	return Result;
}

TMap<FString, FIntPoint> FHyperAIStudioBlueprintPatch::ComputeDeterministicLayout(
	const FHyperAIBlueprintLayoutNodes& Layout,
	FString& OutError)
{
	OutError.Reset();
	TMap<FString, FIntPoint> Result;
	if (!Layout.Graph.IsValid() || Layout.Nodes.IsEmpty() || Layout.Nodes.Num() > MaxLayoutNodes
		|| Layout.Columns < 1 || Layout.Columns > 64
		|| Layout.HorizontalSpacing < 1 || Layout.HorizontalSpacing > 10000
		|| Layout.VerticalSpacing < 1 || Layout.VerticalSpacing > 10000
		|| !HyperAIStudio::BlueprintPatch::Private::CoordinateValid(Layout.OriginX)
		|| !HyperAIStudio::BlueprintPatch::Private::CoordinateValid(Layout.OriginY))
	{
		OutError = TEXT("invalid_layout_bounds");
		return Result;
	}
	TArray<FHyperAIBlueprintNodeId> Nodes = Layout.Nodes;
	Nodes.Sort([](const auto& A, const auto& B) { return A.StableKey() < B.StableKey(); });
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].IsValid() || Nodes[Index].Graph.StableKey() != Layout.Graph.StableKey()
			|| (Index > 0 && Nodes[Index - 1].StableKey() == Nodes[Index].StableKey()))
		{
			OutError = TEXT("invalid_or_duplicate_layout_node");
			Result.Reset();
			return Result;
		}
		const int64 X = static_cast<int64>(Layout.OriginX) + static_cast<int64>(Index % Layout.Columns) * Layout.HorizontalSpacing;
		const int64 Y = static_cast<int64>(Layout.OriginY) + static_cast<int64>(Index / Layout.Columns) * Layout.VerticalSpacing;
		if (FMath::Abs(X) > MaxCoordinate || FMath::Abs(Y) > MaxCoordinate)
		{
			OutError = TEXT("layout_coordinate_bound_exceeded");
			Result.Reset();
			return Result;
		}
		Result.Add(Nodes[Index].StableKey(), FIntPoint(static_cast<int32>(X), static_cast<int32>(Y)));
	}
	return Result;
}

FHyperAIBlueprintPatchPlan FHyperAIStudioBlueprintPatch::PlanSnapshot(
	const FHyperAIBlueprintPatchSnapshot& Snapshot,
	const FHyperAIBlueprintPatch& Patch)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	FHyperAIBlueprintPatchPlan Plan;
	Plan.bValid = true;
	Plan.BaseRevision = Snapshot.Revision;
	Plan.Effects.OperationCount = Patch.Operations.Num();
	if (!Snapshot.bComplete || !IsCanonicalSha256(Snapshot.Revision))
	{
		AddIssue(Plan, TEXT("snapshot_incomplete"), INDEX_NONE, Snapshot.BlueprintAssetPath,
			TEXT("A complete bounded snapshot with a canonical sha256 revision is required."));
	}
	if (Patch.TargetAssetPath != Snapshot.BlueprintAssetPath || !IsBoundedPath(Patch.TargetAssetPath))
	{
		AddIssue(Plan, TEXT("target_precondition_failed"), INDEX_NONE, Patch.TargetAssetPath, TEXT("Patch target must exactly match the loaded Blueprint path."));
	}
	if (Patch.ExpectedRevision.IsEmpty() || Patch.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(Plan, TEXT("revision_precondition_failed"), INDEX_NONE, Patch.ExpectedRevision, TEXT("Patch revision does not match the immutable loaded snapshot."));
	}
	if (Patch.Operations.IsEmpty() || Patch.Operations.Num() > MaxOperations)
	{
		AddIssue(Plan, TEXT("operation_bound_invalid"), INDEX_NONE, {}, TEXT("A patch requires 1..256 typed operations."));
	}

	TMap<FString, const FHyperAIBlueprintPatchNodeSnapshot*> Nodes;
	TMap<FString, const FHyperAIBlueprintPatchPinSnapshot*> Pins;
	TSet<FString> Links;
	TSet<FString> GraphKeys;
	int32 ObservedNodeCount = 0;
	int32 ObservedPinCount = 0;
	if (Snapshot.Graphs.Num() > MaxGraphs)
	{
		AddIssue(Plan, TEXT("snapshot_graph_bound_exceeded"), INDEX_NONE, Snapshot.BlueprintAssetPath,
			TEXT("Snapshot graph count exceeds the fixed planner bound."));
	}
	for (const auto& Graph : Snapshot.Graphs)
	{
		if (!Graph.Id.IsValid() || GraphKeys.Contains(Graph.Id.StableKey()))
		{
			AddIssue(Plan, TEXT("snapshot_graph_identity_invalid"), INDEX_NONE, Graph.Id.StableKey(),
				TEXT("Snapshot graph identities must be valid and unique."));
		}
		GraphKeys.Add(Graph.Id.StableKey());
		for (const auto& Node : Graph.Nodes)
		{
			++ObservedNodeCount;
			if (!Node.Id.IsValid() || Node.Id.Graph.StableKey() != Graph.Id.StableKey()
				|| Nodes.Contains(Node.Id.StableKey()))
			{
				AddIssue(Plan, TEXT("snapshot_node_identity_invalid"), INDEX_NONE, Node.Id.StableKey(),
					TEXT("Snapshot node identities must be valid, unique, and owned by their containing graph."));
			}
			Nodes.Add(Node.Id.StableKey(), &Node);
			Plan.PlannedNodePositions.Add(Node.Id.StableKey(), FIntPoint(Node.X, Node.Y));
			for (const auto& Pin : Node.Pins)
			{
				++ObservedPinCount;
				if (!Pin.Id.IsValid() || Pin.Id.Node.StableKey() != Node.Id.StableKey()
					|| Pins.Contains(Pin.Id.StableKey()))
				{
					AddIssue(Plan, TEXT("snapshot_pin_identity_invalid"), INDEX_NONE, Pin.Id.StableKey(),
						TEXT("Snapshot pin identities must be valid, unique, and owned by their containing node."));
				}
				Pins.Add(Pin.Id.StableKey(), &Pin);
			}
		}
	}
	if (ObservedNodeCount > MaxNodes || ObservedPinCount > MaxPins
		|| Snapshot.GraphCount != Snapshot.Graphs.Num()
		|| Snapshot.NodeCount != ObservedNodeCount || Snapshot.PinCount != ObservedPinCount)
	{
		AddIssue(Plan, TEXT("snapshot_count_invalid"), INDEX_NONE, Snapshot.BlueprintAssetPath,
			TEXT("Snapshot counts must exactly match their bounded value arrays."));
	}
	for (const TPair<FString, const FHyperAIBlueprintPatchPinSnapshot*>& Pair : Pins)
	{
		for (const FString& Linked : Pair.Value->LinkedPinKeys)
		{
			const FHyperAIBlueprintPatchPinSnapshot* const* Other = Pins.Find(Linked);
			if (!Other || !(*Other)->LinkedPinKeys.Contains(Pair.Key))
			{
				AddIssue(Plan, TEXT("snapshot_link_invalid"), INDEX_NONE, Pair.Key,
					TEXT("Every snapshot link endpoint must exist and name the reverse endpoint."));
				continue;
			}
			Links.Add(CanonicalLink(Pair.Key, Linked));
		}
	}
	if (Links.Num() > MaxLinks || Snapshot.LinkCount != Links.Num())
	{
		AddIssue(Plan, TEXT("snapshot_link_count_invalid"), INDEX_NONE, Snapshot.BlueprintAssetPath,
			TEXT("Snapshot link count must exactly match its bounded symmetric link set."));
	}
	TArray<FHyperAIBlueprintPatchNodeSnapshot> CreatedTemplates;
	CreatedTemplates.Reserve(FMath::Min(Patch.Operations.Num(), MaxOperations));
	TSet<FString> ValidCreatedNodes;
	TSet<int32> ValidCreateOperationIndices;
	bool bSawNonCreateOperation = false;
	for (int32 Index = 0; Index < Patch.Operations.Num() && Index < MaxOperations; ++Index)
	{
		const FHyperAIBlueprintPatchOperation& Operation = Patch.Operations[Index];
		if (!Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
		{
			bSawNonCreateOperation = true;
			continue;
		}
		const FHyperAIBlueprintCreateNode& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
		const FString NodeKey = Value.NodeId().StableKey();
		if (bSawNonCreateOperation)
		{
			AddIssue(Plan, TEXT("create_operation_order_invalid"), Index, NodeKey,
				TEXT("All create_node operations must precede operations that address existing or newly created pins."));
			continue;
		}
		FHyperAIBlueprintPatchNodeSnapshot Template;
		FString TemplateError;
		if (!BuildAllowedNodeTemplate(Value, Template, TemplateError))
		{
			AddIssue(Plan, TemplateError, Index, NodeKey, TEXT("The closed allowlisted node template request is invalid."));
			continue;
		}
		if (!GraphKeys.Contains(Value.Graph.StableKey()))
		{
			AddIssue(Plan, TEXT("create_graph_not_found"), Index, Value.Graph.StableKey(),
				TEXT("The create target graph is absent from the immutable loaded snapshot."));
			continue;
		}
		if (Nodes.Contains(NodeKey) || ValidCreatedNodes.Contains(NodeKey))
		{
			AddIssue(Plan, TEXT("create_node_guid_collision"), Index, NodeKey,
				TEXT("Every new node GUID must be valid, unique, and absent from the loaded revision."));
			continue;
		}
		ValidCreatedNodes.Add(NodeKey);
		ValidCreateOperationIndices.Add(Index);
		CreatedTemplates.Add(MoveTemp(Template));
	}
	for (const FHyperAIBlueprintPatchNodeSnapshot& Node : CreatedTemplates)
	{
		Nodes.Add(Node.Id.StableKey(), &Node);
		Plan.PlannedNodePositions.Add(Node.Id.StableKey(), FIntPoint(Node.X, Node.Y));
		for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
		{
			if (Pins.Contains(Pin.Id.StableKey()))
			{
				AddIssue(Plan, TEXT("create_pin_identity_collision"), INDEX_NONE, Pin.Id.StableKey(),
					TEXT("An allowlisted created pin identity collides with the loaded revision."));
			}
			else
			{
				Pins.Add(Pin.Id.StableKey(), &Pin);
			}
		}
	}
	int32 CreatedPinCount = 0;
	for (const FHyperAIBlueprintPatchNodeSnapshot& Node : CreatedTemplates)
	{
		CreatedPinCount += Node.Pins.Num();
	}
	if (ObservedNodeCount + CreatedTemplates.Num() > MaxNodes
		|| ObservedPinCount + CreatedPinCount > MaxPins)
	{
		AddIssue(Plan, TEXT("created_projection_bound_exceeded"), INDEX_NONE, Patch.TargetAssetPath,
			TEXT("Loaded plus projected allowlisted nodes exceed the fixed node or pin bound."));
	}
	TSet<FString> DeletedNodes;
	TSet<FString> PatchDeleteNodes;
	TSet<FString> PositionOperationNodes;
	TSet<FString> LiteralOperationPins;
	TSet<FString> LinkOperationPins;
	bool bCompileOnlySeen = false;
	for (int32 Index = 0; Index < Patch.Operations.Num() && Index < MaxOperations; ++Index)
	{
		const auto& Operation = Patch.Operations[Index];
		if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
		{
			const FString NodeKey = Operation.Value.Get<FHyperAIBlueprintDeleteNode>().Node.StableKey();
			if (PatchDeleteNodes.Contains(NodeKey))
			{
				AddIssue(Plan, TEXT("conflicting_delete_operations"), Index, NodeKey,
					TEXT("A node may be deleted at most once in a patch."));
			}
			PatchDeleteNodes.Add(NodeKey);
			if (ValidCreatedNodes.Contains(NodeKey))
			{
				AddIssue(Plan, TEXT("create_delete_conflict"), Index, NodeKey,
					TEXT("A node cannot be created and deleted in the same bounded patch."));
			}
		}
	}

	for (int32 Index = 0; Index < Patch.Operations.Num() && Index < MaxOperations; ++Index)
	{
		const auto& Operation = Patch.Operations[Index];
		if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
			if (ValidCreateOperationIndices.Contains(Index))
			{
				++Plan.Effects.NodesCreated;
				PositionOperationNodes.Add(Value.NodeId().StableKey());
			}
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintCompileOnly>())
		{
			if (bCompileOnlySeen)
			{
				AddIssue(Plan, TEXT("duplicate_compile_request"), Index, Patch.TargetAssetPath,
					TEXT("A patch may request the closed compile/validate action at most once."));
			}
			else
			{
				bCompileOnlySeen = true;
				++Plan.Effects.CompileRequests;
			}
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
			const auto* const* Node = Nodes.Find(Value.Node.StableKey());
			if (PatchDeleteNodes.Contains(Value.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("delete_operation_conflict"), Index, Value.Node.StableKey(),
					TEXT("A node scheduled for deletion cannot also be moved or laid out in the same patch."));
			}
			else if (PositionOperationNodes.Contains(Value.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("conflicting_position_operations"), Index, Value.Node.StableKey(),
					TEXT("A node position may be written at most once in a patch."));
			}
			else if (!Node || DeletedNodes.Contains(Value.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("node_not_found"), Index, Value.Node.StableKey(), TEXT("Move target is absent or already scheduled for deletion."));
			}
			else if (!CoordinateValid(Value.X) || !CoordinateValid(Value.Y))
			{
				AddIssue(Plan, TEXT("coordinate_bound_exceeded"), Index, Value.Node.StableKey(), TEXT("Node coordinates exceed the fixed bound."));
			}
			else if ((*Node)->X == Value.X && (*Node)->Y == Value.Y)
			{
				++Plan.Effects.NoOpCount;
			}
			else
			{
				Plan.PlannedNodePositions.Add(Value.Node.StableKey(), FIntPoint(Value.X, Value.Y));
				++Plan.Effects.NodesMoved;
			}
			PositionOperationNodes.Add(Value.Node.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
			const auto* const* A = Pins.Find(Value.A.StableKey());
			const auto* const* B = Pins.Find(Value.B.StableKey());
			if (!IsValidPinDirection(Value.A.Direction) || !IsValidPinDirection(Value.B.Direction))
			{
				AddIssue(Plan, TEXT("invalid_pin_direction"), Index, Value.A.StableKey(),
					TEXT("Connection pin directions must be exactly input or output."));
			}
			else if (PatchDeleteNodes.Contains(Value.A.Node.StableKey()) || PatchDeleteNodes.Contains(Value.B.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("delete_operation_conflict"), Index, Value.A.StableKey(),
					TEXT("Pins on a node scheduled for deletion cannot also participate in link operations."));
			}
			else if (LinkOperationPins.Contains(Value.A.StableKey()) || LinkOperationPins.Contains(Value.B.StableKey()))
			{
				AddIssue(Plan, TEXT("conflicting_link_operations"), Index, Value.A.StableKey(),
					TEXT("Each pin may participate in at most one link operation per patch in this conservative core."));
			}
			else if (!A || !B || DeletedNodes.Contains(Value.A.Node.StableKey()) || DeletedNodes.Contains(Value.B.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("pin_not_found"), Index, Value.A.StableKey(), TEXT("Connection pin is absent or belongs to a node scheduled for deletion."));
			}
			else if (Value.A.Node.Graph.StableKey() != Value.B.Node.Graph.StableKey()
				|| Value.A.Direction == Value.B.Direction || (*A)->bNotConnectable || (*B)->bNotConnectable
				|| (*A)->bOrphaned || (*B)->bOrphaned)
			{
				AddIssue(Plan, TEXT("unsupported_connection"), Index, Value.A.StableKey(), TEXT("Connections require opposite-direction, connectable, non-orphan pins in one graph."));
			}
			else
			{
				const FString Link = CanonicalLink(Value.A.StableKey(), Value.B.StableKey());
				if (Links.Contains(Link)) ++Plan.Effects.NoOpCount;
				else { Links.Add(Link); ++Plan.Effects.LinksAdded; }
			}
			LinkOperationPins.Add(Value.A.StableKey());
			LinkOperationPins.Add(Value.B.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
			if (!IsValidPinDirection(Value.A.Direction) || !IsValidPinDirection(Value.B.Direction))
			{
				AddIssue(Plan, TEXT("invalid_pin_direction"), Index, Value.A.StableKey(),
					TEXT("Break-link pin directions must be exactly input or output."));
			}
			else if (PatchDeleteNodes.Contains(Value.A.Node.StableKey()) || PatchDeleteNodes.Contains(Value.B.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("delete_operation_conflict"), Index, Value.A.StableKey(),
					TEXT("Pins on a node scheduled for deletion cannot also participate in link operations."));
			}
			else if (LinkOperationPins.Contains(Value.A.StableKey()) || LinkOperationPins.Contains(Value.B.StableKey()))
			{
				AddIssue(Plan, TEXT("conflicting_link_operations"), Index, Value.A.StableKey(),
					TEXT("Each pin may participate in at most one link operation per patch in this conservative core."));
			}
			else if (!Pins.Contains(Value.A.StableKey()) || !Pins.Contains(Value.B.StableKey()))
			{
				AddIssue(Plan, TEXT("pin_not_found"), Index, Value.A.StableKey(), TEXT("Break-link endpoint is absent."));
			}
			else
			{
				const FString Link = CanonicalLink(Value.A.StableKey(), Value.B.StableKey());
				if (!Links.Contains(Link)) ++Plan.Effects.NoOpCount;
				else { Links.Remove(Link); ++Plan.Effects.LinksBroken; }
			}
			LinkOperationPins.Add(Value.A.StableKey());
			LinkOperationPins.Add(Value.B.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
			const auto* const* Pin = Pins.Find(Value.Pin.StableKey());
			if (!IsValidPinDirection(Value.Pin.Direction))
			{
				AddIssue(Plan, TEXT("invalid_pin_direction"), Index, Value.Pin.StableKey(),
					TEXT("Default target direction must be exactly input or output."));
			}
			else if (PatchDeleteNodes.Contains(Value.Pin.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("delete_operation_conflict"), Index, Value.Pin.StableKey(),
					TEXT("Pins on a node scheduled for deletion cannot also receive defaults."));
			}
			else if (LiteralOperationPins.Contains(Value.Pin.StableKey()))
			{
				AddIssue(Plan, TEXT("conflicting_default_operations"), Index, Value.Pin.StableKey(),
					TEXT("A pin default may be written at most once in a patch."));
			}
			else if (!Pin || DeletedNodes.Contains(Value.Pin.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("pin_not_found"), Index, Value.Pin.StableKey(), TEXT("Default target pin is absent or scheduled for deletion."));
			}
			else if (Value.Value.Len() > MaxLiteralCharacters || ContainsEmbeddedNull(Value.Value))
			{
				AddIssue(Plan, TEXT("literal_bound_invalid"), Index, Value.Pin.StableKey(), TEXT("Literal is oversized or contains an embedded null."));
			}
			else if ((*Pin)->bDefaultReadOnly || (*Pin)->bDefaultIgnored || !(*Pin)->LinkedPinKeys.IsEmpty())
			{
				AddIssue(Plan, TEXT("unsupported_default_target"), Index, Value.Pin.StableKey(), TEXT("Default target must be writable, observed, and unlinked."));
			}
			else if ((*Pin)->DefaultValue == Value.Value) ++Plan.Effects.NoOpCount;
			else ++Plan.Effects.DefaultsChanged;
			LiteralOperationPins.Add(Value.Pin.StableKey());
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
			const auto* const* Node = Nodes.Find(Value.Node.StableKey());
			if (!IsValidDeleteMode(Value.Mode))
			{
				AddIssue(Plan, TEXT("invalid_delete_mode"), Index, Value.Node.StableKey(),
					TEXT("Delete mode must be exactly orphan_only or explicit_confirmed."));
			}
			else if (!Node || DeletedNodes.Contains(Value.Node.StableKey()))
			{
				AddIssue(Plan, TEXT("node_not_found"), Index, Value.Node.StableKey(), TEXT("Delete target is absent or already scheduled for deletion."));
			}
			else if (!(*Node)->bCanUserDelete)
			{
				AddIssue(Plan, TEXT("node_not_user_deletable"), Index, Value.Node.StableKey(), TEXT("The Blueprint schema does not allow user deletion of this node."));
			}
			else
			{
				bool bHasLinks = false;
				for (const auto& Pin : (*Node)->Pins) bHasLinks |= !Pin.LinkedPinKeys.IsEmpty();
				if (Value.Mode == EHyperAIBlueprintDeleteMode::OrphanOnly && bHasLinks)
				{
					AddIssue(Plan, TEXT("node_not_orphan"), Index, Value.Node.StableKey(), TEXT("Orphan-only deletion requires a completely unlinked node."));
				}
				else
				{
					DeletedNodes.Add(Value.Node.StableKey());
					for (const auto& Pin : (*Node)->Pins)
					{
						for (const FString& Linked : Pin.LinkedPinKeys) Links.Remove(CanonicalLink(Pin.Id.StableKey(), Linked));
					}
					++Plan.Effects.NodesDeleted;
				}
			}
		}
		else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
		{
			const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
			FString LayoutError;
			const TMap<FString, FIntPoint> Layout = ComputeDeterministicLayout(Value, LayoutError);
			if (!LayoutError.IsEmpty())
			{
				AddIssue(Plan, LayoutError, Index, Value.Graph.StableKey(), TEXT("Deterministic layout request is invalid."));
			}
			else
			{
				for (const TPair<FString, FIntPoint>& Pair : Layout)
				{
					const auto* const* Node = Nodes.Find(Pair.Key);
					if (PatchDeleteNodes.Contains(Pair.Key))
					{
						AddIssue(Plan, TEXT("delete_operation_conflict"), Index, Pair.Key,
							TEXT("A node scheduled for deletion cannot also be laid out."));
						continue;
					}
					else if (PositionOperationNodes.Contains(Pair.Key))
					{
						AddIssue(Plan, TEXT("conflicting_position_operations"), Index, Pair.Key,
							TEXT("A node position may be written at most once in a patch."));
						continue;
					}
					else if (!Node || DeletedNodes.Contains(Pair.Key))
					{
						AddIssue(Plan, TEXT("layout_node_not_found"), Index, Pair.Key, TEXT("Layout target node is absent or scheduled for deletion."));
						continue;
					}
					const FIntPoint Current = Plan.PlannedNodePositions.FindRef(Pair.Key);
					if (Current == Pair.Value) ++Plan.Effects.NoOpCount;
					else { Plan.PlannedNodePositions.Add(Pair.Key, Pair.Value); ++Plan.Effects.LayoutNodesMoved; }
					PositionOperationNodes.Add(Pair.Key);
				}
			}
		}
	}

	const int32 EffectCount = Plan.Effects.NodesCreated + Plan.Effects.CompileRequests + Plan.Effects.NodesMoved + Plan.Effects.LinksAdded + Plan.Effects.LinksBroken
		+ Plan.Effects.DefaultsChanged + Plan.Effects.NodesDeleted + Plan.Effects.LayoutNodesMoved;
	Plan.Safety = Plan.Effects.NodesDeleted > 0
		? EHyperAIBlueprintPatchSafety::Destructive
		: EHyperAIBlueprintPatchSafety::Edit;
	Plan.Effects.bRequiresCompile = EffectCount > 0;
	Plan.Effects.bRequiresCallerSave = EffectCount > 0;
	FString PlanCanonical;
	AppendToken(PlanCanonical, TEXT("hyperai.blueprint-patch-plan.v2"));
	AppendToken(PlanCanonical, Patch.TargetAssetPath);
	AppendToken(PlanCanonical, Patch.ExpectedRevision);
	AppendToken(PlanCanonical, Plan.Safety == EHyperAIBlueprintPatchSafety::Destructive
		? TEXT("destructive") : TEXT("edit"));
	for (const auto& Operation : Patch.Operations) AppendToken(PlanCanonical, OperationCanonical(Operation));
	Plan.PlanHash = Sha256(PlanCanonical);
	if (!IsCanonicalSha256(Plan.PlanHash))
	{
		AddIssue(Plan, TEXT("plan_hash_unavailable"), INDEX_NONE, Patch.TargetAssetPath,
			TEXT("The platform SHA-256 implementation did not produce a canonical plan hash."));
	}
	Plan.Status = Plan.bValid ? (EffectCount > 0 ? TEXT("valid") : TEXT("no_changes")) : TEXT("invalid");
	return Plan;
}

bool FHyperAIStudioBlueprintPatch::ValidatePlanSafety(
	const FHyperAIBlueprintPatchPlan& Plan,
	const EHyperAIBlueprintPatchSafety RequiredSafety,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	OutError.Reset();
	if (!Plan.bValid)
	{
		OutError = TEXT("patch_plan_invalid");
		return false;
	}
	if (!IsValidPatchSafety(RequiredSafety) || !IsValidPatchSafety(Plan.Safety))
	{
		OutError = TEXT("invalid_patch_safety");
		return false;
	}
	if (Plan.Safety != RequiredSafety)
	{
		OutError = RequiredSafety == EHyperAIBlueprintPatchSafety::Edit
			? TEXT("delete_operation_requires_destructive_route")
			: TEXT("destructive_route_requires_delete_operation");
		return false;
	}
	if ((RequiredSafety == EHyperAIBlueprintPatchSafety::Edit && Plan.Effects.NodesDeleted != 0)
		|| (RequiredSafety == EHyperAIBlueprintPatchSafety::Destructive && Plan.Effects.NodesDeleted <= 0))
	{
		OutError = TEXT("patch_effect_safety_mismatch");
		return false;
	}
	return true;
}

bool FHyperAIStudioBlueprintPatch::TrySafetyForTypedOperation(
	const FString& OperationType,
	EHyperAIBlueprintPatchSafety& OutSafety)
{
	if (OperationType == EditTypedOperationType)
	{
		OutSafety = EHyperAIBlueprintPatchSafety::Edit;
		return true;
	}
	if (OperationType == DeleteTypedOperationType)
	{
		OutSafety = EHyperAIBlueprintPatchSafety::Destructive;
		return true;
	}
	return false;
}

FHyperAIBlueprintPatchResult FHyperAIStudioBlueprintPatch::ValidateLoaded(
	UBlueprint* Blueprint,
	const FHyperAIBlueprintPatch& Patch,
	const EHyperAIBlueprintPatchSafety RequiredSafety,
	FHyperAIBlueprintPatchSnapshot& OutSnapshot,
	FHyperAIBlueprintPatchPlan& OutPlan)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	FHyperAIBlueprintPatchResult Result;
	if (!IsInGameThread())
	{
		Result.Status = TEXT("game_thread_required");
		Result.Diagnostic = TEXT("Blueprint patch validation requires the Unreal game thread.");
		return Result;
	}
	OutSnapshot = CaptureLoaded(Blueprint);
	if (!OutSnapshot.bComplete)
	{
		Result.Status = OutSnapshot.Status;
		Result.Diagnostic = OutSnapshot.Diagnostic;
		return Result;
	}
	OutPlan = PlanSnapshot(OutSnapshot, Patch);
	Result.RevisionBefore = OutSnapshot.Revision;
	Result.PlanHash = OutPlan.PlanHash;
	Result.Effects = OutPlan.Effects;
	Result.Issues = OutPlan.Issues;
	if (!OutPlan.bValid)
	{
		Result.Status = TEXT("invalid_patch");
		Result.Diagnostic = TEXT("Pure patch planning rejected one or more operations before mutation.");
		return Result;
	}
	FString SafetyError;
	if (!ValidatePlanSafety(OutPlan, RequiredSafety, SafetyError))
	{
		Result.Status = TEXT("patch_safety_mismatch");
		Result.Diagnostic = SafetyError;
		return Result;
	}
	FLiveIndex Index;
	FString IndexError;
	if (!BuildLiveIndex(Blueprint, Index, IndexError))
	{
		Result.Status = TEXT("live_index_unavailable");
		Result.Diagnostic = IndexError;
		return Result;
	}
	if (!ValidateLiveSchema(Patch, OutPlan, Index, Result.Issues))
	{
		Result.Status = TEXT("unsupported_or_invalid_live_operation");
		Result.Diagnostic = TEXT("Loaded K2 schema validation rejected an operation; no mutation occurred.");
		return Result;
	}
	Result.bOk = true;
	Result.Status = OutPlan.Status;
	Result.Diagnostic = TEXT("Target, revision, bounds, simulation, and loaded K2 schema checks passed.");
	return Result;
}

FHyperAIBlueprintPatchResult FHyperAIStudioBlueprintPatch::DryRunLoaded(
	UBlueprint* Blueprint,
	const FHyperAIBlueprintPatch& Patch,
	const FString& TypedOperationType)
{
	EHyperAIBlueprintPatchSafety RequiredSafety = EHyperAIBlueprintPatchSafety::Edit;
	if (!TrySafetyForTypedOperation(TypedOperationType, RequiredSafety))
	{
		FHyperAIBlueprintPatchResult InvalidRoute;
		InvalidRoute.bDryRun = true;
		InvalidRoute.Status = TEXT("typed_operation_not_supported");
		InvalidRoute.Diagnostic = TEXT("Blueprint patches accept only the exact edit or destructive TypedPlan operation id.");
		return InvalidRoute;
	}
	FHyperAIBlueprintPatchSnapshot Snapshot;
	FHyperAIBlueprintPatchPlan Plan;
	FHyperAIBlueprintPatchResult Result = ValidateLoaded(Blueprint, Patch, RequiredSafety, Snapshot, Plan);
	Result.bDryRun = true;
	Result.bCallerSaveRequired = false;
	Result.bSavePerformed = false;
	if (Result.bOk)
	{
		Result.Status = Plan.Status == TEXT("no_changes") ? TEXT("dry_run_no_changes") : TEXT("dry_run_valid");
		Result.Diagnostic = TEXT("Dry-run completed without mutation, compile, transaction, asset load, or save.");
	}
	return Result;
}

FHyperAIBlueprintPatchResult FHyperAIStudioBlueprintPatch::ApplyLoaded(
	UBlueprint* Blueprint,
	const FHyperAIBlueprintPatch& Patch,
	const FString& TypedOperationType)
{
	using namespace HyperAIStudio::BlueprintPatch::Private;
	EHyperAIBlueprintPatchSafety RequiredSafety = EHyperAIBlueprintPatchSafety::Edit;
	if (!TrySafetyForTypedOperation(TypedOperationType, RequiredSafety))
	{
		FHyperAIBlueprintPatchResult InvalidRoute;
		InvalidRoute.Status = TEXT("typed_operation_not_supported");
		InvalidRoute.Diagnostic = TEXT("Blueprint patches accept only the exact edit or destructive TypedPlan operation id.");
		return InvalidRoute;
	}
	TArray<FHyperAIBlueprintPatchIssue> LiveEnumIssues;
	if (!ValidateOperationEnums(Patch, LiveEnumIssues))
	{
		FHyperAIBlueprintPatchResult InvalidEnums;
		InvalidEnums.Status = TEXT("invalid_operation_enum");
		InvalidEnums.Diagnostic = TEXT("Live apply rejected an operation enum before Blueprint access or transaction start.");
		InvalidEnums.Issues = MoveTemp(LiveEnumIssues);
		return InvalidEnums;
	}
	FHyperAIBlueprintPatchSnapshot Before;
	FHyperAIBlueprintPatchPlan Plan;
	FHyperAIBlueprintPatchResult Result = ValidateLoaded(Blueprint, Patch, RequiredSafety, Before, Plan);
	if (!Result.bOk)
	{
		return Result;
	}
	if (!Plan.Effects.bRequiresCompile)
	{
		Result.Status = TEXT("no_changes");
		Result.Diagnostic = TEXT("Patch is valid but produces no effects; no transaction, compile, or save occurred.");
		return Result;
	}
	if (!GEditor || !GEditor->Trans || GEditor->Trans->IsActive())
	{
		Result.bOk = false;
		Result.Status = TEXT("transaction_unavailable");
		Result.Diagnostic = TEXT("An idle editor transactor is required so exact ownership and failure undo can be proven.");
		return Result;
	}

	FLiveIndex Index;
	FString IndexError;
	if (!BuildLiveIndex(Blueprint, Index, IndexError))
	{
		Result.bOk = false;
		Result.Status = TEXT("live_index_unavailable");
		Result.Diagnostic = IndexError;
		return Result;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Result.TransactionContext = TransactionContext;
	Result.TransactionTitle = TEXT("HyperAI Blueprint Patch ") + UniqueSuffix;
	bool bFailed = false;
	FString Failure;
	int32 AppliedEffects = 0;
	{
		FScopedTransaction Transaction(TransactionContext, FText::FromString(Result.TransactionTitle), Blueprint, true);
		Blueprint->Modify();
		for (const FHyperAIBlueprintPatchOperation& Operation : Patch.Operations)
		{
			if (Operation.Value.IsType<FHyperAIBlueprintCreateNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintCreateNode>();
				UEdGraph* Graph = Index.Graphs.FindRef(Value.Graph.StableKey());
				if (!Graph)
				{
					bFailed = true; Failure = TEXT("create_graph_disappeared"); break;
				}
				Graph->Modify();
				Result.bMutationStarted = true;
				UEdGraphNode* Created = nullptr;
				switch (Value.Kind)
				{
				case EHyperAIBlueprintCreateNodeKind::Branch:
					{
						FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
						UK2Node_IfThenElse* Node = Creator.CreateNode(false);
						Node->NodePosX = Value.X; Node->NodePosY = Value.Y;
						Creator.Finalize(); Created = Node;
					}
					break;
				case EHyperAIBlueprintCreateNodeKind::Sequence:
					{
						FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
						UK2Node_ExecutionSequence* Node = Creator.CreateNode(false);
						Node->NodePosX = Value.X; Node->NodePosY = Value.Y;
						Creator.Finalize(); Created = Node;
					}
					break;
				case EHyperAIBlueprintCreateNodeKind::Reroute:
					{
						FGraphNodeCreator<UK2Node_Knot> Creator(*Graph);
						UK2Node_Knot* Node = Creator.CreateNode(false);
						Node->NodePosX = Value.X; Node->NodePosY = Value.Y;
						Creator.Finalize(); Created = Node;
					}
					break;
				default:
					bFailed = true; Failure = TEXT("invalid_create_node_kind"); break;
				}
				if (bFailed || !Created)
				{
					if (!bFailed) { bFailed = true; Failure = TEXT("create_node_failed"); }
					break;
				}
				Created->NodeGuid = Value.NewNodeGuid;
				FHyperAIBlueprintPatchNodeSnapshot Expected;
				FString TemplateError;
				if (!BuildAllowedNodeTemplate(Value, Expected, TemplateError)
					|| Created->GetClass()->GetPathName() != Expected.NodeClassPath
					|| Created->Pins.Num() != Expected.Pins.Num())
				{
					bFailed = true; Failure = TEXT("created_node_template_mismatch"); break;
				}
				Index.Nodes.Add(Expected.Id.StableKey(), Created);
				for (int32 PinIndex = 0; PinIndex < Created->Pins.Num(); ++PinIndex)
				{
					UEdGraphPin* Pin = Created->Pins[PinIndex];
					EHyperAIBlueprintPinDirection Direction = EHyperAIBlueprintPinDirection::Input;
					if (!Pin)
					{
						bFailed = true; Failure = TEXT("created_pin_template_mismatch"); break;
					}
					// New allowlisted pins deliberately use the revision-bound fallback identity so
					// subsequent operations in this same patch can address the advertised template.
					Pin->PersistentGuid.Invalidate();
					if (!TryConvertPinDirection(Pin->Direction, Direction)
						|| Pin->PinName != Expected.Pins[PinIndex].Id.PinName
						|| Direction != Expected.Pins[PinIndex].Id.Direction)
					{
						bFailed = true; Failure = TEXT("created_pin_template_mismatch"); break;
					}
					Index.Pins.Add(Expected.Pins[PinIndex].Id.StableKey(), Pin);
				}
				if (bFailed)
				{
					break;
				}
				++AppliedEffects;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintCompileOnly>())
			{
				// The actual compile happens exactly once after the transaction's typed operation phase.
				++AppliedEffects;
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintMoveNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintMoveNode>();
				UEdGraphNode* Node = Index.Nodes.FindRef(Value.Node.StableKey());
				if (Node && (Node->NodePosX != Value.X || Node->NodePosY != Value.Y))
				{
					Node->Modify(); Node->NodePosX = Value.X; Node->NodePosY = Value.Y; ++AppliedEffects;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintConnectPins>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintConnectPins>();
				UEdGraphPin* A = Index.Pins.FindRef(Value.A.StableKey());
				UEdGraphPin* B = Index.Pins.FindRef(Value.B.StableKey());
				if (A && B && !A->LinkedTo.Contains(B))
				{
					A->GetOwningNode()->Modify(); B->GetOwningNode()->Modify();
					Result.bMutationStarted = true;
					if (!A->GetSchema()->TryCreateConnection(A, B) || !A->LinkedTo.Contains(B))
					{
						bFailed = true; Failure = TEXT("connection_apply_failed"); break;
					}
					++AppliedEffects;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintBreakPinLink>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintBreakPinLink>();
				UEdGraphPin* A = Index.Pins.FindRef(Value.A.StableKey());
				UEdGraphPin* B = Index.Pins.FindRef(Value.B.StableKey());
				if (A && B && A->LinkedTo.Contains(B))
				{
					A->GetOwningNode()->Modify(); B->GetOwningNode()->Modify(); Result.bMutationStarted = true;
					A->GetSchema()->BreakSinglePinLink(A, B);
					if (A->LinkedTo.Contains(B)) { bFailed = true; Failure = TEXT("break_link_apply_failed"); break; }
					++AppliedEffects;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintSetLiteralDefault>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintSetLiteralDefault>();
				UEdGraphPin* Pin = Index.Pins.FindRef(Value.Pin.StableKey());
				if (Pin && Pin->GetDefaultAsString() != Value.Value)
				{
					Pin->GetOwningNode()->Modify(); Result.bMutationStarted = true;
					Pin->GetSchema()->TrySetDefaultValue(*Pin, Value.Value, true);
					if (!Pin->GetSchema()->DoesDefaultValueMatch(*Pin, Value.Value))
					{
						bFailed = true; Failure = TEXT("literal_default_apply_failed"); break;
					}
					++AppliedEffects;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintDeleteNode>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintDeleteNode>();
				UEdGraphNode* Node = Index.Nodes.FindRef(Value.Node.StableKey());
				if (Node)
				{
					UEdGraph* Graph = Node->GetGraph(); Graph->Modify(); Node->Modify(); Result.bMutationStarted = true;
					if (!Graph->GetSchema()->SafeDeleteNodeFromGraph(Graph, Node))
					{
						bFailed = true; Failure = TEXT("delete_node_apply_failed"); break;
					}
					++AppliedEffects;
				}
			}
			else if (Operation.Value.IsType<FHyperAIBlueprintLayoutNodes>())
			{
				const auto& Value = Operation.Value.Get<FHyperAIBlueprintLayoutNodes>();
				FString LayoutError;
				const TMap<FString, FIntPoint> Positions = ComputeDeterministicLayout(Value, LayoutError);
				for (const TPair<FString, FIntPoint>& Pair : Positions)
				{
					UEdGraphNode* Node = Index.Nodes.FindRef(Pair.Key);
					if (Node && (Node->NodePosX != Pair.Value.X || Node->NodePosY != Pair.Value.Y))
					{
						Node->Modify(); Node->NodePosX = Pair.Value.X; Node->NodePosY = Pair.Value.Y; ++AppliedEffects;
					}
				}
			}
			Result.bMutationStarted |= AppliedEffects > 0;
		}

		if (!bFailed && AppliedEffects > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			FCompilerResultsLog CompilerLog;
			CompilerLog.bSilentMode = true;
			Result.bCompileAttempted = true;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
			Result.CompileErrors = CompilerLog.NumErrors;
			Result.CompileWarnings = CompilerLog.NumWarnings;
			if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
			{
				bFailed = true;
				Failure = TEXT("compile_failed");
			}
		}

		if (!bFailed)
		{
			const FHyperAIBlueprintPatchSnapshot After = CaptureLoaded(Blueprint);
			if (!After.bComplete || !ValidatePostconditions(After, Patch)
				|| (After.CompileStatus != TEXT("up_to_date") && After.CompileStatus != TEXT("up_to_date_with_warnings")))
			{
				bFailed = true;
				Failure = TEXT("pre_save_validation_failed");
			}
			else
			{
				Result.RevisionAfter = After.Revision;
				Result.bPreSaveValidated = true;
			}
		}

		if (AppliedEffects == 0)
		{
			Transaction.Cancel();
		}
	}

	if (!bFailed)
	{
		Result.bOk = true;
		Result.Status = TEXT("applied_compiled_pre_save_validated");
		Result.Diagnostic = TEXT("Patch applied in one transaction and compiled once. Caller must finalize save.");
		Result.bCallerSaveRequired = true;
		Result.bSavePerformed = false;
		return Result;
	}

	Result.bOk = false;
	Result.Diagnostic = Failure;
	if (!Result.bMutationStarted)
	{
		Result.Status = TEXT("failed_without_effect");
		Result.RollbackState = TEXT("not_needed");
		return Result;
	}

	const FTransactionContext UndoContext = GEditor->Trans->GetUndoContext(false);
	const bool bExactOwnedTop = !GEditor->Trans->IsActive()
		&& UndoContext.IsValid()
		&& UndoContext.Context == Result.TransactionContext
		&& UndoContext.Title.ToString() == Result.TransactionTitle
		&& UndoContext.PrimaryObject == Blueprint;
	if (!bExactOwnedTop)
	{
		Result.Status = TEXT("outcome_unknown");
		Result.RollbackState = TEXT("owned_transaction_not_proven");
		return Result;
	}
	Result.bOwnedUndoVerified = true;
	if (!GEditor->Trans->Undo(false))
	{
		Result.Status = TEXT("outcome_unknown");
		Result.RollbackState = TEXT("owned_undo_failed");
		return Result;
	}
	const FHyperAIBlueprintPatchSnapshot Restored = CaptureLoaded(Blueprint);
	Result.bAuthoringSnapshotRestored = Restored.bComplete && Restored.Revision == Before.Revision;
	Result.bGeneratedClassStateVerified = Result.bAuthoringSnapshotRestored && !Result.bCompileAttempted;
	if (!Result.bAuthoringSnapshotRestored)
	{
		Result.Status = TEXT("outcome_unknown");
		Result.RollbackState = TEXT("undo_revision_mismatch");
		return Result;
	}
	if (Result.bCompileAttempted)
	{
		Result.Status = TEXT("outcome_unknown");
		Result.RollbackState = TEXT("authoring_snapshot_restored_generated_state_unverified");
		return Result;
	}
	Result.Status = TEXT("rolled_back");
	Result.RollbackState = TEXT("verified_exact_owned_transaction_and_revision");
	return Result;
}
