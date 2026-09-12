// Games by Hyper 2026.

#include "HyperAIStudioPCGToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Data/Registry/PCGDataType.h"
#include "Data/Registry/PCGDataTypeIdentifier.h"
#include "Editor/PCGGraphComment.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Internationalization/Text.h"
#include "IO/IoHash.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "RuntimeGen/SchedulingPolicies/PCGSchedulingPolicyBase.h"
#include "StructUtils/PropertyBag.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPCG, Log, All);

namespace HyperAIStudio::PCG::Private
{
	static constexpr int32 MaxIdentityDepth = 32;
	static constexpr int32 MaxTypeIds = 64;

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < 0x20 || Character == 0x7f)
			{
				return true;
			}
		}
		return false;
	}

	bool IsLowerHexOfLength(const FString& Value, const int32 Length)
	{
		if (Value.Len() != Length) return false;
		for (const TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
		}
		return true;
	}

	void AppendTokenUnchecked(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	FString HashCanonical(const FString& Canonical)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	struct FProjectionContext
	{
		int32 WorkUnits = 0;
		int32 MaterializedBytes = 0;
		int32 PropertyCount = 0;
		int32 EmbeddedGraphCount = 0;
		double AbsoluteDeadlineSeconds = 0.0;
		bool bComplete = true;
		FString FirstFailure;

		bool DeadlineExceeded() const
		{
			return FPlatformTime::Seconds() > AbsoluteDeadlineSeconds;
		}

		bool ConsumeWork(const int32 Amount, const TCHAR* Failure)
		{
			if (Amount < 0 || WorkUnits > FHyperAIStudioPCGContracts::MaxProjectionWorkUnits - Amount
				|| DeadlineExceeded())
			{
				MarkIncomplete(Failure);
				return false;
			}
			WorkUnits += Amount;
			return true;
		}

		bool ReserveMaterializedBytes(const int32 Amount, const TCHAR* Failure)
		{
			if (Amount < 0 || MaterializedBytes > FHyperAIStudioPCGContracts::MaxProjectionBytes - Amount)
			{
				MarkIncomplete(Failure);
				return false;
			}
			MaterializedBytes += Amount;
			return true;
		}

		void MarkIncomplete(const TCHAR* Failure)
		{
			bComplete = false;
			if (FirstFailure.IsEmpty())
			{
				FirstFailure = Failure;
			}
		}
	};

	bool AppendBoundedToken(
		FProjectionContext& Context,
		FString& Canonical,
		const FString& Value,
		const bool bPerValue = true)
	{
		const int64 AddedCharacters = static_cast<int64>(Value.Len()) + 24;
		const int64 AddedBytes = AddedCharacters * static_cast<int64>(sizeof(TCHAR));
		if (Value.Len() > FHyperAIStudioPCGContracts::MaxStringCharacters
			|| AddedBytes > MAX_int32
			|| (bPerValue && Canonical.Len() + AddedCharacters
				> FHyperAIStudioPCGContracts::MaxValueProjectionBytes / static_cast<int32>(sizeof(TCHAR)))
			|| !Context.ReserveMaterializedBytes(static_cast<int32>(AddedBytes),
				TEXT("projection_byte_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("projection_value_not_bounded"));
			return false;
		}
		AppendTokenUnchecked(Canonical, Value);
		return true;
	}

	bool AppendName(
		FProjectionContext& Context,
		FString& Canonical,
		const FName Name,
		const int32 MaxCharacters = FHyperAIStudioPCGContracts::MaxNameCharacters)
	{
		const uint32 Length = Name.GetStringLength();
		if (Length > static_cast<uint32>(MaxCharacters)
			|| !Context.ReserveMaterializedBytes(
				static_cast<int32>((Length + 1) * sizeof(TCHAR)), TEXT("name_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("name_not_bounded"));
			return false;
		}
		const FString Value = Name.ToString();
		return AppendBoundedToken(Context, Canonical, Value);
	}

	bool MaterializeNameBounded(
		const FName Name,
		FProjectionContext& Context,
		FString& OutValue,
		const int32 MaxCharacters = FHyperAIStudioPCGContracts::MaxNameCharacters)
	{
		OutValue.Reset();
		const uint32 Length = Name.GetStringLength();
		if (Length > static_cast<uint32>(MaxCharacters)
			|| !Context.ReserveMaterializedBytes(
				static_cast<int32>((Length + 1) * sizeof(TCHAR)),
				TEXT("name_materialization_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("name_not_bounded_before_materialization"));
			return false;
		}
		OutValue = Name.ToString();
		return true;
	}

	bool AppendGuid(
		FProjectionContext& Context,
		FString& Canonical,
		const FGuid& Guid)
	{
		if (!Context.ReserveMaterializedBytes(33 * sizeof(TCHAR),
			TEXT("guid_materialization_budget_exceeded")))
		{
			return false;
		}
		FString Value;
		if (Guid.IsValid())
		{
			Value = Guid.ToString(EGuidFormats::Digits);
			Value.ToLowerInline();
		}
		return AppendBoundedToken(Context, Canonical, Value);
	}

	bool ProjectOverrideMask(
		const TSet<FGuid>& OverrideIds,
		FProjectionContext& Context,
		FString& OutFingerprint)
	{
		OutFingerprint.Reset();
		const int32 Count = OverrideIds.Num();
		const int32 SlotCount = OverrideIds.GetMaxIndex();
		FString Error;
		if (!FHyperAIStudioPCGContracts::AdmitSparseContainerBeforeProjection(
			Count, SlotCount, Context.WorkUnits, Context.MaterializedBytes,
			static_cast<int32>(sizeof(FGuid)), Error)
			|| !Context.ConsumeWork(SlotCount, TEXT("override_mask_slot_work_exceeded"))
			|| !Context.ReserveMaterializedBytes(
				Count * static_cast<int32>(sizeof(FGuid)),
				TEXT("override_mask_copy_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("override_mask_not_bounded_before_copy"));
			return false;
		}
		TArray<FGuid> SortedIds;
		SortedIds.Reserve(Count);
		for (const FGuid& Id : OverrideIds)
		{
			if (!Id.IsValid())
			{
				Context.MarkIncomplete(TEXT("override_mask_contains_invalid_guid"));
				return false;
			}
			SortedIds.Add(Id);
		}
		SortedIds.Sort([](const FGuid& Left, const FGuid& Right)
		{
			if (Left.A != Right.A) return Left.A < Right.A;
			if (Left.B != Right.B) return Left.B < Right.B;
			if (Left.C != Right.C) return Left.C < Right.C;
			return Left.D < Right.D;
		});
		FString Canonical;
		if (!AppendBoundedToken(Context, Canonical, TEXT("hyperai.pcg.instance-override-mask.v1"))
			|| !AppendBoundedToken(Context, Canonical, FString::FromInt(Count)))
		{
			return false;
		}
		for (const FGuid& Id : SortedIds)
		{
			if (!AppendGuid(Context, Canonical, Id)) return false;
		}
		OutFingerprint = HashCanonical(Canonical);
		return Context.bComplete
			&& FHyperAIStudioPCGContracts::IsCanonicalSha256(OutFingerprint);
	}

	bool AppendUtf8(
		FProjectionContext& Context,
		FString& Canonical,
		const FUtf8String& Utf8,
		const int32 MaxCharacters)
	{
		const int32 ByteLength = Utf8.Len();
		if (ByteLength < 0 || ByteLength > MaxCharacters
			|| !Context.ReserveMaterializedBytes(
				(ByteLength + 1) * static_cast<int32>(sizeof(TCHAR)) * 2,
				TEXT("utf8_materialization_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("utf8_value_not_bounded_before_materialization"));
			return false;
		}
		const FString Value{FUtf8StringView(Utf8)};
		return AppendBoundedToken(Context, Canonical, Value);
	}

	bool IsUnsignedIntegerProperty(const FNumericProperty* Property)
	{
		return CastField<FByteProperty>(Property)
			|| CastField<FUInt16Property>(Property)
			|| CastField<FUInt32Property>(Property)
			|| CastField<FUInt64Property>(Property);
	}

	bool BuildObjectIdentity(
		const UObject* Object,
		FProjectionContext& Context,
		FString& OutIdentity)
	{
		OutIdentity.Reset();
		if (!Object)
		{
			OutIdentity = TEXT("null");
			return true;
		}
		TArray<const UObject*, TInlineAllocator<MaxIdentityDepth>> Chain;
		for (const UObject* Current = Object; Current; Current = Current->GetOuter())
		{
			const int32 NameLimit = Current->IsA<UPackage>()
				? FHyperAIStudioPCGContracts::MaxPathCharacters
				: FHyperAIStudioPCGContracts::MaxNameCharacters;
			if (Chain.Num() >= MaxIdentityDepth || Current->GetFName().GetStringLength()
				> static_cast<uint32>(NameLimit))
			{
				Context.MarkIncomplete(TEXT("object_identity_not_bounded"));
				return false;
			}
			Chain.Add(Current);
		}
		int32 RequiredCharacters = 8;
		for (const UObject* Current : Chain)
		{
			RequiredCharacters += static_cast<int32>(Current->GetFName().GetStringLength()) + 2;
		}
		if (RequiredCharacters > FHyperAIStudioPCGContracts::MaxPathCharacters
			|| !Context.ReserveMaterializedBytes(RequiredCharacters * sizeof(TCHAR),
				TEXT("object_identity_byte_budget_exceeded")))
		{
			Context.MarkIncomplete(TEXT("object_identity_not_bounded"));
			return false;
		}
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			if (!OutIdentity.IsEmpty())
			{
				const int32 ObjectOrdinal = Chain.Num() - 1 - Index;
				OutIdentity += ObjectOrdinal == 1 ? TEXT(".")
					: ObjectOrdinal == 2 ? TEXT(":") : TEXT(".");
			}
			OutIdentity += Chain[Index]->GetFName().ToString();
		}
		return true;
	}

	bool ProjectPropertyValue(
		const FProperty* Property,
		const void* ValuePtr,
		int32 Depth,
		FProjectionContext& Context,
		FString& OutCanonical);

	bool ProjectStructValue(
		const UStruct* Struct,
		const void* StructMemory,
		const int32 Depth,
		FProjectionContext& Context,
		FString& OutCanonical)
	{
		if (!Struct || !StructMemory || Depth > FHyperAIStudioPCGContracts::MaxProjectionDepth
			|| !Context.ConsumeWork(1, TEXT("projection_depth_or_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("struct_projection_unavailable"));
			return false;
		}
		FString StructIdentity;
		if (!BuildObjectIdentity(Struct, Context, StructIdentity)
			|| !AppendBoundedToken(Context, OutCanonical, StructIdentity))
		{
			return false;
		}
		int32 PersistedPropertyCount = 0;
		int32 PersistedValueCount = 0;
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (Context.DeadlineExceeded())
			{
				Context.MarkIncomplete(TEXT("struct_property_inventory_deadline_exceeded"));
				return false;
			}
			if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient
				| CPF_NonPIEDuplicateTransient | CPF_SkipSerialization))
			{
				if (It->ArrayDim <= 0
					|| It->ArrayDim > FHyperAIStudioPCGContracts::MaxContainerElements
					|| PersistedValueCount > FHyperAIStudioPCGContracts::MaxProperties - It->ArrayDim)
				{
					Context.MarkIncomplete(TEXT("static_array_dimension_exceeded"));
					return false;
				}
				++PersistedPropertyCount;
				PersistedValueCount += It->ArrayDim;
				if (PersistedPropertyCount > FHyperAIStudioPCGContracts::MaxProperties
					|| PersistedValueCount > FHyperAIStudioPCGContracts::MaxProperties)
				{
					Context.MarkIncomplete(TEXT("property_count_exceeded"));
					return false;
				}
			}
		}
		if (PersistedPropertyCount > FHyperAIStudioPCGContracts::MaxProperties
			|| PersistedValueCount > FHyperAIStudioPCGContracts::MaxProperties
			|| Context.PropertyCount > FHyperAIStudioPCGContracts::MaxProperties - PersistedValueCount
			|| !Context.ConsumeWork(PersistedValueCount, TEXT("property_count_exceeded")))
		{
			Context.MarkIncomplete(TEXT("property_count_exceeded"));
			return false;
		}
		Context.PropertyCount += PersistedValueCount;
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Field = *It;
			if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient
				| CPF_NonPIEDuplicateTransient | CPF_SkipSerialization))
			{
				continue;
			}
			FString FieldCanonical;
			if (!AppendName(Context, FieldCanonical, Field->GetFName()))
			{
				return false;
			}
			for (int32 ArrayIndex = 0; ArrayIndex < Field->ArrayDim; ++ArrayIndex)
			{
				const void* FieldValue = Field->ContainerPtrToValuePtr<void>(StructMemory, ArrayIndex);
				if (!ProjectPropertyValue(Field, FieldValue, Depth + 1, Context, FieldCanonical))
				{
					return false;
				}
			}
			if (!AppendBoundedToken(Context, OutCanonical, HashCanonical(FieldCanonical)))
			{
				return false;
			}
		}
		return Context.bComplete;
	}

	bool ProjectContainerElement(
		const FProperty* Property,
		const void* ValuePtr,
		const int32 Depth,
		FProjectionContext& Context,
		FString& OutHash)
	{
		FString ElementCanonical;
		if (!ProjectPropertyValue(Property, ValuePtr, Depth + 1, Context, ElementCanonical))
		{
			return false;
		}
		if (ElementCanonical.Len() * static_cast<int32>(sizeof(TCHAR))
			> FHyperAIStudioPCGContracts::MaxValueProjectionBytes)
		{
			Context.MarkIncomplete(TEXT("container_element_projection_exceeded"));
			return false;
		}
		OutHash = HashCanonical(ElementCanonical);
		return FHyperAIStudioPCGContracts::IsCanonicalSha256(OutHash);
	}

	bool ProjectPropertyValue(
		const FProperty* Property,
		const void* ValuePtr,
		const int32 Depth,
		FProjectionContext& Context,
		FString& OutCanonical)
	{
		if (!Property || !ValuePtr || Depth > FHyperAIStudioPCGContracts::MaxProjectionDepth
			|| !Context.ConsumeWork(1, TEXT("property_projection_deadline_or_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("property_value_unavailable"));
			return false;
		}
		if (!Property->GetClass()
			|| !AppendName(Context, OutCanonical, Property->GetClass()->GetFName()))
		{
			return false;
		}

		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
		{
			return AppendBoundedToken(Context, OutCanonical,
				Bool->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false"));
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
		{
			FString EnumIdentity;
			const FNumericProperty* Underlying = Enum->GetUnderlyingProperty();
			if (!Underlying || !BuildObjectIdentity(Enum->GetEnum(), Context, EnumIdentity))
			{
				Context.MarkIncomplete(TEXT("enum_projection_unavailable"));
				return false;
			}
			return AppendBoundedToken(Context, OutCanonical, EnumIdentity)
				&& AppendBoundedToken(Context, OutCanonical, IsUnsignedIntegerProperty(Underlying)
					? FString::Printf(TEXT("%llu"), Underlying->GetUnsignedIntPropertyValue(ValuePtr))
					: FString::Printf(TEXT("%lld"), Underlying->GetSignedIntPropertyValue(ValuePtr)));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				FString EnumIdentity;
				if (!BuildObjectIdentity(Enum, Context, EnumIdentity)
					|| !AppendBoundedToken(Context, OutCanonical, EnumIdentity))
				{
					return false;
				}
			}
			if (Numeric->IsFloatingPoint())
			{
				return AppendBoundedToken(Context, OutCanonical,
					FString::Printf(TEXT("%.17g"), Numeric->GetFloatingPointPropertyValue(ValuePtr)));
			}
			return AppendBoundedToken(Context, OutCanonical, IsUnsignedIntegerProperty(Numeric)
				? FString::Printf(TEXT("%llu"), Numeric->GetUnsignedIntPropertyValue(ValuePtr))
				: FString::Printf(TEXT("%lld"), Numeric->GetSignedIntPropertyValue(ValuePtr)));
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return AppendName(Context, OutCanonical, NameProperty->GetPropertyValue(ValuePtr));
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			const FString& Value = StringProperty->GetPropertyValue(ValuePtr);
			if (Value.Len() > FHyperAIStudioPCGContracts::MaxStringCharacters)
			{
				Context.MarkIncomplete(TEXT("string_value_not_bounded"));
				return false;
			}
			return AppendBoundedToken(Context, OutCanonical, Value);
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			const FText& Value = TextProperty->GetPropertyValue(ValuePtr);
			const FString* Source = FTextInspector::GetSourceString(Value);
			if (!Source || Source->Len() > FHyperAIStudioPCGContracts::MaxStringCharacters)
			{
				Context.MarkIncomplete(TEXT("text_source_unavailable_or_unbounded"));
				return false;
			}
			const bool bInvariant = Value.IsCultureInvariant();
			if (!bInvariant && !Value.IsEmpty())
			{
				// Namespace/key access returns newly materialized strings; do not use it in this projector.
				Context.MarkIncomplete(TEXT("localized_text_identity_not_projected"));
				return false;
			}
			return AppendBoundedToken(Context, OutCanonical, *Source)
				&& AppendBoundedToken(Context, OutCanonical, bInvariant ? TEXT("invariant") : TEXT("localized"));
		}
		if (const FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr& SoftPtr = SoftObject->GetPropertyValue(ValuePtr);
			const FSoftObjectPath& Path = SoftPtr.ToSoftObjectPath();
			const FName PackageName = Path.GetAssetPath().GetPackageName();
			const FName AssetName = Path.GetAssetPath().GetAssetName();
			const FUtf8String& SubPath = Path.GetSubPathUtf8String();
			if (SubPath.Len() > FHyperAIStudioPCGContracts::MaxPathCharacters)
			{
				Context.MarkIncomplete(TEXT("soft_path_not_bounded"));
				return false;
			}
			return AppendName(Context, OutCanonical, PackageName,
					FHyperAIStudioPCGContracts::MaxPathCharacters)
				&& AppendName(Context, OutCanonical, AssetName)
				&& AppendUtf8(Context, OutCanonical, SubPath,
					FHyperAIStudioPCGContracts::MaxPathCharacters);
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
			{
				Context.MarkIncomplete(TEXT("instanced_object_content_projection_unsupported"));
				return false;
			}
			FString Identity;
			if (!BuildObjectIdentity(ObjectProperty->GetObjectPropertyValue(ValuePtr), Context, Identity))
			{
				return false;
			}
			return AppendBoundedToken(Context, OutCanonical, Identity);
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return ProjectStructValue(StructProperty->Struct, ValuePtr, Depth + 1, Context, OutCanonical);
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			const int32 Count = Helper.Num();
			FString Error;
			if (!FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
				Count, Context.WorkUnits, Context.MaterializedBytes, 72, Error))
			{
				Context.MarkIncomplete(TEXT("array_rejected_before_projection"));
				return false;
			}
			if (!Context.ConsumeWork(Count, TEXT("array_work_budget_exceeded"))
				|| !AppendBoundedToken(Context, OutCanonical, FString::FromInt(Count)))
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FString ElementHash;
				if (!ProjectContainerElement(ArrayProperty->Inner, Helper.GetRawPtr(Index),
					Depth + 1, Context, ElementHash))
				{
					return false;
				}
				if (!AppendBoundedToken(Context, OutCanonical, ElementHash))
				{
					return false;
				}
			}
			return Context.bComplete;
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			const int32 Count = Helper.Num();
			const int32 SlotCount = Helper.GetMaxIndex();
			FString Error;
			if (!FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
				Count, Context.WorkUnits, Context.MaterializedBytes, 192, Error)
				|| SlotCount < Count
				|| SlotCount > FHyperAIStudioPCGContracts::MaxContainerElements * 2
				|| !Context.ConsumeWork(SlotCount, TEXT("set_slot_work_budget_exceeded")))
			{
				Context.MarkIncomplete(TEXT("set_rejected_before_projection"));
				return false;
			}
			if (!Context.ReserveMaterializedBytes(Count * 192,
				TEXT("set_hash_materialization_exceeded"))) return false;
			TArray<FString> Hashes;
			Hashes.Reserve(Count);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index)) continue;
				FString ElementHash;
				if (!ProjectContainerElement(SetProperty->ElementProp, Helper.GetElementPtr(Index),
					Depth + 1, Context, ElementHash))
				{
					return false;
				}
				Hashes.Add(MoveTemp(ElementHash));
			}
			Hashes.Sort();
			if (!AppendBoundedToken(Context, OutCanonical, FString::FromInt(Count))) return false;
			for (const FString& Hash : Hashes)
			{
				if (!AppendBoundedToken(Context, OutCanonical, Hash)) return false;
			}
			return Context.bComplete;
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			const int32 Count = Helper.Num();
			const int32 SlotCount = Helper.GetMaxIndex();
			FString Error;
			if (!FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
				Count, Context.WorkUnits, Context.MaterializedBytes, 384, Error)
				|| SlotCount < Count
				|| SlotCount > FHyperAIStudioPCGContracts::MaxContainerElements * 2
				|| !Context.ConsumeWork(SlotCount, TEXT("map_slot_work_budget_exceeded")))
			{
				Context.MarkIncomplete(TEXT("map_rejected_before_projection"));
				return false;
			}
			if (!Context.ReserveMaterializedBytes(Count * 384,
				TEXT("map_hash_materialization_exceeded"))) return false;
			TArray<FString> Entries;
			Entries.Reserve(Count);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index)) continue;
				FString KeyHash;
				FString ValueHash;
				if (!ProjectContainerElement(MapProperty->KeyProp, Helper.GetKeyPtr(Index),
					Depth + 1, Context, KeyHash)
					|| !ProjectContainerElement(MapProperty->ValueProp, Helper.GetValuePtr(Index),
						Depth + 1, Context, ValueHash))
				{
					return false;
				}
				Entries.Add(KeyHash + TEXT("=") + ValueHash);
			}
			Entries.Sort();
			if (!AppendBoundedToken(Context, OutCanonical, FString::FromInt(Count))) return false;
			for (const FString& Entry : Entries)
			{
				if (!AppendBoundedToken(Context, OutCanonical, Entry)) return false;
			}
			return Context.bComplete;
		}

		Context.MarkIncomplete(TEXT("unsupported_property_kind"));
		return false;
	}

	bool ProjectObjectProperties(
		const UObject* Object,
		const TSet<FName>& ExcludedNames,
		const bool bIncludeSuper,
		FProjectionContext& Context,
		FString& OutFingerprint)
	{
		OutFingerprint.Reset();
		if (!Object)
		{
			Context.MarkIncomplete(TEXT("projection_object_missing"));
			return false;
		}
		FString Canonical;
		if (!AppendBoundedToken(Context, Canonical,
			TEXT("hyperai.pcg.property-projection.v1"))) return false;
		FString ObjectClass;
		if (!BuildObjectIdentity(Object->GetClass(), Context, ObjectClass)
			|| !AppendBoundedToken(Context, Canonical, ObjectClass)) return false;
		const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeSuper
			? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
		int32 Count = 0;
		int32 ValueCount = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass(), SuperFlag); It; ++It)
		{
			if (Context.DeadlineExceeded())
			{
				Context.MarkIncomplete(TEXT("object_property_inventory_deadline_exceeded"));
				return false;
			}
			if (!ExcludedNames.Contains(It->GetFName())
				&& !It->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient
					| CPF_NonPIEDuplicateTransient | CPF_SkipSerialization))
			{
				if (It->ArrayDim <= 0
					|| It->ArrayDim > FHyperAIStudioPCGContracts::MaxContainerElements
					|| ValueCount > FHyperAIStudioPCGContracts::MaxProperties - It->ArrayDim)
				{
					Context.MarkIncomplete(TEXT("static_array_dimension_exceeded"));
					return false;
				}
				++Count;
				ValueCount += It->ArrayDim;
				if (Count > FHyperAIStudioPCGContracts::MaxProperties
					|| ValueCount > FHyperAIStudioPCGContracts::MaxProperties)
				{
					Context.MarkIncomplete(TEXT("object_property_count_exceeded"));
					return false;
				}
			}
		}
		if (Count > FHyperAIStudioPCGContracts::MaxProperties
			|| ValueCount > FHyperAIStudioPCGContracts::MaxProperties
			|| Context.PropertyCount > FHyperAIStudioPCGContracts::MaxProperties - ValueCount
			|| !Context.ConsumeWork(ValueCount, TEXT("object_property_count_exceeded")))
		{
			Context.MarkIncomplete(TEXT("object_property_count_exceeded"));
			return false;
		}
		Context.PropertyCount += ValueCount;
		for (TFieldIterator<FProperty> It(Object->GetClass(), SuperFlag); It; ++It)
		{
			const FProperty* Property = *It;
			if (ExcludedNames.Contains(Property->GetFName())
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient
					| CPF_NonPIEDuplicateTransient | CPF_SkipSerialization))
			{
				continue;
			}
			FString PropertyCanonical;
			if (!AppendName(Context, PropertyCanonical, Property->GetFName())) return false;
			for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
			{
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object, ArrayIndex);
				if (!ProjectPropertyValue(Property, ValuePtr, 1, Context, PropertyCanonical))
				{
					return false;
				}
			}
			if (!AppendBoundedToken(Context, Canonical, HashCanonical(PropertyCanonical)))
			{
				return false;
			}
		}
		OutFingerprint = HashCanonical(Canonical);
		return Context.bComplete && FHyperAIStudioPCGContracts::IsCanonicalSha256(OutFingerprint);
	}

	bool ProjectPropertyBag(
		const FInstancedPropertyBag* Bag,
		FProjectionContext& Context,
		FString& OutFingerprint,
		int32& OutCount)
	{
		OutFingerprint.Reset();
		OutCount = 0;
		if (!Bag || !Bag->IsValid())
		{
			FString Empty;
			AppendTokenUnchecked(Empty, TEXT("hyperai.pcg.empty-property-bag.v1"));
			OutFingerprint = HashCanonical(Empty);
			return true;
		}
		const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct();
		const FConstStructView View = Bag->GetValue();
		if (!BagStruct || !View.IsValid() || View.GetScriptStruct() != BagStruct || !View.GetMemory())
		{
			Context.MarkIncomplete(TEXT("property_bag_view_unavailable"));
			return false;
		}
		const TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct->GetPropertyDescs();
		OutCount = Descs.Num();
		FString Error;
		if (!FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
			OutCount, Context.WorkUnits, Context.MaterializedBytes, 160, Error))
		{
			Context.MarkIncomplete(TEXT("property_bag_rejected_before_projection"));
			return false;
		}
		FString Canonical;
		if (!AppendBoundedToken(Context, Canonical, TEXT("hyperai.pcg.property-bag.v1"))
			|| !AppendBoundedToken(Context, Canonical, FString::FromInt(OutCount)))
		{
			return false;
		}
		for (const FPropertyBagPropertyDesc& Desc : Descs)
		{
			if (!Context.ConsumeWork(1, TEXT("property_bag_descriptor_work_exceeded")))
			{
				return false;
			}
			if (!Desc.CachedProperty || Desc.Name.GetStringLength()
				> static_cast<uint32>(FHyperAIStudioPCGContracts::MaxNameCharacters)
				|| Desc.MetaData.Num() > FHyperAIStudioPCGContracts::MaxContainerElements
				|| Desc.ContainerTypes.Num()
					> static_cast<uint32>(FHyperAIStudioPCGContracts::MaxProjectionDepth))
			{
				Context.MarkIncomplete(TEXT("property_bag_descriptor_unsupported"));
				continue;
			}
			FString Descriptor;
			if (!AppendGuid(Context, Descriptor, Desc.ID)
				|| !AppendName(Context, Descriptor, Desc.Name)) return false;
			if (!AppendBoundedToken(Context, Descriptor,
					FString::FromInt(static_cast<int32>(Desc.ValueType)))
				|| !AppendBoundedToken(Context, Descriptor,
					FString::FromInt(static_cast<int32>(Desc.KeyType)))
				|| !AppendBoundedToken(Context, Descriptor,
					FString::Printf(TEXT("%llu"), Desc.PropertyFlags))
				|| !AppendBoundedToken(Context, Descriptor,
					FString::FromInt(Desc.ContainerTypes.Num())))
			{
				return false;
			}
			for (uint32 Index = 0; Index < Desc.ContainerTypes.Num(); ++Index)
			{
				if (!Context.ConsumeWork(1, TEXT("property_bag_container_type_work_exceeded")))
				{
					return false;
				}
				if (!AppendBoundedToken(Context, Descriptor,
					FString::FromInt(static_cast<int32>(Desc.ContainerTypes[Index])))) return false;
			}
			FString ValueTypeIdentity;
			FString KeyTypeIdentity;
			if (!BuildObjectIdentity(Desc.ValueTypeObject, Context, ValueTypeIdentity)
				|| !BuildObjectIdentity(Desc.KeyTypeObject, Context, KeyTypeIdentity)
				|| !AppendBoundedToken(Context, Descriptor, ValueTypeIdentity)
				|| !AppendBoundedToken(Context, Descriptor, KeyTypeIdentity)) return false;
#if WITH_EDITORONLY_DATA
			for (const FPropertyBagPropertyDescMetaData& Meta : Desc.MetaData)
			{
				if (!Context.ConsumeWork(1, TEXT("property_bag_metadata_work_exceeded")))
				{
					return false;
				}
				if (Meta.Key.GetStringLength() > static_cast<uint32>(FHyperAIStudioPCGContracts::MaxNameCharacters)
					|| Meta.Value.Len() > FHyperAIStudioPCGContracts::MaxStringCharacters)
				{
					Context.MarkIncomplete(TEXT("property_bag_metadata_unbounded"));
					break;
				}
				if (!AppendName(Context, Descriptor, Meta.Key)
					|| !AppendBoundedToken(Context, Descriptor, Meta.Value)) return false;
			}
#endif
			const void* ValuePtr = Desc.CachedProperty->ContainerPtrToValuePtr<void>(View.GetMemory());
			if (!ProjectPropertyValue(Desc.CachedProperty, ValuePtr, 1, Context, Descriptor))
			{
				return false;
			}
			if (!AppendBoundedToken(Context, Canonical, HashCanonical(Descriptor))) return false;
		}
		OutFingerprint = HashCanonical(Canonical);
		return Context.bComplete && FHyperAIStudioPCGContracts::IsCanonicalSha256(OutFingerprint);
	}
}

namespace HyperAIStudio::PCG::Private
{
	int32 EstimateIssueBytes(const FHyperAIPCGIssue& Issue);
	int32 EstimatePinBytes(const FHyperAIPCGPinRecord& Pin);
	int32 EstimateNodeBytes(const FHyperAIPCGNodeRecord& Node);
	int32 EstimateEdgeBytes(const FHyperAIStudioPCGEdgeState& Edge);
	int32 EstimateGraphBytes(const FHyperAIPCGGraphRecord& Graph);
	int32 EstimateComponentBytes(const FHyperAIPCGComponentRecord& Component);
	int32 EstimateCapabilitiesBytes(const TArray<FHyperAIPCGCapabilityStatus>& Values);
	FHyperAIPCGIssue MakeIssue(
		const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail);
	void AddCaptureIssue(
		FHyperAIStudioPCGValueSnapshot& Snapshot,
		const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail);
	bool CaptureGraphState(
		const UPCGGraph* Graph, const FString& GraphPath,
		FProjectionContext& Context, FHyperAIStudioPCGValueSnapshot& OutSnapshot);
	bool CaptureComponentState(
		const UPCGComponent* Component, const UPCGGraph* Graph,
		const FString& GraphPath, const FString& ComponentPath,
		FProjectionContext& Context, FHyperAIPCGComponentRecord& OutRecord);
}

FString FHyperAIStudioPCGContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPCG.HyperAIStudioPCGToolset");
}

const TArray<FHyperAIStudioPCGManifestEntry>& FHyperAIStudioPCGContracts::GetManifest()
{
	static const TArray<FHyperAIStudioPCGManifestEntry> Manifest = {
		{TEXT("hyper_pcg_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_pcg_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_pcg_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioPCGContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioPCGManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioPCGManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name))
		{
			return false;
		}
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

const TArray<FString>& FHyperAIStudioPCGContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("CreateGraph"), TEXT("GetGraphStructure"), TEXT("SetGraphParams"),
		TEXT("RemoveGraphParams"), TEXT("GetGraphSchema"), TEXT("GetGraphDescription"),
		TEXT("SetGraphDescription"), TEXT("ListGraphInstances"), TEXT("SpawnGraphInstance"),
		TEXT("ExecuteGraphInstance"), TEXT("GetGraphInstanceParams"),
		TEXT("SetGraphInstanceParams"), TEXT("ResetGraphInstanceParams"),
		TEXT("ListNativeNodes"), TEXT("ListAvailableSubgraphs"),
		TEXT("GetNativeNodeSchema"), TEXT("AddNode"), TEXT("AddSubgraphNode"),
		TEXT("UpdateNode"), TEXT("SetNodeComment"), TEXT("GetNodeInfo"),
		TEXT("RepositionNode"), TEXT("RemoveNode"), TEXT("ConnectNodePins"),
		TEXT("DisconnectNodePins"), TEXT("GetNodeDataView"), TEXT("AddCommentBox"),
		TEXT("UpdateCommentBox"), TEXT("RemoveCommentBox"), TEXT("DrawSpline"),
		TEXT("RunPCGInstantGraph")};
	return Delegates;
}

const TArray<FString>& FHyperAIStudioPCGContracts::GetCapabilityRequirementCoordinates()
{
	static const TArray<FString> Coordinates = {
		TEXT("capability.pcg.graph.create"), TEXT("capability.pcg.graph.inspect"),
		TEXT("capability.pcg.graph.configure"), TEXT("capability.pcg.graph.execute"),
		TEXT("capability.pcg.graph.parameters"), TEXT("capability.pcg.node.add"),
		TEXT("capability.pcg.node.configure"), TEXT("capability.pcg.node.comment"),
		TEXT("capability.pcg.node.connect"), TEXT("capability.pcg.graph.instance"),
		TEXT("capability.pcg.graph.generate"), TEXT("capability.pcg.volume.spawn"),
		TEXT("capability.pcg.spline.draw"), TEXT("capability.pcg.subgraph.add"),
		TEXT("capability.pcg.worldgen.capture"), TEXT("capability.pcg.lifecycle.validate")};
	return Coordinates;
}

TArray<FHyperAIPCGCapabilityStatus> FHyperAIStudioPCGContracts::GetCapabilityMatrix()
{
	FHyperAIPCGCapabilityStatus PCG;
	PCG.DelegatedEpicCallables = GetEpicDelegates();
	PCG.CapabilityRequirementCoordinates = GetCapabilityRequirementCoordinates();
	PCG.UniqueCases = {
		TEXT("bounded_loaded_only_exact_graph_component_snapshot"),
		TEXT("independent_value_only_structural_and_generation_ready_validation"),
		TEXT("compound_generate_validate_cleanup_lifecycle_intent")};
	PCG.UnsupportedCases = {
		TEXT("asset_or_map_load_open_or_broad_asset_registry_materialization"),
		TEXT("raw_script_json_python_or_reflection_dispatch"),
		TEXT("process_event_or_arbitrary_class_construction"),
		TEXT("file_network_or_external_process_io"),
		TEXT("unbounded_export_text_tostring_propertybag_or_container_copy"),
		TEXT("recursive_embedded_subgraph_revision_in_v1"),
		TEXT("extra_editor_node_content_revision_in_v1"),
		TEXT("derived_current_pin_type_materialization"),
		TEXT("paging_while_component_task_or_refresh_is_volatile"),
		TEXT("synchronous_generation_cleanup_compile_or_poll"),
		TEXT("non_dry_execution_without_pack_specific_pinned_continuation_and_fresh_validator")};
	PCG.State = TEXT("source_candidate_async_continuation_host_required");
	PCG.Remediation = TEXT("Use Epic PCGToolset/PCGSpatialToolset for all 31 authoring and execution primitives. HyperAI non-dry lifecycle execution remains zero-effect until one generic pinned begin/poll/cancel continuation host has exact pack-specific PCG semantics and a separately owned fresh value validator.");
	return {PCG};
}

bool FHyperAIStudioPCGContracts::IsCanonicalGraphPath(const FString& Path)
{
	using namespace HyperAIStudio::PCG::Private;
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":"))
		|| HasControlCharacter(Path))
	{
		return false;
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioPCGContracts::IsCanonicalComponentPath(const FString& Path)
{
	using namespace HyperAIStudio::PCG::Private;
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| !Path.Contains(TEXT(":")) || Path.Contains(TEXT("*")) || Path.Contains(TEXT("?"))
		|| HasControlCharacter(Path))
	{
		return false;
	}
	const FSoftObjectPath SoftPath(Path);
	if (!SoftPath.IsValid() || SoftPath.GetSubPathUtf8String().IsEmpty()
		|| SoftPath.GetSubPathUtf8String().Len() > MaxPathCharacters)
	{
		return false;
	}
	const FTopLevelAssetPath& AssetPath = SoftPath.GetAssetPath();
	return !AssetPath.IsNull()
		&& AssetPath.GetPackageName().GetStringLength() <= static_cast<uint32>(MaxPathCharacters)
		&& AssetPath.GetAssetName().GetStringLength() <= static_cast<uint32>(MaxNameCharacters);
}

bool FHyperAIStudioPCGContracts::IsCanonicalSha256(const FString& Value)
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

bool FHyperAIStudioPCGContracts::IsSafeOperationId(const FString& Value)
{
	return FHyperAIStudioExtensionRuntime::IsValidOperationId(Value);
}

FString FHyperAIStudioPCGContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists: return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist: return TEXT("does_not_exist");
	default: return TEXT("unknown");
	}
}

bool FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
	const int32 Count,
	const int32 CurrentWork,
	const int32 CurrentBytes,
	const int32 MinimumBytesPerElement,
	FString& OutError)
{
	OutError.Reset();
	if (Count < 0 || Count > MaxContainerElements)
	{
		OutError = TEXT("container_count_exceeded");
		return false;
	}
	if (CurrentWork < 0 || CurrentWork > MaxProjectionWorkUnits
		|| Count > MaxProjectionWorkUnits - CurrentWork)
	{
		OutError = TEXT("container_work_exceeded");
		return false;
	}
	const int64 MinimumBytes = static_cast<int64>(Count) * MinimumBytesPerElement;
	if (MinimumBytesPerElement < 0 || CurrentBytes < 0 || CurrentBytes > MaxProjectionBytes
		|| MinimumBytes > MaxProjectionBytes - CurrentBytes)
	{
		OutError = TEXT("container_byte_budget_exceeded");
		return false;
	}
	return true;
}

bool FHyperAIStudioPCGContracts::AdmitSparseContainerBeforeProjection(
	const int32 Count,
	const int32 SlotCount,
	const int32 CurrentWork,
	const int32 CurrentBytes,
	const int32 MinimumBytesPerElement,
	FString& OutError)
{
	if (!AdmitContainerBeforeProjection(
		Count, CurrentWork, CurrentBytes, MinimumBytesPerElement, OutError))
	{
		return false;
	}
	if (SlotCount < Count || SlotCount > MaxContainerElements * 2)
	{
		OutError = TEXT("sparse_container_slot_count_exceeded");
		return false;
	}
	if (CurrentWork < 0 || CurrentWork > MaxProjectionWorkUnits
		|| SlotCount > MaxProjectionWorkUnits - CurrentWork)
	{
		OutError = TEXT("sparse_container_slot_work_exceeded");
		return false;
	}
	return true;
}

bool FHyperAIStudioPCGContracts::IsPagingStable(const FHyperAIPCGComponentRecord& Component)
{
	return !Component.bPresent || (Component.bPersistedProjectionComplete
		&& Component.bVolatileStateStable && !Component.bGenerating
		&& !Component.bCleaningUp && !Component.bRefreshInProgress);
}

namespace HyperAIStudio::PCG::Private
{
	FString CursorSeal(
		const FString& GraphPath,
		const FString& ComponentPath,
		const FString& PersistedRevision,
		const FString& ComponentFingerprint,
		const int32 PageSize)
	{
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.inspect-cursor.v1"));
		AppendTokenUnchecked(Canonical, GraphPath);
		AppendTokenUnchecked(Canonical, ComponentPath);
		AppendTokenUnchecked(Canonical, PersistedRevision);
		AppendTokenUnchecked(Canonical, ComponentFingerprint);
		AppendTokenUnchecked(Canonical, FString::FromInt(PageSize));
		return HashCanonical(Canonical);
	}
}

FString FHyperAIStudioPCGContracts::BuildCursor(
	const FString& GraphPath,
	const FString& ComponentPath,
	const FString& PersistedRevision,
	const FString& ComponentFingerprint,
	const int32 PageSize,
	const int32 Offset)
{
	if (!IsCanonicalGraphPath(GraphPath)
		|| (!ComponentPath.IsEmpty() && !IsCanonicalComponentPath(ComponentPath))
		|| !IsCanonicalSha256(PersistedRevision)
		|| (!ComponentPath.IsEmpty() && !IsCanonicalSha256(ComponentFingerprint))
		|| PageSize < 1 || PageSize > MaxPageSize || Offset < 0 || Offset > MaxSnapshotItems)
	{
		return {};
	}
	return HyperAIStudio::PCG::Private::CursorSeal(GraphPath, ComponentPath,
		PersistedRevision, ComponentFingerprint, PageSize)
		+ TEXT(".") + FString::FromInt(Offset);
}

bool FHyperAIStudioPCGContracts::ParseCursor(
	const FString& Cursor,
	const FString& GraphPath,
	const FString& ComponentPath,
	const FString& PersistedRevision,
	const FString& ComponentFingerprint,
	const int32 PageSize,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.Len() > MaxCursorCharacters || !IsCanonicalGraphPath(GraphPath)
		|| (!ComponentPath.IsEmpty() && !IsCanonicalComponentPath(ComponentPath))
		|| !IsCanonicalSha256(PersistedRevision)
		|| (!ComponentPath.IsEmpty() && !IsCanonicalSha256(ComponentFingerprint))
		|| PageSize < 1 || PageSize > MaxPageSize)
	{
		return false;
	}
	if (Cursor.IsEmpty()) return true;
	int32 Separator = INDEX_NONE;
	if (!Cursor.FindLastChar(TEXT('.'), Separator) || Separator <= 0
		|| Separator >= Cursor.Len() - 1) return false;
	const FString Seal = Cursor.Left(Separator);
	const FString OffsetText = Cursor.Mid(Separator + 1);
	const FString ExpectedSeal = HyperAIStudio::PCG::Private::CursorSeal(GraphPath,
		ComponentPath, PersistedRevision, ComponentFingerprint, PageSize);
	if (Seal != ExpectedSeal) return false;
	int64 Offset = 0;
	for (const TCHAR Character : OffsetText)
	{
		if (Character < TEXT('0') || Character > TEXT('9')) return false;
		Offset = Offset * 10 + (Character - TEXT('0'));
		if (Offset > MaxSnapshotItems) return false;
	}
	OutOffset = static_cast<int32>(Offset);
	return true;
}

FString FHyperAIStudioPCGContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.inspect.v1|graph_path:string|required|component_path:string|optional|page_size:int32|cursor:string|max_game_thread_ms:int32|max_output_bytes:int32"));
	return Value;
}

FString FHyperAIStudioPCGContracts::LifecyclePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.lifecycle.v1|graph_path:string|component_path:string|base_graph_revision:sha256|base_component_fingerprint:sha256|base_volatile_observation_fingerprint:sha256|lifecycle:generate_validate_cleanup|validation:generation_ready|terminal:clean|async_deadline_ms:int32|force_generate:bool|remove_generated_components:bool"));
	return Value;
}

FString FHyperAIStudioPCGContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.validate.v1|graph_path:string|component_path:string|expected_revision:sha256|expected_component_fingerprint:sha256|policy:string|max_issues:int32|max_game_thread_ms:int32|max_output_bytes:int32"));
	return Value;
}

FString FHyperAIStudioPCGContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.inspect.result.v1|exact_graph|separate_component_persisted_and_volatile|paged_nodes_and_edges|issues|capabilities"));
	return Value;
}

FString FHyperAIStudioPCGContracts::LifecycleResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.lifecycle.result.v1|terminal_phase|fresh_graph_revision|fresh_component_fingerprint|validation_fingerprint|clean_terminal"));
	return Value;
}

FString FHyperAIStudioPCGContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("pcg.validate.result.v1|complete|valid|fresh|policy|revision|validator_fingerprint|issues|capabilities"));
	return Value;
}

const UPCGGraph* FHyperAIStudioPCGContracts::ResolveAlreadyLoadedGraph(const FString& GraphPath)
{
	if (!IsCanonicalGraphPath(GraphPath)) return nullptr;
	return Cast<UPCGGraph>(FSoftObjectPath(GraphPath).ResolveObject());
}

const UPCGComponent* FHyperAIStudioPCGContracts::ResolveAlreadyLoadedComponent(
	const FString& ComponentPath)
{
	if (!IsCanonicalComponentPath(ComponentPath)) return nullptr;
	return Cast<UPCGComponent>(FSoftObjectPath(ComponentPath).ResolveObject());
}

FString FHyperAIStudioPCGContracts::ComputeNodeFingerprint(
	const FHyperAIPCGNodeRecord& Node)
{
	using namespace HyperAIStudio::PCG::Private;
	if (Node.InputPins.Num() > MaxPinsPerNode || Node.OutputPins.Num() > MaxPinsPerNode
		|| Node.NodeId.IsEmpty() || Node.NodeId.Len() > MaxPathCharacters
		|| Node.Role.Len() > 32 || Node.AuthoredTitle.Len() > MaxNameCharacters
		|| Node.Comment.Len() > MaxStringCharacters
		|| Node.SettingsInterfaceClass.Len() > MaxPathCharacters
		|| Node.SettingsClass.Len() > MaxPathCharacters
		|| EstimateNodeBytes(Node) > MaxProjectionBytes)
	{
		return {};
	}
	auto PinIsBounded = [](const FHyperAIPCGPinRecord& Pin)
	{
		return !Pin.PinId.IsEmpty()
			&& Pin.PinId.Len() <= FHyperAIStudioPCGContracts::MaxPathCharacters
			&& Pin.Direction.Len() <= 16
			&& Pin.Label.Len() <= FHyperAIStudioPCGContracts::MaxNameCharacters
			&& Pin.AllowedTypeFingerprint.Len() <= 71
			&& Pin.CurrentTypeFingerprint.Len() <= 71
			&& Pin.PropertiesFingerprint.Len() <= 71;
	};
	for (const FHyperAIPCGPinRecord& Pin : Node.InputPins)
	{
		if (!PinIsBounded(Pin)) return {};
	}
	for (const FHyperAIPCGPinRecord& Pin : Node.OutputPins)
	{
		if (!PinIsBounded(Pin)) return {};
	}
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.detached-node.v1"));
	AppendTokenUnchecked(Canonical, Node.NodeId);
	AppendTokenUnchecked(Canonical, Node.Role);
	AppendTokenUnchecked(Canonical, Node.AuthoredTitle);
	AppendTokenUnchecked(Canonical, Node.Comment);
	AppendTokenUnchecked(Canonical, FString::FromInt(Node.PositionX));
	AppendTokenUnchecked(Canonical, FString::FromInt(Node.PositionY));
	AppendTokenUnchecked(Canonical, Node.bHidden ? TEXT("hidden") : TEXT("visible"));
	AppendTokenUnchecked(Canonical, Node.SettingsInterfaceClass);
	AppendTokenUnchecked(Canonical, Node.SettingsClass);
	AppendTokenUnchecked(Canonical, Node.SettingsFingerprint);
	AppendTokenUnchecked(Canonical, Node.bSettingsProjectionComplete
		? TEXT("settings_complete") : TEXT("settings_incomplete"));
	AppendTokenUnchecked(Canonical, Node.NodePropertiesFingerprint);
	auto AppendPin = [&](const FHyperAIPCGPinRecord& Pin)
	{
		AppendTokenUnchecked(Canonical, Pin.PinId);
		AppendTokenUnchecked(Canonical, Pin.Direction);
		AppendTokenUnchecked(Canonical, Pin.Label);
		AppendTokenUnchecked(Canonical, FString::FromInt(Pin.Usage));
		AppendTokenUnchecked(Canonical, FString::FromInt(Pin.Status));
		AppendTokenUnchecked(Canonical, Pin.bAllowsMultipleConnections ? TEXT("multi_connections") : TEXT("single_connection"));
		AppendTokenUnchecked(Canonical, Pin.bAllowsMultipleData ? TEXT("multi_data") : TEXT("single_data"));
		AppendTokenUnchecked(Canonical, Pin.bInvisible ? TEXT("invisible") : TEXT("visible"));
		AppendTokenUnchecked(Canonical, FString::FromInt(Pin.EdgeCount));
		AppendTokenUnchecked(Canonical, Pin.AllowedTypeFingerprint);
		AppendTokenUnchecked(Canonical, Pin.CurrentTypeFingerprint);
		AppendTokenUnchecked(Canonical, Pin.PropertiesFingerprint);
	};
	for (const FHyperAIPCGPinRecord& Pin : Node.InputPins) AppendPin(Pin);
	for (const FHyperAIPCGPinRecord& Pin : Node.OutputPins) AppendPin(Pin);
	return HashCanonical(Canonical);
}

FString FHyperAIStudioPCGContracts::ComputeEdgeFingerprint(
	const FHyperAIStudioPCGEdgeState& Edge)
{
	using namespace HyperAIStudio::PCG::Private;
	if (Edge.FromNodeId.IsEmpty() || Edge.FromPinId.IsEmpty()
		|| Edge.ToNodeId.IsEmpty() || Edge.ToPinId.IsEmpty()
		|| Edge.FromNodeId.Len() > MaxPathCharacters
		|| Edge.FromPinId.Len() > MaxPathCharacters
		|| Edge.ToNodeId.Len() > MaxPathCharacters
		|| Edge.ToPinId.Len() > MaxPathCharacters
		|| EstimateEdgeBytes(Edge) > MaxProjectionBytes)
	{
		return {};
	}
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.detached-edge.v1"));
	AppendTokenUnchecked(Canonical, Edge.FromNodeId);
	AppendTokenUnchecked(Canonical, Edge.FromPinId);
	AppendTokenUnchecked(Canonical, Edge.ToNodeId);
	AppendTokenUnchecked(Canonical, Edge.ToPinId);
	return HashCanonical(Canonical);
}

FString FHyperAIStudioPCGContracts::ComputePersistedRevision(
	const FHyperAIStudioPCGValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::PCG::Private;
	const FHyperAIPCGGraphRecord& Graph = Snapshot.Graph;
	if (!IsCanonicalGraphPath(Graph.TargetPath)
		|| !IsCanonicalSha256(Graph.GraphPropertiesFingerprint)
		|| !IsCanonicalSha256(Graph.UserParametersFingerprint)
		|| Snapshot.Nodes.Num() > MaxNodes || Snapshot.Edges.Num() > MaxEdges)
	{
		return {};
	}
	int64 EstimatedMaterializedBytes = 2048;
	int64 EstimatedWork = Snapshot.Nodes.Num() + Snapshot.Edges.Num();
	for (const FHyperAIPCGNodeRecord& Node : Snapshot.Nodes)
	{
		EstimatedMaterializedBytes += EstimateNodeBytes(Node);
		EstimatedWork += Node.InputPins.Num() + Node.OutputPins.Num();
	}
	for (const FHyperAIStudioPCGEdgeState& Edge : Snapshot.Edges)
	{
		EstimatedMaterializedBytes += EstimateEdgeBytes(Edge);
	}
	if (EstimatedMaterializedBytes > MaxProjectionBytes
		|| EstimatedWork > MaxProjectionWorkUnits)
	{
		return {};
	}
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.persisted-graph-revision.v1"));
	AppendTokenUnchecked(Canonical, Graph.TargetPath);
	AppendTokenUnchecked(Canonical, Graph.ClassPath);
	AppendTokenUnchecked(Canonical, Graph.PackageName);
	AppendTokenUnchecked(Canonical, Graph.DiskExistence);
	AppendTokenUnchecked(Canonical, Graph.PackageSavedHash);
	AppendTokenUnchecked(Canonical, FString::Printf(TEXT("%lld"), Graph.DiskSize));
	AppendTokenUnchecked(Canonical, Graph.bLoaded ? TEXT("loaded") : TEXT("not_loaded"));
	AppendTokenUnchecked(Canonical, Graph.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
	AppendTokenUnchecked(Canonical, Graph.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("new_object"));
	AppendTokenUnchecked(Canonical, Graph.GraphPropertiesFingerprint);
	AppendTokenUnchecked(Canonical, Graph.UserParametersFingerprint);
	AppendTokenUnchecked(Canonical, FString::FromInt(Graph.NodeCount));
	AppendTokenUnchecked(Canonical, FString::FromInt(Graph.EdgeCount));
	AppendTokenUnchecked(Canonical, FString::FromInt(Graph.UserParameterCount));
	AppendTokenUnchecked(Canonical, FString::FromInt(Graph.EmbeddedSubgraphCount));
	AppendTokenUnchecked(Canonical, FString::FromInt(Graph.CommentCount));
	for (const FHyperAIPCGNodeRecord& Node : Snapshot.Nodes)
	{
		if (!IsCanonicalSha256(Node.SemanticFingerprint)
			|| Node.SemanticFingerprint != ComputeNodeFingerprint(Node)) return {};
		AppendTokenUnchecked(Canonical, Node.SemanticFingerprint);
	}
	TArray<FString> EdgeIds;
	EdgeIds.Reserve(Snapshot.Edges.Num());
	for (const FHyperAIStudioPCGEdgeState& Edge : Snapshot.Edges)
	{
		if (!IsCanonicalSha256(Edge.EdgeId)
			|| Edge.EdgeId != ComputeEdgeFingerprint(Edge)) return {};
		EdgeIds.Add(Edge.EdgeId);
	}
	EdgeIds.Sort();
	for (const FString& EdgeId : EdgeIds) AppendTokenUnchecked(Canonical, EdgeId);
	return HashCanonical(Canonical);
}

FString FHyperAIStudioPCGContracts::ComputeComponentPersistedFingerprint(
	const FHyperAIPCGComponentRecord& Component)
{
	using namespace HyperAIStudio::PCG::Private;
	if (!Component.bPresent
		|| !IsCanonicalComponentPath(Component.ComponentPath)
		|| !IsCanonicalGraphPath(Component.GraphPath)
		|| Component.ComponentClassPath.IsEmpty()
		|| Component.ComponentClassPath.Len() > MaxPathCharacters
		|| Component.OwnerPath.IsEmpty() || Component.OwnerPath.Len() > MaxPathCharacters
		|| Component.GraphInstancePath.IsEmpty()
		|| Component.GraphInstancePath.Len() > MaxPathCharacters
		|| Component.GraphInterfacePath.IsEmpty()
		|| Component.GraphInterfacePath.Len() > MaxPathCharacters
		|| Component.PackageName.IsEmpty() || Component.PackageName.Len() > MaxPathCharacters
		|| (Component.DiskExistence != TEXT("exists")
			&& Component.DiskExistence != TEXT("does_not_exist"))
		|| (Component.DiskExistence == TEXT("exists")
			&& (!IsLowerHexOfLength(Component.PackageSavedHash, 40)
				|| Component.DiskSize < 0))
		|| (Component.DiskExistence == TEXT("does_not_exist")
			&& (!Component.PackageSavedHash.IsEmpty() || Component.DiskSize != -1))
		|| !IsCanonicalSha256(Component.ComponentPropertiesFingerprint)
		|| !IsCanonicalSha256(Component.GraphInstancePropertiesFingerprint)
		|| !IsCanonicalSha256(Component.InstanceParametersFingerprint)
		|| !IsCanonicalSha256(Component.InstanceOverrideMaskFingerprint)
		|| !IsCanonicalSha256(Component.SchedulingPolicyFingerprint)
		|| !IsCanonicalSha256(Component.OwnerTransformFingerprint)
		|| Component.InstanceParameterCount < 0
		|| Component.InstanceParameterCount > MaxContainerElements
		|| Component.InstanceOverrideCount < 0
		|| Component.InstanceOverrideCount > MaxContainerElements
		|| EstimateComponentBytes(Component) > MaxProjectionBytes)
	{
		return {};
	}
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.component-persisted.v1"));
	AppendTokenUnchecked(Canonical, Component.ComponentPath);
	AppendTokenUnchecked(Canonical, Component.ComponentClassPath);
	AppendTokenUnchecked(Canonical, Component.OwnerPath);
	AppendTokenUnchecked(Canonical, Component.GraphPath);
	AppendTokenUnchecked(Canonical, Component.GraphInstancePath);
	AppendTokenUnchecked(Canonical, Component.GraphInterfacePath);
	AppendTokenUnchecked(Canonical, Component.PackageName);
	AppendTokenUnchecked(Canonical, Component.DiskExistence);
	AppendTokenUnchecked(Canonical, Component.PackageSavedHash);
	AppendTokenUnchecked(Canonical, FString::Printf(TEXT("%lld"), Component.DiskSize));
	AppendTokenUnchecked(Canonical, Component.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
	AppendTokenUnchecked(Canonical, Component.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("new_object"));
	AppendTokenUnchecked(Canonical, Component.bActivated ? TEXT("activated") : TEXT("deactivated"));
	AppendTokenUnchecked(Canonical, FString::FromInt(Component.Seed));
	AppendTokenUnchecked(Canonical, FString::FromInt(Component.GenerationTrigger));
	AppendTokenUnchecked(Canonical, Component.bPartitioned ? TEXT("partitioned") : TEXT("not_partitioned"));
	AppendTokenUnchecked(Canonical, Component.ComponentPropertiesFingerprint);
	AppendTokenUnchecked(Canonical, Component.GraphInstancePropertiesFingerprint);
	AppendTokenUnchecked(Canonical, Component.InstanceParametersFingerprint);
	AppendTokenUnchecked(Canonical, FString::FromInt(Component.InstanceParameterCount));
	AppendTokenUnchecked(Canonical, Component.InstanceOverrideMaskFingerprint);
	AppendTokenUnchecked(Canonical, FString::FromInt(Component.InstanceOverrideCount));
	AppendTokenUnchecked(Canonical, Component.SchedulingPolicyFingerprint);
	AppendTokenUnchecked(Canonical, Component.OwnerTransformFingerprint);
	return HashCanonical(Canonical);
}

FString FHyperAIStudioPCGContracts::ComputeComponentVolatileObservationFingerprint(
	const FHyperAIPCGComponentRecord& Component)
{
	using namespace HyperAIStudio::PCG::Private;
	if (!Component.bPresent || Component.GenerationTaskId.Len() > 32
		|| Component.CleanupTaskId.Len() > 32)
	{
		return {};
	}
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.component-observation.v1"));
	AppendTokenUnchecked(Canonical, Component.bGenerated ? TEXT("generated") : TEXT("not_generated"));
	AppendTokenUnchecked(Canonical, Component.bDirtyGenerated ? TEXT("dirty") : TEXT("clean"));
	AppendTokenUnchecked(Canonical, Component.bGenerating ? TEXT("generating") : TEXT("idle_generation"));
	AppendTokenUnchecked(Canonical, Component.bCleaningUp ? TEXT("cleaning") : TEXT("idle_cleanup"));
	AppendTokenUnchecked(Canonical, Component.bRefreshInProgress ? TEXT("refreshing") : TEXT("idle_refresh"));
	AppendTokenUnchecked(Canonical, Component.GenerationTaskId);
	AppendTokenUnchecked(Canonical, Component.CleanupTaskId);
	return HashCanonical(Canonical);
}

bool FHyperAIStudioPCGContracts::ValidateValueSnapshot(
	const FHyperAIStudioPCGValueSnapshot& Snapshot,
	const FString& Policy,
	const int32 MaxIssuesValue,
	TArray<FHyperAIPCGIssue>& OutIssues,
	FString& OutValidatorFingerprint)
{
	using namespace HyperAIStudio::PCG::Private;
	OutIssues.Reset();
	OutValidatorFingerprint.Reset();
	if ((Policy != TEXT("structural") && Policy != TEXT("generation_ready"))
		|| MaxIssuesValue < 1 || MaxIssuesValue > MaxIssues
		|| Snapshot.Nodes.Num() > MaxNodes || Snapshot.Edges.Num() > MaxEdges
		|| Snapshot.CaptureIssues.Num() > MaxIssues)
	{
		return false;
	}
	if (!IsCanonicalGraphPath(Snapshot.Graph.TargetPath)
		|| Snapshot.Graph.ClassPath.Len() > MaxPathCharacters
		|| Snapshot.Graph.PackageName.Len() > MaxPathCharacters
		|| Snapshot.Component.ComponentPath.Len() > MaxPathCharacters
		|| Snapshot.Component.ComponentClassPath.Len() > MaxPathCharacters
		|| Snapshot.Component.OwnerPath.Len() > MaxPathCharacters
		|| Snapshot.Component.GraphPath.Len() > MaxPathCharacters
		|| Snapshot.Component.GraphInstancePath.Len() > MaxPathCharacters
		|| Snapshot.Component.GraphInterfacePath.Len() > MaxPathCharacters
		|| Snapshot.Component.PackageName.Len() > MaxPathCharacters
		|| Snapshot.Component.GenerationTaskId.Len() > 32
		|| Snapshot.Component.CleanupTaskId.Len() > 32)
	{
		return false;
	}
	int64 PreflightBytes = 4096;
	int64 PreflightWork = Snapshot.Nodes.Num() + Snapshot.Edges.Num()
		+ Snapshot.CaptureIssues.Num();
	if (Snapshot.Component.bPresent)
	{
		PreflightBytes += EstimateComponentBytes(Snapshot.Component);
		++PreflightWork;
	}
	for (const FHyperAIPCGNodeRecord& Node : Snapshot.Nodes)
	{
		if (Node.InputPins.Num() > MaxPinsPerNode || Node.OutputPins.Num() > MaxPinsPerNode)
		{
			return false;
		}
		if (ComputeNodeFingerprint(Node).IsEmpty()) return false;
		PreflightBytes += EstimateNodeBytes(Node);
		PreflightWork += Node.InputPins.Num() + Node.OutputPins.Num();
	}
	for (const FHyperAIStudioPCGEdgeState& Edge : Snapshot.Edges)
	{
		if (ComputeEdgeFingerprint(Edge).IsEmpty()) return false;
		PreflightBytes += EstimateEdgeBytes(Edge);
	}
	for (const FHyperAIPCGIssue& Issue : Snapshot.CaptureIssues)
	{
		if (Issue.Code.Len() > FHyperAIStudioDomainLimits::MaxStatusCodeChars
			|| Issue.Severity.Len() > 16 || Issue.StableId.Len() > 71
			|| Issue.Subject.Len() > MaxPathCharacters
			|| Issue.Detail.Len() > FHyperAIStudioDomainLimits::MaxDiagnosticChars)
		{
			return false;
		}
		PreflightBytes += EstimateIssueBytes(Issue);
	}
	if (PreflightBytes > MaxProjectionBytes || PreflightWork > MaxProjectionWorkUnits)
	{
		return false;
	}
	bool bIssueOverflow = false;
	auto Add = [&](const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail)
	{
		if (OutIssues.Num() >= MaxIssuesValue)
		{
			bIssueOverflow = true;
			return;
		}
		OutIssues.Add(MakeIssue(Code, Severity, Subject, Detail));
	};
	for (const FHyperAIPCGIssue& Capture : Snapshot.CaptureIssues)
	{
		Add(Capture.Code, Capture.Severity, Capture.Subject, Capture.Detail);
	}
	if (!Snapshot.Graph.bRevisionComplete || !IsCanonicalSha256(Snapshot.Graph.PersistedRevision))
	{
		Add(TEXT("revision_incomplete"), TEXT("error"), Snapshot.Graph.TargetPath,
			TEXT("Persisted graph identity is incomplete; validation can never admit this capture."));
	}
	if (!Snapshot.Graph.bLoaded || Snapshot.Graph.ClassPath.IsEmpty()
		|| Snapshot.Graph.PackageName.IsEmpty()
		|| (Snapshot.Graph.DiskExistence != TEXT("exists")
			&& Snapshot.Graph.DiskExistence != TEXT("does_not_exist"))
		|| (Snapshot.Graph.DiskExistence == TEXT("exists")
			&& (!IsLowerHexOfLength(Snapshot.Graph.PackageSavedHash, 40)
				|| Snapshot.Graph.DiskSize < 0))
		|| (Snapshot.Graph.DiskExistence == TEXT("does_not_exist")
			&& (!Snapshot.Graph.PackageSavedHash.IsEmpty() || Snapshot.Graph.DiskSize != -1)))
	{
		Add(TEXT("graph_persisted_identity_incomplete"), TEXT("error"),
			Snapshot.Graph.TargetPath,
			TEXT("Detached graph loaded/package/disk identity is outside the closed exact snapshot contract."));
	}
	TSet<FString> NodeIds;
	TSet<FString> PinIds;
	TSet<FString> InputPinIds;
	TSet<FString> OutputPinIds;
	TMap<FString, int32> DeclaredEdgeCounts;
	TMap<FString, int32> ObservedEdgeCounts;
	int32 InputBoundaryCount = 0;
	int32 OutputBoundaryCount = 0;
	for (const FHyperAIPCGNodeRecord& Node : Snapshot.Nodes)
	{
		if (Node.NodeId.IsEmpty() || NodeIds.Contains(Node.NodeId))
		{
			Add(TEXT("duplicate_node_identity"), TEXT("error"), Node.NodeId,
				TEXT("Every captured PCG node requires one unique closed identity."));
		}
		NodeIds.Add(Node.NodeId);
		if (Node.Role != TEXT("graph_input") && Node.Role != TEXT("graph_output")
			&& Node.Role != TEXT("node"))
		{
			Add(TEXT("invalid_node_role"), TEXT("error"), Node.NodeId,
				TEXT("Detached node role is outside the closed PCG snapshot schema."));
		}
		if (Node.Role == TEXT("graph_input")) ++InputBoundaryCount;
		if (Node.Role == TEXT("graph_output")) ++OutputBoundaryCount;
		if (!Node.bSettingsProjectionComplete || !IsCanonicalSha256(Node.SettingsFingerprint)
			|| !IsCanonicalSha256(Node.NodePropertiesFingerprint)
			|| !IsCanonicalSha256(Node.SemanticFingerprint)
			|| Node.SemanticFingerprint != ComputeNodeFingerprint(Node))
		{
			Add(TEXT("node_settings_projection_incomplete"), TEXT("error"), Node.NodeId,
				TEXT("Node settings or semantic projection is incomplete."));
		}
		auto InspectPin = [&](const FHyperAIPCGPinRecord& Pin, const TCHAR* ExpectedDirection)
		{
			if (Pin.PinId.IsEmpty() || PinIds.Contains(Pin.PinId))
			{
				Add(TEXT("duplicate_pin_identity"), TEXT("error"), Pin.PinId,
					TEXT("Every captured PCG pin requires one unique closed identity."));
			}
			PinIds.Add(Pin.PinId);
			if (Pin.Direction != ExpectedDirection || Pin.EdgeCount < 0 || Pin.EdgeCount > MaxEdges
				|| !IsCanonicalSha256(Pin.AllowedTypeFingerprint)
				|| !IsCanonicalSha256(Pin.PropertiesFingerprint))
			{
				Add(TEXT("invalid_pin_record"), TEXT("error"), Pin.PinId,
					TEXT("Pin direction, type, or bounded edge count is invalid."));
			}
			if (Pin.Direction == TEXT("input")) InputPinIds.Add(Pin.PinId);
			if (Pin.Direction == TEXT("output")) OutputPinIds.Add(Pin.PinId);
			DeclaredEdgeCounts.Add(Pin.PinId, Pin.EdgeCount);
			ObservedEdgeCounts.Add(Pin.PinId, 0);
		};
		for (const FHyperAIPCGPinRecord& Pin : Node.InputPins) InspectPin(Pin, TEXT("input"));
		for (const FHyperAIPCGPinRecord& Pin : Node.OutputPins) InspectPin(Pin, TEXT("output"));
	}
	if (InputBoundaryCount != 1 || OutputBoundaryCount != 1)
	{
		Add(TEXT("graph_boundary_cardinality"), TEXT("error"), Snapshot.Graph.TargetPath,
			TEXT("A closed PCG graph requires exactly one graph_input and graph_output node."));
	}
	TSet<FString> EdgeIds;
	for (const FHyperAIStudioPCGEdgeState& Edge : Snapshot.Edges)
	{
		if (!IsCanonicalSha256(Edge.EdgeId)
			|| Edge.EdgeId != ComputeEdgeFingerprint(Edge)
			|| EdgeIds.Contains(Edge.EdgeId)
			|| !NodeIds.Contains(Edge.FromNodeId) || !NodeIds.Contains(Edge.ToNodeId)
			|| !OutputPinIds.Contains(Edge.FromPinId) || !InputPinIds.Contains(Edge.ToPinId))
		{
			Add(TEXT("dangling_or_duplicate_edge"), TEXT("error"), Edge.EdgeId,
				TEXT("Each edge must uniquely connect one captured output pin to one captured input pin."));
		}
		EdgeIds.Add(Edge.EdgeId);
		if (int32* Count = ObservedEdgeCounts.Find(Edge.FromPinId)) ++*Count;
		if (int32* Count = ObservedEdgeCounts.Find(Edge.ToPinId)) ++*Count;
	}
	for (const TPair<FString, int32>& Pair : DeclaredEdgeCounts)
	{
		if (ObservedEdgeCounts.FindRef(Pair.Key) != Pair.Value)
		{
			Add(TEXT("pin_edge_count_mismatch"), TEXT("error"), Pair.Key,
				TEXT("Pin edge cardinality does not match the closed edge inventory."));
		}
	}
	if (Snapshot.Graph.NodeCount != Snapshot.Nodes.Num()
		|| Snapshot.Graph.EdgeCount != Snapshot.Edges.Num())
	{
		Add(TEXT("graph_cardinality_mismatch"), TEXT("error"), Snapshot.Graph.TargetPath,
			TEXT("Graph record cardinalities do not match detached node/edge values."));
	}
	const FString RecomputedRevision = ComputePersistedRevision(Snapshot);
	if (!IsCanonicalSha256(RecomputedRevision)
		|| RecomputedRevision != Snapshot.Graph.PersistedRevision)
	{
		Add(TEXT("persisted_revision_value_mismatch"), TEXT("error"),
			Snapshot.Graph.TargetPath,
			TEXT("Detached graph, node, pin, edge, settings, parameter, package, or dirty values do not reproduce the asserted graph CAS."));
	}
	const FHyperAIPCGComponentRecord& Component = Snapshot.Component;
	if (Component.bPresent)
	{
		const FString RecomputedComponentFingerprint =
			ComputeComponentPersistedFingerprint(Component);
		if (!Component.bPersistedProjectionComplete
			|| !IsCanonicalSha256(Component.PersistedFingerprint)
			|| !IsCanonicalSha256(RecomputedComponentFingerprint)
			|| RecomputedComponentFingerprint != Component.PersistedFingerprint)
		{
			Add(TEXT("component_persisted_value_mismatch"), TEXT("error"),
				Component.ComponentPath,
				TEXT("Detached component, graph-instance, override-mask, scheduling, transform, package, or dirty values do not reproduce the asserted component CAS."));
		}
		const FString RecomputedObservation =
			ComputeComponentVolatileObservationFingerprint(Component);
		const bool bExpectedStable = !Component.bGenerating
			&& !Component.bCleaningUp && !Component.bRefreshInProgress;
		if (!IsCanonicalSha256(RecomputedObservation)
			|| RecomputedObservation != Component.VolatileObservationFingerprint
			|| Component.bVolatileStateStable != bExpectedStable)
		{
			Add(TEXT("component_observation_value_mismatch"), TEXT("error"),
				Component.ComponentPath,
				TEXT("Detached generation, cleanup, refresh, or task observations do not reproduce their separate volatile seal."));
		}
		if (Component.GraphPath != Snapshot.Graph.TargetPath)
		{
			Add(TEXT("component_graph_identity_mismatch"), TEXT("error"),
				Component.ComponentPath,
				TEXT("The component does not bind the exact requested PCG graph identity."));
		}
	}
	else if (!Component.ComponentPath.IsEmpty()
		|| !Component.ComponentClassPath.IsEmpty() || !Component.OwnerPath.IsEmpty()
		|| !Component.GraphPath.IsEmpty() || !Component.GraphInstancePath.IsEmpty()
		|| !Component.GraphInterfacePath.IsEmpty() || !Component.PackageName.IsEmpty()
		|| !Component.PackageSavedHash.IsEmpty()
		|| !Component.ComponentPropertiesFingerprint.IsEmpty()
		|| !Component.GraphInstancePropertiesFingerprint.IsEmpty()
		|| !Component.InstanceParametersFingerprint.IsEmpty()
		|| Component.InstanceParameterCount != 0
		|| !Component.InstanceOverrideMaskFingerprint.IsEmpty()
		|| Component.InstanceOverrideCount != 0
		|| !Component.SchedulingPolicyFingerprint.IsEmpty()
		|| !Component.OwnerTransformFingerprint.IsEmpty()
		|| !Component.PersistedFingerprint.IsEmpty()
		|| !Component.VolatileObservationFingerprint.IsEmpty()
		|| Component.DiskExistence != TEXT("unknown") || Component.DiskSize != -1
		|| Component.bWasLoadedFromDisk || Component.bPackageDirty
		|| Component.bActivated || Component.bGenerated || Component.bDirtyGenerated
		|| Component.bGenerating || Component.bCleaningUp
		|| Component.bRefreshInProgress || !Component.bVolatileStateStable
		|| Component.Seed != 0 || Component.GenerationTrigger != 0
		|| Component.bPartitioned
		|| !Component.GenerationTaskId.IsEmpty() || !Component.CleanupTaskId.IsEmpty())
	{
		Add(TEXT("component_absence_value_mismatch"), TEXT("error"),
			Snapshot.Graph.TargetPath,
			TEXT("An absent optional component must not carry persisted or volatile component evidence."));
	}
	if (Policy == TEXT("generation_ready"))
	{
		if (!Component.bPresent || !Component.bPersistedProjectionComplete
			|| !IsCanonicalSha256(Component.PersistedFingerprint))
		{
			Add(TEXT("component_exact_state_required"), TEXT("error"), Component.ComponentPath,
				TEXT("Generation-ready validation requires a complete exact loaded PCG component."));
		}
		if (!IsPagingStable(Component))
		{
			Add(TEXT("component_not_terminal"), TEXT("error"), Component.ComponentPath,
				TEXT("Generation-ready validation never waits; generation, cleanup, and refresh must be idle."));
		}
		if (!Component.bActivated)
		{
			Add(TEXT("component_not_activated"), TEXT("error"), Component.ComponentPath,
				TEXT("The loaded PCG component is not activated."));
		}
	}
	if (bIssueOverflow)
	{
		return false;
	}
	TArray<FString> IssueIds;
	IssueIds.Reserve(OutIssues.Num());
	for (const FHyperAIPCGIssue& Issue : OutIssues) IssueIds.Add(Issue.StableId);
	IssueIds.Sort();
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.value-validator.v1"));
	AppendTokenUnchecked(Canonical, Policy);
	AppendTokenUnchecked(Canonical, Snapshot.Graph.PersistedRevision);
	AppendTokenUnchecked(Canonical, Snapshot.Component.PersistedFingerprint);
	for (const FString& IssueId : IssueIds) AppendTokenUnchecked(Canonical, IssueId);
	OutValidatorFingerprint = HashCanonical(Canonical);
	return IsCanonicalSha256(OutValidatorFingerprint);
}

bool FHyperAIStudioPCGContracts::CaptureExact(
	const FString& GraphPath,
	const FString& ComponentPath,
	const int32 MaxWorkMs,
	FHyperAIStudioPCGValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::PCG::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	auto Fail = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		OutStatus = Status;
		OutDiagnostic = Diagnostic;
		return false;
	};
	if (!IsInGameThread())
	{
		return Fail(TEXT("game_thread_required"),
			TEXT("Exact PCG capture is a bounded read on Unreal's game thread."));
	}
	if (!IsCanonicalGraphPath(GraphPath)
		|| (!ComponentPath.IsEmpty() && !IsCanonicalComponentPath(ComponentPath)))
	{
		return Fail(TEXT("invalid_exact_path"),
			TEXT("Use one canonical /Game graph object path and at most one exact component subobject path."));
	}
	if (MaxWorkMs < 1 || MaxWorkMs > MaxReadGameThreadMs)
	{
		return Fail(TEXT("invalid_capture_bound"), TEXT("Game-thread capture bound is outside the closed contract."));
	}
	const UPCGGraph* Graph = ResolveAlreadyLoadedGraph(GraphPath);
	if (!Graph)
	{
		return Fail(TEXT("graph_not_already_loaded"),
			TEXT("The exact PCG graph is not already loaded; HyperAI never loads or opens it."));
	}
	const UPCGComponent* Component = nullptr;
	if (!ComponentPath.IsEmpty())
	{
		Component = ResolveAlreadyLoadedComponent(ComponentPath);
		if (!Component)
		{
			return Fail(TEXT("component_not_already_loaded"),
				TEXT("The exact PCG component is not already loaded; HyperAI never searches or loads a map."));
		}
	}
	const double Deadline = FPlatformTime::Seconds() + MaxWorkMs / 1000.0;
	FProjectionContext GraphContext;
	GraphContext.AbsoluteDeadlineSeconds = Deadline;
	CaptureGraphState(Graph, GraphPath, GraphContext, OutSnapshot);
	if (GraphContext.bComplete)
	{
		OutSnapshot.Graph.PersistedRevision = ComputePersistedRevision(OutSnapshot);
		OutSnapshot.Graph.bRevisionComplete = IsCanonicalSha256(OutSnapshot.Graph.PersistedRevision);
		if (!OutSnapshot.Graph.bRevisionComplete)
		{
			GraphContext.MarkIncomplete(TEXT("persisted_revision_hash_failed"));
		}
	}
	if (!GraphContext.bComplete)
	{
		OutSnapshot.Graph.bRevisionComplete = false;
		OutSnapshot.Graph.PersistedRevision.Reset();
		AddCaptureIssue(OutSnapshot, TEXT("graph_revision_incomplete"), TEXT("error"),
			GraphPath, GraphContext.FirstFailure.IsEmpty()
				? TEXT("Graph projection did not produce complete CAS evidence.")
				: GraphContext.FirstFailure);
	}
	FProjectionContext ComponentContext;
	ComponentContext.AbsoluteDeadlineSeconds = Deadline;
	CaptureComponentState(Component, Graph, GraphPath, ComponentPath,
		ComponentContext, OutSnapshot.Component);
	if (!ComponentContext.bComplete)
	{
		AddCaptureIssue(OutSnapshot, TEXT("component_projection_incomplete"), TEXT("error"),
			ComponentPath, ComponentContext.FirstFailure.IsEmpty()
				? TEXT("Component projection did not produce complete CAS evidence.")
				: ComponentContext.FirstFailure);
	}
	OutSnapshot.bComplete = OutSnapshot.Graph.bRevisionComplete
		&& (ComponentPath.IsEmpty() || OutSnapshot.Component.bPersistedProjectionComplete)
		&& GraphContext.bComplete && ComponentContext.bComplete;
	OutStatus = OutSnapshot.bComplete ? TEXT("exact_loaded_snapshot") : TEXT("revision_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured one bounded already-loaded PCG graph and optional component without loading, opening, scanning, or executing.")
		: TEXT("Loaded values were inspected, but unsupported, volatile, unknown, truncated, or over-budget evidence prevents complete revision admission.");
	return true;
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioPCGContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.pcg.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_pcg_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_pcg_apply_plan"), MutationVariantId,
			LifecyclePayloadTypeId, LifecyclePayloadSchemaFingerprint(), LifecycleResultTypeId,
			LifecycleResultSchemaFingerprint(), EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_pcg_validate"), ValidateVariantId,
			ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(), ValidateResultTypeId,
			ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FString FHyperAIStudioPCGInspectPayload::GetTypeId() const
{
	return FHyperAIStudioPCGContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioPCGInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPCGContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPCGInspectPayload::GetBoundedByteSize() const
{
	const int64 Size = 96ll + 2ll * (Request.GraphPath.Len()
		+ Request.ComponentPath.Len() + Request.Cursor.Len());
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioPCGValidatePayload::GetTypeId() const
{
	return FHyperAIStudioPCGContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioPCGValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPCGContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioPCGValidatePayload::GetBoundedByteSize() const
{
	const int64 Size = 128ll + 2ll * (Request.GraphPath.Len()
		+ Request.ComponentPath.Len() + Request.ExpectedPersistedRevision.Len()
		+ Request.ExpectedComponentFingerprint.Len() + Request.Policy.Len());
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioPCGLifecyclePayload::GetTypeId() const
{
	return FHyperAIStudioPCGContracts::LifecyclePayloadTypeId;
}

FString FHyperAIStudioPCGLifecyclePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPCGContracts::LifecyclePayloadSchemaFingerprint();
}

int32 FHyperAIStudioPCGLifecyclePayload::GetBoundedByteSize() const
{
	const int64 Size = 160ll + 2ll * (GraphPath.Len() + ComponentPath.Len()
		+ BasePersistedRevision.Len() + BaseComponentFingerprint.Len()
		+ BaseVolatileObservationFingerprint.Len()
		+ Lifecycle.Len() + ValidationPolicy.Len() + ExpectedTerminalState.Len()
		+ SemanticFingerprint.Len()) + sizeof(AsyncDeadlineMs);
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioPCGLifecyclePayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioPCGLifecyclePayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe>();
	Clone->GraphPath = GraphPath;
	Clone->ComponentPath = ComponentPath;
	Clone->BasePersistedRevision = BasePersistedRevision;
	Clone->BaseComponentFingerprint = BaseComponentFingerprint;
	Clone->BaseVolatileObservationFingerprint = BaseVolatileObservationFingerprint;
	Clone->Lifecycle = Lifecycle;
	Clone->ValidationPolicy = ValidationPolicy;
	Clone->ExpectedTerminalState = ExpectedTerminalState;
	Clone->AsyncDeadlineMs = AsyncDeadlineMs;
	Clone->bForceGenerate = bForceGenerate;
	Clone->bRemoveGeneratedComponentsOnCleanup = bRemoveGeneratedComponentsOnCleanup;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioPCGInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioPCGContracts::InspectResultTypeId;
}

FString FHyperAIStudioPCGInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPCGContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioPCGInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::PCG::Private;
	int64 Size = 2048ll + 2ll * (Report.Status.Len() + Report.Diagnostic.Len()
		+ Report.NextCursor.Len())
		+ EstimateGraphBytes(Report.Graph) + EstimateComponentBytes(Report.Component)
		+ EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAIPCGNodeRecord& Node : Report.Nodes) Size += EstimateNodeBytes(Node);
	for (const FHyperAIPCGEdgeRecord& Edge : Report.Edges)
	{
		Size += 160ll + 2ll * (Edge.EdgeId.Len() + Edge.FromNodeId.Len()
			+ Edge.FromPinId.Len() + Edge.ToNodeId.Len() + Edge.ToPinId.Len());
	}
	for (const FHyperAIPCGIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioPCGValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioPCGContracts::ValidateResultTypeId;
}

FString FHyperAIStudioPCGValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPCGContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioPCGValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::PCG::Private;
	int64 Size = 1024ll + 2ll * (Report.Status.Len() + Report.Diagnostic.Len()
		+ Report.Policy.Len() + Report.PersistedRevision.Len()
		+ Report.ValidatorFingerprint.Len()) + EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAIPCGIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

FString FHyperAIStudioPCGContracts::ComputeLifecycleSemanticFingerprint(
	const FHyperAIStudioPCGLifecyclePayload& Payload)
{
	using namespace HyperAIStudio::PCG::Private;
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.generate-validate-cleanup-intent.v1"));
	AppendTokenUnchecked(Canonical, PackId);
	AppendTokenUnchecked(Canonical, TEXT("hyper_pcg_apply_plan"));
	AppendTokenUnchecked(Canonical, MutationVariantId);
	AppendTokenUnchecked(Canonical, LifecyclePayloadSchemaFingerprint());
	AppendTokenUnchecked(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendTokenUnchecked(Canonical, Payload.GraphPath);
	AppendTokenUnchecked(Canonical, Payload.ComponentPath);
	AppendTokenUnchecked(Canonical, Payload.BasePersistedRevision);
	AppendTokenUnchecked(Canonical, Payload.BaseComponentFingerprint);
	AppendTokenUnchecked(Canonical, Payload.BaseVolatileObservationFingerprint);
	AppendTokenUnchecked(Canonical, Payload.Lifecycle);
	AppendTokenUnchecked(Canonical, Payload.ValidationPolicy);
	AppendTokenUnchecked(Canonical, Payload.ExpectedTerminalState);
	AppendTokenUnchecked(Canonical, FString::FromInt(Payload.AsyncDeadlineMs));
	AppendTokenUnchecked(Canonical, Payload.bForceGenerate ? TEXT("force") : TEXT("normal"));
	AppendTokenUnchecked(Canonical, Payload.bRemoveGeneratedComponentsOnCleanup
		? TEXT("remove_generated_components") : TEXT("retain_generated_components"));
	return HashCanonical(Canonical);
}

FHyperAIPCGInspectReport FHyperAIStudioPCGContracts::Inspect(
	const FHyperAIPCGInspectRequest& Request)
{
	using namespace HyperAIStudio::PCG::Private;
	FHyperAIPCGInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.NextCursor.Reset();
		Report.bCursorEligible = false;
		return Report;
	};
	if (!IsCanonicalGraphPath(Request.GraphPath)
		|| (!Request.ComponentPath.IsEmpty() && !IsCanonicalComponentPath(Request.ComponentPath)))
	{
		return Reject(TEXT("invalid_exact_path"),
			TEXT("Inspect requires one canonical graph path and at most one exact loaded component path."));
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Page, cursor, game-thread, or output bounds are outside the closed PCG contract."));
	}
	FHyperAIStudioPCGValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.GraphPath, Request.ComponentPath, Request.MaxGameThreadMs,
		Snapshot, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.bFreshCapture = true;
	Report.Graph = Snapshot.Graph;
	Report.Component = Snapshot.Component;
	const bool bStableForPaging = IsPagingStable(Snapshot.Component);
	Report.bCursorEligible = Snapshot.Graph.bRevisionComplete && bStableForPaging;
	if (!Request.Cursor.IsEmpty() && !bStableForPaging)
	{
		return Reject(TEXT("volatile_snapshot_unpageable"),
			TEXT("The component entered generation, cleanup, or refresh; no recaptured page cursor is accepted."));
	}
	int32 Offset = 0;
	if (!Request.Cursor.IsEmpty()
		&& (!Snapshot.Graph.bRevisionComplete
			|| !ParseCursor(Request.Cursor, Request.GraphPath, Request.ComponentPath,
				Snapshot.Graph.PersistedRevision, Snapshot.Component.PersistedFingerprint,
				Request.PageSize, Offset)))
	{
		return Reject(TEXT("stale_or_invalid_cursor"),
			TEXT("Cursor identity does not match the fresh persisted graph/component configuration snapshot."));
	}
	const int32 TotalItems = Snapshot.Nodes.Num() + Snapshot.Edges.Num();
	if (Offset < 0 || Offset > TotalItems)
	{
		return Reject(TEXT("cursor_offset_out_of_range"),
			TEXT("Cursor offset is outside the fresh bounded node inventory."));
	}
	int64 EstimatedOutput = 4096ll + EstimateGraphBytes(Report.Graph)
		+ EstimateComponentBytes(Report.Component)
		+ EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAIPCGIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 Bytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= MaxIssues || EstimatedOutput + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutput += Bytes;
		Report.Issues.Add(Issue);
	}
	const int32 RequestedEnd = FMath::Min(Offset + Request.PageSize, TotalItems);
	int32 NextOffset = Offset;
	for (int32 Index = Offset; Index < RequestedEnd; ++Index)
	{
		const bool bNode = Index < Snapshot.Nodes.Num();
		const int32 EdgeIndex = Index - Snapshot.Nodes.Num();
		const int32 Bytes = bNode ? EstimateNodeBytes(Snapshot.Nodes[Index])
			: EstimateEdgeBytes(Snapshot.Edges[EdgeIndex]);
		if (EstimatedOutput + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutput += Bytes;
		if (bNode)
		{
			Report.Nodes.Add(Snapshot.Nodes[Index]);
		}
		else
		{
			const FHyperAIStudioPCGEdgeState& Source = Snapshot.Edges[EdgeIndex];
			FHyperAIPCGEdgeRecord Edge;
			Edge.EdgeId = Source.EdgeId;
			Edge.FromNodeId = Source.FromNodeId;
			Edge.FromPinId = Source.FromPinId;
			Edge.ToNodeId = Source.ToNodeId;
			Edge.ToPinId = Source.ToPinId;
			Report.Edges.Add(MoveTemp(Edge));
		}
		NextOffset = Index + 1;
	}
	if (NextOffset == Offset && Offset < TotalItems)
	{
		return Reject(TEXT("page_item_exceeds_output_bound"),
			TEXT("The next exact node or edge record cannot fit the caller output envelope; no non-advancing cursor is issued."));
	}
	if (NextOffset < TotalItems)
	{
		Report.bTruncated = true;
		if (Report.bCursorEligible)
		{
			Report.NextCursor = BuildCursor(Request.GraphPath, Request.ComponentPath,
				Snapshot.Graph.PersistedRevision, Snapshot.Component.PersistedFingerprint,
				Request.PageSize, NextOffset);
			Report.bCursorEligible = !Report.NextCursor.IsEmpty();
		}
		else
		{
			Report.bCursorEligible = false;
		}
	}
	if (EstimatedOutput > Request.MaxOutputBytes)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("Even the fixed exact PCG report envelope exceeds the caller output bound."));
	}
	Report.bOk = true;
	if (!bStableForPaging)
	{
		Report.Status = TEXT("volatile_snapshot_unpageable");
		Report.Diagnostic = TEXT("Returned one fresh bounded page only. Volatile task/generated observation is separate from persisted CAS and no continuation cursor is issued.");
		Report.NextCursor.Reset();
		Report.bCursorEligible = false;
	}
	else if (!Snapshot.Graph.bRevisionComplete || !Snapshot.bComplete)
	{
		Report.Status = TEXT("revision_incomplete");
		Report.Diagnostic = CaptureDiagnostic;
		Report.NextCursor.Reset();
		Report.bCursorEligible = false;
	}
	else
	{
		Report.Status = Report.bTruncated ? TEXT("exact_loaded_snapshot_page")
			: TEXT("exact_loaded_snapshot");
		Report.Diagnostic = CaptureDiagnostic;
	}
	return Report;
}

FHyperAIPCGValidateReport FHyperAIStudioPCGContracts::Validate(
	const FHyperAIPCGValidateRequest& Request)
{
	using namespace HyperAIStudio::PCG::Private;
	FHyperAIPCGValidateReport Report;
	Report.Policy = Request.Policy.Left(32);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.bValid = false;
		Report.bComplete = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsCanonicalGraphPath(Request.GraphPath)
		|| (!Request.ComponentPath.IsEmpty() && !IsCanonicalComponentPath(Request.ComponentPath))
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| (Request.ComponentPath.IsEmpty() && !Request.ExpectedComponentFingerprint.IsEmpty())
		|| (!Request.ComponentPath.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedComponentFingerprint))
		|| (Request.Policy != TEXT("structural") && Request.Policy != TEXT("generation_ready")))
	{
		return Reject(TEXT("invalid_validation_contract"),
			TEXT("Validation requires exact paths, complete expected CAS fingerprints, and a closed policy."));
	}
	if (Request.Policy == TEXT("generation_ready") && Request.ComponentPath.IsEmpty())
	{
		return Reject(TEXT("component_required"),
			TEXT("generation_ready requires one exact already-loaded PCG component."));
	}
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Issue, game-thread, or output bounds are outside the closed validator contract."));
	}
	FHyperAIStudioPCGValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.GraphPath, Request.ComponentPath, Request.MaxGameThreadMs,
		Snapshot, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.bFreshCapture = true;
	Report.PersistedRevision = Snapshot.Graph.PersistedRevision;
	if (!Snapshot.Graph.bRevisionComplete
		|| Snapshot.Graph.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(Snapshot.Graph.bRevisionComplete ? TEXT("stale_revision") : TEXT("revision_incomplete"),
			TEXT("Fresh graph CAS evidence is incomplete or differs from expected_persisted_revision."));
	}
	if (!Request.ComponentPath.IsEmpty()
		&& (!Snapshot.Component.bPersistedProjectionComplete
			|| Snapshot.Component.PersistedFingerprint != Request.ExpectedComponentFingerprint))
	{
		return Reject(Snapshot.Component.bPersistedProjectionComplete
			? TEXT("stale_component_fingerprint") : TEXT("component_projection_incomplete"),
			TEXT("Fresh component configuration differs from the asserted persisted fingerprint."));
	}
	TArray<FHyperAIPCGIssue> Issues;
	if (!ValidateValueSnapshot(Snapshot, Request.Policy, Request.MaxIssues,
		Issues, Report.ValidatorFingerprint))
	{
		return Reject(TEXT("validation_incomplete"),
			TEXT("Independent value validation exceeded its closed issue bound or could not seal a result."));
	}
	// Reserve worst-case bounded status/diagnostic/policy/hash fields before adding issues.
	int64 EstimatedOutput = 8192ll + EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAIPCGIssue& Issue : Issues)
	{
		const int32 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedOutput + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			return Reject(TEXT("validation_output_bound_exceeded"),
				TEXT("Independent validation issues do not fit the caller's closed output bound."));
		}
		EstimatedOutput += Bytes;
		Report.Issues.Add(Issue);
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
	}
	Report.bComplete = Snapshot.bComplete && !Report.bTruncated;
	Report.bValid = Report.bComplete && Report.ErrorCount == 0;
	Report.bOk = Report.bComplete;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = Report.bValid
		? TEXT("Fresh detached PCG values satisfy the independent closed validator policy.")
		: TEXT("Fresh detached PCG values contain one or more closed structural or lifecycle errors.");
	return Report;
}

FHyperAIPCGApplyPlanReport FHyperAIStudioPCGContracts::BuildPlan(
	const FHyperAIPCGApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::PCG::Private;
	FHyperAIPCGApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("PCG lifecycle planning requires one bounded exact game-thread capture."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("Dry-run prohibits operation_id and expected_plan_hash."));
	}
	if (!IsCanonicalGraphPath(Request.GraphPath)
		|| !IsCanonicalComponentPath(Request.ComponentPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| !IsCanonicalSha256(Request.ExpectedComponentFingerprint))
	{
		return Reject(TEXT("invalid_exact_cas"),
			TEXT("Lifecycle planning requires exact loaded graph/component paths and complete graph/component CAS."));
	}
	if (Request.Intent.Lifecycle != TEXT("generate_validate_cleanup")
		|| Request.Intent.ValidationPolicy != TEXT("generation_ready")
		|| Request.Intent.ExpectedTerminalState != TEXT("clean"))
	{
		return Reject(TEXT("unsupported_lifecycle_intent"),
			TEXT("v1 accepts only the compound generate_validate_cleanup / generation_ready / clean intent."));
	}
	if (Request.DeadlineMs < MinAsyncDeadlineMs || Request.DeadlineMs > MaxAsyncDeadlineMs
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Async deadline, game-thread, or output bounds are outside the closed lifecycle contract."));
	}
	if (!Request.ExpectedPlanHash.IsEmpty() && !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		return Reject(TEXT("invalid_expected_plan_hash"),
			TEXT("A supplied expected_plan_hash must be one canonical lowercase SHA-256."));
	}
	if (!Request.bDryRun && !IsSafeOperationId(Request.OperationId))
	{
		return Reject(TEXT("invalid_operation_id"),
			TEXT("Non-dry intent requires one journal-safe operation_id even though v1 never stages it."));
	}
	FHyperAIStudioPCGValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.GraphPath, Request.ComponentPath,
		FMath::Min(Request.MaxGameThreadMs, MaxReadGameThreadMs), Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, CaptureDiagnostic);
	}
	Report.BasePersistedRevision = Snapshot.Graph.PersistedRevision;
	Report.BaseComponentFingerprint = Snapshot.Component.PersistedFingerprint;
	Report.BaseVolatileObservationFingerprint =
		Snapshot.Component.VolatileObservationFingerprint;
	// Reserve the full bounded plan/result identity envelope before adding evidence.
	int64 EstimatedOutput = 8192ll + EstimateCapabilitiesBytes(Report.Capabilities);
	for (const FHyperAIPCGIssue& Issue : Snapshot.CaptureIssues)
	{
		const int32 Bytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= MaxIssues || EstimatedOutput + Bytes > Request.MaxOutputBytes)
		{
			return Reject(TEXT("issue_or_output_bound_exceeded"),
				TEXT("Exact lifecycle preflight evidence does not fit the closed issue/output bound."));
		}
		EstimatedOutput += Bytes;
		Report.Issues.Add(Issue);
	}
	if (!Snapshot.bComplete || !Snapshot.Graph.bRevisionComplete
		|| !Snapshot.Component.bPersistedProjectionComplete
		|| !IsCanonicalSha256(Snapshot.Component.VolatileObservationFingerprint))
	{
		return Reject(TEXT("revision_incomplete"),
			TEXT("Dry-run requires complete persisted graph and component configuration evidence."));
	}
	if (Snapshot.Graph.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The exact loaded PCG graph changed after inspection."));
	}
	if (Snapshot.Component.PersistedFingerprint != Request.ExpectedComponentFingerprint)
	{
		return Reject(TEXT("stale_component_fingerprint"),
			TEXT("The exact loaded PCG component configuration changed after inspection."));
	}
	if (Snapshot.Graph.bPackageDirty || Snapshot.Graph.DiskExistence != TEXT("exists")
		|| Snapshot.Graph.PackageSavedHash.IsEmpty() || Snapshot.Graph.DiskSize < 0)
	{
		return Reject(TEXT("persisted_clean_base_required"),
			TEXT("Lifecycle preparation requires one clean saved graph package with proven Asset Registry identity."));
	}
	if (!IsPagingStable(Snapshot.Component))
	{
		return Reject(TEXT("component_not_terminal"),
			TEXT("Lifecycle planning never waits; generation, cleanup, and refresh must already be idle."));
	}
	TArray<FHyperAIPCGIssue> ValidationIssues;
	FString ValidatorFingerprint;
	if (!ValidateValueSnapshot(Snapshot, TEXT("generation_ready"), MaxIssues,
		ValidationIssues, ValidatorFingerprint))
	{
		return Reject(TEXT("independent_validation_incomplete"),
			TEXT("Independent detached generation-ready validation could not complete within the closed bound."));
	}
	bool bValidationErrors = false;
	for (const FHyperAIPCGIssue& Issue : ValidationIssues)
	{
		if (Issue.Severity == TEXT("error")) bValidationErrors = true;
		const int32 Bytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= MaxIssues || EstimatedOutput + Bytes > Request.MaxOutputBytes)
		{
			return Reject(TEXT("validation_output_bound_exceeded"),
				TEXT("Independent lifecycle validation evidence does not fit the closed output bound."));
		}
		EstimatedOutput += Bytes;
		Report.Issues.Add(Issue);
	}
	if (bValidationErrors)
	{
		return Reject(TEXT("generation_ready_validation_failed"),
			TEXT("The detached fresh snapshot is not generation-ready."));
	}

	const TSharedRef<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe>();
	Payload->GraphPath = Request.GraphPath;
	Payload->ComponentPath = Request.ComponentPath;
	Payload->BasePersistedRevision = Snapshot.Graph.PersistedRevision;
	Payload->BaseComponentFingerprint = Snapshot.Component.PersistedFingerprint;
	Payload->BaseVolatileObservationFingerprint =
		Snapshot.Component.VolatileObservationFingerprint;
	Payload->Lifecycle = Request.Intent.Lifecycle;
	Payload->ValidationPolicy = Request.Intent.ValidationPolicy;
	Payload->ExpectedTerminalState = Request.Intent.ExpectedTerminalState;
	Payload->AsyncDeadlineMs = Request.DeadlineMs;
	Payload->bForceGenerate = Request.Intent.bForceGenerate;
	Payload->bRemoveGeneratedComponentsOnCleanup =
		Request.Intent.bRemoveGeneratedComponentsOnCleanup;
	Payload->SemanticFingerprint = ComputeLifecycleSemanticFingerprint(*Payload);
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_identity_unavailable"),
			TEXT("The closed typed lifecycle payload could not be semantically sealed."));
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (&Detached.Get() == &Payload.Get()
		|| Detached->GetTypeId() != Payload->GetTypeId()
		|| Detached->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
		|| Detached->GetSemanticFingerprint() != Payload->GetSemanticFingerprint()
		|| Detached->GetBoundedByteSize() != Payload->GetBoundedByteSize())
	{
		return Reject(TEXT("immutable_payload_clone_failed"),
			TEXT("The typed lifecycle payload did not produce an independent exact immutable clone."));
	}
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (ProjectId.IsEmpty())
	{
		return Reject(TEXT("canonical_project_identity_unavailable"),
			TEXT("Typed lifecycle preparation requires the canonical project identity."));
	}
	const FHyperAIStudioDomainAdapterDescriptor& Adapter = GetAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_pcg_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Adapter.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.PCG"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.PCGEditor"), EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bExternalEffectAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = LifecyclePayloadTypeId;
	Contract.ArtifactSchemaFingerprint = LifecyclePayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Contract.EffectTarget = TEXT("pcg:") + Snapshot.Component.PersistedFingerprint;
	Contract.DeadlineMs = FMath::Min(Request.DeadlineMs,
		FHyperAIStudioTypedArtifactLimits::MaxSynchronousDeadlineMs);
	Contract.MaxNativeOperations = 8;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = false;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.BeginCallCount = 1;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bWouldGenerateOnce = true;
	Report.Effects.bWouldPollWithoutBlocking = true;
	Report.Effects.bWouldValidateFreshOnce = true;
	Report.Effects.bWouldCleanupOnce = true;
	Report.Effects.bWouldObserveTerminalState = true;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_execution_blocked");
		Report.Diagnostic = TEXT("Closed persisted CAS, detached generation-ready validation, immutable lifecycle DTO, and public pure typed-artifact hashes are valid. No generation, polling, cleanup, compilation, staging, or execution occurred.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("expected_plan_hash_mismatch"),
			TEXT("Non-dry intent must echo the exact current dry-run plan hash."));
	}
	return Reject(NonDryCallableState,
		TEXT("Zero effects: no artifact was staged or submitted and no PCG call ran. The shared host still lacks exact pack-specific pinned begin/poll/cancel generation-cleanup semantics plus an independently owned fresh terminal validator."));
}

FHyperAIPCGInspectReport UHyperAIStudioPCGToolset::hyper_pcg_inspect(
	const FHyperAIPCGInspectRequest& Request)
{
	return FHyperAIStudioPCGContracts::Inspect(Request);
}

FHyperAIPCGApplyPlanReport UHyperAIStudioPCGToolset::hyper_pcg_apply_plan(
	const FHyperAIPCGApplyPlanRequest& Request)
{
	return FHyperAIStudioPCGContracts::BuildPlan(Request);
}

FHyperAIPCGValidateReport UHyperAIStudioPCGToolset::hyper_pcg_validate(
	const FHyperAIPCGValidateRequest& Request)
{
	return FHyperAIStudioPCGContracts::Validate(Request);
}

FHyperAIStudioPCGDomainAdapter::FHyperAIStudioPCGDomainAdapter()
	: Descriptor(FHyperAIStudioPCGContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPCGDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioPCGDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("PCG adapter rejected pack, adapter, or bounded typed DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_pcg_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioPCGContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPCGContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPCGContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPCGInspectPayload& Typed =
			static_cast<const FHyperAIStudioPCGInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioPCGInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPCGInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPCGContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_pcg_validate")
		&& Context.Binding.VariantId == FHyperAIStudioPCGContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPCGContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPCGContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioPCGValidatePayload& Typed =
			static_cast<const FHyperAIStudioPCGValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioPCGValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPCGValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPCGContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_pcg_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioPCGContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioPCGContracts::LifecyclePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPCGContracts::LifecyclePayloadSchemaFingerprint())
	{
		const FHyperAIStudioPCGLifecyclePayload& Typed =
			static_cast<const FHyperAIStudioPCGLifecyclePayload&>(Payload);
		if (FHyperAIStudioPCGContracts::ComputeLifecycleSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The exact typed PCG lifecycle semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioPCGContracts::NonDryCallableState,
			TEXT("Zero effects: the synchronous PCG adapter cannot begin, poll, cancel, validate, cleanup, compile, or reconcile a lifecycle."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("PCG adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioPCGRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPCGRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioPCGRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioPCGRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioPCGContracts::IsRegistrationAllowed(bDev)
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPCGToolset::StaticClass(),
			FHyperAIStudioPCGContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioPCGRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioPCGRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioPCGContracts::IsRegistrationAllowed(bDev))
	{
		UE_LOG(LogHyperAIStudioPCG, Verbose,
			TEXT("PCG exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("PCG"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("PCGEditor")))
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioPCGDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPCG, Error,
			TEXT("PCG adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioPCGContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPCG, Error,
			TEXT("PCG live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("PCG"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("PCGEditor"))
		&& UPCGGraph::StaticClass() != nullptr && UPCGComponent::StaticClass() != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("PCG and PCGEditor are already loaded; no asset, map, module, or editor was loaded by the probe.")
		: TEXT("PCG source cohort remains unavailable without already-loaded required modules.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioPCG, Error,
			TEXT("PCG live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioPCGToolset::StaticClass(),
		FHyperAIStudioPCGContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPCG, Error,
			TEXT("PCG three-tool cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioPCGRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPCGToolset::StaticClass(),
			FHyperAIStudioPCGContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPCG, Error,
				TEXT("PCG owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioPCG, Error,
				TEXT("PCG probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioPCG, Error,
				TEXT("PCG adapter rollback failed closed: %s"), *Error);
		}
		else
		{
			AdapterHandle = {};
			Adapter.Reset();
		}
	}
}

namespace HyperAIStudio::PCG::Private
{
	int32 EstimateIssueBytes(const FHyperAIPCGIssue& Issue)
	{
		const int64 Size = 96ll + 2ll * (Issue.Code.Len() + Issue.Severity.Len()
			+ Issue.StableId.Len() + Issue.Subject.Len() + Issue.Detail.Len());
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimatePinBytes(const FHyperAIPCGPinRecord& Pin)
	{
		const int64 Size = 160ll + 2ll * (Pin.PinId.Len() + Pin.Direction.Len()
			+ Pin.Label.Len() + Pin.AllowedTypeFingerprint.Len()
			+ Pin.CurrentTypeFingerprint.Len() + Pin.PropertiesFingerprint.Len());
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateNodeBytes(const FHyperAIPCGNodeRecord& Node)
	{
		int64 Size = 320ll + 2ll * (Node.NodeId.Len() + Node.Role.Len()
			+ Node.AuthoredTitle.Len() + Node.Comment.Len()
			+ Node.SettingsInterfaceClass.Len() + Node.SettingsClass.Len()
			+ Node.SettingsFingerprint.Len() + Node.NodePropertiesFingerprint.Len()
			+ Node.SemanticFingerprint.Len());
		for (const FHyperAIPCGPinRecord& Pin : Node.InputPins) Size += EstimatePinBytes(Pin);
		for (const FHyperAIPCGPinRecord& Pin : Node.OutputPins) Size += EstimatePinBytes(Pin);
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateEdgeBytes(const FHyperAIStudioPCGEdgeState& Edge)
	{
		const int64 Size = 160ll + 2ll * (Edge.EdgeId.Len() + Edge.FromNodeId.Len()
			+ Edge.FromPinId.Len() + Edge.ToNodeId.Len() + Edge.ToPinId.Len());
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateGraphBytes(const FHyperAIPCGGraphRecord& Graph)
	{
		const int64 Size = 512ll + 2ll * (Graph.TargetPath.Len() + Graph.ClassPath.Len()
			+ Graph.PackageName.Len() + Graph.PersistedRevision.Len()
			+ Graph.DiskExistence.Len() + Graph.PackageSavedHash.Len()
			+ Graph.GraphPropertiesFingerprint.Len() + Graph.UserParametersFingerprint.Len());
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateComponentBytes(const FHyperAIPCGComponentRecord& Component)
	{
		const int64 Size = 512ll + 2ll * (Component.ComponentPath.Len()
			+ Component.ComponentClassPath.Len() + Component.OwnerPath.Len()
			+ Component.GraphPath.Len() + Component.GraphInstancePath.Len()
			+ Component.GraphInterfacePath.Len() + Component.PackageName.Len()
			+ Component.DiskExistence.Len() + Component.PackageSavedHash.Len()
			+ Component.ComponentPropertiesFingerprint.Len()
			+ Component.GraphInstancePropertiesFingerprint.Len()
			+ Component.InstanceParametersFingerprint.Len()
			+ Component.InstanceOverrideMaskFingerprint.Len()
			+ Component.SchedulingPolicyFingerprint.Len()
			+ Component.OwnerTransformFingerprint.Len()
			+ Component.PersistedFingerprint.Len()
			+ Component.VolatileObservationFingerprint.Len()
			+ Component.GenerationTaskId.Len() + Component.CleanupTaskId.Len()
			);
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	int32 EstimateCapabilitiesBytes(const TArray<FHyperAIPCGCapabilityStatus>& Values)
	{
		int64 Size = 128;
		for (const FHyperAIPCGCapabilityStatus& Value : Values)
		{
			Size += 256ll + 2ll * (Value.Family.Len() + Value.State.Len()
				+ Value.Remediation.Len());
			for (const FString& Item : Value.DelegatedEpicCallables) Size += 24 + 2ll * Item.Len();
			for (const FString& Item : Value.CapabilityRequirementCoordinates) Size += 24 + 2ll * Item.Len();
			for (const FString& Item : Value.UniqueCases) Size += 24 + 2ll * Item.Len();
			for (const FString& Item : Value.UnsupportedCases) Size += 24 + 2ll * Item.Len();
		}
		return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
	}

	FHyperAIPCGIssue MakeIssue(
		const FString& Code,
		const FString& Severity,
		const FString& Subject,
		const FString& Detail)
	{
		FHyperAIPCGIssue Issue;
		Issue.Code = Code.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Issue.Severity = Severity.Left(16);
		Issue.Subject = Subject.Left(FHyperAIStudioPCGContracts::MaxPathCharacters);
		Issue.Detail = Detail.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.pcg.issue.v1"));
		AppendTokenUnchecked(Canonical, Issue.Code);
		AppendTokenUnchecked(Canonical, Issue.Severity);
		AppendTokenUnchecked(Canonical, Issue.Subject);
		Issue.StableId = HashCanonical(Canonical);
		return Issue;
	}

	void AddCaptureIssue(
		FHyperAIStudioPCGValueSnapshot& Snapshot,
		const FString& Code,
		const FString& Severity,
		const FString& Subject,
		const FString& Detail)
	{
		if (Snapshot.CaptureIssues.Num() >= FHyperAIStudioPCGContracts::MaxIssues)
		{
			Snapshot.bComplete = false;
			return;
		}
		Snapshot.CaptureIssues.Add(MakeIssue(Code, Severity, Subject, Detail));
	}

	bool FingerprintDataType(
		const FPCGDataTypeIdentifier& Type,
		FProjectionContext& Context,
		FString& OutFingerprint)
	{
		OutFingerprint.Reset();
		const TConstArrayView<FPCGDataTypeBaseId> Ids = Type.GetIds();
		FString AdmissionError;
		if (Ids.Num() < 0 || Ids.Num() > MaxTypeIds
			|| !FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
				Ids.Num(), Context.WorkUnits, Context.MaterializedBytes, 192, AdmissionError)
			|| !Context.ConsumeWork(Ids.Num() + 1, TEXT("pcg_type_identifier_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("pcg_type_identifier_not_bounded"));
			return false;
		}
		if (!Context.ReserveMaterializedBytes(Ids.Num() * 192,
			TEXT("pcg_type_identity_materialization_exceeded"))) return false;
		TArray<FString> Identities;
		Identities.Reserve(Ids.Num());
		for (const FPCGDataTypeBaseId& Id : Ids)
		{
			FString Identity;
			if (!Id.IsValid() || !BuildObjectIdentity(Id.GetStruct(), Context, Identity))
			{
				Context.MarkIncomplete(TEXT("pcg_type_identifier_invalid"));
				return false;
			}
			Identities.Add(MoveTemp(Identity));
		}
		Identities.Sort();
		FString Canonical;
		AppendBoundedToken(Context, Canonical, TEXT("hyperai.pcg.data-type.v1"));
		AppendBoundedToken(Context, Canonical, FString::FromInt(Type.CustomSubtype));
		AppendBoundedToken(Context, Canonical, FString::FromInt(Identities.Num()));
		for (const FString& Identity : Identities) AppendBoundedToken(Context, Canonical, Identity);
		OutFingerprint = HashCanonical(Canonical);
		return Context.bComplete && FHyperAIStudioPCGContracts::IsCanonicalSha256(OutFingerprint);
	}

	bool BuildPinRecord(
		const UPCGPin* Pin,
		const TCHAR* Direction,
		FProjectionContext& Context,
		FHyperAIPCGPinRecord& OutRecord)
	{
		OutRecord = {};
		if (!Pin || !Pin->Node || Pin->Edges.Num() > FHyperAIStudioPCGContracts::MaxEdges)
		{
			Context.MarkIncomplete(TEXT("pin_identity_or_edge_count_invalid"));
			return false;
		}
		if (!BuildObjectIdentity(Pin, Context, OutRecord.PinId)
			|| Pin->Properties.Label.GetStringLength()
				> static_cast<uint32>(FHyperAIStudioPCGContracts::MaxNameCharacters))
		{
			Context.MarkIncomplete(TEXT("pin_identity_or_label_not_bounded"));
			return false;
		}
		OutRecord.Direction = Direction;
		MaterializeNameBounded(Pin->Properties.Label, Context, OutRecord.Label);
		OutRecord.Usage = static_cast<int32>(Pin->Properties.Usage);
		OutRecord.Status = static_cast<int32>(Pin->Properties.PinStatus);
		OutRecord.bAllowsMultipleConnections = Pin->AllowsMultipleConnections();
		OutRecord.bAllowsMultipleData = Pin->AllowsMultipleData();
		OutRecord.bInvisible = Pin->Properties.bInvisiblePin;
		OutRecord.EdgeCount = Pin->Edges.Num();
		FingerprintDataType(Pin->Properties.AllowedTypes, Context,
			OutRecord.AllowedTypeFingerprint);
		// CurrentTypesID can allocate a composed identifier before the caller can cap it.
		// It is derived from the closed allowed-type/pin/edge snapshot and is intentionally omitted.
		OutRecord.CurrentTypeFingerprint.Reset();
		FString PropertiesCanonical;
		ProjectStructValue(FPCGPinProperties::StaticStruct(), &Pin->Properties, 1,
			Context, PropertiesCanonical);
		OutRecord.PropertiesFingerprint = HashCanonical(PropertiesCanonical);
		return Context.bComplete
			&& FHyperAIStudioPCGContracts::IsCanonicalSha256(OutRecord.PropertiesFingerprint);
	}

	bool BuildNodeRecord(
		const UPCGNode* Node,
		const FString& Role,
		FProjectionContext& Context,
		FHyperAIPCGNodeRecord& OutRecord)
	{
		OutRecord = {};
		if (!Node || Node->GetInputPins().Num() > FHyperAIStudioPCGContracts::MaxPinsPerNode
			|| Node->GetOutputPins().Num() > FHyperAIStudioPCGContracts::MaxPinsPerNode
			|| !Context.ConsumeWork(Node->GetInputPins().Num() + Node->GetOutputPins().Num() + 1,
				TEXT("node_pin_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("node_or_pin_count_not_bounded"));
			return false;
		}
		if (!BuildObjectIdentity(Node, Context, OutRecord.NodeId)
			|| Node->GetAuthoredTitleName().GetStringLength()
				> static_cast<uint32>(FHyperAIStudioPCGContracts::MaxNameCharacters)
			|| Node->NodeComment.Len() > FHyperAIStudioPCGContracts::MaxStringCharacters)
		{
			Context.MarkIncomplete(TEXT("node_identity_or_editor_text_not_bounded"));
			return false;
		}
		OutRecord.Role = Role;
		MaterializeNameBounded(Node->GetAuthoredTitleName(), Context,
			OutRecord.AuthoredTitle);
		if (Context.ReserveMaterializedBytes(
			(Node->NodeComment.Len() + 1) * static_cast<int32>(sizeof(TCHAR)),
			TEXT("node_comment_materialization_budget_exceeded")))
		{
			OutRecord.Comment = Node->NodeComment;
		}
		Node->GetNodePosition(OutRecord.PositionX, OutRecord.PositionY);
		OutRecord.bHidden = Node->IsHidden();

		const UPCGSettingsInterface* SettingsInterface = Node->GetSettingsInterface();
		const UPCGSettings* Settings = Node->GetSettings();
		if (!SettingsInterface || !Settings)
		{
			Context.MarkIncomplete(TEXT("node_settings_missing"));
		}
		BuildObjectIdentity(SettingsInterface ? SettingsInterface->GetClass() : nullptr,
			Context, OutRecord.SettingsInterfaceClass);
		BuildObjectIdentity(Settings ? Settings->GetClass() : nullptr,
			Context, OutRecord.SettingsClass);
		FString InterfaceFingerprint;
		FString SettingsObjectFingerprint;
		static const TSet<FName> NoExclusions;
		if (SettingsInterface)
		{
			ProjectObjectProperties(SettingsInterface, NoExclusions, true, Context,
				InterfaceFingerprint);
		}
		if (Settings && static_cast<const UObject*>(Settings)
			!= static_cast<const UObject*>(SettingsInterface))
		{
			ProjectObjectProperties(Settings, NoExclusions, true, Context,
				SettingsObjectFingerprint);
		}
		else
		{
			SettingsObjectFingerprint = InterfaceFingerprint;
		}
		FString SettingsCanonical;
		AppendBoundedToken(Context, SettingsCanonical, TEXT("hyperai.pcg.node-settings.v1"));
		AppendBoundedToken(Context, SettingsCanonical, OutRecord.SettingsInterfaceClass);
		AppendBoundedToken(Context, SettingsCanonical, OutRecord.SettingsClass);
		AppendBoundedToken(Context, SettingsCanonical, InterfaceFingerprint);
		AppendBoundedToken(Context, SettingsCanonical, SettingsObjectFingerprint);
		OutRecord.SettingsFingerprint = HashCanonical(SettingsCanonical);
		OutRecord.bSettingsProjectionComplete = Context.bComplete
			&& FHyperAIStudioPCGContracts::IsCanonicalSha256(OutRecord.SettingsFingerprint);

		if (!Context.ReserveMaterializedBytes(
			(Node->GetInputPins().Num() + Node->GetOutputPins().Num()) * 1280,
			TEXT("node_pin_fingerprint_materialization_exceeded")))
		{
			return false;
		}
		OutRecord.InputPins.Reserve(Node->GetInputPins().Num());
		OutRecord.OutputPins.Reserve(Node->GetOutputPins().Num());
		for (const TObjectPtr<UPCGPin>& Pin : Node->GetInputPins())
		{
			FHyperAIPCGPinRecord Record;
			BuildPinRecord(Pin.Get(), TEXT("input"), Context, Record);
			OutRecord.InputPins.Add(MoveTemp(Record));
		}
		for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
		{
			FHyperAIPCGPinRecord Record;
			BuildPinRecord(Pin.Get(), TEXT("output"), Context, Record);
			OutRecord.OutputPins.Add(MoveTemp(Record));
		}

		static const TSet<FName> ExcludedNodeProperties = {
			TEXT("SettingsInterface"), TEXT("InputPins"), TEXT("OutputPins"),
			TEXT("DefaultSettings_DEPRECATED"), TEXT("OutboundNodes_DEPRECATED"),
			TEXT("InboundEdges_DEPRECATED"), TEXT("OutboundEdges_DEPRECATED")};
		ProjectObjectProperties(Node, ExcludedNodeProperties, true, Context,
			OutRecord.NodePropertiesFingerprint);
		OutRecord.SemanticFingerprint =
			FHyperAIStudioPCGContracts::ComputeNodeFingerprint(OutRecord);
		return Context.bComplete
			&& FHyperAIStudioPCGContracts::IsCanonicalSha256(OutRecord.SemanticFingerprint);
	}
}

namespace HyperAIStudio::PCG::Private
{
	bool BuildPackageIdentity(
		const UObject* Object,
		FProjectionContext& Context,
		FString& OutCanonical,
		FString* OutPackageName = nullptr,
		FString* OutDiskExistence = nullptr,
		FString* OutSavedHash = nullptr,
		int64* OutDiskSize = nullptr,
		bool* OutDirty = nullptr,
		bool* OutWasLoaded = nullptr)
	{
		if (!Object || !Object->GetPackage())
		{
			Context.MarkIncomplete(TEXT("package_identity_missing"));
			return false;
		}
		const UPackage* Package = Object->GetPackage();
		if (Package == GetTransientPackage()
			|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor)
			|| Package->GetFName().GetStringLength()
				> static_cast<uint32>(FHyperAIStudioPCGContracts::MaxPathCharacters))
		{
			Context.MarkIncomplete(TEXT("transient_or_pie_package_unsupported"));
			return false;
		}
		FString PackageName;
		if (!MaterializeNameBounded(Package->GetFName(), Context, PackageName,
			FHyperAIStudioPCGContracts::MaxPathCharacters))
		{
			return false;
		}
		const bool bDirty = Package->IsDirty();
		const bool bWasLoaded = Object->HasAnyFlags(RF_WasLoaded)
			|| Package->HasAnyFlags(RF_WasLoaded);
		FString DiskExistence = TEXT("unknown");
		FString SavedHash;
		int64 DiskSize = -1;
		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (Registry)
		{
			FAssetPackageData PackageData;
			const UE::AssetRegistry::EExists Exists = Registry->TryGetAssetPackageData(
				Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
			DiskExistence = FHyperAIStudioPCGContracts::ClassifyAssetRegistryExistence(Exists);
			if (Exists == UE::AssetRegistry::EExists::Exists)
			{
				// FIoHash is a fixed 20-byte digest, but still reserve its 40-hex
				// materialization before invoking LexToString.
				if (!Context.ReserveMaterializedBytes(41 * static_cast<int32>(sizeof(TCHAR)),
					TEXT("package_saved_hash_materialization_exceeded")))
				{
					return false;
				}
				SavedHash = LexToString(PackageData.GetPackageSavedHash());
				if (SavedHash.Len() != 40)
				{
					Context.MarkIncomplete(TEXT("package_saved_hash_shape_invalid"));
					return false;
				}
				DiskSize = PackageData.DiskSize;
			}
			else if (Exists == UE::AssetRegistry::EExists::Unknown)
			{
				Context.MarkIncomplete(TEXT("asset_registry_package_identity_unknown"));
			}
		}
		else
		{
			Context.MarkIncomplete(TEXT("asset_registry_not_initialized"));
		}
		AppendBoundedToken(Context, OutCanonical, TEXT("hyperai.pcg.package.v1"));
		AppendBoundedToken(Context, OutCanonical, PackageName);
		AppendBoundedToken(Context, OutCanonical, DiskExistence);
		AppendBoundedToken(Context, OutCanonical, SavedHash);
		AppendBoundedToken(Context, OutCanonical, FString::Printf(TEXT("%lld"), DiskSize));
		AppendBoundedToken(Context, OutCanonical, bDirty ? TEXT("dirty") : TEXT("clean"));
		AppendBoundedToken(Context, OutCanonical, bWasLoaded ? TEXT("was_loaded") : TEXT("new_object"));
		if (OutPackageName) *OutPackageName = PackageName;
		if (OutDiskExistence) *OutDiskExistence = DiskExistence;
		if (OutSavedHash) *OutSavedHash = SavedHash;
		if (OutDiskSize) *OutDiskSize = DiskSize;
		if (OutDirty) *OutDirty = bDirty;
		if (OutWasLoaded) *OutWasLoaded = bWasLoaded;
		return Context.bComplete;
	}

	bool GetReflectedObjectArray(
		const UObject* Object,
		const FName PropertyName,
		FProjectionContext& Context,
		const FArrayProperty*& OutProperty,
		TUniquePtr<FScriptArrayHelper>& OutHelper)
	{
		OutProperty = FindFProperty<FArrayProperty>(Object ? Object->GetClass() : nullptr,
			PropertyName);
		if (!Object || !OutProperty)
		{
			Context.MarkIncomplete(TEXT("expected_reflected_array_missing"));
			return false;
		}
		const void* Value = OutProperty->ContainerPtrToValuePtr<void>(Object);
		OutHelper = MakeUnique<FScriptArrayHelper>(OutProperty, Value);
		FString Error;
		if (!FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
			OutHelper->Num(), Context.WorkUnits, Context.MaterializedBytes, 64, Error))
		{
			Context.MarkIncomplete(TEXT("reflected_array_rejected_before_projection"));
			OutHelper.Reset();
			return false;
		}
		return true;
	}

	bool CaptureGraphState(
		const UPCGGraph* Graph,
		const FString& GraphPath,
		FProjectionContext& Context,
		FHyperAIStudioPCGValueSnapshot& OutSnapshot)
	{
		FHyperAIPCGGraphRecord& Record = OutSnapshot.Graph;
		Record = {};
		Record.TargetPath = GraphPath;
		Record.bLoaded = Graph != nullptr;
		if (!Graph)
		{
			Context.MarkIncomplete(TEXT("loaded_graph_missing"));
			return false;
		}
		FString ActualGraphIdentity;
		if (!BuildObjectIdentity(Graph, Context, ActualGraphIdentity)
			|| ActualGraphIdentity != GraphPath)
		{
			Context.MarkIncomplete(TEXT("resolved_graph_identity_mismatch"));
			return false;
		}
		BuildObjectIdentity(Graph->GetClass(), Context, Record.ClassPath);
		FString PackageCanonical;
		BuildPackageIdentity(Graph, Context, PackageCanonical, &Record.PackageName,
			&Record.DiskExistence, &Record.PackageSavedHash, &Record.DiskSize,
			&Record.bPackageDirty, &Record.bWasLoadedFromDisk);
		if (PackageCanonical.IsEmpty()) Context.MarkIncomplete(TEXT("graph_package_identity_empty"));

		static const TSet<FName> ExcludedGraphProperties = {
			TEXT("Nodes"), TEXT("InputNode"), TEXT("OutputNode"),
			TEXT("UserParameters"), TEXT("EmbeddedSubgraphs"),
			TEXT("ExtraEditorNodes"), TEXT("CommentNodes"),
			TEXT("UserParameterHierarchyRoot"), TEXT("CockedCompilationData"),
			TEXT("CookedCompilationData"), TEXT("CachedPins")};
		FString BaseGraphProperties;
		ProjectObjectProperties(Graph, ExcludedGraphProperties, true, Context,
			BaseGraphProperties);
		ProjectPropertyBag(Graph->GetUserParametersStruct(), Context,
			Record.UserParametersFingerprint, Record.UserParameterCount);

		const TArray<FPCGGraphCommentNodeData>& Comments = Graph->GetCommentNodes();
		if (Comments.Num() > FHyperAIStudioPCGContracts::MaxComments
			|| !Context.ConsumeWork(Comments.Num(), TEXT("comment_projection_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("comment_count_not_bounded"));
		}
		Record.CommentCount = FMath::Min(Comments.Num(), FHyperAIStudioPCGContracts::MaxComments);
		FString CommentsCanonical;
		AppendBoundedToken(Context, CommentsCanonical, TEXT("hyperai.pcg.comments.v1"));
		if (Comments.Num() <= FHyperAIStudioPCGContracts::MaxComments)
		{
			for (const FPCGGraphCommentNodeData& Comment : Comments)
			{
				FString CommentCanonical;
				ProjectStructValue(FPCGGraphCommentNodeData::StaticStruct(), &Comment, 1,
					Context, CommentCanonical);
				AppendBoundedToken(Context, CommentsCanonical, HashCanonical(CommentCanonical));
			}
		}

		FString EmbeddedCanonical;
		AppendBoundedToken(Context, EmbeddedCanonical, TEXT("hyperai.pcg.embedded-subgraphs.v1"));
		const FArrayProperty* EmbeddedProperty = nullptr;
		TUniquePtr<FScriptArrayHelper> EmbeddedHelper;
		if (GetReflectedObjectArray(Graph, TEXT("EmbeddedSubgraphs"), Context,
			EmbeddedProperty, EmbeddedHelper))
		{
			Record.EmbeddedSubgraphCount = EmbeddedHelper->Num();
			if (Record.EmbeddedSubgraphCount > FHyperAIStudioPCGContracts::MaxEmbeddedSubgraphs)
			{
				Context.MarkIncomplete(TEXT("embedded_subgraph_count_exceeded"));
			}
			else
			{
				Context.ConsumeWork(Record.EmbeddedSubgraphCount,
					TEXT("embedded_subgraph_work_exceeded"));
				const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(EmbeddedProperty->Inner);
				if (!Inner && Record.EmbeddedSubgraphCount > 0)
				{
					Context.MarkIncomplete(TEXT("embedded_subgraph_array_type_unsupported"));
				}
				for (int32 Index = 0; Inner && Index < Record.EmbeddedSubgraphCount; ++Index)
				{
					const UObject* Embedded = Inner->GetObjectPropertyValue(EmbeddedHelper->GetRawPtr(Index));
					FString Identity;
					BuildObjectIdentity(Embedded, Context, Identity);
					AppendBoundedToken(Context, EmbeddedCanonical, Identity);
				}
				if (Record.EmbeddedSubgraphCount > 0)
				{
					// The public accessor copies the full array; recursive closure is unsupported in v1.
					Context.MarkIncomplete(TEXT("embedded_subgraph_recursive_revision_unsupported"));
				}
			}
		}

		FString ExtraEditorCanonical;
		AppendBoundedToken(Context, ExtraEditorCanonical, TEXT("hyperai.pcg.extra-editor-nodes.v1"));
		const TArray<TObjectPtr<UObject>>& ExtraEditorNodes = Graph->GetExtraEditorNodes();
		if (ExtraEditorNodes.Num() > FHyperAIStudioPCGContracts::MaxComments)
		{
			Context.MarkIncomplete(TEXT("extra_editor_node_count_exceeded"));
		}
		else
		{
			Context.ConsumeWork(ExtraEditorNodes.Num(),
				TEXT("extra_editor_node_work_exceeded"));
			for (const TObjectPtr<UObject>& Extra : ExtraEditorNodes)
			{
				FString Identity;
				BuildObjectIdentity(Extra.Get(), Context, Identity);
				AppendBoundedToken(Context, ExtraEditorCanonical, Identity);
			}
			if (!ExtraEditorNodes.IsEmpty())
			{
				Context.MarkIncomplete(TEXT("extra_editor_node_content_unsupported"));
			}
		}

		FString GraphPropertiesCanonical;
		AppendBoundedToken(Context, GraphPropertiesCanonical, TEXT("hyperai.pcg.graph-properties.v1"));
		AppendBoundedToken(Context, GraphPropertiesCanonical, BaseGraphProperties);
		AppendBoundedToken(Context, GraphPropertiesCanonical, HashCanonical(CommentsCanonical));
		AppendBoundedToken(Context, GraphPropertiesCanonical, HashCanonical(EmbeddedCanonical));
		AppendBoundedToken(Context, GraphPropertiesCanonical, HashCanonical(ExtraEditorCanonical));
		Record.GraphPropertiesFingerprint = HashCanonical(GraphPropertiesCanonical);

		const TArray<UPCGNode*>& GraphNodes = Graph->GetNodes();
		const int64 TotalNodes64 = static_cast<int64>(GraphNodes.Num()) + 2;
		if (!Graph->GetInputNode() || !Graph->GetOutputNode()
			|| TotalNodes64 > FHyperAIStudioPCGContracts::MaxNodes
			|| !Context.ConsumeWork(static_cast<int32>(TotalNodes64),
				TEXT("graph_node_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("graph_node_count_or_boundary_invalid"));
			return false;
		}
		Record.NodeCount = static_cast<int32>(TotalNodes64);
		if (!Context.ReserveMaterializedBytes(Record.NodeCount * 1024,
			TEXT("node_inventory_materialization_exceeded")))
		{
			return false;
		}
		TArray<const UPCGNode*> NodePointers;
		NodePointers.Reserve(Record.NodeCount);
		NodePointers.Add(Graph->GetInputNode());
		NodePointers.Add(Graph->GetOutputNode());
		for (const UPCGNode* Node : GraphNodes) NodePointers.Add(Node);
		TSet<const UPCGNode*> UniqueNodes;
		UniqueNodes.Reserve(Record.NodeCount);
		TMap<const UPCGNode*, FString> NodeIds;
		TMap<const UPCGPin*, FString> PinIds;
		TSet<const UPCGPin*> InputPins;
		TSet<const UPCGPin*> OutputPins;
		OutSnapshot.Nodes.Reserve(Record.NodeCount);
		for (int32 Index = 0; Index < NodePointers.Num(); ++Index)
		{
			const UPCGNode* Node = NodePointers[Index];
			if (!Node || UniqueNodes.Contains(Node))
			{
				Context.MarkIncomplete(TEXT("duplicate_or_null_graph_node"));
				continue;
			}
				UniqueNodes.Add(Node);
				FHyperAIPCGNodeRecord NodeRecord;
				const bool bNodeBuilt = BuildNodeRecord(Node, Index == 0 ? TEXT("graph_input")
					: Index == 1 ? TEXT("graph_output") : TEXT("node"), Context, NodeRecord);
				if (!bNodeBuilt
					|| NodeRecord.InputPins.Num() != Node->GetInputPins().Num()
					|| NodeRecord.OutputPins.Num() != Node->GetOutputPins().Num())
				{
					Context.MarkIncomplete(TEXT("node_projection_incomplete"));
					OutSnapshot.Nodes.Add(MoveTemp(NodeRecord));
					continue;
				}
				NodeIds.Add(Node, NodeRecord.NodeId);
			for (int32 PinIndex = 0; PinIndex < Node->GetInputPins().Num(); ++PinIndex)
			{
				const UPCGPin* Pin = Node->GetInputPins()[PinIndex].Get();
				if (!Pin || Pin->Node != Node || PinIds.Contains(Pin))
				{
					Context.MarkIncomplete(TEXT("input_pin_owner_or_identity_invalid"));
					continue;
				}
				PinIds.Add(Pin, NodeRecord.InputPins[PinIndex].PinId);
				InputPins.Add(Pin);
			}
			for (int32 PinIndex = 0; PinIndex < Node->GetOutputPins().Num(); ++PinIndex)
			{
				const UPCGPin* Pin = Node->GetOutputPins()[PinIndex].Get();
				if (!Pin || Pin->Node != Node || PinIds.Contains(Pin))
				{
					Context.MarkIncomplete(TEXT("output_pin_owner_or_identity_invalid"));
					continue;
				}
				PinIds.Add(Pin, NodeRecord.OutputPins[PinIndex].PinId);
				OutputPins.Add(Pin);
			}
			OutSnapshot.Nodes.Add(MoveTemp(NodeRecord));
		}

		int64 OutputEdgeCount = 0;
		int64 InputEdgeCount = 0;
		for (const UPCGNode* Node : NodePointers)
		{
			if (Context.DeadlineExceeded())
			{
				Context.MarkIncomplete(TEXT("graph_edge_inventory_deadline_exceeded"));
				return false;
			}
			if (!Node) continue;
			for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
			{
				OutputEdgeCount += Pin ? Pin->Edges.Num() : 0;
				if (OutputEdgeCount > FHyperAIStudioPCGContracts::MaxEdges)
				{
					Context.MarkIncomplete(TEXT("graph_output_edge_count_exceeded"));
					return false;
				}
			}
			for (const TObjectPtr<UPCGPin>& Pin : Node->GetInputPins())
			{
				InputEdgeCount += Pin ? Pin->Edges.Num() : 0;
				if (InputEdgeCount > FHyperAIStudioPCGContracts::MaxEdges)
				{
					Context.MarkIncomplete(TEXT("graph_input_edge_count_exceeded"));
					return false;
				}
			}
		}
		if (OutputEdgeCount < 0 || OutputEdgeCount > FHyperAIStudioPCGContracts::MaxEdges
			|| InputEdgeCount != OutputEdgeCount
			|| !Context.ConsumeWork(static_cast<int32>(OutputEdgeCount * 2),
				TEXT("graph_edge_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("graph_edge_count_not_closed_or_bounded"));
			return false;
		}
		if (OutputEdgeCount > MAX_int32 / 4096
			|| !Context.ReserveMaterializedBytes(static_cast<int32>(OutputEdgeCount) * 4096,
				TEXT("edge_inventory_materialization_exceeded")))
		{
			return false;
		}
		OutSnapshot.Edges.Reserve(static_cast<int32>(OutputEdgeCount));
		TSet<const UPCGEdge*> SeenEdges;
		SeenEdges.Reserve(static_cast<int32>(OutputEdgeCount));
		TSet<FString> SeenEdgeIds;
		for (const UPCGNode* Node : NodePointers)
		{
			if (!Node) continue;
			for (const TObjectPtr<UPCGPin>& FromPinPtr : Node->GetOutputPins())
			{
				const UPCGPin* FromPin = FromPinPtr.Get();
				if (!FromPin) continue;
				for (const TObjectPtr<UPCGEdge>& EdgePtr : FromPin->Edges)
				{
					if (Context.DeadlineExceeded())
					{
						Context.MarkIncomplete(TEXT("graph_edge_projection_deadline_exceeded"));
						return false;
					}
					const UPCGEdge* Edge = EdgePtr.Get();
					const UPCGPin* ToPin = Edge ? Edge->OutputPin.Get() : nullptr;
					if (!Edge || SeenEdges.Contains(Edge) || Edge->InputPin.Get() != FromPin
						|| !ToPin || !InputPins.Contains(ToPin) || !OutputPins.Contains(FromPin)
						|| !PinIds.Contains(FromPin) || !PinIds.Contains(ToPin)
						|| !NodeIds.Contains(FromPin->Node.Get()) || !NodeIds.Contains(ToPin->Node.Get())
						|| !ToPin->Edges.Contains(Edge))
					{
						Context.MarkIncomplete(TEXT("edge_endpoint_or_bidirectional_closure_invalid"));
						continue;
					}
					FHyperAIStudioPCGEdgeState State;
					State.FromNodeId = NodeIds.FindChecked(FromPin->Node.Get());
					State.FromPinId = PinIds.FindChecked(FromPin);
					State.ToNodeId = NodeIds.FindChecked(ToPin->Node.Get());
					State.ToPinId = PinIds.FindChecked(ToPin);
					State.EdgeId = FHyperAIStudioPCGContracts::ComputeEdgeFingerprint(State);
					if (SeenEdgeIds.Contains(State.EdgeId))
					{
						Context.MarkIncomplete(TEXT("duplicate_edge_identity"));
						continue;
					}
					SeenEdges.Add(Edge);
					SeenEdgeIds.Add(State.EdgeId);
					OutSnapshot.Edges.Add(MoveTemp(State));
				}
			}
		}
		for (const UPCGNode* Node : NodePointers)
		{
			if (!Node) continue;
			for (const TObjectPtr<UPCGPin>& ToPinPtr : Node->GetInputPins())
			{
				const UPCGPin* ToPin = ToPinPtr.Get();
				if (!ToPin) continue;
				for (const TObjectPtr<UPCGEdge>& Edge : ToPin->Edges)
				{
					if (Context.DeadlineExceeded())
					{
						Context.MarkIncomplete(TEXT("graph_edge_reciprocity_deadline_exceeded"));
						return false;
					}
					if (!Edge || Edge->OutputPin.Get() != ToPin || !SeenEdges.Contains(Edge.Get()))
					{
						Context.MarkIncomplete(TEXT("input_edge_not_in_closed_output_inventory"));
					}
				}
			}
		}
		Record.EdgeCount = OutSnapshot.Edges.Num();
		return Context.bComplete;
	}

	bool CaptureComponentState(
		const UPCGComponent* Component,
		const UPCGGraph* Graph,
		const FString& GraphPath,
		const FString& ComponentPath,
		FProjectionContext& Context,
		FHyperAIPCGComponentRecord& OutRecord)
	{
		OutRecord = {};
		if (ComponentPath.IsEmpty())
		{
			OutRecord.bVolatileStateStable = true;
			return true;
		}
		OutRecord.bPresent = Component != nullptr;
		OutRecord.ComponentPath = ComponentPath;
		if (!Component || !Graph || Component->GetGraph() != Graph)
		{
			Context.MarkIncomplete(TEXT("component_missing_or_graph_mismatch"));
			return false;
		}
		FString ActualComponentIdentity;
		if (!BuildObjectIdentity(Component, Context, ActualComponentIdentity)
			|| ActualComponentIdentity != ComponentPath)
		{
			Context.MarkIncomplete(TEXT("resolved_component_identity_mismatch"));
			return false;
		}
		BuildObjectIdentity(Component->GetClass(), Context, OutRecord.ComponentClassPath);
		BuildObjectIdentity(Component->GetOwner(), Context, OutRecord.OwnerPath);
		OutRecord.GraphPath = GraphPath;
		OutRecord.bActivated = Component->bActivated;
		OutRecord.bGenerated = Component->bGenerated;
#if WITH_EDITORONLY_DATA
		OutRecord.bDirtyGenerated = Component->bDirtyGenerated;
#endif
		OutRecord.bGenerating = Component->IsGenerating();
		OutRecord.bCleaningUp = Component->IsCleaningUp();
		OutRecord.bRefreshInProgress = Component->IsRefreshInProgress();
		OutRecord.bVolatileStateStable = !OutRecord.bGenerating
			&& !OutRecord.bCleaningUp && !OutRecord.bRefreshInProgress;
		OutRecord.GenerationTaskId = FString::Printf(TEXT("%llu"),
			static_cast<uint64>(Component->GetGenerationTaskId()));
		OutRecord.CleanupTaskId = FString::Printf(TEXT("%llu"),
			static_cast<uint64>(Component->GetCleanupTaskId()));
		OutRecord.Seed = Component->Seed;
		OutRecord.GenerationTrigger = static_cast<int32>(Component->GenerationTrigger);
		OutRecord.bPartitioned = Component->IsPartitioned();

		static const TSet<FName> ExcludedComponentProperties = {
			TEXT("GraphInstance"), TEXT("SchedulingPolicy"),
			TEXT("bGenerated"), TEXT("bDirtyGenerated"),
			TEXT("GeneratedGraphOutput"), TEXT("PerPinGeneratedOutput"),
			TEXT("LoadedPreviewGeneratedGraphOutput"), TEXT("LastGeneratedBounds"),
			TEXT("GeneratedResources_DEPRECATED"), TEXT("GeneratedActors_DEPRECATED")};
		FString ComponentPropertiesFingerprint;
		ProjectObjectProperties(Component, ExcludedComponentProperties, false, Context,
			ComponentPropertiesFingerprint);
		OutRecord.ComponentPropertiesFingerprint = ComponentPropertiesFingerprint;
		if (const UPCGGraphInstance* Instance = Component->GetGraphInstance())
		{
			BuildObjectIdentity(Instance, Context, OutRecord.GraphInstancePath);
			if (!Instance->Graph)
			{
				Context.MarkIncomplete(TEXT("component_graph_interface_missing"));
			}
			else
			{
				BuildObjectIdentity(Instance->Graph, Context, OutRecord.GraphInterfacePath);
			}
			static const TSet<FName> ExcludedGraphInstanceProperties = {
				TEXT("Graph"), TEXT("ParametersOverrides")};
			ProjectObjectProperties(Instance, ExcludedGraphInstanceProperties, true,
				Context, OutRecord.GraphInstancePropertiesFingerprint);
			ProjectPropertyBag(Instance->GetUserParametersStruct(), Context,
				OutRecord.InstanceParametersFingerprint, OutRecord.InstanceParameterCount);
			OutRecord.InstanceOverrideCount =
				Instance->ParametersOverrides.PropertiesIDsOverridden.Num();
			ProjectOverrideMask(Instance->ParametersOverrides.PropertiesIDsOverridden,
				Context, OutRecord.InstanceOverrideMaskFingerprint);
		}
		else
		{
			Context.MarkIncomplete(TEXT("component_graph_instance_missing"));
		}
		if (const UPCGSchedulingPolicyBase* Policy = Component->GetRuntimeGenSchedulingPolicy())
		{
			static const TSet<FName> NoExclusions;
			ProjectObjectProperties(Policy, NoExclusions, true, Context,
				OutRecord.SchedulingPolicyFingerprint);
		}
		else
		{
			FString Empty;
			AppendTokenUnchecked(Empty, TEXT("hyperai.pcg.no-scheduling-policy.v1"));
			OutRecord.SchedulingPolicyFingerprint = HashCanonical(Empty);
		}
		FString PackageCanonical;
		BuildPackageIdentity(Component, Context, PackageCanonical, &OutRecord.PackageName,
			&OutRecord.DiskExistence, &OutRecord.PackageSavedHash, &OutRecord.DiskSize,
			&OutRecord.bPackageDirty, &OutRecord.bWasLoadedFromDisk);
		if (PackageCanonical.IsEmpty()) Context.MarkIncomplete(TEXT("component_package_identity_empty"));
		FString OwnerTransformCanonical;
		AppendBoundedToken(Context, OwnerTransformCanonical, TEXT("hyperai.pcg.owner-transform.v1"));
		if (const AActor* Owner = Component->GetOwner())
		{
			const FTransform Transform = Owner->GetActorTransform();
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			for (const double Value : {Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W, Scale.X, Scale.Y, Scale.Z})
			{
				AppendBoundedToken(Context, OwnerTransformCanonical,
					FString::Printf(TEXT("%.17g"), Value));
			}
		}
		else
		{
			Context.MarkIncomplete(TEXT("component_owner_missing"));
		}
		OutRecord.OwnerTransformFingerprint = HashCanonical(OwnerTransformCanonical);
		OutRecord.PersistedFingerprint =
			FHyperAIStudioPCGContracts::ComputeComponentPersistedFingerprint(OutRecord);
		OutRecord.bPersistedProjectionComplete = Context.bComplete
			&& FHyperAIStudioPCGContracts::IsCanonicalSha256(OutRecord.PersistedFingerprint);
		OutRecord.VolatileObservationFingerprint =
			FHyperAIStudioPCGContracts::ComputeComponentVolatileObservationFingerprint(OutRecord);
		return Context.bComplete;
	}
}
