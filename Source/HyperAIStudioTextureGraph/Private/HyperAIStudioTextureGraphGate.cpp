// Games by Hyper 2026.

#include "HyperAIStudioTextureGraphGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/TG_AsyncExportTask.h"
#include "Algo/AllOf.h"
#include "Editor.h"
#include "Export/TextureExporter.h"
#include "Engine/Texture.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "Expressions/TG_Expression.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "Misc/PackageName.h"
#include "Model/Mix/MixSettings.h"
#include "Runtime/Launch/Resources/Version.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_OutputSettings.h"
#include "TG_Pin.h"
#include "TextureGraph.h"
#include "TextureGraphEngine.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"The Texture Graph gate is verified against UE 5.8 only; re-audit its engine calls before building another version.");

namespace HyperAIStudio::TextureGraph::Gate
{
namespace
{
	constexpr int32 LayoutColumnWidth = 320;
	constexpr int32 LayoutRowHeight = 200;
	constexpr int32 MaxKeyCharacters = 64;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < 0x20 || Character == 0x7F)
			{
				return true;
			}
		}
		return false;
	}

	FString PinDirection(const UTG_Pin& Pin)
	{
		if (Pin.IsPrivate()) return TEXT("private");
		if (Pin.IsOutput()) return TEXT("output");
		return Pin.IsSetting() ? TEXT("setting") : TEXT("input");
	}

	UEnum* PinEnum(const UTG_Pin& Pin)
	{
		const FProperty* Property = Pin.GetExpressionProperty();
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		return ByteProperty ? ByteProperty->Enum : nullptr;
	}

	bool IsOutputSettingsPin(const UTG_Pin& Pin)
	{
		return Pin.GetArgumentCPPTypeName() == TEXT("FTG_OutputSettings");
	}

	/** Outputs are rewritten by evaluation and texture values are render handles, so neither is authored state. */
	bool HasAuthoredValue(const UTG_Pin& Pin)
	{
		return Pin.IsValidSelfVar() && !Pin.IsOutput() && !Pin.IsConnected() && !Pin.IsArgTexture()
			&& !Pin.IsArgArray() && !IsOutputSettingsPin(Pin);
	}

	TArray<int32> NodeIds(const UTG_Graph& Graph)
	{
		TArray<int32> Ids;
		Graph.ForEachNodes([&Ids](const UTG_Node* Node, uint32)
		{
			if (Node)
			{
				Ids.Add(Node->GetId().NodeIdx());
			}
		});
		return Ids;
	}

	FTG_OutputSettings SettingsOf(const UTG_Node& Node, const UTG_Expression_Output& Output)
	{
		const UTG_Pin* Pin = Node.GetPin(GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, OutputSettings));
		// The pin's value is authoritative; the expression property only catches up on evaluation.
		if (Pin && Pin->IsValidSelfVar() && IsOutputSettingsPin(*Pin))
		{
			return Pin->GetSelfVar()->GetAs<FTG_OutputSettings>();
		}
		return Output.OutputSettings;
	}

	void WriteSettings(UTG_Node& Node, UTG_Expression_Output& Output, const FTG_OutputSettings& Settings)
	{
		// Mirrors UTG_Expression_Output::InitializeOutputSettings: property first, then the pin through FromString.
		Output.Modify();
		Output.OutputSettings = Settings;
		if (UTG_Pin* Pin = Node.GetPin(GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, OutputSettings)))
		{
			Pin->FromString(Settings.ToString());
		}
	}

	FString TextureObjectPath(const FTG_OutputSettings& Settings)
	{
		if (Settings.BaseName.IsNone() || Settings.FolderPath.IsNone())
		{
			return FString();
		}
		const FString Name = Settings.BaseName.ToString();
		const FString PackageName = Settings.FolderPath.ToString() / Name;
		return FPackageName::IsValidLongPackageName(PackageName) ? PackageName + TEXT(".") + Name : FString();
	}

	FString TextureStateOf(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return TEXT("missing");
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const bool bOnDisk = FPackageName::DoesPackageExist(PackageName);
		if (const UObject* Loaded = FSoftObjectPath(ObjectPath).ResolveObject())
		{
			if (!Loaded->IsA<UTexture>())
			{
				return TEXT("conflict");
			}
			return Loaded->GetOutermost()->IsDirty() || !bOnDisk ? TEXT("dirty") : TEXT("saved");
		}
		if (!bOnDisk)
		{
			return TEXT("missing");
		}
		const FAssetData Asset = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		return Asset.IsValid() && !Asset.IsInstanceOf(UTexture::StaticClass()) ? TEXT("conflict") : TEXT("saved");
	}

	TMap<FString, UClass*> BuildExpressionCatalog()
	{
		// The same filter Texture Graph's own node palette uses (TG_EdGraphSchema.cpp).
		TMap<FString, UClass*> Catalog;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UTG_Expression::StaticClass())
				&& !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_NewerVersionExists))
			{
				Catalog.Add(It->GetName(), *It);
			}
		}
		return Catalog;
	}

	bool ParseFloat(const FString& Text, float& Out)
	{
		return !Text.IsEmpty() && LexTryParseString(Out, *Text) && FMath::IsFinite(Out);
	}

	bool ParseColor(const FString& Text, FLinearColor& Out)
	{
		if (Text.StartsWith(TEXT("(")))
		{
			return Out.InitFromString(Text);
		}
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 3 || Parts.Num() > 4)
		{
			return false;
		}
		float Values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			if (!ParseFloat(Parts[Index].TrimStartAndEnd(), Values[Index]))
			{
				return false;
			}
		}
		Out = FLinearColor(Values[0], Values[1], Values[2], Values[3]);
		return true;
	}

	bool WithinClamp(const UTG_Pin& Pin, const float Value, FString& OutError)
	{
#if WITH_EDITORONLY_DATA
		const FProperty* Property = Pin.GetExpressionProperty();
		float Limit = 0.0f;
		if (Property && Property->HasMetaData(TEXT("ClampMin"))
			&& LexTryParseString(Limit, *Property->GetMetaData(TEXT("ClampMin"))) && Value < Limit)
		{
			OutError = FString::Printf(TEXT("%g is below this pin's minimum %g."), Value, Limit);
			return false;
		}
		if (Property && Property->HasMetaData(TEXT("ClampMax"))
			&& LexTryParseString(Limit, *Property->GetMetaData(TEXT("ClampMax"))) && Value > Limit)
		{
			OutError = FString::Printf(TEXT("%g is above this pin's maximum %g."), Value, Limit);
			return false;
		}
#endif
		return true;
	}

	bool SetPinValue(UTG_Pin& Pin, const FString& Text, FString& OutError)
	{
		if (Pin.IsOutput() || Pin.IsPrivate())
		{
			OutError = TEXT("Output and private pins have no settable value.");
			return false;
		}
		if (Pin.IsConnected())
		{
			OutError = TEXT("The pin is connected, so its value is ignored; disconnect it first.");
			return false;
		}
		if (Pin.IsArgArray() || Pin.IsArgTexture() || IsOutputSettingsPin(Pin))
		{
			OutError = TEXT("Texture and array pins take a connection, and output settings take set_output.");
			return false;
		}
		auto Applied = [&](const bool bOk)
		{
			if (!bOk) OutError = FString::Printf(TEXT("'%s' does not fit pin type %s."), *Text, *Pin.GetArgumentCPPTypeName().ToString());
			return bOk;
		};
		const FProperty* Property = Pin.GetExpressionProperty();
		if (const UEnum* Enum = PinEnum(Pin))
		{
			if (Enum->GetIndexByNameString(Text) == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("'%s' is not a value of %s; inspect lists enum_values."), *Text, *Enum->GetName());
				return false;
			}
			Pin.FromString(Text);
			return true;
		}
		if (Pin.IsArgBool())
		{
			const bool bTrue = Text.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			return Applied((bTrue || Text.Equals(TEXT("false"), ESearchCase::IgnoreCase)) && Pin.SetValue(bTrue));
		}
		if (Pin.IsArgString())
		{
			return Applied(Pin.SetValue(Text));
		}
		float Scalar = 0.0f;
		if (Pin.IsArgScalar() && ParseFloat(Text, Scalar))
		{
			const FName CppType = Pin.GetArgumentCPPTypeName();
			if ((CppType == TEXT("int32") || CppType == TEXT("uint32")) && FMath::Frac(Scalar) != 0.0f)
			{
				OutError = TEXT("This pin takes a whole number.");
				return false;
			}
			return WithinClamp(Pin, Scalar, OutError) && Applied(Pin.SetValue(Scalar));
		}
		FLinearColor Color;
		if ((Pin.IsArgColor() || Pin.IsArgVector()) && ParseColor(Text, Color))
		{
			return Applied(Pin.SetValue(Color));
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			FText Reason;
			const UObject* Asset = FPackageName::IsValidObjectPath(Text, &Reason) ? FSoftObjectPath(Text).TryLoad() : nullptr;
			if (!Asset || !ObjectProperty->PropertyClass || !Asset->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(TEXT("'%s' is not an existing %s asset path."), *Text,
					ObjectProperty->PropertyClass ? *ObjectProperty->PropertyClass->GetName() : TEXT("object"));
				return false;
			}
			Pin.FromString(Text);
			return true;
		}
		if (CastField<FNameProperty>(Property))
		{
			Pin.FromString(Text);
			return true;
		}
		return Applied(false);
	}

	bool IsResolution(const int32 Value)
	{
		return Value == 0 || (Value >= 8 && Value <= 8192 && FMath::IsPowerOfTwo(Value));
	}

	bool SetOutput(UTG_Node& Node, const FHyperAITextureGraphEditOp& Op, FString& OutError)
	{
		UTG_Expression_Output* Expression = Cast<UTG_Expression_Output>(Node.GetExpression());
		if (!Expression)
		{
			OutError = TEXT("set_output needs an Output node.");
			return false;
		}
		FTG_OutputSettings Settings = SettingsOf(Node, *Expression);
		if (!Op.BaseName.IsEmpty())
		{
			if (Op.BaseName.Len() > MaxKeyCharacters * 2 || Op.BaseName.Contains(TEXT("/"))
				|| !FName::IsValidXName(Op.BaseName, INVALID_OBJECTNAME_CHARACTERS))
			{
				OutError = TEXT("base_name must be a plain asset name.");
				return false;
			}
			Settings.BaseName = FName(*Op.BaseName);
		}
		if (!Op.FolderPath.IsEmpty())
		{
			const FString Folder = Op.FolderPath.EndsWith(TEXT("/")) ? Op.FolderPath.LeftChop(1) : Op.FolderPath;
			if (!(Folder == TEXT("/Game") || Folder.StartsWith(TEXT("/Game/")))
				|| !FPackageName::IsValidLongPackageName(Folder / TEXT("T")))
			{
				OutError = TEXT("folder_path must be a /Game folder.");
				return false;
			}
			Settings.FolderPath = FName(*Folder);
		}
		if (!IsResolution(Op.Width) || !IsResolution(Op.Height))
		{
			OutError = TEXT("width and height must be 0 (Auto) or a power of two from 8 to 8192.");
			return false;
		}
		Settings.Width = static_cast<EResolution>(Op.Width);
		Settings.Height = static_cast<EResolution>(Op.Height);
		if (!Op.TextureFormat.IsEmpty())
		{
			const int64 Format = StaticEnum<ETG_TextureFormat>()->GetValueByNameString(Op.TextureFormat);
			if (Format == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("'%s' is not an ETG_TextureFormat."), *Op.TextureFormat);
				return false;
			}
			Settings.TextureFormat = static_cast<ETG_TextureFormat>(Format);
		}
		Settings.bSRGB = Op.bSRGB;
		Settings.bShouldExport = Op.bShouldExport;
		FString PathErrors;
		if (!Settings.Validate(PathErrors))
		{
			OutError = PathErrors.IsEmpty() ? FString(TEXT("The output texture path is invalid.")) : PathErrors;
			return false;
		}
		WriteSettings(Node, *Expression, Settings);
		return true;
	}

	UTG_Node* FindNode(
		UTG_Graph& Graph,
		const FString& Ref,
		const TMap<FString, int32>& Keys,
		const TSet<int32>& Removed,
		FString& OutError)
	{
		if (const int32* KeyId = Keys.Find(Ref))
		{
			if (UTG_Node* Node = Graph.GetNode(FTG_Id(*KeyId)))
			{
				return Node;
			}
		}
		else if (!Ref.IsEmpty() && Ref.Len() <= 5 && Ref.IsNumeric() && !Ref.Contains(TEXT(".")) && !Ref.StartsWith(TEXT("-")))
		{
			const int32 Id = FCString::Atoi(*Ref);
			// A removed id can be reused by a later add_node; addressing it by number would hit the wrong node.
			if (Id < MAX_int16 && !Removed.Contains(Id))
			{
				if (UTG_Node* Node = Graph.GetNode(FTG_Id(Id)))
				{
					return Node;
				}
			}
		}
		OutError = FString::Printf(TEXT("No node '%s'. Use a node_id from inspect or an earlier add_node key."), *Ref);
		return nullptr;
	}

	UTG_Pin* FindPin(const UTG_Node& Node, const FString& Name, FString& OutError)
	{
		for (const TObjectPtr<UTG_Pin>& Pin : Node.Pins)
		{
			if (Pin && Pin->GetArgumentName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin.Get();
			}
		}
		for (const TObjectPtr<UTG_Pin>& Pin : Node.Pins)
		{
			if (Pin && Pin->GetAliasName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin.Get();
			}
		}
		OutError = FString::Printf(TEXT("Node %d has no pin '%s'."), Node.GetId().NodeIdx(), *Name);
		return nullptr;
	}

	void LayoutAddedNodes(UTG_Graph& Graph, const TSet<int32>& Added)
	{
#if WITH_EDITORONLY_DATA
		if (Added.IsEmpty())
		{
			return;
		}
		TMap<int32, TArray<int32>> Successors;
		Graph.ForEachEdges([&Successors](const UTG_Pin* From, const UTG_Pin* To)
		{
			if (From && To)
			{
				Successors.FindOrAdd(From->GetNodeId().NodeIdx()).AddUnique(To->GetNodeId().NodeIdx());
			}
		});
		// Longest path to a sink. Connect refuses loops, and the guard bounds a corrupt graph anyway.
		TMap<int32, int32> Depths;
		TFunction<int32(int32, int32)> DepthOf = [&](const int32 Id, const int32 Guard) -> int32
		{
			if (const int32* Known = Depths.Find(Id))
			{
				return *Known;
			}
			int32 Depth = 0;
			if (const TArray<int32>* Next = Successors.Find(Id); Next && Guard < 256)
			{
				for (const int32 Successor : *Next)
				{
					Depth = FMath::Max(Depth, DepthOf(Successor, Guard + 1) + 1);
				}
			}
			Depths.Add(Id, Depth);
			return Depth;
		};
		int32 ExistingCount = 0;
		int32 AnchorX = 0;
		int32 MinY = 0;
		int32 MaxY = 0;
		Graph.ForEachNodes([&](const UTG_Node* Node, uint32)
		{
			if (!Node || Added.Contains(Node->GetId().NodeIdx()))
			{
				return;
			}
			AnchorX = ExistingCount == 0 ? Node->EditorData.PosX : FMath::Max(AnchorX, Node->EditorData.PosX);
			MinY = ExistingCount == 0 ? Node->EditorData.PosY : FMath::Min(MinY, Node->EditorData.PosY);
			MaxY = ExistingCount == 0 ? Node->EditorData.PosY : FMath::Max(MaxY, Node->EditorData.PosY);
			++ExistingCount;
		});
		// Beside a lone output the new chain lines up with it; in a populated graph it goes below what is there.
		const int32 BaseY = ExistingCount > 1 ? MaxY + LayoutRowHeight : MinY;
		TArray<int32> Ordered = Added.Array();
		Ordered.Sort();
		TMap<int32, int32> RowsPerDepth;
		for (const int32 Id : Ordered)
		{
			if (UTG_Node* Node = Graph.GetNode(FTG_Id(Id)))
			{
				const int32 Depth = DepthOf(Id, 0);
				int32& Row = RowsPerDepth.FindOrAdd(Depth);
				Node->Modify();
				Node->EditorData.PosX = AnchorX - LayoutColumnWidth * Depth;
				Node->EditorData.PosY = BaseY + LayoutRowHeight * Row++;
			}
		}
#endif
	}

	bool ApplyOp(
		UTG_Graph& Graph,
		const FHyperAITextureGraphEditOp& Op,
		const TMap<FString, UClass*>& Catalog,
		const bool bAutoLayout,
		TMap<FString, int32>& Keys,
		TSet<int32>& Removed,
		TSet<int32>& Added,
		FString& OutError)
	{
		if (HasControlCharacter(Op.Value) || Op.Value.Len() > FHyperAIStudioTextureGraphContracts::MaxTextCharacters)
		{
			OutError = TEXT("value holds control characters or is too long.");
			return false;
		}
		if (Op.Kind == TEXT("add_node"))
		{
			const bool bKeyValid = !Op.NodeKey.IsEmpty() && Op.NodeKey.Len() <= MaxKeyCharacters
				&& FChar::IsAlpha(Op.NodeKey[0])
				&& Algo::AllOf(Op.NodeKey, [](const TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); });
			if (!bKeyValid || Keys.Contains(Op.NodeKey))
			{
				OutError = TEXT("node_key must be a new identifier: a letter, then letters, digits or _.");
				return false;
			}
			UClass* const* Class = Catalog.Find(Op.ExpressionClass);
			if (!Class)
			{
				Class = Catalog.Find(TEXT("TG_Expression_") + Op.ExpressionClass);
			}
			if (!Class)
			{
				OutError = FString::Printf(TEXT("'%s' is not a Texture Graph node class; inspect with bIncludeExpressionCatalog."), *Op.ExpressionClass);
				return false;
			}
			if (NodeIds(Graph).Num() >= FHyperAIStudioTextureGraphContracts::MaxNodes)
			{
				OutError = TEXT("The graph is at its node bound.");
				return false;
			}
			UTG_Node* Node = Graph.CreateExpressionNode(*Class);
			if (!Node)
			{
				OutError = TEXT("Texture Graph did not create the node.");
				return false;
			}
			const int32 Id = Node->GetId().NodeIdx();
			Keys.Add(Op.NodeKey, Id);
			Added.Add(Id);
#if WITH_EDITORONLY_DATA
			if (!bAutoLayout)
			{
				Node->EditorData.PosX = Op.PosX;
				Node->EditorData.PosY = Op.PosY;
			}
#endif
			return true;
		}

		UTG_Node* Node = FindNode(Graph, Op.Node, Keys, Removed, OutError);
		if (!Node)
		{
			return false;
		}
		const int32 NodeId = Node->GetId().NodeIdx();
		if (Op.Kind == TEXT("remove_node"))
		{
			Graph.RemoveNode(Node);
			Removed.Add(NodeId);
			Added.Remove(NodeId);
			for (auto It = Keys.CreateIterator(); It; ++It)
			{
				if (It.Value() == NodeId)
				{
					It.RemoveCurrent();
				}
			}
			return true;
		}
		if (Op.Kind == TEXT("connect") || Op.Kind == TEXT("disconnect"))
		{
			const bool bConnect = Op.Kind == TEXT("connect");
			UTG_Pin* FromPin = FindPin(*Node, Op.Pin, OutError);
			if (!FromPin)
			{
				return false;
			}
			if (!bConnect && Op.ToNode.IsEmpty())
			{
				if (!FromPin->IsConnected())
				{
					OutError = TEXT("The pin has no connections.");
					return false;
				}
				Graph.RemovePinEdges(*Node, FromPin->GetArgumentName());
				return true;
			}
			UTG_Node* ToNode = FindNode(Graph, Op.ToNode, Keys, Removed, OutError);
			UTG_Pin* ToPin = ToNode ? FindPin(*ToNode, Op.ToPin, OutError) : nullptr;
			if (!ToPin)
			{
				return false;
			}
			if (!FromPin->IsOutput() || ToPin->IsOutput() || ToPin->IsPrivate())
			{
				OutError = TEXT("Connections run from an output pin to an input or setting pin.");
				return false;
			}
			if (!bConnect)
			{
				if (!ToPin->GetEdges().Contains(FromPin->GetId()))
				{
					OutError = TEXT("Those pins are not connected.");
					return false;
				}
				Graph.RemoveEdge(*Node, FromPin->GetArgumentName(), *ToNode, ToPin->GetArgumentName());
				return true;
			}
			FName ConverterKey;
			if (ToPin->IsNotConnectable())
			{
				OutError = TEXT("That input takes a value, not a connection.");
				return false;
			}
			if (UTG_Graph::ConnectionCausesLoop(FromPin, ToPin))
			{
				OutError = TEXT("The connection would create a loop.");
				return false;
			}
			if (!UTG_Graph::ArePinsCompatible(FromPin, ToPin, ConverterKey))
			{
				OutError = FString::Printf(TEXT("%s output cannot feed a %s input."),
					*FromPin->GetArgumentCPPTypeName().ToString(), *ToPin->GetArgumentCPPTypeName().ToString());
				return false;
			}
			if (!Graph.Connect(*Node, FromPin->GetArgumentName(), *ToNode, ToPin->GetArgumentName()))
			{
				OutError = TEXT("Texture Graph refused the connection.");
				return false;
			}
			return true;
		}
		if (Op.Kind == TEXT("set_pin_value") || Op.Kind == TEXT("set_pin_alias"))
		{
			UTG_Pin* Pin = FindPin(*Node, Op.Pin, OutError);
			if (!Pin)
			{
				return false;
			}
			if (Op.Kind == TEXT("set_pin_value"))
			{
				return SetPinValue(*Pin, Op.Value, OutError);
			}
			if (Op.Value.IsEmpty() || Op.Value.Len() > MaxKeyCharacters * 2
				|| !FName::IsValidXName(Op.Value, INVALID_NAME_CHARACTERS))
			{
				OutError = TEXT("An alias must be a short valid name.");
				return false;
			}
			UTG_Expression_Output* Output = Cast<UTG_Expression_Output>(Node->GetExpression());
			if (Output && Pin->IsOutput())
			{
				// Renaming an output also renames its export entry, exactly as the editor's title edit does.
				Output->SetTitleName(FName(*Op.Value));
			}
			else
			{
				Pin->SetAliasName(FName(*Op.Value));
			}
			return true;
		}
		if (Op.Kind == TEXT("set_output"))
		{
			return SetOutput(*Node, Op, OutError);
		}
		if (Op.Kind == TEXT("set_node_comment") || Op.Kind == TEXT("move_node"))
		{
#if WITH_EDITORONLY_DATA
			Node->Modify();
			if (Op.Kind == TEXT("move_node"))
			{
				Node->EditorData.PosX = Op.PosX;
				Node->EditorData.PosY = Op.PosY;
				Added.Remove(NodeId);
			}
			else
			{
				Node->EditorData.NodeComment = Op.Value;
				Node->EditorData.bCommentBubbleVisible = !Op.Value.IsEmpty();
				Node->EditorData.bCommentBubblePinned = !Op.Value.IsEmpty();
			}
#endif
			return true;
		}
		OutError = TEXT("Unknown kind. Use add_node, remove_node, connect, disconnect, set_pin_value, set_pin_alias, set_output, set_node_comment or move_node.");
		return false;
	}

	void ApplyCreateDefaults(UTextureGraph& Graph, const FString& TargetPath)
	{
		UTG_Graph* TGGraph = Graph.Graph();
		const FString PackageName = FPackageName::ObjectPathToPackageName(TargetPath);
		const FString AssetName = FPackageName::ObjectPathToObjectName(TargetPath);
		for (const int32 Id : NodeIds(*TGGraph))
		{
			UTG_Node* Node = TGGraph->GetNode(FTG_Id(Id));
			UTG_Expression_Output* Output = Node ? Cast<UTG_Expression_Output>(Node->GetExpression()) : nullptr;
			if (!Output)
			{
				continue;
			}
			// Epic names every first output texture "Output"; per-graph names keep two graphs in one folder apart.
			FTG_OutputSettings Settings = SettingsOf(*Node, *Output);
			Settings.BaseName = FName(*(TEXT("T_") + AssetName));
			Settings.FolderPath = FName(*FPackageName::GetLongPackagePath(PackageName));
			WriteSettings(*Node, *Output, Settings);
		}
	}
}

UTextureGraph* ResolveGraph(const FString& TargetPath, const bool bLoad)
{
	const FSoftObjectPath Path(TargetPath);
	UObject* Object = Path.ResolveObject();
	if (!Object && bLoad && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(TargetPath)))
	{
		Object = Path.TryLoad();
	}
	UTextureGraph* Graph = Cast<UTextureGraph>(Object);
	return Graph && Graph->GetPathName() == TargetPath && Graph->Graph() ? Graph : nullptr;
}

bool IsOpenInEditor(const UTextureGraph& Graph)
{
	// Texture Graph's editor edits a _Runtime copy and writes it back over the asset on save.
	UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	return Editors && Editors->FindEditorForAsset(const_cast<UTextureGraph*>(&Graph), /*bFocusIfOpen=*/false) != nullptr;
}

bool IsEngineAvailable()
{
	return TextureGraphEngine::GetInstance() != nullptr && !TextureGraphEngine::IsDestroying();
}

int32 CountExportsInFlight()
{
	// UTG_AsyncExportTask holds RF_Standalone from creation until OnExportDone clears it.
	int32 Count = 0;
	for (TObjectIterator<UTG_AsyncExportTask> It; It; ++It)
	{
		if (IsValid(*It) && It->HasAnyFlags(RF_Standalone) && !It->HasAnyFlags(RF_ClassDefaultObject))
		{
			++Count;
		}
	}
	return Count;
}

TArray<FString> GetExpressionCatalog()
{
	TArray<FString> Names;
	BuildExpressionCatalog().GenerateKeyArray(Names);
	Names.Sort();
	return Names;
}

FString ComputeContentKey(const UTextureGraph& Graph)
{
	const UPackage* Package = Graph.GetOutermost();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.texture_graph.content-key.v1"));
	AppendToken(Canonical, Graph.GetPathName());
	AppendToken(Canonical, Package ? LexToString(Package->GetSavedHash()) : FString());
	AppendToken(Canonical, Package && Package->IsDirty() ? TEXT("dirty") : TEXT("clean"));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString ComputeRevision(const UTextureGraph& Graph)
{
	TArray<FHyperAITextureGraphNode> Nodes;
	TArray<FHyperAITextureGraphEdge> Edges;
	ReadGraph(Graph, /*bIncludePins=*/true, Nodes, Edges);
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.texture_graph.revision.v1"));
	AppendToken(Canonical, ComputeContentKey(Graph));
	AppendToken(Canonical, FString::FromInt(Nodes.Num()));
	for (const FHyperAITextureGraphNode& Node : Nodes)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%d|%s|%s|%d|%d"), Node.NodeId, *Node.ExpressionClass, *Node.Title, Node.PosX, Node.PosY));
		AppendToken(Canonical, Node.Comment);
		for (const FHyperAITextureGraphPin& Pin : Node.Pins)
		{
			AppendToken(Canonical, Pin.Name + TEXT("|") + Pin.Alias + (Pin.bConnected ? TEXT("|c") : TEXT("|u")));
			AppendToken(Canonical, Pin.Value);
		}
	}
	for (const FHyperAITextureGraphEdge& Edge : Edges)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%d.%s>%d.%s"), Edge.FromNode, *Edge.FromPin, Edge.ToNode, *Edge.ToPin));
	}
	for (const FHyperAITextureGraphOutput& Output : ReadOutputs(Graph))
	{
		AppendToken(Canonical, FString::Printf(TEXT("%d|%s|%s|%s|%d|%d|%s|%d|%d"), Output.NodeId, *Output.OutputName,
			*Output.BaseName, *Output.FolderPath, Output.Width, Output.Height, *Output.TextureFormat,
			Output.bSRGB ? 1 : 0, Output.bShouldExport ? 1 : 0));
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

void ReadGraph(
	const UTextureGraph& Graph,
	const bool bIncludePins,
	TArray<FHyperAITextureGraphNode>& OutNodes,
	TArray<FHyperAITextureGraphEdge>& OutEdges)
{
	OutNodes.Reset();
	OutEdges.Reset();
	const UTG_Graph* TGGraph = Graph.Graph();
	if (!TGGraph)
	{
		return;
	}
	TGGraph->ForEachNodes([&](const UTG_Node* Node, uint32)
	{
		if (!Node)
		{
			return;
		}
		FHyperAITextureGraphNode& Out = OutNodes.AddDefaulted_GetRef();
		Out.NodeId = Node->GetId().NodeIdx();
		const UTG_Expression* Expression = Node->GetExpression();
		Out.ExpressionClass = Expression ? Expression->GetClass()->GetName() : FString();
		Out.Title = Node->GetNodeName().ToString();
#if WITH_EDITORONLY_DATA
		Out.PosX = Node->EditorData.PosX;
		Out.PosY = Node->EditorData.PosY;
		Out.Comment = Node->EditorData.NodeComment;
#endif
		if (!bIncludePins)
		{
			return;
		}
		for (const TObjectPtr<UTG_Pin>& PinPtr : Node->Pins)
		{
			const UTG_Pin* Pin = PinPtr.Get();
			if (!Pin || !Pin->IsValid())
			{
				continue;
			}
			FHyperAITextureGraphPin& OutPin = Out.Pins.AddDefaulted_GetRef();
			OutPin.Name = Pin->GetArgumentName().ToString();
			OutPin.Alias = Pin->GetAliasName().ToString();
			OutPin.Direction = PinDirection(*Pin);
			OutPin.CppType = Pin->GetArgumentCPPTypeName().ToString();
			OutPin.bConnected = Pin->IsConnected();
			OutPin.bParam = Pin->IsParam();
			if (HasAuthoredValue(*Pin))
			{
				OutPin.Value = Pin->GetEvaluatedVarValue();
			}
			if (const UEnum* Enum = PinEnum(*Pin))
			{
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
#if WITH_EDITORONLY_DATA
					if (Enum->HasMetaData(TEXT("Hidden"), Index))
					{
						continue;
					}
#endif
					OutPin.EnumValues.Add(Enum->GetNameStringByIndex(Index));
				}
			}
		}
	});
	TGGraph->ForEachEdges([&OutEdges](const UTG_Pin* From, const UTG_Pin* To)
	{
		if (From && To)
		{
			FHyperAITextureGraphEdge& Edge = OutEdges.AddDefaulted_GetRef();
			Edge.FromNode = From->GetNodeId().NodeIdx();
			Edge.FromPin = From->GetArgumentName().ToString();
			Edge.ToNode = To->GetNodeId().NodeIdx();
			Edge.ToPin = To->GetArgumentName().ToString();
		}
	});
}

TArray<FHyperAITextureGraphOutput> ReadOutputs(const UTextureGraph& Graph)
{
	TArray<FHyperAITextureGraphOutput> Outputs;
	const UTG_Graph* TGGraph = Graph.Graph();
	if (!TGGraph)
	{
		return Outputs;
	}
	const UEnum* FormatEnum = StaticEnum<ETG_TextureFormat>();
	TGGraph->ForEachNodes([&](const UTG_Node* Node, uint32)
	{
		const UTG_Expression_Output* Expression = Node ? Cast<UTG_Expression_Output>(Node->GetExpression()) : nullptr;
		if (!Expression)
		{
			return;
		}
		const FTG_OutputSettings Settings = SettingsOf(*Node, *Expression);
		FHyperAITextureGraphOutput& Out = Outputs.AddDefaulted_GetRef();
		Out.NodeId = Node->GetId().NodeIdx();
		Out.OutputName = Expression->GetTitleName().ToString();
		Out.BaseName = Settings.BaseName.IsNone() ? FString() : Settings.BaseName.ToString();
		Out.FolderPath = Settings.FolderPath.IsNone() ? FString() : Settings.FolderPath.ToString();
		Out.Width = static_cast<int32>(Settings.Width);
		Out.Height = static_cast<int32>(Settings.Height);
		Out.TextureFormat = FormatEnum ? FormatEnum->GetNameStringByValue(static_cast<int64>(Settings.TextureFormat)) : FString();
		Out.bSRGB = Settings.bSRGB;
		Out.bShouldExport = Settings.bShouldExport;
		const UTG_Pin* Source = Node->GetPin(GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, Source));
		Out.bSourceConnected = Source && Source->IsConnected();
		Out.TexturePath = TextureObjectPath(Settings);
		Out.TextureState = TextureStateOf(Out.TexturePath);
	});
	return Outputs;
}

TArray<FHyperAITextureGraphIssue> Validate(const UTextureGraph& Graph, const bool bRequireExported)
{
	TArray<FHyperAITextureGraphIssue> Issues;
	auto Add = [&Issues](const TCHAR* Severity, const TCHAR* Code, const int32 NodeId, const FString& Pin, const FString& Message)
	{
		FHyperAITextureGraphIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Severity = Severity;
		Issue.Code = Code;
		Issue.NodeId = NodeId;
		Issue.Pin = Pin;
		Issue.Message = Message;
	};
	const UTG_Graph* TGGraph = Graph.Graph();
	if (!TGGraph)
	{
		Add(TEXT("error"), TEXT("graph_missing"), INDEX_NONE, FString(), TEXT("The asset has no graph; it was not constructed."));
		return Issues;
	}
	if (Graph.HasCyclicDependency())
	{
		Add(TEXT("error"), TEXT("graph_cyclic_dependency"), INDEX_NONE, FString(), TEXT("A subgraph node references this graph, directly or indirectly."));
	}
	TGGraph->ForEachNodes([&](const UTG_Node* Node, uint32)
	{
		if (Node && !Node->GetExpression())
		{
			Add(TEXT("error"), TEXT("node_missing_expression"), Node->GetId().NodeIdx(), FString(), TEXT("The node lost its expression; remove it."));
		}
	});
	TGGraph->ForEachEdges([&](const UTG_Pin* From, const UTG_Pin* To)
	{
		FName ConverterKey;
		if (From && To && !UTG_Graph::ArePinsCompatible(From, To, ConverterKey))
		{
			Add(TEXT("error"), TEXT("edge_incompatible"), To->GetNodeId().NodeIdx(), To->GetArgumentName().ToString(),
				FString::Printf(TEXT("Node %d %s cannot feed this input."), From->GetNodeId().NodeIdx(), *From->GetArgumentName().ToString()));
		}
	});

	const bool bExportRunning = CountExportsInFlight() > 0;
	TSet<FString> TexturePaths;
	int32 Exporting = 0;
	for (const FHyperAITextureGraphOutput& Output : ReadOutputs(Graph))
	{
		if (!Output.bShouldExport)
		{
			continue;
		}
		++Exporting;
		if (!Output.bSourceConnected)
		{
			Add(TEXT("warning"), TEXT("output_source_unconnected"), Output.NodeId, TEXT("Source"),
				TEXT("Nothing feeds this output, so it exports a flat texture."));
		}
		FString PathErrors;
		if (Output.TexturePath.IsEmpty()
			|| !TextureExporter::IsFilePathValid(FName(*Output.BaseName), FName(*Output.FolderPath), PathErrors))
		{
			Add(TEXT("error"), TEXT("output_path_invalid"), Output.NodeId, FString(),
				PathErrors.IsEmpty() ? FString(TEXT("Set a base_name and a /Game folder_path with set_output.")) : PathErrors);
			continue;
		}
		if (!Output.FolderPath.StartsWith(TEXT("/Game")))
		{
			Add(TEXT("error"), TEXT("output_folder_outside_project"), Output.NodeId, FString(), TEXT("Export into a /Game folder."));
		}
		if (TexturePaths.Contains(Output.TexturePath))
		{
			Add(TEXT("error"), TEXT("duplicate_output_texture"), Output.NodeId, FString(),
				FString::Printf(TEXT("Another output also exports %s."), *Output.TexturePath));
		}
		TexturePaths.Add(Output.TexturePath);
		if (Output.TextureState == TEXT("conflict"))
		{
			Add(TEXT("error"), TEXT("export_target_conflict"), Output.NodeId, FString(),
				FString::Printf(TEXT("%s is an existing non-texture asset; export would not replace it."), *Output.TexturePath));
		}
		else if (bRequireExported && !bExportRunning && Output.TextureState != TEXT("saved"))
		{
			Add(TEXT("error"), TEXT("texture_not_exported"), Output.NodeId, FString(),
				FString::Printf(TEXT("%s is %s; export with apply_plan bExport."), *Output.TexturePath, *Output.TextureState));
		}
	}
	if (Exporting == 0)
	{
		Add(bRequireExported ? TEXT("error") : TEXT("warning"), TEXT("no_exporting_outputs"), INDEX_NONE, FString(),
			TEXT("No output is set to export."));
	}
	if (bRequireExported && bExportRunning)
	{
		Add(TEXT("error"), TEXT("export_in_flight"), INDEX_NONE, FString(), TEXT("An export is still running; validate again shortly."));
	}
	return Issues;
}

bool ApplyOps(
	UTextureGraph& Graph,
	const TArray<FHyperAITextureGraphEditOp>& Ops,
	const bool bAutoLayout,
	const bool bTransact,
	TMap<FString, int32>& OutNodeKeyIds,
	int32& OutApplied,
	FString& OutError)
{
	OutNodeKeyIds.Reset();
	OutApplied = 0;
	OutError.Reset();
	UTG_Graph* TGGraph = Graph.Graph();
	if (!TGGraph)
	{
		OutError = TEXT("The asset has no graph.");
		return false;
	}
	TUniquePtr<FScopedTransaction> Transaction;
	if (bTransact)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("HyperAIStudio", "TextureGraphEditOps", "HyperAI Texture Graph Edit"));
	}
	Graph.Modify();
	TGGraph->Modify();
	const TMap<FString, UClass*> Catalog = BuildExpressionCatalog();
	TSet<int32> Removed;
	TSet<int32> Added;
	for (int32 Index = 0; Index < Ops.Num(); ++Index)
	{
		FString Error;
		if (!ApplyOp(*TGGraph, Ops[Index], Catalog, bAutoLayout, OutNodeKeyIds, Removed, Added, Error))
		{
			OutError = FString::Printf(TEXT("Op %d (%s): %s"), Index, *Ops[Index].Kind, *Error);
			if (Transaction && OutApplied == 0)
			{
				Transaction->Cancel();
			}
			return false;
		}
		++OutApplied;
	}
	if (bAutoLayout)
	{
		LayoutAddedNodes(*TGGraph, Added);
	}
	Graph.MarkPackageDirty();
	return true;
}

bool SimulatePlan(
	const UTextureGraph* Source,
	const FString& TargetPath,
	const TArray<FHyperAITextureGraphEditOp>& Ops,
	const bool bAutoLayout,
	TMap<FString, int32>& OutNodeKeyIds,
	TArray<FHyperAITextureGraphOutput>& OutOutputs,
	TArray<FHyperAITextureGraphIssue>& OutIssues,
	FString& OutError)
{
	OutOutputs.Reset();
	OutIssues.Reset();
	const FName ShadowName = MakeUniqueObjectName(GetTransientPackage(), UTextureGraph::StaticClass(), TEXT("HyperAITextureGraphShadow"));
	UTextureGraph* Shadow = nullptr;
	if (Source)
	{
		Shadow = DuplicateObject<UTextureGraph>(Source, GetTransientPackage(), ShadowName);
	}
	else
	{
		Shadow = NewObject<UTextureGraph>(GetTransientPackage(), ShadowName, RF_Transient);
		if (Shadow)
		{
			Shadow->Construct(FPackageName::ObjectPathToObjectName(TargetPath));
			ApplyCreateDefaults(*Shadow, TargetPath);
		}
	}
	if (!Shadow || !Shadow->Graph())
	{
		OutError = TEXT("Could not build a transient copy of the graph to check the plan against.");
		return false;
	}
	Shadow->ClearFlags(RF_Public | RF_Standalone);
	Shadow->SetFlags(RF_Transient);
	int32 Applied = 0;
	const bool bApplied = ApplyOps(*Shadow, Ops, bAutoLayout, /*bTransact=*/false, OutNodeKeyIds, Applied, OutError);
	if (bApplied)
	{
		OutOutputs = ReadOutputs(*Shadow);
		OutIssues = Validate(*Shadow, /*bRequireExported=*/false);
	}
	Shadow->MarkAsGarbage();
	return bApplied;
}

UTextureGraph* CreateGraphAsset(const FString& TargetPath, FString& OutError)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(TargetPath);
	const FString AssetName = FPackageName::ObjectPathToObjectName(TargetPath);
	if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
	{
		OutError = TEXT("An asset already exists at target_path.");
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	UTextureGraph* Graph = Package
		? NewObject<UTextureGraph>(Package, UTextureGraph::StaticClass(), FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!Graph)
	{
		OutError = TEXT("Could not create the Texture Graph object.");
		return nullptr;
	}
	// Construct builds the inner graph and its Output node; without it Graph() is null.
	Graph->Construct(AssetName);
	ApplyCreateDefaults(*Graph, TargetPath);
	FAssetRegistryModule::AssetCreated(Graph);
	Package->MarkPackageDirty();
	return Graph;
}

bool StartExport(UTextureGraph& Graph, FString& OutError)
{
	if (!IsEngineAvailable())
	{
		OutError = TEXT("Texture Graph's engine is not running; it starts with the Texture Graph editor module.");
		return false;
	}
	UTG_AsyncExportTask* Task = UTG_AsyncExportTask::TG_AsyncExportTask(&Graph,
		/*OverwriteTextures=*/true, /*bSave=*/true, /*bExportAll=*/false, /*bDisableCache=*/false);
	if (!Task)
	{
		OutError = TEXT("Texture Graph did not create an export task.");
		return false;
	}
	Task->Activate();
	return true;
}
}
