// Games by Hyper 2026.

#include "HyperAIStudioMaterialsGraphGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "RHIGlobals.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace HyperAIStudio::Materials::Gate
{
	namespace
	{
		const FString OutputNodeId = TEXT("$material_output");

		bool Fail(FString& OutError, const FString& Message)
		{
			OutError = Message;
			return false;
		}

		bool ParseFloat(const FString& Value, float& Out)
		{
			double Parsed = 0.0;
			if (!LexTryParseString(Parsed, *Value.TrimStartAndEnd()) || !FMath::IsFinite(Parsed))
			{
				return false;
			}
			Out = static_cast<float>(Parsed);
			return FMath::IsFinite(Out);
		}

		bool ParseInt(const FString& Value, const int32 Min, const int32 Max, int32& Out)
		{
			return LexTryParseString(Out, *Value.TrimStartAndEnd()) && Out >= Min && Out <= Max;
		}

		bool ParseBool(const FString& Value, bool& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed == TEXT("1")) { Out = true; return true; }
			if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed == TEXT("0")) { Out = false; return true; }
			return false;
		}

		/** "r,g,b" or "r,g,b,a"; alpha defaults to 1. */
		bool ParseColor(const FString& Value, FLinearColor& Out)
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() != 3 && Parts.Num() != 4)
			{
				return false;
			}
			float Channels[4] = {0.f, 0.f, 0.f, 1.f};
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				if (!ParseFloat(Parts[Index], Channels[Index])) return false;
			}
			Out = FLinearColor(Channels[0], Channels[1], Channels[2], Channels[3]);
			return true;
		}

		/** Parameter and group names: letters, digits, spaces, '_', '-' and '.'. Empty only where allowed. */
		bool ParseName(const FString& Value, const bool bAllowEmpty, FName& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				Out = NAME_None;
				return bAllowEmpty;
			}
			if (Trimmed.Len() > 128) return false;
			for (const TCHAR Char : Trimmed)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT(' ') && Char != TEXT('_') && Char != TEXT('-') && Char != TEXT('.'))
					return false;
			}
			Out = FName(*Trimmed);
			return true;
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > 64 || !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_'))) return false;
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
			}
			return true;
		}

		template <typename EnumType>
		bool ParseEnum(const FString& Value, const TArray<TPair<const TCHAR*, EnumType>>& Table, EnumType& Out)
		{
			for (const TPair<const TCHAR*, EnumType>& Row : Table)
			{
				if (Value.TrimStartAndEnd().Equals(Row.Key, ESearchCase::IgnoreCase)) { Out = Row.Value; return true; }
			}
			return false;
		}

		template <typename EnumType>
		FString EnumName(const EnumType Value, const TArray<TPair<const TCHAR*, EnumType>>& Table)
		{
			for (const TPair<const TCHAR*, EnumType>& Row : Table)
			{
				if (Row.Value == Value) return Row.Key;
			}
			return TEXT("other");
		}

		template <typename EnumType>
		FString EnumChoices(const TArray<TPair<const TCHAR*, EnumType>>& Table)
		{
			TArray<FString> Names;
			for (const TPair<const TCHAR*, EnumType>& Row : Table) Names.Add(Row.Key);
			return FString::Join(Names, TEXT("|"));
		}

		const TArray<TPair<const TCHAR*, EMaterialSamplerType>>& SamplerTypes()
		{
			static const TArray<TPair<const TCHAR*, EMaterialSamplerType>> Table = {
				{TEXT("color"), SAMPLERTYPE_Color}, {TEXT("linear_color"), SAMPLERTYPE_LinearColor},
				{TEXT("normal"), SAMPLERTYPE_Normal}, {TEXT("masks"), SAMPLERTYPE_Masks},
				{TEXT("grayscale"), SAMPLERTYPE_Grayscale}, {TEXT("linear_grayscale"), SAMPLERTYPE_LinearGrayscale},
				{TEXT("alpha"), SAMPLERTYPE_Alpha}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, EWorldPositionIncludedOffsets>>& WorldOffsets()
		{
			static const TArray<TPair<const TCHAR*, EWorldPositionIncludedOffsets>> Table = {
				{TEXT("absolute"), WPT_Default}, {TEXT("absolute_no_offsets"), WPT_ExcludeAllShaderOffsets},
				{TEXT("camera_relative"), WPT_CameraRelative}, {TEXT("camera_relative_no_offsets"), WPT_CameraRelativeNoOffsets}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, ECustomMaterialOutputType>>& CustomOutputTypes()
		{
			static const TArray<TPair<const TCHAR*, ECustomMaterialOutputType>> Table = {
				{TEXT("float1"), CMOT_Float1}, {TEXT("float2"), CMOT_Float2},
				{TEXT("float3"), CMOT_Float3}, {TEXT("float4"), CMOT_Float4}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, EBlendMode>>& BlendModes()
		{
			static const TArray<TPair<const TCHAR*, EBlendMode>> Table = {
				{TEXT("opaque"), BLEND_Opaque}, {TEXT("masked"), BLEND_Masked}, {TEXT("translucent"), BLEND_Translucent},
				{TEXT("additive"), BLEND_Additive}, {TEXT("modulate"), BLEND_Modulate},
				{TEXT("alpha_composite"), BLEND_AlphaComposite}, {TEXT("alpha_holdout"), BLEND_AlphaHoldout}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, EMaterialShadingModel>>& ShadingModels()
		{
			static const TArray<TPair<const TCHAR*, EMaterialShadingModel>> Table = {
				{TEXT("default_lit"), MSM_DefaultLit}, {TEXT("unlit"), MSM_Unlit}, {TEXT("subsurface"), MSM_Subsurface},
				{TEXT("subsurface_profile"), MSM_SubsurfaceProfile}, {TEXT("clear_coat"), MSM_ClearCoat},
				{TEXT("two_sided_foliage"), MSM_TwoSidedFoliage}, {TEXT("cloth"), MSM_Cloth},
				{TEXT("thin_translucent"), MSM_ThinTranslucent}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, EMaterialDomain>>& Domains()
		{
			static const TArray<TPair<const TCHAR*, EMaterialDomain>> Table = {
				{TEXT("surface"), MD_Surface}, {TEXT("deferred_decal"), MD_DeferredDecal},
				{TEXT("light_function"), MD_LightFunction}, {TEXT("volume"), MD_Volume},
				{TEXT("post_process"), MD_PostProcess}, {TEXT("user_interface"), MD_UI}};
			return Table;
		}

		/** Any mounted object path. Planning may load it; phases resolve what planning loaded, and load only if GC dropped it. */
		template <typename T>
		T* ResolveAsset(const FString& Path, FString& OutError)
		{
			const FString Trimmed = Path.TrimStartAndEnd();
			if (Trimmed.Len() > 512 || !Trimmed.StartsWith(TEXT("/")) || !FPackageName::IsValidObjectPath(Trimmed))
			{
				OutError = FString::Printf(TEXT("'%s' is not an object path like /Game/Folder/Asset.Asset."), *Trimmed.Left(128));
				return nullptr;
			}
			const FSoftObjectPath Soft(Trimmed);
			UObject* Object = Soft.ResolveObject();
			// ponytail: a phase that has to reload a GC'd texture can overrun its budget; pin planned assets if that shows up.
			if (!Object) Object = Soft.TryLoad();
			T* Typed = Cast<T>(Object);
			if (!Typed)
			{
				OutError = FString::Printf(TEXT("'%s' does not load as a %s."), *Trimmed.Left(128), *T::StaticClass()->GetName());
			}
			return Typed;
		}

		FString FloatText(const float Value)
		{
			return FString::SanitizeFloat(Value);
		}

		FString ColorText(const FLinearColor& Value)
		{
			return FString::Printf(TEXT("%s,%s,%s,%s"), *FloatText(Value.R), *FloatText(Value.G), *FloatText(Value.B), *FloatText(Value.A));
		}

		FString BoolText(const bool bValue)
		{
			return bValue ? TEXT("true") : TEXT("false");
		}

		FString NameText(const FName Value)
		{
			return Value.IsNone() ? FString() : Value.ToString();
		}

		UMaterial* CreateMaterialAsset(const FString& ObjectPath, FString& OutError)
		{
			UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(ObjectPath));
			UMaterial* Material = Package ? NewObject<UMaterial>(Package, FName(*FPackageName::ObjectPathToObjectName(ObjectPath)),
				RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			if (!Material)
			{
				OutError = TEXT("Could not create the material asset.");
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Material);
			Package->MarkPackageDirty();
			return Material;
		}

		UMaterialInstanceConstant* CreateInstanceAsset(const FString& ObjectPath, UMaterialInterface& Parent, FString& OutError)
		{
			UPackage* Package = CreatePackage(*FPackageName::ObjectPathToPackageName(ObjectPath));
			UMaterialInstanceConstant* Instance = Package ? NewObject<UMaterialInstanceConstant>(Package,
				FName(*FPackageName::ObjectPathToObjectName(ObjectPath)), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			if (!Instance)
			{
				OutError = TEXT("Could not create the material instance asset.");
				return nullptr;
			}
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, &Parent);
			FAssetRegistryModule::AssetCreated(Instance);
			Package->MarkPackageDirty();
			return Instance;
		}

		bool SetInstanceParameters(UMaterialInstanceConstant& Instance, const TArray<FHyperAIMaterialParameterValue>& Parameters,
			int32& OutChanged, FString& OutError)
		{
			Instance.Modify();
			for (const FHyperAIMaterialParameterValue& Parameter : Parameters)
			{
				const FName Name(*Parameter.Name.TrimStartAndEnd());
				const FHashedMaterialParameterInfo Info(Name);
				// UE 5.8's instance setters always return false, so each value is confirmed by reading it back.
				bool bSet = false;
				if (Parameter.Type == TEXT("scalar"))
				{
					float Value = 0.f;
					float Read = 0.f;
					if (ParseFloat(Parameter.Value, Value))
					{
						UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(&Instance, Name, Value);
						bSet = Instance.GetScalarParameterValue(Info, Read) && FMath::IsNearlyEqual(Read, Value);
					}
				}
				else if (Parameter.Type == TEXT("vector"))
				{
					FLinearColor Value;
					FLinearColor Read;
					if (ParseColor(Parameter.Value, Value))
					{
						UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(&Instance, Name, Value);
						bSet = Instance.GetVectorParameterValue(Info, Read) && Read.Equals(Value);
					}
				}
				else if (Parameter.Type == TEXT("texture"))
				{
					UTexture* Texture = ResolveAsset<UTexture>(Parameter.Value, OutError);
					UTexture* Read = nullptr;
					if (Texture)
					{
						UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(&Instance, Name, Texture);
						bSet = Instance.GetTextureParameterValue(Info, Read) && Read == Texture;
					}
				}
				else if (Parameter.Type == TEXT("static_switch"))
				{
					bool bValue = false;
					bool bRead = false;
					FGuid Ignored;
					if (ParseBool(Parameter.Value, bValue))
					{
						UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(
							&Instance, Name, bValue, EMaterialParameterAssociation::GlobalParameter, /*bUpdateMaterialInstance=*/false);
						bSet = Instance.GetStaticSwitchParameterValue(Info, bRead, Ignored) && bRead == bValue;
					}
				}
				if (!bSet)
				{
					return Fail(OutError, FString::Printf(TEXT("Could not set %s parameter '%s' to '%s'. %s"),
						*Parameter.Type, *Parameter.Name, *Parameter.Value.Left(128), *OutError));
				}
				++OutChanged;
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(&Instance);
			return true;
		}

		/** Legacy compound fields become properties; texture and function go first so later keys can refine them. */
		TArray<FHyperAIMaterialNodeProperty> OrderedProperties(const FHyperAIMaterialNodeSpec& Spec)
		{
			TArray<FHyperAIMaterialNodeProperty> Properties = Spec.Properties;
			if (Properties.IsEmpty())
			{
				auto Add = [&Properties](const TCHAR* Key, const FString& Value) { Properties.Add({Key, Value}); };
				if (Spec.Kind == TEXT("constant")) Add(TEXT("r"), FloatText(static_cast<float>(Spec.Scalar)));
				else if ((Spec.Kind == TEXT("scalar_parameter") || Spec.Kind == TEXT("vector_parameter")) && !Spec.Name.IsEmpty())
				{
					Add(TEXT("name"), Spec.Name);
					if (!Spec.Group.IsEmpty()) Add(TEXT("group"), Spec.Group);
					Add(TEXT("default"), Spec.Kind == TEXT("scalar_parameter")
						? FloatText(static_cast<float>(Spec.Scalar)) : ColorText(Spec.Vector));
				}
			}
			Properties.StableSort([](const FHyperAIMaterialNodeProperty& A, const FHyperAIMaterialNodeProperty& B)
			{
				auto Rank = [](const FString& Key) { return Key == TEXT("texture") || Key == TEXT("function") ? 0 : 1; };
				return Rank(A.Key) < Rank(B.Key);
			});
			return Properties;
		}

		TMap<const UClass*, FString>& KindsByClass()
		{
			static TMap<const UClass*, FString> Map;
			return Map;
		}
	}

	const TArray<FNodeKindInfo>& GetNodeKinds()
	{
		// Common building blocks of production surface, VFX and UI materials. Anything else stays readable as an
		// opaque node and can be wired or removed, but not created: add it here, with typed keys, when it is needed.
		static const TArray<FNodeKindInfo> Kinds = {
			{TEXT("constant"), TEXT("/Script/Engine.MaterialExpressionConstant"), {TEXT("r")}},
			{TEXT("constant2"), TEXT("/Script/Engine.MaterialExpressionConstant2Vector"), {TEXT("r"), TEXT("g")}},
			{TEXT("constant3"), TEXT("/Script/Engine.MaterialExpressionConstant3Vector"), {TEXT("color")}},
			{TEXT("constant4"), TEXT("/Script/Engine.MaterialExpressionConstant4Vector"), {TEXT("color")}},
			{TEXT("scalar_parameter"), TEXT("/Script/Engine.MaterialExpressionScalarParameter"),
				{TEXT("name"), TEXT("group"), TEXT("default"), TEXT("slider_min"), TEXT("slider_max"), TEXT("sort_priority")}},
			{TEXT("vector_parameter"), TEXT("/Script/Engine.MaterialExpressionVectorParameter"),
				{TEXT("name"), TEXT("group"), TEXT("default"), TEXT("sort_priority")}},
			{TEXT("static_switch_parameter"), TEXT("/Script/Engine.MaterialExpressionStaticSwitchParameter"),
				{TEXT("name"), TEXT("group"), TEXT("default")}},
			{TEXT("texture_parameter"), TEXT("/Script/Engine.MaterialExpressionTextureSampleParameter2D"),
				{TEXT("name"), TEXT("group"), TEXT("texture"), TEXT("sampler_type")}},
			{TEXT("texture_sample"), TEXT("/Script/Engine.MaterialExpressionTextureSample"), {TEXT("texture"), TEXT("sampler_type")}},
			{TEXT("texcoord"), TEXT("/Script/Engine.MaterialExpressionTextureCoordinate"), {TEXT("index"), TEXT("u_tiling"), TEXT("v_tiling")}},
			{TEXT("panner"), TEXT("/Script/Engine.MaterialExpressionPanner"), {TEXT("speed_x"), TEXT("speed_y"), TEXT("fractional")}},
			{TEXT("rotator"), TEXT("/Script/Engine.MaterialExpressionRotator"), {TEXT("center_x"), TEXT("center_y"), TEXT("speed")}},
			{TEXT("time"), TEXT("/Script/Engine.MaterialExpressionTime"), {TEXT("ignore_pause"), TEXT("period")}},
			{TEXT("add"), TEXT("/Script/Engine.MaterialExpressionAdd"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("subtract"), TEXT("/Script/Engine.MaterialExpressionSubtract"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("multiply"), TEXT("/Script/Engine.MaterialExpressionMultiply"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("divide"), TEXT("/Script/Engine.MaterialExpressionDivide"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("min"), TEXT("/Script/Engine.MaterialExpressionMin"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("max"), TEXT("/Script/Engine.MaterialExpressionMax"), {TEXT("const_a"), TEXT("const_b")}},
			{TEXT("lerp"), TEXT("/Script/Engine.MaterialExpressionLinearInterpolate"), {TEXT("const_a"), TEXT("const_b"), TEXT("const_alpha")}},
			{TEXT("clamp"), TEXT("/Script/Engine.MaterialExpressionClamp"), {TEXT("min"), TEXT("max")}},
			{TEXT("power"), TEXT("/Script/Engine.MaterialExpressionPower"), {TEXT("exponent")}},
			{TEXT("sine"), TEXT("/Script/Engine.MaterialExpressionSine"), {TEXT("period")}},
			{TEXT("cosine"), TEXT("/Script/Engine.MaterialExpressionCosine"), {TEXT("period")}},
			{TEXT("one_minus"), TEXT("/Script/Engine.MaterialExpressionOneMinus"), {}},
			{TEXT("saturate"), TEXT("/Script/Engine.MaterialExpressionSaturate"), {}},
			{TEXT("abs"), TEXT("/Script/Engine.MaterialExpressionAbs"), {}},
			{TEXT("frac"), TEXT("/Script/Engine.MaterialExpressionFrac"), {}},
			{TEXT("floor"), TEXT("/Script/Engine.MaterialExpressionFloor"), {}},
			{TEXT("ceil"), TEXT("/Script/Engine.MaterialExpressionCeil"), {}},
			{TEXT("square_root"), TEXT("/Script/Engine.MaterialExpressionSquareRoot"), {}},
			{TEXT("normalize"), TEXT("/Script/Engine.MaterialExpressionNormalize"), {}},
			{TEXT("dot"), TEXT("/Script/Engine.MaterialExpressionDotProduct"), {}},
			{TEXT("cross"), TEXT("/Script/Engine.MaterialExpressionCrossProduct"), {}},
			{TEXT("distance"), TEXT("/Script/Engine.MaterialExpressionDistance"), {}},
			{TEXT("append"), TEXT("/Script/Engine.MaterialExpressionAppendVector"), {}},
			{TEXT("component_mask"), TEXT("/Script/Engine.MaterialExpressionComponentMask"), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}},
			{TEXT("desaturation"), TEXT("/Script/Engine.MaterialExpressionDesaturation"), {}},
			{TEXT("fresnel"), TEXT("/Script/Engine.MaterialExpressionFresnel"), {TEXT("exponent"), TEXT("base_reflect_fraction")}},
			{TEXT("world_position"), TEXT("/Script/Engine.MaterialExpressionWorldPosition"), {TEXT("offsets")}},
			{TEXT("object_position"), TEXT("/Script/Engine.MaterialExpressionObjectPositionWS"), {}},
			{TEXT("vertex_color"), TEXT("/Script/Engine.MaterialExpressionVertexColor"), {}},
			{TEXT("camera_vector"), TEXT("/Script/Engine.MaterialExpressionCameraVectorWS"), {}},
			{TEXT("pixel_normal_ws"), TEXT("/Script/Engine.MaterialExpressionPixelNormalWS"), {}},
			{TEXT("function_call"), TEXT("/Script/Engine.MaterialExpressionMaterialFunctionCall"), {TEXT("function")}},
			{TEXT("custom_hlsl"), TEXT("/Script/Engine.MaterialExpressionCustom"),
				{TEXT("code"), TEXT("output_type"), TEXT("description"), TEXT("inputs"), TEXT("additional_outputs")}},
		};
		return Kinds;
	}

	const FNodeKindInfo* FindNodeKind(const FString& Kind)
	{
		return GetNodeKinds().FindByPredicate([&Kind](const FNodeKindInfo& Info) { return Kind == Info.Kind; });
	}

	UClass* ResolveKindClass(const FNodeKindInfo& Info)
	{
		UClass* Class = FindObject<UClass>(nullptr, Info.ClassPath);
		return Class && Class->IsChildOf(UMaterialExpression::StaticClass()) ? Class : nullptr;
	}

	bool ResolveAllKinds(FString& OutMissing)
	{
		TArray<FString> Missing;
		for (const FNodeKindInfo& Info : GetNodeKinds())
		{
			if (UClass* Class = ResolveKindClass(Info))
			{
				KindsByClass().Add(Class, Info.Kind);
			}
			else
			{
				Missing.Add(Info.ClassPath);
			}
		}
		OutMissing = FString::Join(Missing, TEXT(", "));
		return Missing.IsEmpty();
	}

	FString KindOf(const UMaterialExpression& Expression)
	{
		if (KindsByClass().IsEmpty())
		{
			FString Ignored;
			ResolveAllKinds(Ignored);
		}
		const FString* Kind = KindsByClass().Find(Expression.GetClass());
		return Kind ? *Kind : FString();
	}

	bool IsCustomKind(const FString& Kind)
	{
		return Kind == TEXT("custom_hlsl");
	}

	bool IsParameterKind(const FString& Kind)
	{
		return Kind == TEXT("scalar_parameter") || Kind == TEXT("vector_parameter")
			|| Kind == TEXT("static_switch_parameter") || Kind == TEXT("texture_parameter");
	}

	FString NodeIdOf(const UMaterialExpression& Expression)
	{
		const FGuid Guid = const_cast<UMaterialExpression&>(Expression).GetMaterialExpressionId();
		return Guid.IsValid() ? TEXT("guid:") + Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
	}

	TArray<FHyperAIMaterialNodeProperty> ReadProperties(const UMaterialExpression& Expression, const FString& Kind)
	{
		TArray<FHyperAIMaterialNodeProperty> Out;
		auto Add = [&Out](const TCHAR* Key, const FString& Value) { Out.Add({Key, Value}); };
		auto AddConstAB = [&Add](const float A, const float B) { Add(TEXT("const_a"), FloatText(A)); Add(TEXT("const_b"), FloatText(B)); };
		if (const auto* E = Cast<UMaterialExpressionConstant>(&Expression)) Add(TEXT("r"), FloatText(E->R));
		else if (const auto* C2 = Cast<UMaterialExpressionConstant2Vector>(&Expression)) { Add(TEXT("r"), FloatText(C2->R)); Add(TEXT("g"), FloatText(C2->G)); }
		else if (const auto* C3 = Cast<UMaterialExpressionConstant3Vector>(&Expression)) Add(TEXT("color"), ColorText(C3->Constant));
		else if (const auto* C4 = Cast<UMaterialExpressionConstant4Vector>(&Expression)) Add(TEXT("color"), ColorText(C4->Constant));
		else if (const auto* Scalar = Cast<UMaterialExpressionScalarParameter>(&Expression))
		{
			Add(TEXT("name"), NameText(Scalar->ParameterName)); Add(TEXT("group"), NameText(Scalar->Group));
			Add(TEXT("default"), FloatText(Scalar->DefaultValue)); Add(TEXT("slider_min"), FloatText(Scalar->SliderMin));
			Add(TEXT("slider_max"), FloatText(Scalar->SliderMax)); Add(TEXT("sort_priority"), FString::FromInt(Scalar->SortPriority));
		}
		else if (const auto* Vector = Cast<UMaterialExpressionVectorParameter>(&Expression))
		{
			Add(TEXT("name"), NameText(Vector->ParameterName)); Add(TEXT("group"), NameText(Vector->Group));
			Add(TEXT("default"), ColorText(Vector->DefaultValue)); Add(TEXT("sort_priority"), FString::FromInt(Vector->SortPriority));
		}
		else if (const auto* Switch = Cast<UMaterialExpressionStaticSwitchParameter>(&Expression))
		{
			Add(TEXT("name"), NameText(Switch->ParameterName)); Add(TEXT("group"), NameText(Switch->Group));
			Add(TEXT("default"), BoolText(Switch->DefaultValue != 0));
		}
		else if (const auto* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter2D>(&Expression))
		{
			Add(TEXT("name"), NameText(TextureParameter->ParameterName)); Add(TEXT("group"), NameText(TextureParameter->Group));
			Add(TEXT("texture"), TextureParameter->Texture ? TextureParameter->Texture->GetPathName() : FString());
			Add(TEXT("sampler_type"), EnumName(static_cast<EMaterialSamplerType>(TextureParameter->SamplerType), SamplerTypes()));
		}
		else if (const auto* Texture = Cast<UMaterialExpressionTextureSample>(&Expression))
		{
			Add(TEXT("texture"), Texture->Texture ? Texture->Texture->GetPathName() : FString());
			Add(TEXT("sampler_type"), EnumName(static_cast<EMaterialSamplerType>(Texture->SamplerType), SamplerTypes()));
		}
		else if (const auto* Coord = Cast<UMaterialExpressionTextureCoordinate>(&Expression))
		{
			Add(TEXT("index"), FString::FromInt(Coord->CoordinateIndex));
			Add(TEXT("u_tiling"), FloatText(Coord->UTiling)); Add(TEXT("v_tiling"), FloatText(Coord->VTiling));
		}
		else if (const auto* Panner = Cast<UMaterialExpressionPanner>(&Expression))
		{
			Add(TEXT("speed_x"), FloatText(Panner->SpeedX)); Add(TEXT("speed_y"), FloatText(Panner->SpeedY));
			Add(TEXT("fractional"), BoolText(Panner->bFractionalPart));
		}
		else if (const auto* Rotator = Cast<UMaterialExpressionRotator>(&Expression))
		{
			Add(TEXT("center_x"), FloatText(Rotator->CenterX)); Add(TEXT("center_y"), FloatText(Rotator->CenterY));
			Add(TEXT("speed"), FloatText(Rotator->Speed));
		}
		else if (const auto* Time = Cast<UMaterialExpressionTime>(&Expression))
		{
			Add(TEXT("ignore_pause"), BoolText(Time->bIgnorePause != 0));
			Add(TEXT("period"), FloatText(Time->bOverride_Period ? Time->Period : 0.f));
		}
		else if (const auto* AddNode = Cast<UMaterialExpressionAdd>(&Expression)) AddConstAB(AddNode->ConstA, AddNode->ConstB);
		else if (const auto* Sub = Cast<UMaterialExpressionSubtract>(&Expression)) AddConstAB(Sub->ConstA, Sub->ConstB);
		else if (const auto* Mul = Cast<UMaterialExpressionMultiply>(&Expression)) AddConstAB(Mul->ConstA, Mul->ConstB);
		else if (const auto* Div = Cast<UMaterialExpressionDivide>(&Expression)) AddConstAB(Div->ConstA, Div->ConstB);
		else if (const auto* Min = Cast<UMaterialExpressionMin>(&Expression)) AddConstAB(Min->ConstA, Min->ConstB);
		else if (const auto* Max = Cast<UMaterialExpressionMax>(&Expression)) AddConstAB(Max->ConstA, Max->ConstB);
		else if (const auto* Lerp = Cast<UMaterialExpressionLinearInterpolate>(&Expression))
		{
			AddConstAB(Lerp->ConstA, Lerp->ConstB); Add(TEXT("const_alpha"), FloatText(Lerp->ConstAlpha));
		}
		else if (const auto* Clamp = Cast<UMaterialExpressionClamp>(&Expression))
		{
			Add(TEXT("min"), FloatText(Clamp->MinDefault)); Add(TEXT("max"), FloatText(Clamp->MaxDefault));
		}
		else if (const auto* Power = Cast<UMaterialExpressionPower>(&Expression)) Add(TEXT("exponent"), FloatText(Power->ConstExponent));
		else if (const auto* Sine = Cast<UMaterialExpressionSine>(&Expression)) Add(TEXT("period"), FloatText(Sine->Period));
		else if (const auto* Cosine = Cast<UMaterialExpressionCosine>(&Expression)) Add(TEXT("period"), FloatText(Cosine->Period));
		else if (const auto* Mask = Cast<UMaterialExpressionComponentMask>(&Expression))
		{
			Add(TEXT("r"), BoolText(Mask->R != 0)); Add(TEXT("g"), BoolText(Mask->G != 0));
			Add(TEXT("b"), BoolText(Mask->B != 0)); Add(TEXT("a"), BoolText(Mask->A != 0));
		}
		else if (const auto* Fresnel = Cast<UMaterialExpressionFresnel>(&Expression))
		{
			Add(TEXT("exponent"), FloatText(Fresnel->Exponent));
			Add(TEXT("base_reflect_fraction"), FloatText(Fresnel->BaseReflectFraction));
		}
		else if (const auto* World = Cast<UMaterialExpressionWorldPosition>(&Expression))
		{
			Add(TEXT("offsets"), EnumName(static_cast<EWorldPositionIncludedOffsets>(World->WorldPositionShaderOffset), WorldOffsets()));
		}
		else if (const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
		{
			Add(TEXT("function"), Call->MaterialFunction ? Call->MaterialFunction->GetPathName() : FString());
		}
		else if (const auto* Custom = Cast<UMaterialExpressionCustom>(&Expression))
		{
			// The full code is sealed by hash; inspect shows a readable prefix.
			Add(TEXT("code"), Custom->Code.Left(4096));
			Add(TEXT("code_sha256"), FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Custom->Code));
			Add(TEXT("output_type"), EnumName(static_cast<ECustomMaterialOutputType>(Custom->OutputType), CustomOutputTypes()));
			Add(TEXT("description"), Custom->Description);
			TArray<FString> Inputs;
			for (const FCustomInput& Input : Custom->Inputs) Inputs.Add(NameText(Input.InputName));
			Add(TEXT("inputs"), FString::Join(Inputs, TEXT(",")));
			TArray<FString> Outputs;
			for (const FCustomOutput& Output : Custom->AdditionalOutputs)
			{
				Outputs.Add(NameText(Output.OutputName) + TEXT(":")
					+ EnumName(static_cast<ECustomMaterialOutputType>(Output.OutputType), CustomOutputTypes()));
			}
			Add(TEXT("additional_outputs"), FString::Join(Outputs, TEXT(",")));
		}
		Out.Sort([](const FHyperAIMaterialNodeProperty& A, const FHyperAIMaterialNodeProperty& B) { return A.Key < B.Key; });
		return Out;
	}

	bool WriteProperty(UMaterialExpression& Expression, const FString& Kind, const FString& Key, const FString& Value,
		const bool bNotify, FString& OutError)
	{
		const FNodeKindInfo* Info = FindNodeKind(Kind);
		if (!Info || !Info->Keys.Contains(Key))
		{
			return Fail(OutError, Info
				? FString::Printf(TEXT("%s has no key '%s'. Keys: %s."), *Kind, *Key, *FString::Join(Info->Keys, TEXT(", ")))
				: FString::Printf(TEXT("'%s' is not a node kind."), *Kind));
		}
		if (Value.Len() > 32 * 1024)
		{
			return Fail(OutError, FString::Printf(TEXT("The value for %s.%s is too long."), *Kind, *Key));
		}
		auto Commit = [&](const TCHAR* Field, TFunctionRef<void()> Assign)
		{
			FProperty* Property = FindFProperty<FProperty>(Expression.GetClass(), Field);
			if (bNotify)
			{
				Expression.Modify();
				Expression.PreEditChange(Property);
			}
			Assign();
			if (bNotify)
			{
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Expression.PostEditChangeProperty(Event);
			}
			return true;
		};
		auto Float = [&](const TCHAR* Field, float& Target)
		{
			float Parsed = 0.f;
			return ParseFloat(Value, Parsed)
				? Commit(Field, [&]() { Target = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("%s.%s must be a finite number, not '%s'."), *Kind, *Key, *Value.Left(64)));
		};
		auto Color = [&](const TCHAR* Field, FLinearColor& Target)
		{
			FLinearColor Parsed;
			return ParseColor(Value, Parsed)
				? Commit(Field, [&]() { Target = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("%s.%s must be 'r,g,b' or 'r,g,b,a'."), *Kind, *Key));
		};
		auto Bool = [&](const TCHAR* Field, TFunctionRef<void(bool)> Assign)
		{
			bool bParsed = false;
			return ParseBool(Value, bParsed)
				? Commit(Field, [&]() { Assign(bParsed); })
				: Fail(OutError, FString::Printf(TEXT("%s.%s must be true or false."), *Kind, *Key));
		};
		auto Name = [&](const TCHAR* Field, FName& Target, const bool bAllowEmpty)
		{
			FName Parsed;
			return ParseName(Value, bAllowEmpty, Parsed)
				? Commit(Field, [&]() { Target = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("%s.%s must be 1-128 letters, digits, spaces, '_', '-' or '.'."), *Kind, *Key));
		};
		auto Int = [&](const TCHAR* Field, int32& Target, const int32 Min, const int32 Max)
		{
			int32 Parsed = 0;
			return ParseInt(Value, Min, Max, Parsed)
				? Commit(Field, [&]() { Target = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("%s.%s must be a whole number from %d to %d."), *Kind, *Key, Min, Max));
		};
		auto ConstAB = [&](float& A, float& B) { return Key == TEXT("const_a") ? Float(TEXT("ConstA"), A) : Float(TEXT("ConstB"), B); };
		auto Sampler = [&](TEnumAsByte<EMaterialSamplerType>& Target)
		{
			EMaterialSamplerType Parsed = SAMPLERTYPE_Color;
			return ParseEnum(Value, SamplerTypes(), Parsed)
				? Commit(TEXT("SamplerType"), [&]() { Target = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("%s.sampler_type must be one of %s."), *Kind, *EnumChoices(SamplerTypes())));
		};
		auto TextureField = [&](UMaterialExpressionTextureBase& Node)
		{
			UTexture* Texture = ResolveAsset<UTexture>(Value, OutError);
			if (!Texture) return false;
			// Picks the sampler type the texture needs (normal, masks, sRGB...); a later sampler_type key overrides it.
			return Commit(TEXT("Texture"), [&]() { Node.Texture = Texture; Node.AutoSetSampleType(); });
		};

		if (auto* E = Cast<UMaterialExpressionConstant>(&Expression)) return Float(TEXT("R"), E->R);
		if (auto* E = Cast<UMaterialExpressionConstant2Vector>(&Expression)) return Key == TEXT("r") ? Float(TEXT("R"), E->R) : Float(TEXT("G"), E->G);
		if (auto* E = Cast<UMaterialExpressionConstant3Vector>(&Expression)) return Color(TEXT("Constant"), E->Constant);
		if (auto* E = Cast<UMaterialExpressionConstant4Vector>(&Expression)) return Color(TEXT("Constant"), E->Constant);
		if (auto* E = Cast<UMaterialExpressionScalarParameter>(&Expression))
		{
			if (Key == TEXT("name")) return Name(TEXT("ParameterName"), E->ParameterName, false);
			if (Key == TEXT("group")) return Name(TEXT("Group"), E->Group, true);
			if (Key == TEXT("default")) return Float(TEXT("DefaultValue"), E->DefaultValue);
			if (Key == TEXT("slider_min")) return Float(TEXT("SliderMin"), E->SliderMin);
			if (Key == TEXT("slider_max")) return Float(TEXT("SliderMax"), E->SliderMax);
			return Int(TEXT("SortPriority"), E->SortPriority, -1000, 1000);
		}
		if (auto* E = Cast<UMaterialExpressionVectorParameter>(&Expression))
		{
			if (Key == TEXT("name")) return Name(TEXT("ParameterName"), E->ParameterName, false);
			if (Key == TEXT("group")) return Name(TEXT("Group"), E->Group, true);
			if (Key == TEXT("default")) return Color(TEXT("DefaultValue"), E->DefaultValue);
			return Int(TEXT("SortPriority"), E->SortPriority, -1000, 1000);
		}
		if (auto* E = Cast<UMaterialExpressionStaticSwitchParameter>(&Expression))
		{
			if (Key == TEXT("name")) return Name(TEXT("ParameterName"), E->ParameterName, false);
			if (Key == TEXT("group")) return Name(TEXT("Group"), E->Group, true);
			return Bool(TEXT("DefaultValue"), [E](const bool b) { E->DefaultValue = b ? 1 : 0; });
		}
		if (auto* E = Cast<UMaterialExpressionTextureSampleParameter2D>(&Expression))
		{
			if (Key == TEXT("name")) return Name(TEXT("ParameterName"), E->ParameterName, false);
			if (Key == TEXT("group")) return Name(TEXT("Group"), E->Group, true);
			if (Key == TEXT("texture")) return TextureField(*E);
			return Sampler(E->SamplerType);
		}
		if (auto* E = Cast<UMaterialExpressionTextureSample>(&Expression))
		{
			return Key == TEXT("texture") ? TextureField(*E) : Sampler(E->SamplerType);
		}
		if (auto* E = Cast<UMaterialExpressionTextureCoordinate>(&Expression))
		{
			if (Key == TEXT("index")) return Int(TEXT("CoordinateIndex"), E->CoordinateIndex, 0, 7);
			return Key == TEXT("u_tiling") ? Float(TEXT("UTiling"), E->UTiling) : Float(TEXT("VTiling"), E->VTiling);
		}
		if (auto* E = Cast<UMaterialExpressionPanner>(&Expression))
		{
			if (Key == TEXT("fractional")) return Bool(TEXT("bFractionalPart"), [E](const bool b) { E->bFractionalPart = b; });
			return Key == TEXT("speed_x") ? Float(TEXT("SpeedX"), E->SpeedX) : Float(TEXT("SpeedY"), E->SpeedY);
		}
		if (auto* E = Cast<UMaterialExpressionRotator>(&Expression))
		{
			if (Key == TEXT("speed")) return Float(TEXT("Speed"), E->Speed);
			return Key == TEXT("center_x") ? Float(TEXT("CenterX"), E->CenterX) : Float(TEXT("CenterY"), E->CenterY);
		}
		if (auto* E = Cast<UMaterialExpressionTime>(&Expression))
		{
			if (Key == TEXT("ignore_pause")) return Bool(TEXT("bIgnorePause"), [E](const bool b) { E->bIgnorePause = b ? 1 : 0; });
			float Period = 0.f;
			if (!ParseFloat(Value, Period) || Period < 0.f)
				return Fail(OutError, TEXT("time.period must be a number >= 0; 0 means no period."));
			return Commit(TEXT("Period"), [&]() { E->Period = Period; E->bOverride_Period = Period > 0.f; });
		}
		if (auto* E = Cast<UMaterialExpressionAdd>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionSubtract>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionMultiply>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionDivide>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionMin>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionMax>(&Expression)) return ConstAB(E->ConstA, E->ConstB);
		if (auto* E = Cast<UMaterialExpressionLinearInterpolate>(&Expression))
		{
			return Key == TEXT("const_alpha") ? Float(TEXT("ConstAlpha"), E->ConstAlpha) : ConstAB(E->ConstA, E->ConstB);
		}
		if (auto* E = Cast<UMaterialExpressionClamp>(&Expression))
		{
			return Key == TEXT("min") ? Float(TEXT("MinDefault"), E->MinDefault) : Float(TEXT("MaxDefault"), E->MaxDefault);
		}
		if (auto* E = Cast<UMaterialExpressionPower>(&Expression)) return Float(TEXT("ConstExponent"), E->ConstExponent);
		if (auto* E = Cast<UMaterialExpressionSine>(&Expression)) return Float(TEXT("Period"), E->Period);
		if (auto* E = Cast<UMaterialExpressionCosine>(&Expression)) return Float(TEXT("Period"), E->Period);
		if (auto* E = Cast<UMaterialExpressionComponentMask>(&Expression))
		{
			if (Key == TEXT("r")) return Bool(TEXT("R"), [E](const bool b) { E->R = b ? 1 : 0; });
			if (Key == TEXT("g")) return Bool(TEXT("G"), [E](const bool b) { E->G = b ? 1 : 0; });
			if (Key == TEXT("b")) return Bool(TEXT("B"), [E](const bool b) { E->B = b ? 1 : 0; });
			return Bool(TEXT("A"), [E](const bool b) { E->A = b ? 1 : 0; });
		}
		if (auto* E = Cast<UMaterialExpressionFresnel>(&Expression))
		{
			return Key == TEXT("exponent") ? Float(TEXT("Exponent"), E->Exponent) : Float(TEXT("BaseReflectFraction"), E->BaseReflectFraction);
		}
		if (auto* E = Cast<UMaterialExpressionWorldPosition>(&Expression))
		{
			EWorldPositionIncludedOffsets Parsed = WPT_Default;
			return ParseEnum(Value, WorldOffsets(), Parsed)
				? Commit(TEXT("WorldPositionShaderOffset"), [&]() { E->WorldPositionShaderOffset = Parsed; })
				: Fail(OutError, FString::Printf(TEXT("world_position.offsets must be one of %s."), *EnumChoices(WorldOffsets())));
		}
		if (auto* E = Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
		{
			UMaterialFunctionInterface* Function = ResolveAsset<UMaterialFunctionInterface>(Value, OutError);
			if (!Function) return false;
			// SetMaterialFunction builds the pins, which needs the owning material; a shadow node only records the function.
			return Commit(TEXT("MaterialFunction"), [&]() { if (bNotify) E->SetMaterialFunction(Function); else E->MaterialFunction = Function; });
		}
		if (auto* E = Cast<UMaterialExpressionCustom>(&Expression))
		{
			auto Rebuilt = [&](const TCHAR* Field, TFunctionRef<void()> Assign)
			{
				return Commit(Field, [&]() { Assign(); E->RebuildOutputs(); });
			};
			if (Key == TEXT("code")) return Commit(TEXT("Code"), [&]() { E->Code = Value; });
			if (Key == TEXT("description"))
			{
				return Value.Len() <= 128 ? Commit(TEXT("Description"), [&]() { E->Description = Value; })
					: Fail(OutError, TEXT("custom_hlsl.description is at most 128 characters."));
			}
			if (Key == TEXT("output_type"))
			{
				ECustomMaterialOutputType Parsed = CMOT_Float1;
				return ParseEnum(Value, CustomOutputTypes(), Parsed)
					? Rebuilt(TEXT("OutputType"), [&]() { E->OutputType = Parsed; })
					: Fail(OutError, TEXT("custom_hlsl.output_type must be float1, float2, float3 or float4."));
			}
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (Key == TEXT("inputs"))
			{
				TArray<FCustomInput> Inputs;
				TSet<FString> Seen;
				for (FString& Part : Parts)
				{
					Part.TrimStartAndEndInline();
					bool bDuplicate = false;
					Seen.Add(Part, &bDuplicate);
					if (!IsIdentifier(Part) || bDuplicate || Parts.Num() > 8)
						return Fail(OutError, TEXT("custom_hlsl.inputs is up to 8 unique HLSL identifiers, comma separated."));
					FCustomInput& Input = Inputs.AddDefaulted_GetRef();
					Input.InputName = FName(*Part);
				}
				return Rebuilt(TEXT("Inputs"), [&]() { E->Inputs = Inputs; });
			}
			TArray<FCustomOutput> Outputs;
			for (FString& Part : Parts)
			{
				FString OutputName;
				FString TypeName;
				ECustomMaterialOutputType Type = CMOT_Float1;
				if (!Part.TrimStartAndEnd().Split(TEXT(":"), &OutputName, &TypeName) || !IsIdentifier(OutputName.TrimStartAndEnd())
					|| !ParseEnum(TypeName, CustomOutputTypes(), Type) || Parts.Num() > 4)
					return Fail(OutError, TEXT("custom_hlsl.additional_outputs is up to 4 'Name:float3' entries, comma separated."));
				FCustomOutput& Output = Outputs.AddDefaulted_GetRef();
				Output.OutputName = FName(*OutputName.TrimStartAndEnd());
				Output.OutputType = Type;
			}
			return Rebuilt(TEXT("AdditionalOutputs"), [&]() { E->AdditionalOutputs = Outputs; });
		}
		return Fail(OutError, FString::Printf(TEXT("The node is not a %s."), *Kind));
	}

	bool ValidateMaterialSetting(const FString& Key, const FString& Value, FString& OutError)
	{
		bool bParsed = false;
		EBlendMode Blend = BLEND_Opaque;
		EMaterialShadingModel Model = MSM_DefaultLit;
		EMaterialDomain Domain = MD_Surface;
		if (Key == TEXT("blend_mode") && ParseEnum(Value, BlendModes(), Blend)) return true;
		if (Key == TEXT("shading_model") && ParseEnum(Value, ShadingModels(), Model)) return true;
		if (Key == TEXT("domain") && ParseEnum(Value, Domains(), Domain)) return true;
		if (Key == TEXT("two_sided") && ParseBool(Value, bParsed)) return true;
		return Fail(OutError, FString::Printf(
			TEXT("Material setting '%s'='%s' is not valid. blend_mode: %s; shading_model: %s; domain: %s; two_sided: true|false."),
			*Key.Left(64), *Value.Left(64), *EnumChoices(BlendModes()), *EnumChoices(ShadingModels()), *EnumChoices(Domains())));
	}

	bool WriteMaterialSetting(UMaterial& Material, const FString& Key, const FString& Value, FString& OutError)
	{
		if (!ValidateMaterialSetting(Key, Value, OutError)) return false;
		const TCHAR* Field = Key == TEXT("blend_mode") ? TEXT("BlendMode") : Key == TEXT("shading_model") ? TEXT("ShadingModel")
			: Key == TEXT("domain") ? TEXT("MaterialDomain") : TEXT("TwoSided");
		FProperty* Property = FindFProperty<FProperty>(UMaterial::StaticClass(), Field);
		Material.Modify();
		Material.PreEditChange(Property);
		if (Key == TEXT("blend_mode")) { EBlendMode Parsed = BLEND_Opaque; ParseEnum(Value, BlendModes(), Parsed); Material.BlendMode = Parsed; }
		else if (Key == TEXT("shading_model")) { EMaterialShadingModel Parsed = MSM_DefaultLit; ParseEnum(Value, ShadingModels(), Parsed); Material.SetShadingModel(Parsed); }
		else if (Key == TEXT("domain")) { EMaterialDomain Parsed = MD_Surface; ParseEnum(Value, Domains(), Parsed); Material.MaterialDomain = Parsed; }
		else { bool bParsed = false; ParseBool(Value, bParsed); Material.TwoSided = bParsed ? 1 : 0; }
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Material.PostEditChangeProperty(Event);
		return true;
	}

	EMaterialProperty OutputPropertyFromName(const FString& Name)
	{
		static const TMap<FString, EMaterialProperty> Properties = {
			{TEXT("base_color"), MP_BaseColor}, {TEXT("metallic"), MP_Metallic}, {TEXT("specular"), MP_Specular},
			{TEXT("roughness"), MP_Roughness}, {TEXT("emissive"), MP_EmissiveColor}, {TEXT("opacity"), MP_Opacity},
			{TEXT("opacity_mask"), MP_OpacityMask}, {TEXT("normal"), MP_Normal}, {TEXT("ao"), MP_AmbientOcclusion},
			{TEXT("world_position_offset"), MP_WorldPositionOffset}, {TEXT("subsurface_color"), MP_SubsurfaceColor},
			{TEXT("refraction"), MP_Refraction}, {TEXT("pixel_depth_offset"), MP_PixelDepthOffset},
			{TEXT("anisotropy"), MP_Anisotropy}, {TEXT("tangent"), MP_Tangent}};
		const EMaterialProperty* Found = Properties.Find(Name);
		return Found ? *Found : MP_MAX;
	}

	UMaterialExpression* MakeShadowNode(const FHyperAIMaterialNodeSpec& Spec, FString& OutError)
	{
		const FNodeKindInfo* Info = FindNodeKind(Spec.Kind);
		UClass* Class = Info ? ResolveKindClass(*Info) : nullptr;
		if (!Class)
		{
			OutError = FString::Printf(TEXT("'%s' is not a node kind."), *Spec.Kind.Left(64));
			return nullptr;
		}
		UMaterialExpression* Shadow = NewObject<UMaterialExpression>(GetTransientPackage(), Class, NAME_None, RF_Transient);
		for (const FHyperAIMaterialNodeProperty& Property : OrderedProperties(Spec))
		{
			if (!WriteProperty(*Shadow, Spec.Kind, Property.Key, Property.Value, /*bNotify=*/false, OutError))
			{
				return nullptr;
			}
		}
		return Shadow;
	}

	bool HasInput(UMaterialExpression& Expression, const FString& Name)
	{
		const int32 Count = Expression.CountInputs();
		if (Name.IsEmpty())
		{
			return Count > 0;
		}
		const FName Wanted(*Name);
		const bool bFunctionCall = Expression.IsA<UMaterialExpressionMaterialFunctionCall>();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FString Candidate = Expression.GetInputName(Index).ToString();
			if (bFunctionCall)
			{
				// Function-call pins read "Name (V3)"; the library matches the bare name.
				Candidate.Split(TEXT(" ("), &Candidate, nullptr);
			}
			if (FName(*Candidate) == Wanted
				|| UMaterialGraphNode::GetShortenPinName(Expression.GetInputName(Index)) == Wanted)
			{
				return true;
			}
		}
		return false;
	}

	bool HasOutput(UMaterialExpression& Expression, const FString& Name)
	{
		const TArray<FExpressionOutput>& Outputs = Expression.GetOutputs();
		if (Name.IsEmpty())
		{
			return !Outputs.IsEmpty();
		}
		const FName Wanted(*Name);
		for (const FExpressionOutput& Output : Outputs)
		{
			if (!Output.OutputName.IsNone() ? Output.OutputName == Wanted
				: (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA && Wanted == TEXT("R"))
				|| (!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA && Wanted == TEXT("G"))
				|| (!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA && Wanted == TEXT("B"))
				|| (!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA && Wanted == TEXT("A")))
			{
				return true;
			}
		}
		return false;
	}

	bool GetFunctionPins(const FString& FunctionPath, TArray<FString>& OutInputs, TArray<FString>& OutOutputs, FString& OutError)
	{
		UMaterialFunctionInterface* Function = ResolveAsset<UMaterialFunctionInterface>(FunctionPath, OutError);
		if (!Function) return false;
		TArray<FFunctionExpressionInput> Inputs;
		TArray<FFunctionExpressionOutput> Outputs;
		Function->GetInputsAndOutputs(Inputs, Outputs);
		for (const FFunctionExpressionInput& Input : Inputs)
		{
			if (Input.ExpressionInput) OutInputs.Add(Input.ExpressionInput->InputName.ToString());
		}
		for (const FFunctionExpressionOutput& Output : Outputs)
		{
			if (Output.ExpressionOutput) OutOutputs.Add(Output.ExpressionOutput->OutputName.ToString());
		}
		return true;
	}

	TMap<FName, FString> GetParentParameters(UMaterialInterface& Parent)
	{
		TMap<FName, FString> Out;
		TArray<FName> Names;
		auto Collect = [&](void (*Getter)(UMaterialInterface*, TArray<FName>&), const TCHAR* Type)
		{
			Names.Reset();
			Getter(&Parent, Names);
			for (const FName Name : Names) Out.Add(Name, Type);
		};
		Collect(&UMaterialEditingLibrary::GetScalarParameterNames, TEXT("scalar"));
		Collect(&UMaterialEditingLibrary::GetVectorParameterNames, TEXT("vector"));
		Collect(&UMaterialEditingLibrary::GetTextureParameterNames, TEXT("texture"));
		Collect(&UMaterialEditingLibrary::GetStaticSwitchParameterNames, TEXT("static_switch"));
		return Out;
	}

	bool CheckParameterValue(const FHyperAIMaterialParameterValue& Parameter, FString& OutError)
	{
		float Scalar = 0.f;
		FLinearColor Color;
		bool bSwitch = false;
		if (Parameter.Type == TEXT("scalar") && ParseFloat(Parameter.Value, Scalar)) return true;
		if (Parameter.Type == TEXT("vector") && ParseColor(Parameter.Value, Color)) return true;
		if (Parameter.Type == TEXT("static_switch") && ParseBool(Parameter.Value, bSwitch)) return true;
		if (Parameter.Type == TEXT("texture")) return ResolveAsset<UTexture>(Parameter.Value, OutError) != nullptr;
		return Fail(OutError, FString::Printf(TEXT("Parameter '%s' of type '%s' has an invalid value '%s'. scalar: number; vector: r,g,b[,a]; texture: object path; static_switch: true|false."),
			*Parameter.Name.Left(64), *Parameter.Type.Left(32), *Parameter.Value.Left(64)));
	}

	bool ApplyOperation(const FHyperAIStudioMaterialBackendOperation& Operation, int32& OutChanged, FString& OutError)
	{
		using EKind = EHyperAIStudioMaterialOperationKind;
		if (Operation.Kind == EKind::RepairSemanticGraph)
		{
			return Fail(OutError, TEXT("repair_semantic_graph is dry-run evidence only; disconnect or remove nodes with edit_graph."));
		}
		if (Operation.Kind == EKind::CreateMaterialInstance || Operation.Kind == EKind::SetInstanceParameters)
		{
			UMaterialInstanceConstant* Instance = nullptr;
			if (Operation.Kind == EKind::CreateMaterialInstance)
			{
				UMaterialInterface* Parent = ResolveAsset<UMaterialInterface>(Operation.ParentPath, OutError);
				Instance = Parent ? CreateInstanceAsset(Operation.TargetPath, *Parent, OutError) : nullptr;
				if (!Instance) return false;
				++OutChanged;
			}
			else
			{
				Instance = Cast<UMaterialInstanceConstant>(FSoftObjectPath(Operation.TargetPath).ResolveObject());
				if (!Instance) return Fail(OutError, TEXT("The material instance is no longer loaded."));
			}
			return SetInstanceParameters(*Instance, Operation.Parameters, OutChanged, OutError);
		}

		const bool bCreate = Operation.Kind != EKind::EditGraph;
		UMaterial* Material = bCreate
			? CreateMaterialAsset(Operation.TargetPath, OutError)
			: Cast<UMaterial>(FSoftObjectPath(Operation.TargetPath).ResolveObject());
		if (!Material)
		{
			return Fail(OutError, OutError.IsEmpty() ? FString(TEXT("The material is no longer loaded.")) : OutError);
		}
		if (bCreate) ++OutChanged;
		Material->Modify();

		TMap<FString, UMaterialExpression*> Nodes;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression) Nodes.Add(NodeIdOf(*Expression), Expression);
		}
		auto Find = [&Nodes](const FString& Id) -> UMaterialExpression*
		{
			UMaterialExpression* const* Found = Nodes.Find(Id);
			return Found ? *Found : nullptr;
		};

		for (const FString& Id : Operation.RemoveNodeIds)
		{
			UMaterialExpression* Expression = Find(Id);
			if (!Expression) return Fail(OutError, FString::Printf(TEXT("Node %s to remove is gone."), *Id));
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			Nodes.Remove(Id);
			++OutChanged;
		}
		for (const FHyperAIMaterialEdgeSpec& Cut : Operation.Disconnects)
		{
			const bool bCut = Cut.ToNodeId == OutputNodeId
				? UMaterialEditingLibrary::DisconnectMaterialProperty(Material, OutputPropertyFromName(Cut.ToInput))
				: (Find(Cut.ToNodeId) && UMaterialEditingLibrary::DisconnectMaterialExpressions(Find(Cut.ToNodeId), Cut.ToInput));
			if (!bCut) return Fail(OutError, FString::Printf(TEXT("Could not disconnect %s.%s; it is not connected."), *Cut.ToNodeId, *Cut.ToInput));
			++OutChanged;
		}
		bool bAllUnplaced = bCreate;
		for (const FHyperAIMaterialNodeSpec& Spec : Operation.Nodes)
		{
			const FNodeKindInfo* Info = FindNodeKind(Spec.Kind);
			UClass* Class = Info ? ResolveKindClass(*Info) : nullptr;
			UMaterialExpression* Expression = Class
				? UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, Spec.EditorX, Spec.EditorY) : nullptr;
			if (!Expression) return Fail(OutError, FString::Printf(TEXT("Could not create %s node %s."), *Spec.Kind, *Spec.NodeId));
			++OutChanged;
			Nodes.Add(Spec.NodeId, Expression);
			bAllUnplaced &= Spec.EditorX == 0 && Spec.EditorY == 0;
			for (const FHyperAIMaterialNodeProperty& Property : OrderedProperties(Spec))
			{
				if (!WriteProperty(*Expression, Spec.Kind, Property.Key, Property.Value, /*bNotify=*/true, OutError)) return false;
			}
		}
		for (const FHyperAIMaterialPropertyEdit& Edit : Operation.PropertyEdits)
		{
			UMaterialExpression* Expression = Find(Edit.NodeId);
			if (!Expression || !WriteProperty(*Expression, KindOf(*Expression), Edit.Key, Edit.Value, /*bNotify=*/true, OutError))
			{
				return Fail(OutError, OutError.IsEmpty() ? FString::Printf(TEXT("Node %s is gone."), *Edit.NodeId) : OutError);
			}
			++OutChanged;
		}
		for (const FHyperAIMaterialEdgeSpec& Edge : Operation.Edges)
		{
			if (!UMaterialEditingLibrary::ConnectMaterialExpressions(Find(Edge.FromNodeId), Edge.FromOutput, Find(Edge.ToNodeId), Edge.ToInput))
			{
				return Fail(OutError, FString::Printf(TEXT("Could not connect %s.%s -> %s.%s."),
					*Edge.FromNodeId, *Edge.FromOutput, *Edge.ToNodeId, *Edge.ToInput));
			}
			++OutChanged;
		}
		for (const FHyperAIMaterialOutputSpec& Output : Operation.Outputs)
		{
			if (!UMaterialEditingLibrary::ConnectMaterialProperty(Find(Output.FromNodeId), Output.FromOutput, OutputPropertyFromName(Output.Property)))
			{
				return Fail(OutError, FString::Printf(TEXT("Could not connect %s.%s to %s."), *Output.FromNodeId, *Output.FromOutput, *Output.Property));
			}
			++OutChanged;
		}
		for (const FHyperAIMaterialNodeProperty& Setting : Operation.MaterialSettings)
		{
			if (!WriteMaterialSetting(*Material, Setting.Key, Setting.Value, OutError)) return false;
			++OutChanged;
		}
		if (bAllUnplaced && !Operation.Nodes.IsEmpty())
		{
			UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
		}
		// Parameters are listed from cached data; refresh it now so an instance made later in this plan can set them.
		Material->UpdateCachedExpressionData();
		return true;
	}

	FString ComputeContentKey(const UObject& Asset)
	{
		const UPackage* Package = Asset.GetOutermost();
		FString Canonical = TEXT("hyperai.material.content-key.v1\n") + Asset.GetPathName() + TEXT("\n");
		Canonical += Package ? LexToString(Package->GetSavedHash()) : FString();
		Canonical += Package && Package->IsDirty() ? TEXT("\ndirty") : TEXT("\nclean");
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool IsOpenInEditor(const UObject& Asset)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		return Editors && Editors->FindEditorForAsset(const_cast<UObject*>(&Asset), /*bFocusIfOpen=*/false) != nullptr;
	}

	TArray<FString> FindCustomNodes(const UMaterial& Material)
	{
		TArray<FString> Out;
		for (const UMaterialExpression* Expression : const_cast<UMaterial&>(Material).GetExpressions())
		{
			if (Expression && Expression->IsA<UMaterialExpressionCustom>()) Out.Add(NodeIdOf(*Expression));
		}
		return Out;
	}

	bool IsExpressibleWithNodes(const FString& Code, const TArray<FString>& InputNames)
	{
		FString Body = Code.TrimStartAndEnd();
		if (!Body.StartsWith(TEXT("return")) || Body.Contains(TEXT("//")) || Body.Contains(TEXT("/*")))
		{
			return false;
		}
		Body = Body.Mid(6).TrimStartAndEnd();
		if (!Body.EndsWith(TEXT(";")))
		{
			return false;
		}
		Body.LeftChopInline(1);
		if (Body.Contains(TEXT(";")) || Body.Contains(TEXT("{")) || Body.Contains(TEXT("[")) || Body.Contains(TEXT("?")))
		{
			return false;
		}
		static const TSet<FString> NodeIntrinsics = {
			TEXT("lerp"), TEXT("saturate"), TEXT("clamp"), TEXT("pow"), TEXT("abs"), TEXT("frac"), TEXT("sin"), TEXT("cos"),
			TEXT("dot"), TEXT("normalize"), TEXT("min"), TEXT("max"), TEXT("sqrt"), TEXT("floor"), TEXT("ceil"), TEXT("cross"),
			TEXT("length"), TEXT("distance"), TEXT("float"), TEXT("float2"), TEXT("float3"), TEXT("float4")};
		for (int32 Index = 0; Index < Body.Len();)
		{
			if (!(FChar::IsAlpha(Body[Index]) || Body[Index] == TEXT('_')))
			{
				++Index;
				continue;
			}
			const int32 Start = Index;
			while (Index < Body.Len() && (FChar::IsAlnum(Body[Index]) || Body[Index] == TEXT('_'))) ++Index;
			const FString Token = Body.Mid(Start, Index - Start);
			const bool bSwizzle = Start > 0 && Body[Start - 1] == TEXT('.');
			if (!bSwizzle && !NodeIntrinsics.Contains(Token) && !InputNames.Contains(Token))
			{
				// Anything else (texture sampling, loops, View/Primitive data, helper functions) justifies Custom.
				return false;
			}
		}
		return true;
	}

	bool IsCompileFinished(const UMaterialInterface& Material)
	{
		return !Material.IsCompiling();
	}

	FHyperAIMaterialCompileStats ReadStats(UMaterialInterface& Material)
	{
		FHyperAIMaterialCompileStats Stats;
		if (!IsCompileFinished(Material))
		{
			Stats.Status = TEXT("compiling");
			return Stats;
		}
		if (UMaterial* Base = Material.GetMaterial())
		{
			if (const FMaterialResource* Resource = Base->GetMaterialResource(GMaxRHIShaderPlatform))
			{
				for (const FString& Error : Resource->GetCompileErrors())
				{
					if (Stats.CompileErrors.Num() >= 16) break;
					Stats.CompileErrors.Add(Error.Left(512));
				}
			}
		}
		// Safe now: nothing is compiling, so GetStatistics does not wait.
		const FMaterialStatistics Measured = UMaterialEditingLibrary::GetStatistics(&Material);
		Stats.bAvailable = true;
		Stats.Status = Stats.CompileErrors.IsEmpty() ? TEXT("compiled") : TEXT("compile_errors");
		Stats.NumPixelShaderInstructions = Measured.NumPixelShaderInstructions;
		Stats.NumVertexShaderInstructions = Measured.NumVertexShaderInstructions;
		Stats.NumSamplers = Measured.NumSamplers;
		Stats.NumPixelTextureSamples = Measured.NumPixelTextureSamples;
		Stats.NumVertexTextureSamples = Measured.NumVertexTextureSamples;
		Stats.NumVirtualTextureSamples = Measured.NumVirtualTextureSamples;
		Stats.NumUVScalars = Measured.NumUVScalars;
		Stats.NumInterpolatorScalars = Measured.NumInterpolatorScalars;
		return Stats;
	}
}
