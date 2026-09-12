// Games by Hyper 2026.

#include "HyperAIStudioDataToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/CurveTable.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/UserDefinedEnum.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDataDelegationMatrix.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/UserDefinedStruct.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioData, Log, All);

namespace HyperAIStudio::Data::Private
{
	constexpr int64 BaseReportBytes = 4096;
	constexpr int64 BaseRecordBytes = 384;
	constexpr int64 BaseIssueBytes = 256;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += LexToString(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("|");
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("1") : TEXT("0");
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ')) return true;
		}
		return false;
	}

	int32 KindRank(const FString& Kind)
	{
		if (Kind == TEXT("asset_state")) return 0;
		if (Kind == TEXT("data_asset") || Kind == TEXT("data_table")
			|| Kind == TEXT("curve_table") || Kind == TEXT("string_table")
			|| Kind == TEXT("user_struct") || Kind == TEXT("user_enum")
			|| Kind == TEXT("unsupported_asset")) return 1;
		return 2;
	}

	FString RecordSortKey(const FHyperAIDataRecord& Record)
	{
		return Record.ObjectPath + TEXT("|")
			+ FString::Printf(TEXT("%02d"), KindRank(Record.Kind)) + TEXT("|")
			+ Record.Kind + TEXT("|") + Record.StableId + TEXT("|") + Record.Name;
	}

	void SortAndSealRecords(TArray<FHyperAIDataRecord>& Records)
	{
		Records.Sort([](const FHyperAIDataRecord& A, const FHyperAIDataRecord& B)
		{
			return RecordSortKey(A) < RecordSortKey(B);
		});
		TMap<FString, int32> ChildIndices;
		for (FHyperAIDataRecord& Record : Records)
		{
			if (KindRank(Record.Kind) == 2)
			{
				Record.Index = ChildIndices.FindOrAdd(Record.ObjectPath)++;
			}
			Record.ElementFingerprint = FHyperAIStudioDataContracts::ComputeElementFingerprint(Record);
		}
	}

	int64 EstimateRecordBytes(const FHyperAIDataRecord& Record)
	{
		return BaseRecordBytes
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.Kind)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.ObjectPath)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.StableId)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.Name)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.TypeId)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.Value)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.SecondaryValue)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.PackageState)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Record.ElementFingerprint);
	}

	int64 EstimateIssueBytes(const FHyperAIDataIssue& Issue)
	{
		return BaseIssueBytes
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Issue.Code)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Issue.Severity)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Issue.ObjectPath)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Issue.StableId)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Issue.Message);
	}

	struct FCapture
	{
		TArray<FHyperAIDataRecord> Records;
		TArray<FHyperAIDataIssue> Issues;
		FString RequestFingerprint;
		FString PersistedFingerprint;
		FString VolatileFingerprint;
		int32 SourceObjectsScanned = 0;
		bool bComplete = true;
		bool bDeadlineExceeded = false;
		bool bIssueOverflow = false;

		void MarkIncomplete(
			const FString& Code,
			const FString& ObjectPath,
			const FString& StableId,
			const FString& Message,
			const FString& Severity = TEXT("warning"))
		{
			bComplete = false;
			if (Issues.Num() >= FHyperAIStudioDataContracts::MaxIssues)
			{
				bIssueOverflow = true;
				return;
			}
			FHyperAIDataIssue& Issue = Issues.AddDefaulted_GetRef();
			Issue.Code = Code;
			Issue.Severity = Severity;
			Issue.ObjectPath = ObjectPath;
			Issue.StableId = StableId;
			Issue.Message = Message;
		}
	};

	bool CheckDeadline(FCapture& Capture, const double Deadline)
	{
		if (FPlatformTime::Seconds() <= Deadline) return true;
		Capture.bDeadlineExceeded = true;
		Capture.MarkIncomplete(TEXT("deadline_exceeded"), FString(), FString(),
			TEXT("Loaded-only projection exhausted the monotonic deadline."), TEXT("error"));
		return false;
	}

	bool CanAddRecord(FCapture& Capture, const FString& ObjectPath)
	{
		if (Capture.Records.Num() < FHyperAIStudioDataContracts::MaxRecords) return true;
		Capture.MarkIncomplete(TEXT("record_bound_exceeded"), ObjectPath, FString(),
			TEXT("The detached projection exceeded its hard record cap."), TEXT("error"));
		return false;
	}

	FString CurveModeToken(const ECurveTableMode Mode)
	{
		switch (Mode)
		{
		case ECurveTableMode::SimpleCurves: return TEXT("simple");
		case ECurveTableMode::RichCurves: return TEXT("rich");
		default: return TEXT("empty");
		}
	}

	bool IsDelegatedClassPath(const FString& ClassPath, FString& OutReason)
	{
		struct FPrefix { const TCHAR* Prefix; const TCHAR* Reason; };
		static constexpr FPrefix Prefixes[] = {
			{TEXT("/Script/DataRegistry."), TEXT("data_registry_epic_delegate")},
			{TEXT("/Script/Chooser."), TEXT("chooser_optional_adapter")},
			{TEXT("/Script/ProxyTable."), TEXT("chooser_optional_adapter")},
			{TEXT("/Script/CommonConversationRuntime."), TEXT("conversation_epic_delegate")},
			{TEXT("/Script/CommonConversationGraph."), TEXT("conversation_epic_delegate")},
			{TEXT("/Script/GameplayTags."), TEXT("gameplay_tags_epic_delegate")},
			{TEXT("/Script/Localization."), TEXT("localization_export_excluded")}};
		for (const FPrefix& Entry : Prefixes)
		{
			if (ClassPath.StartsWith(Entry.Prefix))
			{
				OutReason = Entry.Reason;
				return true;
			}
		}
		return false;
	}

	void AddAssetState(
		FCapture& Capture,
		const FString& ObjectPath,
		const FString& PackageState,
		const UObject* LoadedObject,
		const UPackage* LoadedPackage)
	{
		if (!CanAddRecord(Capture, ObjectPath)) return;
		FHyperAIDataRecord& Record = Capture.Records.AddDefaulted_GetRef();
		Record.Kind = TEXT("asset_state");
		Record.ObjectPath = ObjectPath;
		Record.StableId = TEXT("asset_state");
		Record.PackageState = PackageState;
		Record.bLoaded = LoadedObject != nullptr;
		Record.bDirty = LoadedPackage && LoadedPackage->IsDirty();
		Record.bWasLoaded = (LoadedObject && LoadedObject->HasAnyFlags(RF_WasLoaded))
			|| (LoadedPackage && LoadedPackage->HasAnyFlags(RF_WasLoaded));
	}

	void ProjectLoadedObject(
		FCapture& Capture,
		UObject* Object,
		const FString& ObjectPath,
		const double Deadline)
	{
		if (!Object || !CheckDeadline(Capture, Deadline) || !CanAddRecord(Capture, ObjectPath)) return;
		const FString ClassPath = Object->GetClass()->GetClassPathName().ToString();
		if (ClassPath.Len() > FHyperAIStudioDataContracts::MaxPathCharacters)
		{
			Capture.MarkIncomplete(TEXT("class_path_bound_exceeded"), ObjectPath, FString(),
				TEXT("The loaded class identity exceeded the closed scalar cap."));
			return;
		}
		FString DelegationReason;
		if (IsDelegatedClassPath(ClassPath, DelegationReason))
		{
			FHyperAIDataRecord& Record = Capture.Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("unsupported_asset");
			Record.ObjectPath = ObjectPath;
			Record.StableId = TEXT("delegated");
			Record.TypeId = ClassPath;
			Record.Value = DelegationReason;
			Capture.MarkIncomplete(DelegationReason, ObjectPath, FString(),
				TEXT("This asset family is intentionally delegated or capability-gated."));
			return;
		}

		if (const UDataTable* Table = Cast<UDataTable>(Object))
		{
			const TMap<FName, uint8*>& Rows = Table->UDataTable::GetRowMap();
			FString RowType = Table->GetRowStruct()
				? Table->GetRowStruct()->GetPathName() : TEXT("none");
			if (RowType.Len() > FHyperAIStudioDataContracts::MaxPathCharacters)
			{
				Capture.MarkIncomplete(TEXT("row_struct_path_bound_exceeded"), ObjectPath, FString(),
					TEXT("The DataTable row-struct identity exceeded the hard scalar cap."));
				RowType.Reset();
			}
			FHyperAIDataRecord& Header = Capture.Records.AddDefaulted_GetRef();
			Header.Kind = TEXT("data_table");
			Header.ObjectPath = ObjectPath;
			Header.StableId = TEXT("table");
			Header.Name = Table->GetName();
			Header.TypeId = RowType;
			Header.Count = Rows.Num();
			if (Rows.Num() > FHyperAIStudioDataContracts::MaxRowsPerTable
				|| Rows.GetAllocatedSize() > FHyperAIStudioDataContracts::MaxContainerAllocatedBytes)
			{
				Capture.MarkIncomplete(TEXT("data_table_row_bound_exceeded"), ObjectPath, FString(),
					TEXT("Row names were not copied because the map exceeded its pre-copy count or allocation cap."));
				return;
			}
			TArray<FName> Names;
			Rows.GenerateKeyArray(Names);
			Names.Sort(FNameLexicalLess());
			for (const FName RowName : Names)
			{
				if (!CheckDeadline(Capture, Deadline) || !CanAddRecord(Capture, ObjectPath)) return;
				const FString Name = RowName.ToString();
				if (Name.Len() > FHyperAIStudioDataContracts::MaxNameCharacters)
				{
					Capture.MarkIncomplete(TEXT("row_name_bound_exceeded"), ObjectPath, FString(),
						TEXT("A DataTable row name exceeded the hard string cap."));
					continue;
				}
				FHyperAIDataRecord& Row = Capture.Records.AddDefaulted_GetRef();
				Row.Kind = TEXT("data_table_row");
				Row.ObjectPath = ObjectPath;
				Row.StableId = Name;
				Row.Name = Name;
				Row.TypeId = RowType;
			}
			return;
		}

		if (const UCurveTable* Table = Cast<UCurveTable>(Object))
		{
			const TMap<FName, FRealCurve*>& Rows = Table->GetRowMap();
			const FString Mode = CurveModeToken(Table->GetCurveTableMode());
			FHyperAIDataRecord& Header = Capture.Records.AddDefaulted_GetRef();
			Header.Kind = TEXT("curve_table");
			Header.ObjectPath = ObjectPath;
			Header.StableId = TEXT("table");
			Header.Name = Table->GetName();
			Header.TypeId = Mode;
			Header.Count = Rows.Num();
			if (Rows.Num() > FHyperAIStudioDataContracts::MaxRowsPerTable
				|| Rows.GetAllocatedSize() > FHyperAIStudioDataContracts::MaxContainerAllocatedBytes)
			{
				Capture.MarkIncomplete(TEXT("curve_table_row_bound_exceeded"), ObjectPath, FString(),
					TEXT("Curve rows were not copied because the map exceeded its pre-copy count or allocation cap."));
				return;
			}
			TArray<FName> Names;
			Rows.GenerateKeyArray(Names);
			Names.Sort(FNameLexicalLess());
			for (const FName RowName : Names)
			{
				if (!CheckDeadline(Capture, Deadline) || !CanAddRecord(Capture, ObjectPath)) return;
				const FString Name = RowName.ToString();
				if (Name.Len() > FHyperAIStudioDataContracts::MaxNameCharacters)
				{
					Capture.MarkIncomplete(TEXT("curve_row_name_bound_exceeded"), ObjectPath, FString(),
						TEXT("A CurveTable row name exceeded the hard string cap."));
					continue;
				}
				const FRealCurve* const* Found = Rows.Find(RowName);
				if (!Found || !*Found)
				{
					Capture.MarkIncomplete(TEXT("curve_row_null"), ObjectPath, RowName.ToString(),
						TEXT("The loaded CurveTable contains a null curve pointer."));
					continue;
				}
				FHyperAIDataRecord& Row = Capture.Records.AddDefaulted_GetRef();
				Row.Kind = TEXT("curve_table_row");
				Row.ObjectPath = ObjectPath;
				Row.StableId = Name;
				Row.Name = Row.StableId;
				Row.TypeId = Mode;
				Row.Count = (*Found)->GetNumKeys();
				if (Row.Count > FHyperAIStudioDataContracts::MaxRowsPerTable)
				{
					Capture.MarkIncomplete(TEXT("curve_key_bound_exceeded"), ObjectPath, Row.StableId,
						TEXT("Only key count/range metadata is exposed; this row exceeds the validated key cap."));
				}
				if (Row.Count > 0)
				{
					float MinTime = 0.0f;
					float MaxTime = 0.0f;
					(*Found)->GetTimeRange(MinTime, MaxTime);
					Row.NumberValue = MinTime;
					Row.SecondaryNumberValue = MaxTime;
				}
				const float DefaultValue = (*Found)->GetDefaultValue();
				Row.bFlag = DefaultValue != MAX_flt;
				if (Row.bFlag) Row.Value = FString::Printf(TEXT("%.9g"), DefaultValue);
			}
			return;
		}

		if (const UStringTable* StringTable = Cast<UStringTable>(Object))
		{
			const FStringTableConstRef Table = StringTable->GetStringTable();
			const FString TableId = StringTable->GetStringTableId().ToString();
			const FString Namespace = Table->GetNamespace();
			FHyperAIDataRecord& Record = Capture.Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("string_table");
			Record.ObjectPath = ObjectPath;
			Record.StableId = TEXT("table");
			Record.Name = StringTable->GetName();
			if (TableId.Len() <= FHyperAIStudioDataContracts::MaxNameCharacters)
			{
				Record.TypeId = TableId;
			}
			else
			{
				Capture.MarkIncomplete(TEXT("string_table_id_bound_exceeded"), ObjectPath, FString(),
					TEXT("The string-table id exceeded the hard scalar cap."));
			}
			if (Namespace.Len() <= FHyperAIStudioDataContracts::MaxTextCharacters)
			{
				Record.Value = Namespace;
			}
			else
			{
				Capture.MarkIncomplete(TEXT("string_table_namespace_bound_exceeded"), ObjectPath, FString(),
					TEXT("The string-table namespace exceeded the hard scalar cap."));
			}
			Record.bLoaded = Table->IsLoaded();
			Record.bFlag = Table->IsInternal();
			Capture.MarkIncomplete(TEXT("string_table_entries_epic_delegate"), ObjectPath, FString(),
				TEXT("Entry enumeration is intentionally excluded; use Epic StringTableTools for keys and values."));
			return;
		}

		if (const UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Object))
		{
			const TArray<FStructVariableDescription>* VariablesPtr =
				FStructureEditorUtils::GetVarDescPtr(Struct);
			FHyperAIDataRecord& Header = Capture.Records.AddDefaulted_GetRef();
			Header.Kind = TEXT("user_struct");
			Header.ObjectPath = ObjectPath;
			Header.StableId = TEXT("struct");
			Header.Name = Struct->GetName();
			Header.TypeId = Struct->Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
			if (!VariablesPtr)
			{
				Capture.MarkIncomplete(TEXT("user_struct_editor_data_unavailable"), ObjectPath, FString(),
					TEXT("The loaded user struct has no public editor descriptor array."));
				return;
			}
			const TArray<FStructVariableDescription>& Variables = *VariablesPtr;
			Header.Count = Variables.Num();
			if (Variables.Num() > FHyperAIStudioDataContracts::MaxFieldsPerStruct
				|| Variables.GetAllocatedSize() > FHyperAIStudioDataContracts::MaxContainerAllocatedBytes)
			{
				Capture.MarkIncomplete(TEXT("user_struct_field_bound_exceeded"), ObjectPath, FString(),
					TEXT("Struct descriptors were not copied because the array exceeded its pre-copy cap."));
				return;
			}
			for (const FStructVariableDescription& Variable : Variables)
			{
				if (!CheckDeadline(Capture, Deadline) || !CanAddRecord(Capture, ObjectPath)) return;
				const FString InternalName = Variable.VarName.ToString();
				const FString AuthoredName = Variable.FriendlyName.IsEmpty()
					? InternalName : Variable.FriendlyName;
				const FString FieldType = Variable.Category.ToString() + TEXT("/")
					+ Variable.SubCategory.ToString() + TEXT("/")
					+ LexToString(static_cast<int32>(Variable.ContainerType));
				if (InternalName.Len() > FHyperAIStudioDataContracts::MaxNameCharacters
					|| AuthoredName.Len() > FHyperAIStudioDataContracts::MaxNameCharacters
					|| FieldType.Len() > FHyperAIStudioDataContracts::MaxPathCharacters)
				{
					Capture.MarkIncomplete(TEXT("user_struct_name_bound_exceeded"), ObjectPath,
						Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
						TEXT("A struct member identity exceeded the hard scalar cap."));
					continue;
				}
				FHyperAIDataRecord& Field = Capture.Records.AddDefaulted_GetRef();
				Field.Kind = TEXT("user_struct_field");
				Field.ObjectPath = ObjectPath;
				Field.StableId = Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				Field.Name = AuthoredName;
				Field.TypeId = FieldType;
				Field.Value = InternalName;
				Field.bFlag = Variable.bInvalidMember;
				if (Variable.bInvalidMember)
				{
					Capture.MarkIncomplete(TEXT("user_struct_invalid_member"), ObjectPath, Field.StableId,
						TEXT("The loaded user struct contains an invalid member descriptor."));
				}
			}
			return;
		}

		if (const UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Object))
		{
			const int32 RawCount = Enum->NumEnums();
			const int32 ValueCount = FMath::Max(0, RawCount - 1);
			FHyperAIDataRecord& Header = Capture.Records.AddDefaulted_GetRef();
			Header.Kind = TEXT("user_enum");
			Header.ObjectPath = ObjectPath;
			Header.StableId = TEXT("enum");
			Header.Name = Enum->GetName();
			Header.TypeId = TEXT("user_defined_enum");
			Header.Count = ValueCount;
			if (ValueCount > FHyperAIStudioDataContracts::MaxValuesPerEnum)
			{
				Capture.MarkIncomplete(TEXT("user_enum_value_bound_exceeded"), ObjectPath, FString(),
					TEXT("Enum values were not copied because the count exceeded its pre-copy cap."));
				return;
			}
			for (int32 Index = 0; Index < ValueCount; ++Index)
			{
				if (!CheckDeadline(Capture, Deadline) || !CanAddRecord(Capture, ObjectPath)) return;
				const FString RawName = Enum->GetNameStringByIndex(Index);
				if (RawName.Len() > FHyperAIStudioDataContracts::MaxPathCharacters)
				{
					Capture.MarkIncomplete(TEXT("user_enum_raw_name_bound_exceeded"), ObjectPath, FString(),
						TEXT("An enum raw name exceeded the hard pre-copy scalar cap."));
					continue;
				}
				int32 NamespaceSeparator = INDEX_NONE;
				RawName.FindLastChar(TEXT(':'), NamespaceSeparator);
				const FString AuthoredName = NamespaceSeparator == INDEX_NONE
					? RawName : RawName.Mid(NamespaceSeparator + 1);
				if (AuthoredName.Len() > FHyperAIStudioDataContracts::MaxNameCharacters)
				{
					Capture.MarkIncomplete(TEXT("user_enum_name_bound_exceeded"), ObjectPath, FString(),
						TEXT("An enum value name exceeded the hard scalar cap."));
					continue;
				}
				FHyperAIDataRecord& Value = Capture.Records.AddDefaulted_GetRef();
				Value.Kind = TEXT("user_enum_value");
				Value.ObjectPath = ObjectPath;
				Value.StableId = AuthoredName;
				Value.Name = Value.StableId;
				Value.IntegerValue = Enum->GetValueByIndex(Index);
			}
			return;
		}

		if (const UDataAsset* DataAsset = Cast<UDataAsset>(Object))
		{
			FHyperAIDataRecord& Record = Capture.Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("data_asset");
			Record.ObjectPath = ObjectPath;
			Record.StableId = TEXT("asset");
			Record.Name = DataAsset->GetName();
			Record.TypeId = ClassPath;
			Record.Value = Cast<UPrimaryDataAsset>(DataAsset)
				? TEXT("primary_data_asset") : TEXT("data_asset");
			Capture.MarkIncomplete(TEXT("data_asset_fields_not_projected"), ObjectPath, FString(),
				TEXT("Arbitrary DataAsset property reflection is prohibited; only identity is projected."));
			return;
		}

		FHyperAIDataRecord& Record = Capture.Records.AddDefaulted_GetRef();
		Record.Kind = TEXT("unsupported_asset");
		Record.ObjectPath = ObjectPath;
		Record.StableId = TEXT("unsupported");
		Record.Name = Object->GetName();
		Record.TypeId = ClassPath;
		Capture.MarkIncomplete(TEXT("loaded_type_unsupported"), ObjectPath, FString(),
			TEXT("The loaded object is outside the closed value projector."));
	}

	FString ComputeRequestFingerprint(const TArray<FString>& SortedPaths, const bool bDelegatedValues)
	{
		FString Canonical(TEXT("hyperai.data.inspect.request.v1|"));
		AppendToken(Canonical, BoolToken(bDelegatedValues));
		for (const FString& Path : SortedPaths) AppendToken(Canonical, Path);
		if (Canonical.Len() > FHyperAIStudioDataContracts::MaxCanonicalCharacters) return FString();
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FCapture CaptureLoadedProjection(
		const TArray<FString>& InPaths,
		const bool bDelegatedValues,
		const int32 DeadlineMs)
	{
		FCapture Capture;
		TArray<FString> Paths = InPaths;
		Paths.Sort();
		Capture.RequestFingerprint = ComputeRequestFingerprint(Paths, bDelegatedValues);
		const double Deadline = FPlatformTime::Seconds() + (static_cast<double>(DeadlineMs) / 1000.0);
		IAssetRegistry* Registry = IAssetRegistry::Get();
		for (const FString& ObjectPath : Paths)
		{
			if (!CheckDeadline(Capture, Deadline)) break;
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			UPackage* LoadedPackage = FindPackage(nullptr, *PackageName);
			UObject* LoadedObject = FindObjectSafe<UObject>(nullptr, *ObjectPath);
			FString PackageState = TEXT("unknown");
			if (!Registry)
			{
				Capture.MarkIncomplete(TEXT("asset_registry_not_initialized"), ObjectPath, FString(),
					TEXT("The fail-fast package tri-state API is unavailable."));
			}
			else
			{
				FAssetPackageData PackageData;
				const UE::AssetRegistry::EExists Exists = Registry->TryGetAssetPackageData(
					FName(*PackageName), PackageData, /*bFailIfLockHeld=*/true);
				PackageState = FHyperAIStudioDataContracts::ClassifyPackageExistence(
					static_cast<int32>(Exists));
				if (Exists == UE::AssetRegistry::EExists::Unknown)
				{
					Capture.MarkIncomplete(TEXT("package_state_unknown"), ObjectPath, FString(),
						TEXT("Asset Registry could not acquire its read lock; Unknown is never treated as absence."));
				}
			}
			AddAssetState(Capture, ObjectPath, PackageState, LoadedObject, LoadedPackage);
			if (LoadedObject)
			{
				++Capture.SourceObjectsScanned;
				ProjectLoadedObject(Capture, LoadedObject, ObjectPath, Deadline);
			}
			else if (PackageState == TEXT("exists"))
			{
				Capture.MarkIncomplete(TEXT("asset_not_loaded"), ObjectPath, FString(),
					TEXT("The package exists but its primary object is not loaded; synchronous loading is prohibited."));
			}
			if (bDelegatedValues)
			{
				Capture.MarkIncomplete(TEXT("delegated_value_request_not_served"), ObjectPath, FString(),
					TEXT("Raw rows, arbitrary fields, string entries, config, and localization values remain delegated."));
			}
		}
		SortAndSealRecords(Capture.Records);
		Capture.PersistedFingerprint =
			FHyperAIStudioDataContracts::ComputePersistedFingerprint(Capture.Records);
		Capture.VolatileFingerprint =
			FHyperAIStudioDataContracts::ComputeVolatileFingerprint(Capture.Records);
		return Capture;
	}

	bool IsKnownRecordKind(const FString& Kind)
	{
		static const TSet<FString> Kinds = {
			TEXT("asset_state"), TEXT("data_asset"), TEXT("data_table"),
			TEXT("data_table_row"), TEXT("curve_table"), TEXT("curve_table_row"),
			TEXT("string_table"), TEXT("user_struct"), TEXT("user_struct_field"),
			TEXT("user_enum"), TEXT("user_enum_value"), TEXT("unsupported_asset")};
		return Kinds.Contains(Kind);
	}

	bool IsBoundedRecord(const FHyperAIDataRecord& Record)
	{
		return Record.Kind.Len() <= 64
			&& Record.ObjectPath.Len() <= FHyperAIStudioDataContracts::MaxPathCharacters
			&& Record.StableId.Len() <= FHyperAIStudioDataContracts::MaxNameCharacters
			&& Record.Name.Len() <= FHyperAIStudioDataContracts::MaxNameCharacters
			&& Record.TypeId.Len() <= FHyperAIStudioDataContracts::MaxPathCharacters
			&& Record.Value.Len() <= FHyperAIStudioDataContracts::MaxTextCharacters
			&& Record.SecondaryValue.Len() <= FHyperAIStudioDataContracts::MaxTextCharacters
			&& Record.PackageState.Len() <= 32;
	}

	void CopyIssueWithinBound(
		const FHyperAIDataIssue& Issue,
		const int32 MaxIssues,
		const int32 MaxOutputBytes,
		int64& EstimatedBytes,
		TArray<FHyperAIDataIssue>& OutIssues,
		bool& bTruncated)
	{
		const int64 Cost = EstimateIssueBytes(Issue);
		if (OutIssues.Num() >= MaxIssues || EstimatedBytes + Cost > MaxOutputBytes)
		{
			bTruncated = true;
			return;
		}
		EstimatedBytes += Cost;
		OutIssues.Add(Issue);
	}

	FHyperAIDataInspectReport Inspect(const FHyperAIDataInspectRequest& Request)
	{
		FHyperAIDataInspectReport Report;
		auto Reject = [&Report](const FString& Status, const FString& Diagnostic)
		{
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		if (!IsInGameThread())
		{
			return Reject(TEXT("game_thread_required"),
				TEXT("Loaded UObject value projection is admitted only on the Unreal game thread."));
		}
		if (Request.TargetPaths.IsEmpty()
			|| Request.TargetPaths.Num() > FHyperAIStudioDataContracts::MaxTargetPaths)
		{
			return Reject(TEXT("target_scope_invalid"),
				TEXT("Provide between one and 32 explicit primary /Game object paths."));
		}
		if (Request.PageSize < 1 || Request.PageSize > FHyperAIStudioDataContracts::MaxPageSize
			|| Request.DeadlineMs < 1 || Request.DeadlineMs > FHyperAIStudioDataContracts::MaxDeadlineMs
			|| Request.MaxOutputBytes < 4096
			|| Request.MaxOutputBytes > FHyperAIStudioDataContracts::MaxOutputBytes
			|| Request.Cursor.Len() > FHyperAIStudioDataContracts::MaxCursorCharacters)
		{
			return Reject(TEXT("request_bounds_invalid"),
				TEXT("Page, cursor, deadline, or worst-case JSON output bounds are invalid."));
		}
		TSet<FString> UniquePaths;
		for (const FString& Path : Request.TargetPaths)
		{
			if (!FHyperAIStudioDataContracts::IsCanonicalProjectObjectPath(Path)
				|| UniquePaths.Contains(Path))
			{
				return Reject(TEXT("target_path_invalid"),
					TEXT("Paths must be unique canonical primary /Game object paths without subobjects."));
			}
			UniquePaths.Add(Path);
		}

		FCapture Capture = CaptureLoadedProjection(
			Request.TargetPaths, Request.bRequestDelegatedValues, Request.DeadlineMs);
		if (Capture.bDeadlineExceeded)
		{
			return Reject(TEXT("deadline_exceeded"),
				TEXT("No partial page is returned after deadline exhaustion."));
		}
		if (!FHyperAIStudioDataContracts::IsCanonicalSha256(Capture.RequestFingerprint)
			|| !FHyperAIStudioDataContracts::IsCanonicalSha256(Capture.PersistedFingerprint)
			|| !FHyperAIStudioDataContracts::IsCanonicalSha256(Capture.VolatileFingerprint))
		{
			return Reject(TEXT("snapshot_identity_unavailable"),
				TEXT("The bounded loaded-only projection could not be deterministically sealed."));
		}

		int32 Offset = 0;
		if (!Request.Cursor.IsEmpty()
			&& !FHyperAIStudioDataContracts::DecodeCursor(Request.Cursor,
				Capture.RequestFingerprint, Capture.PersistedFingerprint,
				Capture.VolatileFingerprint, Offset))
		{
			return Reject(TEXT("cursor_revision_mismatch"),
				TEXT("The cursor is malformed or bound to a different request/persisted/volatile revision."));
		}
		if (Offset < 0 || Offset > Capture.Records.Num())
		{
			return Reject(TEXT("cursor_offset_invalid"), TEXT("The cursor offset is outside the snapshot."));
		}

		Report.Snapshot.RequestFingerprint = Capture.RequestFingerprint;
		Report.Snapshot.PersistedFingerprint = Capture.PersistedFingerprint;
		Report.Snapshot.VolatileObservationFingerprint = Capture.VolatileFingerprint;
		Report.Snapshot.TotalRecords = Capture.Records.Num();
		Report.SourceObjectsScanned = Capture.SourceObjectsScanned;
		int64 EstimatedBytes = BaseReportBytes
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Capture.RequestFingerprint)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Capture.PersistedFingerprint)
			+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Capture.VolatileFingerprint);
		const int32 End = FMath::Min(Capture.Records.Num(), Offset + Request.PageSize);
		int32 NextOffset = Offset;
		for (int32 Index = Offset; Index < End; ++Index)
		{
			const int64 Cost = EstimateRecordBytes(Capture.Records[Index]);
			if (EstimatedBytes + Cost > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			EstimatedBytes += Cost;
			Report.Snapshot.Records.Add(Capture.Records[Index]);
			NextOffset = Index + 1;
		}
		if (NextOffset == Offset && Offset < Capture.Records.Num())
		{
			return Reject(TEXT("record_output_bound_exceeded"),
				TEXT("The next value record cannot fit its declared worst-case JSON envelope."));
		}
		for (const FHyperAIDataIssue& Issue : Capture.Issues)
		{
			CopyIssueWithinBound(Issue, FHyperAIStudioDataContracts::MaxIssues,
				Request.MaxOutputBytes, EstimatedBytes, Report.Issues, Report.bTruncated);
		}
		Report.bTruncated |= Capture.bIssueOverflow || NextOffset < Capture.Records.Num();
		if (NextOffset < Capture.Records.Num())
		{
			Report.NextCursor = FHyperAIStudioDataContracts::EncodeCursor(NextOffset,
				Capture.RequestFingerprint, Capture.PersistedFingerprint, Capture.VolatileFingerprint);
			if (EstimatedBytes
				+ FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(Report.NextCursor)
				> Request.MaxOutputBytes)
			{
				return Reject(TEXT("cursor_output_bound_exceeded"),
					TEXT("The revision-bound continuation cursor does not fit the declared output envelope."));
			}
		}
		Report.ReturnedRecords = Report.Snapshot.Records.Num();
		Report.Snapshot.bSnapshotComplete = Capture.bComplete && !Report.bTruncated
			&& Offset == 0 && NextOffset == Capture.Records.Num();
		Report.bOk = true;
		Report.Status = Capture.bComplete ? TEXT("loaded_projection_complete")
			: TEXT("loaded_projection_incomplete");
		Report.Diagnostic = Capture.bComplete
			? TEXT("Returned a hard-bounded, loaded-only, value-only detached projection.")
			: TEXT("Returned bounded evidence, but Unknown, unloaded, delegated, unsupported, or capped values make it incomplete.");
		return Report;
	}

	FHyperAIDataValidateReport Validate(const FHyperAIDataValidateRequest& Request)
	{
		FHyperAIDataValidateReport Report;
		auto Reject = [&Report](const FString& Status, const FString& Diagnostic)
		{
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioDataContracts::MaxIssues
			|| Request.DeadlineMs < 1 || Request.DeadlineMs > FHyperAIStudioDataContracts::MaxDeadlineMs
			|| Request.MaxOutputBytes < 4096
			|| Request.MaxOutputBytes > FHyperAIStudioDataContracts::MaxOutputBytes)
		{
			return Reject(TEXT("request_bounds_invalid"),
				TEXT("Detached validation bounds are invalid."));
		}
		const FHyperAIDataDetachedSnapshot& Snapshot = Request.Snapshot;
		if (Snapshot.Records.Num() > FHyperAIStudioDataContracts::MaxRecords
			|| Snapshot.TotalRecords != Snapshot.Records.Num())
		{
			return Reject(TEXT("detached_snapshot_not_full"),
				TEXT("Validation accepts only a hard-bounded full detached snapshot, never a page."));
		}
		const double Deadline = FPlatformTime::Seconds()
			+ (static_cast<double>(Request.DeadlineMs) / 1000.0);
		TArray<FHyperAIDataIssue> FoundIssues;
		bool bInternalIssueOverflow = false;
		auto Add = [&FoundIssues, &bInternalIssueOverflow](const FString& Code, const FString& Path,
			const FString& StableId, const int32 Index, const FString& Message)
		{
			if (FoundIssues.Num() >= FHyperAIStudioDataContracts::MaxIssues)
			{
				bInternalIssueOverflow = true;
				return;
			}
			FHyperAIDataIssue& Issue = FoundIssues.AddDefaulted_GetRef();
			Issue.Code = Code;
			Issue.Severity = TEXT("error");
			Issue.ObjectPath = Path;
			Issue.StableId = StableId;
			Issue.RecordIndex = Index;
			Issue.Message = Message;
		};
		struct FClosure
		{
			int32 StateCount = 0;
			int32 DataTableHeaders = 0;
			int32 DataTableRows = 0;
			int32 CurveTableHeaders = 0;
			int32 CurveTableRows = 0;
			int32 StructHeaders = 0;
			int32 StructFields = 0;
			int32 EnumHeaders = 0;
			int32 EnumValues = 0;
			int32 IdentityOnlyRecords = 0;
			int32 DeclaredCount = -1;
			FString PackageState;
			bool bLoaded = false;
		};
		TMap<FString, FClosure> ClosureByPath;
		TSet<FString> RecordIdentities;
		TMap<FString, int32> NextChildIndexByPath;
		FString PreviousKey;
		for (int32 Index = 0; Index < Snapshot.Records.Num(); ++Index)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return Reject(TEXT("deadline_exceeded"),
					TEXT("Independent detached value validation exhausted the monotonic deadline."));
			}
			const FHyperAIDataRecord& Record = Snapshot.Records[Index];
			const FString RecordIdentity = Record.ObjectPath + TEXT("|")
				+ Record.Kind + TEXT("|") + Record.StableId;
			if (RecordIdentities.Contains(RecordIdentity))
			{
				Add(TEXT("stable_identity_duplicate"), Record.ObjectPath, Record.StableId, Index,
					TEXT("A detached stable identity may occur only once."));
			}
			RecordIdentities.Add(RecordIdentity);
			if (KindRank(Record.Kind) == 2)
			{
				const int32 ExpectedIndex = NextChildIndexByPath.FindOrAdd(Record.ObjectPath)++;
				if (Record.Index != ExpectedIndex)
				{
					Add(TEXT("child_index_not_contiguous"), Record.ObjectPath, Record.StableId, Index,
						TEXT("Detached child indexes must be zero-based and contiguous per object."));
				}
			}
			else if (Record.Index != -1)
			{
				Add(TEXT("header_index_invalid"), Record.ObjectPath, Record.StableId, Index,
					TEXT("Asset-state and value-header records must retain index -1."));
			}
			FClosure& Closure = ClosureByPath.FindOrAdd(Record.ObjectPath);
			if (Record.Kind == TEXT("asset_state"))
			{
				++Closure.StateCount;
				Closure.PackageState = Record.PackageState;
				Closure.bLoaded = Record.bLoaded;
			}
			else if (Record.Kind == TEXT("data_table"))
			{
				++Closure.DataTableHeaders;
				Closure.DeclaredCount = Record.Count;
			}
			else if (Record.Kind == TEXT("data_table_row")) ++Closure.DataTableRows;
			else if (Record.Kind == TEXT("curve_table"))
			{
				++Closure.CurveTableHeaders;
				Closure.DeclaredCount = Record.Count;
			}
			else if (Record.Kind == TEXT("curve_table_row")) ++Closure.CurveTableRows;
			else if (Record.Kind == TEXT("user_struct"))
			{
				++Closure.StructHeaders;
				Closure.DeclaredCount = Record.Count;
			}
			else if (Record.Kind == TEXT("user_struct_field")) ++Closure.StructFields;
			else if (Record.Kind == TEXT("user_enum"))
			{
				++Closure.EnumHeaders;
				Closure.DeclaredCount = Record.Count;
			}
			else if (Record.Kind == TEXT("user_enum_value")) ++Closure.EnumValues;
			else if (Record.Kind == TEXT("data_asset") || Record.Kind == TEXT("string_table")
				|| Record.Kind == TEXT("unsupported_asset")) ++Closure.IdentityOnlyRecords;
			if (!IsKnownRecordKind(Record.Kind))
			{
				Add(TEXT("record_kind_unknown"), Record.ObjectPath, Record.StableId, Index,
					TEXT("The record kind is outside the closed schema."));
			}
			if (!IsBoundedRecord(Record)
				|| !FHyperAIStudioDataContracts::IsCanonicalProjectObjectPath(Record.ObjectPath))
			{
				Add(TEXT("record_bounds_invalid"), Record.ObjectPath, Record.StableId, Index,
					TEXT("A scalar or primary-object path violates the detached schema bound."));
			}
			if (Record.Kind == TEXT("asset_state")
				&& Record.PackageState != TEXT("exists")
				&& Record.PackageState != TEXT("does_not_exist")
				&& Record.PackageState != TEXT("unknown"))
			{
				Add(TEXT("package_state_invalid"), Record.ObjectPath, Record.StableId, Index,
					TEXT("Package state must be exists, does_not_exist, or unknown."));
			}
			if (Record.Kind != TEXT("asset_state") && !Record.PackageState.IsEmpty())
			{
				Add(TEXT("package_state_on_value_record"), Record.ObjectPath, Record.StableId, Index,
					TEXT("Package tri-state belongs only to the asset-state record."));
			}
			if (Record.Kind == TEXT("asset_state") && Record.PackageState == TEXT("unknown"))
			{
				Add(TEXT("package_state_unknown_incomplete"), Record.ObjectPath, Record.StableId, Index,
					TEXT("Unknown package state is explicitly incomplete and cannot validate."));
			}
			if (Record.Kind == TEXT("asset_state")
				&& Record.PackageState == TEXT("does_not_exist") && Record.bLoaded)
			{
				Add(TEXT("package_absence_loaded_contradiction"), Record.ObjectPath, Record.StableId, Index,
					TEXT("A proven-absent package cannot simultaneously expose a loaded primary object."));
			}
			const FString ExpectedElement = FHyperAIStudioDataContracts::ComputeElementFingerprint(Record);
			if (Record.ElementFingerprint != ExpectedElement)
			{
				Add(TEXT("element_fingerprint_mismatch"), Record.ObjectPath, Record.StableId, Index,
					TEXT("The persisted element CAS seal does not match the closed record values."));
			}
			const FString Key = RecordSortKey(Record);
			if (Index > 0 && !(PreviousKey < Key))
			{
				Add(TEXT("record_order_or_identity_invalid"), Record.ObjectPath, Record.StableId, Index,
					TEXT("Records must be strictly ordered and cannot duplicate a stable identity."));
			}
			PreviousKey = Key;
			if (bInternalIssueOverflow) break;
		}
		if (Snapshot.Records.IsEmpty())
		{
			Add(TEXT("snapshot_empty"), FString(), FString(), -1,
				TEXT("A complete detached projection must contain at least one asset-state record."));
		}
		for (const TPair<FString, FClosure>& Pair : ClosureByPath)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return Reject(TEXT("deadline_exceeded"),
					TEXT("Independent closure validation exhausted the monotonic deadline."));
			}
			const FClosure& Closure = Pair.Value;
			const int32 HeaderCount = Closure.DataTableHeaders + Closure.CurveTableHeaders
				+ Closure.StructHeaders + Closure.EnumHeaders + Closure.IdentityOnlyRecords;
			if (Closure.StateCount != 1)
			{
				Add(TEXT("asset_state_closure_invalid"), Pair.Key, FString(), -1,
					TEXT("Every exact object path requires exactly one asset-state record."));
			}
			if (Closure.PackageState == TEXT("does_not_exist") && HeaderCount != 0)
			{
				Add(TEXT("absent_asset_has_value_records"), Pair.Key, FString(), -1,
					TEXT("A proven-absent package cannot carry loaded value records."));
			}
			if (Closure.PackageState == TEXT("exists") && !Closure.bLoaded)
			{
				Add(TEXT("existing_asset_not_loaded"), Pair.Key, FString(), -1,
					TEXT("Existing-but-unloaded evidence is intentionally incomplete."));
			}
			if (Closure.bLoaded && HeaderCount != 1)
			{
				Add(TEXT("loaded_value_header_closure_invalid"), Pair.Key, FString(), -1,
					TEXT("A loaded primary object requires exactly one closed or identity-only value header."));
			}
			if ((Closure.DataTableHeaders == 1 && Closure.DeclaredCount != Closure.DataTableRows)
				|| (Closure.CurveTableHeaders == 1 && Closure.DeclaredCount != Closure.CurveTableRows)
				|| (Closure.StructHeaders == 1 && Closure.DeclaredCount != Closure.StructFields)
				|| (Closure.EnumHeaders == 1 && Closure.DeclaredCount != Closure.EnumValues))
			{
				Add(TEXT("declared_child_count_mismatch"), Pair.Key, FString(), -1,
					TEXT("The loaded header count does not match its detached child records."));
			}
			if (Closure.IdentityOnlyRecords > 0)
			{
				Add(TEXT("identity_only_projection_incomplete"), Pair.Key, FString(), -1,
					TEXT("DataAsset fields, StringTable entries, and delegated/unsupported assets are never complete projections."));
			}
		}
		Report.RecomputedPersistedFingerprint =
			FHyperAIStudioDataContracts::ComputePersistedFingerprint(Snapshot.Records);
		Report.RecomputedVolatileObservationFingerprint =
			FHyperAIStudioDataContracts::ComputeVolatileFingerprint(Snapshot.Records);
		if (!Snapshot.bSnapshotComplete)
		{
			Add(TEXT("snapshot_marked_incomplete"), FString(), FString(), -1,
				TEXT("A detached snapshot containing Unknown or omitted values cannot validate as complete."));
		}
		if (Snapshot.PersistedFingerprint != Report.RecomputedPersistedFingerprint)
		{
			Add(TEXT("persisted_fingerprint_mismatch"), FString(), FString(), -1,
				TEXT("The whole persisted fingerprint does not match the supplied records."));
		}
		if (Snapshot.VolatileObservationFingerprint
			!= Report.RecomputedVolatileObservationFingerprint)
		{
			Add(TEXT("volatile_fingerprint_mismatch"), FString(), FString(), -1,
				TEXT("The separate volatile observation fingerprint does not match."));
		}
		if (!FHyperAIStudioDataContracts::IsCanonicalSha256(Snapshot.RequestFingerprint))
		{
			Add(TEXT("request_fingerprint_invalid"), FString(), FString(), -1,
				TEXT("The request identity is not a canonical SHA-256 value."));
		}

		int64 EstimatedBytes = BaseReportBytes;
		for (const FHyperAIDataIssue& Issue : FoundIssues)
		{
			CopyIssueWithinBound(Issue, Request.MaxIssues, Request.MaxOutputBytes,
				EstimatedBytes, Report.Issues, Report.bTruncated);
		}
		Report.bTruncated |= bInternalIssueOverflow;
		Report.ErrorCount = Report.Issues.Num();
		Report.bValid = !Report.bTruncated && FoundIssues.IsEmpty();
		Report.bOk = !Report.bTruncated;
		Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
		Report.Diagnostic = Report.bValid
			? TEXT("The full detached projection satisfies independent bounds, ordering, closure, and both fingerprint domains.")
			: TEXT("The detached projection is incomplete, malformed, truncated, or fingerprint-inconsistent.");
		return Report;
	}
}

namespace HyperAIStudio::Data::Private
{
	bool IsIdentifier(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioDataContracts::MaxNameCharacters
			|| HasControlCharacter(Value)) return false;
		const TCHAR First = Value[0];
		if (!((First >= TEXT('A') && First <= TEXT('Z'))
			|| (First >= TEXT('a') && First <= TEXT('z')) || First == TEXT('_'))) return false;
		for (int32 Index = 1; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!((Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_'))) return false;
		}
		return true;
	}

	bool IsBoundedAuthoredName(const FString& Value)
	{
		return !Value.IsEmpty()
			&& Value.Len() <= FHyperAIStudioDataContracts::MaxNameCharacters
			&& !HasControlCharacter(Value);
	}

	FString OperationKindToken(const EHyperAIStudioDataOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioDataOperationKind::UserStructCreate: return TEXT("user_struct.create");
		case EHyperAIStudioDataOperationKind::UserStructAddField: return TEXT("user_struct.add_field");
		case EHyperAIStudioDataOperationKind::UserStructRenameField: return TEXT("user_struct.rename_field");
		case EHyperAIStudioDataOperationKind::UserStructRemoveField: return TEXT("user_struct.remove_field");
		case EHyperAIStudioDataOperationKind::UserEnumCreate: return TEXT("user_enum.create");
		case EHyperAIStudioDataOperationKind::UserEnumAddValue: return TEXT("user_enum.add_value");
		case EHyperAIStudioDataOperationKind::UserEnumRenameValue: return TEXT("user_enum.rename_value");
		case EHyperAIStudioDataOperationKind::UserEnumRemoveValue: return TEXT("user_enum.remove_value");
		default: return TEXT("invalid");
		}
	}

	FString SafetyToken(const EHyperAIStudioDataPlanSafety Safety)
	{
		return Safety == EHyperAIStudioDataPlanSafety::Destructive
			? TEXT("destructive") : TEXT("edit");
	}

	bool IsStructKind(const EHyperAIStudioDataOperationKind Kind)
	{
		return Kind == EHyperAIStudioDataOperationKind::UserStructCreate
			|| Kind == EHyperAIStudioDataOperationKind::UserStructAddField
			|| Kind == EHyperAIStudioDataOperationKind::UserStructRenameField
			|| Kind == EHyperAIStudioDataOperationKind::UserStructRemoveField;
	}

	bool IsCreateKind(const EHyperAIStudioDataOperationKind Kind)
	{
		return Kind == EHyperAIStudioDataOperationKind::UserStructCreate
			|| Kind == EHyperAIStudioDataOperationKind::UserEnumCreate;
	}

	bool IsRemoveKind(const EHyperAIStudioDataOperationKind Kind)
	{
		return Kind == EHyperAIStudioDataOperationKind::UserStructRemoveField
			|| Kind == EHyperAIStudioDataOperationKind::UserEnumRemoveValue;
	}

	bool IsRenameKind(const EHyperAIStudioDataOperationKind Kind)
	{
		return Kind == EHyperAIStudioDataOperationKind::UserStructRenameField
			|| Kind == EHyperAIStudioDataOperationKind::UserEnumRenameValue;
	}

	bool IsAddKind(const EHyperAIStudioDataOperationKind Kind)
	{
		return Kind == EHyperAIStudioDataOperationKind::UserStructAddField
			|| Kind == EHyperAIStudioDataOperationKind::UserEnumAddValue;
	}

	FString CanonicalOperation(const FHyperAIStudioDataBackendOperation& Operation)
	{
		FString Canonical(TEXT("hyperai.data.operation.v1|"));
		AppendToken(Canonical, OperationKindToken(Operation.Kind));
		AppendToken(Canonical, SafetyToken(Operation.Safety));
		AppendToken(Canonical, Operation.SubjectId);
		AppendToken(Canonical, Operation.ExpectedElementFingerprint);
		AppendToken(Canonical, Operation.Name);
		AppendToken(Canonical, Operation.ValueType);
		AppendToken(Canonical, Operation.DisplayName);
		return Canonical;
	}

	FHyperAIDataRecord* FindHeader(TArray<FHyperAIDataRecord>& Records, const FString& Kind)
	{
		return Records.FindByPredicate([&Kind](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == Kind;
		});
	}

	FHyperAIDataRecord* FindChild(
		TArray<FHyperAIDataRecord>& Records,
		const FString& Kind,
		const FString& StableId)
	{
		return Records.FindByPredicate([&Kind, &StableId](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == Kind && Record.StableId == StableId;
		});
	}

	bool NameExists(
		const TArray<FHyperAIDataRecord>& Records,
		const FString& Kind,
		const FString& Name,
		const FString& ExceptStableId = FString())
	{
		return Records.ContainsByPredicate([&](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == Kind && Record.Name == Name
				&& (ExceptStableId.IsEmpty() || Record.StableId != ExceptStableId);
		});
	}

	FString PlannedStableId(
		const FString& TargetPath,
		const FString& Kind,
		const FString& Name,
		const int32 Index)
	{
		FString Canonical(TEXT("hyperai.data.planned-id.v1|"));
		AppendToken(Canonical, TargetPath);
		AppendToken(Canonical, Kind);
		AppendToken(Canonical, Name);
		AppendToken(Canonical, LexToString(Index));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool ValidateDesiredRecords(
		const FString& TargetPath,
		const TArray<FHyperAIDataRecord>& Records,
		FString& OutError)
	{
		const FHyperAIDataRecord* StructHeader = Records.FindByPredicate([](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == TEXT("user_struct");
		});
		const FHyperAIDataRecord* EnumHeader = Records.FindByPredicate([](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == TEXT("user_enum");
		});
		if (!!StructHeader == !!EnumHeader)
		{
			OutError = TEXT("Desired shadow must contain exactly one user_struct or user_enum header.");
			return false;
		}
		const FString ChildKind = StructHeader ? TEXT("user_struct_field") : TEXT("user_enum_value");
		const int32 MaxChildren = StructHeader
			? FHyperAIStudioDataContracts::MaxFieldsPerStruct
			: FHyperAIStudioDataContracts::MaxValuesPerEnum;
		TSet<FString> StableIds;
		TSet<FString> Names;
		int32 Count = 0;
		for (const FHyperAIDataRecord& Record : Records)
		{
			if (Record.ObjectPath != TargetPath)
			{
				OutError = TEXT("Desired shadow escaped the exact target object path.");
				return false;
			}
			if (Record.Kind != ChildKind) continue;
			++Count;
			if (!IsBoundedAuthoredName(Record.Name) || StableIds.Contains(Record.StableId)
				|| Names.Contains(Record.Name))
			{
				OutError = TEXT("Desired child names and stable identities must be unique and closed.");
				return false;
			}
			StableIds.Add(Record.StableId);
			Names.Add(Record.Name);
		}
		if (Count > MaxChildren || (StructHeader ? StructHeader : EnumHeader)->Count != Count)
		{
			OutError = TEXT("Desired child count violates its hard cap or header invariant.");
			return false;
		}
		if (StructHeader && Count < 1)
		{
			OutError = TEXT("A user-struct desired state must retain at least one member.");
			return false;
		}
		return true;
	}

	FString ResultSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.result.data.struct-enum-plan.v1|ok|dry|prepared|zero_effect|status|plan_hash|authorization_hash|capability_hash|effect_hash|effects|issues"));
		return Value;
	}
}

FString FHyperAIStudioDataContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioData.HyperAIStudioDataToolset");
}

const TArray<FHyperAIStudioDataManifestEntry>& FHyperAIStudioDataContracts::GetManifest()
{
	static const TArray<FHyperAIStudioDataManifestEntry> Manifest = {
		{TEXT("hyper_data_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_data_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_data_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioDataContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioDataManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3 || FHyperAIStudioDataDelegationMatrix::GetEpic().Num() != 56
		|| FHyperAIStudioDataDelegationMatrix::GetRequirements().Num() != 18) return false;
	TArray<FString> Names;
	TSet<FString> UniqueNames;
	for (const FHyperAIStudioDataManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| UniqueNames.Contains(Entry.Name)) return false;
		UniqueNames.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioDataContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("\\")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))
		|| HyperAIStudio::Data::Private::HasControlCharacter(Path)) return false;
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FSoftObjectPath SoftPath(Path);
	if (!SoftPath.IsValid() || !SoftPath.GetSubPathUtf8String().IsEmpty()) return false;
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !ObjectName.IsEmpty() && ObjectName.Len() <= MaxNameCharacters
		&& FPackageName::GetShortName(PackageName) == ObjectName;
}

bool FHyperAIStudioDataContracts::IsCanonicalSha256(const FString& Value)
{
	if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioDataContracts::ClassifyPackageExistence(const int32 StateValue)
{
	if (StateValue == static_cast<int32>(UE::AssetRegistry::EExists::Exists)) return TEXT("exists");
	if (StateValue == static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist))
	{
		return TEXT("does_not_exist");
	}
	return TEXT("unknown");
}

bool FHyperAIStudioDataContracts::IsCreateExistenceAdmitted(
	const int32 StateValue,
	const bool bLoadedPackagePresent)
{
	return !bLoadedPackagePresent
		&& StateValue == static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist);
}

int64 FHyperAIStudioDataContracts::EstimateWorstCaseJsonStringBytes(const FString& Value)
{
	return 2ll + (6ll * Value.Len());
}

FString FHyperAIStudioDataContracts::ComputeElementFingerprint(const FHyperAIDataRecord& Record)
{
	FString Canonical(TEXT("hyperai.data.record.persisted.v1|"));
	using HyperAIStudio::Data::Private::AppendToken;
	using HyperAIStudio::Data::Private::BoolToken;
	AppendToken(Canonical, Record.Kind);
	AppendToken(Canonical, Record.ObjectPath);
	AppendToken(Canonical, Record.StableId);
	AppendToken(Canonical, Record.Name);
	AppendToken(Canonical, Record.TypeId);
	AppendToken(Canonical, Record.Value);
	AppendToken(Canonical, Record.SecondaryValue);
	AppendToken(Canonical, LexToString(Record.Index));
	AppendToken(Canonical, LexToString(Record.Count));
	AppendToken(Canonical, LexToString(Record.IntegerValue));
	AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Record.NumberValue));
	AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Record.SecondaryNumberValue));
	AppendToken(Canonical, BoolToken(Record.bFlag));
	if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioDataContracts::ComputePersistedFingerprint(
	const TArray<FHyperAIDataRecord>& Records)
{
	FString Canonical(TEXT("hyperai.data.snapshot.persisted.v1|"));
	HyperAIStudio::Data::Private::AppendToken(Canonical, LexToString(Records.Num()));
	for (const FHyperAIDataRecord& Record : Records)
	{
		const FString Element = ComputeElementFingerprint(Record);
		if (!IsCanonicalSha256(Element)) return FString();
		HyperAIStudio::Data::Private::AppendToken(Canonical, Element);
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioDataContracts::ComputeVolatileFingerprint(
	const TArray<FHyperAIDataRecord>& Records)
{
	FString Canonical(TEXT("hyperai.data.snapshot.volatile.v1|"));
	HyperAIStudio::Data::Private::AppendToken(Canonical, LexToString(Records.Num()));
	for (const FHyperAIDataRecord& Record : Records)
	{
		HyperAIStudio::Data::Private::AppendToken(Canonical, Record.ObjectPath);
		HyperAIStudio::Data::Private::AppendToken(Canonical, Record.Kind);
		HyperAIStudio::Data::Private::AppendToken(Canonical, Record.StableId);
		HyperAIStudio::Data::Private::AppendToken(Canonical, Record.PackageState);
		HyperAIStudio::Data::Private::AppendToken(Canonical,
			HyperAIStudio::Data::Private::BoolToken(Record.bLoaded));
		HyperAIStudio::Data::Private::AppendToken(Canonical,
			HyperAIStudio::Data::Private::BoolToken(Record.bDirty));
		HyperAIStudio::Data::Private::AppendToken(Canonical,
			HyperAIStudio::Data::Private::BoolToken(Record.bWasLoaded));
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

bool FHyperAIStudioDataContracts::DecodeCursor(
	const FString& Cursor,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint,
	const FString& VolatileFingerprint,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.IsEmpty() || Cursor.Len() > MaxCursorCharacters) return false;
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT("|"), false);
	if (Parts.Num() != 5 || Parts[0] != TEXT("v1")
		|| Parts[2] != RequestFingerprint || Parts[3] != PersistedFingerprint
		|| Parts[4] != VolatileFingerprint) return false;
	if (!LexTryParseString(OutOffset, *Parts[1]) || OutOffset < 0) return false;
	return EncodeCursor(OutOffset, RequestFingerprint, PersistedFingerprint, VolatileFingerprint)
		== Cursor;
}

FString FHyperAIStudioDataContracts::EncodeCursor(
	const int32 Offset,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint,
	const FString& VolatileFingerprint)
{
	if (Offset < 0 || !IsCanonicalSha256(RequestFingerprint)
		|| !IsCanonicalSha256(PersistedFingerprint)
		|| !IsCanonicalSha256(VolatileFingerprint)) return FString();
	const FString Cursor = FString::Printf(TEXT("v1|%d|%s|%s|%s"), Offset,
		*RequestFingerprint, *PersistedFingerprint, *VolatileFingerprint);
	return Cursor.Len() <= MaxCursorCharacters ? Cursor : FString();
}

bool FHyperAIStudioDataContracts::ClassifyOperation(
	const FString& Kind,
	EHyperAIStudioDataOperationKind& OutKind,
	EHyperAIStudioDataPlanSafety& OutSafety)
{
	if (Kind == TEXT("user_struct.create")) OutKind = EHyperAIStudioDataOperationKind::UserStructCreate;
	else if (Kind == TEXT("user_struct.add_field")) OutKind = EHyperAIStudioDataOperationKind::UserStructAddField;
	else if (Kind == TEXT("user_struct.rename_field")) OutKind = EHyperAIStudioDataOperationKind::UserStructRenameField;
	else if (Kind == TEXT("user_struct.remove_field")) OutKind = EHyperAIStudioDataOperationKind::UserStructRemoveField;
	else if (Kind == TEXT("user_enum.create")) OutKind = EHyperAIStudioDataOperationKind::UserEnumCreate;
	else if (Kind == TEXT("user_enum.add_value")) OutKind = EHyperAIStudioDataOperationKind::UserEnumAddValue;
	else if (Kind == TEXT("user_enum.rename_value")) OutKind = EHyperAIStudioDataOperationKind::UserEnumRenameValue;
	else if (Kind == TEXT("user_enum.remove_value")) OutKind = EHyperAIStudioDataOperationKind::UserEnumRemoveValue;
	else return false;
	OutSafety = HyperAIStudio::Data::Private::IsRemoveKind(OutKind)
		? EHyperAIStudioDataPlanSafety::Destructive : EHyperAIStudioDataPlanSafety::Edit;
	return true;
}

bool FHyperAIStudioDataContracts::ValidateOperationShape(
	const FHyperAIDataPlanOperation& Operation,
	FHyperAIStudioDataBackendOperation& OutOperation,
	FString& OutError)
{
	OutOperation = {};
	if (!ClassifyOperation(Operation.Kind, OutOperation.Kind, OutOperation.Safety))
	{
		OutError = TEXT("Operation kind is outside the closed struct/enum vocabulary.");
		return false;
	}
	if (Operation.SubjectId.Len() > MaxNameCharacters
		|| Operation.Name.Len() > MaxNameCharacters
		|| Operation.ValueType.Len() > 32 || Operation.DisplayName.Len() > MaxTextCharacters
		|| HyperAIStudio::Data::Private::HasControlCharacter(Operation.SubjectId)
		|| HyperAIStudio::Data::Private::HasControlCharacter(Operation.Name)
		|| HyperAIStudio::Data::Private::HasControlCharacter(Operation.ValueType)
		|| HyperAIStudio::Data::Private::HasControlCharacter(Operation.DisplayName))
	{
		OutError = TEXT("Operation strings violate the closed scalar bounds.");
		return false;
	}
	const bool bCreate = HyperAIStudio::Data::Private::IsCreateKind(OutOperation.Kind);
	const bool bAdd = HyperAIStudio::Data::Private::IsAddKind(OutOperation.Kind);
	const bool bRename = HyperAIStudio::Data::Private::IsRenameKind(OutOperation.Kind);
	const bool bRemove = HyperAIStudio::Data::Private::IsRemoveKind(OutOperation.Kind);
	if (bCreate)
	{
		if (!Operation.SubjectId.IsEmpty() || !Operation.ExpectedElementFingerprint.IsEmpty()
			|| !Operation.Name.IsEmpty() || !Operation.ValueType.IsEmpty()
			|| !Operation.DisplayName.IsEmpty())
		{
			OutError = TEXT("Create carries no free-form fields; the exact target path supplies identity.");
			return false;
		}
	}
	else if (bAdd)
	{
		if (!Operation.SubjectId.IsEmpty() || !Operation.ExpectedElementFingerprint.IsEmpty()
			|| !HyperAIStudio::Data::Private::IsIdentifier(Operation.Name))
		{
			OutError = TEXT("Add requires only a valid new name and carries no existing-element CAS.");
			return false;
		}
		if (OutOperation.Kind == EHyperAIStudioDataOperationKind::UserStructAddField)
		{
			static const TSet<FString> PrimitiveTypes = {
				TEXT("bool"), TEXT("int32"), TEXT("float"), TEXT("name"),
				TEXT("string"), TEXT("text"), TEXT("vector"), TEXT("rotator")};
			if (!PrimitiveTypes.Contains(Operation.ValueType) || !Operation.DisplayName.IsEmpty())
			{
				OutError = TEXT("Struct add accepts one exact primitive type and no display text.");
				return false;
			}
		}
		else if (!Operation.ValueType.IsEmpty())
		{
			OutError = TEXT("Enum add does not accept a value-type token.");
			return false;
		}
	}
	else if (bRename || bRemove)
	{
		if (Operation.SubjectId.IsEmpty()
			|| !IsCanonicalSha256(Operation.ExpectedElementFingerprint)
			|| !Operation.ValueType.IsEmpty() || !Operation.DisplayName.IsEmpty()
			|| (bRename && !HyperAIStudio::Data::Private::IsIdentifier(Operation.Name))
			|| (bRemove && !Operation.Name.IsEmpty()))
		{
			OutError = TEXT("Rename/remove requires exact element CAS and only its closed subject/new-name fields.");
			return false;
		}
	}
	OutOperation.SubjectId = Operation.SubjectId;
	OutOperation.ExpectedElementFingerprint = Operation.ExpectedElementFingerprint;
	OutOperation.Name = Operation.Name;
	OutOperation.ValueType = Operation.ValueType;
	OutOperation.DisplayName = Operation.DisplayName;
	return true;
}

bool FHyperAIStudioDataContracts::IsFastReversibleEditPlan(
	const TArray<FHyperAIStudioDataBackendOperation>& Operations)
{
	return Operations.Num() == 1
		&& Operations[0].Kind == EHyperAIStudioDataOperationKind::UserStructRenameField
		&& Operations[0].Safety == EHyperAIStudioDataPlanSafety::Edit;
}

bool FHyperAIStudioDataContracts::ReplayShadowForTest(
	const FString& TargetPath,
	const TArray<FHyperAIDataRecord>& BaseRecords,
	const TArray<FHyperAIStudioDataBackendOperation>& Operations,
	TArray<FHyperAIDataRecord>& OutDesired,
	FHyperAIDataPlanEffects& OutEffects,
	FString& OutError)
{
	using namespace HyperAIStudio::Data::Private;
	OutDesired = BaseRecords;
	OutEffects = {};
	OutError.Reset();
	bool bFamilySet = false;
	bool bStructFamily = false;
	bool bCreated = false;
	TSet<FString> ConsumedSubjects;
	for (int32 OperationIndex = 0; OperationIndex < Operations.Num(); ++OperationIndex)
	{
		const FHyperAIStudioDataBackendOperation& Operation = Operations[OperationIndex];
		if (!Operation.SubjectId.IsEmpty())
		{
			if (ConsumedSubjects.Contains(Operation.SubjectId))
			{
				OutError = TEXT("Each existing element CAS subject may be consumed only once per plan.");
				return false;
			}
			ConsumedSubjects.Add(Operation.SubjectId);
		}
		const bool bStruct = IsStructKind(Operation.Kind);
		if (bFamilySet && bStructFamily != bStruct)
		{
			OutError = TEXT("A compound plan cannot mix user-struct and user-enum families.");
			return false;
		}
		bFamilySet = true;
		bStructFamily = bStruct;
		++OutEffects.OperationCount;
		if (IsCreateKind(Operation.Kind))
		{
			if (OperationIndex != 0 || bCreated || FindHeader(OutDesired, TEXT("user_struct"))
				|| FindHeader(OutDesired, TEXT("user_enum")))
			{
				OutError = TEXT("Create must be the first and only family-establishing operation.");
				return false;
			}
			const FHyperAIDataRecord* State = OutDesired.FindByPredicate([](const FHyperAIDataRecord& Record)
			{
				return Record.Kind == TEXT("asset_state");
			});
			if (!State || State->PackageState != TEXT("does_not_exist") || State->bLoaded)
			{
				OutError = TEXT("Create requires proven package absence and no loaded primary object.");
				return false;
			}
			FHyperAIDataRecord& Header = OutDesired.AddDefaulted_GetRef();
			Header.Kind = bStruct ? TEXT("user_struct") : TEXT("user_enum");
			Header.ObjectPath = TargetPath;
			Header.StableId = bStruct ? TEXT("struct") : TEXT("enum");
			Header.Name = FPackageName::ObjectPathToObjectName(TargetPath);
			Header.TypeId = bStruct ? TEXT("planned_user_defined_struct") : TEXT("user_defined_enum");
			Header.Count = 0;
			++OutEffects.CreateCount;
			bCreated = true;
			continue;
		}

		FHyperAIDataRecord* Header = FindHeader(
			OutDesired, bStruct ? TEXT("user_struct") : TEXT("user_enum"));
		if (!Header)
		{
			OutError = TEXT("The requested loaded struct/enum family does not match the compound plan.");
			return false;
		}
		const FString ChildKind = bStruct ? TEXT("user_struct_field") : TEXT("user_enum_value");
		if (IsAddKind(Operation.Kind))
		{
			if (NameExists(OutDesired, ChildKind, Operation.Name))
			{
				OutError = TEXT("Add would duplicate an authored child name.");
				return false;
			}
			const int64 PlannedEnumValue = Header->Count;
			++Header->Count;
			FHyperAIDataRecord& Child = OutDesired.AddDefaulted_GetRef();
			Child.Kind = ChildKind;
			Child.ObjectPath = TargetPath;
			Child.StableId = PlannedStableId(TargetPath, ChildKind, Operation.Name, OperationIndex);
			Child.Name = Operation.Name;
			Child.TypeId = bStruct ? TEXT("primitive:") + Operation.ValueType : TEXT("enum_value");
			Child.Value = bStruct ? FString() : Operation.DisplayName;
			Child.IntegerValue = bStruct ? 0 : PlannedEnumValue;
			++OutEffects.AddCount;
			continue;
		}

		FHyperAIDataRecord* Child = FindChild(OutDesired, ChildKind, Operation.SubjectId);
		if (!Child || Child->ElementFingerprint != Operation.ExpectedElementFingerprint)
		{
			OutError = TEXT("Element CAS failed for the exact rename/remove subject.");
			return false;
		}
		if (IsRenameKind(Operation.Kind))
		{
			if (NameExists(OutDesired, ChildKind, Operation.Name, Child->StableId))
			{
				OutError = TEXT("Rename would duplicate an authored child name.");
				return false;
			}
			Child->Name = Operation.Name;
			if (!bStruct) Child->StableId = Operation.Name;
			++OutEffects.RenameCount;
		}
		else
		{
			const FString StableId = Child->StableId;
			OutDesired.RemoveAll([&](const FHyperAIDataRecord& Record)
			{
				return Record.Kind == ChildKind && Record.StableId == StableId;
			});
			Header = FindHeader(OutDesired, bStruct ? TEXT("user_struct") : TEXT("user_enum"));
			if (!Header || Header->Count <= 0)
			{
				OutError = TEXT("Shadow header count became invalid after removal.");
				return false;
			}
			--Header->Count;
			++OutEffects.RemoveCount;
		}
	}
	if (!bFamilySet)
	{
		OutError = TEXT("At least one closed struct/enum operation is required.");
		return false;
	}
	SortAndSealRecords(OutDesired);
	if (!ValidateDesiredRecords(TargetPath, OutDesired, OutError)) return false;
	OutEffects.bWouldCompileOnce = true;
	OutEffects.bWouldSaveOnce = true;
	OutEffects.bWouldValidateOnce = true;
	OutEffects.bWouldVerifyFreshOnce = true;
	return true;
}

FString FHyperAIStudioDataContracts::PayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.payload.data.struct-enum-plan.v1|target_path|base_persisted|desired_persisted|safety|canonical_closed_operations|semantic"));
	return Value;
}

FString FHyperAIStudioDataContracts::ComputePayloadSemanticFingerprint(
	const FHyperAIStudioDataPlanPayload& Payload)
{
	FString Canonical(TEXT("hyperai.data.payload.semantic.v1|"));
	HyperAIStudio::Data::Private::AppendToken(Canonical, Payload.TargetPath);
	HyperAIStudio::Data::Private::AppendToken(Canonical, Payload.BasePersistedFingerprint);
	HyperAIStudio::Data::Private::AppendToken(Canonical, Payload.DesiredPersistedFingerprint);
	HyperAIStudio::Data::Private::AppendToken(Canonical,
		HyperAIStudio::Data::Private::SafetyToken(Payload.Safety));
	for (const FString& Operation : Payload.CanonicalOperations)
	{
		HyperAIStudio::Data::Private::AppendToken(Canonical, Operation);
		if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioDataContracts::GetPreparationDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.data.preparation-only.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_data_apply_plan"), EditVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(),
			TEXT("hyperai.result.data.struct-enum-plan.v1"),
			HyperAIStudio::Data::Private::ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_data_apply_plan"), DestructiveVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(),
			TEXT("hyperai.result.data.struct-enum-plan.v1"),
			HyperAIStudio::Data::Private::ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Destructive});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FString FHyperAIStudioDataPlanPayload::GetTypeId() const
{
	return FHyperAIStudioDataContracts::PayloadTypeId;
}

FString FHyperAIStudioDataPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioDataContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioDataPlanPayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (TargetPath.Len() + BasePersistedFingerprint.Len()
		+ DesiredPersistedFingerprint.Len() + SemanticFingerprint.Len());
	for (const FString& Operation : CanonicalOperations) Size += 32ll + 2ll * Operation.Len();
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioDataPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->DesiredPersistedFingerprint = DesiredPersistedFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	Clone->Safety = Safety;
	Clone->CanonicalOperations = CanonicalOperations;
	return Clone;
}

FHyperAIDataApplyPlanReport FHyperAIStudioDataContracts::BuildPlan(
	const FHyperAIDataApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Data::Private;
	FHyperAIDataApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	auto Reject = [&Report](const FString& Status, const FString& Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Fresh loaded-object CAS projection is admitted only on the Unreal game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < 4096 || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("request_bounds_invalid"),
			TEXT("Target path, operation count, deadline, or output envelope is invalid."));
	}
	if (!IsCanonicalSha256(Request.ExpectedPersistedFingerprint))
	{
		return Reject(TEXT("whole_snapshot_cas_required"),
			TEXT("A canonical whole loaded-projection persisted fingerprint is mandatory."));
	}
	if (!Request.bDryRun
		&& (Request.OperationId.IsEmpty()
			|| Request.OperationId.Len() > FHyperAIStudioDomainLimits::MaxOperationIdChars
			|| HasControlCharacter(Request.OperationId)))
	{
		return Reject(TEXT("operation_id_invalid"),
			TEXT("Non-dry intent requires one bounded caller operation id."));
	}

	const double Deadline = FPlatformTime::Seconds()
		+ (static_cast<double>(Request.DeadlineMs) / 1000.0);
	TArray<FHyperAIStudioDataBackendOperation> Operations;
	Operations.Reserve(Request.Operations.Num());
	EHyperAIStudioDataPlanSafety PlanSafety = EHyperAIStudioDataPlanSafety::Edit;
	bool bStructFamily = false;
	bool bFamilySet = false;
	for (const FHyperAIDataPlanOperation& Operation : Request.Operations)
	{
		FHyperAIStudioDataBackendOperation Parsed;
		FString Error;
		if (!ValidateOperationShape(Operation, Parsed, Error))
		{
			return Reject(TEXT("operation_shape_invalid"), Error);
		}
		const bool bStruct = IsStructKind(Parsed.Kind);
		if (bFamilySet && bStruct != bStructFamily)
		{
			return Reject(TEXT("mixed_operation_family"),
				TEXT("A compound plan cannot mix user-struct and user-enum operations."));
		}
		bFamilySet = true;
		bStructFamily = bStruct;
		if (Parsed.Safety == EHyperAIStudioDataPlanSafety::Destructive)
		{
			PlanSafety = EHyperAIStudioDataPlanSafety::Destructive;
		}
		Operations.Add(MoveTemp(Parsed));
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("deadline_exceeded"), TEXT("Closed operation parsing exhausted the deadline."));
	}

	const int32 RemainingCaptureMs = FMath::FloorToInt(
		FMath::Max(0.0, Deadline - FPlatformTime::Seconds()) * 1000.0);
	if (RemainingCaptureMs < 1)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("No monotonic budget remains for fresh loaded-only CAS capture."));
	}
	const TArray<FString> CapturePaths = {Request.TargetPath};
	FCapture Base = CaptureLoadedProjection(
		CapturePaths, false, FMath::Min(Request.DeadlineMs, RemainingCaptureMs));
	if (Base.bDeadlineExceeded)
	{
		return Reject(TEXT("deadline_exceeded"), TEXT("Fresh loaded-only CAS capture exhausted the deadline."));
	}
	if (!Base.bComplete)
	{
		const FHyperAIDataRecord* State = Base.Records.FindByPredicate([](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == TEXT("asset_state");
		});
		if (State && State->PackageState == TEXT("unknown"))
		{
			return Reject(TEXT("package_existence_unknown"),
				TEXT("Unknown package state is incomplete and can never authorize create or edit."));
		}
		return Reject(TEXT("fresh_loaded_projection_incomplete"),
			TEXT("The exact target is unloaded, unsupported, delegated, invalid, or outside a projection cap."));
	}
	Report.BasePersistedFingerprint = Base.PersistedFingerprint;
	if (Base.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("whole_snapshot_cas_mismatch"),
			TEXT("Fresh persisted loaded projection differs from the caller's whole-snapshot CAS."));
	}

	const bool bCreate = IsCreateKind(Operations[0].Kind);
	if (bCreate)
	{
		if (!IsIdentifier(FPackageName::ObjectPathToObjectName(Request.TargetPath)))
		{
			return Reject(TEXT("create_asset_name_invalid"),
				TEXT("Create requires a bounded identifier as the exact primary asset name."));
		}
		const FHyperAIDataRecord* State = Base.Records.FindByPredicate([](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == TEXT("asset_state");
		});
		if (!State || State->PackageState == TEXT("unknown"))
		{
			return Reject(TEXT("package_existence_unknown"),
				TEXT("Create requires a fail-fast Asset Registry DoesNotExist result."));
		}
		if (State->PackageState != TEXT("does_not_exist") || State->bLoaded)
		{
			return Reject(TEXT("target_already_exists"),
				TEXT("Create is closed unless package absence and no loaded primary object are both proven."));
		}
	}
	else
	{
		const FString RequiredHeader = bStructFamily ? TEXT("user_struct") : TEXT("user_enum");
		if (!Base.Records.ContainsByPredicate([&](const FHyperAIDataRecord& Record)
		{
			return Record.Kind == RequiredHeader;
		}))
		{
			return Reject(TEXT("target_type_mismatch"),
				TEXT("Non-create plans require the exact target family already loaded."));
		}
	}
	for (int32 Index = bCreate ? 1 : 0; Index < Operations.Num(); ++Index)
	{
		if (IsCreateKind(Operations[Index].Kind))
		{
			return Reject(TEXT("create_order_invalid"),
				TEXT("Create may occur exactly once and only as the first operation."));
		}
	}

	TArray<FHyperAIDataRecord> Desired;
	FHyperAIDataPlanEffects Effects;
	FString ShadowError;
	if (!ReplayShadowForTest(Request.TargetPath, Base.Records, Operations,
		Desired, Effects, ShadowError))
	{
		return Reject(TEXT("shadow_replay_failed"), ShadowError);
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Immutable shadow replay exhausted the monotonic deadline."));
	}
	Report.DesiredPersistedFingerprint = ComputePersistedFingerprint(Desired);
	if (!IsCanonicalSha256(Report.DesiredPersistedFingerprint)
		|| Report.DesiredPersistedFingerprint == Base.PersistedFingerprint)
	{
		return Reject(TEXT("plan_has_no_sealed_persisted_effect"),
			TEXT("The immutable desired state must differ and must seal within the canonical bound."));
	}
	Report.Effects = Effects;
	Report.SafetyClass = SafetyToken(PlanSafety);
	Report.VariantId = PlanSafety == EHyperAIStudioDataPlanSafety::Destructive
		? DestructiveVariantId : EditVariantId;

	const TSharedRef<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioDataPlanPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedFingerprint = Base.PersistedFingerprint;
	Payload->DesiredPersistedFingerprint = Report.DesiredPersistedFingerprint;
	Payload->Safety = PlanSafety;
	for (const FHyperAIStudioDataBackendOperation& Operation : Operations)
	{
		const FString Canonical = CanonicalOperation(Operation);
		if (Canonical.Len() > MaxTextCharacters)
		{
			return Reject(TEXT("operation_canonical_bound_exceeded"),
				TEXT("A closed operation exceeded the canonical payload bound."));
		}
		Payload->CanonicalOperations.Add(Canonical);
	}
	Payload->SemanticFingerprint = ComputePayloadSemanticFingerprint(*Payload);
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	if (!IsCanonicalSha256(Payload->SemanticFingerprint)
		|| Payload->GetBoundedByteSize() > MaxOutputBytes)
	{
		return Reject(TEXT("semantic_identity_unavailable"),
			TEXT("The closed typed plan could not be bounded and semantically sealed."));
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
			TEXT("The typed data plan failed independent deep immutable clone verification."));
	}

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (ProjectId.IsEmpty())
	{
		return Reject(TEXT("canonical_project_identity_unavailable"),
			TEXT("Pure typed preparation requires canonical current-project identity."));
	}
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetPreparationDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_data_apply_plan");
	Binding.VariantId = Report.VariantId;
	Binding.ExpectedSafety = PlanSafety == EHyperAIStudioDataPlanSafety::Destructive
		? EHyperAIStudioDomainSafety::Destructive : EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations.Reset();
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bReadAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.bDestructiveAdmitted = false;
	Binding.Admission.bExternalEffectAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = PayloadTypeId;
	Contract.ArtifactSchemaFingerprint = PayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Contract.EffectTarget = TEXT("data:")
		+ FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Request.TargetPath)
		+ TEXT(":") + Base.PersistedFingerprint;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = Operations.Num() + 4;
	Contract.MaxGameThreadMs = FMath::Min(Request.DeadlineMs, 250);
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = true;
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), PrepareError);
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Public pure typed preparation exhausted the monotonic deadline."));
	}
	Report.bTypedPrepared = true;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_zero_effect");
		Report.Diagnostic = TEXT("Fresh whole-snapshot and element CAS, closed typed operations, immutable shadow replay, desired-state validation, deep clone, and public pure Prepare hashes are valid. No UObject mutation, compile, save, staging, submission, config/file, or localization effect occurred.");
		return Report;
	}
	if (!IsCanonicalSha256(Request.ExpectedPlanHash)
		|| Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("expected_plan_hash_mismatch"),
			TEXT("Non-dry intent must echo the exact plan hash from the fresh dry-run."));
	}
	if (FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
		EHyperAIStudioDomainSafety::Edit))
	{
		return Reject(NonDryCallableState,
			TEXT("Strict Safety keeps reversible data edits behind the staged-backend gate."));
	}
	if (!IsFastReversibleEditPlan(Operations))
	{
		return Reject(NonDryCallableState,
			TEXT("Fast mode currently executes only one existing user-struct field rename; create, add, remove, enum, compound, and destructive plans remain gated."));
	}

	UUserDefinedStruct* Target = FindObjectSafe<UUserDefinedStruct>(nullptr, *Request.TargetPath);
	UPackage* Package = Target ? Target->GetOutermost() : nullptr;
	if (!Target || Target->GetPathName() != Request.TargetPath || !Package
		|| Target->GetOuter() != Package
		|| !Package->GetName().StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !Package->IsFullyLoaded())
	{
		return Reject(TEXT("fast_target_unavailable"),
			TEXT("Fast rename requires the exact loaded top-level user-struct asset in a fully loaded /Game package."));
	}

	FGuid FieldGuid;
	const FHyperAIStudioDataBackendOperation& Rename = Operations[0];
	if (!FGuid::ParseExact(Rename.SubjectId, EGuidFormats::DigitsWithHyphensLower, FieldGuid)
		|| !FStructureEditorUtils::GetVarDescByGuid(Target, FieldGuid))
	{
		return Reject(TEXT("fast_field_identity_changed"),
			TEXT("The exact field GUID is no longer available after preflight."));
	}

	// RenameVariable owns the single public UE transaction, Modify, compile and dirty notification.
	if (!FStructureEditorUtils::RenameVariable(Target, FieldGuid, Rename.Name))
	{
		return Reject(TEXT("public_setter_failed"),
			TEXT("UE 5.8 rejected the reversible user-struct field rename."));
	}
	Report.bExecutionSubmitted = true;

	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GWarn;
	if (!UPackage::SavePackage(Package, Target, *Filename, SaveArgs)
		|| Package->IsDirty())
	{
		Report.Status = TEXT("save_failed_asset_dirty");
		Report.Diagnostic = TEXT("The reversible field rename ran, but the single package save failed. Undo or source-control revert is available.");
		return Report;
	}

	const FStructVariableDescription* Updated =
		FStructureEditorUtils::GetVarDescByGuid(Target, FieldGuid);
	if (!Updated || Updated->FriendlyName != Rename.Name)
	{
		Report.Status = TEXT("saved_postcondition_failed");
		Report.Diagnostic = TEXT("The package saved, but the requested field-name postcondition failed; source-control revert is available.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = TEXT("executed_fast_reversible_edit");
	Report.Diagnostic = TEXT("Renamed one existing user-struct field through the public UE 5.8 editor utility, using its single transaction/compile, one package save, and one final postcondition.");
	return Report;
}

FHyperAIDataInspectReport UHyperAIStudioDataToolset::hyper_data_inspect(
	const FHyperAIDataInspectRequest& Request)
{
	return HyperAIStudio::Data::Private::Inspect(Request);
}

FHyperAIDataApplyPlanReport UHyperAIStudioDataToolset::hyper_data_apply_plan(
	const FHyperAIDataApplyPlanRequest& Request)
{
	return FHyperAIStudioDataContracts::BuildPlan(Request);
}

FHyperAIDataValidateReport UHyperAIStudioDataToolset::hyper_data_validate(
	const FHyperAIDataValidateRequest& Request)
{
	return HyperAIStudio::Data::Private::Validate(Request);
}

void FHyperAIStudioDataRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioDataRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioDataRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioDataRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioDataContracts::IsRegistrationAllowed(bDev) && bOwnsToolset
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioDataToolset::StaticClass(),
			FHyperAIStudioDataContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioDataRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()) return;
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioDataContracts::IsRegistrationAllowed(bDev))
	{
		UE_LOG(LogHyperAIStudioData, Verbose,
			TEXT("Data exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioDataToolset::StaticClass(),
		FHyperAIStudioDataContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioData, Error,
			TEXT("Data three-tool cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioDataRegistration::RollBackRegistration()
{
	if (!bOwnsToolset || !IsInGameThread() || !UObjectInitialized()) return;
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
		UHyperAIStudioDataToolset::StaticClass(),
		FHyperAIStudioDataContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioData, Error,
			TEXT("Data owned-toolset rollback failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = false;
}
