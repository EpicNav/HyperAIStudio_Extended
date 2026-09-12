// Games by Hyper 2026.

#include "HyperAIStudioGASToolset.h"

// Every direct GameplayAbilities dependency is isolated in this LoadingPhase=None editor module.

#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "CoreGlobals.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "GameplayTagsManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioGASToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGAS, Log, All);

namespace HyperAIStudio::GAS::Private
{
	constexpr int32 MaxTextCharacters = 512;
	constexpr int32 MinOutputBytes = 32768;
	constexpr int32 MaxUtf8BytesPerCharacter = 4;
	constexpr double MaxAbsoluteMagnitude = 1000000000.0;

	struct FCaptureBudget
	{
		int32 Elements = 0;
		int32 Bytes = 0;
		double AbsoluteDeadlineSeconds = 0.0;

		bool Reserve(const int32 AddedElements, const int64 AddedBytes)
		{
			if (AddedElements < 0 || AddedBytes < 0 || AddedBytes > MAX_int32
				|| Elements > FHyperAIStudioGASContracts::MaxProjectionElements - AddedElements
				|| Bytes > FHyperAIStudioGASContracts::MaxProjectionBytes
					- static_cast<int32>(AddedBytes)
				|| FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
			{
				return false;
			}
			Elements += AddedElements;
			Bytes += static_cast<int32>(AddedBytes);
			return true;
		}
	};

	FString Clip(const FString& Value, const int32 MaxCharacters = MaxTextCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	bool HasEmbeddedNull(const FString& Value)
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

	void AppendToken(FString& Buffer, const FString& Value)
	{
		Buffer += FString::FromInt(Value.Len());
		Buffer += TEXT(":");
		Buffer += Value;
		Buffer += TEXT(";");
	}

	void AppendBool(FString& Buffer, const bool Value)
	{
		AppendToken(Buffer, Value ? TEXT("1") : TEXT("0"));
	}

	void AppendInt(FString& Buffer, const int64 Value)
	{
		AppendToken(Buffer, LexToString(Value));
	}

	void AppendDouble(FString& Buffer, const double Value)
	{
		AppendToken(Buffer, FString::Printf(TEXT("%.17g"), Value));
	}

	void AddIssue(
		TArray<FHyperAIGASIssue>& Issues,
		const int32 Maximum,
		bool& bOutTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
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
		FHyperAIGASIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.AssetPath = Clip(AssetPath, FHyperAIStudioGASContracts::MaxPathCharacters);
		Issue.StableId = Clip(StableId, FHyperAIStudioGASContracts::MaxPathCharacters);
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message);
	}

	bool HasError(const TArray<FHyperAIGASIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIGASIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	TArray<FString> TagStrings(
		const FGameplayTagContainer& Container,
		const int32 Maximum,
		FCaptureBudget& Budget,
		bool& bInOutComplete)
	{
		TArray<FString> Result;
		const TArray<FGameplayTag>& Tags = Container.GetGameplayTagArray();
		const int32 Count = FMath::Min(Tags.Num(), Maximum);
		Result.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const uint32 NameLength = Tags[Index].GetTagName().GetStringLength();
			if (NameLength > static_cast<uint32>(FHyperAIStudioGASContracts::MaxTagCharacters)
				|| !Budget.Reserve(1, 96ll
					+ static_cast<int64>(NameLength + 1) * sizeof(TCHAR)))
			{
				bInOutComplete = false;
				break;
			}
			const FString Value = Tags[Index].ToString();
			if (Value.Len() > FHyperAIStudioGASContracts::MaxTagCharacters
				|| HasEmbeddedNull(Value))
			{
				bInOutComplete = false;
				Result.Add(Value.Left(FHyperAIStudioGASContracts::MaxTagCharacters));
			}
			else
			{
				Result.Add(Value);
			}
		}
		if (Tags.Num() > Count) bInOutComplete = false;
		Result.Sort();
		return Result;
	}

	void AppendStrings(FString& Canonical, TArray<FString> Values)
	{
		Values.Sort();
		AppendInt(Canonical, Values.Num());
		for (const FString& Value : Values)
		{
			AppendToken(Canonical, Value);
		}
	}

	FString BlueprintStatusToken(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_BeingCreated: return TEXT("being_created");
		case BS_Dirty: return TEXT("dirty_compile_required");
		case BS_Error: return TEXT("compile_error");
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_Unknown: return TEXT("unknown");
		default: return TEXT("unknown");
		}
	}

	FString DurationPolicyToken(const EGameplayEffectDurationType Policy)
	{
		switch (Policy)
		{
		case EGameplayEffectDurationType::Instant: return TEXT("instant");
		case EGameplayEffectDurationType::Infinite: return TEXT("infinite");
		case EGameplayEffectDurationType::HasDuration: return TEXT("duration");
		default: return TEXT("unknown");
		}
	}

	bool ParseDurationPolicy(const FString& Token, EGameplayEffectDurationType& Out)
	{
		if (Token == TEXT("instant")) { Out = EGameplayEffectDurationType::Instant; return true; }
		if (Token == TEXT("infinite")) { Out = EGameplayEffectDurationType::Infinite; return true; }
		if (Token == TEXT("duration")) { Out = EGameplayEffectDurationType::HasDuration; return true; }
		return false;
	}

	FString ModifierOperationToken(const EGameplayModOp::Type Operation)
	{
		switch (Operation)
		{
		case EGameplayModOp::Additive: return TEXT("additive");
		case EGameplayModOp::Multiplicitive: return TEXT("multiplicative");
		case EGameplayModOp::Division: return TEXT("division");
		case EGameplayModOp::Override: return TEXT("override");
		case EGameplayModOp::MultiplyCompound: return TEXT("multiply_compound");
		case EGameplayModOp::AddFinal: return TEXT("add_final");
		default: return TEXT("unknown");
		}
	}

	bool ParseModifierOperation(const FString& Token, EGameplayModOp::Type& Out)
	{
		if (Token == TEXT("additive")) { Out = EGameplayModOp::Additive; return true; }
		if (Token == TEXT("multiplicative")) { Out = EGameplayModOp::Multiplicitive; return true; }
		if (Token == TEXT("division")) { Out = EGameplayModOp::Division; return true; }
		if (Token == TEXT("override")) { Out = EGameplayModOp::Override; return true; }
		return false;
	}

	FString MagnitudeKindToken(const EGameplayEffectMagnitudeCalculation Kind)
	{
		switch (Kind)
		{
		case EGameplayEffectMagnitudeCalculation::ScalableFloat: return TEXT("scalable_float");
		case EGameplayEffectMagnitudeCalculation::AttributeBased: return TEXT("attribute_based");
		case EGameplayEffectMagnitudeCalculation::CustomCalculationClass: return TEXT("custom_calculation");
		case EGameplayEffectMagnitudeCalculation::SetByCaller: return TEXT("set_by_caller");
		default: return TEXT("unknown");
		}
	}

	FString InstancingPolicyToken(const EGameplayAbilityInstancingPolicy::Type Value)
	{
		switch (Value)
		{
		case EGameplayAbilityInstancingPolicy::NonInstanced: return TEXT("non_instanced");
		case EGameplayAbilityInstancingPolicy::InstancedPerActor: return TEXT("per_actor");
		case EGameplayAbilityInstancingPolicy::InstancedPerExecution: return TEXT("per_execution");
		default: return TEXT("unknown");
		}
	}

	FString ReplicationPolicyToken(const EGameplayAbilityReplicationPolicy::Type Value)
	{
		return Value == EGameplayAbilityReplicationPolicy::ReplicateYes
			? TEXT("replicate") : TEXT("do_not_replicate");
	}

	FString NetExecutionPolicyToken(const EGameplayAbilityNetExecutionPolicy::Type Value)
	{
		switch (Value)
		{
		case EGameplayAbilityNetExecutionPolicy::LocalPredicted: return TEXT("local_predicted");
		case EGameplayAbilityNetExecutionPolicy::LocalOnly: return TEXT("local_only");
		case EGameplayAbilityNetExecutionPolicy::ServerInitiated: return TEXT("server_initiated");
		case EGameplayAbilityNetExecutionPolicy::ServerOnly: return TEXT("server_only");
		default: return TEXT("unknown");
		}
	}

	FString NetSecurityPolicyToken(const EGameplayAbilityNetSecurityPolicy::Type Value)
	{
		switch (Value)
		{
		case EGameplayAbilityNetSecurityPolicy::ClientOrServer: return TEXT("client_or_server");
		case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution: return TEXT("server_only_execution");
		case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination: return TEXT("server_only_termination");
		case EGameplayAbilityNetSecurityPolicy::ServerOnly: return TEXT("server_only");
		default: return TEXT("unknown");
		}
	}

	FString AttributeOwnerPath(const FGameplayAttribute& Attribute)
	{
		return Attribute.IsValid() && Attribute.GetAttributeSetClass()
			? Attribute.GetAttributeSetClass()->GetPathName() : FString();
	}

	bool IsSupportedVariantFilter(const FString& Variant)
	{
		return Variant == TEXT("all") || Variant == TEXT("gameplay_ability")
			|| Variant == TEXT("gameplay_effect") || Variant == TEXT("attribute_set");
	}

	bool VariantMatches(const FString& Filter, const FString& RecordVariant)
	{
		if (Filter == TEXT("all")) return true;
		if (Filter == TEXT("gameplay_ability")) return RecordVariant == TEXT("gameplay_ability_blueprint");
		if (Filter == TEXT("gameplay_effect")) return RecordVariant == TEXT("gameplay_effect_blueprint");
		return RecordVariant.StartsWith(TEXT("attribute_set"), ESearchCase::CaseSensitive);
	}

	FString ModifierCanonical(const FHyperAIGASModifierView& Value)
	{
		FString Result;
		AppendToken(Result, Value.AttributeOwnerClassPath);
		AppendToken(Result, Value.AttributeName);
		AppendToken(Result, Value.Operation);
		AppendToken(Result, Value.MagnitudeKind);
		AppendBool(Result, Value.bHasStaticMagnitude);
		AppendDouble(Result, Value.StaticMagnitude);
		AppendStrings(Result, Value.SourceRequiredTags);
		AppendStrings(Result, Value.SourceBlockedTags);
		AppendStrings(Result, Value.TargetRequiredTags);
		AppendStrings(Result, Value.TargetBlockedTags);
		return Result;
	}

	FString CueCanonical(const FHyperAIGASCueView& Value)
	{
		FString Result;
		AppendStrings(Result, Value.CueTags);
		AppendDouble(Result, Value.MinLevel);
		AppendDouble(Result, Value.MaxLevel);
		AppendToken(Result, Value.MagnitudeAttributeOwnerClassPath);
		AppendToken(Result, Value.MagnitudeAttributeName);
		return Result;
	}

	FString AttributeCanonical(const FHyperAIGASAttributeView& Value)
	{
		FString Result;
		AppendToken(Result, Value.OwnerClassPath);
		AppendToken(Result, Value.Name);
		AppendToken(Result, Value.PropertyType);
		AppendBool(Result, Value.bGameplayAttributeData);
		AppendBool(Result, Value.bHasDefaultValue);
		AppendDouble(Result, Value.DefaultValue);
		return Result;
	}

	FString RecordCanonical(const FHyperAIGASAssetRecord& Record)
	{
		FString Result;
		AppendToken(Result, TEXT("hyperai.gas-record.v1"));
		AppendToken(Result, Record.Variant);
		AppendToken(Result, Record.AssetPath);
		AppendToken(Result, Record.GeneratedClassPath);
		AppendToken(Result, Record.ParentClassPath);
		AppendToken(Result, Record.BlueprintStatus);
		AppendBool(Result, Record.bBlueprint);
		AppendBool(Result, Record.bPackageDirty);
		AppendBool(Result, Record.bRevisionComplete);
		AppendBool(Result, Record.bMutationCasEligible);
		AppendToken(Result, Record.InstancingPolicy);
		AppendToken(Result, Record.ReplicationPolicy);
		AppendToken(Result, Record.NetExecutionPolicy);
		AppendToken(Result, Record.NetSecurityPolicy);
		AppendToken(Result, Record.CostEffectClassPath);
		AppendToken(Result, Record.CooldownEffectClassPath);
		AppendToken(Result, Record.DurationPolicy);
		AppendBool(Result, Record.bHasStaticDuration);
		AppendDouble(Result, Record.DurationSeconds);
		AppendBool(Result, Record.bHasStaticMaxDuration);
		AppendDouble(Result, Record.MaxDurationSeconds);
		AppendDouble(Result, Record.PeriodSeconds);
		AppendBool(Result, Record.bExecutePeriodicEffectOnApplication);
		AppendBool(Result, Record.bRequireModifierSuccessToTriggerCues);
		AppendBool(Result, Record.bSuppressStackingCues);
		AppendInt(Result, Record.ExecutionCount);
		AppendInt(Result, Record.ComponentCount);
		AppendStrings(Result, Record.AssetTags);
		AppendStrings(Result, Record.GrantedTags);
		AppendStrings(Result, Record.BlockedAbilityTags);
		AppendInt(Result, Record.Modifiers.Num());
		for (const FHyperAIGASModifierView& Value : Record.Modifiers)
		{
			AppendToken(Result, ModifierCanonical(Value));
		}
		AppendInt(Result, Record.Cues.Num());
		for (const FHyperAIGASCueView& Value : Record.Cues)
		{
			AppendToken(Result, CueCanonical(Value));
		}
		TArray<FString> Attributes;
		for (const FHyperAIGASAttributeView& Value : Record.Attributes) Attributes.Add(AttributeCanonical(Value));
		Attributes.Sort();
		AppendStrings(Result, Attributes);
		return Result;
	}

	int32 ApproximateRecordBytes(const FHyperAIGASAssetRecord& Record)
	{
		const int64 ValueBytes = static_cast<int64>(RecordCanonical(Record).Len()
			+ Record.StableId.Len() + Record.Revision.Len()) * MaxUtf8BytesPerCharacter;
		const int64 StructuralBytes = 4096ll
			+ static_cast<int64>(Record.Modifiers.Num()) * 768
			+ static_cast<int64>(Record.Cues.Num()) * 512
			+ static_cast<int64>(Record.Attributes.Num()) * 512;
		return static_cast<int32>(FMath::Min<int64>(
			ValueBytes + StructuralBytes, TNumericLimits<int32>::Max()));
	}

	void CaptureAttributeSet(
		UClass* Class,
		const bool bBlueprint,
		const FString& AssetPath,
		const bool bIncludeAttributes,
		FCaptureBudget& Budget,
		const double AbsoluteDeadlineSeconds,
		FHyperAIGASAssetRecord& Out)
	{
		Out.Variant = bBlueprint ? TEXT("attribute_set_blueprint") : TEXT("attribute_set");
		Out.AssetPath = AssetPath;
		Out.GeneratedClassPath = Class ? Class->GetPathName() : FString();
		Out.ParentClassPath = Class && Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : FString();
		Out.StableId = TEXT("gas:") + Out.Variant + TEXT(":") + Out.AssetPath;
		Out.bBlueprint = bBlueprint;
		Out.bPackageDirty = Class && Class->GetOutermost() && Class->GetOutermost()->IsDirty();
		Out.bRevisionComplete = Class != nullptr;
		if (!Class || !Budget.Reserve(1, 1024))
		{
			Out.bRevisionComplete = false;
			return;
		}
		if (!bIncludeAttributes) return;

		TArray<FProperty*> AttributeProperties;
		AttributeProperties.Reserve(FHyperAIStudioGASContracts::MaxAttributes);
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
			{
				Out.bRevisionComplete = false;
				return;
			}
			if (!FGameplayAttribute::IsSupportedProperty(*It)) continue;
			if (AttributeProperties.Num() >= FHyperAIStudioGASContracts::MaxAttributes)
			{
				Out.bRevisionComplete = false;
				return;
			}
			if (!Budget.Reserve(1, 512))
			{
				Out.bRevisionComplete = false;
				return;
			}
			AttributeProperties.Add(*It);
		}
		AttributeProperties.Sort([](const FProperty& A, const FProperty& B)
		{
			return A.GetFName().LexicalLess(B.GetFName());
		});
		const UAttributeSet* Defaults = Cast<UAttributeSet>(Class->GetDefaultObject(false));
		for (FProperty* Property : AttributeProperties)
		{
			if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
			{
				Out.bRevisionComplete = false;
				return;
			}
			const FGameplayAttribute Attribute(Property);
			FHyperAIGASAttributeView& View = Out.Attributes.AddDefaulted_GetRef();
			View.OwnerClassPath = AttributeOwnerPath(Attribute);
			View.Name = Attribute.GetName();
			View.StableId = View.OwnerClassPath + TEXT(":") + View.Name;
			const FProperty* AttributeProperty = Attribute.GetUProperty();
			View.bGameplayAttributeData =
				FGameplayAttribute::IsGameplayAttributeDataProperty(AttributeProperty);
			View.PropertyType = View.bGameplayAttributeData ? TEXT("gameplay_attribute_data") : TEXT("float");
			if (Defaults && Attribute.IsValid())
			{
				const float Value = Attribute.GetNumericValue(Defaults);
				View.bHasDefaultValue = FMath::IsFinite(Value);
				View.DefaultValue = View.bHasDefaultValue ? Value : 0.0;
			}
		}
	}

	bool CaptureBlueprint(
		UBlueprint* Blueprint,
		const FHyperAIGASInspectRequest& Projection,
		FCaptureBudget& Budget,
		const double AbsoluteDeadlineSeconds,
		FHyperAIGASAssetRecord& Out)
	{
		if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GetOutermost()
			|| !Budget.Reserve(1, 4096)) return false;
		UClass* GeneratedClass = Blueprint->GeneratedClass;
		Out.AssetPath = Blueprint->GetPathName();
		Out.GeneratedClassPath = GeneratedClass->GetPathName();
		Out.ParentClassPath = GeneratedClass->GetSuperClass()
			? GeneratedClass->GetSuperClass()->GetPathName() : FString();
		Out.BlueprintStatus = BlueprintStatusToken(Blueprint->Status);
		Out.bBlueprint = true;
		Out.bPackageDirty = Blueprint->GetOutermost()->IsDirty();
		Out.bRevisionComplete = true;
		if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
		{
			Out.bRevisionComplete = false;
			return false;
		}

		if (GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			Out.Variant = TEXT("gameplay_ability_blueprint");
			const UGameplayAbility* Ability = Cast<UGameplayAbility>(GeneratedClass->GetDefaultObject(false));
			if (!Ability) return false;
			Out.InstancingPolicy = InstancingPolicyToken(Ability->GetInstancingPolicy());
			Out.ReplicationPolicy = ReplicationPolicyToken(Ability->GetReplicationPolicy());
			Out.NetExecutionPolicy = NetExecutionPolicyToken(Ability->GetNetExecutionPolicy());
			Out.NetSecurityPolicy = NetSecurityPolicyToken(Ability->GetNetSecurityPolicy());
			if (const UGameplayEffect* Cost = Ability->GetCostGameplayEffect())
			{
				Out.CostEffectClassPath = Cost->GetClass()->GetPathName();
			}
			if (const UGameplayEffect* Cooldown = Ability->GetCooldownGameplayEffect())
			{
				Out.CooldownEffectClassPath = Cooldown->GetClass()->GetPathName();
			}
			if (Projection.bIncludeTags)
			{
				Out.AssetTags = TagStrings(Ability->GetAssetTags(),
					FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
			}
			Out.bMutationCasEligible = Projection.bIncludeTags;
		}
		else if (GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			Out.Variant = TEXT("gameplay_effect_blueprint");
			const UGameplayEffect* Effect = Cast<UGameplayEffect>(GeneratedClass->GetDefaultObject(false));
			if (!Effect) return false;
			Out.DurationPolicy = DurationPolicyToken(Effect->DurationPolicy);
			float Magnitude = 0.0f;
			Out.bHasStaticDuration = Effect->DurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, Magnitude);
			Out.DurationSeconds = Out.bHasStaticDuration && FMath::IsFinite(Magnitude) ? Magnitude : 0.0;
			Magnitude = 0.0f;
			Out.bHasStaticMaxDuration = Effect->MaxDurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, Magnitude);
			Out.MaxDurationSeconds = Out.bHasStaticMaxDuration && FMath::IsFinite(Magnitude) ? Magnitude : 0.0;
			const float Period = Effect->Period.GetValueAtLevel(1.0f);
			Out.PeriodSeconds = FMath::IsFinite(Period) ? Period : 0.0;
			if (!Out.bHasStaticDuration || !Out.bHasStaticMaxDuration
				|| !Effect->Period.IsStatic() || !FMath::IsFinite(Period))
			{
				Out.bRevisionComplete = false;
			}
			Out.bExecutePeriodicEffectOnApplication = Effect->bExecutePeriodicEffectOnApplication;
			Out.bRequireModifierSuccessToTriggerCues = Effect->bRequireModifierSuccessToTriggerCues;
			Out.bSuppressStackingCues = Effect->bSuppressStackingCues;
			Out.ExecutionCount = Effect->Executions.Num();
			if (Out.ExecutionCount != 0)
			{
				// Execution definitions have additional persisted semantics not projected in v1.
				Out.bRevisionComplete = false;
			}
			int32 ComponentObjectsScanned = 0;
			bool bComponentInventoryComplete = true;
			ForEachObjectWithOuterBreakable(Effect,
				[&](UObject* Child)
				{
					if (ComponentObjectsScanned
							>= FHyperAIStudioGASContracts::MaxEffectComponentsScanned
						|| FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
					{
						bComponentInventoryComplete = false;
						return false;
					}
					++ComponentObjectsScanned;
					if (Child && Child->IsA<UGameplayEffectComponent>()) ++Out.ComponentCount;
					return true;
					}, EGetObjectsFlags::IncludeNestedObjects);
			if (!bComponentInventoryComplete || Out.ComponentCount != 0)
			{
				// Component contents are intentionally unsupported; a count alone is never exact CAS.
				Out.bRevisionComplete = false;
			}
			if (Projection.bIncludeTags)
			{
				Out.AssetTags = TagStrings(Effect->GetAssetTags(),
					FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
				Out.GrantedTags = TagStrings(Effect->GetGrantedTags(),
					FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
				Out.BlockedAbilityTags = TagStrings(Effect->GetBlockedAbilityTags(),
					FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
			}
			if (Projection.bIncludeModifiers)
			{
				const int32 Count = FMath::Min(Effect->Modifiers.Num(), FHyperAIStudioGASContracts::MaxModifiers);
				if (!Budget.Reserve(Count, static_cast<int64>(Count) * 1536))
				{
					Out.bRevisionComplete = false;
					return false;
				}
				for (int32 Index = 0; Index < Count; ++Index)
				{
					if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
					{
						Out.bRevisionComplete = false;
						break;
					}
					const FGameplayModifierInfo& Modifier = Effect->Modifiers[Index];
					FHyperAIGASModifierView& View = Out.Modifiers.AddDefaulted_GetRef();
					View.AttributeOwnerClassPath = AttributeOwnerPath(Modifier.Attribute);
					View.AttributeName = Modifier.Attribute.IsValid() ? Modifier.Attribute.GetName() : FString();
					View.Operation = ModifierOperationToken(Modifier.ModifierOp);
					View.MagnitudeKind = MagnitudeKindToken(Modifier.ModifierMagnitude.GetMagnitudeCalculationType());
					float StaticMagnitude = 0.0f;
					View.bHasStaticMagnitude = Modifier.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.0f, StaticMagnitude);
					View.StaticMagnitude = View.bHasStaticMagnitude && FMath::IsFinite(StaticMagnitude)
						? StaticMagnitude : 0.0;
					if (!View.bHasStaticMagnitude || !FMath::IsFinite(StaticMagnitude))
					{
						Out.bRevisionComplete = false;
					}
					View.SourceRequiredTags = TagStrings(Modifier.SourceTags.RequireTags,
						FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
					View.SourceBlockedTags = TagStrings(Modifier.SourceTags.IgnoreTags,
						FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
					View.TargetRequiredTags = TagStrings(Modifier.TargetTags.RequireTags,
						FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
					View.TargetBlockedTags = TagStrings(Modifier.TargetTags.IgnoreTags,
						FHyperAIStudioGASContracts::MaxTagsPerSet, Budget, Out.bRevisionComplete);
					View.StableId = FString::Printf(TEXT("modifier:%04d:%s:%s"), Index,
						*View.AttributeOwnerClassPath, *View.AttributeName);
				}
				if (Effect->Modifiers.Num() > Count) Out.bRevisionComplete = false;
			}
			if (Projection.bIncludeCues)
			{
				const int32 Count = FMath::Min(Effect->GameplayCues.Num(), FHyperAIStudioGASContracts::MaxCues);
				if (!Budget.Reserve(Count, static_cast<int64>(Count) * 1024))
				{
					Out.bRevisionComplete = false;
					return false;
				}
				for (int32 Index = 0; Index < Count; ++Index)
				{
					if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
					{
						Out.bRevisionComplete = false;
						break;
					}
					const FGameplayEffectCue& Cue = Effect->GameplayCues[Index];
					FHyperAIGASCueView& View = Out.Cues.AddDefaulted_GetRef();
					View.CueTags = TagStrings(Cue.GameplayCueTags,
						FHyperAIStudioGASContracts::MaxCueTags, Budget, Out.bRevisionComplete);
					View.MinLevel = Cue.MinLevel;
					View.MaxLevel = Cue.MaxLevel;
					View.MagnitudeAttributeOwnerClassPath = AttributeOwnerPath(Cue.MagnitudeAttribute);
					View.MagnitudeAttributeName = Cue.MagnitudeAttribute.IsValid()
						? Cue.MagnitudeAttribute.GetName() : FString();
					View.StableId = FString::Printf(TEXT("cue:%04d"), Index);
				}
				if (Effect->GameplayCues.Num() > Count) Out.bRevisionComplete = false;
			}
			Out.bMutationCasEligible = Projection.bIncludeTags
				&& Projection.bIncludeModifiers && Projection.bIncludeCues;
		}
		else if (GeneratedClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			CaptureAttributeSet(GeneratedClass, true, Out.AssetPath,
				Projection.bIncludeAttributes, Budget, AbsoluteDeadlineSeconds, Out);
			Out.BlueprintStatus = BlueprintStatusToken(Blueprint->Status);
			Out.bPackageDirty = Blueprint->GetOutermost()->IsDirty();
		}
		else
		{
			return false;
		}

		Out.StableId = TEXT("gas:") + Out.Variant + TEXT(":") + Out.AssetPath;
		Out.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(RecordCanonical(Out));
		Out.bRevisionComplete &= FHyperAIStudioGASContracts::IsCanonicalSha256(Out.Revision);
		return true;
	}

	bool IsRelevantBlueprint(UBlueprint* Blueprint)
	{
		return Blueprint && Blueprint->GeneratedClass
			&& (Blueprint->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass())
				|| Blueprint->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass())
				|| Blueprint->GeneratedClass->IsChildOf(UAttributeSet::StaticClass()));
	}

	FHyperAIStudioGASValueSnapshot CaptureSnapshot(const FHyperAIGASInspectRequest& Request)
	{
		FHyperAIStudioGASValueSnapshot Snapshot;
		const double AbsoluteDeadlineSeconds = FPlatformTime::Seconds()
			+ static_cast<double>(Request.MaxGameThreadMs) / 1000.0;
		FCaptureBudget Budget;
		Budget.AbsoluteDeadlineSeconds = AbsoluteDeadlineSeconds;
		bool bIssueTruncated = false;
		bool bRecordLimitHit = false;
		TSet<FString> SeenPaths;
		auto AddBlueprint = [&](UBlueprint* Blueprint)
		{
			if (!Blueprint || SeenPaths.Contains(Blueprint->GetPathName())) return;
			if (Snapshot.Records.Num() >= FHyperAIStudioGASContracts::MaxRecords)
			{
				bRecordLimitHit = true;
				return;
			}
			FHyperAIGASAssetRecord Record;
			if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
			{
				Snapshot.bComplete = false;
				AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
					bIssueTruncated, TEXT("capture_deadline_exceeded"), TEXT("warning"),
					Blueprint->GetPathName(), TEXT("gas:deadline"), -1,
					TEXT("Loaded GAS projection reached its hard game-thread deadline."));
				return;
			}
			if (!CaptureBlueprint(Blueprint, Request, Budget,
				AbsoluteDeadlineSeconds, Record))
			{
				Snapshot.bComplete = false;
				AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
					bIssueTruncated, TEXT("asset_capture_unavailable"), TEXT("warning"),
					Blueprint->GetPathName(), TEXT("gas:asset:") + Blueprint->GetPathName(), -1,
					TEXT("Loaded GAS Blueprint did not expose complete generated-class defaults."));
				return;
			}
			if (VariantMatches(Request.Variant, Record.Variant))
			{
				SeenPaths.Add(Record.AssetPath);
				Snapshot.bComplete &= Record.bRevisionComplete;
				Snapshot.Records.Add(MoveTemp(Record));
			}
		};

		if (!Request.AssetPaths.IsEmpty())
		{
			for (const FString& Path : Request.AssetPaths)
			{
				if (FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
				{
					Snapshot.bComplete = false;
					AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
						bIssueTruncated, TEXT("capture_deadline_exceeded"), TEXT("warning"),
						Path, TEXT("gas:deadline"), -1,
						TEXT("Exact loaded GAS projection reached its hard game-thread deadline."));
					break;
				}
				UObject* Object = FSoftObjectPath(Path).ResolveObject();
				UBlueprint* Blueprint = Cast<UBlueprint>(Object);
				++Snapshot.LoadedObjectsScanned;
				if (!Blueprint)
				{
					Snapshot.bComplete = false;
					AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
						bIssueTruncated, TEXT("asset_not_loaded"), TEXT("warning"), Path,
						TEXT("gas:asset:") + Path, -1,
						TEXT("Exact GAS asset is not already loaded; inspection never synchronously loads it."));
					continue;
				}
				if (!IsRelevantBlueprint(Blueprint))
				{
					Snapshot.bComplete = false;
					AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
						bIssueTruncated, TEXT("unsupported_asset_variant"), TEXT("warning"), Path,
						TEXT("gas:asset:") + Path, -1,
						TEXT("Loaded object is not a GameplayAbility, GameplayEffect, or AttributeSet Blueprint."));
					continue;
				}
				const int32 RecordCountBefore = Snapshot.Records.Num();
				const int32 IssueCountBefore = Snapshot.CaptureIssues.Num();
				AddBlueprint(Blueprint);
				if (Request.Variant != TEXT("all")
					&& Snapshot.Records.Num() == RecordCountBefore
					&& Snapshot.CaptureIssues.Num() == IssueCountBefore)
				{
					Snapshot.bComplete = false;
					AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
						bIssueTruncated, TEXT("asset_variant_filter_mismatch"), TEXT("warning"),
						Path, TEXT("gas:asset:") + Path, -1,
						TEXT("Exact loaded GAS asset does not match the requested projection variant."));
				}
			}
		}
		else
		{
			int32 Scanned = 0;
			for (TObjectIterator<UBlueprint> It; It
				&& Scanned < FHyperAIStudioGASContracts::MaxLoadedObjectsScanned
				&& FPlatformTime::Seconds() <= AbsoluteDeadlineSeconds; ++It)
			{
				++Scanned;
				UBlueprint* Blueprint = *It;
				if (!IsRelevantBlueprint(Blueprint) || !Blueprint->GetOutermost()
					|| !Blueprint->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))) continue;
				AddBlueprint(Blueprint);
			}
			Snapshot.LoadedObjectsScanned += Scanned;
			if (Scanned >= FHyperAIStudioGASContracts::MaxLoadedObjectsScanned
				|| FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
			{
				Snapshot.bComplete = false;
				AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
					bIssueTruncated, TEXT("loaded_scan_truncated"), TEXT("warning"), FString(),
					TEXT("gas:scan"), -1,
					TEXT("Loaded UObject scan reached its hard object or game-thread bound."));
			}

			if (Request.bIncludeNativeAttributeSets && (Request.Variant == TEXT("all")
				|| Request.Variant == TEXT("attribute_set")))
			{
				int32 ClassScanned = 0;
				for (TObjectIterator<UClass> It; It
					&& ClassScanned < FHyperAIStudioGASContracts::MaxLoadedObjectsScanned
					&& FPlatformTime::Seconds() <= AbsoluteDeadlineSeconds; ++It)
				{
					++ClassScanned;
					UClass* Class = *It;
					if (!Class || Class == UAttributeSet::StaticClass() || !Class->HasAnyClassFlags(CLASS_Native)
						|| Class->HasAnyClassFlags(CLASS_Abstract) || !Class->IsChildOf(UAttributeSet::StaticClass())) continue;
					const FString ClassPath = Class->GetPathName();
					if (SeenPaths.Contains(ClassPath)) continue;
					if (Snapshot.Records.Num() >= FHyperAIStudioGASContracts::MaxRecords)
					{
						bRecordLimitHit = true;
						continue;
					}
					FHyperAIGASAssetRecord Record;
					CaptureAttributeSet(Class, false, ClassPath, Request.bIncludeAttributes,
						Budget, AbsoluteDeadlineSeconds, Record);
					Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(RecordCanonical(Record));
					Record.bRevisionComplete &= FHyperAIStudioGASContracts::IsCanonicalSha256(Record.Revision);
					Snapshot.bComplete &= Record.bRevisionComplete;
					SeenPaths.Add(ClassPath);
					Snapshot.Records.Add(MoveTemp(Record));
				}
				Snapshot.LoadedObjectsScanned += ClassScanned;
				if (ClassScanned >= FHyperAIStudioGASContracts::MaxLoadedObjectsScanned
					|| FPlatformTime::Seconds() > AbsoluteDeadlineSeconds)
				{
					Snapshot.bComplete = false;
					AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
						bIssueTruncated, TEXT("native_attribute_class_scan_truncated"),
						TEXT("warning"), FString(), TEXT("gas:native-attribute-scan"), -1,
						TEXT("Loaded native AttributeSet class scan reached its hard object or game-thread bound."));
				}
			}
		}

		if (bRecordLimitHit)
		{
			Snapshot.bComplete = false;
			AddIssue(Snapshot.CaptureIssues, FHyperAIStudioGASContracts::MaxIssues,
				bIssueTruncated, TEXT("record_projection_truncated"), TEXT("warning"), FString(),
				TEXT("gas:records"), -1, TEXT("Loaded GAS record projection reached its hard bound."));
		}
		Snapshot.CaptureIssues.Sort([](const FHyperAIGASIssue& A, const FHyperAIGASIssue& B)
		{
			return A.Code == B.Code ? A.StableId < B.StableId : A.Code < B.Code;
		});
		FHyperAIStudioGASContracts::ComputeSnapshotRevision(Snapshot);
		return Snapshot;
	}

	bool ParseCursor(const FString& Cursor, const FString& Revision, int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("|"), false);
		if (Parts.Num() != 3 || Parts[0] != TEXT("gas-v1") || Parts[1] != Revision
			|| !LexTryParseString(OutOffset, *Parts[2]) || OutOffset < 0
			|| LexToString(OutOffset) != Parts[2]) return false;
		return true;
	}

	FString MakeCursor(const FString& Revision, const int32 Offset)
	{
		return FString::Printf(TEXT("gas-v1|%s|%d"), *Revision, Offset);
	}
}

FString FHyperAIStudioGASContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioGAS.HyperAIStudioGASToolset");
}

const TArray<FHyperAIStudioGASManifestEntry>& FHyperAIStudioGASContracts::GetManifest()
{
	static const TArray<FHyperAIStudioGASManifestEntry> Manifest = {
		{TEXT("hyper_gas_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_gas_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_gas_validate"), GetQualifiedToolsetName()}
	};
	return Manifest;
}

const TArray<FHyperAIGASEpicDelegate>& FHyperAIStudioGASContracts::GetEpicDelegates()
{
	static const TArray<FHyperAIGASEpicDelegate> Delegates = {
		{TEXT("inspect_attribute_values"), TEXT("GASToolsets.AbilitySystemInspectorToolset"),
			TEXT("GetAttributeValues"), TEXT("read"), TEXT("Runtime ASC inspection is already native in Epic GAS Toolsets.")},
		{TEXT("inspect_active_effects"), TEXT("GASToolsets.AbilitySystemInspectorToolset"),
			TEXT("GetActiveEffects"), TEXT("read"), TEXT("Runtime active-effect inspection remains an Epic delegate.")},
		{TEXT("inspect_granted_abilities"), TEXT("GASToolsets.AbilitySystemInspectorToolset"),
			TEXT("GetGrantedAbilities"), TEXT("read"), TEXT("Runtime granted-ability inspection remains an Epic delegate.")},
		{TEXT("inspect_active_tags"), TEXT("GASToolsets.AbilitySystemInspectorToolset"),
			TEXT("GetActiveTags"), TEXT("read"), TEXT("Runtime active-tag inspection remains an Epic delegate.")},
		{TEXT("find_attribute_set_classes"), TEXT("GASToolsets.AttributeSetToolset"),
			TEXT("FindAttributeSetClasses"), TEXT("read"), TEXT("Epic owns broad native AttributeSet class discovery.")},
		{TEXT("list_attributes"), TEXT("GASToolsets.AttributeSetToolset"),
			TEXT("ListAttributes"), TEXT("read"), TEXT("Epic owns direct AttributeSet member listing.")},
		{TEXT("list_gameplay_cues"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("ListCues"), TEXT("read"), TEXT("Cue-manager inventory remains an Epic delegate.")},
		{TEXT("inspect_gameplay_cue"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("GetCueInfo"), TEXT("read"), TEXT("Cue-manager details remain an Epic delegate.")},
		{TEXT("find_cue_notify_assets"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("FindCueNotifyAssets"), TEXT("read"), TEXT("Cue notify discovery remains an Epic delegate.")},
		{TEXT("create_cue_notify_asset"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("CreateCueNotifyAsset"), TEXT("edit"), TEXT("HyperAI does not duplicate Epic cue asset creation.")},
		{TEXT("add_cue_tag"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("AddCueTag"), TEXT("edit"), TEXT("HyperAI only references existing tags; Epic owns tag creation.")},
		{TEXT("remove_cue_tag"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("RemoveCueTag"), TEXT("edit"), TEXT("HyperAI does not duplicate Epic cue-tag mutation.")},
		{TEXT("find_cue_tags_without_notifies"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("FindCueTagsWithoutNotifies"), TEXT("read"), TEXT("Cue coverage analysis remains an Epic delegate.")},
		{TEXT("execute_cue_on_selected_actor"), TEXT("GASToolsets.GameplayCueToolset"),
			TEXT("ExecuteCueOnSelectedActor"), TEXT("external_effect"), TEXT("Runtime cue execution is never routed through this editor mutation pack.")}
	};
	return Delegates;
}

bool FHyperAIStudioGASContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioGASContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioGASManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
	TArray<FString> Names;
	TSet<FString> UniqueNames;
	for (const FHyperAIStudioGASManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| UniqueNames.Contains(Entry.Name)) return false;
		UniqueNames.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioGASContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters
		|| HyperAIStudio::GAS::Private::HasEmbeddedNull(Path) || Path.Contains(TEXT("\\"))
		|| Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))
		|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidObjectPath(Path)) return false;
	FString PackageName;
	FString ObjectName;
	return Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd) && !ObjectName.IsEmpty() && !ObjectName.EndsWith(TEXT("_C"))
		&& FPackageName::GetShortName(PackageName) == ObjectName;
}

FHyperAIStudioGASValueSnapshot FHyperAIStudioGASContracts::CaptureLoadedSnapshot(
	const FHyperAIGASInspectRequest& Request)
{
	return HyperAIStudio::GAS::Private::CaptureSnapshot(Request);
}

FString FHyperAIStudioGASContracts::ComputeBaseRevision(
	const TArray<FHyperAIGASAssetRecord>& Records)
{
	using namespace HyperAIStudio::GAS::Private;
	if (Records.IsEmpty() || Records.Num() > MaxOperations)
	{
		return FString();
	}
	TArray<FHyperAIGASAssetRecord> Sorted = Records;
	Sorted.Sort([](const FHyperAIGASAssetRecord& A, const FHyperAIGASAssetRecord& B)
	{
		return A.AssetPath < B.AssetPath;
	});
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gas-base.v1"));
	TSet<FString> Paths;
	for (const FHyperAIGASAssetRecord& Record : Sorted)
	{
		if (!IsCanonicalProjectObjectPath(Record.AssetPath)
			|| !Record.bRevisionComplete || !Record.bMutationCasEligible
			|| !IsCanonicalSha256(Record.Revision)
			|| Paths.Contains(Record.AssetPath))
		{
			return FString();
		}
		Paths.Add(Record.AssetPath);
		AppendToken(Canonical, Record.AssetPath);
		AppendToken(Canonical, Record.Revision);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

bool FHyperAIStudioGASContracts::IsCanonicalSha256(const FString& Value)
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

FString FHyperAIStudioGASContracts::ComputeSnapshotRevision(FHyperAIStudioGASValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::GAS::Private;
	Snapshot.Records.Sort([](const FHyperAIGASAssetRecord& A, const FHyperAIGASAssetRecord& B)
	{
		return A.StableId < B.StableId;
	});
	TSet<FString> SeenStableIds;
	for (FHyperAIGASAssetRecord& Record : Snapshot.Records)
	{
		if (Record.StableId.IsEmpty() || SeenStableIds.Contains(Record.StableId))
		{
			Record.bRevisionComplete = false;
		}
		SeenStableIds.Add(Record.StableId);
		Snapshot.bComplete &= Record.bRevisionComplete;
		const FString ExactRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			RecordCanonical(Record));
		if (!IsCanonicalSha256(ExactRevision))
		{
			Snapshot.bComplete = false;
			Record.bRevisionComplete = false;
			Record.Revision.Reset();
		}
		else
		{
			Record.Revision = ExactRevision;
		}
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gas-snapshot.v1"));
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.Records.Num());
	for (const FHyperAIGASAssetRecord& Record : Snapshot.Records)
	{
		AppendToken(Canonical, Record.StableId);
		AppendToken(Canonical, Record.Revision);
	}
	Snapshot.CaptureIssues.Sort([](const FHyperAIGASIssue& A, const FHyperAIGASIssue& B)
	{
		if (A.Code != B.Code) return A.Code < B.Code;
		if (A.StableId != B.StableId) return A.StableId < B.StableId;
		if (A.AssetPath != B.AssetPath) return A.AssetPath < B.AssetPath;
		if (A.Severity != B.Severity) return A.Severity < B.Severity;
		return A.Message < B.Message;
	});
	for (const FHyperAIGASIssue& Issue : Snapshot.CaptureIssues)
	{
		AppendToken(Canonical, Issue.Code);
		AppendToken(Canonical, Issue.Severity);
		AppendToken(Canonical, Issue.AssetPath);
		AppendToken(Canonical, Issue.StableId);
		AppendInt(Canonical, Issue.OperationIndex);
		AppendToken(Canonical, Issue.Message);
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Snapshot.Revision)) Snapshot.bComplete = false;
	return Snapshot.Revision;
}

TArray<FHyperAIGASIssue> FHyperAIStudioGASContracts::ValidateValueSnapshot(
	const FHyperAIStudioGASValueSnapshot& Snapshot,
	const bool bRequirePackagesClean,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::GAS::Private;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	TArray<FHyperAIGASIssue> Issues;
	auto Add = [&](const TCHAR* Code, const TCHAR* Severity, const FHyperAIGASAssetRecord& Record,
		const FString& StableId, const FString& Message)
	{
		AddIssue(Issues, Maximum, bOutTruncated, Code, Severity, Record.AssetPath,
			StableId.IsEmpty() ? Record.StableId : StableId, -1, Message);
	};
	for (const FHyperAIGASIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *CaptureIssue.Code, *CaptureIssue.Severity,
			CaptureIssue.AssetPath, CaptureIssue.StableId, CaptureIssue.OperationIndex, CaptureIssue.Message);
	}

	TSet<FString> StableIds;
	for (const FHyperAIGASAssetRecord& Record : Snapshot.Records)
	{
		if (Record.StableId.IsEmpty() || StableIds.Contains(Record.StableId))
		{
			Add(TEXT("duplicate_or_missing_stable_id"), TEXT("error"), Record, Record.StableId,
				TEXT("Every captured GAS asset needs one unique stable identity."));
		}
		StableIds.Add(Record.StableId);
			if (!Record.bRevisionComplete || !IsCanonicalSha256(Record.Revision))
			{
				Add(TEXT("revision_incomplete"), TEXT("error"), Record, Record.StableId,
					TEXT("Independent validation requires a complete canonical per-asset revision."));
			}
			if (Record.bBlueprint && !FHyperAIStudioGASContracts::IsCanonicalProjectObjectPath(Record.AssetPath))
			{
				Add(TEXT("blueprint_asset_path_invalid"), TEXT("error"), Record, Record.StableId,
					TEXT("Blueprint evidence must use one canonical project object path."));
			}
		if (Record.bBlueprint && Record.BlueprintStatus == TEXT("compile_error"))
		{
			Add(TEXT("blueprint_compile_error"), TEXT("error"), Record, Record.StableId,
				TEXT("The GAS Blueprint is in a compiler error state."));
		}
		else if (Record.bBlueprint && (Record.BlueprintStatus == TEXT("dirty_compile_required")
			|| Record.BlueprintStatus == TEXT("being_created") || Record.BlueprintStatus == TEXT("unknown")))
		{
			Add(TEXT("blueprint_compile_state_unproven"), TEXT("warning"), Record, Record.StableId,
				TEXT("Blueprint compile correctness is not currently proven."));
		}
			if (bRequirePackagesClean && Record.bPackageDirty)
			{
				Add(TEXT("package_dirty"), TEXT("error"), Record, Record.StableId,
					TEXT("Fresh persistence validation requires the owning package to be clean."));
			}
			auto ValidateTags = [&](const TArray<FString>& Tags, const TCHAR* Code)
			{
				for (const FString& TagName : Tags)
				{
					if (!UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), false).IsValid())
					{
						Add(Code, TEXT("error"), Record, Record.StableId,
							TEXT("Captured GAS tag reference is no longer registered."));
					}
				}
			};
			ValidateTags(Record.AssetTags, TEXT("asset_tag_invalid"));
			ValidateTags(Record.GrantedTags, TEXT("granted_tag_invalid"));
			ValidateTags(Record.BlockedAbilityTags, TEXT("blocked_ability_tag_invalid"));

			if (Record.Variant == TEXT("gameplay_ability_blueprint"))
			{
				if (Record.InstancingPolicy == TEXT("unknown") || Record.InstancingPolicy.IsEmpty()
					|| Record.ReplicationPolicy.IsEmpty() || Record.NetExecutionPolicy == TEXT("unknown")
					|| Record.NetExecutionPolicy.IsEmpty() || Record.NetSecurityPolicy == TEXT("unknown")
					|| Record.NetSecurityPolicy.IsEmpty())
				{
					Add(TEXT("ability_policy_unknown"), TEXT("error"), Record, Record.StableId,
						TEXT("GameplayAbility policy projection is incomplete or unsupported."));
				}
			}
			else if (Record.Variant == TEXT("gameplay_effect_blueprint"))
			{
				if (Record.ExecutionCount != 0)
				{
					Add(TEXT("execution_projection_unsupported"), TEXT("error"), Record,
						Record.StableId,
						TEXT("GameplayEffect execution definitions are outside the closed v1 projection."));
				}
				if (Record.ComponentCount != 0)
				{
					Add(TEXT("component_projection_unsupported"), TEXT("error"), Record,
						Record.StableId,
						TEXT("GameplayEffect component contents are outside the closed v1 projection."));
				}
				if (Record.DurationPolicy != TEXT("instant")
					&& Record.DurationPolicy != TEXT("infinite")
					&& Record.DurationPolicy != TEXT("duration"))
				{
					Add(TEXT("duration_policy_unknown"), TEXT("error"), Record, Record.StableId,
						TEXT("GameplayEffect duration policy is not one of the closed supported policies."));
				}
				if (Record.DurationPolicy == TEXT("duration")
					&& (!Record.bHasStaticDuration || !IsFinite(Record.DurationSeconds)
						|| Record.DurationSeconds <= 0.0))
			{
					Add(TEXT("duration_invalid"), TEXT("error"), Record, Record.StableId,
						TEXT("A duration GameplayEffect needs a finite positive static duration for this supported slice."));
				}
				if (Record.DurationPolicy == TEXT("duration")
					&& (!Record.bHasStaticMaxDuration || !IsFinite(Record.MaxDurationSeconds)
						|| Record.MaxDurationSeconds < 0.0))
				{
					Add(TEXT("max_duration_invalid"), TEXT("error"), Record, Record.StableId,
						TEXT("A duration GameplayEffect needs a finite non-negative static maximum duration."));
				}
			if (!IsFinite(Record.PeriodSeconds) || Record.PeriodSeconds < 0.0)
			{
				Add(TEXT("period_invalid"), TEXT("error"), Record, Record.StableId,
					TEXT("GameplayEffect period must be finite and non-negative."));
			}
			if (Record.DurationPolicy == TEXT("instant") && Record.PeriodSeconds > 0.0)
			{
				Add(TEXT("instant_effect_has_period"), TEXT("error"), Record, Record.StableId,
					TEXT("Instant GameplayEffects cannot safely retain periodic execution data."));
			}
			for (const FHyperAIGASModifierView& Modifier : Record.Modifiers)
			{
				if (Modifier.AttributeOwnerClassPath.IsEmpty() || Modifier.AttributeName.IsEmpty())
				{
					Add(TEXT("modifier_attribute_missing"), TEXT("error"), Record, Modifier.StableId,
						TEXT("GameplayEffect modifier does not resolve to a valid AttributeSet property."));
				}
				if (Modifier.MagnitudeKind == TEXT("scalable_float")
					&& (!Modifier.bHasStaticMagnitude || !IsFinite(Modifier.StaticMagnitude)))
				{
					Add(TEXT("modifier_static_magnitude_invalid"), TEXT("error"), Record, Modifier.StableId,
						TEXT("Supported constant modifier magnitude is not finite or independently readable."));
				}
				if (Modifier.Operation == TEXT("division") && Modifier.bHasStaticMagnitude
					&& FMath::IsNearlyZero(Modifier.StaticMagnitude))
				{
					Add(TEXT("modifier_division_by_zero"), TEXT("error"), Record, Modifier.StableId,
						TEXT("A division modifier cannot use zero magnitude."));
				}
			}
			for (const FHyperAIGASCueView& Cue : Record.Cues)
			{
				if (Cue.CueTags.IsEmpty())
				{
					Add(TEXT("cue_tags_empty"), TEXT("error"), Record, Cue.StableId,
						TEXT("GameplayEffect cue reference must contain at least one registered cue tag."));
				}
					if (!IsFinite(Cue.MinLevel) || !IsFinite(Cue.MaxLevel) || Cue.MinLevel > Cue.MaxLevel)
					{
						Add(TEXT("cue_level_range_invalid"), TEXT("error"), Record, Cue.StableId,
							TEXT("GameplayEffect cue level bounds must be finite and ordered."));
					}
					for (const FString& CueTag : Cue.CueTags)
					{
						if (!CueTag.StartsWith(TEXT("GameplayCue."), ESearchCase::CaseSensitive)
							|| !UGameplayTagsManager::Get().RequestGameplayTag(FName(*CueTag), false).IsValid())
						{
							Add(TEXT("cue_tag_invalid"), TEXT("error"), Record, Cue.StableId,
								TEXT("GameplayEffect cue references must remain registered GameplayCue tags."));
						}
					}
				}
		}
		else if (Record.Variant.StartsWith(TEXT("attribute_set"), ESearchCase::CaseSensitive)
			&& Record.Attributes.IsEmpty())
			{
				Add(TEXT("attribute_set_empty"), TEXT("warning"), Record, Record.StableId,
					TEXT("AttributeSet declares no supported gameplay attributes in this projection."));
			}
			for (const FHyperAIGASAttributeView& Attribute : Record.Attributes)
			{
				if (Attribute.OwnerClassPath.IsEmpty() || Attribute.Name.IsEmpty())
				{
					Add(TEXT("attribute_identity_missing"), TEXT("error"), Record, Attribute.StableId,
						TEXT("AttributeSet projection contains an attribute without a stable owner/name identity."));
				}
				if (Attribute.bHasDefaultValue && !IsFinite(Attribute.DefaultValue))
				{
					Add(TEXT("attribute_default_non_finite"), TEXT("error"), Record, Attribute.StableId,
						TEXT("AttributeSet default values must be finite when independently readable."));
				}
			}
	}
	return Issues;
}

FHyperAIGASInspectReport UHyperAIStudioGASToolset::hyper_gas_inspect(
	const FHyperAIGASInspectRequest& Request)
{
	using namespace HyperAIStudio::GAS::Private;
	FHyperAIGASInspectReport Report;
	Report.EpicDelegates = FHyperAIStudioGASContracts::GetEpicDelegates();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Loaded Unreal GAS objects may only be inspected on the serialized game thread.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioGASContracts::MaxAssetPaths
		|| !IsSupportedVariantFilter(Request.Variant)
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioGASContracts::MaxPageSize
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > FHyperAIStudioGASContracts::MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioGASContracts::MaxOutputBytes
		|| Request.Cursor.Len() > FHyperAIStudioGASContracts::MaxCursorCharacters)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("GAS inspect request violates variant, path, page, cursor, or output bounds.");
		return Report;
	}
	TSet<FString> UniquePaths;
	for (const FString& Path : Request.AssetPaths)
	{
		if (!FHyperAIStudioGASContracts::IsCanonicalProjectObjectPath(Path) || UniquePaths.Contains(Path))
		{
			Report.Status = TEXT("invalid_asset_path");
			Report.Diagnostic = TEXT("Asset paths must be unique canonical /Game Blueprint object paths.");
			return Report;
		}
		UniquePaths.Add(Path);
	}

	FHyperAIStudioGASValueSnapshot Snapshot = CaptureSnapshot(Request);
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
	Report.TotalRecords = Snapshot.Records.Num();
	int32 UsedBytes = 8192;
	for (const FHyperAIGASEpicDelegate& Delegate : Report.EpicDelegates)
	{
		UsedBytes += 512 + (Delegate.Capability.Len() + Delegate.Toolset.Len()
			+ Delegate.Tool.Len() + Delegate.Safety.Len() + Delegate.Reason.Len())
			* MaxUtf8BytesPerCharacter;
	}
	if (UsedBytes > Request.MaxOutputBytes)
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("Output budget cannot hold the fixed bounded GAS delegation envelope.");
		return Report;
	}
	for (const FHyperAIGASIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 IssueBytes = 512 + (Issue.Code.Len() + Issue.Severity.Len()
			+ Issue.AssetPath.Len() + Issue.StableId.Len() + Issue.Message.Len())
			* MaxUtf8BytesPerCharacter;
		if (UsedBytes + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		UsedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	for (const FHyperAIGASAssetRecord& Record : Snapshot.Records)
	{
		if (Record.Variant == TEXT("gameplay_ability_blueprint")) ++Report.AbilityCount;
		else if (Record.Variant == TEXT("gameplay_effect_blueprint")) ++Report.EffectCount;
		else if (Record.Variant.StartsWith(TEXT("attribute_set"))) ++Report.AttributeSetCount;
	}

	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Report.Revision, Offset) || Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("stale_or_invalid_cursor");
		Report.Diagnostic = TEXT("Cursor must bind exactly to the current projection revision and a valid offset.");
		return Report;
	}
	for (int32 Index = Offset; Index < Snapshot.Records.Num()
		&& Report.Records.Num() < Request.PageSize; ++Index)
	{
		const int32 RecordBytes = ApproximateRecordBytes(Snapshot.Records[Index]);
		if (UsedBytes + RecordBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		UsedBytes += RecordBytes;
		Report.Records.Add(Snapshot.Records[Index]);
	}
	Report.ReturnedRecords = Report.Records.Num();
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	if (NextOffset < Snapshot.Records.Num())
	{
		Report.bTruncated = true;
		Report.NextCursor = MakeCursor(Report.Revision, NextOffset);
	}
	if (Report.ReturnedRecords == 0 && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("Output budget cannot hold the next complete GAS record.");
		return Report;
	}
	Report.bOk = FHyperAIStudioGASContracts::IsCanonicalSha256(Report.Revision);
	Report.Status = Snapshot.bComplete && Report.Issues.IsEmpty() ? TEXT("complete") : TEXT("partial");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a stable bounded projection of already-loaded GAS assets and explicit Epic delegate routes.")
		: TEXT("Returned bounded loaded-only GAS evidence; revision completeness or requested assets were partial.");
	return Report;
}

FHyperAIGASValidateReport UHyperAIStudioGASToolset::hyper_gas_validate(
	const FHyperAIGASValidateRequest& Request)
{
	using namespace HyperAIStudio::GAS::Private;
	FHyperAIGASValidateReport Report;
	Report.EpicDelegates = FHyperAIStudioGASContracts::GetEpicDelegates();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Fresh Unreal GAS validation requires the serialized game thread.");
		return Report;
	}
	if (!Request.bRequireFreshCapture)
	{
		Report.Status = TEXT("fresh_capture_required");
		Report.Diagnostic = TEXT("GAS validation never accepts planner simulation or a stale capture in place of fresh loaded state.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioGASContracts::MaxAssetPaths
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioGASContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1
		|| Request.MaxGameThreadMs > FHyperAIStudioGASContracts::MaxReadGameThreadMs
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioGASContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("GAS validation request violates path, issue, or revision bounds.");
		return Report;
	}
	TSet<FString> UniquePaths;
	for (const FString& Path : Request.AssetPaths)
	{
		if (!FHyperAIStudioGASContracts::IsCanonicalProjectObjectPath(Path) || UniquePaths.Contains(Path))
		{
			Report.Status = TEXT("invalid_asset_path");
			Report.Diagnostic = TEXT("Validation paths must be unique canonical /Game Blueprint object paths.");
			return Report;
		}
		UniquePaths.Add(Path);
	}

	FHyperAIGASInspectRequest CaptureRequest;
	CaptureRequest.AssetPaths = Request.AssetPaths;
	CaptureRequest.Variant = TEXT("all");
	CaptureRequest.bIncludeTags = true;
	CaptureRequest.bIncludeModifiers = true;
	CaptureRequest.bIncludeCues = true;
	CaptureRequest.bIncludeAttributes = true;
	CaptureRequest.MaxGameThreadMs = Request.MaxGameThreadMs;
	CaptureRequest.PageSize = FHyperAIStudioGASContracts::MaxPageSize;
	CaptureRequest.MaxOutputBytes = FHyperAIStudioGASContracts::MaxOutputBytes;
	FHyperAIStudioGASValueSnapshot Snapshot = CaptureSnapshot(CaptureRequest);
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.Issues = FHyperAIStudioGASContracts::ValidateValueSnapshot(
		Snapshot, Request.bRequirePackagesClean, Request.MaxIssues, Report.bTruncated);
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Report.Revision)
	{
		AddIssue(Report.Issues, Request.MaxIssues, Report.bTruncated,
			TEXT("expected_revision_mismatch"), TEXT("error"), FString(), TEXT("gas:snapshot"), -1,
			TEXT("Fresh loaded-state revision no longer matches the caller's expected revision."));
	}
	for (const FHyperAIGASIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	Report.bOk = FHyperAIStudioGASContracts::IsCanonicalSha256(Report.Revision);
	Report.bValid = Report.bOk && Report.bRevisionComplete && Report.ErrorCount == 0;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("Independent fresh loaded-state GAS validation passed.")
		: TEXT("Independent GAS validation found incomplete evidence, stale state, or correctness errors.");
	return Report;
}

namespace HyperAIStudio::GAS::Private
{
	bool IsUnusedDurationFields(const FHyperAIGASPlanOperation& Operation)
	{
		return Operation.DurationPolicy.IsEmpty() && Operation.DurationSeconds == -1.0
			&& Operation.MaxDurationSeconds == -1.0 && Operation.PeriodSeconds == -1.0
			&& !Operation.bSetExecutePeriodicEffectOnApplication
			&& !Operation.bExecutePeriodicEffectOnApplication;
	}

	bool IsUnusedCueFlagFields(const FHyperAIGASPlanOperation& Operation)
	{
		return !Operation.bSetRequireModifierSuccessToTriggerCues
			&& !Operation.bRequireModifierSuccessToTriggerCues
			&& !Operation.bSetSuppressStackingCues && !Operation.bSuppressStackingCues;
	}

	bool ValidateTagList(
		const TArray<FString>& Tags,
		const int32 Maximum,
		const bool bRequireGameplayCue,
		FString& OutError)
	{
		OutError.Reset();
		if (Tags.Num() > Maximum)
		{
			OutError = TEXT("Tag list exceeds its hard bound.");
			return false;
		}
		TSet<FString> Unique;
		for (const FString& TagName : Tags)
		{
			if (TagName.IsEmpty() || TagName.Len() > FHyperAIStudioGASContracts::MaxTagCharacters
				|| HasEmbeddedNull(TagName) || TagName.TrimStartAndEnd() != TagName
				|| Unique.Contains(TagName)
				|| (bRequireGameplayCue
					&& !TagName.StartsWith(TEXT("GameplayCue."), ESearchCase::CaseSensitive)))
			{
				OutError = TEXT("Tags must be unique bounded canonical names in the required namespace.");
				return false;
			}
			const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
				FName(*TagName), false);
			if (!Tag.IsValid())
			{
				OutError = TEXT("Tag is not already registered; this pack never creates Gameplay Tags.");
				return false;
			}
			Unique.Add(TagName);
		}
		return true;
	}

	bool ValidateOperationEnvelopeBounds(
		const FHyperAIGASPlanOperation& Operation,
		int32& OutBytes,
		FString& OutError)
	{
		OutBytes = 0;
		OutError.Reset();
		if (Operation.Type.IsEmpty() || Operation.Type.Len() > 64
			|| Operation.TargetPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
			|| Operation.ExpectedRevision.Len() > 71 || Operation.DurationPolicy.Len() > 32
			|| Operation.ParentClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
			|| Operation.Tags.Num() > FHyperAIStudioGASContracts::MaxTagsPerSet
			|| Operation.Modifiers.Num() > FHyperAIStudioGASContracts::MaxModifiers
			|| Operation.Cues.Num() > FHyperAIStudioGASContracts::MaxCues
			|| HasEmbeddedNull(Operation.Type) || HasEmbeddedNull(Operation.TargetPath)
			|| HasEmbeddedNull(Operation.ExpectedRevision) || HasEmbeddedNull(Operation.DurationPolicy)
			|| HasEmbeddedNull(Operation.ParentClassPath))
		{
			OutError = TEXT("Operation identity, discriminant, or top-level arrays exceed their hard bounds.");
			return false;
		}

		int64 Characters = Operation.Type.Len() + Operation.TargetPath.Len()
			+ Operation.ExpectedRevision.Len() + Operation.DurationPolicy.Len()
			+ Operation.ParentClassPath.Len();
		for (const FString& Tag : Operation.Tags)
		{
			if (Tag.Len() > FHyperAIStudioGASContracts::MaxTagCharacters || HasEmbeddedNull(Tag))
			{
				OutError = TEXT("Operation tag exceeds its hard character bound.");
				return false;
			}
			Characters += Tag.Len();
		}
		for (const FHyperAIGASModifierSpec& Modifier : Operation.Modifiers)
		{
			if (Modifier.AttributeOwnerClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
				|| Modifier.AttributeName.Len() > 128 || Modifier.Operation.Len() > 32
				|| HasEmbeddedNull(Modifier.AttributeOwnerClassPath)
				|| HasEmbeddedNull(Modifier.AttributeName) || HasEmbeddedNull(Modifier.Operation))
			{
				OutError = TEXT("Operation modifier identity exceeds its hard bounds.");
				return false;
			}
			Characters += Modifier.AttributeOwnerClassPath.Len()
				+ Modifier.AttributeName.Len() + Modifier.Operation.Len();
		}
		for (const FHyperAIGASCueSpec& Cue : Operation.Cues)
		{
			if (Cue.CueTags.Num() > FHyperAIStudioGASContracts::MaxCueTags
				|| Cue.MagnitudeAttributeOwnerClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
				|| Cue.MagnitudeAttributeName.Len() > 128
				|| HasEmbeddedNull(Cue.MagnitudeAttributeOwnerClassPath)
				|| HasEmbeddedNull(Cue.MagnitudeAttributeName))
			{
				OutError = TEXT("Operation cue identity exceeds its hard bounds.");
				return false;
			}
			Characters += Cue.MagnitudeAttributeOwnerClassPath.Len()
				+ Cue.MagnitudeAttributeName.Len();
			for (const FString& Tag : Cue.CueTags)
			{
				if (Tag.Len() > FHyperAIStudioGASContracts::MaxTagCharacters || HasEmbeddedNull(Tag))
				{
					OutError = TEXT("Operation cue tag exceeds its hard character bound.");
					return false;
				}
				Characters += Tag.Len();
			}
		}
		const int64 Bytes = 1024ll + Characters * MaxUtf8BytesPerCharacter
			+ static_cast<int64>(Operation.Tags.Num()) * 64
			+ static_cast<int64>(Operation.Modifiers.Num()) * 256
			+ static_cast<int64>(Operation.Cues.Num()) * 256;
		if (Bytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
		{
			OutError = TEXT("Operation exceeds the shared typed-request byte envelope.");
			return false;
		}
		OutBytes = static_cast<int32>(Bytes);
		return true;
	}

	bool ResolveAttribute(
		const FString& OwnerClassPath,
		const FString& AttributeName,
		FGameplayAttribute& OutAttribute,
		FString& OutError)
	{
		OutAttribute = FGameplayAttribute();
		OutError.Reset();
		if (OwnerClassPath.IsEmpty() || OwnerClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
			|| AttributeName.IsEmpty() || AttributeName.Len() > 128
				|| HasEmbeddedNull(OwnerClassPath) || HasEmbeddedNull(AttributeName))
		{
			OutError = TEXT("Attribute class path or name is malformed.");
			return false;
		}
		UClass* OwnerClass = FSoftClassPath(OwnerClassPath).ResolveClass();
		if (!OwnerClass || !OwnerClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			OutError = TEXT("Attribute owner must be an already-loaded UAttributeSet class.");
			return false;
		}
		FProperty* Property = FindFProperty<FProperty>(OwnerClass, FName(*AttributeName));
		if (!Property || Property->GetOwnerClass() != OwnerClass
			|| !FGameplayAttribute::IsSupportedProperty(Property))
		{
			OutError = TEXT("Attribute must resolve to a supported property declared by the exact loaded owner class.");
			return false;
		}
		OutAttribute = FGameplayAttribute(Property);
		if (!OutAttribute.IsValid())
		{
			OutError = TEXT("Resolved Gameplay Attribute is invalid.");
			return false;
		}
		return true;
	}

	FString ModifierSpecCanonical(const FHyperAIGASModifierSpec& Spec)
	{
		FString Canonical;
		AppendToken(Canonical, Spec.AttributeOwnerClassPath);
		AppendToken(Canonical, Spec.AttributeName);
		AppendToken(Canonical, Spec.Operation);
		AppendDouble(Canonical, Spec.Magnitude);
		return Canonical;
	}

	FString CueSpecCanonical(const FHyperAIGASCueSpec& Spec)
	{
		FString Canonical;
		AppendStrings(Canonical, Spec.CueTags);
		AppendDouble(Canonical, Spec.MinLevel);
		AppendDouble(Canonical, Spec.MaxLevel);
		AppendToken(Canonical, Spec.MagnitudeAttributeOwnerClassPath);
		AppendToken(Canonical, Spec.MagnitudeAttributeName);
		return Canonical;
	}

	FString BackendOperationCanonical(const FHyperAIStudioGASBackendOperation& Operation)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.gas-operation.v1"));
		AppendInt(Canonical, static_cast<int32>(Operation.Kind));
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.DurationPolicy);
		AppendDouble(Canonical, Operation.DurationSeconds);
		AppendDouble(Canonical, Operation.MaxDurationSeconds);
		AppendDouble(Canonical, Operation.PeriodSeconds);
		AppendBool(Canonical, Operation.bSetExecutePeriodicEffectOnApplication);
		AppendBool(Canonical, Operation.bExecutePeriodicEffectOnApplication);
		AppendBool(Canonical, Operation.bSetRequireModifierSuccessToTriggerCues);
		AppendBool(Canonical, Operation.bRequireModifierSuccessToTriggerCues);
		AppendBool(Canonical, Operation.bSetSuppressStackingCues);
		AppendBool(Canonical, Operation.bSuppressStackingCues);
		AppendStrings(Canonical, Operation.Tags);
		AppendInt(Canonical, Operation.Modifiers.Num());
		for (const FHyperAIGASModifierSpec& Modifier : Operation.Modifiers)
			AppendToken(Canonical, ModifierSpecCanonical(Modifier));
		AppendInt(Canonical, Operation.Cues.Num());
		for (const FHyperAIGASCueSpec& Cue : Operation.Cues)
			AppendToken(Canonical, CueSpecCanonical(Cue));
		AppendToken(Canonical, Operation.ParentClassPath);
		AppendBool(Canonical, Operation.bBackendSupported);
		return Canonical;
	}

	FString OperationCategory(const EHyperAIStudioGASOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioGASOperationKind::SetEffectDuration: return TEXT("duration");
		case EHyperAIStudioGASOperationKind::ReplaceEffectModifiers: return TEXT("modifiers");
		case EHyperAIStudioGASOperationKind::ReplaceEffectAssetTags: return TEXT("effect_asset_tags");
		case EHyperAIStudioGASOperationKind::ReplaceEffectGrantedTags: return TEXT("effect_granted_tags");
		case EHyperAIStudioGASOperationKind::ReplaceEffectCues: return TEXT("cues");
		case EHyperAIStudioGASOperationKind::SetEffectCueFlags: return TEXT("cue_flags");
		case EHyperAIStudioGASOperationKind::SetAbilityAssetTags: return TEXT("ability_asset_tags");
		default: return TEXT("unsupported");
		}
	}

	bool RequiresEffect(const EHyperAIStudioGASOperationKind Kind)
	{
		return Kind == EHyperAIStudioGASOperationKind::SetEffectDuration
			|| Kind == EHyperAIStudioGASOperationKind::ReplaceEffectModifiers
			|| Kind == EHyperAIStudioGASOperationKind::ReplaceEffectAssetTags
			|| Kind == EHyperAIStudioGASOperationKind::ReplaceEffectGrantedTags
			|| Kind == EHyperAIStudioGASOperationKind::ReplaceEffectCues
			|| Kind == EHyperAIStudioGASOperationKind::SetEffectCueFlags;
	}

	FHyperAIGASModifierView ModifierViewFromSpec(const FHyperAIGASModifierSpec& Spec, const int32 Index)
	{
		FHyperAIGASModifierView View;
		View.StableId = FString::Printf(TEXT("modifier:%04d:%s:%s"), Index,
			*Spec.AttributeOwnerClassPath, *Spec.AttributeName);
		View.AttributeOwnerClassPath = Spec.AttributeOwnerClassPath;
		View.AttributeName = Spec.AttributeName;
		View.Operation = Spec.Operation;
		View.MagnitudeKind = TEXT("scalable_float");
		View.bHasStaticMagnitude = true;
		View.StaticMagnitude = Spec.Magnitude;
		return View;
	}

	FHyperAIGASCueView CueViewFromSpec(const FHyperAIGASCueSpec& Spec, const int32 Index)
	{
		FHyperAIGASCueView View;
		View.StableId = FString::Printf(TEXT("cue:%04d"), Index);
		View.CueTags = Spec.CueTags;
		View.CueTags.Sort();
		View.MinLevel = Spec.MinLevel;
		View.MaxLevel = Spec.MaxLevel;
		View.MagnitudeAttributeOwnerClassPath = Spec.MagnitudeAttributeOwnerClassPath;
		View.MagnitudeAttributeName = Spec.MagnitudeAttributeName;
		return View;
	}
}

bool FHyperAIStudioGASContracts::ValidateOperationShape(
	const FHyperAIGASPlanOperation& Operation,
	FHyperAIStudioGASBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::GAS::Private;
	OutOperation = FHyperAIStudioGASBackendOperation{};
	OutErrorCode.Reset();
	OutError.Reset();
	auto Fail = [&](const TCHAR* Code, const TCHAR* Message)
	{
		OutErrorCode = Code;
		OutError = Message;
		return false;
	};
	int32 BoundedOperationBytes = 0;
	FString EnvelopeError;
	if (!ValidateOperationEnvelopeBounds(Operation, BoundedOperationBytes, EnvelopeError))
	{
		return Fail(TEXT("invalid_operation_bounds"), *EnvelopeError);
	}
	if (!IsCanonicalProjectObjectPath(Operation.TargetPath))
		return Fail(TEXT("invalid_target_path"), TEXT("Target must be a canonical /Game Blueprint object path."));
	if ((!Operation.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Operation.ExpectedRevision))
		|| Operation.ParentClassPath.Len() > MaxPathCharacters
		|| HasEmbeddedNull(Operation.ParentClassPath))
		return Fail(TEXT("invalid_operation_identity"), TEXT("Expected revision or parent class identity is malformed."));

	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.DurationPolicy = Operation.DurationPolicy;
	OutOperation.DurationSeconds = Operation.DurationSeconds;
	OutOperation.MaxDurationSeconds = Operation.MaxDurationSeconds;
	OutOperation.PeriodSeconds = Operation.PeriodSeconds;
	OutOperation.bSetExecutePeriodicEffectOnApplication = Operation.bSetExecutePeriodicEffectOnApplication;
	OutOperation.bExecutePeriodicEffectOnApplication = Operation.bExecutePeriodicEffectOnApplication;
	OutOperation.bSetRequireModifierSuccessToTriggerCues = Operation.bSetRequireModifierSuccessToTriggerCues;
	OutOperation.bRequireModifierSuccessToTriggerCues = Operation.bRequireModifierSuccessToTriggerCues;
	OutOperation.bSetSuppressStackingCues = Operation.bSetSuppressStackingCues;
	OutOperation.bSuppressStackingCues = Operation.bSuppressStackingCues;
	OutOperation.Tags = Operation.Tags;
	OutOperation.Modifiers = Operation.Modifiers;
	OutOperation.Cues = Operation.Cues;
	OutOperation.ParentClassPath = Operation.ParentClassPath;

	if (Operation.Type == TEXT("set_effect_duration"))
	{
		OutOperation.Kind = EHyperAIStudioGASOperationKind::SetEffectDuration;
		OutOperation.bBackendSupported = true;
		EGameplayEffectDurationType Policy;
		if (!ParseDurationPolicy(Operation.DurationPolicy, Policy)
			|| Operation.PeriodSeconds < 0.0 || !IsFinite(Operation.PeriodSeconds)
			|| Operation.PeriodSeconds > MaxAbsoluteMagnitude
			|| !Operation.Tags.IsEmpty() || !Operation.Modifiers.IsEmpty() || !Operation.Cues.IsEmpty()
				|| !Operation.ParentClassPath.IsEmpty() || !IsUnusedCueFlagFields(Operation)
				|| (!Operation.bSetExecutePeriodicEffectOnApplication
					&& Operation.bExecutePeriodicEffectOnApplication))
			return Fail(TEXT("invalid_duration_shape"), TEXT("set_effect_duration has invalid or unrelated fields."));
		if (Policy == EGameplayEffectDurationType::HasDuration)
		{
			if (!IsFinite(Operation.DurationSeconds) || Operation.DurationSeconds <= 0.0
				|| Operation.DurationSeconds > MaxAbsoluteMagnitude
				|| !IsFinite(Operation.MaxDurationSeconds) || Operation.MaxDurationSeconds < 0.0
				|| Operation.MaxDurationSeconds > MaxAbsoluteMagnitude)
				return Fail(TEXT("invalid_duration_values"), TEXT("Duration requires positive seconds and non-negative max duration."));
		}
		else if (Operation.DurationSeconds != -1.0 || Operation.MaxDurationSeconds != -1.0
			|| (Policy == EGameplayEffectDurationType::Instant && Operation.PeriodSeconds != 0.0))
		{
			return Fail(TEXT("unused_duration_values"), TEXT("Instant/infinite policies reject duration-only values; instant also requires zero period."));
		}
	}
	else if (Operation.Type == TEXT("replace_effect_modifiers"))
	{
		OutOperation.Kind = EHyperAIStudioGASOperationKind::ReplaceEffectModifiers;
		OutOperation.bBackendSupported = true;
		if (!IsUnusedDurationFields(Operation) || !IsUnusedCueFlagFields(Operation)
			|| !Operation.Tags.IsEmpty() || !Operation.Cues.IsEmpty() || !Operation.ParentClassPath.IsEmpty()
			|| Operation.Modifiers.Num() > MaxModifiers)
			return Fail(TEXT("invalid_modifier_shape"), TEXT("replace_effect_modifiers has invalid, unrelated, or oversized fields."));
		TSet<FString> UniqueModifiers;
		for (const FHyperAIGASModifierSpec& Modifier : Operation.Modifiers)
		{
			EGameplayModOp::Type ParsedOperation;
			if (!ParseModifierOperation(Modifier.Operation, ParsedOperation)
				|| !IsFinite(Modifier.Magnitude) || FMath::Abs(Modifier.Magnitude) > MaxAbsoluteMagnitude
				|| (ParsedOperation == EGameplayModOp::Division && FMath::IsNearlyZero(Modifier.Magnitude))
				|| Modifier.AttributeOwnerClassPath.IsEmpty()
				|| Modifier.AttributeOwnerClassPath.Len() > MaxPathCharacters
				|| Modifier.AttributeName.IsEmpty() || Modifier.AttributeName.Len() > 128
				|| HasEmbeddedNull(Modifier.AttributeOwnerClassPath)
				|| HasEmbeddedNull(Modifier.AttributeName))
				return Fail(TEXT("invalid_modifier_spec"), TEXT("Modifier must use a loaded attribute, closed operation, and bounded finite constant magnitude."));
			const FString Key = Modifier.AttributeOwnerClassPath + TEXT("\n") + Modifier.AttributeName
				+ TEXT("\n") + Modifier.Operation;
			if (UniqueModifiers.Contains(Key))
				return Fail(TEXT("duplicate_modifier_spec"), TEXT("Duplicate attribute/operation modifier entries are rejected."));
			UniqueModifiers.Add(Key);
		}
	}
	else if (Operation.Type == TEXT("replace_effect_asset_tags")
		|| Operation.Type == TEXT("replace_effect_granted_tags"))
	{
		OutOperation.Kind = Operation.Type == TEXT("replace_effect_asset_tags")
			? EHyperAIStudioGASOperationKind::ReplaceEffectAssetTags
			: EHyperAIStudioGASOperationKind::ReplaceEffectGrantedTags;
		OutOperation.bBackendSupported = false;
		FString TagError;
		if (!IsUnusedDurationFields(Operation) || !IsUnusedCueFlagFields(Operation)
			|| !Operation.Modifiers.IsEmpty() || !Operation.Cues.IsEmpty() || !Operation.ParentClassPath.IsEmpty()
			|| !ValidateTagList(Operation.Tags, MaxTagsPerSet, false, TagError))
			return Fail(TEXT("invalid_tag_shape"), *TagError);
	}
	else if (Operation.Type == TEXT("replace_effect_cues"))
	{
		OutOperation.Kind = EHyperAIStudioGASOperationKind::ReplaceEffectCues;
		OutOperation.bBackendSupported = true;
		if (!IsUnusedDurationFields(Operation) || !IsUnusedCueFlagFields(Operation)
			|| !Operation.Tags.IsEmpty() || !Operation.Modifiers.IsEmpty()
				|| !Operation.ParentClassPath.IsEmpty() || Operation.Cues.Num() > MaxCues)
			return Fail(TEXT("invalid_cue_shape"), TEXT("replace_effect_cues has invalid, unrelated, or oversized fields."));
		TSet<FString> UniqueCues;
		TSet<FString> UniqueCueTags;
		for (const FHyperAIGASCueSpec& Cue : Operation.Cues)
		{
			FString TagError;
			const bool bHasOwner = !Cue.MagnitudeAttributeOwnerClassPath.IsEmpty();
			const bool bHasName = !Cue.MagnitudeAttributeName.IsEmpty();
			if (!ValidateTagList(Cue.CueTags, MaxCueTags, true, TagError)
				|| Cue.CueTags.IsEmpty() || !IsFinite(Cue.MinLevel) || !IsFinite(Cue.MaxLevel)
				|| FMath::Abs(Cue.MinLevel) > MaxAbsoluteMagnitude
				|| FMath::Abs(Cue.MaxLevel) > MaxAbsoluteMagnitude
				|| Cue.MinLevel > Cue.MaxLevel || bHasOwner != bHasName
				|| Cue.MagnitudeAttributeOwnerClassPath.Len() > MaxPathCharacters
				|| Cue.MagnitudeAttributeName.Len() > 128
				|| HasEmbeddedNull(Cue.MagnitudeAttributeOwnerClassPath)
				|| HasEmbeddedNull(Cue.MagnitudeAttributeName))
				return Fail(TEXT("invalid_cue_spec"), TagError.IsEmpty()
					? TEXT("Cue needs registered GameplayCue tags, ordered finite levels, and a complete optional attribute.") : *TagError);
			TArray<FString> SortedTags = Cue.CueTags;
			SortedTags.Sort();
			const FString Key = FString::Join(SortedTags, TEXT("|"));
			if (UniqueCues.Contains(Key))
				return Fail(TEXT("duplicate_cue_spec"), TEXT("Duplicate cue-tag groups are rejected."));
			UniqueCues.Add(Key);
			for (const FString& Tag : SortedTags)
			{
				if (UniqueCueTags.Contains(Tag))
					return Fail(TEXT("duplicate_cue_tag"), TEXT("A cue tag may appear in only one cue entry."));
				UniqueCueTags.Add(Tag);
			}
		}
	}
	else if (Operation.Type == TEXT("set_effect_cue_flags"))
	{
		OutOperation.Kind = EHyperAIStudioGASOperationKind::SetEffectCueFlags;
		OutOperation.bBackendSupported = true;
		if (!IsUnusedDurationFields(Operation) || !Operation.Tags.IsEmpty()
			|| !Operation.Modifiers.IsEmpty() || !Operation.Cues.IsEmpty()
			|| !Operation.ParentClassPath.IsEmpty()
				|| (!Operation.bSetRequireModifierSuccessToTriggerCues
					&& !Operation.bSetSuppressStackingCues)
				|| (!Operation.bSetRequireModifierSuccessToTriggerCues
					&& Operation.bRequireModifierSuccessToTriggerCues)
				|| (!Operation.bSetSuppressStackingCues && Operation.bSuppressStackingCues))
			return Fail(TEXT("invalid_cue_flag_shape"), TEXT("set_effect_cue_flags requires at least one explicit flag and rejects unrelated fields."));
	}
	else if (Operation.Type == TEXT("set_ability_asset_tags"))
	{
		OutOperation.Kind = EHyperAIStudioGASOperationKind::SetAbilityAssetTags;
		OutOperation.bBackendSupported = true;
		FString TagError;
		if (!IsUnusedDurationFields(Operation) || !IsUnusedCueFlagFields(Operation)
			|| !Operation.Modifiers.IsEmpty() || !Operation.Cues.IsEmpty()
			|| !Operation.ParentClassPath.IsEmpty()
			|| !ValidateTagList(Operation.Tags, MaxTagsPerSet, false, TagError))
			return Fail(TEXT("invalid_ability_tag_shape"), *TagError);
	}
	else if (Operation.Type == TEXT("create_gameplay_ability_blueprint")
		|| Operation.Type == TEXT("create_gameplay_effect_blueprint")
		|| Operation.Type == TEXT("create_attribute_set_blueprint")
		|| Operation.Type == TEXT("edit_attribute_set_blueprint"))
	{
		if (Operation.Type == TEXT("create_gameplay_ability_blueprint"))
			OutOperation.Kind = EHyperAIStudioGASOperationKind::CreateGameplayAbilityBlueprint;
		else if (Operation.Type == TEXT("create_gameplay_effect_blueprint"))
			OutOperation.Kind = EHyperAIStudioGASOperationKind::CreateGameplayEffectBlueprint;
		else if (Operation.Type == TEXT("create_attribute_set_blueprint"))
			OutOperation.Kind = EHyperAIStudioGASOperationKind::CreateAttributeSetBlueprint;
		else OutOperation.Kind = EHyperAIStudioGASOperationKind::EditAttributeSetBlueprint;
		OutOperation.bBackendSupported = false;
		if (!IsUnusedDurationFields(Operation) || !IsUnusedCueFlagFields(Operation)
			|| !Operation.Tags.IsEmpty() || !Operation.Modifiers.IsEmpty() || !Operation.Cues.IsEmpty())
			return Fail(TEXT("invalid_unavailable_variant_shape"), TEXT("Unavailable creation/AttributeSet variants reject unrelated mutation fields."));
	}
	else
	{
		return Fail(TEXT("unknown_operation_type"), TEXT("Operation type is not in the closed GAS variant set."));
	}

	if (OutOperation.bBackendSupported && !IsCanonicalSha256(Operation.ExpectedRevision))
		return Fail(TEXT("revision_required"), TEXT("Every supported GAS edit requires an exact per-asset revision."));
	return true;
}

FString FHyperAIStudioGASContracts::ComputePayloadSemanticFingerprint(
	const TArray<FHyperAIStudioGASBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::GAS::Private;
	if (!IsCanonicalSha256(BaseRevision) || Operations.IsEmpty() || Operations.Num() > MaxOperations)
		return FString();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.gas-payload.v1"));
	AppendToken(Canonical, BaseRevision);
	AppendInt(Canonical, Operations.Num());
	for (const FHyperAIStudioGASBackendOperation& Operation : Operations)
	{
		AppendToken(Canonical, BackendOperationCanonical(Operation));
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAIGASApplyPlanReport FHyperAIStudioGASContracts::BuildPlan(
	const FHyperAIGASApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::GAS::Private;
	FHyperAIGASApplyPlanReport Report;
	const double StartedSeconds = FPlatformTime::Seconds();
	auto Finish = [&]()
	{
		Report.GameThreadMs = FMath::Max(0, FMath::RoundToInt(
			(FPlatformTime::Seconds() - StartedSeconds) * 1000.0));
		return Report;
	};
	Report.bDryRun = Request.bDryRun;
	Report.EpicDelegates = GetEpicDelegates();
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("GAS plan capture and simulation require the serialized game thread.");
		return Finish();
	}
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 2000
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("GAS plan violates operation, deadline, game-thread, or output bounds.");
		return Finish();
	}
	if (Request.OperationId.Len() > FHyperAIStudioDomainLimits::MaxOperationIdChars
		|| Request.ExpectedPlanHash.Len() > 71
		|| HasEmbeddedNull(Request.OperationId) || HasEmbeddedNull(Request.ExpectedPlanHash))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("GAS plan identity fields exceed their hard character bounds.");
		return Finish();
	}
	int64 ApproxRequestBytes = 512ll
		+ static_cast<int64>(Request.OperationId.Len() + Request.ExpectedPlanHash.Len())
			* MaxUtf8BytesPerCharacter;
	for (const FHyperAIGASPlanOperation& Operation : Request.Operations)
	{
		int32 OperationBytes = 0;
		FString BoundsError;
		if (!ValidateOperationEnvelopeBounds(Operation, OperationBytes, BoundsError))
		{
			Report.Status = TEXT("invalid_request_bounds");
			Report.Diagnostic = BoundsError;
			return Finish();
		}
		ApproxRequestBytes += OperationBytes;
		if (ApproxRequestBytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
		{
			Report.Status = TEXT("invalid_request_bounds");
			Report.Diagnostic = TEXT("GAS plan exceeds the shared typed-request byte envelope.");
			return Finish();
		}
	}
	Report.OperationId = Request.OperationId;
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		Report.Status = TEXT("invalid_dry_run_identity");
		Report.Diagnostic = TEXT("Dry runs reject execution-only operation_id and expected_plan_hash fields.");
		return Finish();
	}
	auto BudgetExceeded = [&]()
	{
		const int32 ElapsedMs = FMath::Max(0, FMath::RoundToInt(
			(FPlatformTime::Seconds() - StartedSeconds) * 1000.0));
		return ElapsedMs > Request.DeadlineMs || ElapsedMs > Request.MaxGameThreadMs;
	};
	auto FailBudget = [&]()
	{
		Report.bOk = false;
		Report.Status = TEXT("plan_budget_exceeded");
		Report.Diagnostic = TEXT("Bounded GAS planning exceeded its declared synchronous time budget without mutation.");
	};

	TArray<FHyperAIStudioGASBackendOperation> Operations;
	TArray<FString> TargetPaths;
	TSet<FString> TargetSet;
	TSet<FString> TargetCategories;
	const int32 PlanIssueLimit = FMath::Clamp((Request.MaxOutputBytes - 24576) / 16384, 1, MaxIssues);
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioGASBackendOperation Backend;
		FString ErrorCode;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Backend, ErrorCode, Error))
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated, *ErrorCode, TEXT("error"),
				Request.Operations[Index].TargetPath, TEXT("gas:operation:") + FString::FromInt(Index),
				Index, Error);
			continue;
		}
		if (!Backend.bBackendSupported)
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
				TEXT("variant_backend_unavailable"), TEXT("error"), Backend.TargetPath,
				TEXT("gas:operation:") + FString::FromInt(Index), Index,
				TEXT("Variant is recognized but fails closed until its dedicated typed mutator and independent validator are proven."));
			continue;
		}
		const FString CategoryKey = Backend.TargetPath + TEXT("\n") + OperationCategory(Backend.Kind);
		if (TargetCategories.Contains(CategoryKey))
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
				TEXT("duplicate_target_operation"), TEXT("error"), Backend.TargetPath,
				TEXT("gas:operation:") + FString::FromInt(Index), Index,
				TEXT("A target may contain at most one operation from each semantic category."));
			continue;
		}
		TargetCategories.Add(CategoryKey);
		if (!TargetSet.Contains(Backend.TargetPath))
		{
			TargetSet.Add(Backend.TargetPath);
			TargetPaths.Add(Backend.TargetPath);
		}
		Operations.Add(MoveTemp(Backend));
		if (BudgetExceeded())
		{
			FailBudget();
			return Finish();
		}
	}
	if (HasError(Report.Issues) || Operations.Num() != Request.Operations.Num())
	{
		Report.Status = Report.Issues.ContainsByPredicate([](const FHyperAIGASIssue& Issue)
		{
			return Issue.Code == TEXT("variant_backend_unavailable");
		}) ? TEXT("variant_backend_unavailable") : TEXT("invalid_plan_shape");
		Report.Diagnostic = TEXT("One or more closed GAS operation variants were invalid or unavailable.");
		return Finish();
	}

	FHyperAIGASInspectRequest CaptureRequest;
	CaptureRequest.AssetPaths = TargetPaths;
	CaptureRequest.Variant = TEXT("all");
	CaptureRequest.bIncludeTags = true;
	CaptureRequest.bIncludeModifiers = true;
	CaptureRequest.bIncludeCues = true;
	CaptureRequest.bIncludeAttributes = true;
	CaptureRequest.MaxGameThreadMs = Request.MaxGameThreadMs;
	CaptureRequest.MaxOutputBytes = MaxOutputBytes;
	FHyperAIStudioGASValueSnapshot BaseSnapshot = CaptureSnapshot(CaptureRequest);
	if (BudgetExceeded())
	{
		FailBudget();
		return Finish();
	}
	TMap<FString, FHyperAIGASAssetRecord> OriginalByPath;
	TMap<FString, FHyperAIGASAssetRecord> SimulatedByPath;
	for (const FHyperAIGASAssetRecord& Record : BaseSnapshot.Records)
	{
		OriginalByPath.Add(Record.AssetPath, Record);
		SimulatedByPath.Add(Record.AssetPath, Record);
	}
	if (!BaseSnapshot.bComplete || OriginalByPath.Num() != TargetPaths.Num())
	{
		Report.Status = TEXT("target_state_incomplete");
		Report.Diagnostic = TEXT("Every target must be already loaded with a complete independent revision.");
		for (const FHyperAIGASIssue& Issue : BaseSnapshot.CaptureIssues)
		{
			if (Report.Issues.Num() >= PlanIssueLimit)
			{
				Report.bTruncated = true;
				break;
			}
			Report.Issues.Add(Issue);
		}
		return Finish();
	}

	TSet<FString> EffectTargets;
	TSet<FString> AbilityTargets;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const FHyperAIStudioGASBackendOperation& Operation = Operations[Index];
		FHyperAIGASAssetRecord* Original = OriginalByPath.Find(Operation.TargetPath);
		FHyperAIGASAssetRecord* Simulated = SimulatedByPath.Find(Operation.TargetPath);
		if (!Original || !Simulated || Original->Revision != Operation.ExpectedRevision)
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
				TEXT("revision_precondition_failed"), TEXT("error"), Operation.TargetPath,
				TEXT("gas:operation:") + FString::FromInt(Index), Index,
				TEXT("Target is missing or its fresh loaded revision no longer matches expected_revision."));
			continue;
		}
		if (RequiresEffect(Operation.Kind) && Original->Variant != TEXT("gameplay_effect_blueprint"))
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
				TEXT("target_variant_mismatch"), TEXT("error"), Operation.TargetPath,
				TEXT("gas:operation:") + FString::FromInt(Index), Index,
				TEXT("GameplayEffect operation target is not a loaded GameplayEffect Blueprint."));
			continue;
		}
		if (Operation.Kind == EHyperAIStudioGASOperationKind::SetAbilityAssetTags
			&& Original->Variant != TEXT("gameplay_ability_blueprint"))
		{
			AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
				TEXT("target_variant_mismatch"), TEXT("error"), Operation.TargetPath,
				TEXT("gas:operation:") + FString::FromInt(Index), Index,
				TEXT("Ability tag operation target is not a loaded GameplayAbility Blueprint."));
			continue;
		}

		if (Operation.Kind == EHyperAIStudioGASOperationKind::SetEffectDuration)
		{
			Simulated->DurationPolicy = Operation.DurationPolicy;
			Simulated->bHasStaticDuration = true;
			Simulated->DurationSeconds = Operation.DurationPolicy == TEXT("duration")
				? Operation.DurationSeconds : 0.0;
			Simulated->bHasStaticMaxDuration = true;
			Simulated->MaxDurationSeconds = Operation.DurationPolicy == TEXT("duration")
				? Operation.MaxDurationSeconds : 0.0;
			Simulated->PeriodSeconds = Operation.PeriodSeconds;
			if (Operation.bSetExecutePeriodicEffectOnApplication)
				Simulated->bExecutePeriodicEffectOnApplication = Operation.bExecutePeriodicEffectOnApplication;
		}
		else if (Operation.Kind == EHyperAIStudioGASOperationKind::ReplaceEffectModifiers)
		{
			Simulated->Modifiers.Reset();
			for (int32 ModifierIndex = 0; ModifierIndex < Operation.Modifiers.Num(); ++ModifierIndex)
			{
				FGameplayAttribute Resolved;
				FString AttributeError;
				if (!ResolveAttribute(Operation.Modifiers[ModifierIndex].AttributeOwnerClassPath,
					Operation.Modifiers[ModifierIndex].AttributeName, Resolved, AttributeError))
				{
					AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
						TEXT("modifier_attribute_unavailable"), TEXT("error"), Operation.TargetPath,
						TEXT("gas:operation:") + FString::FromInt(Index), Index, AttributeError);
					continue;
				}
				Simulated->Modifiers.Add(ModifierViewFromSpec(Operation.Modifiers[ModifierIndex], ModifierIndex));
			}
		}
		else if (Operation.Kind == EHyperAIStudioGASOperationKind::ReplaceEffectCues)
		{
			Simulated->Cues.Reset();
			for (int32 CueIndex = 0; CueIndex < Operation.Cues.Num(); ++CueIndex)
			{
				const FHyperAIGASCueSpec& Cue = Operation.Cues[CueIndex];
				if (!Cue.MagnitudeAttributeOwnerClassPath.IsEmpty())
				{
					FGameplayAttribute Resolved;
					FString AttributeError;
					if (!ResolveAttribute(Cue.MagnitudeAttributeOwnerClassPath,
						Cue.MagnitudeAttributeName, Resolved, AttributeError))
					{
						AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
							TEXT("cue_attribute_unavailable"), TEXT("error"), Operation.TargetPath,
							TEXT("gas:operation:") + FString::FromInt(Index), Index, AttributeError);
						continue;
					}
				}
				Simulated->Cues.Add(CueViewFromSpec(Cue, CueIndex));
			}
		}
		else if (Operation.Kind == EHyperAIStudioGASOperationKind::SetEffectCueFlags)
		{
			if (Operation.bSetRequireModifierSuccessToTriggerCues)
				Simulated->bRequireModifierSuccessToTriggerCues = Operation.bRequireModifierSuccessToTriggerCues;
			if (Operation.bSetSuppressStackingCues)
				Simulated->bSuppressStackingCues = Operation.bSuppressStackingCues;
		}
		else if (Operation.Kind == EHyperAIStudioGASOperationKind::SetAbilityAssetTags)
		{
			Simulated->AssetTags = Operation.Tags;
			Simulated->AssetTags.Sort();
		}
		Simulated->Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(RecordCanonical(*Simulated));
		Simulated->bRevisionComplete = IsCanonicalSha256(Simulated->Revision);
		if (RequiresEffect(Operation.Kind)) EffectTargets.Add(Operation.TargetPath);
		else AbilityTargets.Add(Operation.TargetPath);
		if (BudgetExceeded())
		{
			FailBudget();
			return Finish();
		}
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("plan_state_invalid");
		Report.Diagnostic = TEXT("Target type, CAS, registered tag, or loaded attribute preconditions failed.");
		return Finish();
	}
	if (BudgetExceeded())
	{
		FailBudget();
		return Finish();
	}

	FHyperAIStudioGASValueSnapshot SimulatedSnapshot;
	SimulatedSnapshot.bComplete = true;
	SimulatedByPath.GenerateValueArray(SimulatedSnapshot.Records);
	ComputeSnapshotRevision(SimulatedSnapshot);
	bool bValidationTruncated = false;
	const TArray<FHyperAIGASIssue> ValidationIssues = ValidateValueSnapshot(
		SimulatedSnapshot, false, MaxIssues, bValidationTruncated);
	for (const FHyperAIGASIssue& Issue : ValidationIssues)
	{
		if (Issue.Severity != TEXT("error")) continue;
		if (Report.Issues.Num() >= PlanIssueLimit)
		{
			Report.bTruncated = true;
			break;
		}
		Report.Issues.Add(Issue);
	}
	if (bValidationTruncated)
	{
		Report.bTruncated = true;
		AddIssue(Report.Issues, PlanIssueLimit, Report.bTruncated,
			TEXT("validation_evidence_truncated"), TEXT("error"), FString(),
			TEXT("gas:simulation"), -1,
			TEXT("Independent simulated postcondition evidence exceeded its hard finding bound."));
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("postcondition_invalid");
		Report.Diagnostic = TEXT("Pure simulation fails the independent GAS validator.");
		return Finish();
	}
	if (BudgetExceeded())
	{
		FailBudget();
		return Finish();
	}

	TargetPaths.Sort();
	TArray<FHyperAIGASAssetRecord> OriginalRecords;
	OriginalRecords.Reserve(TargetPaths.Num());
	for (const FString& Path : TargetPaths)
	{
		OriginalRecords.Add(OriginalByPath.FindChecked(Path));
	}
	Report.BaseRevision = ComputeBaseRevision(OriginalRecords);
	const FString SemanticFingerprint = ComputePayloadSemanticFingerprint(Operations, Report.BaseRevision);
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	FHyperAIStudioGASTypedPayload Payload;
	Payload.Operations = Operations;
	Payload.BaseRevision = Report.BaseRevision;
	Payload.SemanticFingerprint = SemanticFingerprint;
	if (!IsCanonicalSha256(SemanticFingerprint) || !IsCanonicalSha256(Report.BaseRevision)
		|| ProjectId.IsEmpty() || Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		Report.Status = TEXT("semantic_identity_unavailable");
		Report.Diagnostic = TEXT("Bounded GAS payload identity, project identity, or semantic hashes could not be produced.");
		return Finish();
	}

	const FHyperAIStudioGASDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterDescriptor& AdapterDescriptor = Adapter.GetDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_gas_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = AdapterDescriptor.AdapterFingerprint;
	// Pure source-candidate sealing only. The future async host must reseal against its live pin.
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("plugin.GameplayAbilities"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.GameplayAbilities"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("probe.ability_system"), EHyperAIStudioDomainPrerequisiteState::Available}};
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
	Contract.ArtifactTypeId = Payload.GetTypeId();
	Contract.ArtifactSchemaFingerprint = Payload.GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = SemanticFingerprint;
	Contract.EffectTarget = TEXT("gas:") + Report.BaseRevision;
	Contract.DeadlineMs = Request.DeadlineMs;
	// Pure non-executable sealing still needs the shared plan envelope minimum. No native
	// operation is advertised as runnable until compile/save/fresh CAS is hard-bounded.
	Contract.MaxNativeOperations = 5;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = false;
	// Shared typed sealing requires independent validate/fresh postconditions in the
	// hypothetical plan. The report below still advertises zero executable backend phases.
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		Report.Status = TEXT("typed_artifact_prepare_failed");
		Report.Diagnostic = Clip(PrepareError);
		return Finish();
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	if (BudgetExceeded())
	{
		FailBudget();
		return Finish();
	}

	Report.Effects.OperationCount = Operations.Num();
	Report.NativeOperationCount = 0;
	Report.Effects.TargetCount = TargetPaths.Num();
	Report.Effects.EffectsUpdated = EffectTargets.Num();
	Report.Effects.AbilitiesUpdated = AbilityTargets.Num();
	for (const FHyperAIStudioGASBackendOperation& Operation : Operations)
	{
		if (Operation.Kind == EHyperAIStudioGASOperationKind::ReplaceEffectModifiers)
			Report.Effects.ModifiersReplaced += Operation.Modifiers.Num();
		if (Operation.Kind == EHyperAIStudioGASOperationKind::ReplaceEffectCues)
			Report.Effects.CueSetsReplaced++;
		if (Operation.Kind == EHyperAIStudioGASOperationKind::SetAbilityAssetTags)
			Report.Effects.TagSetsReplaced++;
	}
	Report.Effects.bTransactionOnce = false;
	Report.Effects.bCompileOnce = false;
	Report.Effects.bSaveOnce = false;
	Report.Effects.bValidateOnce = false;
	Report.Effects.bFreshVerifyOnce = false;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("valid_dry_run_execution_blocked");
		Report.Diagnostic = TEXT("Closed GAS edit intent, loaded-state CAS, simulated postconditions, and typed hashes are valid evidence only. Transaction, compile, save, validation, and fresh verification remain zero-effect until a hard-bounded backend exists.");
		return Finish();
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Execution requires one journal-safe operation_id.");
		return Finish();
	}
	if (Request.ExpectedPlanHash != Report.PlanHash || !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Execution must echo the exact dry-run plan hash.");
		return Finish();
	}

	// Synchronous Blueprint compilation and editor package saving can load, scan, block, and
	// trigger global work inside UE 5.8. A journal host alone cannot make those effects bounded.
	Report.bOk = false;
	Report.bStaged = false;
	Report.bExecutionSubmitted = false;
	Report.bFallbackPermitted = false;
	Report.Status = ExecutionBlocker;
	Report.Diagnostic = TEXT("Zero effects: GAS mutation is not staged or submitted until a bounded non-loading compile/save backend and exact runtime-side-effect CAS are available.");
	return Finish();
}

FHyperAIGASApplyPlanReport UHyperAIStudioGASToolset::hyper_gas_apply_plan(
	const FHyperAIGASApplyPlanRequest& Request)
{
	return FHyperAIStudioGASContracts::BuildPlan(Request);
}

FString FHyperAIStudioGASTypedPayload::GetTypeId() const
{
	return FHyperAIStudioGASContracts::PayloadTypeId;
}

FString FHyperAIStudioGASTypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGASContracts::PayloadSchemaFingerprint;
}

int32 FHyperAIStudioGASTypedPayload::GetBoundedByteSize() const
{
	if (Operations.IsEmpty() || Operations.Num() > FHyperAIStudioGASContracts::MaxOperations
		|| BaseRevision.Len() > 71 || SemanticFingerprint.Len() > 71)
	{
		return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	}
	int64 Bytes = 256ll + static_cast<int64>(BaseRevision.Len() + SemanticFingerprint.Len())
		* HyperAIStudio::GAS::Private::MaxUtf8BytesPerCharacter;
	for (const FHyperAIStudioGASBackendOperation& Operation : Operations)
	{
		if (Operation.TargetPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
			|| Operation.ExpectedRevision.Len() > 71
			|| Operation.ParentClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
			|| Operation.Tags.Num() > FHyperAIStudioGASContracts::MaxTagsPerSet
			|| Operation.Modifiers.Num() > FHyperAIStudioGASContracts::MaxModifiers
			|| Operation.Cues.Num() > FHyperAIStudioGASContracts::MaxCues)
		{
			return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
		}
		for (const FString& Tag : Operation.Tags)
		{
			if (Tag.Len() > FHyperAIStudioGASContracts::MaxTagCharacters)
				return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
		}
		for (const FHyperAIGASModifierSpec& Modifier : Operation.Modifiers)
		{
			if (Modifier.AttributeOwnerClassPath.Len() > FHyperAIStudioGASContracts::MaxPathCharacters
				|| Modifier.AttributeName.Len() > 128 || Modifier.Operation.Len() > 32)
				return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
		}
		for (const FHyperAIGASCueSpec& Cue : Operation.Cues)
		{
			if (Cue.CueTags.Num() > FHyperAIStudioGASContracts::MaxCueTags
				|| Cue.MagnitudeAttributeOwnerClassPath.Len()
					> FHyperAIStudioGASContracts::MaxPathCharacters
				|| Cue.MagnitudeAttributeName.Len() > 128)
				return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
			for (const FString& Tag : Cue.CueTags)
			{
				if (Tag.Len() > FHyperAIStudioGASContracts::MaxTagCharacters)
					return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
			}
		}
		Bytes += static_cast<int64>(
			HyperAIStudio::GAS::Private::BackendOperationCanonical(Operation).Len())
			* HyperAIStudio::GAS::Private::MaxUtf8BytesPerCharacter;
		if (Bytes > FHyperAIStudioDomainLimits::MaxRequestBytes) return FHyperAIStudioDomainLimits::MaxRequestBytes + 1;
	}
	return static_cast<int32>(Bytes);
}

FString FHyperAIStudioGASTypedPayload::GetSemanticFingerprint() const
{
	const FString Recomputed = FHyperAIStudioGASContracts::ComputePayloadSemanticFingerprint(
		Operations, BaseRevision);
	return Recomputed == SemanticFingerprint ? Recomputed : FString();
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioGASTypedPayload::CloneImmutable() const
{
	// All members are owning value types; this copy deep-copies every operation/tag/modifier/cue array.
	TSharedRef<FHyperAIStudioGASTypedPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioGASTypedPayload, ESPMode::ThreadSafe>();
	Clone->Operations = Operations;
	Clone->BaseRevision = BaseRevision;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioGASResultPayload::GetTypeId() const
{
	return FHyperAIStudioGASContracts::ResultTypeId;
}

FString FHyperAIStudioGASResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioGASContracts::ResultSchemaFingerprint;
}

int32 FHyperAIStudioGASResultPayload::GetBoundedByteSize() const
{
	const int64 Bytes = 96ll + static_cast<int64>(Phase.Len() + Revision.Len())
		* HyperAIStudio::GAS::Private::MaxUtf8BytesPerCharacter;
	return Bytes > FHyperAIStudioDomainLimits::MaxResultBytes
		? FHyperAIStudioDomainLimits::MaxResultBytes + 1 : static_cast<int32>(Bytes);
}
