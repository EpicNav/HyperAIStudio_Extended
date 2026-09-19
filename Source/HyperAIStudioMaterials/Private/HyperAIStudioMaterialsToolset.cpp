// Games by Hyper 2026.

#include "HyperAIStudioMaterialsToolset.h"
#include "HyperAIStudioAgentActivity.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioMaterialsGraphGate.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSettings.h"
#include "Internationalization/Text.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMaterials, Log, All);

namespace HyperAIStudio::Materials::Private
{
	constexpr int32 MaxUtf8BytesPerCharacter = 4;

	void AppendToken(FString& Out, const FString& Value)
	{
		Out += FString::FromInt(Value.Len());
		Out += TEXT(":");
		Out += Value;
		Out += TEXT("|");
	}

	FString BoolToken(const bool bValue) { return bValue ? TEXT("1") : TEXT("0"); }

	FString CanonicalDouble(const double Value)
	{
		return FMath::IsFinite(Value) ? FString::Printf(TEXT("%.17g"), Value) : TEXT("non_finite");
	}

	FString Clip(const FString& Value, const int32 MaxCharacters = 512)
	{
		return Value.Left(FMath::Max(0, MaxCharacters));
	}

	bool IsBeforeDeadline(const double Deadline)
	{
		return Deadline == MAX_dbl || FPlatformTime::Seconds() < Deadline;
	}

	bool TryGetBoundedObjectPath(
		const UObject* Object,
		const int32 MaxCharacters,
		const double Deadline,
		FString& OutPath)
	{
		OutPath.Reset();
		if (!Object || MaxCharacters < 1 || !IsBeforeDeadline(Deadline)) return false;
		TArray<const UObject*, TInlineAllocator<32>> Chain;
		TSet<const UObject*> Seen;
		const UObject* Cursor = Object;
		int32 CharacterBudget = 0;
		while (Cursor)
		{
			if (!IsBeforeDeadline(Deadline) || Chain.Num() >= 32 || Seen.Contains(Cursor)) return false;
			Seen.Add(Cursor);
			const FString Name = Cursor->GetFName().ToString();
			if (Name.IsEmpty() || Name.Len() > MaxCharacters
				|| CharacterBudget > MaxCharacters - Name.Len()) return false;
			CharacterBudget += Name.Len();
			Chain.Add(Cursor);
			Cursor = Cursor->GetOuter();
		}
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			const UObject* Current = Chain[Index];
			if (!OutPath.IsEmpty())
			{
				const UObject* Outer = Current->GetOuter();
				const UObject* OuterOuter = Outer ? Outer->GetOuter() : nullptr;
				OutPath.AppendChar(Outer && Outer->GetClass() != UPackage::StaticClass()
					&& OuterOuter && OuterOuter->GetClass() == UPackage::StaticClass()
					? SUBOBJECT_DELIMITER_CHAR : TEXT('.'));
			}
			OutPath += Current->GetFName().ToString();
			if (OutPath.Len() > MaxCharacters) return false;
		}
		return !OutPath.IsEmpty();
	}

	bool TryAppendBoundedCanonicalToken(
		FString& Out,
		int64& InOutUtf8Bytes,
		const FString& Value,
		const double Deadline)
	{
		if (!IsBeforeDeadline(Deadline)) return false;
		const int64 WorstCaseValueBytes = 32ll
			+ static_cast<int64>(Value.Len()) * MaxUtf8BytesPerCharacter;
		if (WorstCaseValueBytes > FHyperAIStudioMaterialsContracts::MaxSnapshotMaterializedBytes
			|| Out.Len() > FHyperAIStudioExtensionRuntime::MaxHashInputBytes
				- Value.Len() - 32)
			return false;
		const FString Prefix = FString::FromInt(Value.Len()) + TEXT(":");
		const FTCHARToUTF8 PrefixUtf8(*Prefix);
		const FTCHARToUTF8 ValueUtf8(*Value);
		const int64 Delta = static_cast<int64>(PrefixUtf8.Length())
			+ static_cast<int64>(ValueUtf8.Length()) + 1;
		if (Delta < 0
			|| InOutUtf8Bytes > FHyperAIStudioMaterialsContracts::MaxSnapshotMaterializedBytes - Delta
			|| Out.Len() > FHyperAIStudioExtensionRuntime::MaxHashInputBytes
				- Prefix.Len() - Value.Len() - 1)
			return false;
		Out += Prefix; Out += Value; Out += TEXT("|"); InOutUtf8Bytes += Delta;
		return true;
	}

	template <typename ElementType, typename CanonicalizerType>
	bool TryAppendSortedElementHashes(
		FString& Out,
		int64& InOutUtf8Bytes,
		const TArray<ElementType>& Elements,
		CanonicalizerType Canonicalizer,
		const double Deadline)
	{
		TArray<FString> Hashes;
		Hashes.Reserve(Elements.Num());
		for (const ElementType& Element : Elements)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			const FString Hash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
				Canonicalizer(Element));
			if (!FHyperAIStudioMaterialsContracts::IsCanonicalSha256(Hash)) return false;
			Hashes.Add(Hash);
		}
		Hashes.Sort();
		for (const FString& Hash : Hashes)
			if (!TryAppendBoundedCanonicalToken(Out, InOutUtf8Bytes, Hash, Deadline)) return false;
		return true;
	}

	template <typename ElementType>
	bool AddUnique(TSet<ElementType>& Set, const ElementType& Value)
	{
		bool bAlreadyPresent = false;
		Set.Add(Value, &bAlreadyPresent);
		return !bAlreadyPresent;
	}

	bool IsAllowedFamily(const FString& Family)
	{
		return Family == TEXT("material") || Family == TEXT("material_function");
	}

	bool IsAllowedScope(const FString& Scope)
	{
		return Scope == TEXT("loaded_only") || Scope == TEXT("on_disk_index");
	}

	FString StableIdFor(const UMaterialExpression* Expression, const int32 Index)
	{
		if (!Expression)
		{
			return FString::Printf(TEXT("invalid:%d"), Index);
		}
		const FGuid Guid = const_cast<UMaterialExpression*>(Expression)->GetMaterialExpressionId();
		return Guid.IsValid()
			? FString(TEXT("guid:")) + Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
			: FString::Printf(TEXT("invalid:%d"), Index);
	}

	FString NodeKind(const UMaterialExpression* Expression)
	{
		if (!Expression) return TEXT("other");
		if (Expression->GetClass() == UMaterialExpressionConstant::StaticClass()) return TEXT("constant");
		if (Expression->GetClass() == UMaterialExpressionScalarParameter::StaticClass()) return TEXT("scalar_parameter");
		if (Expression->GetClass() == UMaterialExpressionVectorParameter::StaticClass()) return TEXT("vector_parameter");
		if (Expression->GetClass() == UMaterialExpressionAdd::StaticClass()) return TEXT("add");
		if (Expression->GetClass() == UMaterialExpressionMultiply::StaticClass()) return TEXT("multiply");
		if (Expression->GetClass() == UMaterialExpressionFunctionInput::StaticClass()) return TEXT("function_input");
		if (Expression->GetClass() == UMaterialExpressionFunctionOutput::StaticClass()) return TEXT("function_output");
		const FString Kind = HyperAIStudio::Materials::Gate::KindOf(*Expression);
		return Kind.IsEmpty() ? TEXT("opaque") : Kind;
	}

	bool CaptureSemanticText(const FString& Value, FString& OutValue)
	{
		OutValue.Reset();
		if (Value.Len() > FHyperAIStudioMaterialsContracts::MaxSemanticTextCharacters)
			return false;
		OutValue = Value;
		return true;
	}

	bool CaptureSemanticText(const FText& Text, FString& OutValue)
	{
		OutValue.Reset();
		if (Text.IsEmpty()) { OutValue = TEXT("text:empty"); return true; }
		const FString* Source = FTextInspector::GetSourceString(Text);
		if (!Source || Source->Len() > FHyperAIStudioMaterialsContracts::MaxSemanticTextCharacters)
			return false;
		const FTextId TextId = FTextInspector::GetTextId(Text);
		const bool bInitializedFromString = Text.IsInitializedFromString();
		const bool bCultureInvariant = Text.IsCultureInvariant();
		const bool bStableLocalizedIdentity =
			!TextId.GetNamespace().IsEmpty() && !TextId.GetKey().IsEmpty();
		// Format/numeric/string-table histories are deliberately not expanded or serialized here.
		if (Text.IsFromStringTable()
			|| (!bInitializedFromString && !bCultureInvariant && !bStableLocalizedIdentity))
			return false;
		AppendToken(OutValue, TEXT("bounded_simple_text.v1"));
		AppendToken(OutValue, bInitializedFromString ? TEXT("initialized_from_string")
			: (bCultureInvariant ? TEXT("culture_invariant") : TEXT("localized_identity")));
		AppendToken(OutValue, *Source);
		AppendToken(OutValue, FString::Printf(TEXT("%08x"), GetTypeHash(TextId.GetNamespace())));
		AppendToken(OutValue, FString::Printf(TEXT("%08x"), GetTypeHash(TextId.GetKey())));
		AppendToken(OutValue, FString::Printf(TEXT("%08x"), FTextInspector::GetFlags(Text)));
		return OutValue.Len() <= FHyperAIStudioMaterialsContracts::MaxSemanticTextCharacters + 128;
	}

	FString GuidString(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
	}

	FString CanonicalInputPort(const FHyperAIMaterialInputPortView& Port)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Port.Index));
		AppendToken(Out, Port.Name);
		AppendToken(Out, Port.PersistedName);
		AppendToken(Out, FString::FromInt(Port.ValueType));
		AppendToken(Out, BoolToken(Port.bConnected));
		AppendToken(Out, FString::FromInt(Port.OutputIndex));
		AppendToken(Out, FString::FromInt(Port.Mask));
		AppendToken(Out, FString::FromInt(Port.MaskR));
		AppendToken(Out, FString::FromInt(Port.MaskG));
		AppendToken(Out, FString::FromInt(Port.MaskB));
		AppendToken(Out, FString::FromInt(Port.MaskA));
		return Out;
	}

	FString CanonicalOutputPort(const FHyperAIMaterialOutputPortView& Port)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Port.Index));
		AppendToken(Out, Port.Name);
		AppendToken(Out, FString::FromInt(Port.ValueType));
		AppendToken(Out, FString::FromInt(Port.Mask));
		AppendToken(Out, FString::FromInt(Port.MaskR));
		AppendToken(Out, FString::FromInt(Port.MaskG));
		AppendToken(Out, FString::FromInt(Port.MaskB));
		AppendToken(Out, FString::FromInt(Port.MaskA));
		return Out;
	}

	FHyperAIMaterialNodeView MakeNodeView(
		const UMaterialExpression* Expression,
		const int32 Index,
		const double Deadline,
		bool& bOutComplete)
	{
		FHyperAIMaterialNodeView View;
		bool bComplete = Expression != nullptr && FPlatformTime::Seconds() < Deadline;
		View.StableId = StableIdFor(Expression, Index);
		if (!Expression)
		{
			View.Kind = TEXT("other");
			View.bSemanticProjectionComplete = false;
			bOutComplete = false;
			return View;
		}
		UMaterialExpression* MutableExpression = const_cast<UMaterialExpression*>(Expression);
		const FGuid PersistedGuid = MutableExpression->GetMaterialExpressionId();
		View.PersistedGuid = GuidString(PersistedGuid);
		View.Kind = NodeKind(Expression);
		bComplete &= TryGetBoundedObjectPath(Expression->GetClass(),
			FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, View.ClassPath);
		if (const UObject* Outer = Expression->GetOuter())
		{
			bComplete &= TryGetBoundedObjectPath(Outer,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, View.OuterPath);
			bComplete &= TryGetBoundedObjectPath(Outer->GetClass(),
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, View.OuterClassPath);
		}
		if (const UPackage* Package = Expression->GetOutermost())
			bComplete &= CaptureSemanticText(Package->GetFName().ToString(), View.OutermostPackageName);
		if (Expression->Material)
			bComplete &= TryGetBoundedObjectPath(Expression->Material,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, View.MaterialOwnerPath);
		if (Expression->Function)
			bComplete &= TryGetBoundedObjectPath(Expression->Function,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, View.FunctionOwnerPath);
		View.EditorX = Expression->MaterialExpressionEditorX;
		View.EditorY = Expression->MaterialExpressionEditorY;
		View.bGuidValid = PersistedGuid.IsValid();
		bComplete &= CaptureSemanticText(Expression->Desc, View.Description);
		if (Expression->SubgraphExpression)
			bComplete &= TryGetBoundedObjectPath(Expression->SubgraphExpression,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline,
				View.SubgraphExpressionPath);
		View.bRealtimePreview = Expression->bRealtimePreview;
		View.bIsParameterExpression = Expression->bIsParameterExpression;
		View.bCommentBubbleVisible = Expression->bCommentBubbleVisible;
		View.bShowOutputNameOnPin = Expression->bShowOutputNameOnPin;
		View.bShowMaskColorsOnPin = Expression->bShowMaskColorsOnPin;
		View.bHidePreviewWindow = Expression->bHidePreviewWindow;
		View.bCollapsed = Expression->bCollapsed;
		View.bShaderInputData = Expression->bShaderInputData;
		View.bShowInputs = Expression->bShowInputs;
		View.bShowOutputs = Expression->bShowOutputs;
		if (Expression->MenuCategories.Num()
			> FHyperAIStudioMaterialsContracts::MaxMenuCategoriesPerNode)
		{
			bComplete = false;
		}
		const int32 CategoryLimit = FMath::Min(
			Expression->MenuCategories.Num(),
			FHyperAIStudioMaterialsContracts::MaxMenuCategoriesPerNode);
		for (int32 CategoryIndex = 0; CategoryIndex < CategoryLimit; ++CategoryIndex)
		{
			if (FPlatformTime::Seconds() >= Deadline) { bComplete = false; break; }
			FString Category;
			bComplete &= CaptureSemanticText(Expression->MenuCategories[CategoryIndex], Category);
			View.MenuCategories.Add(MoveTemp(Category));
		}
		if (Expression->GetClass() == UMaterialExpressionConstant::StaticClass())
		{
			const auto* Constant = static_cast<const UMaterialExpressionConstant*>(Expression);
			View.Scalar = Constant->R;
		}
		else if (Expression->GetClass() == UMaterialExpressionScalarParameter::StaticClass())
		{
			const auto* Scalar = static_cast<const UMaterialExpressionScalarParameter*>(Expression);
			bComplete &= CaptureSemanticText(Scalar->ParameterName.ToString(), View.Name);
			bComplete &= CaptureSemanticText(Scalar->Group.ToString(), View.Group);
			View.ParameterGuid = GuidString(
				const_cast<UMaterialExpressionScalarParameter*>(Scalar)->GetParameterExpressionId());
			View.SortPriority = Scalar->SortPriority;
			View.Scalar = Scalar->DefaultValue;
			View.ScalarControlType = static_cast<int32>(Scalar->ControlType);
			View.SliderMin = Scalar->SliderMin;
			View.SliderMax = Scalar->SliderMax;
			bComplete &= CaptureSemanticText(
				Scalar->Enumeration.ToSoftObjectPath().ToString(), View.EnumerationPath);
			View.EnumerationIndex = Scalar->EnumerationIndex;
			View.bUseCustomPrimitiveData = Scalar->bUseCustomPrimitiveData;
			View.PrimitiveDataIndex = Scalar->PrimitiveDataIndex;
		}
		else if (Expression->GetClass() == UMaterialExpressionVectorParameter::StaticClass())
		{
			const auto* Vector = static_cast<const UMaterialExpressionVectorParameter*>(Expression);
			bComplete &= CaptureSemanticText(Vector->ParameterName.ToString(), View.Name);
			bComplete &= CaptureSemanticText(Vector->Group.ToString(), View.Group);
			View.ParameterGuid = GuidString(
				const_cast<UMaterialExpressionVectorParameter*>(Vector)->GetParameterExpressionId());
			View.SortPriority = Vector->SortPriority;
			View.Vector = Vector->DefaultValue;
			View.bUseCustomPrimitiveData = Vector->bUseCustomPrimitiveData;
			View.PrimitiveDataIndex = Vector->PrimitiveDataIndex;
			bComplete &= CaptureSemanticText(Vector->ChannelNames.R, View.ChannelR);
			bComplete &= CaptureSemanticText(Vector->ChannelNames.G, View.ChannelG);
			bComplete &= CaptureSemanticText(Vector->ChannelNames.B, View.ChannelB);
			bComplete &= CaptureSemanticText(Vector->ChannelNames.A, View.ChannelA);
		}
		else if (Expression->GetClass() == UMaterialExpressionAdd::StaticClass())
		{
			const auto* Add = static_cast<const UMaterialExpressionAdd*>(Expression);
			View.ConstA = Add->ConstA;
			View.ConstB = Add->ConstB;
		}
		else if (Expression->GetClass() == UMaterialExpressionMultiply::StaticClass())
		{
			const auto* Multiply = static_cast<const UMaterialExpressionMultiply*>(Expression);
			View.ConstA = Multiply->ConstA;
			View.ConstB = Multiply->ConstB;
		}
		else if (Expression->GetClass() == UMaterialExpressionFunctionInput::StaticClass())
		{
			const auto* Input = static_cast<const UMaterialExpressionFunctionInput*>(Expression);
			bComplete &= CaptureSemanticText(Input->InputName.ToString(), View.Name);
			bComplete &= CaptureSemanticText(Input->Description, View.FunctionDescription);
			View.FunctionId = GuidString(Input->Id);
			View.FunctionInputType = static_cast<int32>(Input->InputType.GetValue());
			View.FunctionPreviewValue = FVector4(
				Input->PreviewValue.X, Input->PreviewValue.Y,
				Input->PreviewValue.Z, Input->PreviewValue.W);
			View.bUseFunctionPreviewValueAsDefault = Input->bUsePreviewValueAsDefault;
			View.SortPriority = Input->SortPriority;
			View.FunctionBlendInputRelevance =
				static_cast<int32>(Input->BlendInputRelevance.GetValue());
		}
		else if (Expression->GetClass() == UMaterialExpressionFunctionOutput::StaticClass())
		{
			const auto* Output = static_cast<const UMaterialExpressionFunctionOutput*>(Expression);
			bComplete &= CaptureSemanticText(Output->OutputName.ToString(), View.Name);
			bComplete &= CaptureSemanticText(Output->Description, View.FunctionDescription);
			View.FunctionId = GuidString(Output->Id);
			View.SortPriority = Output->SortPriority;
			View.bFunctionOutputLastPreviewed = Output->bLastPreviewed;
		}
		// Catalog kinds expose their typed keys. Opaque nodes keep only pins and wiring; the record seals the saved
		// package hash for them instead, so an edit to one still changes the revision once saved.
		else if (View.Kind != TEXT("opaque"))
		{
			View.Properties = HyperAIStudio::Materials::Gate::ReadProperties(*Expression, View.Kind);
		}

		if (View.Kind != TEXT("other"))
		{
			const int32 InputCount = MutableExpression->CountInputs();
			if (InputCount < 0 || InputCount > FHyperAIStudioMaterialsContracts::MaxPortsPerNode)
				bComplete = false;
			const int32 InputLimit = FMath::Clamp(
				InputCount, 0, FHyperAIStudioMaterialsContracts::MaxPortsPerNode);
			for (int32 InputIndex = 0; InputIndex < InputLimit; ++InputIndex)
			{
				if (FPlatformTime::Seconds() >= Deadline) { bComplete = false; break; }
				FExpressionInput* Input = MutableExpression->GetInput(InputIndex);
				if (!Input) { bComplete = false; continue; }
				FHyperAIMaterialInputPortView& Port = View.InputPorts.AddDefaulted_GetRef();
				Port.Index = InputIndex;
				bComplete &= CaptureSemanticText(
					MutableExpression->GetInputName(InputIndex).ToString(), Port.Name);
				bComplete &= CaptureSemanticText(Input->InputName.ToString(), Port.PersistedName);
				Port.ValueType = static_cast<int32>(MutableExpression->GetInputValueType(InputIndex));
				Port.bConnected = Input->Expression != nullptr;
				Port.OutputIndex = Input->OutputIndex;
				Port.Mask = Input->Mask; Port.MaskR = Input->MaskR; Port.MaskG = Input->MaskG;
				Port.MaskB = Input->MaskB; Port.MaskA = Input->MaskA;
			}

			if (FPlatformTime::Seconds() >= Deadline)
			{
				bComplete = false;
			}
			else
			{
				TArray<FExpressionOutput>& Outputs = MutableExpression->GetOutputs();
				if (Outputs.Num() > FHyperAIStudioMaterialsContracts::MaxPortsPerNode)
					bComplete = false;
				const int32 OutputLimit = FMath::Min(
					Outputs.Num(), FHyperAIStudioMaterialsContracts::MaxPortsPerNode);
				for (int32 OutputIndex = 0; OutputIndex < OutputLimit; ++OutputIndex)
				{
					if (FPlatformTime::Seconds() >= Deadline) { bComplete = false; break; }
					const FExpressionOutput& Output = Outputs[OutputIndex];
					FHyperAIMaterialOutputPortView& Port = View.OutputPorts.AddDefaulted_GetRef();
					Port.Index = OutputIndex;
					bComplete &= CaptureSemanticText(Output.OutputName.ToString(), Port.Name);
					Port.ValueType = static_cast<int32>(MutableExpression->GetOutputValueType(OutputIndex));
					Port.Mask = Output.Mask; Port.MaskR = Output.MaskR; Port.MaskG = Output.MaskG;
					Port.MaskB = Output.MaskB; Port.MaskA = Output.MaskA;
				}
			}
		}

		bComplete = bComplete
			&& FMath::IsFinite(View.Scalar)
			&& FMath::IsFinite(View.Vector.R) && FMath::IsFinite(View.Vector.G)
			&& FMath::IsFinite(View.Vector.B) && FMath::IsFinite(View.Vector.A)
			&& FMath::IsFinite(View.SliderMin) && FMath::IsFinite(View.SliderMax)
			&& FMath::IsFinite(View.ConstA) && FMath::IsFinite(View.ConstB)
			&& FMath::IsFinite(View.FunctionPreviewValue.X)
			&& FMath::IsFinite(View.FunctionPreviewValue.Y)
			&& FMath::IsFinite(View.FunctionPreviewValue.Z)
			&& FMath::IsFinite(View.FunctionPreviewValue.W);
		View.bSemanticProjectionComplete = bComplete;
		bOutComplete = bComplete;
		return View;
	}

	FString CanonicalNode(const FHyperAIMaterialNodeView& Node)
	{
		FString Out;
		AppendToken(Out, Node.StableId);
		AppendToken(Out, Node.PersistedGuid);
		AppendToken(Out, Node.Kind);
		AppendToken(Out, Node.ClassPath);
		AppendToken(Out, Node.OuterPath);
		AppendToken(Out, Node.OuterClassPath);
		AppendToken(Out, Node.OutermostPackageName);
		AppendToken(Out, Node.MaterialOwnerPath);
		AppendToken(Out, Node.FunctionOwnerPath);
		AppendToken(Out, FString::FromInt(Node.EditorX));
		AppendToken(Out, FString::FromInt(Node.EditorY));
		AppendToken(Out, Node.Name);
		AppendToken(Out, Node.Group);
		AppendToken(Out, Node.Description);
		AppendToken(Out, Node.ParameterGuid);
		AppendToken(Out, FString::FromInt(Node.SortPriority));
		AppendToken(Out, CanonicalDouble(Node.Scalar));
		AppendToken(Out, CanonicalDouble(Node.Vector.R));
		AppendToken(Out, CanonicalDouble(Node.Vector.G));
		AppendToken(Out, CanonicalDouble(Node.Vector.B));
		AppendToken(Out, CanonicalDouble(Node.Vector.A));
		AppendToken(Out, FString::FromInt(Node.ScalarControlType));
		AppendToken(Out, CanonicalDouble(Node.SliderMin));
		AppendToken(Out, CanonicalDouble(Node.SliderMax));
		AppendToken(Out, Node.EnumerationPath);
		AppendToken(Out, FString::FromInt(Node.EnumerationIndex));
		AppendToken(Out, BoolToken(Node.bUseCustomPrimitiveData));
		AppendToken(Out, FString::FromInt(Node.PrimitiveDataIndex));
		AppendToken(Out, Node.ChannelR); AppendToken(Out, Node.ChannelG);
		AppendToken(Out, Node.ChannelB); AppendToken(Out, Node.ChannelA);
		AppendToken(Out, CanonicalDouble(Node.ConstA));
		AppendToken(Out, CanonicalDouble(Node.ConstB));
		AppendToken(Out, Node.FunctionDescription);
		AppendToken(Out, Node.FunctionId);
		AppendToken(Out, FString::FromInt(Node.FunctionInputType));
		AppendToken(Out, CanonicalDouble(Node.FunctionPreviewValue.X));
		AppendToken(Out, CanonicalDouble(Node.FunctionPreviewValue.Y));
		AppendToken(Out, CanonicalDouble(Node.FunctionPreviewValue.Z));
		AppendToken(Out, CanonicalDouble(Node.FunctionPreviewValue.W));
		AppendToken(Out, BoolToken(Node.bUseFunctionPreviewValueAsDefault));
		AppendToken(Out, FString::FromInt(Node.FunctionBlendInputRelevance));
		AppendToken(Out, BoolToken(Node.bFunctionOutputLastPreviewed));
		AppendToken(Out, Node.SubgraphExpressionPath);
		AppendToken(Out, Node.SubgraphExpressionStableId);
		AppendToken(Out, Node.SubgraphRootStableId);
		AppendToken(Out, BoolToken(Node.bOwnerTopologyComplete));
		AppendToken(Out, BoolToken(Node.bRealtimePreview));
		AppendToken(Out, BoolToken(Node.bIsParameterExpression));
		AppendToken(Out, BoolToken(Node.bCommentBubbleVisible));
		AppendToken(Out, BoolToken(Node.bShowOutputNameOnPin));
		AppendToken(Out, BoolToken(Node.bShowMaskColorsOnPin));
		AppendToken(Out, BoolToken(Node.bHidePreviewWindow));
		AppendToken(Out, BoolToken(Node.bCollapsed));
		AppendToken(Out, BoolToken(Node.bShaderInputData));
		AppendToken(Out, BoolToken(Node.bShowInputs));
		AppendToken(Out, BoolToken(Node.bShowOutputs));
		AppendToken(Out, TEXT("menu_categories"));
		AppendToken(Out, FString::FromInt(Node.MenuCategories.Num()));
		for (const FString& Category : Node.MenuCategories)
			AppendToken(Out, Category);
		AppendToken(Out, TEXT("input_ports"));
		AppendToken(Out, FString::FromInt(Node.InputPorts.Num()));
		for (const FHyperAIMaterialInputPortView& Port : Node.InputPorts)
			AppendToken(Out, CanonicalInputPort(Port));
		AppendToken(Out, TEXT("output_ports"));
		AppendToken(Out, FString::FromInt(Node.OutputPorts.Num()));
		for (const FHyperAIMaterialOutputPortView& Port : Node.OutputPorts)
			AppendToken(Out, CanonicalOutputPort(Port));
		AppendToken(Out, TEXT("properties"));
		AppendToken(Out, FString::FromInt(Node.Properties.Num()));
		for (const FHyperAIMaterialNodeProperty& Property : Node.Properties)
		{
			AppendToken(Out, Property.Key);
			AppendToken(Out, Property.Value);
		}
		AppendToken(Out, BoolToken(Node.bGuidValid));
		AppendToken(Out, BoolToken(Node.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalEdge(const FHyperAIMaterialEdgeView& Edge)
	{
		FString Out;
		AppendToken(Out, Edge.FromStableId);
		AppendToken(Out, FString::FromInt(Edge.FromOutputIndex));
		AppendToken(Out, Edge.FromOutputName);
		AppendToken(Out, Edge.ToStableId);
		AppendToken(Out, FString::FromInt(Edge.ToInputIndex));
		AppendToken(Out, Edge.ToInputName);
		AppendToken(Out, FString::FromInt(Edge.Mask));
		AppendToken(Out, FString::FromInt(Edge.MaskR));
		AppendToken(Out, FString::FromInt(Edge.MaskG));
		AppendToken(Out, FString::FromInt(Edge.MaskB));
		AppendToken(Out, FString::FromInt(Edge.MaskA));
		return Out;
	}

	FString CanonicalPropertyInput(const FHyperAIMaterialPropertyInputView& Property)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Property.PropertyIndex));
		AppendToken(Out, Property.Name);
		AppendToken(Out, BoolToken(Property.bAvailable));
		AppendToken(Out, FString::FromInt(Property.ValueType));
		AppendToken(Out, BoolToken(Property.bUseConstant));
		AppendToken(Out, BoolToken(Property.bHidden));
		AppendToken(Out, Property.ConstantState);
		AppendToken(Out, Property.PersistedInputName);
		AppendToken(Out, BoolToken(Property.bConnected));
		AppendToken(Out, FString::FromInt(Property.OutputIndex));
		AppendToken(Out, FString::FromInt(Property.Mask));
		AppendToken(Out, FString::FromInt(Property.MaskR));
		AppendToken(Out, FString::FromInt(Property.MaskG));
		AppendToken(Out, FString::FromInt(Property.MaskB));
		AppendToken(Out, FString::FromInt(Property.MaskA));
		return Out;
	}

	FString CanonicalNodeSpec(const FHyperAIMaterialNodeSpec& Node)
	{
		FString Out;
		AppendToken(Out, Node.Kind);
		AppendToken(Out, Node.NodeId);
		AppendToken(Out, Node.Name);
		AppendToken(Out, Node.Group);
		AppendToken(Out, CanonicalDouble(Node.Scalar));
		AppendToken(Out, CanonicalDouble(Node.Vector.R));
		AppendToken(Out, CanonicalDouble(Node.Vector.G));
		AppendToken(Out, CanonicalDouble(Node.Vector.B));
		AppendToken(Out, CanonicalDouble(Node.Vector.A));
		AppendToken(Out, FString::FromInt(Node.EditorX));
		AppendToken(Out, FString::FromInt(Node.EditorY));
		AppendToken(Out, FString::FromInt(Node.Properties.Num()));
		for (const FHyperAIMaterialNodeProperty& Property : Node.Properties)
		{
			AppendToken(Out, Property.Key);
			AppendToken(Out, Property.Value);
		}
		AppendToken(Out, Node.Justification);
		return Out;
	}

	const TCHAR* OperationTypeName(const EHyperAIStudioMaterialOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph: return TEXT("compound_create_configure_graph");
		case EHyperAIStudioMaterialOperationKind::RepairSemanticGraph: return TEXT("repair_semantic_graph");
		case EHyperAIStudioMaterialOperationKind::CreateMaterial: return TEXT("create_material");
		case EHyperAIStudioMaterialOperationKind::EditGraph: return TEXT("edit_graph");
		case EHyperAIStudioMaterialOperationKind::CreateMaterialInstance: return TEXT("create_material_instance");
		default: return TEXT("set_instance_parameters");
		}
	}

	FString CanonicalBackendOperation(const FHyperAIStudioMaterialBackendOperation& Operation)
	{
		FString Out;
		AppendToken(Out, OperationTypeName(Operation.Kind));
		AppendToken(Out, Operation.TargetPath);
		AppendToken(Out, Operation.TargetFamily);
		AppendToken(Out, Operation.ExpectedRevision);
		AppendToken(Out, TEXT("nodes")); AppendToken(Out, FString::FromInt(Operation.Nodes.Num()));
		for (const FHyperAIMaterialNodeSpec& Node : Operation.Nodes) AppendToken(Out, CanonicalNodeSpec(Node));
		AppendToken(Out, TEXT("edges")); AppendToken(Out, FString::FromInt(Operation.Edges.Num()));
		for (const FHyperAIMaterialEdgeSpec& Edge : Operation.Edges)
		{
			AppendToken(Out, Edge.FromNodeId);
			AppendToken(Out, Edge.FromOutput);
			AppendToken(Out, Edge.ToNodeId);
			AppendToken(Out, Edge.ToInput);
		}
		AppendToken(Out, TEXT("outputs")); AppendToken(Out, FString::FromInt(Operation.Outputs.Num()));
		for (const FHyperAIMaterialOutputSpec& Output : Operation.Outputs)
		{
			AppendToken(Out, Output.Property);
			AppendToken(Out, Output.FromNodeId);
			AppendToken(Out, Output.FromOutput);
		}
		AppendToken(Out, TEXT("repair_kinds"));
		AppendToken(Out, FString::FromInt(Operation.RepairKinds.Num()));
		for (const FString& Repair : Operation.RepairKinds) AppendToken(Out, Repair);
		// Order matters in every list below: edit_graph applies them in sequence.
		AppendToken(Out, TEXT("remove")); AppendToken(Out, FString::FromInt(Operation.RemoveNodeIds.Num()));
		for (const FString& Id : Operation.RemoveNodeIds) AppendToken(Out, Id);
		AppendToken(Out, TEXT("disconnect")); AppendToken(Out, FString::FromInt(Operation.Disconnects.Num()));
		for (const FHyperAIMaterialEdgeSpec& Cut : Operation.Disconnects)
		{
			AppendToken(Out, Cut.ToNodeId);
			AppendToken(Out, Cut.ToInput);
		}
		AppendToken(Out, TEXT("property_edits")); AppendToken(Out, FString::FromInt(Operation.PropertyEdits.Num()));
		for (const FHyperAIMaterialPropertyEdit& Edit : Operation.PropertyEdits)
		{
			AppendToken(Out, Edit.NodeId);
			AppendToken(Out, Edit.Key);
			AppendToken(Out, Edit.Value);
		}
		AppendToken(Out, TEXT("settings")); AppendToken(Out, FString::FromInt(Operation.MaterialSettings.Num()));
		for (const FHyperAIMaterialNodeProperty& Setting : Operation.MaterialSettings)
		{
			AppendToken(Out, Setting.Key);
			AppendToken(Out, Setting.Value);
		}
		AppendToken(Out, TEXT("parent")); AppendToken(Out, Operation.ParentPath);
		AppendToken(Out, TEXT("parameters")); AppendToken(Out, FString::FromInt(Operation.Parameters.Num()));
		for (const FHyperAIMaterialParameterValue& Parameter : Operation.Parameters)
		{
			AppendToken(Out, Parameter.Name);
			AppendToken(Out, Parameter.Type);
			AppendToken(Out, Parameter.Value);
		}
		return Out;
	}

	void AddIssue(
		TArray<FHyperAIMaterialIssue>& Issues,
		const int32 Limit,
		bool& bTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& AssetPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Limit)
		{
			bTruncated = true;
			return;
		}
		FHyperAIMaterialIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = FString(Code).Left(128);
		Issue.Severity = FString(Severity).Left(32);
		Issue.AssetPath = Clip(AssetPath, 256);
		Issue.StableId = Clip(StableId, 256);
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message);
	}

	bool HasErrors(const TArray<FHyperAIMaterialIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIMaterialIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	bool IsSimpleIdentifier(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioMaterialsContracts::MaxNameCharacters)
			return false;
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
				return false;
		}
		return true;
	}

	bool IsFiniteNode(const FHyperAIMaterialNodeSpec& Node)
	{
		return FMath::IsFinite(Node.Scalar)
			&& FMath::IsFinite(Node.Vector.R) && FMath::IsFinite(Node.Vector.G)
			&& FMath::IsFinite(Node.Vector.B) && FMath::IsFinite(Node.Vector.A)
			&& FMath::Abs(Node.EditorX) <= 1000000 && FMath::Abs(Node.EditorY) <= 1000000;
	}

	bool IsAllowedNodeKind(const FString& Kind, const FString& Family)
	{
		if (Kind == TEXT("constant") || Kind == TEXT("scalar_parameter")
			|| Kind == TEXT("vector_parameter") || Kind == TEXT("add") || Kind == TEXT("multiply"))
			return true;
		return Family == TEXT("material_function")
			&& (Kind == TEXT("function_input") || Kind == TEXT("function_output"));
	}

	bool IsAllowedOutputProperty(const FString& Property)
	{
		static const TSet<FString> Allowed = {
			TEXT("base_color"), TEXT("metallic"), TEXT("specular"), TEXT("roughness"),
			TEXT("emissive"), TEXT("opacity"), TEXT("opacity_mask"), TEXT("normal"), TEXT("ao")};
		return Allowed.Contains(Property);
	}

	bool IsAllowedRepair(const FString& Repair)
	{
		return Repair == TEXT("regenerate_duplicate_guids")
			|| Repair == TEXT("disconnect_dangling_inputs")
			|| Repair == TEXT("disconnect_cycles");
	}

	bool HasCycle(
		const TArray<FHyperAIMaterialEdgeSpec>& Edges,
		FString& OutNode,
		const double Deadline,
		bool& bOutDeadlineReached)
	{
		bOutDeadlineReached = false;
		TMap<FString, TArray<FString>> Adjacent;
		for (const FHyperAIMaterialEdgeSpec& Edge : Edges)
		{
			if (!IsBeforeDeadline(Deadline)) { bOutDeadlineReached = true; return false; }
			Adjacent.FindOrAdd(Edge.FromNodeId).Add(Edge.ToNodeId);
		}
		TMap<FString, uint8> Color;
		TFunction<bool(const FString&)> Visit = [&](const FString& Node)
		{
			if (!IsBeforeDeadline(Deadline)) { bOutDeadlineReached = true; return false; }
			const uint8 Existing = Color.FindRef(Node);
			if (Existing == 1) { OutNode = Node; return true; }
			if (Existing == 2) return false;
			Color.Add(Node, 1);
			if (const TArray<FString>* Next = Adjacent.Find(Node))
			{
				for (const FString& Child : *Next)
				{
					if (!IsBeforeDeadline(Deadline)) { bOutDeadlineReached = true; return false; }
					if (Visit(Child)) return true;
				}
			}
			Color.Add(Node, 2);
			return false;
		};
		for (const TPair<FString, TArray<FString>>& Pair : Adjacent)
		{
			if (!IsBeforeDeadline(Deadline)) { bOutDeadlineReached = true; return false; }
			if (Visit(Pair.Key)) return true;
		}
		return false;
	}

	bool ValidateCompoundGraph(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		TArray<FHyperAIMaterialIssue>& Issues,
		const int32 OperationIndex,
		const int32 IssueLimit,
		bool& bTruncated,
		const double Deadline)
	{
		auto DeadlineFailed = [&]()
		{
			if (IsBeforeDeadline(Deadline)) return false;
			AddIssue(Issues, IssueLimit, bTruncated, TEXT("planning_deadline_exceeded"),
				TEXT("error"), Operation.TargetPath, FString(), OperationIndex,
				TEXT("Compound graph validation exhausted the caller-owned deadline."));
			bTruncated = true;
			return true;
		};
		TSet<FString> NodeIds;
		TSet<FName> ParameterNames;
		for (const FHyperAIMaterialNodeSpec& Node : Operation.Nodes)
		{
			if (DeadlineFailed()) return false;
			if (!AddUnique(NodeIds, Node.NodeId))
				AddIssue(Issues, IssueLimit, bTruncated,
					TEXT("duplicate_node_id"), TEXT("error"), Operation.TargetPath, Node.NodeId,
					OperationIndex, TEXT("Each compound graph node_id must be unique."));
			if ((Node.Kind == TEXT("scalar_parameter") || Node.Kind == TEXT("vector_parameter"))
				&& !AddUnique(ParameterNames, FName(*Node.Name)))
				AddIssue(Issues, IssueLimit, bTruncated,
					TEXT("duplicate_parameter_name"), TEXT("error"), Operation.TargetPath, Node.NodeId,
					OperationIndex, TEXT("Parameter names must be unique in the persisted graph."));
		}
		TSet<FString> Inputs;
		for (const FHyperAIMaterialEdgeSpec& Edge : Operation.Edges)
		{
			if (DeadlineFailed()) return false;
			if (!NodeIds.Contains(Edge.FromNodeId) || !NodeIds.Contains(Edge.ToNodeId))
				AddIssue(Issues, IssueLimit, bTruncated,
					TEXT("edge_endpoint_missing"), TEXT("error"), Operation.TargetPath,
					Edge.ToNodeId, OperationIndex, TEXT("Every edge endpoint must name a node in the same closed operation."));
			const FString InputKey = Edge.ToNodeId + TEXT("\n") + Edge.ToInput;
			if (!AddUnique(Inputs, InputKey))
				AddIssue(Issues, IssueLimit, bTruncated,
					TEXT("contradictory_input_assignments"), TEXT("error"), Operation.TargetPath,
					Edge.ToNodeId, OperationIndex, TEXT("One input cannot receive more than one source in the same plan."));
		}
		FString CycleNode;
		bool bCycleDeadlineReached = false;
		if (HasCycle(Operation.Edges, CycleNode, Deadline, bCycleDeadlineReached))
			AddIssue(Issues, IssueLimit, bTruncated,
				TEXT("graph_cycle"), TEXT("error"), Operation.TargetPath, CycleNode,
				OperationIndex, TEXT("The typed shadow graph contains a cycle."));
		if (bCycleDeadlineReached || DeadlineFailed()) return false;
		if (Operation.TargetFamily == TEXT("material") && Operation.Outputs.IsEmpty())
			AddIssue(Issues, IssueLimit, bTruncated,
				TEXT("material_output_missing"), TEXT("error"), Operation.TargetPath, FString(),
				OperationIndex, TEXT("A material compound operation must configure at least one persisted material output."));
		for (const FHyperAIMaterialOutputSpec& Output : Operation.Outputs)
		{
			if (DeadlineFailed()) return false;
			if (!NodeIds.Contains(Output.FromNodeId))
				AddIssue(Issues, IssueLimit, bTruncated,
					TEXT("output_source_missing"), TEXT("error"), Operation.TargetPath, Output.FromNodeId,
					OperationIndex, TEXT("Every material output source must name a node in the compound graph."));
		}
		return !HasErrors(Issues);
	}

	EMaterialProperty OutputProperty(const FString& Name)
	{
		if (Name == TEXT("base_color")) return MP_BaseColor;
		if (Name == TEXT("metallic")) return MP_Metallic;
		if (Name == TEXT("specular")) return MP_Specular;
		if (Name == TEXT("roughness")) return MP_Roughness;
		if (Name == TEXT("emissive")) return MP_EmissiveColor;
		if (Name == TEXT("opacity")) return MP_Opacity;
		if (Name == TEXT("opacity_mask")) return MP_OpacityMask;
		if (Name == TEXT("normal")) return MP_Normal;
		if (Name == TEXT("ao")) return MP_AmbientOcclusion;
		return MP_MAX;
	}

	FString OutputName(const EMaterialProperty Property)
	{
		switch (Property)
		{
		case MP_BaseColor: return TEXT("base_color");
		case MP_Metallic: return TEXT("metallic");
		case MP_Specular: return TEXT("specular");
		case MP_Roughness: return TEXT("roughness");
		case MP_EmissiveColor: return TEXT("emissive");
		case MP_Opacity: return TEXT("opacity");
		case MP_OpacityMask: return TEXT("opacity_mask");
		case MP_Normal: return TEXT("normal");
		case MP_AmbientOcclusion: return TEXT("ao");
		default: return TEXT("unknown");
		}
	}

	const TArray<EMaterialProperty>& InspectedMaterialProperties()
	{
		static const TArray<EMaterialProperty> Values = []
		{
			TArray<EMaterialProperty> Result;
			Result.Reserve(static_cast<int32>(MP_MAX));
			for (int32 Index = 0; Index < static_cast<int32>(MP_MAX); ++Index)
				Result.Add(static_cast<EMaterialProperty>(Index));
			return Result;
		}();
		return Values;
	}

	FExpressionInput* GetPersistedMaterialPropertyInput(
		UMaterial* Material,
		const EMaterialProperty Property)
	{
		if (!Material) return nullptr;
		FMaterialInputDescription Description;
		return Material->GetExpressionInputDescription(Property, Description)
			? Description.Input : nullptr;
	}

	bool TryGetShaderComponentCount(
		const UE::Shader::FValue& Value,
		int32& OutComponentCount)
	{
		const UE::Shader::FType& Type = Value.GetType();
		const int32 TypeIndex = static_cast<int32>(Type.ValueType);
		if (TypeIndex < 0 || TypeIndex >= UE::Shader::NumValueTypes)
		{
			return false;
		}

		int32 DeclaredComponentCount = 0;
		if (Type.ValueType == UE::Shader::EValueType::Struct)
		{
			if (!Type.StructType)
			{
				return false;
			}
			DeclaredComponentCount = Type.StructType->ComponentTypes.Num();
		}
		else if (Type.ValueType == UE::Shader::EValueType::Object)
		{
			DeclaredComponentCount = 1;
		}
		else
		{
			DeclaredComponentCount =
				UE::Shader::GetValueTypeDescription(Type.ValueType).NumComponents;
		}

		const int32 StoredComponentCount = Value.Component.Num();
		if (DeclaredComponentCount != StoredComponentCount)
		{
			return false;
		}
		OutComponentCount = StoredComponentCount;
		return true;
	}

	FString CanonicalShaderValue(const UE::Shader::FValue& Value, bool& bOutComplete)
	{
		FString Out;
		const UE::Shader::FType& Type = Value.GetType();
		AppendToken(Out, FString::FromInt(static_cast<int32>(Type.ValueType)));
		if (Type.StructType)
		{
			const FString StructName = Type.StructType->Name ? Type.StructType->Name : TEXT("");
			FString BoundedName;
			bOutComplete &= CaptureSemanticText(StructName, BoundedName);
			AppendToken(Out, BoundedName);
			AppendToken(Out, FString::Printf(TEXT("%016llx"),
				static_cast<unsigned long long>(Type.StructType->Hash)));
		}
		else
		{
			AppendToken(Out, FString());
			AppendToken(Out, TEXT("0000000000000000"));
		}
		AppendToken(Out, Type.ObjectType.ToString());
		int32 ComponentCount = Value.Component.Num();
		AppendToken(Out, FString::FromInt(ComponentCount));
		if (!TryGetShaderComponentCount(Value, ComponentCount)
			|| ComponentCount < 0
			|| ComponentCount > FHyperAIStudioMaterialsContracts::MaxShaderValueComponents)
		{
			bOutComplete = false;
			return Out;
		}
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			AppendToken(Out, FString::Printf(TEXT("%016llx"),
				static_cast<unsigned long long>(Value.GetComponent(Index).Packed)));
		}
		return Out;
	}

	FHyperAIMaterialPropertyInputView CaptureMaterialPropertyInput(
		UMaterial* Material,
		const EMaterialProperty Property,
		bool& bOutComplete)
	{
		FHyperAIMaterialPropertyInputView View;
		View.PropertyIndex = static_cast<int32>(Property);
		View.Name = OutputName(Property);
		FMaterialInputDescription Description;
		View.bAvailable = Material
			&& Material->GetExpressionInputDescription(Property, Description);
		if (!View.bAvailable) return View;
		View.ValueType = static_cast<int32>(Description.Type);
		View.bUseConstant = Description.bUseConstant;
		View.bHidden = Description.bHidden;
		View.ConstantState = CanonicalShaderValue(Description.ConstantValue, bOutComplete);
		if (const FExpressionInput* Input = Description.Input)
		{
			bOutComplete &= CaptureSemanticText(
				Input->InputName.ToString(), View.PersistedInputName);
			View.bConnected = Input->Expression != nullptr;
			View.OutputIndex = Input->OutputIndex;
			View.Mask = Input->Mask; View.MaskR = Input->MaskR; View.MaskG = Input->MaskG;
			View.MaskB = Input->MaskB; View.MaskA = Input->MaskA;
		}
		return View;
	}

	UClass* ExpectedNodeClass(const FString& Kind)
	{
		if (Kind == TEXT("constant")) return UMaterialExpressionConstant::StaticClass();
		if (Kind == TEXT("scalar_parameter")) return UMaterialExpressionScalarParameter::StaticClass();
		if (Kind == TEXT("vector_parameter")) return UMaterialExpressionVectorParameter::StaticClass();
		if (Kind == TEXT("add")) return UMaterialExpressionAdd::StaticClass();
		if (Kind == TEXT("multiply")) return UMaterialExpressionMultiply::StaticClass();
		return nullptr;
	}

	bool TryExpectedSourceOutputIndex(
		const FString& NodeKindValue,
		const FString& Output,
		int32& OutIndex,
		FString* OutCanonicalName = nullptr)
	{
		UClass* Class = ExpectedNodeClass(NodeKindValue);
		UMaterialExpression* Cdo = Class
			? Cast<UMaterialExpression>(Class->GetDefaultObject()) : nullptr;
		if (!Cdo) return false;
		TArray<FExpressionOutput>& Outputs = Cdo->GetOutputs();
		if (Outputs.IsEmpty()) return false;
		if (Output.IsEmpty()) OutIndex = 0;
		else
		{
			OutIndex = Outputs.IndexOfByPredicate([&](const FExpressionOutput& Candidate)
			{
				return Candidate.OutputName.ToString() == Output;
			});
		}
		if (!Outputs.IsValidIndex(OutIndex)) return false;
		if (OutCanonicalName) *OutCanonicalName = Outputs[OutIndex].OutputName.ToString();
		return true;
	}

	bool TryExpectedTargetInputIndex(
		const FString& NodeKindValue,
		const FString& Input,
		int32& OutIndex,
		FString* OutCanonicalName = nullptr)
	{
		UClass* Class = ExpectedNodeClass(NodeKindValue);
		const UMaterialExpression* Cdo = Class
			? Cast<UMaterialExpression>(Class->GetDefaultObject()) : nullptr;
		if (!Cdo) return false;
		OutIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Cdo->CountInputs(); ++Index)
		{
			if (Cdo->GetInputName(Index).ToString() == Input)
			{
				OutIndex = Index;
				break;
			}
		}
		if (OutIndex == INDEX_NONE) return false;
		if (OutCanonicalName) *OutCanonicalName = Cdo->GetInputName(OutIndex).ToString();
		return true;
	}

	enum class EClosedMaterialValueType : uint8
	{
		Scalar,
		Vector3
	};

	bool TryClosedOutputType(
		const FString& NodeKindValue,
		const FString& Output,
		EClosedMaterialValueType& OutType)
	{
		int32 OutputIndex = INDEX_NONE;
		if (!TryExpectedSourceOutputIndex(NodeKindValue, Output, OutputIndex)) return false;
		OutType = NodeKindValue == TEXT("vector_parameter") && OutputIndex == 0
			? EClosedMaterialValueType::Vector3
			: EClosedMaterialValueType::Scalar;
		return true;
	}

	bool IsClosedMaterialOutputCompatible(
		const FString& Property,
		const EClosedMaterialValueType Type)
	{
		const bool bVectorProperty = Property == TEXT("base_color")
			|| Property == TEXT("emissive") || Property == TEXT("normal");
		return Type == EClosedMaterialValueType::Scalar || bVectorProperty;
	}

	FString ExpectedNodeClassPath(const FString& Kind)
	{
		UClass* Class = ExpectedNodeClass(Kind);
		return Class ? Class->GetPathName() : FString();
	}

	FString SemanticEdgeKey(
		const FString& FromStableId,
		const int32 FromOutputIndex,
		const FString& FromOutputName,
		const FString& ToStableId,
		const int32 ToInputIndex,
		const FString& ToInputName,
		const int32 Mask = 0,
		const int32 MaskR = 0,
		const int32 MaskG = 0,
		const int32 MaskB = 0,
		const int32 MaskA = 0)
	{
		FString Out;
		AppendToken(Out, FromStableId);
		AppendToken(Out, FString::FromInt(FromOutputIndex));
		AppendToken(Out, FromOutputName);
		AppendToken(Out, ToStableId);
		AppendToken(Out, FString::FromInt(ToInputIndex));
		AppendToken(Out, ToInputName);
		AppendToken(Out, FString::FromInt(Mask));
		AppendToken(Out, FString::FromInt(MaskR));
		AppendToken(Out, FString::FromInt(MaskG));
		AppendToken(Out, FString::FromInt(MaskB));
		AppendToken(Out, FString::FromInt(MaskA));
		return Out;
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> GetExpressionView(UObject* Object)
	{
		if (const UMaterial* Material = Cast<UMaterial>(Object))
			return Material->GetExpressions();
		if (const UMaterialFunction* Function = Cast<UMaterialFunction>(Object))
			return Function->GetExpressions();
		return {};
	}

	UE::AssetRegistry::EExists CaptureDiskEvidence(
		const FString& Path,
		FHyperAIMaterialAssetRecord& Record,
		bool& bComplete,
		const UObject* LoadedExactObject,
		const double Deadline)
	{
		if (!IsBeforeDeadline(Deadline))
		{
			Record.DiskExistence = TEXT("unknown"); bComplete = false;
			return UE::AssetRegistry::EExists::Unknown;
		}
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
		FAssetPackageData PackageData;
		const UE::AssetRegistry::EExists State =
			Registry.TryGetAssetPackageData(FName(*PackageName), PackageData, true);
		Record.DiskExistence = FHyperAIStudioMaterialsContracts::ClassifyAssetRegistryExistence(State);
		Record.bExistsOnDisk = false;
		if (State == UE::AssetRegistry::EExists::Unknown)
		{
			bComplete = false;
			return State;
		}
		if (State == UE::AssetRegistry::EExists::DoesNotExist)
			return State;
		FString LoadedPath;
		const bool bLoadedIdentityExact = LoadedExactObject
			&& TryGetBoundedObjectPath(LoadedExactObject,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, LoadedPath)
			&& LoadedPath == Path
			&& LoadedExactObject->GetOuter() == LoadedExactObject->GetOutermost()
			&& LoadedExactObject->GetOutermost()->GetFName().ToString() == PackageName;
		if (!bLoadedIdentityExact)
		{
			Record.DiskExistence = TEXT("unknown");
			bComplete = false;
			return UE::AssetRegistry::EExists::Unknown;
		}
		Record.bExistsOnDisk = true;
		Record.ClassPath = LoadedExactObject->GetClass()->GetClassPathName().ToString();
		Record.DiskSize = PackageData.DiskSize;
		return State;
	}

	bool CheckCreateTargetAbsence(const FString& Path, FString& OutError)
	{
		const bool bLoadedObjectPresent = FSoftObjectPath(Path).ResolveObject() != nullptr;
		if (bLoadedObjectPresent)
		{
			OutError = TEXT("create_target_exists");
			return false;
		}
		FAssetPackageData PackageData;
		const FName PackageName(*FPackageName::ObjectPathToPackageName(Path));
		const UE::AssetRegistry::EExists State =
			IAssetRegistry::GetChecked().TryGetAssetPackageData(PackageName, PackageData, true);
		if (State == UE::AssetRegistry::EExists::DoesNotExist)
			return true;
		// Package Exists does not prove the exact object row; both Exists and lock-contention
		// Unknown remain non-authoritative and therefore block creation.
		OutError = TEXT("create_target_existence_unknown");
		return false;
	}

	bool FillLoadedRecord(
		UObject* Object,
		const FString& Family,
		const int32 MaxNodes,
		const int32 MaxEdges,
		const double Deadline,
		FHyperAIStudioMaterialValueSnapshot& Snapshot,
		FString& OutStatus,
		FString& OutDiagnostic)
	{
		FHyperAIMaterialAssetRecord& Record = Snapshot.Record;
		Record.Family = Family;
		bool bComplete = true;
			int64 MaterializedBytes = 16 * 1024;
			auto ConsumeMaterialized = [&](const FString& CanonicalValue)
			{
				const int64 WorstCaseBytes = 256ll
					+ static_cast<int64>(CanonicalValue.Len()) * MaxUtf8BytesPerCharacter;
				if (!IsBeforeDeadline(Deadline)
					|| WorstCaseBytes > FHyperAIStudioMaterialsContracts::MaxSnapshotMaterializedBytes)
				{
					Record.bGraphTruncated = true;
					bComplete = false;
					return false;
				}
				const FTCHARToUTF8 Utf8(*CanonicalValue);
				const int64 Delta = 256ll + static_cast<int64>(Utf8.Length());
				if (Delta > FHyperAIStudioMaterialsContracts::MaxSnapshotMaterializedBytes
					|| MaterializedBytes
						> FHyperAIStudioMaterialsContracts::MaxSnapshotMaterializedBytes - Delta)
				{
					Record.bGraphTruncated = true;
					bComplete = false;
					return false;
				}
			MaterializedBytes += Delta;
			return true;
		};
		bComplete &= TryGetBoundedObjectPath(Object,
			FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, Record.AssetPath);
		bComplete &= TryGetBoundedObjectPath(Object->GetClass(),
			FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, Record.ClassPath);
		Record.bLoaded = true;
		if (const UObject* Outer = Object->GetOuter())
			bComplete &= TryGetBoundedObjectPath(Outer,
				FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, Record.OuterPath);
		if (const UPackage* Package = Object->GetOutermost())
			Record.OutermostPackageName = Package->GetFName().ToString();
		const FString ExpectedPackageName = FPackageName::ObjectPathToPackageName(Record.AssetPath);
		Record.bRootOwnershipComplete = Object->GetOuter() == Object->GetOutermost()
			&& Record.OuterPath == ExpectedPackageName
			&& Record.OutermostPackageName == ExpectedPackageName;
		bComplete &= Record.bRootOwnershipComplete;
		Record.bPackageDirty = Object->GetOutermost()->IsDirty();
		CaptureDiskEvidence(Record.AssetPath, Record, bComplete, Object, Deadline);

		const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = GetExpressionView(Object);
		Record.NodeCount = Expressions.Num();
		if (!FHyperAIStudioMaterialsContracts::IsExpressionCollectionWithinBound(
			Expressions.Num(), MaxNodes))
		{
			Record.bGraphTruncated = true;
			bComplete = false;
		}
		const int32 NodeLimit = FMath::Min(Expressions.Num(), MaxNodes);
		TMap<FGuid, int32> GuidCounts;
		for (int32 Index = 0; Index < NodeLimit; ++Index)
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Record.bGraphTruncated = true;
				bComplete = false;
				break;
			}
			UMaterialExpression* Expression = Expressions[Index];
			const FGuid Guid = Expression ? Expression->GetMaterialExpressionId() : FGuid();
			if (Guid.IsValid()) ++GuidCounts.FindOrAdd(Guid);
		}
		TMap<const UMaterialExpression*, FString> StableByExpression;
		TMap<const UMaterialExpression*, int32> NodeIndexByExpression;
		for (int32 Index = 0; Index < NodeLimit; ++Index)
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				bComplete = false;
				Record.bGraphTruncated = true;
				break;
			}
			UMaterialExpression* Expression = Expressions[Index];
			bool bNodeProjectionComplete = false;
			FHyperAIMaterialNodeView View = MakeNodeView(
				Expression, Index, Deadline, bNodeProjectionComplete);
			if (Family == TEXT("material")
				&& (View.MaterialOwnerPath != Record.AssetPath || !View.FunctionOwnerPath.IsEmpty()))
			{
				View.bSemanticProjectionComplete = false;
				bNodeProjectionComplete = false;
			}
			if (Family == TEXT("material_function")
				&& (View.FunctionOwnerPath != Record.AssetPath || !View.MaterialOwnerPath.IsEmpty()))
			{
				View.bSemanticProjectionComplete = false;
				bNodeProjectionComplete = false;
			}
			View.bOwnerTopologyComplete = Expression
				&& Expression->GetOuter() == Object
				&& Expression->GetOutermost() == Object->GetOutermost()
				&& View.OuterPath == Record.AssetPath
				&& View.OutermostPackageName == Record.OutermostPackageName;
			if (!View.bOwnerTopologyComplete)
			{
				View.bSemanticProjectionComplete = false;
				bNodeProjectionComplete = false;
			}
			bComplete &= bNodeProjectionComplete;
			const FGuid Guid = Expression ? Expression->GetMaterialExpressionId() : FGuid();
			if (Guid.IsValid() && GuidCounts.FindRef(Guid) > 1)
				View.StableId = FString(TEXT("duplicate-guid:")) + View.PersistedGuid
					+ TEXT(":") + FString::FromInt(Index);
			if (Expression)
			{
				StableByExpression.Add(Expression, View.StableId);
				NodeIndexByExpression.Add(Expression, Record.Nodes.Num());
			}
			if (!ConsumeMaterialized(CanonicalNode(View))) break;
			Record.Nodes.Add(MoveTemp(View));
		}
		for (int32 Index = 0; Index < NodeLimit && Index < Record.Nodes.Num(); ++Index)
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Record.bGraphTruncated = true; bComplete = false; break;
			}
			UMaterialExpression* Expression = Expressions[Index];
			FHyperAIMaterialNodeView& View = Record.Nodes[Index];
			if (!Expression) { View.bSemanticProjectionComplete = false; bComplete = false; continue; }
			UMaterialExpression* Parent = Expression->SubgraphExpression;
			if (!Parent)
			{
				View.SubgraphRootStableId = View.StableId;
				continue;
			}
			const FString* ParentStable = StableByExpression.Find(Parent);
			if (!ParentStable || Parent == Expression)
			{
				View.bOwnerTopologyComplete = false;
				View.bSemanticProjectionComplete = false;
				bComplete = false;
				continue;
			}
			View.SubgraphExpressionStableId = *ParentStable;
			TSet<const UMaterialExpression*> SeenSubgraphOwners;
			SeenSubgraphOwners.Add(Expression);
			UMaterialExpression* Root = Parent;
			int32 Depth = 0;
			while (Root->SubgraphExpression && Depth++ < MaxNodes)
			{
				if (FPlatformTime::Seconds() >= Deadline
					|| SeenSubgraphOwners.Contains(Root)
					|| !StableByExpression.Contains(Root->SubgraphExpression))
				{
					View.bOwnerTopologyComplete = false;
					View.bSemanticProjectionComplete = false;
					bComplete = false;
					break;
				}
				SeenSubgraphOwners.Add(Root);
				Root = Root->SubgraphExpression;
			}
			if (Depth >= MaxNodes || !View.bSemanticProjectionComplete) continue;
			if (const FString* RootStable = StableByExpression.Find(Root))
				View.SubgraphRootStableId = *RootStable;
			else
			{
				View.bOwnerTopologyComplete = false;
				View.bSemanticProjectionComplete = false;
				bComplete = false;
			}
		}

		int32 TotalEdges = 0;
		bool bStopEdgeCapture = false;
		auto AddEdgeFromInput = [&](const FExpressionInput* Input, const FString& ToStableId,
			const int32 InputIndex, const FString& InputName)
		{
			if (bStopEdgeCapture || !Input || !Input->Expression) return;
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Record.bGraphTruncated = true;
				bComplete = false;
				bStopEdgeCapture = true;
				return;
			}
			++TotalEdges;
			if (Record.Edges.Num() >= MaxEdges)
			{
				Record.bGraphTruncated = true;
				bComplete = false;
				bStopEdgeCapture = true;
				return;
			}
			FHyperAIMaterialEdgeView Edge;
			if (const FString* Stable = StableByExpression.Find(Input->Expression))
				Edge.FromStableId = *Stable;
			else
			{
				FString ExternalPath;
				if (!TryGetBoundedObjectPath(Input->Expression,
					FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, ExternalPath))
				{
					bComplete = false;
					bStopEdgeCapture = true;
					return;
				}
				const FString ExternalIdentity = FString(TEXT("outside_collection:")) + ExternalPath;
				Edge.FromStableId = Clip(
					ExternalIdentity, FHyperAIStudioMaterialsContracts::MaxPathCharacters);
				// A dangling FExpressionInput persists only the typed object pointer, output index,
				// and masks. Its bounded exact loaded object identity is sufficient CAS evidence
				// for the closed disconnect repair; the external expression body is not traversed.
				if (ExternalPath.IsEmpty()
					|| ExternalIdentity.Len() > FHyperAIStudioMaterialsContracts::MaxPathCharacters)
					bComplete = false;
			}
			Edge.FromOutputIndex = Input->OutputIndex;
			if (const int32* SourceNodeIndex = NodeIndexByExpression.Find(Input->Expression))
			{
				if (Record.Nodes.IsValidIndex(*SourceNodeIndex)
					&& Record.Nodes[*SourceNodeIndex].OutputPorts.IsValidIndex(Input->OutputIndex))
					Edge.FromOutputName =
						Record.Nodes[*SourceNodeIndex].OutputPorts[Input->OutputIndex].Name;
				else bComplete = false;
			}
			Edge.ToStableId = ToStableId;
			Edge.ToInputIndex = InputIndex;
			Edge.ToInputName = InputName;
			Edge.Mask = Input->Mask; Edge.MaskR = Input->MaskR; Edge.MaskG = Input->MaskG;
			Edge.MaskB = Input->MaskB; Edge.MaskA = Input->MaskA;
			if (!ConsumeMaterialized(CanonicalEdge(Edge)))
			{
				bStopEdgeCapture = true;
				return;
			}
			Record.Edges.Add(MoveTemp(Edge));
		};

		for (int32 Index = 0;
			Index < NodeLimit && Index < Record.Nodes.Num() && !bStopEdgeCapture;
			++Index)
		{
			UMaterialExpression* Expression = Expressions[Index];
			if (!Expression || !Record.Nodes[Index].bSemanticProjectionComplete)
			{
				bComplete = false;
				continue;
			}
			for (const FHyperAIMaterialInputPortView& Port : Record.Nodes[Index].InputPorts)
			{
				if (FPlatformTime::Seconds() >= Deadline)
				{
					Record.bGraphTruncated = true;
					bComplete = false;
					bStopEdgeCapture = true;
					break;
				}
				AddEdgeFromInput(Expression->GetInput(Port.Index), Record.Nodes[Index].StableId,
					Port.Index, Port.Name);
			}
		}
		if (UMaterial* Material = Cast<UMaterial>(Object))
		{
			Record.StateId = Material->StateId.ToString(EGuidFormats::DigitsWithHyphensLower);
			Record.bCompileStateKnown = GMaxRHIShaderPlatform != SP_NumPlatforms;
			Record.bCompiling = Material->IsCompiling();
			if (Record.bCompileStateKnown)
				Record.bCompileError = Material->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform)
					&& !Record.bCompiling;
			else bComplete = false;
			for (const EMaterialProperty Property : InspectedMaterialProperties())
			{
				if (FPlatformTime::Seconds() >= Deadline)
				{
					Record.bGraphTruncated = true; bComplete = false; break;
				}
				FHyperAIMaterialPropertyInputView PropertyView = CaptureMaterialPropertyInput(
					Material, Property, bComplete);
				if (!ConsumeMaterialized(CanonicalPropertyInput(PropertyView))) break;
				Record.PropertyInputs.Add(MoveTemp(PropertyView));
				AddEdgeFromInput(GetPersistedMaterialPropertyInput(Material, Property),
					TEXT("$material_output"), static_cast<int32>(Property), OutputName(Property));
				if (bStopEdgeCapture) break;
			}
			const FMaterialExpressionCollection& Collection = Material->GetExpressionCollection();
			auto CaptureExec = [&](const UMaterialExpression* Expression, FString& OutStableId)
			{
				if (!Expression) return;
				if (const FString* Stable = StableByExpression.Find(Expression)) OutStableId = *Stable;
				else
				{
					FString ExternalPath;
					if (!TryGetBoundedObjectPath(Expression,
						FHyperAIStudioMaterialsContracts::MaxPathCharacters, Deadline, ExternalPath))
						ExternalPath = TEXT("unbounded_external_expression");
					OutStableId = Clip(FString(TEXT("outside_collection:")) + ExternalPath,
						FHyperAIStudioMaterialsContracts::MaxPathCharacters);
					bComplete = false;
				}
			};
			CaptureExec(Collection.ExpressionExecBegin, Record.ExpressionExecBeginStableId);
			CaptureExec(Collection.ExpressionExecEnd, Record.ExpressionExecEndStableId);
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Object))
		{
			Record.StateId = Function->StateId.ToString(EGuidFormats::DigitsWithHyphensLower);
			// UE 5.8 exposes UpdateMaterialFunction but no persisted function compile/error query.
			Record.bCompileStateKnown = false;
			bComplete = false;
			const FMaterialExpressionCollection& Collection = Function->GetExpressionCollection();
			auto CaptureExec = [&](const UMaterialExpression* Expression, FString& OutStableId)
			{
				if (!Expression) return;
				if (const FString* Stable = StableByExpression.Find(Expression)) OutStableId = *Stable;
				else { OutStableId = TEXT("outside_collection"); bComplete = false; }
			};
			CaptureExec(Collection.ExpressionExecBegin, Record.ExpressionExecBeginStableId);
			CaptureExec(Collection.ExpressionExecEnd, Record.ExpressionExecEndStableId);
		}
		for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
		{
			Record.OpaqueNodeCount += Node.Kind == TEXT("opaque") ? 1 : 0;
			if (Node.Kind == TEXT("custom_hlsl")) Record.CustomHlslNodeIds.Add(Node.StableId);
		}
		if (Record.OpaqueNodeCount > 0)
		{
			Record.PackageSavedHash = LexToString(Object->GetOutermost()->GetSavedHash());
			bComplete &= !Record.bPackageDirty;
		}
		Record.EdgeCount = TotalEdges;
		Record.bRevisionComplete = bComplete && !Record.bGraphTruncated;
		Snapshot.bComplete = Record.bRevisionComplete;
		Snapshot.Revision = FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(Snapshot, Deadline);
		Record.Revision = Snapshot.Revision;
		if (!FHyperAIStudioMaterialsContracts::IsCanonicalSha256(Snapshot.Revision))
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
			Record.bGraphTruncated = true;
		}
		OutStatus = Record.bRevisionComplete ? TEXT("captured") : TEXT("capture_incomplete");
		OutDiagnostic = Record.bRevisionComplete
			? TEXT("Exact loaded persisted semantic graph and full CAS evidence captured without loading another asset.")
			: TEXT("The exact graph, disk-state, supported-node, or compile evidence is incomplete, or a finite bound was reached.");
		return true;
	}

	bool TryCanonicalRecord(
		const FHyperAIMaterialAssetRecord& Record,
		const double Deadline,
		FString& Canonical)
	{
		Canonical.Reset();
		int64 Utf8Bytes = 0;
		auto Add = [&](const FString& Value)
		{
			return TryAppendBoundedCanonicalToken(Canonical, Utf8Bytes, Value, Deadline);
		};
		if (!Add(TEXT("hyperai.material.snapshot.v2"))
			|| !Add(Record.Family) || !Add(Record.AssetPath) || !Add(Record.ClassPath)
			|| !Add(Record.OuterPath) || !Add(Record.OutermostPackageName)
			|| !Add(BoolToken(Record.bRootOwnershipComplete)) || !Add(Record.StateId)
			|| !Add(BoolToken(Record.bLoaded)) || !Add(Record.DiskExistence)
			|| !Add(BoolToken(Record.bExistsOnDisk)) || !Add(BoolToken(Record.bPackageDirty))
			|| !Add(BoolToken(Record.bCompileStateKnown)) || !Add(BoolToken(Record.bCompiling))
			|| !Add(BoolToken(Record.bCompileError))
			|| !Add(FString::Printf(TEXT("%lld"), Record.DiskSize))
			|| !Add(Record.ReferenceEvidence) || !Add(Record.ExpressionExecBeginStableId)
			|| !Add(Record.ExpressionExecEndStableId)
			|| !Add(FString::FromInt(Record.OpaqueNodeCount)) || !Add(Record.PackageSavedHash))
			return false;
			if (!Add(TEXT("nodes")) || !Add(FString::FromInt(Record.Nodes.Num()))
				|| !TryAppendSortedElementHashes(Canonical, Utf8Bytes, Record.Nodes,
				[](const FHyperAIMaterialNodeView& Node) { return CanonicalNode(Node); }, Deadline)
				|| !Add(TEXT("edges")) || !Add(FString::FromInt(Record.Edges.Num()))
				|| !TryAppendSortedElementHashes(Canonical, Utf8Bytes, Record.Edges,
					[](const FHyperAIMaterialEdgeView& Edge) { return CanonicalEdge(Edge); }, Deadline)
				|| !Add(TEXT("property_inputs"))
				|| !Add(FString::FromInt(Record.PropertyInputs.Num()))
				|| !TryAppendSortedElementHashes(Canonical, Utf8Bytes, Record.PropertyInputs,
				[](const FHyperAIMaterialPropertyInputView& Property)
				{
					return CanonicalPropertyInput(Property);
				}, Deadline)
			|| !Add(FString::FromInt(Record.NodeCount))
			|| !Add(FString::FromInt(Record.EdgeCount))
			|| !Add(BoolToken(Record.bGraphTruncated)))
			return false;
		return IsBeforeDeadline(Deadline);
	}

	bool BuildDiff(
		const FHyperAIMaterialAssetRecord& Before,
		const FHyperAIMaterialAssetRecord& After,
		TArray<FHyperAIMaterialDiffEntry>& OutDiff,
		bool& bOutTruncated,
		const double Deadline)
	{
		TMap<FString, FString> BeforeNodes;
		TMap<FString, FString> AfterNodes;
		for (const FHyperAIMaterialNodeView& Node : Before.Nodes)
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			BeforeNodes.Add(Node.StableId, CanonicalNode(Node));
		}
		for (const FHyperAIMaterialNodeView& Node : After.Nodes)
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			AfterNodes.Add(Node.StableId, CanonicalNode(Node));
		}
		TSet<FString> NodeIds;
		for (const auto& Pair : BeforeNodes) NodeIds.Add(Pair.Key);
		for (const auto& Pair : AfterNodes) NodeIds.Add(Pair.Key);
		TArray<FString> SortedIds = NodeIds.Array(); SortedIds.Sort();
		auto Add = [&](const FString& Kind, const FString& StableId, const FString& A, const FString& B)
		{
			if (OutDiff.Num() >= FHyperAIStudioMaterialsContracts::MaxIssues)
			{
				bOutTruncated = true;
				return;
			}
			FHyperAIMaterialDiffEntry& Entry = OutDiff.AddDefaulted_GetRef();
			Entry.Kind = Kind; Entry.StableId = StableId; Entry.Before = Clip(A, 1024); Entry.After = Clip(B, 1024);
		};
		for (const FString& Id : SortedIds)
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			const FString* A = BeforeNodes.Find(Id); const FString* B = AfterNodes.Find(Id);
			if (!A) Add(TEXT("node_added"), Id, FString(), *B);
			else if (!B) Add(TEXT("node_removed"), Id, *A, FString());
			else if (*A != *B) Add(TEXT("node_changed"), Id, *A, *B);
		}
		TSet<FString> BeforeEdges; TSet<FString> AfterEdges;
		for (const FHyperAIMaterialEdgeView& Edge : Before.Edges)
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			BeforeEdges.Add(CanonicalEdge(Edge));
		}
		for (const FHyperAIMaterialEdgeView& Edge : After.Edges)
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			AfterEdges.Add(CanonicalEdge(Edge));
		}
		TArray<FString> Removed = BeforeEdges.Difference(AfterEdges).Array(); Removed.Sort();
		TArray<FString> Added = AfterEdges.Difference(BeforeEdges).Array(); Added.Sort();
		for (const FString& Edge : Removed) Add(TEXT("edge_removed"), FString(), Edge, FString());
		for (const FString& Edge : Added) Add(TEXT("edge_added"), FString(), FString(), Edge);
		return FPlatformTime::Seconds() < Deadline;
	}

	bool TryEstimateInspectBytes(
		const FHyperAIMaterialInspectReport& Report,
		const double Deadline,
		int64& OutBytes)
	{
		// Conservative JSON envelope: six-character control escaping plus
		// reflected field names, punctuation, and container overhead.
		constexpr int64 JsonExpansion = 12;
		auto StringBytes = [](const FString& Value)
		{
			return 128ll + JsonExpansion * static_cast<int64>(Value.Len());
		};
		OutBytes = 8192ll + StringBytes(Report.Status) + StringBytes(Report.Diagnostic)
			+ StringBytes(Report.Scope) + StringBytes(Report.Revision)
			+ StringBytes(Report.NextCursor);
		for (const FHyperAIMaterialDiffEntry& Diff : Report.Diff)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += 1024ll + JsonExpansion * static_cast<int64>(
				Diff.Kind.Len() + Diff.StableId.Len() + Diff.Before.Len() + Diff.After.Len());
		}
		for (const FHyperAIMaterialIssue& Issue : Report.Issues)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += 512ll + StringBytes(Issue.Code) + StringBytes(Issue.Severity)
				+ StringBytes(Issue.AssetPath) + StringBytes(Issue.StableId)
				+ StringBytes(Issue.Message);
		}
		for (const FHyperAIMaterialCapabilityStatus& Capability : Report.Capabilities)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += 1024ll + StringBytes(Capability.Family) + StringBytes(Capability.State)
				+ StringBytes(Capability.Remediation);
			for (const TArray<FString>* Values : {
				&Capability.SupportedCases, &Capability.DelegatedEpicCases,
				&Capability.UnsupportedCases})
				for (const FString& Value : *Values) OutBytes += StringBytes(Value);
		}
		auto AddRecord = [&](const FHyperAIMaterialAssetRecord& Record)
		{
			OutBytes += 2048ll;
			for (const FString* Value : {
				&Record.Family, &Record.AssetPath, &Record.ClassPath, &Record.OuterPath,
				&Record.OutermostPackageName, &Record.StateId, &Record.Revision,
				&Record.DiskExistence, &Record.ReferenceEvidence,
				&Record.ExpressionExecBeginStableId, &Record.ExpressionExecEndStableId})
				OutBytes += StringBytes(*Value);
			for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
			{
				if (FPlatformTime::Seconds() >= Deadline) return false;
				OutBytes += 2048ll + static_cast<int64>(CanonicalNode(Node).Len())
					* JsonExpansion;
			}
			for (const FHyperAIMaterialEdgeView& Edge : Record.Edges)
			{
				if (FPlatformTime::Seconds() >= Deadline) return false;
				OutBytes += 512ll + static_cast<int64>(CanonicalEdge(Edge).Len())
					* JsonExpansion;
			}
			for (const FHyperAIMaterialPropertyInputView& Property : Record.PropertyInputs)
			{
				if (FPlatformTime::Seconds() >= Deadline) return false;
				OutBytes += 768ll + static_cast<int64>(CanonicalPropertyInput(Property).Len())
					* JsonExpansion;
			}
			return true;
		};
		return IsBeforeDeadline(Deadline)
			&& AddRecord(Report.Target) && AddRecord(Report.Compare);
	}

	bool EnforceInspectOutputBound(
		FHyperAIMaterialInspectReport& Report,
		const int32 MaxBytes,
		const double Deadline)
	{
		constexpr int64 JsonExpansion = 12;
		int64 Bytes = 0;
		if (!TryEstimateInspectBytes(Report, Deadline, Bytes)) return false;
		while (Bytes > MaxBytes && !Report.Diff.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			const FHyperAIMaterialDiffEntry& Diff = Report.Diff.Last();
			Bytes -= 1024ll + JsonExpansion * static_cast<int64>(Diff.Kind.Len()
				+ Diff.StableId.Len() + Diff.Before.Len() + Diff.After.Len());
			Report.Diff.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Compare.Edges.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 512ll + static_cast<int64>(CanonicalEdge(Report.Compare.Edges.Last()).Len())
				* JsonExpansion;
			Report.Compare.Edges.Pop(EAllowShrinking::No); Report.Compare.bGraphTruncated = true; Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Target.Edges.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 512ll + static_cast<int64>(CanonicalEdge(Report.Target.Edges.Last()).Len())
				* JsonExpansion;
			Report.Target.Edges.Pop(EAllowShrinking::No); Report.Target.bGraphTruncated = true; Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Compare.PropertyInputs.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 768ll + static_cast<int64>(CanonicalPropertyInput(Report.Compare.PropertyInputs.Last()).Len())
				* JsonExpansion;
			Report.Compare.PropertyInputs.Pop(EAllowShrinking::No); Report.Compare.bGraphTruncated = true; Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Target.PropertyInputs.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 768ll + static_cast<int64>(CanonicalPropertyInput(Report.Target.PropertyInputs.Last()).Len())
				* JsonExpansion;
			Report.Target.PropertyInputs.Pop(EAllowShrinking::No); Report.Target.bGraphTruncated = true; Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Compare.Nodes.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 2048ll + static_cast<int64>(CanonicalNode(Report.Compare.Nodes.Last()).Len())
				* JsonExpansion;
			Report.Compare.Nodes.Pop(EAllowShrinking::No); Report.Compare.bGraphTruncated = true; Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Target.Nodes.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= 2048ll + static_cast<int64>(CanonicalNode(Report.Target.Nodes.Last()).Len())
				* JsonExpansion;
			Report.Target.Nodes.Pop(EAllowShrinking::No); Report.Target.bGraphTruncated = true; Report.bTruncated = true;
		}
		return Bytes <= MaxBytes;
	}

	FHyperAIStudioDomainAdapterResult Failure(
		const EHyperAIStudioDomainDispatchOutcome Outcome,
		const FString& Code,
		const FString& Diagnostic = FString())
	{
		FHyperAIStudioDomainAdapterResult Result;
		Result.Outcome = Outcome;
		Result.StatusCode = Code;
		Result.Diagnostic = Clip(Diagnostic);
		return Result;
	}

}

FString FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioMaterials.HyperAIStudioMaterialsToolset");
}

const TArray<FHyperAIStudioMaterialManifestEntry>& FHyperAIStudioMaterialsContracts::GetManifest()
{
	static const TArray<FHyperAIStudioMaterialManifestEntry> Manifest = {
		{TEXT("hyper_material_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_material_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_material_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioMaterialsContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioMaterialsContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	TArray<FString> Names;
	for (const FHyperAIStudioMaterialManifestEntry& Entry : GetManifest()) Names.Add(Entry.Name);
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioMaterialsContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT(":")) || Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")))
		return false;
	for (int32 Index = 0; Index < Path.Len(); ++Index) if (Path[Index] == TEXT('\0')) return false;
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioMaterialsContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioMaterialsContracts::IsExactLoadedFamily(
	const UObject* Object, const FString& Family)
{
	if (!Object) return false;
	if (Family == TEXT("material")) return Object->GetClass() == UMaterial::StaticClass();
	if (Family == TEXT("material_function")) return Object->GetClass() == UMaterialFunction::StaticClass();
	return false;
}

FString FHyperAIStudioMaterialsContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	if (State == UE::AssetRegistry::EExists::Exists) return TEXT("exists");
	if (State == UE::AssetRegistry::EExists::DoesNotExist) return TEXT("does_not_exist");
	return TEXT("unknown");
}

bool FHyperAIStudioMaterialsContracts::IsCreateAbsenceProven(
	const bool bLoadedObjectPresent,
	const UE::AssetRegistry::EExists State)
{
	return !bLoadedObjectPresent && State == UE::AssetRegistry::EExists::DoesNotExist;
}

bool FHyperAIStudioMaterialsContracts::IsExpressionCollectionWithinBound(
	const int32 Count,
	const int32 RequestedMaxNodes)
{
	return Count >= 0 && RequestedMaxNodes >= 1 && RequestedMaxNodes <= MaxNodes
		&& Count <= RequestedMaxNodes;
}

bool FHyperAIStudioMaterialsContracts::IsBoundedSimpleSemanticText(const FText& Text)
{
	FString Ignored;
	return HyperAIStudio::Materials::Private::CaptureSemanticText(Text, Ignored);
}

TArray<FHyperAIMaterialCapabilityStatus> FHyperAIStudioMaterialsContracts::GetCapabilityMatrix()
{
	FHyperAIMaterialCapabilityStatus Material;
	Material.Family = TEXT("material");
	Material.bCompoundBackendImplemented = true;
	Material.SupportedCases = {
		TEXT("exact_loaded_persisted_semantic_snapshot"),
		TEXT("bounded_exact_on_disk_identity_without_load"),
		TEXT("semantic_graph_diff_by_expression_guid"),
		TEXT("independent_guid_edge_cycle_reachability_validation"),
		TEXT("node_catalog_graph_authoring"),
		TEXT("create_material_and_edit_graph_at_revision"),
		TEXT("material_instances_and_parameters"),
		TEXT("authoring_mode_nodes_hlsl_hybrid"),
		TEXT("compile_statistics_and_preview_png"),
		TEXT("journaled_apply_compile_validate_save_fresh_verify"),
		TEXT("pure_repair_shadow_evidence")};
	Material.DelegatedEpicCases = {
		TEXT("create_material"), TEXT("list_expression_classes"), TEXT("add_expression"),
		TEXT("delete_expression"), TEXT("get_expressions"), TEXT("layout_expressions"),
		TEXT("get_expression_input_names"), TEXT("get_expression_output_names"),
		TEXT("connect_expressions"), TEXT("disconnect_expressions"),
		TEXT("get_expression_inputs"), TEXT("get_property_input"), TEXT("connect_to_output"),
		TEXT("disconnect_from_output"), TEXT("delete_unused_expressions"), TEXT("recompile"),
		TEXT("get_referencing_materials"), TEXT("hyper_asset_dependency_graph"),
		TEXT("material_instance_parameter_and_parent_tools"),
		TEXT("mesh_material_assignment_tools"), TEXT("texture_import_and_size_tools")};
	Material.UnsupportedCases = {
		TEXT("arbitrary_expression_class_or_property_dispatch"),
		TEXT("raw_script_or_reflection_dispatch"),
		TEXT("synchronous_asset_load"),
		TEXT("unbounded_asset_registry_scan"),
		TEXT("standalone_add_connect_delete_or_recompile_duplicates"),
		TEXT("repair_semantic_graph_execution"),
		TEXT("expression_classes_outside_the_node_catalog_created_or_configured"),
		TEXT("target_open_in_material_editor")};
	Material.State = TEXT("source_candidate_executable");
	Material.Remediation = TEXT("Inspect for the revision and node ids, dry-run apply_plan, resubmit with operation_id and expected_plan_hash, poll hyper_operation_status, then hyper_material_validate for stats and the preview PNG. Close the material's editor tab first.");

	FHyperAIMaterialCapabilityStatus Function;
	Function.Family = TEXT("material_function");
	Function.bCompoundBackendImplemented = false;
	Function.SupportedCases = {
		TEXT("exact_loaded_persisted_semantic_snapshot"),
		TEXT("bounded_exact_on_disk_identity_without_load"),
		TEXT("semantic_graph_diff_by_expression_guid"),
		TEXT("independent_guid_edge_cycle_reachability_validation")};
	Function.DelegatedEpicCases = {
		TEXT("create_function"), TEXT("add_expression"), TEXT("delete_expression"),
		TEXT("connect_expressions"), TEXT("disconnect_expressions"),
		TEXT("layout_expressions"), TEXT("recompile_or_update_function"),
		TEXT("hyper_asset_dependency_graph")};
	Function.UnsupportedCases = {
		TEXT("mutation_without_public_persisted_compile_error_state"),
		TEXT("material_layer_or_layer_blend_function_authoring"),
		TEXT("arbitrary_expression_class_or_property_dispatch")};
	Function.State = TEXT("inspect_validate_only_compile_evidence_gap");
	Function.Remediation = TEXT("Use Epic Material tools for authoring; HyperAI will not claim complete function CAS until UE exposes compile/error state.");
	return {Material, Function};
}

FString FHyperAIStudioMaterialsContracts::PayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.payload.v2|closed_operation|exact_family|target|cas|nodes|properties|edges|outputs|repairs|removes|disconnects|edits|settings|instances|authoring_mode|deep_clone|bounded"));
	return Fingerprint;
}

FString FHyperAIStudioMaterialsContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.inspect.payload.v1|exact_target|family|scope|compare|page|bounds"));
	return Fingerprint;
}

FString FHyperAIStudioMaterialsContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.validate.payload.v1|exact_target|family|expected_revision|clean|bounds"));
	return Fingerprint;
}

FString FHyperAIStudioMaterialsContracts::InspectResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.inspect.result.v1|revision|record|nodes|properties|edges|diff|issues|bounded"));
	return Fingerprint;
}

FString FHyperAIStudioMaterialsContracts::ValidateResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.validate.result.v1|valid|revision|issues|stats|preview|custom_nodes|bounded"));
	return Fingerprint;
}

FString FHyperAIStudioMaterialsContracts::GetAuthoringModeName()
{
	switch (GetDefault<UHyperAIStudioSettings>()->MaterialAuthoringMode)
	{
	case EHyperAIStudioMaterialAuthoringMode::Nodes: return TEXT("nodes");
	case EHyperAIStudioMaterialAuthoringMode::Hlsl: return TEXT("hlsl");
	default: return TEXT("hybrid");
	}
}

FString FHyperAIStudioMaterialsContracts::ResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("material.result.v2|phase|content_key|valid|error_count|bounded"));
	return Fingerprint;
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioMaterialsContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.material.persisted_semantic_graph.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), ResultTypeId, ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({ValidateToolName, ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioMaterialsContracts::ValidateOperationShape(
	const FHyperAIMaterialPlanOperation& Operation,
	FHyperAIStudioMaterialBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError,
	const double Deadline)
{
	using namespace HyperAIStudio::Materials::Private;
	OutOperation = {};
	OutErrorCode.Reset(); OutError.Reset();
	auto Fail = [&](const TCHAR* Code, const TCHAR* Message)
	{
		OutErrorCode = Code; OutError = Message; return false;
	};
	if (!IsBeforeDeadline(Deadline))
		return Fail(TEXT("planning_deadline_exceeded"), TEXT("Operation validation started after its caller-owned deadline."));
	if (Operation.Nodes.Num() > MaxNodes || Operation.Edges.Num() > MaxEdges
		|| Operation.Outputs.Num() > 16 || Operation.RepairKinds.Num() > 3
		|| static_cast<int64>(Operation.Edges.Num()) + Operation.Outputs.Num() > MaxEdges
		|| Operation.RemoveNodeIds.Num() > MaxNodes || Operation.Disconnects.Num() > MaxEdges
		|| Operation.PropertyEdits.Num() > MaxNodes || Operation.MaterialSettings.Num() > 8
		|| Operation.Parameters.Num() > MaxParametersPerPlan)
		return Fail(TEXT("operation_collection_bound_exceeded"), TEXT("Operation collections exceed the closed pre-copy bounds."));
	int64 MaterializedBytes = 1024;
	auto ConsumeField = [&](const FString& Value)
	{
		if (!IsBeforeDeadline(Deadline)) return false;
		const int64 Delta = 128ll + static_cast<int64>(Value.Len()) * MaxUtf8BytesPerCharacter;
		if (Delta > MaxSnapshotMaterializedBytes
			|| MaterializedBytes > MaxSnapshotMaterializedBytes - Delta)
			return false;
		MaterializedBytes += Delta;
		return true;
	};
	if (!ConsumeField(Operation.Type) || !ConsumeField(Operation.TargetPath)
		|| !ConsumeField(Operation.TargetFamily) || !ConsumeField(Operation.ExpectedRevision))
		return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Operation strings exceed the closed pre-copy byte or deadline bound."));
	for (const FHyperAIMaterialNodeSpec& Node : Operation.Nodes)
		if (!ConsumeField(Node.Kind) || !ConsumeField(Node.NodeId)
			|| !ConsumeField(Node.Name) || !ConsumeField(Node.Group))
			return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Node strings exceed the closed pre-copy byte or deadline bound."));
	for (const FHyperAIMaterialEdgeSpec& Edge : Operation.Edges)
		if (!ConsumeField(Edge.FromNodeId) || !ConsumeField(Edge.FromOutput)
			|| !ConsumeField(Edge.ToNodeId) || !ConsumeField(Edge.ToInput))
			return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Edge strings exceed the closed pre-copy byte or deadline bound."));
	for (const FHyperAIMaterialOutputSpec& Output : Operation.Outputs)
		if (!ConsumeField(Output.Property) || !ConsumeField(Output.FromNodeId)
			|| !ConsumeField(Output.FromOutput))
			return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Output strings exceed the closed pre-copy byte or deadline bound."));
	for (const FString& Repair : Operation.RepairKinds)
		if (!ConsumeField(Repair))
			return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Repair strings exceed the closed pre-copy byte or deadline bound."));
	bool bNewFieldsFit = ConsumeField(Operation.ParentPath);
	for (const FHyperAIMaterialNodeSpec& Node : Operation.Nodes)
	{
		bNewFieldsFit &= Node.Properties.Num() <= 16 && ConsumeField(Node.Justification);
		for (const FHyperAIMaterialNodeProperty& Property : Node.Properties)
			bNewFieldsFit &= ConsumeField(Property.Key) && ConsumeField(Property.Value);
	}
	for (const FString& Id : Operation.RemoveNodeIds) bNewFieldsFit &= ConsumeField(Id);
	for (const FHyperAIMaterialEdgeSpec& Cut : Operation.Disconnects)
		bNewFieldsFit &= ConsumeField(Cut.ToNodeId) && ConsumeField(Cut.ToInput);
	for (const FHyperAIMaterialPropertyEdit& Edit : Operation.PropertyEdits)
		bNewFieldsFit &= ConsumeField(Edit.NodeId) && ConsumeField(Edit.Key) && ConsumeField(Edit.Value);
	for (const FHyperAIMaterialNodeProperty& Setting : Operation.MaterialSettings)
		bNewFieldsFit &= ConsumeField(Setting.Key) && ConsumeField(Setting.Value);
	for (const FHyperAIMaterialParameterValue& Parameter : Operation.Parameters)
		bNewFieldsFit &= ConsumeField(Parameter.Name) && ConsumeField(Parameter.Type) && ConsumeField(Parameter.Value);
	if (!bNewFieldsFit)
		return Fail(TEXT("operation_materialized_bound_exceeded"), TEXT("Operation strings exceed the closed pre-copy byte or deadline bound."));
	if (!IsCanonicalProjectObjectPath(Operation.TargetPath))
		return Fail(TEXT("invalid_target_path"), TEXT("Target must be one canonical /Game object path."));
	if (!IsAllowedFamily(Operation.TargetFamily))
		return Fail(TEXT("invalid_target_family"), TEXT("target_family must be material or material_function."));
	if (Operation.TargetFamily != TEXT("material"))
		return Fail(TEXT("function_mutation_compile_evidence_gap"), TEXT("Material functions are inspect/validate only because UE 5.8 exposes no complete persisted compile-error CAS."));
	if (Operation.Type == TEXT("compound_create_configure_graph"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph;
	else if (Operation.Type == TEXT("repair_semantic_graph"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::RepairSemanticGraph;
	else if (Operation.Type == TEXT("create_material"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::CreateMaterial;
	else if (Operation.Type == TEXT("edit_graph"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::EditGraph;
	else if (Operation.Type == TEXT("create_material_instance"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::CreateMaterialInstance;
	else if (Operation.Type == TEXT("set_instance_parameters"))
		OutOperation.Kind = EHyperAIStudioMaterialOperationKind::SetInstanceParameters;
	else return Fail(TEXT("unsupported_operation"), TEXT("type must be create_material, edit_graph, create_material_instance, set_instance_parameters, compound_create_configure_graph or repair_semantic_graph."));

	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.TargetFamily = Operation.TargetFamily;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.Nodes = Operation.Nodes;
	OutOperation.Edges = Operation.Edges;
	OutOperation.Outputs = Operation.Outputs;
	OutOperation.RepairKinds = Operation.RepairKinds;
	OutOperation.RemoveNodeIds = Operation.RemoveNodeIds;
	OutOperation.Disconnects = Operation.Disconnects;
	OutOperation.PropertyEdits = Operation.PropertyEdits;
	OutOperation.MaterialSettings = Operation.MaterialSettings;
	OutOperation.ParentPath = Operation.ParentPath;
	OutOperation.Parameters = Operation.Parameters;
	if (!IsBeforeDeadline(Deadline))
		return Fail(TEXT("planning_deadline_exceeded"), TEXT("Operation copy exhausted its caller-owned deadline."));

	const bool bNodeCatalogFieldsUsed = !Operation.RemoveNodeIds.IsEmpty() || !Operation.Disconnects.IsEmpty()
		|| !Operation.PropertyEdits.IsEmpty() || !Operation.MaterialSettings.IsEmpty()
		|| Operation.Nodes.ContainsByPredicate([](const FHyperAIMaterialNodeSpec& Node)
		{
			return !Node.Properties.IsEmpty() || !Node.Justification.IsEmpty();
		});
	const bool bInstanceFieldsUsed = !Operation.ParentPath.IsEmpty() || !Operation.Parameters.IsEmpty();
	using EKind = EHyperAIStudioMaterialOperationKind;
	if ((OutOperation.Kind == EKind::CompoundCreateConfigureGraph || OutOperation.Kind == EKind::RepairSemanticGraph)
		&& (bNodeCatalogFieldsUsed || bInstanceFieldsUsed))
		return Fail(TEXT("unused_operation_field"), TEXT("Node properties, edits, settings and instance fields belong to create_material, edit_graph and the instance operations."));
	if (OutOperation.Kind == EKind::CreateMaterial || OutOperation.Kind == EKind::EditGraph)
	{
		if (bInstanceFieldsUsed || !Operation.RepairKinds.IsEmpty())
			return Fail(TEXT("unused_operation_field"), TEXT("Graph operations take no parent, parameters or repair kinds."));
		if (OutOperation.Kind == EKind::CreateMaterial
			&& (!Operation.ExpectedRevision.IsEmpty() || Operation.Nodes.IsEmpty() || Operation.Outputs.IsEmpty()
				|| !Operation.RemoveNodeIds.IsEmpty() || !Operation.Disconnects.IsEmpty()))
			return Fail(TEXT("create_shape_invalid"), TEXT("create_material needs nodes and outputs, no expected_revision, and nothing to remove or disconnect."));
		if (OutOperation.Kind == EKind::EditGraph && !IsCanonicalSha256(Operation.ExpectedRevision))
			return Fail(TEXT("invalid_expected_revision"), TEXT("edit_graph needs the revision hyper_material_inspect reported."));
		if (OutOperation.Kind == EKind::EditGraph && Operation.Nodes.IsEmpty() && Operation.Edges.IsEmpty()
			&& Operation.Outputs.IsEmpty() && Operation.RemoveNodeIds.IsEmpty() && Operation.Disconnects.IsEmpty()
			&& Operation.PropertyEdits.IsEmpty() && Operation.MaterialSettings.IsEmpty())
			return Fail(TEXT("edit_graph_empty"), TEXT("edit_graph changes nothing."));
		for (const FHyperAIMaterialNodeSpec& Node : Operation.Nodes)
		{
			if (!IsSimpleIdentifier(Node.NodeId) || !Node.Name.IsEmpty() || !Node.Group.IsEmpty()
				|| Node.Scalar != 0.0 || !Node.Vector.Equals(FLinearColor::Black) || !IsFiniteNode(Node)
				|| Node.Justification.Len() > 256)
				return Fail(TEXT("invalid_node_shape"), TEXT("New nodes need a simple node_id and set values through properties; the legacy name/group/scalar/vector fields are for compound_create_configure_graph."));
		}
		return true;
	}
	if (OutOperation.Kind == EKind::CreateMaterialInstance || OutOperation.Kind == EKind::SetInstanceParameters)
	{
		if (!Operation.ExpectedRevision.IsEmpty() || bNodeCatalogFieldsUsed || !Operation.Nodes.IsEmpty()
			|| !Operation.Edges.IsEmpty() || !Operation.Outputs.IsEmpty() || !Operation.RepairKinds.IsEmpty())
			return Fail(TEXT("instance_shape_invalid"), TEXT("Instance operations take only parameters (and parent_path to create); expected_revision stays empty because parameters are set, not merged."));
		if (OutOperation.Kind == EKind::CreateMaterialInstance && !IsCanonicalProjectObjectPath(Operation.ParentPath)
			&& !(Operation.ParentPath.StartsWith(TEXT("/")) && FPackageName::IsValidObjectPath(Operation.ParentPath)))
			return Fail(TEXT("invalid_parent_path"), TEXT("create_material_instance needs parent_path, an exact material or instance object path."));
		if (OutOperation.Kind == EKind::SetInstanceParameters && Operation.Parameters.IsEmpty())
			return Fail(TEXT("instance_shape_invalid"), TEXT("set_instance_parameters needs at least one parameter."));
		TSet<FString> Names;
		for (const FHyperAIMaterialParameterValue& Parameter : Operation.Parameters)
		{
			const FString Name = Parameter.Name.TrimStartAndEnd();
			if (Name.IsEmpty() || Name.Len() > MaxNameCharacters || !AddUnique(Names, Name.ToLower())
				|| !(Parameter.Type == TEXT("scalar") || Parameter.Type == TEXT("vector")
					|| Parameter.Type == TEXT("texture") || Parameter.Type == TEXT("static_switch")))
				return Fail(TEXT("invalid_parameter_shape"), TEXT("Parameters need a unique name and type scalar, vector, texture or static_switch."));
		}
		return true;
	}

		if (OutOperation.Kind == EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph)
		{
		if (!Operation.ExpectedRevision.IsEmpty())
			return Fail(TEXT("create_revision_must_be_empty"), TEXT("Create binds proven absence; expected_revision must be empty."));
			if (Operation.Nodes.IsEmpty() || Operation.Nodes.Num() > MaxNodes
				|| Operation.Edges.Num() + Operation.Outputs.Num() > MaxEdges
				|| Operation.Outputs.IsEmpty()
			|| Operation.Outputs.Num() > 16 || !Operation.RepairKinds.IsEmpty())
			return Fail(TEXT("compound_graph_bounds_invalid"), TEXT("Compound create requires bounded nodes and outputs, with no repair fields."));
		TSet<FString> NodeIds;
		TMap<FString, FString> NodeKinds;
		for (FHyperAIMaterialNodeSpec& Node : OutOperation.Nodes)
		{
			if (!IsBeforeDeadline(Deadline))
				return Fail(TEXT("planning_deadline_exceeded"), TEXT("Node validation exhausted its caller-owned deadline."));
			if (!IsSimpleIdentifier(Node.NodeId) || !IsAllowedNodeKind(Node.Kind, Operation.TargetFamily)
				|| !IsFiniteNode(Node) || Node.Name.Len() > MaxNameCharacters
				|| Node.Group.Len() > MaxNameCharacters)
				return Fail(TEXT("invalid_node_shape"), TEXT("A node uses an unknown kind, invalid identifier, non-finite value, or oversized name."));
			if (!AddUnique(NodeIds, Node.NodeId)) return Fail(TEXT("duplicate_node_id"), TEXT("node_id values must be unique."));
			NodeKinds.Add(Node.NodeId, Node.Kind);
			const bool bParameter = Node.Kind == TEXT("scalar_parameter") || Node.Kind == TEXT("vector_parameter");
			if (bParameter && !IsSimpleIdentifier(Node.Name))
				return Fail(TEXT("invalid_parameter_name"), TEXT("Parameter nodes require one closed simple name."));
			if (!bParameter && (!Node.Name.IsEmpty() || !Node.Group.IsEmpty()))
				return Fail(TEXT("unused_node_field"), TEXT("name/group are accepted only for parameter nodes in the Material fast path."));
			const bool bScalarBacked = Node.Kind == TEXT("constant") || Node.Kind == TEXT("scalar_parameter");
			if (bScalarBacked)
			{
				const float Persisted = static_cast<float>(Node.Scalar);
				if (!FMath::IsFinite(Persisted))
					return Fail(TEXT("node_float_overflow"), TEXT("Scalar must round to a finite persisted float."));
				Node.Scalar = static_cast<double>(Persisted);
			}
			else if (Node.Scalar != 0.0)
				return Fail(TEXT("unused_node_field"), TEXT("scalar is accepted only for constant/scalar-parameter nodes."));
			if (Node.Kind != TEXT("vector_parameter") && !Node.Vector.Equals(FLinearColor::Black))
				return Fail(TEXT("unused_node_field"), TEXT("vector is accepted only for vector-parameter nodes."));
		}
		for (const FHyperAIMaterialEdgeSpec& Edge : OutOperation.Edges)
		{
			if (!IsBeforeDeadline(Deadline))
				return Fail(TEXT("planning_deadline_exceeded"), TEXT("Edge validation exhausted its caller-owned deadline."));
			if (!IsSimpleIdentifier(Edge.FromNodeId) || !IsSimpleIdentifier(Edge.ToNodeId)
				|| Edge.FromOutput.Len() > MaxNameCharacters || Edge.ToInput.IsEmpty()
				|| Edge.ToInput.Len() > MaxNameCharacters)
				return Fail(TEXT("invalid_edge_shape"), TEXT("Edges require bounded plan-local endpoints and one explicit target input."));
			const FString* FromKind = NodeKinds.Find(Edge.FromNodeId);
			const FString* ToKind = NodeKinds.Find(Edge.ToNodeId);
			int32 FromIndex = INDEX_NONE, ToIndex = INDEX_NONE;
			EClosedMaterialValueType SourceType;
			if (!FromKind || !ToKind)
				return Fail(TEXT("edge_endpoint_missing"), TEXT("Every edge endpoint must name a node in this operation."));
			if (!TryExpectedSourceOutputIndex(*FromKind, Edge.FromOutput, FromIndex)
				|| !TryExpectedTargetInputIndex(*ToKind, Edge.ToInput, ToIndex))
				return Fail(TEXT("closed_pin_not_found"), TEXT("The source output or target input is outside the exact closed UE 5.8 per-kind port schema."));
			if (!TryClosedOutputType(*FromKind, Edge.FromOutput, SourceType)
				|| SourceType != EClosedMaterialValueType::Scalar)
				return Fail(TEXT("closed_pin_type_mismatch"), TEXT("The bounded add/multiply fast path accepts scalar inputs only."));
		}
		TSet<FString> OutputProperties;
		for (const FHyperAIMaterialOutputSpec& Output : OutOperation.Outputs)
		{
			if (!IsBeforeDeadline(Deadline))
				return Fail(TEXT("planning_deadline_exceeded"), TEXT("Output validation exhausted its caller-owned deadline."));
			if (!IsAllowedOutputProperty(Output.Property) || !IsSimpleIdentifier(Output.FromNodeId)
				|| Output.FromOutput.Len() > MaxNameCharacters || !AddUnique(OutputProperties, Output.Property))
				return Fail(TEXT("invalid_output_shape"), TEXT("Material outputs use a unique closed property and a bounded plan-local source."));
			const FString* FromKind = NodeKinds.Find(Output.FromNodeId);
			EClosedMaterialValueType SourceType;
			if (!FromKind || !TryClosedOutputType(*FromKind, Output.FromOutput, SourceType))
				return Fail(TEXT("closed_output_pin_not_found"), TEXT("Material output source is outside the exact closed UE 5.8 port schema."));
			if (!IsClosedMaterialOutputCompatible(Output.Property, SourceType))
				return Fail(TEXT("material_output_type_mismatch"), TEXT("Vector output cannot feed a scalar-only material property in the typed shadow."));
		}
	}
	else
	{
		if (!IsCanonicalSha256(Operation.ExpectedRevision))
			return Fail(TEXT("invalid_expected_revision"), TEXT("Semantic repair requires the exact complete inspector-issued CAS revision."));
		if (!Operation.Nodes.IsEmpty() || !Operation.Edges.IsEmpty() || !Operation.Outputs.IsEmpty()
			|| Operation.RepairKinds.IsEmpty() || Operation.RepairKinds.Num() > 3)
			return Fail(TEXT("repair_shape_invalid"), TEXT("Semantic repair accepts only one non-empty bounded repair-kind set."));
		TSet<FString> Seen;
		for (const FString& Repair : Operation.RepairKinds)
		{
			if (!IsBeforeDeadline(Deadline))
				return Fail(TEXT("planning_deadline_exceeded"), TEXT("Repair validation exhausted its caller-owned deadline."));
			if (!IsAllowedRepair(Repair) || !AddUnique(Seen, Repair))
				return Fail(TEXT("unsupported_or_duplicate_repair"), TEXT("Repair kinds must be unique members of the closed semantic repair vocabulary."));
		}
	}
	return true;
}

bool FHyperAIStudioMaterialsContracts::ValidateGraphOperation(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const UMaterial* Existing,
	const EHyperAIStudioMaterialAuthoringMode Mode,
	int32& InOutCustomNodes,
	int32& InOutCustomBytes,
	TArray<FString>& OutCustomNodes,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Materials::Private;
	namespace Gate = HyperAIStudio::Materials::Gate;
	auto Fail = [&](const TCHAR* Code, const FString& Message)
	{
		OutErrorCode = Code;
		OutError = Message;
		return false;
	};
	TMap<FString, UMaterialExpression*> Nodes;
	TMap<FString, FString> Kinds;
	TSet<FName> ParameterNames;
	auto ParameterNameOf = [](UMaterialExpression* Expression)
	{
		const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression);
		if (Parameter) return Parameter->ParameterName;
		const UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Expression);
		return Texture ? Texture->ParameterName : FName();
	};
	if (Existing)
	{
		for (UMaterialExpression* Expression : const_cast<UMaterial*>(Existing)->GetExpressions())
		{
			if (!Expression) continue;
			const FString Id = Gate::NodeIdOf(*Expression);
			Nodes.Add(Id, Expression);
			Kinds.Add(Id, Gate::KindOf(*Expression));
			if (!ParameterNameOf(Expression).IsNone()) ParameterNames.Add(ParameterNameOf(Expression));
			if (Expression->IsA<UMaterialExpressionCustom>())
				OutCustomNodes.Add(FString::Printf(TEXT("%s: %s (already in the material)"), *Operation.TargetPath, *Id));
		}
	}
	for (const FString& Id : Operation.RemoveNodeIds)
	{
		UMaterialExpression* const* Found = Nodes.Find(Id);
		if (!Found) return Fail(TEXT("node_not_found"), FString::Printf(TEXT("remove: no node %s in the material."), *Id.Left(64)));
		ParameterNames.Remove(ParameterNameOf(*Found));
		Nodes.Remove(Id);
	}
	for (const FHyperAIMaterialEdgeSpec& Cut : Operation.Disconnects)
	{
		if (Cut.ToNodeId == TEXT("$material_output"))
		{
			if (Gate::OutputPropertyFromName(Cut.ToInput) == MP_MAX)
				return Fail(TEXT("invalid_output_shape"), FString::Printf(TEXT("disconnect: '%s' is not a material output."), *Cut.ToInput.Left(64)));
			continue;
		}
		UMaterialExpression* const* Found = Nodes.Find(Cut.ToNodeId);
		if (!Found || !Gate::HasInput(**Found, Cut.ToInput))
			return Fail(TEXT("closed_pin_not_found"), FString::Printf(TEXT("disconnect: %s has no input '%s'."), *Cut.ToNodeId.Left(64), *Cut.ToInput.Left(64)));
	}

	TMap<FString, TArray<FString>> FunctionInputs;
	TMap<FString, TArray<FString>> FunctionOutputs;
	for (const FHyperAIMaterialNodeSpec& Spec : Operation.Nodes)
	{
		if (Nodes.Contains(Spec.NodeId))
			return Fail(TEXT("duplicate_node_id"), FString::Printf(TEXT("node_id %s is already used."), *Spec.NodeId));
		if (!Gate::FindNodeKind(Spec.Kind))
			return Fail(TEXT("invalid_node_shape"), FString::Printf(TEXT("'%s' is not a node kind; see the FHyperAIMaterialNodeSpec kinds."), *Spec.Kind.Left(64)));
		FString Code;
		if (Gate::IsCustomKind(Spec.Kind))
		{
			if (Mode == EHyperAIStudioMaterialAuthoringMode::Nodes)
				return Fail(TEXT("authoring_mode_forbids_custom_hlsl"), TEXT("Materials are set to Nodes only; build this from graph nodes or change the Materials authoring mode in Settings."));
			for (const FHyperAIMaterialNodeProperty& Property : Spec.Properties)
			{
				if (Property.Key == TEXT("code")) Code = Property.Value;
			}
			const bool bHybrid = Mode == EHyperAIStudioMaterialAuthoringMode::Hybrid;
			++InOutCustomNodes;
			InOutCustomBytes += Code.Len();
			if (bHybrid && Spec.Justification.TrimStartAndEnd().IsEmpty())
				return Fail(TEXT("custom_hlsl_justification_required"), TEXT("Hybrid mode: give each custom_hlsl node a justification naming what graph nodes cannot express."));
			if (InOutCustomNodes > (bHybrid ? MaxHybridCustomNodes : MaxHlslCustomNodes)
				|| (bHybrid ? Code.Len() > MaxHybridCustomCodeBytes : InOutCustomBytes > MaxHlslCustomCodeBytes))
				return Fail(TEXT("custom_hlsl_budget_exceeded"), bHybrid
					? FString::Printf(TEXT("Hybrid mode allows %d custom_hlsl nodes per plan of up to %d bytes each."), MaxHybridCustomNodes, MaxHybridCustomCodeBytes)
					: FString::Printf(TEXT("HLSL mode allows %d custom_hlsl nodes and %d bytes of code per plan."), MaxHlslCustomNodes, MaxHlslCustomCodeBytes));
		}
		FString Error;
		UMaterialExpression* Shadow = Gate::MakeShadowNode(Spec, Error);
		if (!Shadow) return Fail(TEXT("invalid_node_property"), FString::Printf(TEXT("%s: %s"), *Spec.NodeId, *Error));
		if (Gate::IsCustomKind(Spec.Kind))
		{
			TArray<FString> InputNames;
			for (const FCustomInput& Input : CastChecked<UMaterialExpressionCustom>(Shadow)->Inputs) InputNames.Add(Input.InputName.ToString());
			if (Mode == EHyperAIStudioMaterialAuthoringMode::Hybrid && Gate::IsExpressibleWithNodes(Code, InputNames))
				return Fail(TEXT("custom_hlsl_expressible_with_nodes"), FString::Printf(TEXT("%s is a single expression the graph already has nodes for; use those nodes so artists can read and tweak it."), *Spec.NodeId));
			OutCustomNodes.Add(FString::Printf(TEXT("%s: %s (%s)"), *Operation.TargetPath, *Spec.NodeId,
				Spec.Justification.IsEmpty() ? TEXT("HLSL mode") : *Spec.Justification.Left(256)));
		}
		const FName ParameterName = ParameterNameOf(Shadow);
		if (Gate::IsParameterKind(Spec.Kind))
		{
			bool bDuplicate = false;
			ParameterNames.Add(ParameterName, &bDuplicate);
			if (ParameterName.IsNone() || bDuplicate)
				return Fail(TEXT("duplicate_parameter_name"), FString::Printf(TEXT("%s needs a name property unique in the material."), *Spec.NodeId));
		}
		if (Spec.Kind == TEXT("function_call"))
		{
			FString FunctionPath;
			for (const FHyperAIMaterialNodeProperty& Property : Spec.Properties)
			{
				if (Property.Key == TEXT("function")) FunctionPath = Property.Value;
			}
			if (!Gate::GetFunctionPins(FunctionPath, FunctionInputs.Add(Spec.NodeId), FunctionOutputs.Add(Spec.NodeId), Error))
				return Fail(TEXT("invalid_node_property"), FString::Printf(TEXT("%s needs a function property: %s"), *Spec.NodeId, *Error));
		}
		Nodes.Add(Spec.NodeId, Shadow);
		Kinds.Add(Spec.NodeId, Spec.Kind);
	}
	for (const FHyperAIMaterialPropertyEdit& Edit : Operation.PropertyEdits)
	{
		UMaterialExpression* const* Found = Nodes.Find(Edit.NodeId);
		const FString Kind = Kinds.FindRef(Edit.NodeId);
		if (!Found || Kind.IsEmpty())
			return Fail(TEXT("node_not_editable"), FString::Printf(TEXT("%s is not a node this plan can set properties on (opaque nodes can only be wired or removed)."), *Edit.NodeId.Left(64)));
		if (Gate::IsCustomKind(Kind) && Mode == EHyperAIStudioMaterialAuthoringMode::Nodes)
			return Fail(TEXT("authoring_mode_forbids_custom_hlsl"), TEXT("Materials are set to Nodes only, so Custom HLSL nodes cannot be edited."));
		// Checked on a throwaway node of the same class, so a bad value never touches the material.
		UMaterialExpression* Probe = NewObject<UMaterialExpression>(GetTransientPackage(), (*Found)->GetClass(), NAME_None, RF_Transient);
		FString Error;
		if (!Gate::WriteProperty(*Probe, Kind, Edit.Key, Edit.Value, /*bNotify=*/false, Error))
			return Fail(TEXT("invalid_node_property"), FString::Printf(TEXT("%s: %s"), *Edit.NodeId, *Error));
	}
	TSet<FString> AssignedInputs;
	for (const FHyperAIMaterialEdgeSpec& Edge : Operation.Edges)
	{
		UMaterialExpression* const* From = Nodes.Find(Edge.FromNodeId);
		UMaterialExpression* const* To = Nodes.Find(Edge.ToNodeId);
		if (!From || !To)
			return Fail(TEXT("edge_endpoint_missing"), FString::Printf(TEXT("Edge %s -> %s names a node that does not exist."), *Edge.FromNodeId.Left(64), *Edge.ToNodeId.Left(64)));
		const TArray<FString>* CallOutputs = FunctionOutputs.Find(Edge.FromNodeId);
		const TArray<FString>* CallInputs = FunctionInputs.Find(Edge.ToNodeId);
		const bool bOutputOk = CallOutputs ? (Edge.FromOutput.IsEmpty() ? !CallOutputs->IsEmpty() : CallOutputs->Contains(Edge.FromOutput))
			: Gate::HasOutput(**From, Edge.FromOutput);
		const bool bInputOk = CallInputs ? (Edge.ToInput.IsEmpty() ? !CallInputs->IsEmpty() : CallInputs->Contains(Edge.ToInput))
			: Gate::HasInput(**To, Edge.ToInput);
		if (!bOutputOk || !bInputOk)
			return Fail(TEXT("closed_pin_not_found"), FString::Printf(TEXT("%s has no output '%s', or %s has no input '%s'. Inspect a node to see its pins."),
				*Edge.FromNodeId.Left(64), *Edge.FromOutput.Left(64), *Edge.ToNodeId.Left(64), *Edge.ToInput.Left(64)));
		if (!AddUnique(AssignedInputs, Edge.ToNodeId + TEXT("\n") + Edge.ToInput))
			return Fail(TEXT("contradictory_input_assignments"), TEXT("One input cannot receive more than one source in the same plan."));
	}
	TSet<FString> AssignedOutputs;
	for (const FHyperAIMaterialOutputSpec& Output : Operation.Outputs)
	{
		UMaterialExpression* const* From = Nodes.Find(Output.FromNodeId);
		const TArray<FString>* CallOutputs = FunctionOutputs.Find(Output.FromNodeId);
		if (Gate::OutputPropertyFromName(Output.Property) == MP_MAX || !AddUnique(AssignedOutputs, Output.Property))
			return Fail(TEXT("invalid_output_shape"), FString::Printf(TEXT("'%s' is not a material output, or is set twice."), *Output.Property.Left(64)));
		if (!From || !(CallOutputs ? (Output.FromOutput.IsEmpty() || CallOutputs->Contains(Output.FromOutput)) : Gate::HasOutput(**From, Output.FromOutput)))
			return Fail(TEXT("closed_output_pin_not_found"), FString::Printf(TEXT("%s has no output '%s'."), *Output.FromNodeId.Left(64), *Output.FromOutput.Left(64)));
	}
	for (const FHyperAIMaterialNodeProperty& Setting : Operation.MaterialSettings)
	{
		FString Error;
		if (!Gate::ValidateMaterialSetting(Setting.Key, Setting.Value, Error))
			return Fail(TEXT("invalid_material_setting"), Error);
	}
	return true;
}

bool FHyperAIStudioMaterialsContracts::CaptureExact(
	const FString& Path,
	const FString& Family,
	const FString& Scope,
	const int32 MaxNodesValue,
	const int32 MaxEdgesValue,
	const int32 MaxWorkMs,
	FHyperAIStudioMaterialValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Materials::Private;
	OutSnapshot = {}; OutStatus.Reset(); OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded UObject graph capture is serialized on the Unreal game thread.");
		return false;
	}
	if (!IsCanonicalProjectObjectPath(Path) || !IsAllowedFamily(Family) || !IsAllowedScope(Scope)
		|| MaxNodesValue < 1 || MaxNodesValue > MaxNodes || MaxEdgesValue < 1 || MaxEdgesValue > MaxEdges
		|| MaxWorkMs < 1 || MaxWorkMs > 2000)
	{
		OutStatus = TEXT("invalid_request_bounds");
		OutDiagnostic = TEXT("Exact path, family, scope, node/edge bounds, or deadline is invalid.");
		return false;
	}
	const double Deadline = FPlatformTime::Seconds() + static_cast<double>(MaxWorkMs) / 1000.0;

	if (Scope == TEXT("on_disk_index"))
	{
		FHyperAIMaterialAssetRecord& Record = OutSnapshot.Record;
		Record.Family = Family;
		Record.AssetPath = Path;
		UObject* LoadedExactObject = FSoftObjectPath(Path).ResolveObject();
		Record.bLoaded = LoadedExactObject != nullptr;
		Record.bCompileStateKnown = false;
		bool bDiskComplete = true;
		const UE::AssetRegistry::EExists DiskState =
			CaptureDiskEvidence(Path, Record, bDiskComplete, LoadedExactObject, Deadline);
		if (DiskState == UE::AssetRegistry::EExists::Unknown || !bDiskComplete)
		{
			OutStatus = TEXT("asset_registry_state_unknown");
			OutDiagnostic = TEXT("Exact on-disk existence or package evidence is Unknown; absence is not inferred and no load was attempted.");
			return false;
		}
		if (DiskState == UE::AssetRegistry::EExists::DoesNotExist)
		{
			OutStatus = TEXT("asset_not_found_on_disk");
			OutDiagnostic = TEXT("Asset Registry proved exact on-disk absence; no load was attempted.");
			return false;
		}
		if (!LoadedExactObject)
		{
			OutStatus = TEXT("asset_registry_object_identity_unavailable");
			OutDiagnostic = TEXT("Nonblocking package evidence cannot prove an unloaded exact object row or class; no load was attempted.");
			return false;
		}
		const FTopLevelAssetPath ExpectedClass = Family == TEXT("material")
			? UMaterial::StaticClass()->GetClassPathName() : UMaterialFunction::StaticClass()->GetClassPathName();
		if (LoadedExactObject->GetClass()->GetClassPathName() != ExpectedClass)
		{
			OutStatus = TEXT("exact_type_mismatch");
			OutDiagnostic = TEXT("The exact on-disk class is outside the requested type family; derived assets are not accepted.");
			return false;
		}
		Record.ClassPath = ExpectedClass.ToString();
		// Identity-only rows deliberately cannot issue mutation CAS because dirty/compile/graph state is absent.
		Record.bRevisionComplete = false;
		OutSnapshot.bComplete = false;
		OutSnapshot.Revision = ComputeSnapshotRevision(OutSnapshot, Deadline);
		Record.Revision = OutSnapshot.Revision;
		OutStatus = TEXT("on_disk_identity_only");
		OutDiagnostic = TEXT("Nonblocking package evidence plus already-loaded exact object/class/package identity was captured without loading or graph traversal; dependency fanout is delegated and mutation CAS is intentionally incomplete.");
		return true;
	}

	UObject* Object = FSoftObjectPath(Path).ResolveObject();
	if (!Object)
	{
		OutStatus = TEXT("asset_not_loaded");
		OutDiagnostic = TEXT("The exact object is not already loaded; synchronous loading is forbidden.");
		return false;
	}
	if (!IsExactLoadedFamily(Object, Family))
	{
		OutStatus = TEXT("exact_type_mismatch");
		OutDiagnostic = TEXT("The loaded object is not the exact requested Material family; derived assets are rejected.");
		return false;
	}
	return FillLoadedRecord(Object, Family, MaxNodesValue, MaxEdgesValue, Deadline,
		OutSnapshot, OutStatus, OutDiagnostic);
}

FString FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(
	FHyperAIStudioMaterialValueSnapshot& Snapshot,
	const double Deadline)
{
	FString Canonical;
	if (!HyperAIStudio::Materials::Private::TryCanonicalRecord(
		Snapshot.Record, Deadline, Canonical))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
		Snapshot.Record.bRevisionComplete = false;
		Snapshot.Record.Revision.Reset();
		return FString();
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Snapshot.Revision)
		|| !HyperAIStudio::Materials::Private::IsBeforeDeadline(Deadline))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
		Snapshot.Record.bRevisionComplete = false;
	}
	Snapshot.Record.Revision = Snapshot.Revision;
	return Snapshot.Revision;
}

TArray<FHyperAIMaterialIssue> FHyperAIStudioMaterialsContracts::ValidateValueSnapshot(
	const FHyperAIStudioMaterialValueSnapshot& Snapshot,
	const bool bRequirePackageClean,
	const int32 MaxIssueCount,
	bool& bOutTruncated,
	const double Deadline)
{
	using namespace HyperAIStudio::Materials::Private;
	TArray<FHyperAIMaterialIssue> Issues;
	bOutTruncated = false;
	const int32 Limit = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	const FHyperAIMaterialAssetRecord& Record = Snapshot.Record;
	auto AbortForDeadline = [&]()
	{
		if (HyperAIStudio::Materials::Private::IsBeforeDeadline(Deadline)) return false;
		AddIssue(Issues, Limit, bOutTruncated, TEXT("validation_deadline_exceeded"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("The absolute validation deadline elapsed before the bounded proof completed."));
		bOutTruncated = true;
		return true;
	};
	if (AbortForDeadline()) return Issues;
	if (!Record.bRootOwnershipComplete
		|| Record.OuterPath != Record.OutermostPackageName
		|| Record.OutermostPackageName
			!= FPackageName::ObjectPathToPackageName(Record.AssetPath))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("asset_root_owner_mismatch"), TEXT("error"),
			Record.AssetPath, FString(), -1,
			TEXT("The loaded asset UObject outer/outermost package chain is not the exact canonical target package."));
	if (!Snapshot.bComplete || !Record.bRevisionComplete || !IsCanonicalSha256(Snapshot.Revision))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("revision_incomplete"), TEXT("error"),
			Record.AssetPath, FString(), -1, TEXT("Independent validation requires one complete fresh loaded-state CAS snapshot."));
	if (Record.DiskExistence == TEXT("unknown")
		|| Record.ReferenceEvidence
			!= TEXT("semantic_expression_edges_exact;asset_registry_fanout_delegated"))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("reference_or_disk_evidence_unproven"), TEXT("error"),
			Record.AssetPath, FString(), -1,
			TEXT("Complete CAS requires exact tri-state disk evidence and bounded semantic-edge reference scope."));
	if (bRequirePackageClean && Record.bPackageDirty)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("package_dirty"), TEXT("error"),
			Record.AssetPath, FString(), -1, TEXT("The package is dirty while clean persisted state was required."));
	if (!Record.bCompileStateKnown)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("compile_state_unproven"), TEXT("error"),
			Record.AssetPath, FString(), -1, TEXT("UE 5.8 did not expose complete compile-state evidence for this exact family."));
	else if (Record.bCompiling)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("compile_in_progress"), TEXT("error"),
			Record.AssetPath, FString(), -1, TEXT("The material is still compiling."));
	else if (Record.bCompileError)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("compile_error"), TEXT("error"),
			Record.AssetPath, FString(), -1, TEXT("The material has compile-error evidence on the active shader platform."));

	TSet<FString> StableIds;
	TSet<FString> PersistedGuids;
	TSet<FName> ParameterNames;
	for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
	{
		if (AbortForDeadline()) return Issues;
		if (!Node.bSemanticProjectionComplete
			|| Node.InputPorts.Num() > MaxPortsPerNode
			|| Node.OutputPorts.Num() > MaxPortsPerNode
			|| Node.MenuCategories.Num() > MaxMenuCategoriesPerNode
			|| Node.MenuCategories.ContainsByPredicate([](const FString& Category)
					{ return Category.Len() > MaxSemanticTextCharacters + 128; }))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("node_semantic_projection_incomplete"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1,
				TEXT("The exact supported-class persisted semantic and bounded port projection is incomplete."));
		if (Record.Family == TEXT("material")
			&& (Node.MaterialOwnerPath != Record.AssetPath || !Node.FunctionOwnerPath.IsEmpty()))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("material_expression_owner_mismatch"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1,
				TEXT("A Material expression does not bind the exact target Material and an empty function context."));
		if (Record.Family == TEXT("material_function")
			&& (Node.FunctionOwnerPath != Record.AssetPath || !Node.MaterialOwnerPath.IsEmpty()))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("material_function_expression_owner_mismatch"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1,
				TEXT("A Material Function expression does not bind the exact target function and an empty Material context."));
		if (!Node.bOwnerTopologyComplete || Node.OuterPath != Record.AssetPath
			|| Node.OutermostPackageName != Record.OutermostPackageName)
			AddIssue(Issues, Limit, bOutTruncated, TEXT("expression_uobject_owner_mismatch"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1,
				TEXT("Expression semantic owner pointers disagree with the actual UObject outer/package topology."));
		if (!Node.bGuidValid || Node.PersistedGuid.IsEmpty())
			AddIssue(Issues, Limit, bOutTruncated, TEXT("expression_guid_invalid"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Persisted expression GUID is invalid."));
		if (!AddUnique(StableIds, Node.StableId))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("semantic_stable_id_duplicate"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Inspector stable identity is duplicated."));
		if (!Node.PersistedGuid.IsEmpty() && !AddUnique(PersistedGuids, Node.PersistedGuid))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("expression_guid_duplicate"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Persisted expression GUID is duplicated."));
		if ((Node.Kind == TEXT("scalar_parameter") || Node.Kind == TEXT("vector_parameter"))
			&& (Node.Name.IsEmpty() || !AddUnique(ParameterNames, FName(*Node.Name))))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("parameter_name_invalid_or_duplicate"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Persisted parameter name is empty or duplicated."));
		if ((Node.Kind == TEXT("scalar_parameter") || Node.Kind == TEXT("vector_parameter"))
			&& Node.ParameterGuid.IsEmpty())
			AddIssue(Issues, Limit, bOutTruncated, TEXT("parameter_guid_invalid"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Persisted parameter GUID is invalid."));
		if ((Node.Kind == TEXT("function_input") || Node.Kind == TEXT("function_output"))
			&& Node.FunctionId.IsEmpty())
			AddIssue(Issues, Limit, bOutTruncated, TEXT("function_connector_guid_invalid"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("Persisted function connector GUID is invalid."));
		if (!FMath::IsFinite(Node.Scalar) || !FMath::IsFinite(Node.Vector.R)
			|| !FMath::IsFinite(Node.Vector.G) || !FMath::IsFinite(Node.Vector.B)
			|| !FMath::IsFinite(Node.Vector.A) || !FMath::IsFinite(Node.SliderMin)
			|| !FMath::IsFinite(Node.SliderMax) || !FMath::IsFinite(Node.ConstA)
			|| !FMath::IsFinite(Node.ConstB) || !FMath::IsFinite(Node.FunctionPreviewValue.X)
			|| !FMath::IsFinite(Node.FunctionPreviewValue.Y)
			|| !FMath::IsFinite(Node.FunctionPreviewValue.Z)
			|| !FMath::IsFinite(Node.FunctionPreviewValue.W))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("non_finite_node_value"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1, TEXT("A persisted closed numeric value is not finite."));
	}
	for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
	{
		if (AbortForDeadline()) return Issues;
		if ((!Node.SubgraphExpressionStableId.IsEmpty()
				&& !StableIds.Contains(Node.SubgraphExpressionStableId))
			|| Node.SubgraphRootStableId.IsEmpty()
			|| !StableIds.Contains(Node.SubgraphRootStableId))
			AddIssue(Issues, Limit, bOutTruncated,
				TEXT("subgraph_root_projection_invalid"), TEXT("error"),
				Record.AssetPath, Node.StableId, -1,
				TEXT("Expression subgraph parent/root identity is outside the exact bounded collection."));
	}

	TMap<FString, TArray<FString>> Adjacent;
	TMap<FString, TArray<FString>> Reverse;
	TSet<FString> InputKeys;
	for (const FHyperAIMaterialEdgeView& Edge : Record.Edges)
	{
		if (AbortForDeadline()) return Issues;
		if (!StableIds.Contains(Edge.FromStableId))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("dangling_input_source"), TEXT("error"),
				Record.AssetPath, Edge.ToStableId, -1, TEXT("An input points outside the exact persisted expression collection."));
		if (Edge.ToStableId != TEXT("$material_output") && !StableIds.Contains(Edge.ToStableId))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("dangling_input_target"), TEXT("error"),
				Record.AssetPath, Edge.ToStableId, -1, TEXT("An edge target is outside the exact persisted expression collection."));
		const FString InputKey = Edge.ToStableId + TEXT("\n") + FString::FromInt(Edge.ToInputIndex);
		if (!AddUnique(InputKeys, InputKey))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("multiple_sources_for_input"), TEXT("error"),
				Record.AssetPath, Edge.ToStableId, -1, TEXT("One persisted input has multiple source edges."));
		if (StableIds.Contains(Edge.FromStableId)
			&& (StableIds.Contains(Edge.ToStableId) || Edge.ToStableId == TEXT("$material_output")))
		{
			Adjacent.FindOrAdd(Edge.FromStableId).Add(Edge.ToStableId);
				Reverse.FindOrAdd(Edge.ToStableId).Add(Edge.FromStableId);
			}
		}
		for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
		{
			if (AbortForDeadline()) return Issues;
			for (const FHyperAIMaterialInputPortView& Port : Node.InputPorts)
			{
				if (AbortForDeadline()) return Issues;
				const FString InputKey = Node.StableId + TEXT("\n")
					+ FString::FromInt(Port.Index);
				if (Port.bConnected != InputKeys.Contains(InputKey))
					AddIssue(Issues, Limit, bOutTruncated,
						Port.bConnected
							? TEXT("connected_input_edge_projection_missing")
							: TEXT("disconnected_input_edge_projection_present"),
						TEXT("error"), Record.AssetPath, Node.StableId, -1,
						TEXT("Persisted input connection flags and the exact edge projection disagree."));
			}
		}
		for (const FHyperAIMaterialPropertyInputView& Property : Record.PropertyInputs)
		{
			if (AbortForDeadline()) return Issues;
			const FString InputKey = FString(TEXT("$material_output\n"))
				+ FString::FromInt(Property.PropertyIndex);
			if (Property.bConnected != InputKeys.Contains(InputKey))
				AddIssue(Issues, Limit, bOutTruncated,
					Property.bConnected
						? TEXT("connected_material_output_edge_projection_missing")
						: TEXT("disconnected_material_output_edge_projection_present"),
					TEXT("error"), Record.AssetPath, TEXT("$material_output"), -1,
					TEXT("Persisted material-output connection flags and the exact edge projection disagree."));
		}
		TMap<FString, uint8> Color;
	TFunction<bool(const FString&)> Visit = [&](const FString& Node)
	{
		if (!HyperAIStudio::Materials::Private::IsBeforeDeadline(Deadline)) return true;
		const uint8 Existing = Color.FindRef(Node);
		if (Existing == 1) return true;
		if (Existing == 2 || Node == TEXT("$material_output")) return false;
		Color.Add(Node, 1);
		if (const TArray<FString>* Next = Adjacent.Find(Node))
			for (const FString& Child : *Next) if (Visit(Child)) return true;
		Color.Add(Node, 2); return false;
	};
	for (const FString& StableId : StableIds)
	{
		if (AbortForDeadline()) return Issues;
		if (Visit(StableId))
		{
			if (AbortForDeadline()) return Issues;
			AddIssue(Issues, Limit, bOutTruncated, TEXT("semantic_graph_cycle"), TEXT("error"),
				Record.AssetPath, StableId, -1, TEXT("The persisted semantic graph contains a cycle."));
			break;
		}
	}

	TSet<FString> ReachesOutput;
	TArray<FString> Stack;
	if (Record.Family == TEXT("material")) Stack.Add(TEXT("$material_output"));
	else for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
		if (Node.Kind == TEXT("function_output")) Stack.Add(Node.StableId);
	while (!Stack.IsEmpty())
	{
		if (AbortForDeadline()) return Issues;
		const FString Current = Stack.Pop(EAllowShrinking::No);
		if (!AddUnique(ReachesOutput, Current)) continue;
		if (const TArray<FString>* Prior = Reverse.Find(Current)) for (const FString& Node : *Prior) Stack.Add(Node);
	}
	for (const FHyperAIMaterialNodeView& Node : Record.Nodes)
	{
		if (AbortForDeadline()) return Issues;
		if (!ReachesOutput.Contains(Node.StableId))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("node_not_output_reachable"), TEXT("warning"),
				Record.AssetPath, Node.StableId, -1, TEXT("Expression is not reachable from a persisted material/function output."));
	}
	return Issues;
}

FString FHyperAIStudioMaterialsContracts::ComputePayloadSemanticFingerprint(
	const TArray<FHyperAIStudioMaterialBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::Materials::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.material.payload.semantic.v1"));
	AppendToken(Canonical, BaseRevision);
	for (const FHyperAIStudioMaterialBackendOperation& Operation : Operations)
		AppendToken(Canonical, CanonicalBackendOperation(Operation));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioMaterialsContracts::ComputeSealedNodeStableId(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const FString& NodeId)
{
	using namespace HyperAIStudio::Materials::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.material.persisted-node-seal.v1"));
	AppendToken(Canonical, CanonicalBackendOperation(Operation));
	AppendToken(Canonical, NodeId);
	const FString Hash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Hash)) return FString();
	FGuid Guid;
	if (!FGuid::ParseExact(Hash.Mid(7, 32), EGuidFormats::Digits, Guid) || !Guid.IsValid())
		return FString();
	return TEXT("guid:") + Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString FHyperAIStudioMaterialsContracts::ComputeSealedParameterGuid(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const FString& NodeId)
{
	using namespace HyperAIStudio::Materials::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.material.persisted-parameter-seal.v1"));
	AppendToken(Canonical, CanonicalBackendOperation(Operation));
	AppendToken(Canonical, NodeId);
	const FString Hash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Hash)) return FString();
	FGuid Guid;
	if (!FGuid::ParseExact(Hash.Mid(7, 32), EGuidFormats::Digits, Guid) || !Guid.IsValid())
		return FString();
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

bool FHyperAIStudioMaterialsContracts::BuildExpectedCompoundSemanticGraph(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	TArray<FHyperAIMaterialNodeView>& OutNodes,
	TArray<FHyperAIMaterialEdgeView>& OutEdges,
	FString& OutError,
	const double Deadline)
{
	using namespace HyperAIStudio::Materials::Private;
	OutNodes.Reset(); OutEdges.Reset(); OutError.Reset();
	if (Operation.Kind != EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph
		|| Operation.TargetFamily != TEXT("material") || Operation.Nodes.IsEmpty()
		|| Operation.Nodes.Num() > MaxNodes
		|| Operation.Edges.Num() + Operation.Outputs.Num() > MaxEdges
		|| Operation.Outputs.Num() > 16)
	{
		OutError = TEXT("material_compound_expected_shape_invalid");
		return false;
	}

	TMap<FString, int32> NodeIndexByPlanId;
	for (int32 Index = 0; Index < Operation.Nodes.Num(); ++Index)
	{
		if (!IsBeforeDeadline(Deadline))
		{
			OutError = TEXT("material_compound_expected_deadline_exceeded");
			return false;
		}
		const FHyperAIMaterialNodeSpec& Spec = Operation.Nodes[Index];
		UClass* Class = ExpectedNodeClass(Spec.Kind);
		const UMaterialExpression* Cdo = Class
			? Cast<UMaterialExpression>(Class->GetDefaultObject()) : nullptr;
		bool bProjectionComplete = false;
		FHyperAIMaterialNodeView Node = MakeNodeView(
			Cdo, Index, Deadline, bProjectionComplete);
		const FString StableId = ComputeSealedNodeStableId(Operation, Spec.NodeId);
		if (!bProjectionComplete || StableId.IsEmpty() || NodeIndexByPlanId.Contains(Spec.NodeId))
		{
			OutError = TEXT("material_compound_expected_node_projection_failed");
			return false;
		}
		Node.StableId = StableId;
		Node.PersistedGuid = StableId.Mid(5);
		Node.bGuidValid = true;
		Node.MaterialOwnerPath = Operation.TargetPath;
		Node.FunctionOwnerPath.Reset();
		Node.OuterPath = Operation.TargetPath;
		Node.OuterClassPath = UMaterial::StaticClass()->GetPathName();
		Node.OutermostPackageName = FPackageName::ObjectPathToPackageName(Operation.TargetPath);
		Node.SubgraphExpressionPath.Reset();
		Node.SubgraphExpressionStableId.Reset();
		Node.SubgraphRootStableId = StableId;
		Node.bOwnerTopologyComplete = true;
		Node.EditorX = Spec.EditorX; Node.EditorY = Spec.EditorY;
		Node.Name = Spec.Name; Node.Group = Spec.Group;
		if (Spec.Kind == TEXT("constant") || Spec.Kind == TEXT("scalar_parameter"))
			Node.Scalar = Spec.Scalar;
		if (Spec.Kind == TEXT("vector_parameter")) Node.Vector = Spec.Vector;
		if (Spec.Kind == TEXT("scalar_parameter") || Spec.Kind == TEXT("vector_parameter"))
		{
			Node.ParameterGuid = ComputeSealedParameterGuid(Operation, Spec.NodeId);
			if (Node.ParameterGuid.IsEmpty())
			{
				OutError = TEXT("material_compound_expected_parameter_guid_failed");
				return false;
			}
		}
		Node.bSemanticProjectionComplete = true;
		NodeIndexByPlanId.Add(Spec.NodeId, OutNodes.Num());
		OutNodes.Add(MoveTemp(Node));
	}

	auto ResolveOutput = [&](const FString& NodeId, const FString& OutputNameValue,
		int32& OutNodeIndex, const FHyperAIMaterialOutputPortView*& OutPort)
	{
		const int32* NodeIndex = NodeIndexByPlanId.Find(NodeId);
		if (!NodeIndex || !OutNodes.IsValidIndex(*NodeIndex)) return false;
		OutNodeIndex = *NodeIndex;
		int32 OutputIndex = INDEX_NONE;
		if (!TryExpectedSourceOutputIndex(
			OutNodes[*NodeIndex].Kind, OutputNameValue, OutputIndex)) return false;
		if (!OutNodes[*NodeIndex].OutputPorts.IsValidIndex(OutputIndex)) return false;
		OutPort = &OutNodes[*NodeIndex].OutputPorts[OutputIndex];
		return true;
	};

	TSet<FString> InputWriters;
	for (const FHyperAIMaterialEdgeSpec& Spec : Operation.Edges)
	{
		if (!IsBeforeDeadline(Deadline))
		{
			OutError = TEXT("material_compound_expected_deadline_exceeded");
			return false;
		}
		int32 SourceNodeIndex = INDEX_NONE;
		const FHyperAIMaterialOutputPortView* SourcePort = nullptr;
		const int32* TargetNodeIndex = NodeIndexByPlanId.Find(Spec.ToNodeId);
		int32 TargetInputIndex = INDEX_NONE;
		FString TargetInputName;
		if (!ResolveOutput(Spec.FromNodeId, Spec.FromOutput, SourceNodeIndex, SourcePort)
			|| !TargetNodeIndex || !OutNodes.IsValidIndex(*TargetNodeIndex)
			|| !TryExpectedTargetInputIndex(OutNodes[*TargetNodeIndex].Kind,
				Spec.ToInput, TargetInputIndex, &TargetInputName)
			|| !OutNodes[*TargetNodeIndex].InputPorts.IsValidIndex(TargetInputIndex)
			|| !AddUnique(InputWriters,
				Spec.ToNodeId + TEXT("\n") + FString::FromInt(TargetInputIndex)))
		{
			OutError = TEXT("material_compound_expected_edge_projection_failed");
			return false;
		}
		FHyperAIMaterialInputPortView& TargetPort =
			OutNodes[*TargetNodeIndex].InputPorts[TargetInputIndex];
		TargetPort.bConnected = true;
		TargetPort.OutputIndex = SourcePort->Index;
		TargetPort.Mask = SourcePort->Mask; TargetPort.MaskR = SourcePort->MaskR;
		TargetPort.MaskG = SourcePort->MaskG; TargetPort.MaskB = SourcePort->MaskB;
		TargetPort.MaskA = SourcePort->MaskA;

		FHyperAIMaterialEdgeView& Edge = OutEdges.AddDefaulted_GetRef();
		Edge.FromStableId = OutNodes[SourceNodeIndex].StableId;
		Edge.FromOutputIndex = SourcePort->Index; Edge.FromOutputName = SourcePort->Name;
		Edge.ToStableId = OutNodes[*TargetNodeIndex].StableId;
		Edge.ToInputIndex = TargetInputIndex; Edge.ToInputName = TargetInputName;
		Edge.Mask = SourcePort->Mask; Edge.MaskR = SourcePort->MaskR;
		Edge.MaskG = SourcePort->MaskG; Edge.MaskB = SourcePort->MaskB;
		Edge.MaskA = SourcePort->MaskA;
	}

	TSet<FString> OutputWriters;
	for (const FHyperAIMaterialOutputSpec& Spec : Operation.Outputs)
	{
		if (!IsBeforeDeadline(Deadline))
		{
			OutError = TEXT("material_compound_expected_deadline_exceeded");
			return false;
		}
		int32 SourceNodeIndex = INDEX_NONE;
		const FHyperAIMaterialOutputPortView* SourcePort = nullptr;
		const EMaterialProperty Property = OutputProperty(Spec.Property);
		if (Property == MP_MAX
			|| !ResolveOutput(Spec.FromNodeId, Spec.FromOutput, SourceNodeIndex, SourcePort)
			|| !AddUnique(OutputWriters, Spec.Property))
		{
			OutError = TEXT("material_compound_expected_output_projection_failed");
			return false;
		}
		FHyperAIMaterialEdgeView& Edge = OutEdges.AddDefaulted_GetRef();
		Edge.FromStableId = OutNodes[SourceNodeIndex].StableId;
		Edge.FromOutputIndex = SourcePort->Index; Edge.FromOutputName = SourcePort->Name;
		Edge.ToStableId = TEXT("$material_output");
		Edge.ToInputIndex = static_cast<int32>(Property); Edge.ToInputName = OutputName(Property);
		Edge.Mask = SourcePort->Mask; Edge.MaskR = SourcePort->MaskR;
		Edge.MaskG = SourcePort->MaskG; Edge.MaskB = SourcePort->MaskB;
		Edge.MaskA = SourcePort->MaskA;
	}
	return true;
}

bool FHyperAIStudioMaterialsContracts::BuildExpectedCompoundPropertyState(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	TArray<FHyperAIMaterialPropertyInputView>& OutProperties,
	FString& OutError,
	const double Deadline)
{
	using namespace HyperAIStudio::Materials::Private;
	OutProperties.Reset(); OutError.Reset();
	TArray<FHyperAIMaterialNodeView> ExpectedNodes;
	TArray<FHyperAIMaterialEdgeView> IgnoredEdges;
	if (!BuildExpectedCompoundSemanticGraph(
		Operation, ExpectedNodes, IgnoredEdges, OutError, Deadline)) return false;
	UMaterial* DefaultMaterial = GetMutableDefault<UMaterial>();
	if (!DefaultMaterial)
	{
		OutError = TEXT("material_compound_default_property_projection_unavailable");
		return false;
	}
	bool bDefaultProjectionComplete = true;
	for (const EMaterialProperty Property : InspectedMaterialProperties())
	{
		if (!IsBeforeDeadline(Deadline))
		{
			OutError = TEXT("material_compound_expected_deadline_exceeded");
			return false;
		}
		FHyperAIMaterialPropertyInputView PropertyView = CaptureMaterialPropertyInput(
			DefaultMaterial, Property, bDefaultProjectionComplete);
		if (const FHyperAIMaterialOutputSpec* Requested = Operation.Outputs.FindByPredicate(
			[&](const FHyperAIMaterialOutputSpec& Candidate)
			{
				return Candidate.Property == OutputName(Property);
			}))
		{
			const int32 SourceNodeIndex = Operation.Nodes.IndexOfByPredicate(
				[&](const FHyperAIMaterialNodeSpec& Candidate)
				{
					return Candidate.NodeId == Requested->FromNodeId;
				});
			if (!ExpectedNodes.IsValidIndex(SourceNodeIndex))
			{
				OutError = TEXT("material_compound_expected_property_source_missing");
				return false;
			}
			int32 SourceOutputIndex = INDEX_NONE;
			if (!TryExpectedSourceOutputIndex(ExpectedNodes[SourceNodeIndex].Kind,
				Requested->FromOutput, SourceOutputIndex)
				|| !ExpectedNodes[SourceNodeIndex].OutputPorts.IsValidIndex(SourceOutputIndex))
			{
				OutError = TEXT("material_compound_expected_property_pin_missing");
				return false;
			}
			const FHyperAIMaterialOutputPortView& SourcePort =
				ExpectedNodes[SourceNodeIndex].OutputPorts[SourceOutputIndex];
			PropertyView.bConnected = true;
			PropertyView.OutputIndex = SourcePort.Index;
			PropertyView.Mask = SourcePort.Mask; PropertyView.MaskR = SourcePort.MaskR;
			PropertyView.MaskG = SourcePort.MaskG; PropertyView.MaskB = SourcePort.MaskB;
			PropertyView.MaskA = SourcePort.MaskA;
		}
		OutProperties.Add(MoveTemp(PropertyView));
	}
	if (!bDefaultProjectionComplete)
	{
		OutError = TEXT("material_compound_default_property_projection_incomplete");
		OutProperties.Reset();
		return false;
	}
	return true;
}

bool FHyperAIStudioMaterialsContracts::VerifyCompoundPostconditions(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const FHyperAIStudioMaterialValueSnapshot& Snapshot,
	FString& OutEffectFingerprint,
	FString& OutError)
{
	using namespace HyperAIStudio::Materials::Private;
	OutEffectFingerprint.Reset(); OutError.Reset();
	if (Operation.Kind != EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph
		|| !Snapshot.bComplete || !Snapshot.Record.bRevisionComplete
		|| Snapshot.Record.Family != TEXT("material")
		|| Snapshot.Record.AssetPath != Operation.TargetPath
		|| Snapshot.Record.NodeCount != Operation.Nodes.Num()
		|| Snapshot.Record.Nodes.Num() != Operation.Nodes.Num()
		|| Snapshot.Record.EdgeCount != Snapshot.Record.Edges.Num())
	{
		OutError = TEXT("material_compound_effect_shape_mismatch");
		return false;
	}

	TArray<FHyperAIMaterialNodeView> ExpectedNodes;
	TArray<FHyperAIMaterialEdgeView> ExpectedEdgeViews;
	if (!BuildExpectedCompoundSemanticGraph(
		Operation, ExpectedNodes, ExpectedEdgeViews, OutError)) return false;

	TMap<FString, const FHyperAIMaterialNodeView*> ActualNodes;
	for (const FHyperAIMaterialNodeView& Node : Snapshot.Record.Nodes)
	{
		if (Node.StableId.IsEmpty() || ActualNodes.Contains(Node.StableId))
		{
			OutError = TEXT("material_compound_effect_node_identity_mismatch");
			return false;
		}
		ActualNodes.Add(Node.StableId, &Node);
	}
	for (const FHyperAIMaterialNodeView& ExpectedNode : ExpectedNodes)
	{
		const FHyperAIMaterialNodeView* const* Found = ActualNodes.Find(ExpectedNode.StableId);
		if (!Found || !*Found)
		{
			OutError = TEXT("material_compound_effect_node_missing");
			return false;
		}
		if (CanonicalNode(**Found) != CanonicalNode(ExpectedNode))
		{
			OutError = TEXT("material_compound_effect_node_projection_mismatch");
			return false;
		}
	}

	TSet<FString> ExpectedEdges;
	for (const FHyperAIMaterialEdgeView& Edge : ExpectedEdgeViews)
	{
		if (!AddUnique(ExpectedEdges, SemanticEdgeKey(Edge.FromStableId,
			Edge.FromOutputIndex, Edge.FromOutputName, Edge.ToStableId,
			Edge.ToInputIndex, Edge.ToInputName, Edge.Mask, Edge.MaskR,
			Edge.MaskG, Edge.MaskB, Edge.MaskA)))
		{
			OutError = TEXT("material_compound_expected_edge_noncanonical");
			return false;
		}
	}
	TSet<FString> ActualEdges;
	for (const FHyperAIMaterialEdgeView& Edge : Snapshot.Record.Edges)
	{
		if (!AddUnique(ActualEdges, SemanticEdgeKey(Edge.FromStableId, Edge.FromOutputIndex,
			Edge.FromOutputName, Edge.ToStableId, Edge.ToInputIndex, Edge.ToInputName,
			Edge.Mask, Edge.MaskR, Edge.MaskG, Edge.MaskB, Edge.MaskA)))
		{
			OutError = TEXT("material_compound_effect_duplicate_edge");
			return false;
		}
	}
	if (ActualEdges.Num() != ExpectedEdges.Num()
		|| !ActualEdges.Difference(ExpectedEdges).IsEmpty()
		|| !ExpectedEdges.Difference(ActualEdges).IsEmpty())
	{
		OutError = TEXT("material_compound_effect_edge_or_output_mismatch");
		return false;
	}
	if (Snapshot.Record.ClassPath != UMaterial::StaticClass()->GetPathName()
		|| !Snapshot.Record.ExpressionExecBeginStableId.IsEmpty()
		|| !Snapshot.Record.ExpressionExecEndStableId.IsEmpty())
	{
		OutError = TEXT("material_compound_effect_material_state_mismatch");
		return false;
	}

	TArray<FHyperAIMaterialPropertyInputView> ExpectedPropertyViews;
	if (!BuildExpectedCompoundPropertyState(
		Operation, ExpectedPropertyViews, OutError)) return false;
	TMap<int32, FHyperAIMaterialPropertyInputView> ExpectedProperties;
	for (const FHyperAIMaterialPropertyInputView& Property : ExpectedPropertyViews)
		ExpectedProperties.Add(Property.PropertyIndex, Property);
	if (Snapshot.Record.PropertyInputs.Num() != ExpectedProperties.Num())
	{
		OutError = TEXT("material_compound_effect_property_projection_incomplete");
		return false;
	}
	for (const FHyperAIMaterialPropertyInputView& Actual : Snapshot.Record.PropertyInputs)
	{
		const FHyperAIMaterialPropertyInputView* Expected =
			ExpectedProperties.Find(Actual.PropertyIndex);
		if (!Expected || CanonicalPropertyInput(Actual) != CanonicalPropertyInput(*Expected))
		{
			OutError = TEXT("material_compound_effect_property_state_mismatch");
			return false;
		}
	}

	FString EffectCanonical;
	AppendToken(EffectCanonical, TEXT("hyperai.material.compound-effect.v1"));
	AppendToken(EffectCanonical, CanonicalBackendOperation(Operation));
	AppendToken(EffectCanonical, Snapshot.Revision);
	OutEffectFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(EffectCanonical);
	if (!IsCanonicalSha256(OutEffectFingerprint))
	{
		OutError = TEXT("material_compound_effect_fingerprint_failed");
		return false;
	}
	return true;
}

namespace HyperAIStudio::Materials::Private
{
	bool FindShadowCycleEdge(
		const TArray<FHyperAIMaterialEdgeView>& Edges,
		int32& OutEdgeIndex,
		const double Deadline,
		bool& bOutDeadlineReached)
	{
		TMap<FString, TArray<int32>> BySource;
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			if (FPlatformTime::Seconds() >= Deadline) { bOutDeadlineReached = true; return false; }
			if (Edges[Index].ToStableId != TEXT("$material_output"))
				BySource.FindOrAdd(Edges[Index].FromStableId).Add(Index);
		}
		TMap<FString, uint8> Color;
		TFunction<bool(const FString&)> Visit = [&](const FString& Node)
		{
			if (FPlatformTime::Seconds() >= Deadline) { bOutDeadlineReached = true; return false; }
			Color.Add(Node, 1);
			if (const TArray<int32>* NextEdges = BySource.Find(Node))
			{
				for (const int32 EdgeIndex : *NextEdges)
				{
					if (FPlatformTime::Seconds() >= Deadline) { bOutDeadlineReached = true; return false; }
					const FString& Child = Edges[EdgeIndex].ToStableId;
					if (Color.FindRef(Child) == 1) { OutEdgeIndex = EdgeIndex; return true; }
					if (Color.FindRef(Child) == 0 && Visit(Child)) return true;
				}
			}
			Color.Add(Node, 2); return false;
		};
		for (const TPair<FString, TArray<int32>>& Pair : BySource)
		{
			if (FPlatformTime::Seconds() >= Deadline) { bOutDeadlineReached = true; return false; }
			if (Color.FindRef(Pair.Key) == 0 && Visit(Pair.Key)) return true;
		}
		return false;
	}

	bool ReplayRepairShadow(
		const FHyperAIStudioMaterialValueSnapshot& Before,
		const FHyperAIStudioMaterialBackendOperation& Operation,
		FHyperAIStudioMaterialValueSnapshot& OutAfter,
		const double Deadline,
		FString& OutError)
	{
		OutAfter = Before;
		FHyperAIMaterialAssetRecord& Record = OutAfter.Record;
		const TSet<FString> Repairs(Operation.RepairKinds);
		auto ClearPersistedTargetInput = [&](const FHyperAIMaterialEdgeView& Edge)
		{
			if (Edge.ToStableId == TEXT("$material_output"))
			{
				if (FHyperAIMaterialPropertyInputView* Property =
					Record.PropertyInputs.FindByPredicate([&](const auto& Candidate)
					{
						return Candidate.PropertyIndex == Edge.ToInputIndex;
					}))
				{
					Property->bConnected = false;
					Property->OutputIndex = 0;
					Property->Mask = Property->MaskR = Property->MaskG =
						Property->MaskB = Property->MaskA = 0;
				}
				return;
			}
			if (FHyperAIMaterialNodeView* Node =
				Record.Nodes.FindByPredicate([&](const auto& Candidate)
				{
					return Candidate.StableId == Edge.ToStableId;
				}))
			{
				if (FHyperAIMaterialInputPortView* Port =
					Node->InputPorts.FindByPredicate([&](const auto& Candidate)
					{
						return Candidate.Index == Edge.ToInputIndex;
					}))
				{
					Port->bConnected = false;
					Port->OutputIndex = 0;
					Port->Mask = Port->MaskR = Port->MaskG = Port->MaskB = Port->MaskA = 0;
				}
			}
		};
		if (Repairs.Contains(TEXT("regenerate_duplicate_guids")))
		{
			TSet<FString> Seen;
			TMap<FString, FString> StableRemap;
			for (int32 Index = 0; Index < Record.Nodes.Num(); ++Index)
			{
				FHyperAIMaterialNodeView& Node = Record.Nodes[Index];
				if (Node.PersistedGuid.IsEmpty() || Seen.Contains(Node.PersistedGuid))
				{
					const FString NewGuidHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
						Operation.TargetPath + TEXT("|repair-guid|") + FString::FromInt(Index));
					if (!FHyperAIStudioMaterialsContracts::IsCanonicalSha256(NewGuidHash))
					{
						OutError = TEXT("repair_shadow_guid_hash_failed"); return false;
					}
					const FString NewGuid = NewGuidHash.Mid(7, 32);
					const FString NewStable = TEXT("shadow-guid:") + NewGuid;
					StableRemap.Add(Node.StableId, NewStable);
					Node.StableId = NewStable;
					Node.PersistedGuid = NewGuid;
					Node.bGuidValid = true;
				}
				Seen.Add(Node.PersistedGuid);
			}
			for (FHyperAIMaterialEdgeView& Edge : Record.Edges)
			{
				if (const FString* NewFrom = StableRemap.Find(Edge.FromStableId)) Edge.FromStableId = *NewFrom;
				if (const FString* NewTo = StableRemap.Find(Edge.ToStableId)) Edge.ToStableId = *NewTo;
			}
		}
		if (Repairs.Contains(TEXT("disconnect_dangling_inputs")))
		{
			TSet<FString> StableIds;
			for (const FHyperAIMaterialNodeView& Node : Record.Nodes) StableIds.Add(Node.StableId);
			Record.Edges.RemoveAll([&](const FHyperAIMaterialEdgeView& Edge)
			{
				const bool bDangling = !StableIds.Contains(Edge.FromStableId)
					|| (Edge.ToStableId != TEXT("$material_output") && !StableIds.Contains(Edge.ToStableId));
				if (bDangling) ClearPersistedTargetInput(Edge);
				return bDangling;
			});
		}
		if (Repairs.Contains(TEXT("disconnect_cycles")))
		{
			int32 Removed = 0;
			int32 EdgeIndex = INDEX_NONE;
			bool bDeadlineReached = false;
			while (FindShadowCycleEdge(Record.Edges, EdgeIndex, Deadline, bDeadlineReached))
			{
				if (!Record.Edges.IsValidIndex(EdgeIndex)
					|| ++Removed > FHyperAIStudioMaterialsContracts::MaxEdges)
				{
					OutError = TEXT("repair_shadow_cycle_bound_exceeded"); return false;
				}
				ClearPersistedTargetInput(Record.Edges[EdgeIndex]);
				Record.Edges.RemoveAt(EdgeIndex, 1, EAllowShrinking::No);
				EdgeIndex = INDEX_NONE;
			}
			if (bDeadlineReached)
			{
				OutError = TEXT("repair_shadow_deadline_exceeded"); return false;
			}
		}
		Record.EdgeCount = Record.Edges.Num();
		Record.bGraphTruncated = false;
		Record.bRevisionComplete = true;
		OutAfter.bComplete = true;
		FHyperAIStudioMaterialsContracts::ComputeSnapshotRevision(OutAfter, Deadline);
		if (!OutAfter.bComplete)
		{
			OutError = TEXT("repair_shadow_seal_deadline_or_size_exceeded"); return false;
		}
		if (OutAfter.Revision == Before.Revision)
		{
			OutError = TEXT("semantic_repair_has_no_effect"); return false;
		}
		bool bTruncated = false;
		const TArray<FHyperAIMaterialIssue> Issues = FHyperAIStudioMaterialsContracts::ValidateValueSnapshot(
			OutAfter, false, FHyperAIStudioMaterialsContracts::MaxIssues, bTruncated, Deadline);
		if (bTruncated || HasErrors(Issues))
		{
			OutError = TEXT("repair_shadow_still_invalid"); return false;
		}
		return true;
	}
}

bool FHyperAIStudioMaterialsContracts::BuildExpectedRepairSemanticGraph(
	const FHyperAIStudioMaterialValueSnapshot& Before,
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const int32 MaxWorkMs,
	FHyperAIStudioMaterialValueSnapshot& OutAfter,
	FString& OutError)
{
	OutAfter = FHyperAIStudioMaterialValueSnapshot();
	OutError.Reset();
	if (Operation.Kind != EHyperAIStudioMaterialOperationKind::RepairSemanticGraph
		|| !Before.bComplete || !Before.Record.bRevisionComplete
		|| Before.Record.AssetPath != Operation.TargetPath
		|| Before.Record.Family != Operation.TargetFamily
		|| Before.Revision != Operation.ExpectedRevision
		|| MaxWorkMs < 1 || MaxWorkMs > 2000)
	{
		OutError = TEXT("repair_shadow_precondition_failed");
		return false;
	}
	return HyperAIStudio::Materials::Private::ReplayRepairShadow(
		Before, Operation, OutAfter,
		FPlatformTime::Seconds() + static_cast<double>(MaxWorkMs) / 1000.0,
		OutError);
}

bool FHyperAIStudioMaterialsContracts::VerifyRepairPostconditions(
	const FHyperAIStudioMaterialBackendOperation& Operation,
	const FHyperAIStudioMaterialValueSnapshot& Snapshot,
	FString& OutEffectFingerprint,
	FString& OutError)
{
	using namespace HyperAIStudio::Materials::Private;
	OutEffectFingerprint.Reset(); OutError.Reset();
	if (Operation.Kind != EHyperAIStudioMaterialOperationKind::RepairSemanticGraph
		|| !Snapshot.bComplete || !Snapshot.Record.bRevisionComplete
		|| Snapshot.Record.AssetPath != Operation.TargetPath
		|| Snapshot.Revision == Operation.ExpectedRevision)
	{
		OutError = TEXT("material_repair_effect_state_mismatch");
		return false;
	}
	bool bValidationTruncated = false;
	const TArray<FHyperAIMaterialIssue> IndependentIssues = ValidateValueSnapshot(
		Snapshot, false, MaxIssues, bValidationTruncated);
	if (bValidationTruncated || HasErrors(IndependentIssues))
	{
		OutError = TEXT("material_repair_post_projection_invalid");
		return false;
	}
	const TSet<FString> Repairs(Operation.RepairKinds);
	TSet<FString> StableIds;
	TSet<FString> PersistedGuids;
	for (const FHyperAIMaterialNodeView& Node : Snapshot.Record.Nodes)
	{
		StableIds.Add(Node.StableId);
		if (Repairs.Contains(TEXT("regenerate_duplicate_guids"))
			&& (!Node.bGuidValid || Node.PersistedGuid.IsEmpty()
				|| !AddUnique(PersistedGuids, Node.PersistedGuid)))
		{
			OutError = TEXT("material_repair_guid_defect_remains");
			return false;
		}
	}
	if (Repairs.Contains(TEXT("disconnect_dangling_inputs")))
	{
		for (const FHyperAIMaterialEdgeView& Edge : Snapshot.Record.Edges)
		{
			if (!StableIds.Contains(Edge.FromStableId)
				|| (Edge.ToStableId != TEXT("$material_output")
					&& !StableIds.Contains(Edge.ToStableId)))
			{
				OutError = TEXT("material_repair_dangling_defect_remains");
				return false;
			}
		}
	}
	if (Repairs.Contains(TEXT("disconnect_cycles")))
	{
		int32 CycleEdge = INDEX_NONE;
		bool bDeadlineReached = false;
		if (FindShadowCycleEdge(Snapshot.Record.Edges, CycleEdge,
			FPlatformTime::Seconds() + 0.5, bDeadlineReached))
		{
			OutError = TEXT("material_repair_cycle_defect_remains");
			return false;
		}
		if (bDeadlineReached)
		{
			OutError = TEXT("material_repair_verify_deadline_exceeded");
			return false;
		}
	}
	FString EffectCanonical;
	AppendToken(EffectCanonical, TEXT("hyperai.material.repair-effect.v1"));
	AppendToken(EffectCanonical, CanonicalBackendOperation(Operation));
	AppendToken(EffectCanonical, Snapshot.Revision);
	OutEffectFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(EffectCanonical);
	if (!IsCanonicalSha256(OutEffectFingerprint))
	{
		OutError = TEXT("material_repair_effect_fingerprint_failed");
		return false;
	}
	return true;
}

namespace HyperAIStudio::Materials::Private
{
	namespace Gate = HyperAIStudio::Materials::Gate;
	using EKind = EHyperAIStudioMaterialOperationKind;

	bool IsCreateKind(const EKind Kind)
	{
		return Kind == EKind::CompoundCreateConfigureGraph || Kind == EKind::CreateMaterial
			|| Kind == EKind::CreateMaterialInstance;
	}

	/**
	 * One revision over every target: the inspect revision of an edited material, the content key of an edited
	 * instance, "absent" for a create. Planning seals it and Apply recomputes it, so a target that changed in
	 * between, by an agent or a person, is refused before a single edit lands.
	 */
	bool ComputeBaseRevision(
		const TArray<FHyperAIStudioMaterialBackendOperation>& Operations,
		const int32 MaxWorkMs,
		FString& OutBase,
		FString& OutError)
	{
		TArray<const FHyperAIStudioMaterialBackendOperation*> Sorted;
		for (const FHyperAIStudioMaterialBackendOperation& Operation : Operations) Sorted.Add(&Operation);
		Sorted.Sort([](const FHyperAIStudioMaterialBackendOperation& A, const FHyperAIStudioMaterialBackendOperation& B)
		{
			return A.TargetPath < B.TargetPath;
		});
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.material.base.v2"));
		for (const FHyperAIStudioMaterialBackendOperation* Operation : Sorted)
		{
			AppendToken(Canonical, Operation->TargetPath);
			if (IsCreateKind(Operation->Kind))
			{
				const bool bAbsent = !FSoftObjectPath(Operation->TargetPath).ResolveObject()
					&& !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Operation->TargetPath));
				AppendToken(Canonical, bAbsent ? TEXT("absent") : TEXT("present"));
			}
			else if (Operation->Kind == EKind::SetInstanceParameters)
			{
				const UObject* Instance = FSoftObjectPath(Operation->TargetPath).ResolveObject();
				if (!Instance)
				{
					OutError = TEXT("The material instance is not loaded.");
					return false;
				}
				AppendToken(Canonical, Gate::ComputeContentKey(*Instance));
			}
			else
			{
				FHyperAIStudioMaterialValueSnapshot Snapshot;
				FString Status;
				if (!FHyperAIStudioMaterialsContracts::CaptureExact(Operation->TargetPath, Operation->TargetFamily,
					TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes, FHyperAIStudioMaterialsContracts::MaxEdges,
					MaxWorkMs, Snapshot, Status, OutError) || !Snapshot.bComplete)
				{
					if (OutError.IsEmpty()) OutError = TEXT("The material no longer captures completely.");
					return false;
				}
				AppendToken(Canonical, Snapshot.Revision);
			}
		}
		OutBase = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		return FHyperAIStudioMaterialsContracts::IsCanonicalSha256(OutBase);
	}

	/** The reviewable identity of a plan: its ordered operations, the base they apply to, and the authoring mode. */
	FString SealFingerprint(const FHyperAIStudioMaterialTypedPayload& Payload)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.material.sealed-plan.v2"));
		AppendToken(Canonical, FHyperAIStudioMaterialsContracts::ComputePayloadSemanticFingerprint(
			Payload.Operations, Payload.BaseRevision));
		AppendToken(Canonical, Payload.AuthoringMode);
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString EffectTargetOf(const FHyperAIStudioMaterialTypedPayload& Payload)
	{
		return Payload.Operations.Num() == 1
			? Payload.Operations[0].TargetPath
			: FString(TEXT("material-plan:")) + Payload.BaseRevision;
	}

	FString DescribeOperation(const FHyperAIStudioMaterialBackendOperation& Operation)
	{
		switch (Operation.Kind)
		{
		case EKind::CreateMaterial:
		case EKind::CompoundCreateConfigureGraph:
			return FString::Printf(TEXT("create material %s: %d nodes, %d connections, %d outputs"),
				*Operation.TargetPath, Operation.Nodes.Num(), Operation.Edges.Num(), Operation.Outputs.Num());
		case EKind::EditGraph:
			return FString::Printf(TEXT("edit %s: +%d nodes, -%d nodes, %d connections, %d property edits"),
				*Operation.TargetPath, Operation.Nodes.Num(), Operation.RemoveNodeIds.Num(),
				Operation.Edges.Num() + Operation.Outputs.Num(), Operation.PropertyEdits.Num());
		case EKind::CreateMaterialInstance:
			return FString::Printf(TEXT("create instance %s of %s with %d parameters"),
				*Operation.TargetPath, *Operation.ParentPath, Operation.Parameters.Num());
		case EKind::SetInstanceParameters:
			return FString::Printf(TEXT("set %d parameters on %s"), Operation.Parameters.Num(), *Operation.TargetPath);
		default:
			return FString::Printf(TEXT("repair %s"), *Operation.TargetPath);
		}
	}

	/**
	 * One executor phase; the order is fixed: Apply, Compile, Validate, Save, VerifyFresh. After Apply every failure is
	 * FailedAfterKnownEffect: the edit stays in memory, unsaved and undoable, rather than being silently rolled back.
	 */
	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioMaterialTypedPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioMaterials, Log, TEXT("Material edit phase %d finished %s in %.1f ms. %s"),
				static_cast<int32>(Phase), *Status, (FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioMaterialResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioMaterialResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->Revision = ContentKey;
				Output->bValid = true;
				Result.Payload = Output;
			}
			return Result;
		};
		const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
		const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		if (!IsInGameThread())
		{
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Material edits run on the game thread."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Apply:
		{
			if (Payload.AuthoringMode != FHyperAIStudioMaterialsContracts::GetAuthoringModeName())
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("authoring_mode_changed"),
					TEXT("The Materials authoring mode changed after this plan was reviewed; nothing was applied. Dry-run again."));
			}
			FString Base;
			FString Error;
			if (!ComputeBaseRevision(Payload.Operations, 150, Base, Error) || Base != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("A target changed after planning; nothing was applied. Inspect and dry-run again. ") + Error);
			}
			for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
			{
				const UObject* Target = FSoftObjectPath(Operation.TargetPath).ResolveObject();
				if (Target && Gate::IsOpenInEditor(*Target))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_open_in_editor"),
						FString::Printf(TEXT("%s was opened in an editor after planning; nothing was applied. Close its tab and retry."), *Operation.TargetPath));
				}
			}
			const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioMaterials", "ApplyPlan", "HyperAI Material Edit"));
			int32 Changed = 0;
			for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
			{
				if (!Gate::ApplyOperation(Operation, Changed, Error))
				{
					return Finish(Changed > 0
						? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect
						: EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect,
						TEXT("edit_op_failed"), Error + TEXT(" Nothing was saved; undo reverts what landed."));
				}
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d edits applied."), Changed), TEXT("applied"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Compile:
		{
			TArray<FString> Targets;
			for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
			{
				Targets.Add(Operation.TargetPath);
			}
			// Translating a graph can take over a second on the game thread, more than a phase may spend, so the
			// recompile runs on the next tick outside the executor's budget, as it would after an edit in the material
			// editor. A job then waits for the shaders; translation errors fail that job and show in validate.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Targets](float)
			{
				TArray<FString> TranslationErrors;
				for (const FString& Path : Targets)
				{
					if (UMaterial* Material = Cast<UMaterial>(FSoftObjectPath(Path).ResolveObject()))
					{
						for (const FString& Error : UMaterialEditingLibrary::RecompileMaterial(Material))
						{
							TranslationErrors.Add(Material->GetName() + TEXT(": ") + Error.Left(256));
						}
					}
				}
				FHyperAIStudioAsyncJobRequest Job;
				Job.PackId = FHyperAIStudioMaterialsContracts::PackId;
				Job.ToolName = FHyperAIStudioMaterialsContracts::MutationToolName;
				Job.Target = Targets.Num() == 1 ? Targets[0] : FString::Printf(TEXT("%d materials"), Targets.Num());
				Job.DeadlineMs = 10 * 60 * 1000;
				Job.PollIntervalMs = 250;
				FString JobId;
				FString JobError;
				FHyperAIStudioAsyncJobHost::Start(Job, [Targets, TranslationErrors](FString& OutProgress, FString& OutDiagnostic)
				{
					TArray<FString> Failures = TranslationErrors;
					for (const FString& Path : Targets)
					{
						UMaterialInterface* Material = Cast<UMaterialInterface>(FSoftObjectPath(Path).ResolveObject());
						if (!Material) continue;
						if (!Gate::IsCompileFinished(*Material))
						{
							OutProgress = TEXT("compiling shaders");
							return EHyperAIStudioAsyncJobPoll::Running;
						}
						UMaterial* Base = Material->GetMaterial();
						const FMaterialResource* Resource = Base && TranslationErrors.IsEmpty()
							? Base->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
						if (Resource)
						{
							for (const FString& Error : Resource->GetCompileErrors())
							{
								Failures.Add(Material->GetName() + TEXT(": ") + Error.Left(256));
							}
						}
					}
					OutDiagnostic = Failures.IsEmpty()
						? FString(TEXT("Shaders compiled. hyper_material_validate reports instruction counts and writes the preview PNG."))
						: FString::Join(Failures, TEXT(" | ")).Left(1024);
					return Failures.IsEmpty() ? EHyperAIStudioAsyncJobPoll::Completed : EHyperAIStudioAsyncJobPoll::Failed;
				}, JobId, JobError);
				return false;
			}));
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("compile_scheduled"),
				TEXT("The graph recompiles on the next tick and a job watches the shaders. hyper_material_validate reports errors, instruction counts and the preview once they finish."),
				TEXT("compile_requested"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
			{
				UObject* Target = FSoftObjectPath(Operation.TargetPath).ResolveObject();
				if (!Target)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("target_not_loaded"),
						FString::Printf(TEXT("%s is no longer loaded."), *Operation.TargetPath));
				}
				if (Target->IsA<UMaterial>())
				{
					// Structure only: shaders are still compiling, and the validator counts that as an error.
					FHyperAIStudioMaterialValueSnapshot Snapshot;
					FString Status;
					FString Diagnostic;
					if (!FHyperAIStudioMaterialsContracts::CaptureExact(Operation.TargetPath, TEXT("material"), TEXT("loaded_only"),
						FHyperAIStudioMaterialsContracts::MaxNodes, FHyperAIStudioMaterialsContracts::MaxEdges, 150,
						Snapshot, Status, Diagnostic))
					{
						return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, Status,
							TEXT("The edited material no longer captures and was not saved; undo reverts it. ") + Diagnostic);
					}
				}
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"),
				TEXT("Every target captures. hyper_material_validate reports compile results once shaders finish."), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
		{
			TArray<UPackage*> Packages;
			for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
			{
				if (UObject* Target = FSoftObjectPath(Operation.TargetPath).ResolveObject())
				{
					Packages.AddUnique(Target->GetOutermost());
				}
			}
			if (Packages.Num() != Payload.Operations.Num() || !UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The edit applied but not every package saved; it remains in memory and can be undone."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Packages saved."), TEXT("saved"));
		}
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh content captured for verification."), TEXT("completed"),
				FHyperAIStudioMaterialsContracts::ComputePlanContentKey(Payload));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

bool FHyperAIStudioMaterialsContracts::ComputePlanBaseRevision(
	const TArray<FHyperAIStudioMaterialBackendOperation>& Operations, const int32 MaxWorkMs, FString& OutBase, FString& OutError)
{
	return HyperAIStudio::Materials::Private::ComputeBaseRevision(Operations, MaxWorkMs, OutBase, OutError);
}

FString FHyperAIStudioMaterialsContracts::ComputeSealedPlanFingerprint(const FHyperAIStudioMaterialTypedPayload& Payload)
{
	return HyperAIStudio::Materials::Private::SealFingerprint(Payload);
}

FString FHyperAIStudioMaterialsContracts::ComputePlanContentKey(const FHyperAIStudioMaterialTypedPayload& Payload)
{
	using namespace HyperAIStudio::Materials::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.material.plan-content.v1"));
	for (const FHyperAIStudioMaterialBackendOperation& Operation : Payload.Operations)
	{
		const UObject* Target = FSoftObjectPath(Operation.TargetPath).ResolveObject();
		AppendToken(Canonical, Target ? Gate::ComputeContentKey(*Target) : FString(TEXT("missing:")) + Operation.TargetPath);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIMaterialApplyPlanReport FHyperAIStudioMaterialsContracts::BuildPlan(
	const FHyperAIMaterialApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Materials::Private;
	FHyperAIMaterialApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Clip(Request.OperationId, 256);
	Report.Capabilities = GetCapabilityMatrix();
	Report.AuthoringMode = GetAuthoringModeName();
	auto Reject = [&Report](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic.Left(1024);
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Material plans capture loaded state on the game thread."));
	}
	if (Request.OperationId.Len() > 256
		|| Request.ExpectedPlanHash.Len() > 71
		|| (!Request.ExpectedPlanHash.IsEmpty() && !IsCanonicalSha256(Request.ExpectedPlanHash))
		|| Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 30000
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > 2000
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_request_bounds"),
			TEXT("Operation identity/count, expected hash, deadline, game-thread budget, or output bound is invalid."));
	}
	if (!Request.bDryRun && (Request.OperationId.IsEmpty() || Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("operation_identity_required"),
			TEXT("Applying needs operation_id and expected_plan_hash (the plan_hash of the dry run you reviewed)."));
	}
	const EHyperAIStudioMaterialAuthoringMode Mode = GetDefault<UHyperAIStudioSettings>()->MaterialAuthoringMode;
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(FMath::Min(Request.DeadlineMs, Request.MaxGameThreadMs)) / 1000.0;
	const int32 IssueLimit = FMath::Clamp(Request.MaxOutputBytes / 16384, 1, MaxIssues);
	TArray<FHyperAIStudioMaterialBackendOperation> Operations;
	TMap<FString, FString> ExpectedPostProjectionByTarget;
	TMap<FString, const FHyperAIStudioMaterialBackendOperation*> PlannedCreates;
	TSet<FString> Targets;
	int64 PlanMaterializedBytes = 1024;
	int32 CustomNodes = 0;
	int32 CustomBytes = 0;
	int32 ParameterCount = 0;
	bool bIssueTruncated = false;
	bool bHasRepair = false;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioMaterialBackendOperation Operation;
		FString ErrorCode, Error;
		if (!ValidateOperationShape(Request.Operations[Index], Operation, ErrorCode, Error, Deadline))
		{
			AddIssue(Report.Issues, IssueLimit, bIssueTruncated, *ErrorCode, TEXT("error"),
				Request.Operations[Index].TargetPath, FString(), Index, Error);
			continue;
		}
		const FString OperationCanonical = CanonicalBackendOperation(Operation);
		const int64 OperationBytes = 256ll + static_cast<int64>(OperationCanonical.Len()) * MaxUtf8BytesPerCharacter;
		if (OperationBytes > FHyperAIStudioDomainLimits::MaxRequestBytes
			|| PlanMaterializedBytes > FHyperAIStudioDomainLimits::MaxRequestBytes - OperationBytes)
		{
			AddIssue(Report.Issues, IssueLimit, bIssueTruncated, TEXT("typed_payload_exceeds_shared_bound"), TEXT("error"),
				Operation.TargetPath, FString(), Index, TEXT("The plan is too large; split it into smaller plans."));
			continue;
		}
		PlanMaterializedBytes += OperationBytes;
		if (!AddUnique(Targets, Operation.TargetPath))
		{
			AddIssue(Report.Issues, IssueLimit, bIssueTruncated, TEXT("duplicate_plan_target"), TEXT("error"),
				Operation.TargetPath, FString(), Index, TEXT("Each plan may bind one operation per exact target."));
			continue;
		}
		auto AddError = [&](const TCHAR* Code, const FString& Message)
		{
			AddIssue(Report.Issues, IssueLimit, bIssueTruncated, Code, TEXT("error"), Operation.TargetPath, FString(), Index, Message);
		};
		auto RequireAbsent = [&]()
		{
			FString AbsenceError;
			if (!CheckCreateTargetAbsence(Operation.TargetPath, AbsenceError))
				AddError(*AbsenceError, TEXT("Create needs a target path with no asset loaded or on disk; Unknown fails closed."));
		};
		switch (Operation.Kind)
		{
		case EKind::CompoundCreateConfigureGraph:
		{
			RequireAbsent();
			ValidateCompoundGraph(Operation, Report.Issues, Index, IssueLimit, bIssueTruncated, Deadline);
			TArray<FHyperAIMaterialNodeView> ExpectedNodes;
			TArray<FHyperAIMaterialEdgeView> ExpectedEdges;
			TArray<FHyperAIMaterialPropertyInputView> ExpectedProperties;
			FString ExpectedError;
			if (!BuildExpectedCompoundSemanticGraph(Operation, ExpectedNodes, ExpectedEdges, ExpectedError, Deadline)
				|| !BuildExpectedCompoundPropertyState(Operation, ExpectedProperties, ExpectedError, Deadline))
			{
				AddError(TEXT("expected_post_projection_unavailable"), ExpectedError);
				break;
			}
			FString ExpectedCanonical;
			AppendToken(ExpectedCanonical, TEXT("hyperai.material.expected-compound-projection.v1"));
			for (const FHyperAIMaterialNodeView& Node : ExpectedNodes) AppendToken(ExpectedCanonical, CanonicalNode(Node));
			for (const FHyperAIMaterialEdgeView& Edge : ExpectedEdges) AppendToken(ExpectedCanonical, CanonicalEdge(Edge));
			for (const FHyperAIMaterialPropertyInputView& Property : ExpectedProperties)
				AppendToken(ExpectedCanonical, CanonicalPropertyInput(Property));
			const FString ExpectedProjection = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ExpectedCanonical);
			if (IsCanonicalSha256(ExpectedProjection)) ExpectedPostProjectionByTarget.Add(Operation.TargetPath, ExpectedProjection);
			else AddError(TEXT("expected_post_projection_unavailable"), TEXT("Expected projection exceeded its canonical byte bound."));
			PlannedCreates.Add(Operation.TargetPath, nullptr);
			break;
		}
		case EKind::RepairSemanticGraph:
		{
			bHasRepair = true;
			FHyperAIStudioMaterialValueSnapshot Snapshot;
			FString Status, Diagnostic;
			const int32 CaptureBudgetMs = FMath::Clamp(
				FMath::CeilToInt((Deadline - FPlatformTime::Seconds()) * 1000.0), 1, Request.MaxGameThreadMs);
			if (!CaptureExact(Operation.TargetPath, Operation.TargetFamily, TEXT("loaded_only"),
				MaxNodes, MaxEdges, CaptureBudgetMs, Snapshot, Status, Diagnostic))
				AddError(*Status, Diagnostic);
			else if (Snapshot.Record.DiskExistence != TEXT("exists") || !Snapshot.Record.bExistsOnDisk)
				AddError(TEXT("mutation_target_not_persisted"), TEXT("Semantic repair requires exact Asset Registry Exists."));
			else if (!Snapshot.bComplete || Snapshot.Revision != Operation.ExpectedRevision)
				AddError(TEXT("revision_precondition_failed"), TEXT("Fresh loaded CAS is incomplete or differs from expected_revision."));
			else if (Snapshot.Record.bCompiling || Snapshot.Record.bCompileError)
				AddError(TEXT("compile_state_not_editable"), TEXT("Semantic repair cannot begin while compile is active or failed."));
			else
			{
				FHyperAIStudioMaterialValueSnapshot Shadow;
				FString ShadowError;
				if (!ReplayRepairShadow(Snapshot, Operation, Shadow, Deadline, ShadowError))
					AddError(*ShadowError, TEXT("Closed typed repair shadow did not produce a distinct independently valid graph."));
				else ExpectedPostProjectionByTarget.Add(Operation.TargetPath, Shadow.Revision);
			}
			break;
		}
		case EKind::CreateMaterial:
		case EKind::EditGraph:
		{
			const UMaterial* Existing = nullptr;
			if (Operation.Kind == EKind::CreateMaterial)
			{
				RequireAbsent();
				PlannedCreates.Add(Operation.TargetPath, nullptr);
			}
			else
			{
				FHyperAIStudioMaterialValueSnapshot Snapshot;
				FString Status, Diagnostic;
				if (!CaptureExact(Operation.TargetPath, TEXT("material"), TEXT("loaded_only"), MaxNodes, MaxEdges,
					Request.MaxGameThreadMs, Snapshot, Status, Diagnostic))
				{
					AddError(*Status, Diagnostic + TEXT(" Inspect the material first (it must be loaded)."));
					break;
				}
				if (!Snapshot.bComplete)
				{
					AddError(TEXT("revision_incomplete"), TEXT("The material does not capture completely; save it (opaque nodes need a clean package) and inspect again."));
					break;
				}
				if (Snapshot.Revision != Operation.ExpectedRevision)
				{
					AddError(TEXT("stale_revision"), TEXT("The material changed since you inspected it; inspect again for its current revision and node ids.")
						+ FHyperAIStudioAgentActivityLog::DescribeLastChange(Operation.TargetPath));
					break;
				}
				if (Snapshot.Record.bCompiling)
				{
					AddError(TEXT("compile_in_progress"), TEXT("The material is still compiling; retry once it finishes."));
					break;
				}
				Existing = Cast<UMaterial>(FSoftObjectPath(Operation.TargetPath).ResolveObject());
				if (Existing && Gate::IsOpenInEditor(*Existing))
				{
					AddError(TEXT("target_open_in_editor"), TEXT("Close the material's editor tab: edits to an open material are overwritten by its preview copy."));
					break;
				}
			}
			TArray<FString> CustomDescriptions;
			if (!ValidateGraphOperation(Operation, Existing, Mode, CustomNodes, CustomBytes, CustomDescriptions, ErrorCode, Error))
			{
				AddError(*ErrorCode, Error);
				break;
			}
			Report.CustomHlslNodes.Append(CustomDescriptions);
			break;
		}
		case EKind::CreateMaterialInstance:
		case EKind::SetInstanceParameters:
		{
			ParameterCount += Operation.Parameters.Num();
			if (ParameterCount > MaxParametersPerPlan)
			{
				AddError(TEXT("operation_collection_bound_exceeded"), FString::Printf(TEXT("A plan may set at most %d parameters."), MaxParametersPerPlan));
				break;
			}
			TMap<FName, FString> Declared;
			if (Operation.Kind == EKind::CreateMaterialInstance)
			{
				RequireAbsent();
				PlannedCreates.Add(Operation.TargetPath, nullptr);
				// A parent created earlier in this plan declares its parameters through its parameter nodes.
				if (const FHyperAIStudioMaterialBackendOperation* const* Planned = PlannedCreates.Find(Operation.ParentPath))
				{
					for (const FHyperAIStudioMaterialBackendOperation& Earlier : Operations)
					{
						if (Earlier.TargetPath != Operation.ParentPath) continue;
						for (const FHyperAIMaterialNodeSpec& Node : Earlier.Nodes)
						{
							FString NodeError;
							if (UMaterialExpression* Shadow = Gate::IsParameterKind(Node.Kind) ? Gate::MakeShadowNode(Node, NodeError) : nullptr)
							{
								const FName Name = Cast<UMaterialExpressionParameter>(Shadow) ? Cast<UMaterialExpressionParameter>(Shadow)->ParameterName
									: Cast<UMaterialExpressionTextureSampleParameter>(Shadow)->ParameterName;
								Declared.Add(Name, Node.Kind == TEXT("scalar_parameter") ? TEXT("scalar") : Node.Kind == TEXT("vector_parameter")
									? TEXT("vector") : Node.Kind == TEXT("texture_parameter") ? TEXT("texture") : TEXT("static_switch"));
							}
							else if (Node.Kind == TEXT("scalar_parameter") || Node.Kind == TEXT("vector_parameter"))
							{
								Declared.Add(FName(*Node.Name), Node.Kind == TEXT("scalar_parameter") ? TEXT("scalar") : TEXT("vector"));
							}
						}
					}
				}
				else
				{
					UObject* Parent = FSoftObjectPath(Operation.ParentPath).TryLoad();
					UMaterialInterface* ParentMaterial = Cast<UMaterialInterface>(Parent);
					if (!ParentMaterial)
					{
						AddError(TEXT("parent_not_found"), FString::Printf(TEXT("%s is not a material or material instance."), *Operation.ParentPath));
						break;
					}
					Declared = Gate::GetParentParameters(*ParentMaterial);
				}
			}
			else
			{
				UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(FSoftObjectPath(Operation.TargetPath).TryLoad());
				if (!Instance)
				{
					AddError(TEXT("target_not_found"), TEXT("The target is not a material instance constant."));
					break;
				}
				if (Gate::IsOpenInEditor(*Instance))
				{
					AddError(TEXT("target_open_in_editor"), TEXT("Close the instance's editor tab first."));
					break;
				}
				if (UMaterialInterface* Parent = Instance->Parent)
				{
					Declared = Gate::GetParentParameters(*Parent);
				}
			}
			for (const FHyperAIMaterialParameterValue& Parameter : Operation.Parameters)
			{
				const FString* Type = Declared.Find(FName(*Parameter.Name.TrimStartAndEnd()));
				FString ValueError;
				if (!Type || *Type != Parameter.Type)
				{
					TArray<FString> Known;
					for (const TPair<FName, FString>& Pair : Declared) Known.Add(Pair.Key.ToString() + TEXT(":") + Pair.Value);
					AddError(TEXT("parameter_not_on_parent"), FString::Printf(TEXT("The parent has no %s parameter '%s'. It has: %s."),
						*Parameter.Type, *Parameter.Name, *FString::Join(Known, TEXT(", ")).Left(512)));
				}
				else if (!Gate::CheckParameterValue(Parameter, ValueError))
				{
					AddError(TEXT("invalid_parameter_value"), ValueError);
				}
			}
			break;
		}
		}
		Operations.Add(MoveTemp(Operation));
		if (FPlatformTime::Seconds() >= Deadline && Request.Operations.Num() > 1 && Index + 1 < Request.Operations.Num())
		{
			return Reject(TEXT("planning_budget_exceeded"), TEXT("Planning ran out of its game-thread budget; split the plan or raise max_game_thread_ms."));
		}
	}
	Report.bTruncated = bIssueTruncated;
	if (HasErrors(Report.Issues) || Operations.Num() != Request.Operations.Num())
	{
		Report.Status = TEXT("plan_state_invalid");
		Report.Diagnostic = TEXT("One or more operations failed their checks; see issues.");
		return Report;
	}
	if (bHasRepair && !Request.bDryRun)
	{
		return Reject(TEXT("repair_is_evidence_only"),
			TEXT("repair_semantic_graph only produces dry-run evidence; disconnect or remove nodes with edit_graph instead."));
	}

	const TSharedRef<FHyperAIStudioMaterialTypedPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioMaterialTypedPayload, ESPMode::ThreadSafe>();
	Payload->Operations = Operations;
	Payload->AuthoringMode = Report.AuthoringMode;
	FString BaseError;
	if (!ComputeBaseRevision(Operations, Request.MaxGameThreadMs, Payload->BaseRevision, BaseError))
	{
		return Reject(TEXT("base_revision_unavailable"), BaseError);
	}
	Payload->SemanticFingerprint = SealFingerprint(*Payload);
	Report.BaseRevision = Payload->BaseRevision;
	if (Payload->GetBoundedByteSize() <= 0 || Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_payload_exceeds_shared_bound"), TEXT("The plan is too large; split it into smaller plans."));
	}
	FString ExpectedPostCanonical;
	AppendToken(ExpectedPostCanonical, TEXT("hyperai.material.expected-post-projection-set.v1"));
	TArray<FString> ExpectedTargets;
	ExpectedPostProjectionByTarget.GetKeys(ExpectedTargets);
	ExpectedTargets.Sort();
	for (const FString& Target : ExpectedTargets)
	{
		AppendToken(ExpectedPostCanonical, Target);
		AppendToken(ExpectedPostCanonical, ExpectedPostProjectionByTarget.FindChecked(Target));
	}
	Report.ExpectedPostProjectionFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ExpectedPostCanonical);

	for (const FHyperAIStudioMaterialBackendOperation& Operation : Operations)
	{
		const bool bGraph = Operation.Kind != EKind::CreateMaterialInstance && Operation.Kind != EKind::SetInstanceParameters;
		Report.Effects.AssetsCreated += IsCreateKind(Operation.Kind) ? 1 : 0;
		Report.Effects.NodesCreated += Operation.Nodes.Num();
		Report.Effects.ConnectionsCreated += Operation.Edges.Num() + Operation.Outputs.Num();
		Report.Effects.NodesRemoved += Operation.RemoveNodeIds.Num();
		Report.Effects.PropertiesSet += Operation.PropertyEdits.Num() + Operation.MaterialSettings.Num();
		Report.Effects.InstancesCreated += Operation.Kind == EKind::CreateMaterialInstance ? 1 : 0;
		Report.Effects.ParametersSet += Operation.Parameters.Num();
		if (bGraph && Operation.Kind != EKind::RepairSemanticGraph)
		{
			Report.PreviewImagePaths.Add(FHyperAIStudioResultPreview::GetAssetPreviewPath(Operation.TargetPath));
		}
	}
	Report.Effects.OperationCount = Operations.Num();
	Report.Effects.TargetCount = Targets.Num();

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = MutationToolName;
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = EffectTargetOf(*Payload);
	// Phases run one per tick and share these operation-wide budgets.
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = FMath::Max(5, Operations.Num() + 4);
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	Trusted.bCompileOnce = true;
	Trusted.bSaveOnce = true;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;
	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioMaterialsFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioMaterialsFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError)
		|| !Prepare.bPrepared)
	{
		FString Diagnostic = PrepareError.IsEmpty() ? Prepare.Status.Diagnostic : PrepareError;
		if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
		{
			Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
		}
		return Reject(Prepare.Status.StatusCode.IsEmpty() ? TEXT("prepare_failed") : *Prepare.Status.StatusCode, Diagnostic);
	}
	Report.bTrustedPrepared = true;
	// The executor's own plan hash differs between preparations of the same intent; review binds to the sealed plan.
	Report.PlanHash = Payload->SemanticFingerprint;
	Report.AuthorizationPlanHash = Prepare.PlanHash;
	Report.Effects.bTypedShadowReplayComplete = true;
	Report.Effects.bTransactionOnce = true;
	Report.Effects.bCompileOnce = true;
	Report.Effects.bSaveOnce = true;
	Report.Effects.bValidateOnce = true;
	Report.Effects.bFreshVerifyOnce = true;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = TEXT("Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("The plan changed since its dry run (operations, target state, or authoring mode). Dry-run again and review it."));
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = MutationToolName;
		Summary.VariantId = MutationVariantId;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Trusted.EffectTarget;
		Summary.PlanHash = Payload->SemanticFingerprint;
		for (const FHyperAIStudioMaterialBackendOperation& Operation : Operations)
		{
			Summary.Effects.Add(DescribeOperation(Operation));
			Summary.Touches.Add(Operation.TargetPath);
		}
		for (const FString& Custom : Report.CustomHlslNodes)
		{
			Summary.Effects.Add(TEXT("custom HLSL: ") + Custom);
		}
		FString ApprovalError;
		if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, ApprovalError))
		{
			return Reject(TEXT("approval_not_queued"), ApprovalError);
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing changed yet. Poll hyper_operation_status with operation_id: it starts once approved.");
		return Report;
	}
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic StageStatus;
	FString StageError;
	if (!FHyperAIStudioTrustedExecutionFacade::StageExact(Prepared, Request.OperationId, Receipt, StageStatus, StageError))
	{
		return Reject(StageStatus.StatusCode.IsEmpty() ? TEXT("stage_failed") : *StageStatus.StatusCode,
			StageError.IsEmpty() ? StageStatus.Diagnostic : StageError);
	}
	Report.bStaged = true;
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	FHyperAIStudioTrustedExecutionDiagnostic SubmitStatus;
	FString SubmitError;
	// Edit is tokenless; only risky safety classes need a server-issued grant.
	if (!FHyperAIStudioTrustedExecutionFacade::SubmitExact(Receipt, FString(), Submission, SubmitStatus, SubmitError))
	{
		Report.Status = SubmitStatus.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : SubmitStatus.StatusCode;
		Report.Diagnostic = SubmitError.IsEmpty() ? SubmitStatus.Diagnostic : SubmitError;
		return Report;
	}
	Report.bExecutionSubmitted = true;
	Report.bOk = true;
	Report.Status = TEXT("submitted");
	Report.Diagnostic = TEXT("The edit runs over the next editor ticks: apply, compile, validate, save, verify. Poll hyper_operation_status with operation_id, then hyper_material_validate for instruction counts and the preview PNG.");
	return Report;
}

FHyperAIMaterialInspectReport UHyperAIStudioMaterialsToolset::hyper_material_inspect(
	const FHyperAIMaterialInspectRequest& Request)
{
	using namespace HyperAIStudio::Materials::Private;
	FHyperAIMaterialInspectReport Report;
	Report.Scope = Clip(Request.Scope, 32);
	Report.Capabilities = FHyperAIStudioMaterialsContracts::GetCapabilityMatrix();
	if (Request.Scope.Len() > 32
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioMaterialsContracts::MaxPageSize
		|| Request.Cursor.Len() > 16 || Request.MaxNodes < 1
		|| Request.MaxNodes > FHyperAIStudioMaterialsContracts::MaxNodes || Request.MaxEdges < 1
		|| Request.MaxEdges > FHyperAIStudioMaterialsContracts::MaxEdges
		|| Request.MaxOutputBytes < FHyperAIStudioMaterialsContracts::MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioMaterialsContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Page, cursor, graph, or output bounds are invalid.");
		return Report;
	}
	int32 Offset = 0;
	if (!Request.Cursor.IsEmpty()
		&& (!LexTryParseString(Offset, *Request.Cursor) || Offset < 0))
	{
		Report.Status = TEXT("invalid_cursor");
		Report.Diagnostic = TEXT("Cursor must be a bounded non-negative decimal node offset.");
		return Report;
	}
	const double OverallDeadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.MaxGameThreadMs) / 1000.0;
	FHyperAIStudioMaterialValueSnapshot TargetSnapshot;
	const int32 CaptureBudgetMs = Request.ComparePath.IsEmpty()
		? Request.MaxGameThreadMs : FMath::Max(1, Request.MaxGameThreadMs / 2);
	if (!FHyperAIStudioMaterialsContracts::CaptureExact(Request.TargetPath, Request.TargetFamily,
		Request.Scope, Request.MaxNodes, Request.MaxEdges, CaptureBudgetMs,
		TargetSnapshot, Report.Status, Report.Diagnostic))
		return Report;
	Report.Target = TargetSnapshot.Record;
	Report.Revision = TargetSnapshot.Revision;
	Report.bRevisionComplete = TargetSnapshot.bComplete;
	Report.bFreshCapture = true;
	if (!Request.ComparePath.IsEmpty())
	{
		if (Request.Scope != TEXT("loaded_only"))
		{
			Report.Status = TEXT("diff_requires_loaded_scope");
			Report.Diagnostic = TEXT("Semantic graph diff requires two exact already-loaded assets; on-disk identity rows contain no graph.");
			return Report;
		}
		FHyperAIStudioMaterialValueSnapshot CompareSnapshot;
		FString CompareStatus, CompareDiagnostic;
		if (!FHyperAIStudioMaterialsContracts::CaptureExact(Request.ComparePath, Request.TargetFamily,
			TEXT("loaded_only"), Request.MaxNodes, Request.MaxEdges, CaptureBudgetMs,
			CompareSnapshot, CompareStatus, CompareDiagnostic))
		{
			Report.Status = CompareStatus; Report.Diagnostic = CompareDiagnostic; return Report;
		}
		Report.Compare = CompareSnapshot.Record;
		if (!TargetSnapshot.bComplete || !CompareSnapshot.bComplete)
		{
			Report.Status = TEXT("semantic_diff_capture_incomplete");
			Report.Diagnostic = TEXT("Semantic diff refuses truncated or compile-state-incomplete graph evidence.");
			return Report;
		}
		if (!BuildDiff(TargetSnapshot.Record, CompareSnapshot.Record, Report.Diff,
			Report.bTruncated, OverallDeadline))
		{
			Report.Status = TEXT("inspect_deadline_exceeded");
			Report.Diagnostic = TEXT("The finite total inspect/diff/output deadline elapsed before a complete diff was formed.");
			return Report;
		}
		Report.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TargetSnapshot.Revision + TEXT("|") + CompareSnapshot.Revision);
		Report.bRevisionComplete = TargetSnapshot.bComplete && CompareSnapshot.bComplete;
	}
	const int32 TotalNodes = Report.Target.Nodes.Num();
	if (Offset > TotalNodes)
	{
		Report.Status = TEXT("cursor_out_of_range");
		Report.Diagnostic = TEXT("Cursor exceeds the bounded exact node result.");
		return Report;
	}
	const int32 Count = FMath::Min(Request.PageSize, TotalNodes - Offset);
	TArray<FHyperAIMaterialNodeView> Page;
	for (int32 Index = 0; Index < Count; ++Index) Page.Add(Report.Target.Nodes[Offset + Index]);
	Report.Target.Nodes = MoveTemp(Page);
	if (Offset + Count < TotalNodes)
	{
		Report.bTruncated = true;
		Report.NextCursor = FString::FromInt(Offset + Count);
	}
	const int32 PageNodeCountBeforeOutputBound = Report.Target.Nodes.Num();
	if (!EnforceInspectOutputBound(Report, Request.MaxOutputBytes, OverallDeadline)
		|| (PageNodeCountBeforeOutputBound > 0 && Report.Target.Nodes.IsEmpty()))
	{
		Report.Status = TEXT("output_bound_too_small");
		Report.Diagnostic = TEXT("Even the bounded identity/page envelope cannot fit max_output_bytes.");
		return Report;
	}
	if (Report.Target.Nodes.Num() < PageNodeCountBeforeOutputBound)
	{
		Report.bTruncated = true;
		Report.NextCursor = FString::FromInt(Offset + Report.Target.Nodes.Num());
	}
	Report.bOk = true;
	if (Report.Status.IsEmpty() || Report.Status == TEXT("captured")) Report.Status = TEXT("ok");
	if (Report.Diagnostic.IsEmpty())
		Report.Diagnostic = TEXT("Bounded exact persisted semantic snapshot/diff returned; no asset load or broad registry scan occurred.");
	return Report;
}

FHyperAIMaterialApplyPlanReport UHyperAIStudioMaterialsToolset::hyper_material_apply_plan(
	const FHyperAIMaterialApplyPlanRequest& Request)
{
	return FHyperAIStudioMaterialsContracts::BuildPlan(Request);
}

FHyperAIMaterialValidateReport UHyperAIStudioMaterialsToolset::hyper_material_validate(
	const FHyperAIMaterialValidateRequest& Request)
{
	using namespace HyperAIStudio::Materials::Private;
	FHyperAIMaterialValidateReport Report;
	Report.Capabilities = FHyperAIStudioMaterialsContracts::GetCapabilityMatrix();
	if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioMaterialsContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 2000
		|| Request.MaxOutputBytes < FHyperAIStudioMaterialsContracts::MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioMaterialsContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioMaterialsContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Issue/deadline/output bounds or expected_revision are invalid.");
		return Report;
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.MaxGameThreadMs) / 1000.0;
	FHyperAIStudioMaterialValueSnapshot Snapshot;
	if (!FHyperAIStudioMaterialsContracts::CaptureExact(Request.TargetPath, Request.TargetFamily,
		TEXT("loaded_only"), FHyperAIStudioMaterialsContracts::MaxNodes,
		FHyperAIStudioMaterialsContracts::MaxEdges, Request.MaxGameThreadMs,
		Snapshot, Report.Status, Report.Diagnostic))
		return Report;
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	const int32 EffectiveIssueLimit = FMath::Clamp(
		Request.MaxOutputBytes / 16384, 1, Request.MaxIssues);
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(Report.Issues, EffectiveIssueLimit, Report.bTruncated,
			TEXT("expected_revision_mismatch"), TEXT("error"), Request.TargetPath,
			FString(), -1, TEXT("Fresh loaded semantic CAS differs from expected_revision."));
	}
	bool bValidationTruncated = false;
	const int32 RemainingIssueCapacity = EffectiveIssueLimit - Report.Issues.Num();
	if (RemainingIssueCapacity > 0)
	{
		TArray<FHyperAIMaterialIssue> Validation =
			FHyperAIStudioMaterialsContracts::ValidateValueSnapshot(
				Snapshot, Request.bRequirePackageClean, RemainingIssueCapacity,
				bValidationTruncated, Deadline);
		Report.Issues.Append(Validation);
	}
	else bValidationTruncated = true;
	Report.bTruncated |= bValidationTruncated;
	for (const FHyperAIMaterialIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
	}
	Report.CustomHlslNodeIds = Snapshot.Record.CustomHlslNodeIds;
	if (UMaterialInterface* Material = Cast<UMaterialInterface>(FSoftObjectPath(Request.TargetPath).ResolveObject()))
	{
		// Stats and the preview need finished shaders; until then Stats.Status says compiling and validate can be re-run.
		Report.Stats = HyperAIStudio::Materials::Gate::ReadStats(*Material);
		FString PreviewError;
		if (Report.Stats.bAvailable && !FHyperAIStudioResultPreview::WriteAssetPreview(*Material, Report.PreviewImagePath, PreviewError))
		{
			Report.PreviewImagePath.Reset();
		}
	}
	Report.bOk = true;
	Report.bValid = Report.ErrorCount == 0 && !Report.bTruncated;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("Independent persisted semantic graph validation passed on one fresh exact loaded capture.")
		: TEXT("Independent validation found semantic, CAS, compile, dirty, or bound failures.");
	return Report;
}

FHyperAIMaterialInspectReport FHyperAIStudioMaterialsContracts::Inspect(const FHyperAIMaterialInspectRequest& Request)
{
	return UHyperAIStudioMaterialsToolset::hyper_material_inspect(Request);
}

FHyperAIMaterialValidateReport FHyperAIStudioMaterialsContracts::Validate(const FHyperAIMaterialValidateRequest& Request)
{
	return UHyperAIStudioMaterialsToolset::hyper_material_validate(Request);
}

FString FHyperAIStudioMaterialTypedPayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::PayloadTypeId;
}

FString FHyperAIStudioMaterialTypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioMaterialTypedPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Materials::Private;
	if (Operations.IsEmpty() || Operations.Num() > FHyperAIStudioMaterialsContracts::MaxOperations
		|| BaseRevision.Len() > 71 || SemanticFingerprint.Len() > 71 || AuthoringMode.Len() > 16)
		return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	int64 Bytes = 256ll + static_cast<int64>(BaseRevision.Len() + SemanticFingerprint.Len() + AuthoringMode.Len())
		* MaxUtf8BytesPerCharacter;
	for (const FHyperAIStudioMaterialBackendOperation& Operation : Operations)
	{
		if (Operation.TargetPath.Len() > FHyperAIStudioMaterialsContracts::MaxPathCharacters
			|| Operation.Nodes.Num() > FHyperAIStudioMaterialsContracts::MaxNodes
			|| Operation.Edges.Num() > FHyperAIStudioMaterialsContracts::MaxEdges)
			return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
		Bytes += static_cast<int64>(CanonicalBackendOperation(Operation).Len()) * MaxUtf8BytesPerCharacter;
		if (Bytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
			return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	}
	return static_cast<int32>(Bytes);
}

FString FHyperAIStudioMaterialTypedPayload::GetSemanticFingerprint() const
{
	const FString Recomputed = HyperAIStudio::Materials::Private::SealFingerprint(*this);
	return Recomputed == SemanticFingerprint ? Recomputed : FString();
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioMaterialTypedPayload::CloneImmutable() const
{
	// Every member is an owning value type, so this copy owns every operation, node, edge and parameter.
	TSharedRef<FHyperAIStudioMaterialTypedPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioMaterialTypedPayload, ESPMode::ThreadSafe>();
	Clone->Operations = Operations;
	Clone->BaseRevision = BaseRevision;
	Clone->AuthoringMode = AuthoringMode;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioMaterialResultPayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::ResultTypeId;
}

FString FHyperAIStudioMaterialResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::ResultSchemaFingerprint();
}

int32 FHyperAIStudioMaterialResultPayload::GetBoundedByteSize() const
{
	// Same UTF-16 estimate as the other packs: "completed" plus a content hash is exactly the host's 256-byte cap.
	const int64 Bytes = 96ll + 2ll * static_cast<int64>(Phase.Len() + Revision.Len());
	return Bytes > FHyperAIStudioDomainLimits::MaxResultBytes
		? FHyperAIStudioDomainLimits::MaxResultBytes + 1 : static_cast<int32>(Bytes);
}

namespace HyperAIStudio::Materials::Private
{
	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}
}

FString FHyperAIStudioMaterialInspectPayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioMaterialInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioMaterialInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Materials::Private::ClampBytes(
		128 + 4ll * (Request.TargetPath.Len() + Request.ComparePath.Len() + Request.Cursor.Len()));
}

FString FHyperAIStudioMaterialValidatePayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioMaterialValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioMaterialValidatePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::Materials::Private::ClampBytes(
		128 + 4ll * (Request.TargetPath.Len() + Request.ExpectedRevision.Len()));
}

FString FHyperAIStudioMaterialInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::InspectResultTypeId;
}

FString FHyperAIStudioMaterialInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioMaterialInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::Materials::Private;
	int64 Size = 1024;
	for (const FHyperAIMaterialNodeView& Node : Report.Target.Nodes) Size += 512 + 4ll * CanonicalNode(Node).Len();
	for (const FHyperAIMaterialEdgeView& Edge : Report.Target.Edges) Size += 64 + 4ll * CanonicalEdge(Edge).Len();
	return ClampBytes(Size);
}

FString FHyperAIStudioMaterialValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioMaterialsContracts::ValidateResultTypeId;
}

FString FHyperAIStudioMaterialValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioMaterialValidateResultPayload::GetBoundedByteSize() const
{
	int64 Size = 1024 + 4ll * Report.PreviewImagePath.Len();
	for (const FHyperAIMaterialIssue& Issue : Report.Issues) Size += 128 + 4ll * (Issue.Message.Len() + Issue.Code.Len());
	for (const FString& Error : Report.Stats.CompileErrors) Size += 16 + 4ll * Error.Len();
	return HyperAIStudio::Materials::Private::ClampBytes(Size);
}

FHyperAIStudioMaterialsDomainAdapter::FHyperAIStudioMaterialsDomainAdapter()
	: Descriptor(FHyperAIStudioMaterialsContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioMaterialsDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioMaterialsDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioMaterialsContracts;
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety,
		const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output,
		const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Materials adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioMaterialInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioMaterialInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioMaterialInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioMaterialValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioMaterialValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioMaterialValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit,
		Contracts::PayloadTypeId, Contracts::PayloadSchemaFingerprint()))
	{
		const FHyperAIStudioMaterialTypedPayload& Typed = static_cast<const FHyperAIStudioMaterialTypedPayload&>(Payload);
		if (Typed.GetSemanticFingerprint().IsEmpty())
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed material plan fingerprint drifted."));
		}
		return HyperAIStudio::Materials::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Materials adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioMaterialsFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioMaterialsContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioMaterialsFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != FHyperAIStudioMaterialsContracts::PayloadTypeId
		|| Request.GetSchemaFingerprint() != FHyperAIStudioMaterialsContracts::PayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed material request schema.");
		return false;
	}
	const FHyperAIStudioMaterialTypedPayload& Typed = static_cast<const FHyperAIStudioMaterialTypedPayload&>(Request);
	if (Typed.GetSemanticFingerprint().IsEmpty()
		|| Typed.Operations.ContainsByPredicate([](const FHyperAIStudioMaterialBackendOperation& Operation)
		{
			return !FHyperAIStudioMaterialsContracts::IsCanonicalProjectObjectPath(Operation.TargetPath);
		}))
	{
		OutError = TEXT("Fresh verifier rejected a target path or the semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = HyperAIStudio::Materials::Private::EffectTargetOf(Typed);
	return true;
}

bool FHyperAIStudioMaterialsFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	using namespace HyperAIStudio::Materials::Private;
	OutPostconditionHash.Reset();
	OutError.Reset();
	// The executor reports only that verification failed; the reason goes to the log.
	ON_SCOPE_EXIT
	{
		if (!OutError.IsEmpty())
		{
			UE_LOG(LogHyperAIStudioMaterials, Warning, TEXT("Material plan verification refused: %s"), *OutError);
		}
	};
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh material verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != FHyperAIStudioMaterialsContracts::ResultTypeId
		|| Result.GetSchemaFingerprint() != FHyperAIStudioMaterialsContracts::ResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioMaterialTypedPayload& Typed = static_cast<const FHyperAIStudioMaterialTypedPayload&>(Request);
	const FHyperAIStudioMaterialResultPayload& TypedResult = static_cast<const FHyperAIStudioMaterialResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !TypedResult.bValid
		|| !FHyperAIStudioMaterialsContracts::IsCanonicalSha256(TypedResult.Revision))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	// Content identity, not the compile-sensitive revision: shaders may finish between capture and here.
	const FString ContentKey = FHyperAIStudioMaterialsContracts::ComputePlanContentKey(Typed);
	if (ContentKey != TypedResult.Revision)
	{
		OutError = TEXT("A target changed between fresh capture and verification.");
		return false;
	}
	for (const FHyperAIStudioMaterialBackendOperation& Operation : Typed.Operations)
	{
		const UObject* Target = FSoftObjectPath(Operation.TargetPath).ResolveObject();
		if (!Target || Target->GetOutermost()->IsDirty())
		{
			OutError = FString::Printf(TEXT("%s is missing or still has unsaved changes after the save phase."), *Operation.TargetPath);
			return false;
		}
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.material.plan-postcondition.v2"));
	AppendToken(Canonical, Typed.SemanticFingerprint);
	AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!FHyperAIStudioMaterialsContracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioMaterialsRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FHyperAIStudioMaterialsRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioMaterialsRegistration::Shutdown()
{
	if (!bStarted) return;
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioMaterialsRegistration::IsRegistered() const
{
	return bOwnsRegistration && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioMaterialsToolset::StaticClass(),
			FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioMaterialsRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioMaterialsContracts::IsRegistrationAllowed(
		FHyperAIStudioMaterialsContracts::IsPendingTestRegistrationEnabled()))
	{
		UE_LOG(LogHyperAIStudioMaterials, Verbose,
			TEXT("Materials source cohort is not admitted; registration remains fail-closed."));
		return;
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("MaterialEditor")))
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioMaterialsDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioMaterialsContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	const auto Observe = []()
	{
		FString Missing;
		FHyperAIStudioTrustedProbeResult Observation;
		const bool bKinds = HyperAIStudio::Materials::Gate::ResolveAllKinds(Missing);
		Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("MaterialEditor"))
			&& GMaxRHIShaderPlatform != SP_NumPlatforms && bKinds;
		Observation.StatusCode = Observation.bReady ? TEXT("ready_loaded_only") : TEXT("material_editor_unavailable");
		Observation.Diagnostic = Observation.bReady
			? TEXT("MaterialEditor is loaded and every node catalog class resolves.")
			: FString(TEXT("MaterialEditor, the shader platform, or node classes are unavailable. ")) + Missing;
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioMaterialsToolset::StaticClass(),
		FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName(), Error);
	if (!bOwnsRegistration)
	{
		UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
	}
}

void FHyperAIStudioMaterialsRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsRegistration && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioMaterialsToolset::StaticClass(),
			FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsRegistration = false;
	}
	ProbePublisher.Stop();
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioMaterials, Error, TEXT("Materials adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
