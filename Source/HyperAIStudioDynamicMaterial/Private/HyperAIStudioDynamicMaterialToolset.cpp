// Games by Hyper 2026.

#include "HyperAIStudioDynamicMaterialToolset.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Components/DMMaterialComponent.h"
#include "Components/DMMaterialEffectStack.h"
#include "Components/DMMaterialLayer.h"
#include "Components/DMMaterialParameter.h"
#include "Components/DMMaterialProperty.h"
#include "Components/DMMaterialSlot.h"
#include "Components/DMMaterialStage.h"
#include "Components/DMMaterialStageBlend.h"
#include "Components/DMMaterialStageBlendFunction.h"
#include "Components/DMMaterialStageSource.h"
#include "Components/DMMaterialStageThroughput.h"
#include "Components/DMMaterialStageThroughputLayerBlend.h"
#include "Components/DMMaterialValue.h"
#include "Components/MaterialStageInputs/DMMSISlot.h"
#include "Components/MaterialStageInputs/DMMSIValue.h"
#include "Components/MaterialValues/DMMaterialValueFloat.h"
#include "Components/MaterialValues/DMMaterialValueBool.h"
#include "Components/MaterialValues/DMMaterialValueFloat1.h"
#include "Components/MaterialValues/DMMaterialValueFloat2.h"
#include "Components/MaterialValues/DMMaterialValueFloat3RGB.h"
#include "Components/MaterialValues/DMMaterialValueFloat3RPY.h"
#include "Components/MaterialValues/DMMaterialValueFloat3XYZ.h"
#include "Components/MaterialValues/DMMaterialValueFloat4.h"
#include "Components/MaterialValues/DMMaterialValueTexture.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Internationalization/Text.h"
#include "Interfaces/IPluginManager.h"
#include "Material/DynamicMaterialInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Model/DynamicMaterialModel.h"
#include "Model/DynamicMaterialModelEditorOnlyData.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDynamicMaterial, Log, All);

namespace HyperAIStudio::DynamicMaterial::Private
{
	constexpr int32 MaxUtf8BytesPerCharacter = 4;

	void AppendToken(FString& Out, const FString& Value)
	{
		Out += FString::FromInt(Value.Len()); Out += TEXT(":"); Out += Value; Out += TEXT("|");
	}

	FString BoolToken(const bool bValue) { return bValue ? TEXT("1") : TEXT("0"); }

	FString DoubleToken(const double Value)
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
		for (const UObject* Cursor = Object; Cursor; Cursor = Cursor->GetOuter())
		{
			if (!IsBeforeDeadline(Deadline) || Chain.Num() >= 32 || Seen.Contains(Cursor)) return false;
			Seen.Add(Cursor); Chain.Add(Cursor);
			if (Cursor->GetFName().ToString().Len() > MaxCharacters) return false;
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

	bool TryGetBoundedComponentPath(
		const UDMMaterialComponent* Component,
		const double Deadline,
		FString& OutPath)
	{
		OutPath.Reset();
		if (!Component || !IsBeforeDeadline(Deadline)) return false;
		TArray<FString, TInlineAllocator<32>> Parts;
		TSet<const UDMMaterialComponent*> Seen;
		for (const UDMMaterialComponent* Cursor = Component; Cursor;
			Cursor = Cursor->GetParentComponent())
		{
			if (!IsBeforeDeadline(Deadline) || Parts.Num() >= 32 || Seen.Contains(Cursor)) return false;
			Seen.Add(Cursor);
			const FString Part = Cursor->GetComponentPathComponent();
			if (Part.IsEmpty()
				|| Part.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
				return false;
			Parts.Add(Part);
		}
		for (int32 Index = Parts.Num() - 1; Index >= 0; --Index)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			if (!OutPath.IsEmpty()) OutPath.AppendChar(TEXT('.'));
			if (OutPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
				- Parts[Index].Len()) return false;
			OutPath += Parts[Index];
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
		if (WorstCaseValueBytes
				> FHyperAIStudioDynamicMaterialContracts::MaxSnapshotMaterializedBytes
			|| Out.Len() > FHyperAIStudioExtensionRuntime::MaxHashInputBytes
				- Value.Len() - 32)
			return false;
		const FString Prefix = FString::FromInt(Value.Len()) + TEXT(":");
		const FTCHARToUTF8 PrefixUtf8(*Prefix);
		const FTCHARToUTF8 ValueUtf8(*Value);
		const int64 Delta = static_cast<int64>(PrefixUtf8.Length())
			+ static_cast<int64>(ValueUtf8.Length()) + 1;
		if (Delta < 0
			|| InOutUtf8Bytes > FHyperAIStudioDynamicMaterialContracts::MaxSnapshotMaterializedBytes - Delta
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
			if (!FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(Hash)) return false;
			Hashes.Add(Hash);
		}
		Hashes.Sort();
		for (const FString& Hash : Hashes)
			if (!TryAppendBoundedCanonicalToken(Out, InOutUtf8Bytes, Hash, Deadline)) return false;
		return true;
	}

	bool CaptureBoundedSimpleText(const FText& Text, const int32 MaxCharacters, FString& Out)
	{
		Out.Reset();
		if (Text.IsEmpty()) { Out = TEXT("text:empty"); return true; }
		const FString* Source = FTextInspector::GetSourceString(Text);
		if (!Source || Source->Len() > MaxCharacters) return false;
		const FTextId TextId = FTextInspector::GetTextId(Text);
		const bool bInitializedFromString = Text.IsInitializedFromString();
		const bool bCultureInvariant = Text.IsCultureInvariant();
		const bool bStableLocalizedIdentity =
			!TextId.GetNamespace().IsEmpty() && !TextId.GetKey().IsEmpty();
		if (Text.IsFromStringTable()
			|| (!bInitializedFromString && !bCultureInvariant && !bStableLocalizedIdentity))
			return false;
		AppendToken(Out, TEXT("bounded_simple_text.v1"));
		AppendToken(Out, bInitializedFromString ? TEXT("initialized_from_string")
			: (bCultureInvariant ? TEXT("culture_invariant") : TEXT("localized_identity")));
		AppendToken(Out, *Source);
		AppendToken(Out, FString::Printf(TEXT("%08x"), GetTypeHash(TextId.GetNamespace())));
		AppendToken(Out, FString::Printf(TEXT("%08x"), GetTypeHash(TextId.GetKey())));
		AppendToken(Out, FString::Printf(TEXT("%08x"), FTextInspector::GetFlags(Text)));
		return Out.Len() <= MaxCharacters + 128;
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
		return Family == TEXT("dynamic_material_instance") || Family == TEXT("dynamic_material_model");
	}

	bool DoesBuiltInPropertyRequireAlphaValue(const int32 Property)
	{
		const EDMMaterialPropertyType Type = static_cast<EDMMaterialPropertyType>(Property);
		return Type > EDMMaterialPropertyType::None
			&& Type < EDMMaterialPropertyType::Any
			&& Type != EDMMaterialPropertyType::Custom1
			&& Type != EDMMaterialPropertyType::Custom2
			&& Type != EDMMaterialPropertyType::Custom3
			&& Type != EDMMaterialPropertyType::Custom4;
	}

	bool IsAllowedScope(const FString& Scope)
	{
		return Scope == TEXT("loaded_only") || Scope == TEXT("on_disk_index");
	}

	void AddIssue(
		TArray<FHyperAIDynamicMaterialIssue>& Issues,
		const int32 Limit,
		bool& bTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& AssetPath,
		const FString& ComponentPath,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Limit) { bTruncated = true; return; }
		FHyperAIDynamicMaterialIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = FString(Code).Left(128); Issue.Severity = FString(Severity).Left(32);
		Issue.AssetPath = Clip(AssetPath, 256); Issue.ComponentPath = Clip(ComponentPath, 256);
		Issue.OperationIndex = OperationIndex; Issue.Message = Clip(Message);
	}

	bool HasErrors(const TArray<FHyperAIDynamicMaterialIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const auto& Issue){ return Issue.Severity == TEXT("error"); });
	}

	bool IsMediaBridgeClassPath(const FString& ClassPath)
	{
		return ClassPath == TEXT("/Script/DynamicMaterialMediaStreamBridge.DMMaterialValueMediaStream")
			|| ClassPath == TEXT("/Script/DynamicMaterialMediaStreamBridge.DMMaterialValueMediaStreamDynamic");
	}

	FHyperAIDynamicMaterialValueView CaptureValue(
		UDMMaterialValue* Value,
		const double Deadline,
		bool& bOutPathComplete)
	{
		FHyperAIDynamicMaterialValueView View;
		bOutPathComplete = TryGetBoundedComponentPath(Value, Deadline, View.ComponentPath)
			&& TryGetBoundedObjectPath(Value->GetClass(),
				FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, View.ClassPath);
		View.LifetimeState = static_cast<int32>(Value->GetComponentState());
		View.ValueType = static_cast<int32>(Value->GetType());
		View.ParameterName = Value->GetMaterialParameterName().ToString();
		View.ParameterGroup = static_cast<int32>(Value->GetParameterGroup());
		if (const UDMMaterialParameter* Parameter = Value->GetParameter())
		{
			View.bHasExplicitParameter = true;
			bOutPathComplete &= TryGetBoundedComponentPath(
				Parameter, Deadline, View.ParameterComponentPath);
			bOutPathComplete &= TryGetBoundedObjectPath(Parameter->GetClass(),
				FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
				Deadline, View.ParameterClassPath);
			View.ParameterLifetimeState = static_cast<int32>(Parameter->GetComponentState());
			if (const UDMMaterialComponent* ParameterParent = Parameter->GetParentComponent())
				bOutPathComplete &= TryGetBoundedComponentPath(
					ParameterParent, Deadline, View.ParameterParentComponentPath);
			View.ExplicitParameterName = Parameter->GetParameterName().ToString();
		}
		View.bLocal = Value->IsLocal();
		View.bExposed = Value->GetShouldExposeParameter();
		if (Value->GetClass() == UDMMaterialValueBool::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueBool>(Value);
			View.ValueKind = TEXT("bool"); View.bBoolValue = Typed->GetValue();
			View.bDefaultBoolValue = Typed->GetDefaultValue();
		}
		else if (Value->GetClass() == UDMMaterialValueFloat1::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat1>(Value);
			View.ValueKind = TEXT("scalar"); View.ScalarValue = Typed->GetValue();
			View.DefaultScalarValue = Typed->GetDefaultValue();
		}
		else if (Value->GetClass() == UDMMaterialValueFloat2::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat2>(Value);
			const FVector2D V = Typed->GetValue();
			const FVector2D D = Typed->GetDefaultValue();
			View.ValueKind = TEXT("vector2"); View.VectorValue = FVector4(V.X, V.Y, 0, 0);
			View.DefaultVectorValue = FVector4(D.X, D.Y, 0, 0);
		}
		else if (Value->GetClass() == UDMMaterialValueFloat3XYZ::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat3XYZ>(Value);
			const FVector V = Typed->GetValue();
			const FVector D = Typed->GetDefaultValue();
			View.ValueKind = TEXT("vector3"); View.VectorValue = FVector4(V.X, V.Y, V.Z, 0);
			View.DefaultVectorValue = FVector4(D.X, D.Y, D.Z, 0);
		}
		else if (Value->GetClass() == UDMMaterialValueFloat3RPY::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat3RPY>(Value);
			const FRotator V = Typed->GetValue();
			const FRotator D = Typed->GetDefaultValue();
			View.ValueKind = TEXT("rotator"); View.VectorValue = FVector4(V.Roll, V.Pitch, V.Yaw, 0);
			View.DefaultVectorValue = FVector4(D.Roll, D.Pitch, D.Yaw, 0);
		}
		else if (Value->GetClass() == UDMMaterialValueFloat3RGB::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat3RGB>(Value);
			const FLinearColor V = Typed->GetValue();
			const FLinearColor D = Typed->GetDefaultValue();
			View.ValueKind = TEXT("color"); View.VectorValue = FVector4(V.R, V.G, V.B, V.A);
			View.DefaultVectorValue = FVector4(D.R, D.G, D.B, D.A);
		}
		else if (Value->GetClass() == UDMMaterialValueFloat4::StaticClass())
		{
			const auto* Typed = CastChecked<UDMMaterialValueFloat4>(Value);
			const FLinearColor V = Typed->GetValue();
			const FLinearColor D = Typed->GetDefaultValue();
			View.ValueKind = TEXT("color"); View.VectorValue = FVector4(V.R, V.G, V.B, V.A);
			View.DefaultVectorValue = FVector4(D.R, D.G, D.B, D.A);
		}
		else if (Value->GetClass() == UDMMaterialValueTexture::StaticClass())
		{
			const auto* Texture = CastChecked<UDMMaterialValueTexture>(Value);
			View.ValueKind = TEXT("texture");
			if (Texture->GetValue()) bOutPathComplete &= TryGetBoundedObjectPath(
				Texture->GetValue(), FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
				Deadline, View.ObjectValuePath);
			if (Texture->GetDefaultValue())
				bOutPathComplete &= TryGetBoundedObjectPath(Texture->GetDefaultValue(),
					FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
					Deadline, View.DefaultObjectValuePath);
		}
		else View.ValueKind = TEXT("unsupported");
		if (const auto* FloatValue = Cast<UDMMaterialValueFloat>(Value))
		{
			View.bHasValueRange = FloatValue->HasValueRange();
			const FFloatInterval& Range = FloatValue->GetValueRange();
			View.ValueRangeMin = static_cast<double>(Range.Min);
			View.ValueRangeMax = static_cast<double>(Range.Max);
		}
		View.bMediaBridgeValue = IsMediaBridgeClassPath(View.ClassPath);
		if (View.bMediaBridgeValue)
		{
			View.ValueKind = TEXT("media_stream_bridge");
			// The optional bridge is intentionally not a hard dependency. Base DynamicMaterial exposes the
			// generated texture but not the UMediaStream identity, so this evidence is explicitly incomplete.
			View.bMediaSourceIdentityKnown = false;
		}
		View.bSemanticProjectionComplete = bOutPathComplete && Value->IsComponentValid()
			&& View.ValueKind != TEXT("unsupported") && !View.bMediaBridgeValue
			&& (!View.bHasExplicitParameter
				|| (View.ParameterClassPath == UDMMaterialParameter::StaticClass()->GetPathName()
					&& View.ParameterParentComponentPath == View.ComponentPath
					&& View.ParameterLifetimeState >= 0));
		return View;
	}

	FString CanonicalValue(const FHyperAIDynamicMaterialValueView& Value)
	{
		FString Out;
		AppendToken(Out, Value.ComponentPath); AppendToken(Out, Value.ClassPath);
		AppendToken(Out, FString::FromInt(Value.LifetimeState));
		AppendToken(Out, Value.ValueKind); AppendToken(Out, FString::FromInt(Value.ValueType));
		AppendToken(Out, Value.ParameterName);
		AppendToken(Out, FString::FromInt(Value.ParameterGroup));
		AppendToken(Out, BoolToken(Value.bHasExplicitParameter));
		AppendToken(Out, Value.ParameterComponentPath);
		AppendToken(Out, Value.ParameterClassPath);
		AppendToken(Out, FString::FromInt(Value.ParameterLifetimeState));
		AppendToken(Out, Value.ParameterParentComponentPath);
		AppendToken(Out, Value.ExplicitParameterName);
		AppendToken(Out, BoolToken(Value.bLocal)); AppendToken(Out, BoolToken(Value.bExposed));
		AppendToken(Out, BoolToken(Value.bBoolValue)); AppendToken(Out, DoubleToken(Value.ScalarValue));
		AppendToken(Out, DoubleToken(Value.VectorValue.X)); AppendToken(Out, DoubleToken(Value.VectorValue.Y));
		AppendToken(Out, DoubleToken(Value.VectorValue.Z)); AppendToken(Out, DoubleToken(Value.VectorValue.W));
		AppendToken(Out, BoolToken(Value.bDefaultBoolValue));
		AppendToken(Out, DoubleToken(Value.DefaultScalarValue));
		AppendToken(Out, DoubleToken(Value.DefaultVectorValue.X));
		AppendToken(Out, DoubleToken(Value.DefaultVectorValue.Y));
		AppendToken(Out, DoubleToken(Value.DefaultVectorValue.Z));
		AppendToken(Out, DoubleToken(Value.DefaultVectorValue.W));
		AppendToken(Out, BoolToken(Value.bHasValueRange));
		AppendToken(Out, DoubleToken(Value.ValueRangeMin)); AppendToken(Out, DoubleToken(Value.ValueRangeMax));
		AppendToken(Out, Value.ObjectValuePath); AppendToken(Out, Value.DefaultObjectValuePath);
		AppendToken(Out, BoolToken(Value.bMediaBridgeValue));
		AppendToken(Out, BoolToken(Value.bMediaSourceIdentityKnown));
		AppendToken(Out, BoolToken(Value.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalConnector(const FHyperAIDynamicMaterialConnectorView& Connector)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Connector.Index));
		AppendToken(Out, Connector.Name);
		AppendToken(Out, FString::FromInt(Connector.ValueType));
		return Out;
	}

	FString CanonicalChannel(const FHyperAIDynamicMaterialConnectionChannelView& Channel)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Channel.SourceIndex));
		AppendToken(Out, FString::FromInt(Channel.MaterialProperty));
		AppendToken(Out, FString::FromInt(Channel.OutputIndex));
		AppendToken(Out, FString::FromInt(Channel.OutputChannel));
		return Out;
	}

	FString CanonicalConnection(const FHyperAIDynamicMaterialConnectionView& Connection)
	{
		FString Out;
		AppendToken(Out, FString::FromInt(Connection.InputIndex));
		AppendToken(Out, TEXT("channels"));
		AppendToken(Out, FString::FromInt(Connection.Channels.Num()));
		for (const FHyperAIDynamicMaterialConnectionChannelView& Channel : Connection.Channels)
			AppendToken(Out, CanonicalChannel(Channel));
		return Out;
	}

	FString CanonicalComponent(const FHyperAIDynamicMaterialComponentView& Component)
	{
		FString Out;
		AppendToken(Out, Component.ComponentPath); AppendToken(Out, Component.ClassPath);
		AppendToken(Out, FString::FromInt(Component.LifetimeState));
		AppendToken(Out, Component.Description);
		AppendToken(Out, BoolToken(Component.bInputRequired));
		AppendToken(Out, BoolToken(Component.bAllowsNestedInputs));
		AppendToken(Out, Component.ValueComponentPath);
		AppendToken(Out, Component.SlotComponentPath);
		AppendToken(Out, FString::FromInt(Component.MaterialProperty));
		AppendToken(Out, FString::FromInt(Component.ChannelOverride));
		AppendToken(Out, TEXT("editable_properties"));
		AppendToken(Out, FString::FromInt(Component.EditablePropertyNames.Num()));
		for (const FString& Property : Component.EditablePropertyNames) AppendToken(Out, Property);
		AppendToken(Out, TEXT("input_connectors"));
		AppendToken(Out, FString::FromInt(Component.InputConnectors.Num()));
		for (const FHyperAIDynamicMaterialConnectorView& Connector : Component.InputConnectors)
			AppendToken(Out, CanonicalConnector(Connector));
		AppendToken(Out, TEXT("output_connectors"));
		AppendToken(Out, FString::FromInt(Component.OutputConnectors.Num()));
		for (const FHyperAIDynamicMaterialConnectorView& Connector : Component.OutputConnectors)
			AppendToken(Out, CanonicalConnector(Connector));
		AppendToken(Out, BoolToken(Component.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalMaterialProperty(const FHyperAIDynamicMaterialPropertyView& Property)
	{
		FString Out;
		AppendToken(Out, Property.ComponentPath); AppendToken(Out, Property.ClassPath);
		AppendToken(Out, FString::FromInt(Property.LifetimeState));
		AppendToken(Out, FString::FromInt(Property.MaterialProperty));
		AppendToken(Out, BoolToken(Property.bEnabled));
		AppendToken(Out, BoolToken(Property.bMaterialPin));
		AppendToken(Out, FString::FromInt(Property.InputConnectorType));
		AppendToken(Out, Property.OutputProcessorPath);
		AppendToken(Out, BoolToken(Property.bHasAlphaValueComponent));
		AppendToken(Out, Property.AlphaValueComponentIdentity);
		AppendToken(Out, TEXT("input_channels"));
		AppendToken(Out, FString::FromInt(Property.InputChannels.Num()));
		for (const FHyperAIDynamicMaterialConnectionChannelView& Channel : Property.InputChannels)
			AppendToken(Out, CanonicalChannel(Channel));
		AppendToken(Out, Property.SlotComponentPath);
		AppendToken(Out, BoolToken(Property.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalSlot(const FHyperAIDynamicMaterialSlotView& Slot)
	{
		FString Out;
		AppendToken(Out, Slot.ComponentPath); AppendToken(Out, Slot.ClassPath);
		AppendToken(Out, FString::FromInt(Slot.LifetimeState));
		AppendToken(Out, FString::FromInt(Slot.SlotIndex));
		AppendToken(Out, TEXT("output_connector_type_sets"));
		AppendToken(Out, FString::FromInt(Slot.OutputConnectorTypeSets.Num()));
		for (const FString& ConnectorTypes : Slot.OutputConnectorTypeSets)
			AppendToken(Out, ConnectorTypes);
		TArray<FString> References = Slot.ReferencedBySlotCounts;
		References.Sort();
		AppendToken(Out, TEXT("referenced_by_slot_counts"));
		AppendToken(Out, FString::FromInt(References.Num()));
		for (const FString& Reference : References) AppendToken(Out, Reference);
		AppendToken(Out, BoolToken(Slot.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalStage(const FHyperAIDynamicMaterialStageView& Stage)
	{
		FString Out;
		AppendToken(Out, Stage.ComponentPath); AppendToken(Out, Stage.ClassPath);
		AppendToken(Out, FString::FromInt(Stage.LifetimeState));
		AppendToken(Out, FString::FromInt(Stage.StageIndex));
		AppendToken(Out, Stage.SourceClassPath);
		AppendToken(Out, FString::FromInt(Stage.StageType)); AppendToken(Out, BoolToken(Stage.bEnabled));
		AppendToken(Out, BoolToken(Stage.bCanChangeSource));
		AppendToken(Out, TEXT("source"));
		AppendToken(Out, CanonicalComponent(Stage.Source));
		AppendToken(Out, TEXT("inputs"));
		AppendToken(Out, FString::FromInt(Stage.Inputs.Num()));
		for (const FHyperAIDynamicMaterialComponentView& Input : Stage.Inputs)
			AppendToken(Out, CanonicalComponent(Input));
		AppendToken(Out, TEXT("input_connections"));
		AppendToken(Out, FString::FromInt(Stage.InputConnections.Num()));
		for (const FHyperAIDynamicMaterialConnectionView& Connection : Stage.InputConnections)
			AppendToken(Out, CanonicalConnection(Connection));
		AppendToken(Out, BoolToken(Stage.bSemanticProjectionComplete));
		return Out;
	}

	FString CanonicalLayer(const FHyperAIDynamicMaterialLayerView& Layer)
	{
		FString Out;
		AppendToken(Out, Layer.ComponentPath); AppendToken(Out, Layer.ClassPath);
		AppendToken(Out, FString::FromInt(Layer.LifetimeState));
		AppendToken(Out, Layer.Name);
		AppendToken(Out, FString::FromInt(Layer.SlotIndex)); AppendToken(Out, FString::FromInt(Layer.LayerIndex));
		AppendToken(Out, FString::FromInt(Layer.MaterialProperty)); AppendToken(Out, BoolToken(Layer.bEnabled));
		AppendToken(Out, BoolToken(Layer.bTextureUVLinkEnabled));
		AppendToken(Out, Layer.EffectStackComponentPath);
		AppendToken(Out, Layer.EffectStackClassPath);
		AppendToken(Out, FString::FromInt(Layer.EffectStackLifetimeState));
		AppendToken(Out, BoolToken(Layer.bEffectStackEnabled));
		AppendToken(Out, FString::FromInt(Layer.EffectCount));
		AppendToken(Out, BoolToken(Layer.bSemanticProjectionComplete));
		TArray<FString> Stages; for (const auto& Stage : Layer.Stages) Stages.Add(CanonicalStage(Stage));
		Stages.Sort();
		AppendToken(Out, TEXT("stages")); AppendToken(Out, FString::FromInt(Stages.Num()));
		for (const FString& Stage : Stages) AppendToken(Out, Stage);
		return Out;
	}

	bool TryCanonicalRecord(
		const FHyperAIDynamicMaterialAssetRecord& Record,
		const double Deadline,
		FString& Out)
	{
		Out.Reset();
		int64 Utf8Bytes = 0;
		auto Add = [&](const FString& Value)
		{
			return TryAppendBoundedCanonicalToken(Out, Utf8Bytes, Value, Deadline);
		};
		if (!Add(TEXT("hyperai.dynamic_material.snapshot.v2"))
			|| !Add(Record.Family) || !Add(Record.AssetPath) || !Add(Record.ClassPath)
			|| !Add(Record.ModelPath) || !Add(Record.ModelClassPath)
			|| !Add(Record.AssociatedInstancePath)
			|| !Add(Record.GeneratedMaterialPath) || !Add(Record.GeneratedMaterialStateId)
			|| !Add(BoolToken(Record.bLoaded)) || !Add(Record.DiskExistence)
			|| !Add(BoolToken(Record.bExistsOnDisk))
			|| !Add(BoolToken(Record.bPackageDirty))
			|| !Add(BoolToken(Record.bGeneratedMaterialPackageDirty))
			|| !Add(BoolToken(Record.bModelValid))
			|| !Add(BoolToken(Record.bEditorModelDataAvailable))
			|| !Add(FString::FromInt(Record.EditorState))
			|| !Add(BoolToken(Record.bBuildRequested))
			|| !Add(BoolToken(Record.bNeedsWizard))
			|| !Add(BoolToken(Record.bPreviewModified))
			|| !Add(FString::FromInt(Record.MaterialDomain))
			|| !Add(FString::FromInt(Record.BlendMode))
			|| !Add(FString::FromInt(Record.ShadingModel))
			|| !Add(FString::FromInt(Record.UsageFlags))
			|| !Add(FString::FromInt(Record.GeneralFlags))
			|| !Add(FString::FromInt(Record.LightingFlags))
			|| !Add(FString::FromInt(Record.TranslucencyFlags))
			|| !Add(FString::FromInt(Record.MotionFlags))
			|| !Add(FString::FromInt(Record.ForwardRendererFlags))
			|| !Add(DoubleToken(Record.OpacityMaskClipValue))
			|| !Add(DoubleToken(Record.DisplacementCenter))
			|| !Add(DoubleToken(Record.DisplacementMagnitude))
			|| !Add(BoolToken(Record.bCompileStateKnown)) || !Add(BoolToken(Record.bCompiling))
			|| !Add(BoolToken(Record.bCompileError))
			|| !Add(FString::Printf(TEXT("%lld"), Record.DiskSize))
			|| !Add(Record.ReferenceEvidence)
			|| !Add(Record.ComponentProjectionEvidence)
			|| !Add(BoolToken(Record.bMutationRevisionComplete)))
			return false;

		auto Identity = [](const FString& Value) { return Value; };
		if (!Add(TEXT("mutation_revision_blockers"))
			|| !Add(FString::FromInt(Record.MutationRevisionBlockers.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.MutationRevisionBlockers,
				Identity, Deadline)
			|| !Add(BoolToken(Record.bComponentOwnershipProjectionComplete))
			|| !Add(TEXT("component_ownership_identities"))
			|| !Add(FString::FromInt(Record.ComponentOwnershipIdentities.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.ComponentOwnershipIdentities,
				Identity, Deadline)
			|| !Add(TEXT("values")) || !Add(FString::FromInt(Record.Values.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.Values,
				[](const FHyperAIDynamicMaterialValueView& Value) { return CanonicalValue(Value); },
				Deadline)
			|| !Add(TEXT("material_properties"))
			|| !Add(FString::FromInt(Record.MaterialProperties.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.MaterialProperties,
				[](const FHyperAIDynamicMaterialPropertyView& Property)
				{
					return CanonicalMaterialProperty(Property);
				}, Deadline)
			|| !Add(TEXT("slots")) || !Add(FString::FromInt(Record.Slots.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.Slots,
				[](const FHyperAIDynamicMaterialSlotView& Slot) { return CanonicalSlot(Slot); },
				Deadline)
			|| !Add(TEXT("runtime_component_identities"))
			|| !Add(FString::FromInt(Record.RuntimeComponentIdentities.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.RuntimeComponentIdentities,
				Identity, Deadline)
			|| !Add(TEXT("layers")) || !Add(FString::FromInt(Record.Layers.Num()))
			|| !TryAppendSortedElementHashes(Out, Utf8Bytes, Record.Layers,
				[](const FHyperAIDynamicMaterialLayerView& Layer) { return CanonicalLayer(Layer); },
				Deadline)
			|| !Add(FString::FromInt(Record.ValueCount))
			|| !Add(FString::FromInt(Record.MaterialPropertyCount))
			|| !Add(FString::FromInt(Record.SlotCount))
			|| !Add(FString::FromInt(Record.LayerCount))
			|| !Add(FString::FromInt(Record.StageCount))
			|| !Add(BoolToken(Record.bDetailsTruncated)))
			return false;
		return IsBeforeDeadline(Deadline);
	}

	FString CanonicalOperation(const FHyperAIStudioDynamicMaterialBackendOperation& Operation)
	{
		FString Out;
		AppendToken(Out, Operation.Type); AppendToken(Out, Operation.TargetPath);
		AppendToken(Out, Operation.TargetFamily); AppendToken(Out, Operation.ExpectedRevision);
		AppendToken(Out, Operation.ComponentPath); AppendToken(Out, BoolToken(Operation.bBoolValue));
		AppendToken(Out, DoubleToken(Operation.ScalarValue));
		AppendToken(Out, DoubleToken(Operation.VectorValue.X)); AppendToken(Out, DoubleToken(Operation.VectorValue.Y));
		AppendToken(Out, DoubleToken(Operation.VectorValue.Z)); AppendToken(Out, DoubleToken(Operation.VectorValue.W));
		return Out;
	}

	UE::AssetRegistry::EExists CaptureDisk(
		const FString& Path,
		FHyperAIDynamicMaterialAssetRecord& Record,
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
		Record.DiskExistence =
			FHyperAIStudioDynamicMaterialContracts::ClassifyAssetRegistryExistence(State);
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
				FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
				Deadline, LoadedPath)
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

	UDynamicMaterialModel* ResolveModel(UObject* Object)
	{
		if (auto* Instance = Cast<UDynamicMaterialInstance>(Object)) return Instance->GetMaterialModel();
		return Cast<UDynamicMaterialModel>(Object);
	}

	bool CaptureClosedConnectorArray(
		const TArray<FDMMaterialStageConnector>& Connectors,
		TArray<FHyperAIDynamicMaterialConnectorView>& OutConnectors,
		TFunctionRef<bool()> ConsumeWork)
	{
		bool bComplete = Connectors.Num()
			<= FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent;
		const int32 Limit = FMath::Min(Connectors.Num(),
			FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			if (!ConsumeWork()) return false;
			const FDMMaterialStageConnector& Connector = Connectors[Index];
			FHyperAIDynamicMaterialConnectorView& View = OutConnectors.AddDefaulted_GetRef();
			View.Index = Connector.Index;
			FString Name;
			const bool bTextComplete = CaptureBoundedSimpleText(
				Connector.Name,
				FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters,
				Name);
			View.Name = MoveTemp(Name);
			bComplete &= bTextComplete;
			View.ValueType = static_cast<int32>(Connector.Type);
		}
		return bComplete;
	}

	bool CaptureClosedStageComponent(
		UDMMaterialStageSource* Component,
		FHyperAIDynamicMaterialComponentView& Out,
		const double Deadline,
		TFunctionRef<bool()> ConsumeWork,
		TFunctionRef<bool(UDMMaterialValue*)> CaptureReferencedValue)
	{
		if (!Component || !ConsumeWork()) return false;
		bool bComplete = Component->IsComponentValid();
		FString ComponentPath;
		FString ClassPath;
		bComplete &= TryGetBoundedComponentPath(Component, Deadline, ComponentPath);
		bComplete &= TryGetBoundedObjectPath(Component->GetClass(),
			FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, ClassPath);
		FString Description;
		const bool bDescriptionComplete = CaptureBoundedSimpleText(
			Component->GetComponentDescription(),
			FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters,
			Description);
		Out.ComponentPath = Clip(ComponentPath,
			FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters);
		Out.ClassPath = Clip(ClassPath, FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters);
		Out.Description = Clip(Description,
			FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters);
		bComplete &= !ComponentPath.IsEmpty()
			&& ComponentPath.Len() <= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
			&& ClassPath.Len() <= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
			&& bDescriptionComplete;
		Out.LifetimeState = static_cast<int32>(Component->GetComponentState());

		TSet<FName> ClosedEditableProperties;
		bool bClosedClass = false;
		if (Component->GetClass() == UDMMaterialStageInputValue::StaticClass())
		{
			bClosedClass = true;
			ClosedEditableProperties.Add(UDMMaterialStageInputValue::GetValuePropertyName());
			UDMMaterialValue* Value = CastChecked<UDMMaterialStageInputValue>(Component)->GetValue();
			if (Value)
			{
				bComplete &= TryGetBoundedComponentPath(
					Value, Deadline, Out.ValueComponentPath);
				bComplete &= CaptureReferencedValue(Value);
			}
			else bComplete = false;
		}
		else if (Component->GetClass() == UDMMaterialStageInputSlot::StaticClass())
		{
			// Cross-slot output connector maps and reference associations are outside this
			// closed value-source subset. Disclose identity, but never issue complete CAS.
			bClosedClass = false;
			ClosedEditableProperties.Add(TEXT("Slot"));
			ClosedEditableProperties.Add(TEXT("MaterialProperty"));
			const auto* SlotInput = CastChecked<UDMMaterialStageInputSlot>(Component);
			if (const UDMMaterialSlot* Slot = SlotInput->GetSlot())
				bComplete &= TryGetBoundedComponentPath(
					Slot, Deadline, Out.SlotComponentPath);
			else bComplete = false;
			Out.MaterialProperty = static_cast<int32>(SlotInput->GetMaterialProperty());
		}
		else if (const auto* Blend = Cast<UDMMaterialStageBlend>(Component))
		{
			// Every concrete UE 5.8 blend source is function- or subclass-backed and carries
			// semantics outside the base channel override. Capture the public field for
			// diagnostics, but keep the revision incomplete.
			bClosedClass = false;
			ClosedEditableProperties.Add(TEXT("BaseChannelOverride"));
			Out.ChannelOverride = static_cast<int32>(Blend->GetBaseChannelOverride());
		}
		bComplete &= bClosedClass;

		const TArray<FName>& EditableProperties = Component->GetEditableProperties();
		if (EditableProperties.Num()
			> FHyperAIStudioDynamicMaterialContracts::MaxEditablePropertiesPerComponent)
			bComplete = false;
		const int32 EditableLimit = FMath::Min(EditableProperties.Num(),
			FHyperAIStudioDynamicMaterialContracts::MaxEditablePropertiesPerComponent);
		for (int32 Index = 0; Index < EditableLimit; ++Index)
		{
			if (!ConsumeWork()) return false;
			const FString Name = EditableProperties[Index].ToString();
			Out.EditablePropertyNames.Add(Clip(Name, 256));
			bComplete &= Name.Len() <= 256
				&& ClosedEditableProperties.Contains(EditableProperties[Index]);
		}

		bComplete &= CaptureClosedConnectorArray(
			Component->GetOutputConnectors(), Out.OutputConnectors, ConsumeWork);
		if (const auto* Throughput = Cast<UDMMaterialStageThroughput>(Component))
		{
			Out.bInputRequired = Throughput->IsInputRequired();
			Out.bAllowsNestedInputs = Throughput->AllowsNestedInputs();
			bComplete &= CaptureClosedConnectorArray(
				Throughput->GetInputConnectors(), Out.InputConnectors, ConsumeWork);
		}
		Out.bSemanticProjectionComplete = bComplete;
		return true;
	}

	bool FillLoaded(
		UObject* Object,
		const FString& Family,
		const int32 MaxComponents,
		const double Deadline,
		FHyperAIStudioDynamicMaterialValueSnapshot& Snapshot,
		FString& OutStatus,
		FString& OutDiagnostic)
		{
			FHyperAIDynamicMaterialAssetRecord& Record = Snapshot.Record;
			bool bComplete = true;
			int64 MaterializedBytes = 16 * 1024;
			auto ConsumeMaterialized = [&](const FString& Canonical)
			{
					const int64 WorstCaseBytes = 256ll
						+ static_cast<int64>(Canonical.Len()) * MaxUtf8BytesPerCharacter;
					if (!IsBeforeDeadline(Deadline)
						|| WorstCaseBytes
							> FHyperAIStudioDynamicMaterialContracts::MaxSnapshotMaterializedBytes)
				{
					Record.bDetailsTruncated = true;
					bComplete = false;
					return false;
				}
				const FTCHARToUTF8 Utf8(*Canonical);
				const int64 Delta = 256ll + static_cast<int64>(Utf8.Length());
				if (Delta > FHyperAIStudioDynamicMaterialContracts::MaxSnapshotMaterializedBytes
					|| MaterializedBytes
						> FHyperAIStudioDynamicMaterialContracts::MaxSnapshotMaterializedBytes - Delta)
				{
					Record.bDetailsTruncated = true;
					bComplete = false;
					return false;
				}
				MaterializedBytes += Delta;
				return true;
			};
			Record.Family = Family;
		bComplete &= TryGetBoundedObjectPath(Object,
			FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, Record.AssetPath);
		bComplete &= TryGetBoundedObjectPath(Object->GetClass(),
			FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, Record.ClassPath);
		Record.bLoaded = true; Record.bPackageDirty = Object->GetOutermost()->IsDirty();
		CaptureDisk(Record.AssetPath, Record, bComplete, Object, Deadline);
		UDynamicMaterialModel* Model = ResolveModel(Object);
		if (!Model)
		{
			OutStatus = TEXT("dynamic_model_missing"); OutDiagnostic = TEXT("Exact target has no resolvable DynamicMaterial model.");
			return false;
		}
		bComplete &= TryGetBoundedObjectPath(Model,
			FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, Record.ModelPath);
		bComplete &= TryGetBoundedObjectPath(Model->GetClass(),
			FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters, Deadline, Record.ModelClassPath);
		if (UDynamicMaterialInstance* AssociatedInstance = Model->GetDynamicMaterialInstance())
			bComplete &= TryGetBoundedObjectPath(AssociatedInstance,
				FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
				Deadline, Record.AssociatedInstancePath);
		if (Model->GetClass() != UDynamicMaterialModel::StaticClass()
			|| (Family == TEXT("dynamic_material_instance")
				&& Record.AssociatedInstancePath != Record.AssetPath))
			bComplete = false;
		Record.bModelValid = Model->IsModelValid();
		Record.bPreviewModified = Model->IsPreviewModified();
		if (!Record.bModelValid) bComplete = false;
		const TArray<UDMMaterialValue*>& LocalValues = Model->GetValues();
		static const TArray<FName> GlobalValueNames = {
			TEXT("GlobalBaseColorValue"), TEXT("GlobalEmissiveColorValue"), TEXT("GlobalOpacityValue"),
			TEXT("GlobalMetallicValue"), TEXT("GlobalSpecularValue"), TEXT("GlobalRoughnessValue"),
			TEXT("GlobalNormalValue"), TEXT("GlobalAnisotropyValue"), TEXT("GlobalWorldPositionOffsetValue"),
			TEXT("GlobalAmbientOcclusionValue"), TEXT("GlobalRefractionValue"), TEXT("GlobalTangentValue"),
			TEXT("GlobalPixelDepthOffsetValue"), TEXT("GlobalDisplacementValue"),
			TEXT("GlobalSubsurfaceColorValue"), TEXT("GlobalSurfaceThicknessValue"),
			TEXT("GlobalOffsetValue"), TEXT("GlobalTilingValue"), TEXT("GlobalRotationValue")};
			int32 TraversalWorkItems = 0;
			auto ConsumeWork = [&]()
			{
				if (TraversalWorkItems >= MaxComponents || FPlatformTime::Seconds() >= Deadline)
				{
					Record.bDetailsTruncated = true; bComplete = false; return false;
				}
				++TraversalWorkItems;
				return true;
			};
			TSet<const UDMMaterialValue*> SeenValues;
			auto CaptureOneValue = [&](UDMMaterialValue* Value, const bool bNullIsGap)
			{
				if (!ConsumeWork()) return false;
			if (!Value) { if (bNullIsGap) bComplete = false; return true; }
			if (SeenValues.Contains(Value)) return true;
			SeenValues.Add(Value);
			bool bValuePathComplete = true;
			FHyperAIDynamicMaterialValueView View = CaptureValue(
				Value, Deadline, bValuePathComplete);
			if (View.ComponentPath.IsEmpty() || View.ComponentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
				|| View.ObjectValuePath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
				|| View.DefaultObjectValuePath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
				|| View.ParameterName.Len() > 256 || View.ExplicitParameterName.Len() > 256
				|| View.ParameterComponentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
				|| View.ParameterClassPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
				|| (View.bHasExplicitParameter
						&& (View.ParameterComponentPath.IsEmpty()
							|| View.ParameterClassPath != UDMMaterialParameter::StaticClass()->GetPathName()
							|| View.ParameterParentComponentPath != View.ComponentPath
							|| View.ParameterLifetimeState < 0
							|| View.ExplicitParameterName.IsEmpty()))
				|| View.ValueKind == TEXT("unsupported")
				|| !bValuePathComplete || !View.bSemanticProjectionComplete
				|| !FMath::IsFinite(View.ScalarValue)
				|| !FMath::IsFinite(View.VectorValue.X)
				|| !FMath::IsFinite(View.VectorValue.Y)
				|| !FMath::IsFinite(View.VectorValue.Z)
				|| !FMath::IsFinite(View.VectorValue.W)
				|| !FMath::IsFinite(View.DefaultScalarValue)
				|| !FMath::IsFinite(View.DefaultVectorValue.X)
				|| !FMath::IsFinite(View.DefaultVectorValue.Y)
				|| !FMath::IsFinite(View.DefaultVectorValue.Z)
				|| !FMath::IsFinite(View.DefaultVectorValue.W)
				|| !FMath::IsFinite(View.ValueRangeMin) || !FMath::IsFinite(View.ValueRangeMax)
				|| (View.bHasValueRange && View.ValueRangeMin > View.ValueRangeMax))
				bComplete = false;
			if (View.bMediaBridgeValue && !View.bMediaSourceIdentityKnown) bComplete = false;
				if (!ConsumeMaterialized(CanonicalValue(View))) return false;
				Record.Values.Add(MoveTemp(View));
			return true;
		};
		auto CaptureReferencedValue = [&](UDMMaterialValue* Value)
		{
			return CaptureOneValue(Value, true);
		};
		if (!FHyperAIStudioDynamicMaterialContracts::IsComponentCollectionWithinBound(
			LocalValues.Num(), MaxComponents))
		{
			Record.bDetailsTruncated = true;
			bComplete = false;
		}
		for (int32 Index = 0; Index < LocalValues.Num() && Index < MaxComponents; ++Index)
		{
			if (!CaptureOneValue(LocalValues[Index], true)) break;
		}
		if (!Record.bDetailsTruncated)
		{
			for (const FName Name : GlobalValueNames)
			{
				if (!CaptureOneValue(Model->GetGlobalParameterValue(Name), false)) break;
			}
		}
		UDynamicMaterialModelEditorOnlyData* EditorData = UDynamicMaterialModelEditorOnlyData::Get(Model);
			Record.bEditorModelDataAvailable = EditorData != nullptr;
			if (!EditorData) bComplete = false;
			else
			{
				bComplete &= EditorData->GetClass()
						== UDynamicMaterialModelEditorOnlyData::StaticClass()
					&& EditorData->GetMaterialModel() == Model;
				Record.EditorState = static_cast<int32>(EditorData->GetState());
			// UE 5.8 does not export HasBuildBeenRequested and its backing state is private.
			// Keep the observation fail-closed instead of crossing the pack's no-reflection
			// boundary or falsely certifying a stable material-build state.
			Record.bBuildRequested = true;
			bComplete = false;
			// NeedsWizard() is also unexported in UE 5.8. Epic defines it as an empty
			// property-to-slot map; project that through the exported finite lookup
			// below. Default true is the fail-closed value if the bounded scan stops.
			Record.bNeedsWizard = true;
			Record.MaterialDomain = static_cast<int32>(EditorData->GetDomain().GetValue());
			Record.BlendMode = static_cast<int32>(EditorData->GetBlendMode().GetValue());
			Record.ShadingModel = static_cast<int32>(EditorData->GetShadingModel());
			Record.UsageFlags = static_cast<int32>(EditorData->GetUsageFlags());
			Record.GeneralFlags = static_cast<int32>(EditorData->GetGeneralFlags());
			Record.LightingFlags = static_cast<int32>(EditorData->GetLightingFlags());
			Record.TranslucencyFlags = static_cast<int32>(EditorData->GetTranslucencyFlags());
			Record.MotionFlags = static_cast<int32>(EditorData->GetMotionFlags());
			Record.ForwardRendererFlags = static_cast<int32>(EditorData->GetForwardRendererFlags());
			Record.OpacityMaskClipValue = EditorData->GetOpacityMaskClipValue();
			Record.DisplacementCenter = EditorData->GetDisplacementCenter();
			Record.DisplacementMagnitude = EditorData->GetDisplacementMagnitude();
			bComplete &= FMath::IsFinite(Record.OpacityMaskClipValue)
				&& FMath::IsFinite(Record.DisplacementCenter)
				&& FMath::IsFinite(Record.DisplacementMagnitude);

			for (int32 PropertyIndex = static_cast<int32>(EDMMaterialPropertyType::None) + 1;
				PropertyIndex < static_cast<int32>(EDMMaterialPropertyType::Any);
				++PropertyIndex)
			{
				if (!ConsumeWork()) break;
				const EDMMaterialPropertyType PropertyType =
					static_cast<EDMMaterialPropertyType>(PropertyIndex);
				UDMMaterialProperty* Property = EditorData->GetMaterialProperty(PropertyType);
				if (!Property)
				{
					bComplete = false;
					continue;
				}
				FHyperAIDynamicMaterialPropertyView& PropertyView =
					Record.MaterialProperties.AddDefaulted_GetRef();
				bool bPropertyPathComplete = TryGetBoundedComponentPath(
					Property, Deadline, PropertyView.ComponentPath)
					&& TryGetBoundedObjectPath(Property->GetClass(),
						FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
						Deadline, PropertyView.ClassPath);
				PropertyView.LifetimeState = static_cast<int32>(Property->GetComponentState());
				PropertyView.MaterialProperty = static_cast<int32>(Property->GetMaterialProperty());
				PropertyView.bEnabled = Property->IsEnabled();
				PropertyView.bMaterialPin = Property->IsMaterialPin();
				PropertyView.InputConnectorType = static_cast<int32>(Property->GetInputConnectorType());
				bool bPropertyComplete = bPropertyPathComplete && Property->IsComponentValid()
					&& !PropertyView.ComponentPath.IsEmpty()
					&& PropertyView.ComponentPath.Len()
						<= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
					&& !PropertyView.ClassPath.IsEmpty()
					&& PropertyView.ClassPath.Len()
						<= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
					&& PropertyView.ClassPath
						== FHyperAIStudioDynamicMaterialContracts::ExpectedMaterialPropertyClassPath(
							PropertyIndex)
					&& PropertyView.MaterialProperty == PropertyIndex;
				if (UMaterialFunctionInterface* Processor = Property->GetOutputProcessor())
				{
					bPropertyComplete &= TryGetBoundedObjectPath(Processor,
						FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
						Deadline, PropertyView.OutputProcessorPath);
					// The processor graph belongs to the core Material Function family. Identity
					// alone cannot make its semantics exact, so this model CAS fails closed.
					bPropertyComplete = false;
				}
				PropertyView.bHasAlphaValueComponent = Property->HasComponent(
					UDynamicMaterialModelEditorOnlyData::AlphaValueName);
				const bool bExpectedAlphaValue = DoesBuiltInPropertyRequireAlphaValue(
					PropertyIndex);
				bPropertyComplete &= PropertyView.bHasAlphaValueComponent
					== bExpectedAlphaValue;
				if (PropertyView.bHasAlphaValueComponent)
				{
					UDMMaterialComponent* AlphaValue = Property->GetComponent(
						UDynamicMaterialModelEditorOnlyData::AlphaValueName);
					UDMMaterialValueFloat1* AlphaFloat = Cast<UDMMaterialValueFloat1>(AlphaValue);
					if (!AlphaFloat || AlphaValue->GetClass()
							!= UDMMaterialValueFloat1::StaticClass()
						|| !AlphaValue->IsComponentValid()
						|| !SeenValues.Contains(AlphaFloat))
						bPropertyComplete = false;
					else
					{
						FString AlphaPath;
						FString AlphaClassPath;
						bPropertyComplete &= TryGetBoundedComponentPath(
							AlphaValue, Deadline, AlphaPath);
						bPropertyComplete &= TryGetBoundedObjectPath(AlphaValue->GetClass(),
							FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
							Deadline, AlphaClassPath);
						AppendToken(PropertyView.AlphaValueComponentIdentity, AlphaPath);
						AppendToken(PropertyView.AlphaValueComponentIdentity, AlphaClassPath);
						AppendToken(PropertyView.AlphaValueComponentIdentity,
							FString::FromInt(static_cast<int32>(AlphaValue->GetComponentState())));
						bPropertyComplete &= !AlphaPath.IsEmpty()
							&& AlphaPath.Len()
								<= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
							&& AlphaClassPath.Len()
								<= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters;
					}
				}
				const FDMMaterialStageConnection& PropertyConnection =
					Property->GetInputConnectionMap();
				if (PropertyConnection.Channels.Num()
					> FHyperAIStudioDynamicMaterialContracts::MaxChannelsPerConnection)
					bPropertyComplete = false;
				const int32 PropertyChannelLimit = FMath::Min(
					PropertyConnection.Channels.Num(),
					FHyperAIStudioDynamicMaterialContracts::MaxChannelsPerConnection);
				for (int32 ChannelIndex = 0; ChannelIndex < PropertyChannelLimit; ++ChannelIndex)
				{
					if (!ConsumeWork()) { bPropertyComplete = false; break; }
					const FDMMaterialStageConnectorChannel& Channel =
						PropertyConnection.Channels[ChannelIndex];
					FHyperAIDynamicMaterialConnectionChannelView& ChannelView =
						PropertyView.InputChannels.AddDefaulted_GetRef();
					ChannelView.SourceIndex = Channel.SourceIndex;
					ChannelView.MaterialProperty = static_cast<int32>(Channel.MaterialProperty);
					ChannelView.OutputIndex = Channel.OutputIndex;
					ChannelView.OutputChannel = Channel.OutputChannel;
				}
				if (const UDMMaterialSlot* PropertySlot =
					EditorData->GetSlotForMaterialProperty(PropertyType))
				{
					Record.bNeedsWizard = false;
					bPropertyComplete &= TryGetBoundedComponentPath(
						PropertySlot, Deadline, PropertyView.SlotComponentPath);
				}
				if (PropertyView.SlotComponentPath.Len()
					> FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
					bPropertyComplete = false;
					PropertyView.bSemanticProjectionComplete = bPropertyComplete
						&& !Record.bDetailsTruncated;
					bComplete &= PropertyView.bSemanticProjectionComplete;
					if (!ConsumeMaterialized(CanonicalMaterialProperty(PropertyView)))
					{
						Record.MaterialProperties.Pop();
						break;
					}
				}
			Record.MaterialPropertyCount = Record.MaterialProperties.Num();
			const int32 ExpectedPropertyCount =
				static_cast<int32>(EDMMaterialPropertyType::Any)
				- static_cast<int32>(EDMMaterialPropertyType::None) - 1;
			if (Record.MaterialPropertyCount != ExpectedPropertyCount) bComplete = false;
			if (Record.EditorState != static_cast<int32>(EDMState::Idle)
				|| Record.bBuildRequested || Record.bNeedsWizard)
				bComplete = false;
			const TArray<UDMMaterialSlot*>& Slots = EditorData->GetSlots();
			Record.SlotCount = Slots.Num();
			if (Slots.Num() > MaxComponents)
			{
				Record.bDetailsTruncated = true;
				bComplete = false;
			}
			bool bStopComponentTraversal = Record.bDetailsTruncated;
			for (int32 SlotIndex = 0;
				SlotIndex < Slots.Num() && !bStopComponentTraversal;
				++SlotIndex)
			{
					if (!ConsumeWork()) break;
					UDMMaterialSlot* Slot = Slots[SlotIndex];
					if (!Slot || Slot->GetClass() != UDMMaterialSlot::StaticClass())
					{
						bComplete = false; continue;
					}
					FHyperAIDynamicMaterialSlotView& SlotView =
						Record.Slots.AddDefaulted_GetRef();
					bool bSlotPathComplete = TryGetBoundedComponentPath(
						Slot, Deadline, SlotView.ComponentPath)
						&& TryGetBoundedObjectPath(Slot->GetClass(),
							FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
							Deadline, SlotView.ClassPath);
					SlotView.LifetimeState = static_cast<int32>(Slot->GetComponentState());
					SlotView.SlotIndex = Slot->GetIndex();
					bool bSlotProjectionComplete = bSlotPathComplete && Slot->IsComponentValid()
						&& !SlotView.ComponentPath.IsEmpty()
						&& SlotView.ComponentPath.Len()
							<= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
						&& SlotView.SlotIndex == SlotIndex;
					for (int32 PropertyIndex = static_cast<int32>(EDMMaterialPropertyType::None) + 1;
						PropertyIndex < static_cast<int32>(EDMMaterialPropertyType::Any);
						++PropertyIndex)
					{
						if (!ConsumeWork()) { bSlotProjectionComplete = false; break; }
						const TArray<EDMValueType>& Types =
							Slot->GetOutputConnectorTypesForMaterialProperty(
								static_cast<EDMMaterialPropertyType>(PropertyIndex));
						if (Types.Num()
							> FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent)
							bSlotProjectionComplete = false;
						FString CanonicalTypes;
						AppendToken(CanonicalTypes, FString::FromInt(PropertyIndex));
						const int32 TypeLimit = FMath::Min(Types.Num(),
							FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent);
						for (int32 TypeIndex = 0; TypeIndex < TypeLimit; ++TypeIndex)
							AppendToken(CanonicalTypes,
								FString::FromInt(static_cast<int32>(Types[TypeIndex])));
						SlotView.OutputConnectorTypeSets.Add(MoveTemp(CanonicalTypes));
					}
					const TMap<TWeakObjectPtr<UDMMaterialSlot>, int32>& ReferencedBy =
						Slot->GetSlotsReferencedBy();
					if (ReferencedBy.Num() > MaxComponents) bSlotProjectionComplete = false;
					int32 ReferenceIndex = 0;
					for (const TPair<TWeakObjectPtr<UDMMaterialSlot>, int32>& Reference : ReferencedBy)
					{
						if (ReferenceIndex++ >= MaxComponents || !ConsumeWork())
						{
							bSlotProjectionComplete = false; break;
						}
						UDMMaterialSlot* ReferencingSlot = Reference.Key.Get();
						if (!ReferencingSlot || Reference.Value <= 0)
						{
							bSlotProjectionComplete = false; continue;
						}
						FString ReferencingPath;
						if (!TryGetBoundedComponentPath(
							ReferencingSlot, Deadline, ReferencingPath))
						{
							bSlotProjectionComplete = false; continue;
						}
						if (ReferencingPath.IsEmpty()
							|| ReferencingPath.Len()
								> FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
						{
							bSlotProjectionComplete = false; continue;
						}
						FString ReferenceIdentity;
						AppendToken(ReferenceIdentity, ReferencingPath);
						AppendToken(ReferenceIdentity, FString::FromInt(Reference.Value));
						if (!ConsumeMaterialized(ReferenceIdentity))
						{
							bSlotProjectionComplete = false;
							break;
						}
						SlotView.ReferencedBySlotCounts.Add(MoveTemp(ReferenceIdentity));
					}
					SlotView.ReferencedBySlotCounts.Sort();
					SlotView.bSemanticProjectionComplete = bSlotProjectionComplete
						&& !Record.bDetailsTruncated;
					bComplete &= SlotView.bSemanticProjectionComplete;
					if (!ConsumeMaterialized(CanonicalSlot(SlotView)))
					{
						Record.Slots.Pop();
						bStopComponentTraversal = true;
						break;
					}
				const TArray<TObjectPtr<UDMMaterialLayerObject>>& Layers = Slot->GetLayers();
				for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
				{
						if (!ConsumeWork())
						{
							bStopComponentTraversal = true; break;
						}
						UDMMaterialLayerObject* Layer = Layers[LayerIndex];
						if (!Layer || Layer->GetClass() != UDMMaterialLayerObject::StaticClass())
						{
							bComplete = false; continue;
						}
						FHyperAIDynamicMaterialLayerView View;
						bool bLayerProjectionComplete = Layer->IsComponentValid();
						bLayerProjectionComplete &= TryGetBoundedComponentPath(
							Layer, Deadline, View.ComponentPath);
						bLayerProjectionComplete &= TryGetBoundedObjectPath(Layer->GetClass(),
							FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
							Deadline, View.ClassPath);
						View.LifetimeState = static_cast<int32>(Layer->GetComponentState());
						const bool bLayerNameComplete = CaptureBoundedSimpleText(
							Layer->GetLayerName(),
							FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters,
							View.Name);
						if (View.ComponentPath.IsEmpty()
							|| View.ComponentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
							bLayerProjectionComplete = false;
						if (!bLayerNameComplete)
							bLayerProjectionComplete = false;
						View.SlotIndex = SlotIndex; View.LayerIndex = LayerIndex;
						View.MaterialProperty = static_cast<int32>(Layer->GetMaterialProperty()); View.bEnabled = Layer->IsEnabled();
						View.bTextureUVLinkEnabled = Layer->IsTextureUVLinkEnabled();
						if (UDMMaterialEffectStack* EffectStack = Layer->GetEffectStack())
						{
							bLayerProjectionComplete &= TryGetBoundedComponentPath(
								EffectStack, Deadline, View.EffectStackComponentPath);
							bLayerProjectionComplete &= TryGetBoundedObjectPath(EffectStack->GetClass(),
								FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
								Deadline, View.EffectStackClassPath);
							View.EffectStackLifetimeState =
								static_cast<int32>(EffectStack->GetComponentState());
							View.bEffectStackEnabled = EffectStack->IsEnabled();
							const TArray<TObjectPtr<UDMMaterialEffect>>& Effects = EffectStack->GetEffects();
							View.EffectCount = Effects.Num();
							bLayerProjectionComplete &= EffectStack->IsComponentValid()
								&& EffectStack->GetClass() == UDMMaterialEffectStack::StaticClass()
								&& !View.EffectStackComponentPath.IsEmpty()
								&& View.EffectStackComponentPath.Len()
									<= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters;
							// Effect bodies are a separate type family. Empty stacks are exact; non-empty
							// stacks block this layer/stage/value CAS rather than being silently omitted.
							if (!Effects.IsEmpty()) bLayerProjectionComplete = false;
						}
						else bLayerProjectionComplete = false;
						const TArray<TObjectPtr<UDMMaterialStage>>& Stages = Layer->GetAllStages();
					for (int32 StageIndex = 0; StageIndex < Stages.Num(); ++StageIndex)
					{
							if (!ConsumeWork())
							{
								bStopComponentTraversal = true; break;
							}
							UDMMaterialStage* Stage = Stages[StageIndex];
							if (!Stage || Stage->GetClass() != UDMMaterialStage::StaticClass())
							{
								bLayerProjectionComplete = false; continue;
							}
							FHyperAIDynamicMaterialStageView& StageView = View.Stages.AddDefaulted_GetRef();
							bool bStageProjectionComplete = Stage->IsComponentValid();
							bStageProjectionComplete &= TryGetBoundedComponentPath(
								Stage, Deadline, StageView.ComponentPath);
							bStageProjectionComplete &= TryGetBoundedObjectPath(Stage->GetClass(),
								FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
								Deadline, StageView.ClassPath);
							StageView.LifetimeState = static_cast<int32>(Stage->GetComponentState());
							StageView.StageIndex = StageIndex;
							StageView.bEnabled = Stage->IsEnabled();
							StageView.bCanChangeSource = Stage->CanChangeSource();
							if (StageView.ComponentPath.IsEmpty()
								|| StageView.ComponentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
								bStageProjectionComplete = false;
							StageView.StageType = static_cast<int32>(Layer->GetStageType(Stage));
							if (UDMMaterialStageSource* Source = Stage->GetSource())
							{
								bStageProjectionComplete &= TryGetBoundedObjectPath(Source->GetClass(),
									FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
									Deadline, StageView.SourceClassPath);
								if (!CaptureClosedStageComponent(
									Source, StageView.Source, Deadline, ConsumeWork, CaptureReferencedValue)
									|| !StageView.Source.bSemanticProjectionComplete)
									bStageProjectionComplete = false;
							}
							else bStageProjectionComplete = false;

							const TArray<UDMMaterialStageInput*>& Inputs = Stage->GetInputs();
							if (Inputs.Num() > FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent)
								bStageProjectionComplete = false;
							const int32 InputLimit = FMath::Min(Inputs.Num(),
								FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent);
							for (int32 InputIndex = 0; InputIndex < InputLimit; ++InputIndex)
							{
								FHyperAIDynamicMaterialComponentView& InputView =
									StageView.Inputs.AddDefaulted_GetRef();
								if (!CaptureClosedStageComponent(
									Inputs[InputIndex], InputView, Deadline, ConsumeWork, CaptureReferencedValue)
									|| !InputView.bSemanticProjectionComplete)
									bStageProjectionComplete = false;
								if (Record.bDetailsTruncated) break;
							}

							const TArray<FDMMaterialStageConnection>& Connections =
								Stage->GetInputConnectionMap();
							if (Connections.Num() > FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent)
								bStageProjectionComplete = false;
							const int32 ConnectionLimit = FMath::Min(Connections.Num(),
								FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent);
							for (int32 ConnectionIndex = 0; ConnectionIndex < ConnectionLimit; ++ConnectionIndex)
							{
								if (!ConsumeWork()) break;
								const FDMMaterialStageConnection& Connection = Connections[ConnectionIndex];
								FHyperAIDynamicMaterialConnectionView& ConnectionView =
									StageView.InputConnections.AddDefaulted_GetRef();
								ConnectionView.InputIndex = ConnectionIndex;
								if (Connection.Channels.Num()
									> FHyperAIStudioDynamicMaterialContracts::MaxChannelsPerConnection)
									bStageProjectionComplete = false;
								const int32 ChannelLimit = FMath::Min(Connection.Channels.Num(),
									FHyperAIStudioDynamicMaterialContracts::MaxChannelsPerConnection);
								for (int32 ChannelIndex = 0; ChannelIndex < ChannelLimit; ++ChannelIndex)
								{
									if (!ConsumeWork()) break;
									const FDMMaterialStageConnectorChannel& Channel =
										Connection.Channels[ChannelIndex];
									FHyperAIDynamicMaterialConnectionChannelView& ChannelView =
										ConnectionView.Channels.AddDefaulted_GetRef();
									ChannelView.SourceIndex = Channel.SourceIndex;
									ChannelView.MaterialProperty = static_cast<int32>(Channel.MaterialProperty);
									ChannelView.OutputIndex = Channel.OutputIndex;
									ChannelView.OutputChannel = Channel.OutputChannel;
								}
							}
								// The only fully projected source class here is a direct value source. It has no
								// source inputs; persisted orphan/nested inputs or mappings are captured for
								// diagnostics and CAS sensitivity; hidden membership still blocks mutation.
							if (StageView.Source.ClassPath
								== UDMMaterialStageInputValue::StaticClass()->GetPathName()
								&& (!StageView.Source.InputConnectors.IsEmpty()
									|| !StageView.Inputs.IsEmpty()
									|| !StageView.InputConnections.IsEmpty()))
								bStageProjectionComplete = false;
							StageView.bSemanticProjectionComplete = bStageProjectionComplete
								&& !Record.bDetailsTruncated;
							bLayerProjectionComplete &= StageView.bSemanticProjectionComplete;
							if (!ConsumeMaterialized(CanonicalStage(StageView)))
							{
								View.Stages.Pop();
								bLayerProjectionComplete = false;
								bStopComponentTraversal = true;
								break;
							}
							++Record.StageCount;
						}
						View.bSemanticProjectionComplete = bLayerProjectionComplete
							&& !Record.bDetailsTruncated;
						bComplete &= View.bSemanticProjectionComplete;
						if (!ConsumeMaterialized(CanonicalLayer(View)))
						{
							bStopComponentTraversal = true;
							break;
						}
						Record.Layers.Add(MoveTemp(View)); ++Record.LayerCount;
				}
			}
				TSet<FString> ProjectedComponentPaths;
				bool bProjectedPathBudgetComplete = true;
				auto AddProjectedPath = [&](const FString& Path)
				{
					if (Path.IsEmpty() || ProjectedComponentPaths.Contains(Path)) return;
					if (ProjectedComponentPaths.Num() >= MaxComponents
						|| !ConsumeMaterialized(Path))
					{
						bProjectedPathBudgetComplete = false;
						return;
					}
					ProjectedComponentPaths.Add(Path);
				};
				for (const FHyperAIDynamicMaterialValueView& Value : Record.Values)
				{
					AddProjectedPath(Value.ComponentPath);
					if (Value.bHasExplicitParameter)
						AddProjectedPath(Value.ParameterComponentPath);
				}
				for (const FHyperAIDynamicMaterialPropertyView& Property : Record.MaterialProperties)
					AddProjectedPath(Property.ComponentPath);
				for (const FHyperAIDynamicMaterialSlotView& Slot : Record.Slots)
					AddProjectedPath(Slot.ComponentPath);
				for (const FHyperAIDynamicMaterialLayerView& Layer : Record.Layers)
				{
					AddProjectedPath(Layer.ComponentPath);
					AddProjectedPath(Layer.EffectStackComponentPath);
					for (const FHyperAIDynamicMaterialStageView& Stage : Layer.Stages)
					{
						AddProjectedPath(Stage.ComponentPath);
						AddProjectedPath(Stage.Source.ComponentPath);
						for (const FHyperAIDynamicMaterialComponentView& Input : Stage.Inputs)
							AddProjectedPath(Input.ComponentPath);
					}
				}
				bool bOwnershipProjectionComplete = bProjectedPathBudgetComplete &&
					ProjectedComponentPaths.Num() <= MaxComponents;
				for (const FString& ProjectedPath : ProjectedComponentPaths)
			{
				if (!ConsumeWork()) { bOwnershipProjectionComplete = false; break; }
				UDMMaterialComponent* Component = Model->GetComponentByPath(ProjectedPath);
				FString RoundTripPath;
				bool bIdentityComplete = Component
					&& TryGetBoundedComponentPath(Component, Deadline, RoundTripPath)
					&& RoundTripPath == ProjectedPath
					&& Component->GetOutermost() == Model->GetOutermost();
				if (!Component)
				{
					bOwnershipProjectionComplete = false;
					continue;
				}
				FString ClassPath;
				bIdentityComplete &= TryGetBoundedObjectPath(Component->GetClass(),
					FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
					Deadline, ClassPath);
				const FString PackageName = Component->GetOutermost()->GetFName().ToString();
				bIdentityComplete &= !ProjectedPath.IsEmpty()
					&& ProjectedPath.Len() <= FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
					&& ClassPath.Len() <= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
					&& PackageName.Len() <= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters;

				FString OuterChain;
				TSet<const UObject*> SeenOuters;
				const UObject* OuterCursor = Component;
				bool bReachedModel = false;
				int32 OuterDepth = 0;
				for (; OuterCursor && OuterDepth < 32; ++OuterDepth)
				{
					if (SeenOuters.Contains(OuterCursor)) { bIdentityComplete = false; break; }
					SeenOuters.Add(OuterCursor);
					FString OuterPath;
					FString OuterClass;
					if (!TryGetBoundedObjectPath(OuterCursor,
						FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters,
						Deadline, OuterPath)
						|| !TryGetBoundedObjectPath(OuterCursor->GetClass(),
							FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
							Deadline, OuterClass))
					{
						bIdentityComplete = false; break;
					}
					if (OuterPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
						|| OuterClass.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters)
					{
						bIdentityComplete = false; break;
					}
					AppendToken(OuterChain, OuterPath); AppendToken(OuterChain, OuterClass);
					bReachedModel |= OuterCursor == Model;
					OuterCursor = OuterCursor->GetOuter();
				}
				if (OuterCursor || !bReachedModel) bIdentityComplete = false;

				FString ParentChain;
				TSet<const UDMMaterialComponent*> SeenParents;
				const UDMMaterialComponent* ParentCursor = Component;
				int32 ParentDepth = 0;
				for (; ParentCursor && ParentDepth < 32; ++ParentDepth)
				{
					if (SeenParents.Contains(ParentCursor)) { bIdentityComplete = false; break; }
					SeenParents.Add(ParentCursor);
					FString ParentPath;
					FString ParentClass;
					if (!TryGetBoundedComponentPath(ParentCursor, Deadline, ParentPath)
						|| !TryGetBoundedObjectPath(ParentCursor->GetClass(),
							FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
							Deadline, ParentClass))
					{
						bIdentityComplete = false; break;
					}
					if (ParentPath.IsEmpty()
						|| ParentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
						|| ParentClass.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
						|| Model->GetComponentByPath(ParentPath) != ParentCursor)
					{
						bIdentityComplete = false; break;
					}
					AppendToken(ParentChain, ParentPath); AppendToken(ParentChain, ParentClass);
					ParentCursor = ParentCursor->GetParentComponent();
				}
				if (ParentCursor) bIdentityComplete = false;

				FString Identity;
				AppendToken(Identity, ProjectedPath); AppendToken(Identity, ClassPath);
				AppendToken(Identity, PackageName);
					AppendToken(Identity, FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(OuterChain));
					AppendToken(Identity, FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ParentChain));
					AppendToken(Identity, BoolToken(bIdentityComplete));
					if (!ConsumeMaterialized(Identity))
					{
						bOwnershipProjectionComplete = false;
						break;
					}
					Record.ComponentOwnershipIdentities.Add(MoveTemp(Identity));
				bOwnershipProjectionComplete &= bIdentityComplete;
			}
			Record.ComponentOwnershipIdentities.Sort();
			Record.bComponentOwnershipProjectionComplete = bOwnershipProjectionComplete
				&& Record.ComponentOwnershipIdentities.Num() == ProjectedComponentPaths.Num();
			bComplete &= Record.bComponentOwnershipProjectionComplete;
			const TSet<TObjectPtr<UDMMaterialComponent>>& RuntimeComponents =
				Model->GetRuntimeComponents();
			if (RuntimeComponents.Num() > MaxComponents) bComplete = false;
			int32 RuntimeComponentIndex = 0;
			for (const TObjectPtr<UDMMaterialComponent>& RuntimeComponentPtr : RuntimeComponents)
			{
				UDMMaterialComponent* RuntimeComponent = RuntimeComponentPtr.Get();
				if (RuntimeComponentIndex++ >= MaxComponents || !ConsumeWork())
				{
					bComplete = false; break;
				}
				if (!RuntimeComponent)
				{
					bComplete = false; continue;
				}
				FString RuntimePath;
				FString RuntimeClassPath;
				const bool bRuntimePathComplete = TryGetBoundedComponentPath(
					RuntimeComponent, Deadline, RuntimePath)
					&& TryGetBoundedObjectPath(RuntimeComponent->GetClass(),
						FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
						Deadline, RuntimeClassPath);
				if (RuntimePath.IsEmpty()
					|| !bRuntimePathComplete
					|| RuntimePath.Len()
						> FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters
					|| RuntimeClassPath.Len()
						> FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
					|| !ProjectedComponentPaths.Contains(RuntimePath))
					bComplete = false;
				FString RuntimeIdentity;
				AppendToken(RuntimeIdentity, RuntimePath);
					AppendToken(RuntimeIdentity, RuntimeClassPath);
					AppendToken(RuntimeIdentity,
						FString::FromInt(static_cast<int32>(RuntimeComponent->GetComponentState())));
					if (!ConsumeMaterialized(RuntimeIdentity)) break;
					Record.RuntimeComponentIdentities.Add(MoveTemp(RuntimeIdentity));
			}
			Record.RuntimeComponentIdentities.Sort();

			UMaterial* Generated = EditorData->GetGeneratedMaterial();
			if (!Generated) bComplete = false;
				else
				{
					bComplete &= TryGetBoundedObjectPath(Generated,
						FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters,
						Deadline, Record.GeneratedMaterialPath);
					Record.GeneratedMaterialStateId = Generated->StateId.ToString(EGuidFormats::DigitsWithHyphensLower);
					bComplete &= Generated->GetClass() == UMaterial::StaticClass()
						&& !Record.GeneratedMaterialPath.IsEmpty()
						&& Record.GeneratedMaterialPath.Len()
							<= FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
						&& Generated->StateId.IsValid();
					Record.bGeneratedMaterialPackageDirty = Generated->GetOutermost()->IsDirty();
				Record.bCompileStateKnown = GMaxRHIShaderPlatform != SP_NumPlatforms;
				Record.bCompiling = Generated->IsCompiling();
				if (Record.bCompileStateKnown)
					Record.bCompileError = Generated->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform)
						&& !Record.bCompiling;
				else bComplete = false;
			}
		}
			Record.ValueCount = SeenValues.Num();
			Record.MutationRevisionBlockers = {
				TEXT("property_component_membership_unenumerable"),
				TEXT("parameter_map_membership_unenumerable"),
				TEXT("dynamic_material_instance_mid_state_unenumerable")};
			Record.bMutationRevisionComplete = false;
			Record.bRevisionComplete = bComplete && !Record.bDetailsTruncated;
		Snapshot.bComplete = Record.bRevisionComplete;
			Snapshot.Revision = FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(
				Snapshot, Deadline);
			Record.Revision = Snapshot.Revision;
			if (!FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(Snapshot.Revision))
			{
				Record.bRevisionComplete = false;
				Record.bDetailsTruncated = true;
				Snapshot.bComplete = false;
			}
		OutStatus = Snapshot.bComplete ? TEXT("captured") : TEXT("capture_incomplete");
		OutDiagnostic = Snapshot.bComplete
			? TEXT("Complete closed public DynamicMaterial root/property/slot/value/direct-stage projection and generated-material CAS captured.")
			: TEXT("The closed public model projection, compile/media identity evidence, or a finite work bound is incomplete.");
		return true;
	}

	FHyperAIStudioDomainAdapterResult Failure(EHyperAIStudioDomainDispatchOutcome Outcome,
		const FString& Code, const FString& Diagnostic = FString())
	{
		FHyperAIStudioDomainAdapterResult Result; Result.Outcome = Outcome;
		Result.StatusCode = Code; Result.Diagnostic = Clip(Diagnostic); return Result;
	}

	bool TryEstimateInspectBytes(
		const FHyperAIDynamicMaterialInspectReport& Report,
		const double Deadline,
		int64& OutBytes)
	{
		// Six-character JSON control escaping plus reflected-field/container overhead.
		constexpr int64 JsonExpansion = 12;
		auto StringBytes = [](const FString& Value)
		{
			return 128ll + JsonExpansion * static_cast<int64>(Value.Len());
		};
		auto CanonicalBytes = [](const FString& Value)
		{
			return 1024ll + JsonExpansion * static_cast<int64>(Value.Len());
		};
		OutBytes = 8192ll;
		for (const FString* Value : {
			&Report.Status, &Report.Diagnostic, &Report.Scope, &Report.Revision,
			&Report.NextCursor, &Report.Record.Family, &Report.Record.AssetPath,
			&Report.Record.ClassPath, &Report.Record.ModelPath, &Report.Record.ModelClassPath,
			&Report.Record.AssociatedInstancePath, &Report.Record.GeneratedMaterialPath,
			&Report.Record.GeneratedMaterialStateId, &Report.Record.Revision,
			&Report.Record.DiskExistence, &Report.Record.ReferenceEvidence,
			&Report.Record.ComponentProjectionEvidence})
			OutBytes += StringBytes(*Value);
		for (const FString& Blocker : Report.Record.MutationRevisionBlockers)
			OutBytes += StringBytes(Blocker);
		for (const FHyperAIDynamicMaterialIssue& Issue : Report.Issues)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += 512ll + StringBytes(Issue.Code) + StringBytes(Issue.Severity)
				+ StringBytes(Issue.AssetPath) + StringBytes(Issue.ComponentPath)
				+ StringBytes(Issue.Message);
		}
		for (const FHyperAIDynamicMaterialCapabilityStatus& Capability : Report.Capabilities)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += 1024ll + StringBytes(Capability.Family) + StringBytes(Capability.State)
				+ StringBytes(Capability.Remediation);
			for (const TArray<FString>* Values : {
				&Capability.RequiredPlugins, &Capability.RequiredModules, &Capability.SupportedCases,
				&Capability.DelegatedEpicCases, &Capability.UnsupportedCases})
				for (const FString& Value : *Values) OutBytes += StringBytes(Value);
		}
		for (const FHyperAIDynamicMaterialValueView& Value : Report.Record.Values)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += CanonicalBytes(CanonicalValue(Value));
		}
		for (const FHyperAIDynamicMaterialPropertyView& Property : Report.Record.MaterialProperties)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += CanonicalBytes(CanonicalMaterialProperty(Property));
		}
		for (const FHyperAIDynamicMaterialSlotView& Slot : Report.Record.Slots)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += CanonicalBytes(CanonicalSlot(Slot));
		}
		for (const FString& RuntimeComponent : Report.Record.RuntimeComponentIdentities)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += StringBytes(RuntimeComponent);
		}
		for (const FString& Ownership : Report.Record.ComponentOwnershipIdentities)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += StringBytes(Ownership);
		}
		for (const FHyperAIDynamicMaterialLayerView& Layer : Report.Record.Layers)
		{
			if (!IsBeforeDeadline(Deadline)) return false;
			OutBytes += CanonicalBytes(CanonicalLayer(Layer));
		}
		return IsBeforeDeadline(Deadline);
	}

	bool EnforceInspectOutputBound(
		FHyperAIDynamicMaterialInspectReport& Report,
		const int32 MaxBytes,
		const double Deadline)
	{
		constexpr int64 JsonExpansion = 12;
		auto StringBytes = [](const FString& Value)
		{
			return 128ll + JsonExpansion * static_cast<int64>(Value.Len());
		};
		auto CanonicalBytes = [](const FString& Value)
		{
			return 1024ll + JsonExpansion * static_cast<int64>(Value.Len());
		};
		int64 Bytes = 0;
		if (!TryEstimateInspectBytes(Report, Deadline, Bytes)) return false;
		while (Bytes > MaxBytes && !Report.Record.Layers.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= CanonicalBytes(CanonicalLayer(Report.Record.Layers.Last()));
			Report.Record.Layers.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Record.Slots.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= CanonicalBytes(CanonicalSlot(Report.Record.Slots.Last()));
			Report.Record.Slots.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Record.MaterialProperties.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= CanonicalBytes(CanonicalMaterialProperty(
				Report.Record.MaterialProperties.Last()));
			Report.Record.MaterialProperties.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		while (Bytes > MaxBytes
			&& !Report.Record.ComponentOwnershipIdentities.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= StringBytes(Report.Record.ComponentOwnershipIdentities.Last());
			Report.Record.ComponentOwnershipIdentities.Pop(EAllowShrinking::No);
			Report.bTruncated = true;
		}
		while (Bytes > MaxBytes
			&& !Report.Record.RuntimeComponentIdentities.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= StringBytes(Report.Record.RuntimeComponentIdentities.Last());
			Report.Record.RuntimeComponentIdentities.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		while (Bytes > MaxBytes && !Report.Record.Values.IsEmpty())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			Bytes -= CanonicalBytes(CanonicalValue(Report.Record.Values.Last()));
			Report.Record.Values.Pop(EAllowShrinking::No); Report.bTruncated = true;
		}
		return Bytes <= MaxBytes;
	}
}

FString FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioDynamicMaterial.HyperAIStudioDynamicMaterialToolset");
}

const TArray<FHyperAIStudioDynamicMaterialManifestEntry>&
FHyperAIStudioDynamicMaterialContracts::GetManifest()
{
	static const TArray<FHyperAIStudioDynamicMaterialManifestEntry> Manifest = {
		{TEXT("hyper_dynamic_material_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_dynamic_material_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_dynamic_material_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioDynamicMaterialContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioDynamicMaterialContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	TArray<FString> Names; for (const auto& Entry : GetManifest()) Names.Add(Entry.Name);
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioDynamicMaterialContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT(":")) || Path.Contains(TEXT("*")) || Path.Contains(TEXT("?"))) return false;
	for (int32 Index = 0; Index < Path.Len(); ++Index) if (Path[Index] == TEXT('\0')) return false;
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FString Package = FPackageName::ObjectPathToPackageName(Path);
	const FString Object = FPackageName::ObjectPathToObjectName(Path);
	return !Package.IsEmpty() && FPackageName::GetLongPackageAssetName(Package) == Object;
}

bool FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR C = Value[Index];
		if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioDynamicMaterialContracts::IsExactLoadedFamily(
	const UObject* Object, const FString& Family)
{
	if (!Object) return false;
	if (Family == TEXT("dynamic_material_instance"))
		return Object->GetClass() == UDynamicMaterialInstance::StaticClass();
	if (Family == TEXT("dynamic_material_model"))
		return Object->GetClass() == UDynamicMaterialModel::StaticClass();
	return false;
}

FString FHyperAIStudioDynamicMaterialContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	if (State == UE::AssetRegistry::EExists::Exists) return TEXT("exists");
	if (State == UE::AssetRegistry::EExists::DoesNotExist) return TEXT("does_not_exist");
	return TEXT("unknown");
}

FString FHyperAIStudioDynamicMaterialContracts::ExpectedMaterialPropertyClassPath(
	const int32 MaterialProperty)
{
	switch (static_cast<EDMMaterialPropertyType>(MaterialProperty))
	{
	case EDMMaterialPropertyType::BaseColor:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyBaseColor");
	case EDMMaterialPropertyType::EmissiveColor:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyEmissiveColor");
	case EDMMaterialPropertyType::Opacity:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyOpacity");
	case EDMMaterialPropertyType::OpacityMask:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyOpacityMask");
	case EDMMaterialPropertyType::Roughness:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyRoughness");
	case EDMMaterialPropertyType::Specular:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertySpecular");
	case EDMMaterialPropertyType::Metallic:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyMetallic");
	case EDMMaterialPropertyType::Normal:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyNormal");
	case EDMMaterialPropertyType::PixelDepthOffset:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyPixelDepthOffset");
	case EDMMaterialPropertyType::WorldPositionOffset:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyWorldPositionOffset");
	case EDMMaterialPropertyType::AmbientOcclusion:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyAmbientOcclusion");
	case EDMMaterialPropertyType::Anisotropy:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyAnisotropy");
	case EDMMaterialPropertyType::Refraction:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyRefraction");
	case EDMMaterialPropertyType::Tangent:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyTangent");
	case EDMMaterialPropertyType::Custom1:
	case EDMMaterialPropertyType::Custom2:
	case EDMMaterialPropertyType::Custom3:
	case EDMMaterialPropertyType::Custom4:
		return UDMMaterialProperty::StaticClass()->GetPathName();
	case EDMMaterialPropertyType::Displacement:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertyDisplacement");
	case EDMMaterialPropertyType::SubsurfaceColor:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertySubsurfaceColor");
	case EDMMaterialPropertyType::SurfaceThickness:
		return TEXT("/Script/DynamicMaterialEditor.DMMaterialPropertySurfaceThickness");
	default:
		return FString();
	}
}

bool FHyperAIStudioDynamicMaterialContracts::IsComponentCollectionWithinBound(
	const int32 Count,
	const int32 RequestedMaxComponents)
{
	return Count >= 0 && RequestedMaxComponents >= 1
		&& RequestedMaxComponents <= MaxComponents && Count <= RequestedMaxComponents;
}

bool FHyperAIStudioDynamicMaterialContracts::IsBoundedSimpleSemanticText(
	const FText& Text,
	const int32 MaxCharacters)
{
	if (MaxCharacters < 0 || MaxCharacters > MaxComponentPathCharacters) return false;
	FString Ignored;
	return HyperAIStudio::DynamicMaterial::Private::CaptureBoundedSimpleText(
		Text, MaxCharacters, Ignored);
}

bool FHyperAIStudioDynamicMaterialContracts::IsExactValueOperationMatch(
	const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
	const FHyperAIDynamicMaterialValueView& Value)
{
	if (Value.bMediaBridgeValue || Value.ComponentPath != Operation.ComponentPath) return false;
	switch (Operation.Kind)
	{
	case EHyperAIStudioDynamicMaterialOperationKind::SetBool:
		return Value.ValueKind == TEXT("bool")
			&& Value.ClassPath == UDMMaterialValueBool::StaticClass()->GetPathName();
	case EHyperAIStudioDynamicMaterialOperationKind::SetScalar:
		return Value.ValueKind == TEXT("scalar")
			&& Value.ClassPath == UDMMaterialValueFloat1::StaticClass()->GetPathName();
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector2:
		return Value.ValueKind == TEXT("vector2")
			&& Value.ClassPath == UDMMaterialValueFloat2::StaticClass()->GetPathName();
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector3:
		return Value.ValueKind == TEXT("vector3")
			&& Value.ClassPath == UDMMaterialValueFloat3XYZ::StaticClass()->GetPathName();
	case EHyperAIStudioDynamicMaterialOperationKind::SetRotator:
		return Value.ValueKind == TEXT("rotator")
			&& Value.ClassPath == UDMMaterialValueFloat3RPY::StaticClass()->GetPathName();
	case EHyperAIStudioDynamicMaterialOperationKind::SetColor:
		return Value.ValueKind == TEXT("color")
			&& (Value.ClassPath == UDMMaterialValueFloat3RGB::StaticClass()->GetPathName()
				|| Value.ClassPath == UDMMaterialValueFloat4::StaticClass()->GetPathName());
	default:
		return false;
	}
}

bool FHyperAIStudioDynamicMaterialContracts::NormalizeOperationForCapturedValue(
	FHyperAIStudioDynamicMaterialBackendOperation& Operation,
	const FHyperAIDynamicMaterialValueView& Value,
	FString& OutErrorCode)
{
	OutErrorCode.Reset();
	if (!IsExactValueOperationMatch(Operation, Value))
	{
		OutErrorCode = TEXT("component_type_mismatch_or_missing");
		return false;
	}
	if (!FMath::IsFinite(Value.ValueRangeMin) || !FMath::IsFinite(Value.ValueRangeMax)
		|| (Value.bHasValueRange && Value.ValueRangeMin > Value.ValueRangeMax))
	{
		OutErrorCode = TEXT("component_value_range_invalid");
		return false;
	}
	const auto ClampToRange = [&](const double Input)
	{
		return Value.bHasValueRange
			? FMath::Clamp(Input, Value.ValueRangeMin, Value.ValueRangeMax)
			: Input;
	};
	switch (Operation.Kind)
	{
	case EHyperAIStudioDynamicMaterialOperationKind::SetScalar:
	{
		const float Persisted = static_cast<float>(ClampToRange(Operation.ScalarValue));
		if (!FMath::IsFinite(Persisted))
		{
			OutErrorCode = TEXT("scalar_float_overflow"); return false;
		}
		Operation.ScalarValue = static_cast<double>(Persisted);
		break;
	}
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector2:
		Operation.VectorValue.X = ClampToRange(Operation.VectorValue.X);
		Operation.VectorValue.Y = ClampToRange(Operation.VectorValue.Y);
		break;
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector3:
		Operation.VectorValue.X = ClampToRange(Operation.VectorValue.X);
		Operation.VectorValue.Y = ClampToRange(Operation.VectorValue.Y);
		Operation.VectorValue.Z = ClampToRange(Operation.VectorValue.Z);
		break;
	case EHyperAIStudioDynamicMaterialOperationKind::SetRotator:
		Operation.VectorValue.X = FRotator::NormalizeAxis(Operation.VectorValue.X);
		Operation.VectorValue.Y = FRotator::NormalizeAxis(Operation.VectorValue.Y);
		Operation.VectorValue.Z = FRotator::NormalizeAxis(Operation.VectorValue.Z);
		if (Value.bHasValueRange && Value.ValueRangeMin >= -180.0 && Value.ValueRangeMax <= 180.0)
		{
			Operation.VectorValue.X = ClampToRange(Operation.VectorValue.X);
			Operation.VectorValue.Y = ClampToRange(Operation.VectorValue.Y);
			Operation.VectorValue.Z = ClampToRange(Operation.VectorValue.Z);
		}
		break;
	case EHyperAIStudioDynamicMaterialOperationKind::SetColor:
	{
		const float X = static_cast<float>(ClampToRange(Operation.VectorValue.X));
		const float Y = static_cast<float>(ClampToRange(Operation.VectorValue.Y));
		const float Z = static_cast<float>(ClampToRange(Operation.VectorValue.Z));
		float W = static_cast<float>(ClampToRange(Operation.VectorValue.W));
		if (Value.ClassPath == UDMMaterialValueFloat3RGB::StaticClass()->GetPathName()) W = 1.0f;
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z) || !FMath::IsFinite(W))
		{
			OutErrorCode = TEXT("color_float_overflow"); return false;
		}
		Operation.VectorValue = FVector4(X, Y, Z, W);
		break;
	}
	default:
		break;
	}
	return true;
}

bool FHyperAIStudioDynamicMaterialContracts::WouldValueSetterHaveEffect(
	const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
	const FHyperAIDynamicMaterialValueView& Value)
{
	if (!IsExactValueOperationMatch(Operation, Value)) return false;
	const auto Nearly = [](const double A, const double B)
	{
		return FMath::IsNearlyEqual(A, B);
	};
	switch (Operation.Kind)
	{
	case EHyperAIStudioDynamicMaterialOperationKind::SetBool:
		return Value.bBoolValue != Operation.bBoolValue;
	case EHyperAIStudioDynamicMaterialOperationKind::SetScalar:
		return !FMath::IsNearlyEqual(
			static_cast<float>(Value.ScalarValue),
			static_cast<float>(Operation.ScalarValue));
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector2:
		return !Nearly(Value.VectorValue.X, Operation.VectorValue.X)
			|| !Nearly(Value.VectorValue.Y, Operation.VectorValue.Y);
	case EHyperAIStudioDynamicMaterialOperationKind::SetVector3:
	case EHyperAIStudioDynamicMaterialOperationKind::SetRotator:
		return !Nearly(Value.VectorValue.X, Operation.VectorValue.X)
			|| !Nearly(Value.VectorValue.Y, Operation.VectorValue.Y)
			|| !Nearly(Value.VectorValue.Z, Operation.VectorValue.Z);
	case EHyperAIStudioDynamicMaterialOperationKind::SetColor:
	{
		const bool bRgbChanged = !FMath::IsNearlyEqual(static_cast<float>(Value.VectorValue.X),
				static_cast<float>(Operation.VectorValue.X))
			|| !FMath::IsNearlyEqual(static_cast<float>(Value.VectorValue.Y),
				static_cast<float>(Operation.VectorValue.Y))
			|| !FMath::IsNearlyEqual(static_cast<float>(Value.VectorValue.Z),
				static_cast<float>(Operation.VectorValue.Z));
		// UE 5.8 Float3RGB::SetValue normalizes alpha to one, but its public
		// no-op gate deliberately compares RGB only. Float4 compares RGBA.
		return bRgbChanged
			|| (Value.ClassPath == UDMMaterialValueFloat4::StaticClass()->GetPathName()
				&& !FMath::IsNearlyEqual(static_cast<float>(Value.VectorValue.W),
					static_cast<float>(Operation.VectorValue.W)));
	}
	default:
		return false;
	}
}

bool FHyperAIStudioDynamicMaterialContracts::IsClosedMutationProjection(
	const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
	const FHyperAIDynamicMaterialAssetRecord& Record)
{
	if (!Record.bRevisionComplete || !Record.bMutationRevisionComplete
		|| Record.MutationRevisionBlockers.Num() != 0
		|| !Record.bComponentOwnershipProjectionComplete
		|| Record.DiskExistence != TEXT("exists") || !Record.bExistsOnDisk
		|| Record.bDetailsTruncated
		|| Record.Family != Operation.TargetFamily || Record.AssetPath != Operation.TargetPath)
		return false;
	const int32 ExpectedPropertyCount =
		static_cast<int32>(EDMMaterialPropertyType::Any)
		- static_cast<int32>(EDMMaterialPropertyType::None) - 1;
	if (Record.MaterialPropertyCount != ExpectedPropertyCount
		|| Record.MaterialProperties.Num() != ExpectedPropertyCount
		|| Record.MaterialProperties.ContainsByPredicate([](const auto& Property)
		{
			return !Property.bSemanticProjectionComplete
				|| Property.ClassPath
					!= FHyperAIStudioDynamicMaterialContracts::ExpectedMaterialPropertyClassPath(
						Property.MaterialProperty)
				|| Property.bHasAlphaValueComponent
					!= HyperAIStudio::DynamicMaterial::Private::DoesBuiltInPropertyRequireAlphaValue(
						Property.MaterialProperty)
				|| (Property.bHasAlphaValueComponent
					&& Property.AlphaValueComponentIdentity.IsEmpty())
				|| !Property.OutputProcessorPath.IsEmpty();
		})
		|| Record.SlotCount != Record.Slots.Num()
		|| Record.Slots.ContainsByPredicate([](const auto& Slot)
		{
			return !Slot.bSemanticProjectionComplete
				|| Slot.ClassPath != UDMMaterialSlot::StaticClass()->GetPathName();
		}))
		return false;
	const auto IsClosedStage = [](const FHyperAIDynamicMaterialStageView& Stage)
	{
		return Stage.bSemanticProjectionComplete
			&& Stage.ClassPath == UDMMaterialStage::StaticClass()->GetPathName()
			&& Stage.SourceClassPath
				== UDMMaterialStageInputValue::StaticClass()->GetPathName()
			&& Stage.Source.ClassPath == Stage.SourceClassPath
			&& Stage.Source.bSemanticProjectionComplete
			&& !Stage.Source.ValueComponentPath.IsEmpty()
			&& Stage.Source.InputConnectors.IsEmpty()
			&& !Stage.Source.OutputConnectors.IsEmpty()
			&& Stage.Source.OutputConnectors.Num()
				<= FHyperAIStudioDynamicMaterialContracts::MaxConnectorsPerComponent
			&& Stage.Inputs.IsEmpty() && Stage.InputConnections.IsEmpty();
	};
	if (Operation.Kind <= EHyperAIStudioDynamicMaterialOperationKind::SetColor)
	{
		const FHyperAIDynamicMaterialValueView* Value = Record.Values.FindByPredicate(
			[&](const FHyperAIDynamicMaterialValueView& Candidate)
			{
				return Candidate.ComponentPath == Operation.ComponentPath;
			});
		return Value && Value->bSemanticProjectionComplete
			&& IsExactValueOperationMatch(Operation, *Value);
	}
	for (const FHyperAIDynamicMaterialLayerView& Layer : Record.Layers)
	{
		if (!Layer.bSemanticProjectionComplete || Layer.EffectCount != 0
			|| Layer.ClassPath != UDMMaterialLayerObject::StaticClass()->GetPathName()
			|| Layer.EffectStackClassPath
				!= UDMMaterialEffectStack::StaticClass()->GetPathName()
			|| Layer.Stages.IsEmpty()
			|| Layer.Stages.ContainsByPredicate([&](const auto& Stage)
			{
				return !IsClosedStage(Stage);
			})) continue;
		if (Operation.Kind == EHyperAIStudioDynamicMaterialOperationKind::SetLayerEnabled
			&& Layer.ComponentPath == Operation.ComponentPath) return true;
		if (Operation.Kind == EHyperAIStudioDynamicMaterialOperationKind::SetStageEnabled)
		{
			const FHyperAIDynamicMaterialStageView* Stage = Layer.Stages.FindByPredicate(
				[&](const FHyperAIDynamicMaterialStageView& Candidate)
				{
					return Candidate.ComponentPath == Operation.ComponentPath;
				});
			if (Stage && IsClosedStage(*Stage)) return true;
		}
	}
	return false;
}

TArray<FHyperAIDynamicMaterialCapabilityStatus>
FHyperAIStudioDynamicMaterialContracts::GetCapabilityMatrix()
{
	auto Resolve = [](FHyperAIDynamicMaterialCapabilityStatus& Status)
	{
		Status.bPluginsInstalled = true; Status.bPluginsEnabled = true; Status.bModulesLoaded = true;
		for (const FString& PluginName : Status.RequiredPlugins)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			Status.bPluginsInstalled &= Plugin.IsValid();
			Status.bPluginsEnabled &= Plugin.IsValid() && Plugin->IsEnabled();
		}
		for (const FString& ModuleName : Status.RequiredModules)
			Status.bModulesLoaded &= FModuleManager::Get().IsModuleLoaded(*ModuleName);
	};

	FHyperAIDynamicMaterialCapabilityStatus Core;
	Core.Family = TEXT("dynamic_material_model_components");
	Core.RequiredPlugins = {TEXT("DynamicMaterial")};
	Core.RequiredModules = {TEXT("DynamicMaterial"), TEXT("DynamicMaterialEditor")};
	Core.bTypedBackendImplemented = false;
		Core.SupportedCases = {
		TEXT("bounded_closed_public_model_and_instance_projection"),
		TEXT("bounded_exact_on_disk_identity_without_load"),
		TEXT("generated_material_compile_dirty_reference_cas"),
		TEXT("pure_non_executable_typed_shadow_and_independent_validation_evidence")};
	Core.DelegatedEpicCases = {
		TEXT("generated_material_expression_authoring"),
		TEXT("hyper_asset_dependency_graph"),
		TEXT("material_instance_parameter_tools"),
		TEXT("texture_import_and_asset_authoring")};
		Core.UnsupportedCases = {
			TEXT("dynamic_model_or_instance_creation_factory_not_public"),
			TEXT("typed_value_layer_or_stage_mutation_without_bounded_async_cas_backend"),
			TEXT("hidden_property_component_parameter_map_or_mid_membership_for_mutation"),
			TEXT("unprojected_blend_cross_slot_stage_source_input_or_nonempty_effect_stack"),
			TEXT("custom_material_property_component_keys_output_processor_or_direct_value_input_topology"),
		TEXT("arbitrary_component_class_or_property_dispatch"),
		TEXT("raw_script_or_reflection_dispatch"),
		TEXT("synchronous_asset_load"),
		TEXT("unbounded_asset_registry_scan")};
	Core.State = TEXT("source_candidate_non_executable_backend_required");
	Core.Remediation = TEXT("bounded_compile_or_runtime_cas_backend_required");
	Resolve(Core);

	FHyperAIDynamicMaterialCapabilityStatus Media;
	Media.Family = TEXT("media_stream_bridge");
	Media.RequiredPlugins = {TEXT("DynamicMaterial"), TEXT("DynamicMaterialMediaStreamBridge"), TEXT("MediaStream")};
	Media.RequiredModules = {TEXT("DynamicMaterial"), TEXT("DynamicMaterialMediaStreamBridge")};
	Media.bTypedBackendImplemented = false;
	Media.SupportedCases = {TEXT("exact_bridge_value_class_and_generated_texture_detection")};
	Media.DelegatedEpicCases = {TEXT("media_stream_asset_creation_and_player_control")};
	Media.UnsupportedCases = {
		TEXT("media_stream_source_identity_without_optional_typed_facade"),
		TEXT("media_stream_value_mutation_without_public_setter")};
	Media.State = TEXT("optional_typed_facade_gap_fail_closed");
	Media.Remediation = TEXT("Add a separately admitted optional bridge facade only if Epic exposes a public typed setter/source identity; never reflect private MediaStream storage.");
	Resolve(Media);
	return {Core, Media};
}

FString FHyperAIStudioDynamicMaterialContracts::PayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dynamic_material.payload.v1|exact_target|projected_cas|component_path|closed_root_property_slot_runtime_value_direct_stage|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioDynamicMaterialContracts::ResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("dynamic_material.result.v1|phase|revision|valid|error_count|bounded"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioDynamicMaterialContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.dynamic_material.model_components.ue58");
		Value.SemanticVersion = TEXT("1.0.0"); Value.AdapterVersion = 1;
		Value.ApplicableNonBlockingRequirementGroupIds = {TEXT("dynamic_material_variant")};
		Value.Variants.Add({TEXT("hyper_dynamic_material_apply_plan"), MutationVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), ResultTypeId, ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioDynamicMaterialContracts::ValidateOperationShape(
	const FHyperAIDynamicMaterialPlanOperation& Operation,
	FHyperAIStudioDynamicMaterialBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError,
	const double Deadline)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	OutOperation = {}; OutErrorCode.Reset(); OutError.Reset();
	auto Fail = [&](const TCHAR* Code, const TCHAR* Message)
	{
		OutErrorCode = Code; OutError = Message; return false;
	};
	if (!IsBeforeDeadline(Deadline))
		return Fail(TEXT("planning_deadline_exceeded"), TEXT("Operation validation started after its caller-owned deadline."));
	if (!IsCanonicalProjectObjectPath(Operation.TargetPath))
		return Fail(TEXT("invalid_target_path"), TEXT("Target must be one canonical /Game object path."));
	if (!HyperAIStudio::DynamicMaterial::Private::IsAllowedFamily(Operation.TargetFamily))
		return Fail(TEXT("invalid_target_family"), TEXT("target_family must be the exact DynamicMaterial instance or model family."));
	if (!IsCanonicalSha256(Operation.ExpectedRevision))
		return Fail(TEXT("invalid_expected_revision"), TEXT("Every DynamicMaterial edit requires one complete inspector-issued CAS."));
	if (Operation.ComponentPath.IsEmpty() || Operation.ComponentPath.Len() > MaxComponentPathCharacters
		|| Operation.ComponentPath.Contains(TEXT("*")) || Operation.ComponentPath.Contains(TEXT("?")))
		return Fail(TEXT("invalid_component_path"), TEXT("component_path must be one bounded exact inspector-issued model path."));
	for (int32 Index = 0; Index < Operation.ComponentPath.Len(); ++Index)
	{
		if (!IsBeforeDeadline(Deadline))
			return Fail(TEXT("planning_deadline_exceeded"), TEXT("Component-path validation exhausted its caller-owned deadline."));
		if (Operation.ComponentPath[Index] == TEXT('\0')) return Fail(TEXT("invalid_component_path"), TEXT("component_path contains an embedded null."));
	}
	if (!FMath::IsFinite(Operation.ScalarValue) || !FMath::IsFinite(Operation.VectorValue.X)
		|| !FMath::IsFinite(Operation.VectorValue.Y) || !FMath::IsFinite(Operation.VectorValue.Z)
		|| !FMath::IsFinite(Operation.VectorValue.W))
		return Fail(TEXT("non_finite_value"), TEXT("All closed numeric values must be finite."));
	static const TMap<FString, EHyperAIStudioDynamicMaterialOperationKind> Kinds = {
		{TEXT("value.set_bool"), EHyperAIStudioDynamicMaterialOperationKind::SetBool},
		{TEXT("value.set_scalar"), EHyperAIStudioDynamicMaterialOperationKind::SetScalar},
		{TEXT("value.set_vector2"), EHyperAIStudioDynamicMaterialOperationKind::SetVector2},
		{TEXT("value.set_vector3"), EHyperAIStudioDynamicMaterialOperationKind::SetVector3},
		{TEXT("value.set_rotator"), EHyperAIStudioDynamicMaterialOperationKind::SetRotator},
		{TEXT("value.set_color"), EHyperAIStudioDynamicMaterialOperationKind::SetColor},
		{TEXT("layer.set_enabled"), EHyperAIStudioDynamicMaterialOperationKind::SetLayerEnabled},
		{TEXT("stage.set_enabled"), EHyperAIStudioDynamicMaterialOperationKind::SetStageEnabled}};
	const EHyperAIStudioDynamicMaterialOperationKind* Kind = Kinds.Find(Operation.Type);
	if (!Kind) return Fail(TEXT("unsupported_operation"), TEXT("Operation is outside the closed type-specific value/layer/stage vocabulary."));
	const bool bVector = *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetVector2
		|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetVector3
		|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetRotator
		|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetColor;
	const bool bScalar = *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetScalar;
	const bool bBoolean = *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetBool
		|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetLayerEnabled
		|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetStageEnabled;
	if (!bBoolean && Operation.bBoolValue)
		return Fail(TEXT("unused_bool_field"), TEXT("bool_value must remain default for non-boolean discriminants."));
	if (!bScalar && Operation.ScalarValue != 0.0)
		return Fail(TEXT("unused_scalar_field"), TEXT("scalar_value must remain default for non-scalar discriminants."));
	if (!bVector && !Operation.VectorValue.Equals(FVector4(0, 0, 0, 0)))
		return Fail(TEXT("unused_vector_field"), TEXT("vector_value must remain default for non-vector discriminants."));
	if (*Kind == EHyperAIStudioDynamicMaterialOperationKind::SetVector2
		&& (Operation.VectorValue.Z != 0.0 || Operation.VectorValue.W != 0.0))
		return Fail(TEXT("unused_vector_component"), TEXT("vector2 accepts exactly X/Y; Z/W must remain default."));
	if ((*Kind == EHyperAIStudioDynamicMaterialOperationKind::SetVector3
			|| *Kind == EHyperAIStudioDynamicMaterialOperationKind::SetRotator)
		&& Operation.VectorValue.W != 0.0)
		return Fail(TEXT("unused_vector_component"), TEXT("vector3/rotator accept exactly X/Y/Z; W must remain default."));
	OutOperation.Kind = *Kind; OutOperation.Type = Operation.Type;
	OutOperation.TargetPath = Operation.TargetPath; OutOperation.TargetFamily = Operation.TargetFamily;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision; OutOperation.ComponentPath = Operation.ComponentPath;
	OutOperation.bBoolValue = Operation.bBoolValue;
	OutOperation.ScalarValue = Operation.ScalarValue;
	OutOperation.VectorValue = Operation.VectorValue;
	if (!IsBeforeDeadline(Deadline))
		return Fail(TEXT("planning_deadline_exceeded"), TEXT("Operation normalization exhausted its caller-owned deadline."));
	if (*Kind == EHyperAIStudioDynamicMaterialOperationKind::SetScalar)
	{
		const float Persisted = static_cast<float>(Operation.ScalarValue);
		if (!FMath::IsFinite(Persisted))
			return Fail(TEXT("scalar_float_overflow"), TEXT("Scalar must round to a finite persisted float."));
		OutOperation.ScalarValue = static_cast<double>(Persisted);
	}
	if (*Kind == EHyperAIStudioDynamicMaterialOperationKind::SetColor)
	{
		const float X = static_cast<float>(Operation.VectorValue.X);
		const float Y = static_cast<float>(Operation.VectorValue.Y);
		const float Z = static_cast<float>(Operation.VectorValue.Z);
		const float W = static_cast<float>(Operation.VectorValue.W);
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y)
			|| !FMath::IsFinite(Z) || !FMath::IsFinite(W))
			return Fail(TEXT("color_float_overflow"), TEXT("Color components must round to finite persisted floats."));
		OutOperation.VectorValue = FVector4(X, Y, Z, W);
	}
	return true;
}

bool FHyperAIStudioDynamicMaterialContracts::CaptureExact(
	const FString& Path,
	const FString& Family,
	const FString& Scope,
	const int32 MaxComponentsValue,
	const int32 MaxWorkMs,
	FHyperAIStudioDynamicMaterialValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	OutSnapshot = {}; OutStatus.Reset(); OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required"); OutDiagnostic = TEXT("Loaded model capture is serialized on the game thread."); return false;
	}
	if (!IsCanonicalProjectObjectPath(Path) || !IsAllowedFamily(Family) || !IsAllowedScope(Scope)
		|| MaxComponentsValue < 1 || MaxComponentsValue > MaxComponents || MaxWorkMs < 1 || MaxWorkMs > 2000)
	{
		OutStatus = TEXT("invalid_request_bounds"); OutDiagnostic = TEXT("Exact path/family/scope/component/deadline bounds are invalid."); return false;
	}
	const double Deadline = FPlatformTime::Seconds() + static_cast<double>(MaxWorkMs) / 1000.0;
	if (Scope == TEXT("on_disk_index"))
	{
		auto& Record = OutSnapshot.Record;
		Record.Family = Family; Record.AssetPath = Path;
		UObject* LoadedExactObject = FSoftObjectPath(Path).ResolveObject();
		Record.bLoaded = LoadedExactObject != nullptr;
		bool bDiskComplete = true;
		const UE::AssetRegistry::EExists DiskState = CaptureDisk(
			Path, Record, bDiskComplete, LoadedExactObject, Deadline);
		if (DiskState == UE::AssetRegistry::EExists::Unknown || !bDiskComplete)
		{
			OutStatus = TEXT("asset_registry_state_unknown");
			OutDiagnostic = TEXT("Exact on-disk existence or package evidence is Unknown; absence is not inferred and no load was attempted.");
			return false;
		}
		if (DiskState == UE::AssetRegistry::EExists::DoesNotExist)
		{
			OutStatus = TEXT("asset_not_found_on_disk"); OutDiagnostic = TEXT("Asset Registry proved exact on-disk absence; no load was attempted."); return false;
		}
		if (!LoadedExactObject)
		{
			OutStatus = TEXT("asset_registry_object_identity_unavailable");
			OutDiagnostic = TEXT("Nonblocking package evidence cannot prove an unloaded exact object row or class; no load was attempted.");
			return false;
		}
		const FTopLevelAssetPath Expected = Family == TEXT("dynamic_material_instance")
			? UDynamicMaterialInstance::StaticClass()->GetClassPathName()
			: UDynamicMaterialModel::StaticClass()->GetClassPathName();
		if (LoadedExactObject->GetClass()->GetClassPathName() != Expected)
		{
			OutStatus = TEXT("exact_type_mismatch"); OutDiagnostic = TEXT("On-disk class is outside the exact requested DynamicMaterial family."); return false;
		}
		Record.ClassPath = Expected.ToString();
		Record.bRevisionComplete = false;
		OutSnapshot.bComplete = false; OutSnapshot.Revision = ComputeSnapshotRevision(
			OutSnapshot, Deadline); Record.Revision = OutSnapshot.Revision;
		OutStatus = TEXT("on_disk_identity_only");
		OutDiagnostic = TEXT("Nonblocking package evidence plus already-loaded exact object/class/package identity was captured without loading or model traversal; dependency fanout is delegated and model/dirty/compile CAS remains intentionally incomplete.");
		return true;
	}
	UObject* Object = FSoftObjectPath(Path).ResolveObject();
	if (!Object)
	{
		OutStatus = TEXT("asset_not_loaded"); OutDiagnostic = TEXT("Exact target is not already loaded; synchronous loading is forbidden."); return false;
	}
	if (!IsExactLoadedFamily(Object, Family))
	{
		OutStatus = TEXT("exact_type_mismatch"); OutDiagnostic = TEXT("Loaded object is not the exact requested DynamicMaterial type; derived assets are rejected."); return false;
	}
	return FillLoaded(Object, Family, MaxComponentsValue, Deadline,
		OutSnapshot, OutStatus, OutDiagnostic);
}

FString FHyperAIStudioDynamicMaterialContracts::ComputeSnapshotRevision(
	FHyperAIStudioDynamicMaterialValueSnapshot& Snapshot,
	const double Deadline)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	FString Canonical;
	if (!TryCanonicalRecord(Snapshot.Record, Deadline, Canonical)
		|| !IsBeforeDeadline(Deadline))
	{
		Snapshot.Revision.Reset();
		Snapshot.Record.Revision.Reset();
		Snapshot.Record.bRevisionComplete = false;
		Snapshot.bComplete = false;
		return Snapshot.Revision;
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Snapshot.Revision) || !IsBeforeDeadline(Deadline))
	{
		Snapshot.Revision.Reset();
		Snapshot.Record.bRevisionComplete = false;
		Snapshot.bComplete = false;
	}
	Snapshot.Record.Revision = Snapshot.Revision;
	return Snapshot.Revision;
}

TArray<FHyperAIDynamicMaterialIssue>
FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
	const FHyperAIStudioDynamicMaterialValueSnapshot& Snapshot,
	const bool bRequirePackageClean,
	const int32 MaxIssueCount,
	bool& bOutTruncated,
	const double Deadline)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	TArray<FHyperAIDynamicMaterialIssue> Issues; bOutTruncated = false;
	const int32 Limit = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	const auto& Record = Snapshot.Record;
	auto AbortForDeadline = [&]()
	{
		if (IsBeforeDeadline(Deadline)) return false;
		AddIssue(Issues, Limit, bOutTruncated, TEXT("validation_deadline_exceeded"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Independent validation exhausted its caller-owned deadline."));
		bOutTruncated = true;
		return true;
	};
	if (AbortForDeadline()) return Issues;
	if (!Snapshot.bComplete || !Record.bRevisionComplete || !IsCanonicalSha256(Snapshot.Revision))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("revision_incomplete"), TEXT("error"), Record.AssetPath,
			FString(), -1, TEXT("Independent validation requires one complete fresh loaded DynamicMaterial CAS."));
		if (Record.DiskExistence == TEXT("unknown")
			|| Record.ReferenceEvidence
				!= TEXT("typed_object_paths_and_generated_material_exact;asset_registry_fanout_delegated")
			|| Record.ComponentProjectionEvidence
				!= TEXT("closed_public_projection_with_owner_parent_roundtrip_v2;mutation_hidden_membership_fail_closed")
			|| !Record.bComponentOwnershipProjectionComplete)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("reference_or_disk_evidence_unproven"), TEXT("error"),
			Record.AssetPath, FString(), -1,
			TEXT("Complete CAS requires exact tri-state disk evidence and bounded typed object-reference scope."));
	if (!Record.bModelValid)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("model_invalid"), TEXT("error"), Record.AssetPath,
			FString(), -1, TEXT("Resolved DynamicMaterial model reports invalid state."));
	if (Record.ModelClassPath != UDynamicMaterialModel::StaticClass()->GetPathName()
		|| (Record.Family == TEXT("dynamic_material_instance")
			&& Record.AssociatedInstancePath != Record.AssetPath))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("model_type_or_owner_projection_invalid"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Resolved model class or owning DynamicMaterial instance relation is not exact."));
	if (!Record.bEditorModelDataAvailable)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("editor_model_data_missing"), TEXT("error"), Record.AssetPath,
			FString(), -1, TEXT("Layer/stage and generated-material editor data is unavailable."));
	if (Record.EditorState != static_cast<int32>(EDMState::Idle)
		|| Record.bBuildRequested || Record.bNeedsWizard)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("model_editor_state_not_stable"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Editor model is building, has a queued build, or still requires its wizard."));
	if (bRequirePackageClean && (Record.bPackageDirty || Record.bGeneratedMaterialPackageDirty))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("package_dirty"), TEXT("error"), Record.AssetPath,
			FString(), -1, TEXT("Target or generated-material package is dirty while persisted clean state was required."));
	if (!Record.bCompileStateKnown || Record.bCompiling || Record.bCompileError)
		AddIssue(Issues, Limit, bOutTruncated,
			!Record.bCompileStateKnown ? TEXT("compile_state_unproven")
				: (Record.bCompiling ? TEXT("compile_in_progress") : TEXT("compile_error")),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Generated material compile state is unknown, active, or failed."));
	const bool bRootEnumsValid = Record.MaterialDomain >= 0
		&& Record.MaterialDomain < static_cast<int32>(MD_MAX)
		&& Record.BlendMode >= 0 && Record.BlendMode < static_cast<int32>(BLEND_MAX)
		&& (Record.ShadingModel == static_cast<int32>(EDMMaterialShadingModel::Unlit)
			|| Record.ShadingModel == static_cast<int32>(EDMMaterialShadingModel::DefaultLit));
	if (!bRootEnumsValid || !FMath::IsFinite(Record.OpacityMaskClipValue)
		|| !FMath::IsFinite(Record.DisplacementCenter)
		|| !FMath::IsFinite(Record.DisplacementMagnitude))
		AddIssue(Issues, Limit, bOutTruncated, TEXT("model_root_projection_invalid"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Closed model domain/blend/shading/flag numeric root state is invalid."));
	const int32 ExpectedPropertyCount =
		static_cast<int32>(EDMMaterialPropertyType::Any)
		- static_cast<int32>(EDMMaterialPropertyType::None) - 1;
	if (Record.MaterialPropertyCount != ExpectedPropertyCount
		|| Record.MaterialProperties.Num() != ExpectedPropertyCount)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("material_property_projection_incomplete"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Every finite UE 5.8 DynamicMaterial property must have one closed projection."));
	if (Record.ValueCount != Record.Values.Num() || Record.SlotCount != Record.Slots.Num()
		|| Record.LayerCount != Record.Layers.Num())
		AddIssue(Issues, Limit, bOutTruncated, TEXT("component_count_projection_mismatch"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Captured public collection counts do not match the sealed bounded projection."));
	TSet<FString> ComponentPaths;
	TSet<FString> SlotPaths;
	for (const FHyperAIDynamicMaterialSlotView& Slot : Record.Slots)
	{
		if (AbortForDeadline()) return Issues;
		if (Slot.ComponentPath.IsEmpty() || !AddUnique(ComponentPaths, Slot.ComponentPath)
			|| !AddUnique(SlotPaths, Slot.ComponentPath))
			AddIssue(Issues, Limit, bOutTruncated,
				TEXT("component_path_missing_or_duplicate"), TEXT("error"),
				Record.AssetPath, Slot.ComponentPath, -1,
				TEXT("Slot component identity is empty or duplicated."));
		if (!Slot.bSemanticProjectionComplete
			|| Slot.ClassPath != UDMMaterialSlot::StaticClass()->GetPathName()
			|| Slot.LifetimeState < 0
			|| Slot.SlotIndex < 0 || Slot.SlotIndex >= Record.SlotCount
			|| Slot.OutputConnectorTypeSets.Num() != ExpectedPropertyCount
			|| Slot.ReferencedBySlotCounts.Num() > MaxComponents)
			AddIssue(Issues, Limit, bOutTruncated, TEXT("slot_semantic_projection_incomplete"),
				TEXT("error"), Record.AssetPath, Slot.ComponentPath, -1,
				TEXT("Slot index, connector types, reference membership, or public state is incomplete."));
		TSet<FString> ReferenceIdentities;
		for (const FString& ReferenceIdentity : Slot.ReferencedBySlotCounts)
		{
			if (AbortForDeadline()) return Issues;
			if (ReferenceIdentity.IsEmpty()
				|| !AddUnique(ReferenceIdentities, ReferenceIdentity))
				AddIssue(Issues, Limit, bOutTruncated,
					TEXT("slot_reference_identity_invalid"), TEXT("error"),
					Record.AssetPath, Slot.ComponentPath, -1,
					TEXT("Slot reference membership is empty or duplicated."));
		}
	}
	for (const auto& Value : Record.Values)
	{
		if (AbortForDeadline()) return Issues;
		if (Value.ComponentPath.IsEmpty() || !AddUnique(ComponentPaths, Value.ComponentPath))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("component_path_missing_or_duplicate"), TEXT("error"),
				Record.AssetPath, Value.ComponentPath, -1, TEXT("Value component identity is empty or duplicated."));
		if (!Value.bSemanticProjectionComplete
			|| !FMath::IsFinite(Value.ScalarValue) || !FMath::IsFinite(Value.VectorValue.X)
			|| !FMath::IsFinite(Value.VectorValue.Y) || !FMath::IsFinite(Value.VectorValue.Z)
			|| !FMath::IsFinite(Value.VectorValue.W) || !FMath::IsFinite(Value.ValueRangeMin)
			|| !FMath::IsFinite(Value.ValueRangeMax)
			|| !FMath::IsFinite(Value.DefaultScalarValue)
			|| !FMath::IsFinite(Value.DefaultVectorValue.X)
			|| !FMath::IsFinite(Value.DefaultVectorValue.Y)
			|| !FMath::IsFinite(Value.DefaultVectorValue.Z)
			|| !FMath::IsFinite(Value.DefaultVectorValue.W)
			|| (Value.bHasValueRange && Value.ValueRangeMin > Value.ValueRangeMax))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("non_finite_value"), TEXT("error"),
				Record.AssetPath, Value.ComponentPath, -1, TEXT("Typed model value or public setter range is invalid."));
		if (Value.ValueKind == TEXT("unsupported"))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("unsupported_value_class"), TEXT("error"),
				Record.AssetPath, Value.ComponentPath, -1, TEXT("Value class has no closed typed semantic projection."));
		if (Value.bMediaBridgeValue && !Value.bMediaSourceIdentityKnown)
			AddIssue(Issues, Limit, bOutTruncated, TEXT("media_bridge_source_identity_unproven"), TEXT("error"),
				Record.AssetPath, Value.ComponentPath, -1, TEXT("Optional bridge does not expose media-source identity through the base typed facade."));
		if (Value.bHasExplicitParameter
			&& (Value.ParameterComponentPath.IsEmpty()
				|| Value.ParameterClassPath != UDMMaterialParameter::StaticClass()->GetPathName()
				|| Value.ParameterParentComponentPath != Value.ComponentPath
				|| Value.ParameterLifetimeState < 0
				|| Value.ExplicitParameterName.IsEmpty()))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("value_parameter_projection_invalid"),
				TEXT("error"), Record.AssetPath, Value.ComponentPath, -1,
				TEXT("Explicit parameter identity is not closed and exact."));
	}
	TSet<FString> ValuePaths;
	for (const auto& Value : Record.Values)
	{
		if (AbortForDeadline()) return Issues;
		ValuePaths.Add(Value.ComponentPath);
	}
	TSet<int32> MaterialPropertyTypes;
	for (const FHyperAIDynamicMaterialPropertyView& Property : Record.MaterialProperties)
	{
		if (AbortForDeadline()) return Issues;
		if (Property.ComponentPath.IsEmpty()
			|| !AddUnique(ComponentPaths, Property.ComponentPath))
			AddIssue(Issues, Limit, bOutTruncated,
				TEXT("component_path_missing_or_duplicate"), TEXT("error"),
				Record.AssetPath, Property.ComponentPath, -1,
				TEXT("Material-property component identity is empty or duplicated."));
		const bool bPropertyTypeValid = Property.MaterialProperty
			> static_cast<int32>(EDMMaterialPropertyType::None)
			&& Property.MaterialProperty < static_cast<int32>(EDMMaterialPropertyType::Any)
			&& AddUnique(MaterialPropertyTypes, Property.MaterialProperty);
		if (!Property.bSemanticProjectionComplete || !bPropertyTypeValid
			|| Property.LifetimeState < 0
			|| Property.ClassPath != ExpectedMaterialPropertyClassPath(
				Property.MaterialProperty)
			|| !Property.OutputProcessorPath.IsEmpty()
			|| Property.bHasAlphaValueComponent
				!= DoesBuiltInPropertyRequireAlphaValue(Property.MaterialProperty)
			|| (Property.bHasAlphaValueComponent
				&& Property.AlphaValueComponentIdentity.IsEmpty())
			|| Property.InputChannels.Num() > MaxChannelsPerConnection
			|| (!Property.SlotComponentPath.IsEmpty()
				&& !SlotPaths.Contains(Property.SlotComponentPath)))
			AddIssue(Issues, Limit, bOutTruncated,
				TEXT("material_property_projection_incomplete"), TEXT("error"),
				Record.AssetPath, Property.ComponentPath, -1,
				TEXT("Material-property type/input/processor/alpha/slot state is outside the closed projection."));
		for (const FHyperAIDynamicMaterialConnectionChannelView& Channel : Property.InputChannels)
		{
			if (AbortForDeadline()) return Issues;
			if (Channel.SourceIndex < FDMMaterialStageConnectorChannel::NO_SOURCE
				|| Channel.OutputIndex < 0 || Channel.OutputChannel < 0
				|| Channel.OutputChannel > FDMMaterialStageConnectorChannel::FOUR_CHANNELS)
				AddIssue(Issues, Limit, bOutTruncated,
					TEXT("material_property_connection_invalid"), TEXT("error"),
					Record.AssetPath, Property.ComponentPath, -1,
					TEXT("Material-property input channel tuple is outside closed UE 5.8 bounds."));
		}
	}
	TSet<FString> RuntimeIdentities;
	for (const FString& RuntimeIdentity : Record.RuntimeComponentIdentities)
	{
		if (AbortForDeadline()) return Issues;
		if (RuntimeIdentity.IsEmpty() || !AddUnique(RuntimeIdentities, RuntimeIdentity))
			AddIssue(Issues, Limit, bOutTruncated,
				TEXT("runtime_component_identity_invalid"), TEXT("error"),
				Record.AssetPath, FString(), -1,
				TEXT("Runtime-component public membership identity is empty or duplicated."));
	}
	auto ValidateComponent = [&](const FHyperAIDynamicMaterialComponentView& Component)
		{
			if (AbortForDeadline()) return false;
			if (!Component.bSemanticProjectionComplete
				|| Component.ClassPath
					!= UDMMaterialStageInputValue::StaticClass()->GetPathName()
				|| Component.LifetimeState < 0
				|| Component.ComponentPath.IsEmpty()
				|| Component.ValueComponentPath.IsEmpty()
				|| Component.OutputConnectors.IsEmpty()
				|| Component.InputConnectors.Num() > MaxConnectorsPerComponent
				|| Component.OutputConnectors.Num() > MaxConnectorsPerComponent
				|| Component.EditablePropertyNames.Num() > MaxEditablePropertiesPerComponent)
				AddIssue(Issues, Limit, bOutTruncated, TEXT("component_semantic_projection_incomplete"),
					TEXT("error"), Record.AssetPath, Component.ComponentPath, -1,
					TEXT("Source/input instance state is outside the bounded closed public projection."));
			if (!Component.ValueComponentPath.IsEmpty()
				&& !ValuePaths.Contains(Component.ValueComponentPath))
				AddIssue(Issues, Limit, bOutTruncated, TEXT("component_value_reference_missing"),
					TEXT("error"), Record.AssetPath, Component.ComponentPath, -1,
					TEXT("A stage input references a value absent from the exact bounded value projection."));
			TSet<int32> ConnectorIndices;
			for (const FHyperAIDynamicMaterialConnectorView& Connector : Component.OutputConnectors)
			{
				if (AbortForDeadline()) return false;
				if (Connector.Index < 0 || !AddUnique(ConnectorIndices, Connector.Index)
					|| Connector.Name.Len() > MaxComponentPathCharacters
					|| Connector.ValueType <= static_cast<int32>(EDMValueType::VT_None)
					|| Connector.ValueType >= static_cast<int32>(EDMValueType::VT_MAX))
					AddIssue(Issues, Limit, bOutTruncated,
						TEXT("component_connector_projection_invalid"), TEXT("error"),
						Record.AssetPath, Component.ComponentPath, -1,
						TEXT("Source connector index/name/value type is outside the closed schema."));
			}
			return true;
	};
	int32 CountedStages = 0;
	for (const auto& Layer : Record.Layers)
		{
			if (AbortForDeadline()) return Issues;
			if (Layer.ComponentPath.IsEmpty() || !AddUnique(ComponentPaths, Layer.ComponentPath))
			AddIssue(Issues, Limit, bOutTruncated, TEXT("component_path_missing_or_duplicate"), TEXT("error"),
				Record.AssetPath, Layer.ComponentPath, -1, TEXT("Layer component identity is empty or duplicated."));
			if (!Layer.bSemanticProjectionComplete || Layer.EffectCount != 0
				|| Layer.ClassPath != UDMMaterialLayerObject::StaticClass()->GetPathName()
				|| Layer.EffectStackClassPath
					!= UDMMaterialEffectStack::StaticClass()->GetPathName()
				|| Layer.LifetimeState < 0 || Layer.EffectStackLifetimeState < 0
				|| Layer.SlotIndex < 0 || Layer.SlotIndex >= Record.SlotCount
				|| Layer.LayerIndex < 0
				|| Layer.Stages.IsEmpty()
				|| Layer.MaterialProperty <= static_cast<int32>(EDMMaterialPropertyType::None)
				|| Layer.MaterialProperty >= static_cast<int32>(EDMMaterialPropertyType::Any))
				AddIssue(Issues, Limit, bOutTruncated, TEXT("layer_semantic_projection_incomplete"),
					TEXT("error"), Record.AssetPath, Layer.ComponentPath, -1,
					TEXT("Layer, effect-stack, or child stage state is outside the closed projection."));
			if (Layer.EffectStackComponentPath.IsEmpty()
				|| !AddUnique(ComponentPaths, Layer.EffectStackComponentPath))
				AddIssue(Issues, Limit, bOutTruncated,
					TEXT("component_path_missing_or_duplicate"), TEXT("error"),
					Record.AssetPath, Layer.EffectStackComponentPath, -1,
					TEXT("Effect-stack component identity is empty or duplicated."));
			TSet<int32> StageIndices;
			TSet<int32> StageTypes;
			for (const auto& Stage : Layer.Stages)
			{
				if (AbortForDeadline()) return Issues;
				++CountedStages;
				if (!AddUnique(StageIndices, Stage.StageIndex)
					|| !AddUnique(StageTypes, Stage.StageType))
					AddIssue(Issues, Limit, bOutTruncated,
						TEXT("stage_identity_duplicate"), TEXT("error"),
						Record.AssetPath, Stage.ComponentPath, -1,
						TEXT("Layer stage index or Base/Mask role is duplicated."));
				if (Stage.ComponentPath.IsEmpty() || !AddUnique(ComponentPaths, Stage.ComponentPath))
					AddIssue(Issues, Limit, bOutTruncated, TEXT("component_path_missing_or_duplicate"), TEXT("error"),
						Record.AssetPath, Stage.ComponentPath, -1, TEXT("Stage component identity is empty or duplicated."));
				if (!Stage.bSemanticProjectionComplete
					|| Stage.ClassPath != UDMMaterialStage::StaticClass()->GetPathName()
					|| Stage.LifetimeState < 0
					|| (Stage.StageType != static_cast<int32>(EDMMaterialLayerStage::Base)
						&& Stage.StageType != static_cast<int32>(EDMMaterialLayerStage::Mask))
					|| Stage.StageIndex < 0 || Stage.StageIndex >= Layer.Stages.Num()
					|| Stage.SourceClassPath != Stage.Source.ClassPath
					|| Stage.SourceClassPath
						!= UDMMaterialStageInputValue::StaticClass()->GetPathName()
					|| !Stage.Source.InputConnectors.IsEmpty()
					|| !Stage.Inputs.IsEmpty() || !Stage.InputConnections.IsEmpty()
					|| Stage.Inputs.Num() > MaxConnectorsPerComponent
					|| Stage.InputConnections.Num() > MaxConnectorsPerComponent)
					AddIssue(Issues, Limit, bOutTruncated, TEXT("stage_semantic_projection_incomplete"),
						TEXT("error"), Record.AssetPath, Stage.ComponentPath, -1,
						TEXT("Stage source, input, or connection-map state is incomplete."));
				if (Stage.Source.ComponentPath.IsEmpty()
					|| !AddUnique(ComponentPaths, Stage.Source.ComponentPath))
					AddIssue(Issues, Limit, bOutTruncated, TEXT("component_path_missing_or_duplicate"),
						TEXT("error"), Record.AssetPath, Stage.Source.ComponentPath, -1,
						TEXT("Stage source component identity is duplicated."));
				if (!ValidateComponent(Stage.Source)) return Issues;
				for (const auto& Input : Stage.Inputs)
				{
					if (AbortForDeadline()) return Issues;
					if (Input.ComponentPath.IsEmpty() || !AddUnique(ComponentPaths, Input.ComponentPath))
						AddIssue(Issues, Limit, bOutTruncated,
							TEXT("component_path_missing_or_duplicate"), TEXT("error"),
							Record.AssetPath, Input.ComponentPath, -1,
							TEXT("Stage input component identity is empty or duplicated."));
					if (!ValidateComponent(Input)) return Issues;
				}
				for (const auto& Connection : Stage.InputConnections)
				{
					if (AbortForDeadline()) return Issues;
					if (Connection.Channels.Num() > MaxChannelsPerConnection)
						AddIssue(Issues, Limit, bOutTruncated,
							TEXT("stage_connection_bound_exceeded"), TEXT("error"),
							Record.AssetPath, Stage.ComponentPath, -1,
							TEXT("Stage input connection exceeds the closed channel bound."));
					for (const auto& Channel : Connection.Channels)
					{
						if (AbortForDeadline()) return Issues;
						if (Channel.SourceIndex < FDMMaterialStageConnectorChannel::NO_SOURCE
							|| Channel.SourceIndex > Stage.Inputs.Num())
							AddIssue(Issues, Limit, bOutTruncated,
								TEXT("stage_connection_source_invalid"), TEXT("error"),
								Record.AssetPath, Stage.ComponentPath, -1,
								TEXT("Stage connection source index is outside previous-stage/input bounds."));
					}
				}
			}
		}
	if (CountedStages != Record.StageCount)
		AddIssue(Issues, Limit, bOutTruncated, TEXT("component_count_projection_mismatch"),
			TEXT("error"), Record.AssetPath, FString(), -1,
			TEXT("Captured stage count does not match the sealed ordered layer projection."));
	return Issues;
}

FString FHyperAIStudioDynamicMaterialContracts::ComputePayloadSemanticFingerprint(
	const TArray<FHyperAIStudioDynamicMaterialBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	FString Out; AppendToken(Out, TEXT("hyperai.dynamic_material.payload.semantic.v1")); AppendToken(Out, BaseRevision);
	for (const auto& Operation : Operations) AppendToken(Out, CanonicalOperation(Operation));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Out);
}

FHyperAIDynamicMaterialApplyPlanReport FHyperAIStudioDynamicMaterialContracts::BuildPlan(
	const FHyperAIDynamicMaterialApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	FHyperAIDynamicMaterialApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun; Report.OperationId = Clip(Request.OperationId, 256);
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required"); Report.Diagnostic = TEXT("DynamicMaterial CAS planning runs on the game thread."); return Report;
	}
	if (Request.OperationId.Len() > 256
		|| Request.ExpectedPlanHash.Len() > 71
		|| (!Request.ExpectedPlanHash.IsEmpty() && !IsCanonicalSha256(Request.ExpectedPlanHash))
		|| Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 30000
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > 2000
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds"); Report.Diagnostic = TEXT("Operation identity/count, expected hash, deadline, game-thread budget, or output bounds are invalid."); return Report;
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(FMath::Min(Request.DeadlineMs, Request.MaxGameThreadMs)) / 1000.0;
	const int32 IssueLimit = FMath::Clamp(Request.MaxOutputBytes / 16384, 1, MaxIssues);
	TArray<FHyperAIStudioDynamicMaterialBackendOperation> Operations;
	TMap<FString, FHyperAIStudioDynamicMaterialValueSnapshot> Snapshots;
	TSet<FString> ComponentWrites;
	bool bTruncated = false;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		if (!IsBeforeDeadline(Deadline))
		{
			Report.Status = TEXT("planning_budget_exceeded");
			Report.Diagnostic = TEXT("Bounded typed planning exhausted its total game-thread deadline during shape validation.");
			return Report;
		}
		FHyperAIStudioDynamicMaterialBackendOperation Operation; FString Code, Error;
		if (!ValidateOperationShape(
			Request.Operations[Index], Operation, Code, Error, Deadline))
		{
			AddIssue(Report.Issues, IssueLimit, bTruncated, *Code, TEXT("error"),
				Request.Operations[Index].TargetPath, Request.Operations[Index].ComponentPath, Index, Error);
			continue;
		}
		const FString WriteKey = Operation.TargetPath + TEXT("\n") + Operation.ComponentPath;
		if (!AddUnique(ComponentWrites, WriteKey))
		{
			AddIssue(Report.Issues, IssueLimit, bTruncated, TEXT("contradictory_component_writes"), TEXT("error"),
				Operation.TargetPath, Operation.ComponentPath, Index,
				TEXT("One atomic plan may write an exact model component only once."));
			continue;
		}
		FHyperAIStudioDynamicMaterialValueSnapshot* Snapshot = Snapshots.Find(Operation.TargetPath);
		if (!Snapshot)
		{
			FHyperAIStudioDynamicMaterialValueSnapshot Fresh; FString Status, Diagnostic;
			const double RemainingSeconds = Deadline - FPlatformTime::Seconds();
			if (RemainingSeconds <= 0.0)
			{
				Report.Status = TEXT("planning_budget_exceeded");
				Report.Diagnostic = TEXT("Bounded typed planning exhausted its total game-thread deadline before capture.");
				return Report;
			}
			const int32 CaptureBudgetMs = FMath::Clamp(
				FMath::CeilToInt(RemainingSeconds * 1000.0), 1, Request.MaxGameThreadMs);
			if (!CaptureExact(Operation.TargetPath, Operation.TargetFamily, TEXT("loaded_only"),
				MaxComponents, CaptureBudgetMs, Fresh, Status, Diagnostic))
			{
				AddIssue(Report.Issues, IssueLimit, bTruncated, *Status, TEXT("error"),
					Operation.TargetPath, Operation.ComponentPath, Index, Diagnostic);
				continue;
			}
			if (Fresh.Record.DiskExistence != TEXT("exists") || !Fresh.Record.bExistsOnDisk)
			{
				AddIssue(Report.Issues, IssueLimit, bTruncated,
					TEXT("mutation_target_not_persisted"), TEXT("error"),
					Operation.TargetPath, Operation.ComponentPath, Index,
					TEXT("DynamicMaterial edit planning requires exact Asset Registry Exists; DoesNotExist and Unknown fail closed."));
				continue;
			}
			if (!Fresh.bComplete || Fresh.Revision != Operation.ExpectedRevision)
			{
				AddIssue(Report.Issues, IssueLimit, bTruncated, TEXT("revision_precondition_failed"), TEXT("error"),
					Operation.TargetPath, Operation.ComponentPath, Index,
					TEXT("Fresh complete closed public model projection is incomplete or differs from expected_revision."));
				continue;
			}
			Snapshot = &Snapshots.Add(Operation.TargetPath, MoveTemp(Fresh));
		}
		else if (Snapshot->Record.Family != Operation.TargetFamily
			|| Operation.ExpectedRevision != Snapshot->Revision)
		{
			AddIssue(Report.Issues, IssueLimit, bTruncated, TEXT("conflicting_target_revision_or_family"), TEXT("error"),
				Operation.TargetPath, Operation.ComponentPath, Index,
				TEXT("All operations for one target must bind the same exact family and CAS revision."));
			continue;
		}
		if (!IsClosedMutationProjection(Operation, Snapshot->Record))
		{
			AddIssue(Report.Issues, IssueLimit, bTruncated,
				TEXT("component_projection_not_executable"), TEXT("error"),
				Operation.TargetPath, Operation.ComponentPath, Index,
				TEXT("The exact value/layer/stage target is outside the closed complete value-source-only projection."));
			continue;
		}
		Operations.Add(MoveTemp(Operation));
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Report.Status = TEXT("planning_budget_exceeded"); Report.Diagnostic = TEXT("Bounded typed planning exceeded its deadline.");
			Report.bTruncated = bTruncated; return Report;
		}
	}
	Report.bTruncated = bTruncated;
	if (HasErrors(Report.Issues) || Operations.Num() != Request.Operations.Num())
	{
		Report.Status = TEXT("plan_state_invalid"); Report.Diagnostic = TEXT("Closed shape, component uniqueness, exact type, or loaded CAS preconditions failed."); return Report;
	}

	TMap<FString, FHyperAIStudioDynamicMaterialValueSnapshot> Shadows = Snapshots;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Report.Status = TEXT("planning_budget_exceeded");
			Report.Diagnostic = TEXT("Bounded typed shadow replay exceeded its total game-thread deadline.");
			return Report;
		}
		auto& Operation = Operations[Index];
		auto& Shadow = Shadows.FindChecked(Operation.TargetPath);
		bool bFound = false;
		if (Operation.Kind <= EHyperAIStudioDynamicMaterialOperationKind::SetColor)
		{
			FHyperAIDynamicMaterialValueView* Value = Shadow.Record.Values.FindByPredicate([&](const auto& Item)
			{
				return Item.ComponentPath == Operation.ComponentPath;
			});
			if (Value)
			{
				FString NormalizationError;
					if (NormalizeOperationForCapturedValue(Operation, *Value, NormalizationError))
					{
						if (!WouldValueSetterHaveEffect(Operation, *Value))
						{
							AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
								TEXT("setter_would_be_noop"), TEXT("error"),
								Operation.TargetPath, Operation.ComponentPath, Index,
								TEXT("The normalized request is within the exact public setter no-op tolerance."));
							continue;
						}
						bFound = true;
					if (Operation.Kind == EHyperAIStudioDynamicMaterialOperationKind::SetBool)
						Value->bBoolValue = Operation.bBoolValue;
					else if (Operation.Kind == EHyperAIStudioDynamicMaterialOperationKind::SetScalar)
						Value->ScalarValue = Operation.ScalarValue;
					else
						Value->VectorValue = Operation.VectorValue;
				}
				else if (!NormalizationError.IsEmpty())
				{
					AddIssue(Report.Issues, IssueLimit, Report.bTruncated, *NormalizationError,
						TEXT("error"), Operation.TargetPath, Operation.ComponentPath, Index,
						TEXT("Requested value cannot be normalized to the exact public setter representation."));
				}
			}
		}
		else if (Operation.Kind == EHyperAIStudioDynamicMaterialOperationKind::SetLayerEnabled)
		{
			if (auto* Layer = Shadow.Record.Layers.FindByPredicate([&](const auto& Item)
			{
				return Item.ComponentPath == Operation.ComponentPath;
				}))
				{
					if (Layer->bEnabled == Operation.bBoolValue)
						AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
							TEXT("setter_would_be_noop"), TEXT("error"),
							Operation.TargetPath, Operation.ComponentPath, Index,
							TEXT("The exact public layer setter would reject this no-op."));
					else { Layer->bEnabled = Operation.bBoolValue; bFound = true; }
				}
		}
		else
		{
			for (auto& Layer : Shadow.Record.Layers)
				if (auto* Stage = Layer.Stages.FindByPredicate([&](const auto& Item)
				{
					return Item.ComponentPath == Operation.ComponentPath;
					}))
					{
						if (Stage->bEnabled == Operation.bBoolValue)
							AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
								TEXT("setter_would_be_noop"), TEXT("error"),
								Operation.TargetPath, Operation.ComponentPath, Index,
								TEXT("The exact public stage setter would reject this no-op."));
						else { Stage->bEnabled = Operation.bBoolValue; bFound = true; }
						break;
					}
		}
		if (!bFound)
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("component_type_mismatch_or_missing"), TEXT("error"),
				Operation.TargetPath, Operation.ComponentPath, Index,
				TEXT("Exact component path does not resolve to the operation's closed value/layer/stage type."));
	}
	if (HasErrors(Report.Issues))
	{
		Report.Status = TEXT("typed_shadow_replay_failed"); Report.Diagnostic = TEXT("Typed shadow could not resolve an exact component/type binding."); return Report;
	}
	for (TPair<FString, FHyperAIStudioDynamicMaterialValueSnapshot>& Pair : Shadows)
	{
		const FString BeforeRevision = Pair.Value.Revision;
		ComputeSnapshotRevision(Pair.Value, Deadline);
		if (!Pair.Value.bComplete || !IsCanonicalSha256(Pair.Value.Revision))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
				TEXT("shadow_revision_incomplete"), TEXT("error"), Pair.Key, FString(), -1,
				TEXT("Bounded shadow sealing exceeded its canonical byte or deadline budget."));
			continue;
		}
		if (Pair.Value.Revision == BeforeRevision)
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("plan_has_no_semantic_effect"), TEXT("error"),
				Pair.Key, FString(), -1, TEXT("Typed shadow is identical to the exact captured state."));
			continue;
		}
		bool ValidationTruncated = false;
		const int32 RemainingIssueCapacity = IssueLimit - Report.Issues.Num();
		if (RemainingIssueCapacity <= 0)
		{
			Report.bTruncated = true;
			continue;
		}
		const TArray<FHyperAIDynamicMaterialIssue> Issues = ValidateValueSnapshot(
			Pair.Value, false, RemainingIssueCapacity, ValidationTruncated, Deadline);
		for (const auto& Issue : Issues)
			if (Issue.Severity == TEXT("error")) Report.Issues.Add(Issue);
		Report.bTruncated |= ValidationTruncated;
	}
	if (HasErrors(Report.Issues) || Report.bTruncated)
	{
		Report.Status = TEXT("typed_shadow_validation_failed"); Report.Diagnostic = TEXT("Independent validation rejected the post-plan typed shadow."); return Report;
	}

	FString BaseCanonical; AppendToken(BaseCanonical, TEXT("hyperai.dynamic_material.base.v1"));
		TArray<FString> TargetPaths; Snapshots.GetKeys(TargetPaths); TargetPaths.Sort();
		for (const FString& Target : TargetPaths)
		{
			if (!IsBeforeDeadline(Deadline))
			{
				Report.Status = TEXT("planning_budget_exceeded");
				Report.Diagnostic = TEXT("Base revision sealing exhausted its caller-owned deadline.");
				return Report;
			}
			AppendToken(BaseCanonical, Target); AppendToken(BaseCanonical, Snapshots.FindChecked(Target).Revision);
	}
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	const FString SemanticFingerprint = ComputePayloadSemanticFingerprint(Operations, Report.BaseRevision);
	FString ExpectedPostCanonical;
	AppendToken(ExpectedPostCanonical, TEXT("hyperai.dynamic_material.expected-post-projection-set.v1"));
	TArray<FString> ShadowTargets;
	Shadows.GetKeys(ShadowTargets);
	ShadowTargets.Sort();
		for (const FString& Target : ShadowTargets)
		{
			if (!IsBeforeDeadline(Deadline))
			{
				Report.Status = TEXT("planning_budget_exceeded");
				Report.Diagnostic = TEXT("Expected projection-set sealing exhausted its caller-owned deadline.");
				return Report;
			}
		AppendToken(ExpectedPostCanonical, Target);
		AppendToken(ExpectedPostCanonical, Shadows.FindChecked(Target).Revision);
	}
		Report.ExpectedPostProjectionFingerprint =
			FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ExpectedPostCanonical);
		if (!IsBeforeDeadline(Deadline))
		{
			Report.Status = TEXT("planning_budget_exceeded");
			Report.Diagnostic = TEXT("Semantic fingerprint sealing exhausted its caller-owned deadline.");
			return Report;
		}
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		if (!IsCanonicalSha256(Report.BaseRevision)
			|| !IsCanonicalSha256(SemanticFingerprint)
			|| !IsCanonicalSha256(Report.ExpectedPostProjectionFingerprint)
			|| ProjectId.IsEmpty())
	{
		Report.Status = TEXT("semantic_identity_unavailable"); Report.Diagnostic = TEXT("Bounded semantic or project identity is unavailable."); return Report;
	}
	const auto Payload = MakeShared<FHyperAIStudioDynamicMaterialTypedPayload, ESPMode::ThreadSafe>();
	Payload->Operations = Operations; Payload->BaseRevision = Report.BaseRevision; Payload->SemanticFingerprint = SemanticFingerprint;
	if (Payload->GetBoundedByteSize() <= 0 || Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		Report.Status = TEXT("typed_payload_exceeds_shared_bound"); Report.Diagnostic = TEXT("Closed immutable DynamicMaterial DTO exceeds the shared request bound."); return Report;
	}
	const auto& Adapter = GetAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId; Binding.ToolName = TEXT("hyper_dynamic_material_apply_plan");
	Binding.VariantId = MutationVariantId; Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId; Binding.ExpectedAdapterFingerprint = Adapter.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1; Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId; Binding.Prerequisites.bPackEnabled = true; Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("plugin.DynamicMaterial"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.DynamicMaterial"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.DynamicMaterialEditor"), EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint = FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId; Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false; Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding; Contract.ArtifactTypeId = PayloadTypeId;
	Contract.ArtifactSchemaFingerprint = PayloadSchemaFingerprint(); Contract.ArtifactSemanticFingerprint = SemanticFingerprint;
	Contract.EffectTarget = TEXT("non_executable_dynamic_material_evidence:") + Report.BaseRevision;
	Contract.DeadlineMs = Request.DeadlineMs; Contract.MaxNativeOperations = 1;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs; Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256; Contract.StageLifetimeMs = StageLifetimeMs;
	Contract.bCompileOnce = false; Contract.bSaveOnce = false;
	Contract.bValidateOnce = false; Contract.bVerifyFreshOnce = false;
	FHyperAIStudioPreparedTypedArtifact Prepared; FString PrepareError;
	if (FPlatformTime::Seconds() >= Deadline)
	{
		Report.Status = TEXT("planning_budget_exceeded");
		Report.Diagnostic = TEXT("Typed planning deadline elapsed before shared Prepare could seal the artifact.");
		return Report;
	}
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		Report.Status = TEXT("typed_artifact_prepare_failed"); Report.Diagnostic = Clip(PrepareError); return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.bOk = false; Report.bStaged = false; Report.bExecutionSubmitted = false; Report.bFallbackPermitted = false;
	Report.Status = NonDryCallableState;
	Report.Diagnostic = TEXT("Prepare produced pure non-executable DTO evidence only. No authorization/effect chain is certified; the bounded async CAS/compile/save/fresh host is required.");
	return Report;
}

FHyperAIDynamicMaterialInspectReport UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_inspect(
	const FHyperAIDynamicMaterialInspectRequest& Request)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	FHyperAIDynamicMaterialInspectReport Report; Report.Scope = Clip(Request.Scope, 32);
	Report.Capabilities = FHyperAIStudioDynamicMaterialContracts::GetCapabilityMatrix();
	if (Request.Scope.Len() > 32
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioDynamicMaterialContracts::MaxPageSize
		|| Request.Cursor.Len() > 16 || Request.MaxComponents < 1
		|| Request.MaxComponents > FHyperAIStudioDynamicMaterialContracts::MaxComponents
		|| Request.MaxOutputBytes < FHyperAIStudioDynamicMaterialContracts::MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioDynamicMaterialContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds"); Report.Diagnostic = TEXT("Page/cursor/component/output bounds are invalid."); return Report;
	}
	int32 Offset = 0;
	if (!Request.Cursor.IsEmpty() && (!LexTryParseString(Offset, *Request.Cursor) || Offset < 0))
	{
		Report.Status = TEXT("invalid_cursor"); Report.Diagnostic = TEXT("Cursor must be a bounded non-negative decimal value offset."); return Report;
	}
	const double OverallDeadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.MaxGameThreadMs) / 1000.0;
	FHyperAIStudioDynamicMaterialValueSnapshot Snapshot;
	if (!FHyperAIStudioDynamicMaterialContracts::CaptureExact(Request.TargetPath, Request.TargetFamily,
		Request.Scope, Request.MaxComponents, Request.MaxGameThreadMs,
		Snapshot, Report.Status, Report.Diagnostic)) return Report;
	Report.Record = Snapshot.Record; Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete; Report.bFreshCapture = true;
	const int32 Total = Report.Record.Values.Num();
	if (Offset > Total)
	{
		Report.Status = TEXT("cursor_out_of_range"); Report.Diagnostic = TEXT("Cursor exceeds the bounded exact value result."); return Report;
	}
	const int32 Count = FMath::Min(Request.PageSize, Total - Offset);
	TArray<FHyperAIDynamicMaterialValueView> Page;
	for (int32 Index = 0; Index < Count; ++Index) Page.Add(Report.Record.Values[Offset + Index]);
	Report.Record.Values = MoveTemp(Page);
	if (Offset + Count < Total) Report.bTruncated = true;
	const int32 PageValueCountBeforeOutputBound = Report.Record.Values.Num();
	if (!EnforceInspectOutputBound(Report, Request.MaxOutputBytes, OverallDeadline)
		|| (PageValueCountBeforeOutputBound > 0 && Report.Record.Values.IsEmpty()))
	{
		Report.Status = TEXT("output_bound_too_small");
		Report.Diagnostic = TEXT("Even the bounded identity/value-page envelope cannot fit max_output_bytes.");
		return Report;
	}
	Report.NextCursor.Reset();
	if (Offset + Report.Record.Values.Num() < Total)
	{
		Report.bTruncated = true;
		Report.NextCursor = FString::FromInt(Offset + Report.Record.Values.Num());
	}
	Report.bOk = true; if (Report.Status.IsEmpty() || Report.Status == TEXT("captured")) Report.Status = TEXT("ok");
	return Report;
}

FHyperAIDynamicMaterialApplyPlanReport UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_apply_plan(
	const FHyperAIDynamicMaterialApplyPlanRequest& Request)
{
	return FHyperAIStudioDynamicMaterialContracts::BuildPlan(Request);
}

FHyperAIDynamicMaterialValidateReport UHyperAIStudioDynamicMaterialToolset::hyper_dynamic_material_validate(
	const FHyperAIDynamicMaterialValidateRequest& Request)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	FHyperAIDynamicMaterialValidateReport Report;
	Report.Capabilities = FHyperAIStudioDynamicMaterialContracts::GetCapabilityMatrix();
	if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioDynamicMaterialContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 2000
		|| Request.MaxOutputBytes < FHyperAIStudioDynamicMaterialContracts::MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioDynamicMaterialContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioDynamicMaterialContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds"); Report.Diagnostic = TEXT("Issue/deadline/output bounds or expected_revision are invalid."); return Report;
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.MaxGameThreadMs) / 1000.0;
	FHyperAIStudioDynamicMaterialValueSnapshot Snapshot;
	if (!FHyperAIStudioDynamicMaterialContracts::CaptureExact(Request.TargetPath, Request.TargetFamily,
		TEXT("loaded_only"), FHyperAIStudioDynamicMaterialContracts::MaxComponents,
		Request.MaxGameThreadMs, Snapshot, Report.Status, Report.Diagnostic)) return Report;
	Report.bFreshCapture = true; Report.Revision = Snapshot.Revision; Report.bRevisionComplete = Snapshot.bComplete;
	const int32 EffectiveIssueLimit = FMath::Clamp(
		Request.MaxOutputBytes / 16384, 1, Request.MaxIssues);
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
		AddIssue(Report.Issues, EffectiveIssueLimit, Report.bTruncated, TEXT("expected_revision_mismatch"), TEXT("error"),
			Request.TargetPath, FString(), -1, TEXT("Fresh closed public model projection differs from expected_revision."));
	bool ValidationTruncated = false;
	const int32 RemainingIssueCapacity = EffectiveIssueLimit - Report.Issues.Num();
	if (RemainingIssueCapacity > 0)
	{
		const TArray<FHyperAIDynamicMaterialIssue> Validation =
			FHyperAIStudioDynamicMaterialContracts::ValidateValueSnapshot(
				Snapshot, Request.bRequirePackageClean, RemainingIssueCapacity,
				ValidationTruncated, Deadline);
		Report.Issues.Append(Validation);
	}
	else ValidationTruncated = true;
	Report.bTruncated |= ValidationTruncated;
	for (const auto& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
	}
	Report.bOk = true; Report.bValid = Report.ErrorCount == 0 && !Report.bTruncated;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("Independent closed public DynamicMaterial root/property/slot/value/direct-stage/generated-material validation passed.")
		: TEXT("Independent validation found model, component, media bridge, CAS, compile, dirty, or bound failures.");
	return Report;
}

FString FHyperAIStudioDynamicMaterialTypedPayload::GetTypeId() const
{
	return FHyperAIStudioDynamicMaterialContracts::PayloadTypeId;
}

FString FHyperAIStudioDynamicMaterialTypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioDynamicMaterialContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioDynamicMaterialTypedPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	if (Operations.IsEmpty() || Operations.Num() > FHyperAIStudioDynamicMaterialContracts::MaxOperations
		|| BaseRevision.Len() > 71 || SemanticFingerprint.Len() > 71)
		return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	int64 Bytes = 256ll + 4ll * (BaseRevision.Len() + SemanticFingerprint.Len());
	for (const auto& Operation : Operations)
	{
		if (Operation.TargetPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxPathCharacters
			|| Operation.ComponentPath.Len() > FHyperAIStudioDynamicMaterialContracts::MaxComponentPathCharacters)
			return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
		Bytes += static_cast<int64>(CanonicalOperation(Operation).Len()) * MaxUtf8BytesPerCharacter;
		if (Bytes > FHyperAIStudioDomainLimits::MaxRequestBytes) return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	}
	return static_cast<int32>(Bytes);
}

FString FHyperAIStudioDynamicMaterialTypedPayload::GetSemanticFingerprint() const
{
	const FString Recomputed = FHyperAIStudioDynamicMaterialContracts::ComputePayloadSemanticFingerprint(
		Operations, BaseRevision);
	return Recomputed == SemanticFingerprint ? Recomputed : FString();
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioDynamicMaterialTypedPayload::CloneImmutable() const
{
	auto Clone = MakeShared<FHyperAIStudioDynamicMaterialTypedPayload, ESPMode::ThreadSafe>();
	Clone->Operations = Operations; Clone->BaseRevision = BaseRevision; Clone->SemanticFingerprint = SemanticFingerprint;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioDynamicMaterialResultPayload::GetTypeId() const
{
	return FHyperAIStudioDynamicMaterialContracts::ResultTypeId;
}

FString FHyperAIStudioDynamicMaterialResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioDynamicMaterialContracts::ResultSchemaFingerprint();
}

int32 FHyperAIStudioDynamicMaterialResultPayload::GetBoundedByteSize() const
{
	const int64 Bytes = 96ll + 4ll * (Phase.Len() + Revision.Len());
	return Bytes > FHyperAIStudioDomainLimits::MaxResultBytes
		? FHyperAIStudioDomainLimits::MaxResultBytes + 1 : static_cast<int32>(Bytes);
}


FHyperAIStudioDynamicMaterialDomainAdapter::FHyperAIStudioDynamicMaterialDomainAdapter()
	: Descriptor(FHyperAIStudioDynamicMaterialContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioDynamicMaterialDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioDynamicMaterialDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using namespace HyperAIStudio::DynamicMaterial::Private;
	if (Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Apply
		|| Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Compile
		|| Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Save
		|| Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::Validate
		|| Context.ActionKind == EHyperAIStudioDomainExecutionActionKind::VerifyFresh)
	{
		return Failure(
			EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect,
			TEXT("bounded_compile_or_runtime_cas_backend_required"),
				TEXT("This SourceCandidate adapter is deliberately zero-effect until the central bounded async CAS host is integrated."));
	}
	return Failure(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect,
		TEXT("dynamic_material_action_kind_invalid"));
}

void FHyperAIStudioDynamicMaterialRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FHyperAIStudioDynamicMaterialRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioDynamicMaterialRegistration::Shutdown()
{
	if (!bStarted) return;
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle); PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration)
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioDynamicMaterialToolset::StaticClass(),
			FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName(), Error))
			UE_LOG(LogHyperAIStudioDynamicMaterial, Error,
				TEXT("DynamicMaterial owned unregister failed closed: %s"), *Error);
		bOwnsRegistration = false;
	}
	bStarted = false;
}

bool FHyperAIStudioDynamicMaterialRegistration::IsRegistered() const
{
	return bOwnsRegistration && FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioDynamicMaterialToolset::StaticClass(),
		FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioDynamicMaterialRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsRegistration || !IsInGameThread() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()) return;
	if (!FHyperAIStudioDynamicMaterialContracts::IsRegistrationAllowed(
		FHyperAIStudioDynamicMaterialContracts::IsPendingTestRegistrationEnabled()))
	{
		UE_LOG(LogHyperAIStudioDynamicMaterial, Verbose,
			TEXT("DynamicMaterial source cohort is not admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioDynamicMaterialToolset::StaticClass(),
		FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName(), Error);
	if (!bOwnsRegistration)
		UE_LOG(LogHyperAIStudioDynamicMaterial, Error,
			TEXT("DynamicMaterial central owner registration failed closed: %s"), *Error);
}
