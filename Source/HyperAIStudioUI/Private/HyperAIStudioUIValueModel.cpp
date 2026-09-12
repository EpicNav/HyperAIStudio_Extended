// Games by Hyper 2026.

#include "HyperAIStudioUIValueModel.h"

#include "HyperAIStudioExtensionRuntime.h"

namespace HyperAIStudio::UI::ValueModel::Private
{
	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical.AppendChar(TEXT('|'));
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("1") : TEXT("0");
	}

	FString NumberToken(const double Value)
	{
		return FMath::IsFinite(Value)
			? FString::Printf(TEXT("%.17g"), Value)
			: TEXT("non_finite");
	}

	FString ColorToken(const FLinearColor& Value)
	{
		return FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g"),
			Value.R, Value.G, Value.B, Value.A);
	}

	FString VectorToken(const FVector2D& Value)
	{
		return FString::Printf(TEXT("%.17g,%.17g"), Value.X, Value.Y);
	}

	FString MarginToken(const FMargin& Value)
	{
		return FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g"),
			Value.Left, Value.Top, Value.Right, Value.Bottom);
	}

	FString FieldCanonical(const FHyperAIUIFieldValue& Field)
	{
		FString Canonical;
		AppendToken(Canonical, Field.Id);
		AppendToken(Canonical, Field.Type);
		if (Field.Type == TEXT("bool"))
		{
			AppendToken(Canonical, BoolToken(Field.bBoolValue));
		}
		else if (Field.Type == TEXT("int"))
		{
			AppendToken(Canonical, LexToString(Field.IntValue));
		}
		else if (Field.Type == TEXT("number"))
		{
			AppendToken(Canonical, NumberToken(Field.NumberValue));
		}
		else if (Field.Type == TEXT("color"))
		{
			AppendToken(Canonical, ColorToken(Field.ColorValue));
		}
		else if (Field.Type == TEXT("vector2"))
		{
			AppendToken(Canonical, VectorToken(Field.Vector2Value));
		}
		else if (Field.Type == TEXT("margin"))
		{
			AppendToken(Canonical, MarginToken(Field.MarginValue));
		}
		else
		{
			AppendToken(Canonical, Field.StringValue);
		}
		return Canonical;
	}

	const FHyperAIUIFieldValue* FindField(const FHyperAIUIRecord& Record, const FString& Id)
	{
		return Record.Fields.FindByPredicate(
			[&](const FHyperAIUIFieldValue& Field) { return Field.Id == Id; });
	}

	FHyperAIUIFieldValue* FindField(FHyperAIUIRecord& Record, const FString& Id)
	{
		return Record.Fields.FindByPredicate(
			[&](const FHyperAIUIFieldValue& Field) { return Field.Id == Id; });
	}

	void SetStringField(FHyperAIUIRecord& Record, const FString& Id,
		const FString& Type, const FString& Value)
	{
		FHyperAIUIFieldValue* Existing = FindField(Record, Id);
		if (!Existing)
		{
			Existing = &Record.Fields.AddDefaulted_GetRef();
		}
		*Existing = {};
		Existing->Id = Id;
		Existing->Type = Type;
		Existing->StringValue = Value;
	}

	void SetBoolField(FHyperAIUIRecord& Record, const FString& Id, const bool Value)
	{
		FHyperAIUIFieldValue* Existing = FindField(Record, Id);
		if (!Existing) Existing = &Record.Fields.AddDefaulted_GetRef();
		*Existing = {};
		Existing->Id = Id;
		Existing->Type = TEXT("bool");
		Existing->bBoolValue = Value;
	}

	void SetNumberField(FHyperAIUIRecord& Record, const FString& Id, const double Value)
	{
		FHyperAIUIFieldValue* Existing = FindField(Record, Id);
		if (!Existing) Existing = &Record.Fields.AddDefaulted_GetRef();
		*Existing = {};
		Existing->Id = Id;
		Existing->Type = TEXT("number");
		Existing->NumberValue = Value;
	}

	void SetIntField(FHyperAIUIRecord& Record, const FString& Id, const int32 Value)
	{
		FHyperAIUIFieldValue* Existing = FindField(Record, Id);
		if (!Existing) Existing = &Record.Fields.AddDefaulted_GetRef();
		*Existing = {};
		Existing->Id = Id;
		Existing->Type = TEXT("int");
		Existing->IntValue = Value;
	}

	void SetColorField(FHyperAIUIRecord& Record, const FString& Id, const FLinearColor& Value)
	{
		FHyperAIUIFieldValue* Existing = FindField(Record, Id);
		if (!Existing) Existing = &Record.Fields.AddDefaulted_GetRef();
		*Existing = {};
		Existing->Id = Id;
		Existing->Type = TEXT("color");
		Existing->ColorValue = Value;
	}

	bool IsNameToken(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioUIContracts::MaxNameCharacters)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
			{
				return false;
			}
		}
		return true;
	}

	FHyperAIUIIssue MakeIssue(
		const FString& Code,
		const FString& Severity,
		const FString& BlueprintPath,
		const FString& RecordKey,
		const FString& Message)
	{
		FHyperAIUIIssue Issue;
		Issue.Code = Code.Left(96);
		Issue.Severity = Severity;
		Issue.BlueprintPath = BlueprintPath.Left(FHyperAIStudioUIContracts::MaxPathCharacters);
		Issue.RecordKey = RecordKey.Left(512);
		Issue.Message = Message.Left(1024);
		return Issue;
	}

	bool MatchesRecordIdentity(const FHyperAIUIRecord& Record, const FString& SubjectId)
	{
		return Record.StableId == SubjectId || Record.Name == SubjectId
			|| Record.RecordKey == SubjectId;
	}

	int32 FindRecord(
		const TArray<FHyperAIUIRecord>& Records,
		const FString& Kind,
		const FString& SubjectId)
	{
		return Records.IndexOfByPredicate([&](const FHyperAIUIRecord& Record)
		{
			return Record.Kind == Kind && MatchesRecordIdentity(Record, SubjectId);
		});
	}

	FString BlueprintPathFromSnapshot(const FHyperAIStudioUIValueSnapshot& Snapshot)
	{
		if (const FHyperAIUIRecord* Blueprint = Snapshot.Records.FindByPredicate(
			[](const FHyperAIUIRecord& Record) { return Record.Kind == TEXT("blueprint"); }))
		{
			return Blueprint->BlueprintPath;
		}
		return FString();
	}

	bool IsInteractiveClass(const FString& ClassPath)
	{
		return ClassPath.EndsWith(TEXT(".Button"))
			|| ClassPath.EndsWith(TEXT(".CheckBox"))
			|| ClassPath.EndsWith(TEXT(".Slider"))
			|| ClassPath.EndsWith(TEXT(".ComboBoxString"))
			|| ClassPath.EndsWith(TEXT(".EditableText"))
			|| ClassPath.EndsWith(TEXT(".EditableTextBox"));
	}

	bool ClassSupportsProperty(
		const FString& ClassPath,
		const FString& PropertyId,
		const bool bStyle)
	{
		if (!bStyle && (PropertyId == TEXT("visibility") || PropertyId == TEXT("is_enabled")))
		{
			return true;
		}
		if (!bStyle && PropertyId == TEXT("text")) return ClassPath.EndsWith(TEXT(".TextBlock"));
		if (!bStyle && PropertyId == TEXT("percent")) return ClassPath.EndsWith(TEXT(".ProgressBar"));
		if (!bStyle && PropertyId == TEXT("value")) return ClassPath.EndsWith(TEXT(".Slider"));
		if (!bStyle && PropertyId == TEXT("checked_state")) return ClassPath.EndsWith(TEXT(".CheckBox"));
		if (bStyle && PropertyId == TEXT("color_and_opacity"))
		{
			return ClassPath.EndsWith(TEXT(".TextBlock")) || ClassPath.EndsWith(TEXT(".Image"));
		}
		if (bStyle && PropertyId == TEXT("brush_color")) return ClassPath.EndsWith(TEXT(".Border"));
		if (bStyle && PropertyId == TEXT("fill_color_and_opacity"))
		{
			return ClassPath.EndsWith(TEXT(".ProgressBar"));
		}
		return false;
	}

	bool CheckFiniteField(const FHyperAIUIFieldValue& Field)
	{
		if (Field.Type == TEXT("number")) return FMath::IsFinite(Field.NumberValue);
		if (Field.Type == TEXT("vector2"))
		{
			return FMath::IsFinite(Field.Vector2Value.X) && FMath::IsFinite(Field.Vector2Value.Y);
		}
		if (Field.Type == TEXT("margin"))
		{
			return FMath::IsFinite(Field.MarginValue.Left)
				&& FMath::IsFinite(Field.MarginValue.Top)
				&& FMath::IsFinite(Field.MarginValue.Right)
				&& FMath::IsFinite(Field.MarginValue.Bottom);
		}
		if (Field.Type == TEXT("color"))
		{
			return FMath::IsFinite(Field.ColorValue.R) && FMath::IsFinite(Field.ColorValue.G)
				&& FMath::IsFinite(Field.ColorValue.B) && FMath::IsFinite(Field.ColorValue.A);
		}
		return true;
	}

	bool IsKnownFieldType(const FString& Type)
	{
		return Type == TEXT("string") || Type == TEXT("bool") || Type == TEXT("int")
			|| Type == TEXT("number") || Type == TEXT("color") || Type == TEXT("vector2")
			|| Type == TEXT("margin") || Type == TEXT("enum") || Type == TEXT("guid")
			|| Type == TEXT("path");
	}

	bool IsKnownRecordKind(const FString& Kind)
	{
		return Kind == TEXT("asset_state") || Kind == TEXT("blueprint")
			|| Kind == TEXT("widget") || Kind == TEXT("slot")
			|| Kind == TEXT("animation") || Kind == TEXT("animation_binding")
			|| Kind == TEXT("legacy_binding") || Kind == TEXT("mvvm_view")
			|| Kind == TEXT("mvvm_viewmodel")
			|| Kind == TEXT("mvvm_binding");
	}

	bool IsClosedFieldPath(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioUIContracts::MaxTextCharacters)
		{
			return false;
		}
		TArray<FString> Segments;
		Value.ParseIntoArray(Segments, TEXT("."), false);
		if (Segments.IsEmpty()
			|| Segments.Num() > FHyperAIStudioUIContracts::MaxMVVMPathSegments) return false;
		for (const FString& Segment : Segments)
		{
			if (!IsNameToken(Segment)) return false;
		}
		return true;
	}

	bool IsResolvedMVVMEndpoint(
		const FString& Endpoint,
		const TSet<FString>& WidgetNames,
		const TSet<FString>& ViewModelIds)
	{
		if (Endpoint == TEXT("self")) return true;
		if (Endpoint.StartsWith(TEXT("widget:"), ESearchCase::CaseSensitive))
		{
			return WidgetNames.Contains(Endpoint.RightChop(7));
		}
		if (Endpoint.StartsWith(TEXT("viewmodel:"), ESearchCase::CaseSensitive))
		{
			return ViewModelIds.Contains(Endpoint.RightChop(10));
		}
		return false;
	}

	FString BindingRecordKind(const EHyperAIStudioUIOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioUIOperationKind::BindingCreateLegacy:
		case EHyperAIStudioUIOperationKind::BindingUpdateLegacy:
		case EHyperAIStudioUIOperationKind::BindingReplaceLegacy:
		case EHyperAIStudioUIOperationKind::BindingDeleteLegacy:
			return TEXT("legacy_binding");
		case EHyperAIStudioUIOperationKind::BindingCreateMVVM:
		case EHyperAIStudioUIOperationKind::BindingUpdateMVVM:
		case EHyperAIStudioUIOperationKind::BindingReplaceMVVM:
		case EHyperAIStudioUIOperationKind::BindingDeleteMVVM:
			return TEXT("mvvm_binding");
		default:
			return FString();
		}
	}

	void PopulateBindingRecord(
		FHyperAIUIRecord& Record,
		const FHyperAIStudioUIBackendOperation& Operation,
		const FString& BlueprintPath,
		const FString& Kind)
	{
		Record.Kind = Kind;
		Record.BlueprintPath = BlueprintPath;
		Record.StableId = Operation.SubjectId;
		Record.Name = Operation.SubjectId;
		Record.RecordKey = Kind + TEXT(":") + BlueprintPath + TEXT(":") + Operation.SubjectId;
		Record.Fields.Reset();
		if (Kind == TEXT("legacy_binding"))
		{
			SetStringField(Record, TEXT("target_widget"), TEXT("string"), Operation.Name);
			SetStringField(Record, TEXT("target_property"), TEXT("string"), Operation.SecondaryName);
			SetStringField(Record, TEXT("source_function"), TEXT("string"), Operation.SourcePath);
		}
		else
		{
			SetStringField(Record, TEXT("source_endpoint"), TEXT("string"), Operation.SourceEndpoint);
			SetStringField(Record, TEXT("source_path"), TEXT("string"), Operation.SourcePath);
			SetStringField(Record, TEXT("destination_endpoint"), TEXT("string"), Operation.DestinationEndpoint);
			SetStringField(Record, TEXT("destination_path"), TEXT("string"), Operation.DestinationPath);
		}
	}
}

FString FHyperAIStudioUIValueContracts::ComputeRecordFingerprint(const FHyperAIUIRecord& Record)
{
	using namespace HyperAIStudio::UI::ValueModel::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.ui.record.v1"));
	AppendToken(Canonical, Record.Kind);
	AppendToken(Canonical, Record.RecordKey);
	AppendToken(Canonical, Record.BlueprintPath);
	AppendToken(Canonical, Record.StableId);
	AppendToken(Canonical, Record.ParentStableId);
	AppendToken(Canonical, Record.Name);
	AppendToken(Canonical, Record.ClassPath);
	AppendToken(Canonical, LexToString(Record.Index));
	AppendToken(Canonical, BoolToken(Record.bPersisted));
	TArray<FHyperAIUIFieldValue> Fields = Record.Fields;
	Fields.Sort([](const FHyperAIUIFieldValue& A, const FHyperAIUIFieldValue& B)
	{
		if (A.Id != B.Id) return A.Id < B.Id;
		return A.Type < B.Type;
	});
	for (const FHyperAIUIFieldValue& Field : Fields)
	{
		AppendToken(Canonical, FieldCanonical(Field));
		if (Canonical.Len() > FHyperAIStudioUIContracts::MaxCaptureCanonicalCharacters)
		{
			return FString();
		}
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

bool FHyperAIStudioUIValueContracts::ComputeFingerprints(
	FHyperAIStudioUIValueSnapshot& Snapshot,
	FString& OutError)
{
	using namespace HyperAIStudio::UI::ValueModel::Private;
	OutError.Reset();
	Snapshot.Records.Sort([](const FHyperAIUIRecord& A, const FHyperAIUIRecord& B)
	{
		if (A.RecordKey != B.RecordKey) return A.RecordKey < B.RecordKey;
		return A.Kind < B.Kind;
	});
	FString PersistedCanonical;
	FString VolatileCanonical;
	AppendToken(PersistedCanonical, TEXT("hyperai.ui.persisted-snapshot.v1"));
	AppendToken(VolatileCanonical, TEXT("hyperai.ui.volatile-observation.v1"));
	for (const FHyperAIUIRecord& Record : Snapshot.Records)
	{
		const FString Fingerprint = ComputeRecordFingerprint(Record);
		if (Fingerprint.IsEmpty())
		{
			OutError = TEXT("record_fingerprint_bound_exceeded");
			return false;
		}
		AppendToken(Record.bPersisted ? PersistedCanonical : VolatileCanonical, Fingerprint);
		if (PersistedCanonical.Len() > FHyperAIStudioUIContracts::MaxCaptureCanonicalCharacters
			|| VolatileCanonical.Len() > FHyperAIStudioUIContracts::MaxCaptureCanonicalCharacters)
		{
			OutError = TEXT("snapshot_fingerprint_bound_exceeded");
			return false;
		}
	}
	Snapshot.PersistedFingerprint =
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(PersistedCanonical);
	Snapshot.VolatileObservationFingerprint =
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(VolatileCanonical);
	if (Snapshot.PersistedFingerprint.IsEmpty()
		|| Snapshot.VolatileObservationFingerprint.IsEmpty())
	{
		OutError = TEXT("snapshot_fingerprint_unavailable");
		return false;
	}
	return true;
}

int32 FHyperAIStudioUIValueContracts::EstimateRecordBytes(const FHyperAIUIRecord& Record)
{
	// Conservative JSON/MCP envelope bound: one UTF-16 code unit can require a
	// six-byte escape, with headroom for property names and framing.
	int64 Size = 384ll + 12ll * (Record.Kind.Len() + Record.RecordKey.Len()
		+ Record.BlueprintPath.Len() + Record.StableId.Len() + Record.ParentStableId.Len()
		+ Record.Name.Len() + Record.ClassPath.Len());
	for (const FHyperAIUIFieldValue& Field : Record.Fields)
	{
		Size += 320ll + 12ll * (Field.Id.Len() + Field.Type.Len() + Field.StringValue.Len());
	}
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

int32 FHyperAIStudioUIValueContracts::EstimateIssueBytes(const FHyperAIUIIssue& Issue)
{
	const int64 Size = 256ll + 12ll * (Issue.Code.Len() + Issue.Severity.Len()
		+ Issue.BlueprintPath.Len() + Issue.RecordKey.Len() + Issue.Message.Len());
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

TArray<FHyperAIUIIssue> FHyperAIStudioUIValueValidator::Validate(
	const FHyperAIStudioUIValueSnapshot& Snapshot,
	const FHyperAIStudioUIValidationOptions& Options,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::UI::ValueModel::Private;
	TArray<FHyperAIUIIssue> Issues;
	bOutTruncated = false;
	const int32 Limit = FMath::Clamp(Options.MaxIssues, 1, FHyperAIStudioUIContracts::MaxIssues);
	auto Add = [&](FHyperAIUIIssue&& Issue)
	{
		if (Issues.Num() >= Limit)
		{
			bOutTruncated = true;
			return false;
		}
		Issues.Add(MoveTemp(Issue));
		return true;
	};
	for (const FHyperAIUIIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (!Add(FHyperAIUIIssue(CaptureIssue))) break;
	}
	if (!Snapshot.bComplete)
	{
		Add(MakeIssue(TEXT("snapshot_incomplete"), TEXT("error"), FString(), FString(),
			TEXT("The loaded-only projection did not close over every admitted bounded value.")));
	}

	TMap<FString, TSet<FString>> WidgetIdsByBlueprint;
	TMap<FString, TSet<FString>> WidgetNamesByBlueprint;
	TMap<FString, TSet<FString>> ProjectedWidgetNamesByBlueprint;
	TMap<FString, TMap<FString, FString>> ParentByBlueprint;
	TMap<FString, TSet<FString>> AnimationNamesByBlueprint;
	TMap<FString, TSet<FString>> BindingIdsByBlueprint;
	TMap<FString, TSet<FString>> ViewModelIdsByBlueprint;
	TMap<FString, TSet<FString>> SeenViewModelIdsByBlueprint;
	TMap<FString, int32> MVVMBindingCountsByBlueprint;
	TMap<FString, int32> MVVMViewModelCountsByBlueprint;
	TSet<FString> MVVMViewBlueprints;
	TSet<FString> RecordKeys;
	for (const FHyperAIUIRecord& Record : Snapshot.Records)
	{
		if (Record.Kind == TEXT("widget") && !Record.Name.IsEmpty())
		{
			ProjectedWidgetNamesByBlueprint.FindOrAdd(Record.BlueprintPath).Add(Record.Name);
		}
		else if (Record.Kind == TEXT("mvvm_viewmodel") && !Record.StableId.IsEmpty())
		{
			ViewModelIdsByBlueprint.FindOrAdd(Record.BlueprintPath).Add(Record.StableId);
			++MVVMViewModelCountsByBlueprint.FindOrAdd(Record.BlueprintPath);
		}
		else if (Record.Kind == TEXT("mvvm_binding"))
		{
			++MVVMBindingCountsByBlueprint.FindOrAdd(Record.BlueprintPath);
		}
	}
	for (const FHyperAIUIRecord& Record : Snapshot.Records)
	{
		if (bOutTruncated) break;
		if (Record.RecordKey.IsEmpty() || Record.BlueprintPath.IsEmpty()
			|| RecordKeys.Contains(Record.RecordKey)
			|| !FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Record.BlueprintPath)
			|| !IsKnownRecordKind(Record.Kind))
		{
			Add(MakeIssue(TEXT("record_identity_missing"), TEXT("error"),
				Record.BlueprintPath, Record.RecordKey,
				TEXT("A UI row has an unknown kind, noncanonical blueprint identity, empty key, or duplicate key.")));
		}
		RecordKeys.Add(Record.RecordKey);
		TSet<FString> FieldIds;
		for (const FHyperAIUIFieldValue& Field : Record.Fields)
		{
			if (Field.Id.IsEmpty() || FieldIds.Contains(Field.Id))
			{
				Add(MakeIssue(TEXT("field_identity_duplicate"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("A UI row contains an empty or duplicate closed field id.")));
				continue;
			}
			FieldIds.Add(Field.Id);
			if (!IsKnownFieldType(Field.Type))
			{
				Add(MakeIssue(TEXT("field_type_unknown"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("A projected field uses a type outside the closed value union.")));
			}
			else if (!CheckFiniteField(Field))
			{
				Add(MakeIssue(TEXT("non_finite_value"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("A projected layout/style scalar is not finite.")));
			}
		}
		if (Record.Kind == TEXT("widget"))
		{
			TSet<FString>& Ids = WidgetIdsByBlueprint.FindOrAdd(Record.BlueprintPath);
			TSet<FString>& Names = WidgetNamesByBlueprint.FindOrAdd(Record.BlueprintPath);
			if (Record.StableId.IsEmpty() || Ids.Contains(Record.StableId))
			{
				Add(MakeIssue(TEXT("widget_identity_duplicate"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("Widget stable ids must be non-empty and unique per blueprint.")));
			}
			if (Record.Name.IsEmpty() || Names.Contains(Record.Name))
			{
				Add(MakeIssue(TEXT("widget_name_duplicate"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("Widget names must be non-empty and unique per blueprint.")));
			}
			Ids.Add(Record.StableId);
			Names.Add(Record.Name);
			ParentByBlueprint.FindOrAdd(Record.BlueprintPath).Add(
				Record.StableId, Record.ParentStableId);
			if (Options.bCheckAccessibility && IsInteractiveClass(Record.ClassPath))
			{
				const FHyperAIUIFieldValue* Accessible = FindField(Record, TEXT("accessible_text"));
				if (!Accessible || Accessible->StringValue.TrimStartAndEnd().IsEmpty())
				{
					Add(MakeIssue(TEXT("interactive_accessible_text_missing"), TEXT("warning"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("An interactive widget has no persisted override accessible text.")));
				}
			}
			if (Options.bCheckLayout)
			{
				if (const FHyperAIUIFieldValue* AnchorsMin = FindField(Record, TEXT("anchors_min")))
				{
					if (AnchorsMin->Vector2Value.X < 0.0 || AnchorsMin->Vector2Value.X > 1.0
						|| AnchorsMin->Vector2Value.Y < 0.0 || AnchorsMin->Vector2Value.Y > 1.0)
					{
						Add(MakeIssue(TEXT("anchors_out_of_range"), TEXT("warning"),
							Record.BlueprintPath, Record.RecordKey,
							TEXT("Canvas minimum anchors are outside the normalized range.")));
					}
				}
			}
			const FHyperAIUIFieldValue* Visibility = FindField(Record, TEXT("visibility"));
			static const TSet<FString> VisibilityValues = {TEXT("visible"), TEXT("collapsed"),
				TEXT("hidden"), TEXT("hit_test_invisible"), TEXT("self_hit_test_invisible")};
			if (!Visibility || Visibility->Type != TEXT("enum")
				|| !VisibilityValues.Contains(Visibility->StringValue))
			{
				Add(MakeIssue(TEXT("widget_visibility_invalid"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Every projected widget requires one closed persisted visibility value.")));
			}
			if (Record.ClassPath.EndsWith(TEXT(".ProgressBar")))
			{
				const FHyperAIUIFieldValue* Percent = FindField(Record, TEXT("percent"));
				if (!Percent || Percent->Type != TEXT("number")
					|| Percent->NumberValue < 0.0 || Percent->NumberValue > 1.0)
				{
					Add(MakeIssue(TEXT("progress_percent_out_of_range"), TEXT("error"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("Progress percent must stay in the normalized range.")));
				}
			}
			else if (Record.ClassPath.EndsWith(TEXT(".Slider")))
			{
				const FHyperAIUIFieldValue* Value = FindField(Record, TEXT("value"));
				const FHyperAIUIFieldValue* Minimum = FindField(Record, TEXT("min_value"));
				const FHyperAIUIFieldValue* Maximum = FindField(Record, TEXT("max_value"));
				if (!Value || !Minimum || !Maximum || Value->Type != TEXT("number")
					|| Minimum->Type != TEXT("number") || Maximum->Type != TEXT("number")
					|| Minimum->NumberValue > Maximum->NumberValue
					|| Value->NumberValue < Minimum->NumberValue
					|| Value->NumberValue > Maximum->NumberValue)
				{
					Add(MakeIssue(TEXT("slider_range_invalid"), TEXT("error"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("Slider value must lie within its finite ordered minimum/maximum range.")));
				}
			}
		}
		else if (Record.Kind == TEXT("animation") && Options.bCheckAnimations)
		{
			TSet<FString>& Names = AnimationNamesByBlueprint.FindOrAdd(Record.BlueprintPath);
			if (!IsNameToken(Record.Name) || Names.Contains(Record.Name))
			{
				Add(MakeIssue(TEXT("animation_name_invalid_or_duplicate"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Animation names must be safe and unique per blueprint.")));
			}
			Names.Add(Record.Name);
			const FHyperAIUIFieldValue* Start = FindField(Record, TEXT("start_frame"));
			const FHyperAIUIFieldValue* End = FindField(Record, TEXT("end_frame"));
			if (!Start || !End || Start->Type != TEXT("int") || End->Type != TEXT("int")
				|| Start->IntValue >= End->IntValue)
			{
				Add(MakeIssue(TEXT("animation_range_invalid"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Animation playback ranges require start_frame < end_frame.")));
			}
		}
		else if (Record.Kind == TEXT("slot") && Options.bCheckLayout)
		{
			const FHyperAIUIFieldValue* AnchorsMin = FindField(Record, TEXT("anchors_min"));
			const FHyperAIUIFieldValue* AnchorsMax = FindField(Record, TEXT("anchors_max"));
			if (AnchorsMin && AnchorsMax
				&& (AnchorsMin->Vector2Value.X > AnchorsMax->Vector2Value.X
					|| AnchorsMin->Vector2Value.Y > AnchorsMax->Vector2Value.Y))
			{
				Add(MakeIssue(TEXT("canvas_anchor_order_invalid"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Canvas minimum anchors must not exceed maximum anchors.")));
			}
			if (AnchorsMin
				&& (AnchorsMin->Vector2Value.X < 0.0 || AnchorsMin->Vector2Value.X > 1.0
					|| AnchorsMin->Vector2Value.Y < 0.0 || AnchorsMin->Vector2Value.Y > 1.0))
			{
				Add(MakeIssue(TEXT("anchors_out_of_range"), TEXT("warning"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Canvas minimum anchors are outside the normalized range.")));
			}
			const FHyperAIUIFieldValue* RowSpan = FindField(Record, TEXT("row_span"));
			const FHyperAIUIFieldValue* ColumnSpan = FindField(Record, TEXT("column_span"));
			if ((RowSpan && RowSpan->IntValue < 1) || (ColumnSpan && ColumnSpan->IntValue < 1))
			{
				Add(MakeIssue(TEXT("grid_span_invalid"), TEXT("error"), Record.BlueprintPath,
					Record.RecordKey, TEXT("Grid row/column spans must be at least one.")));
			}
		}
		else if (Record.Kind == TEXT("animation_binding") && Options.bCheckAnimations)
		{
			const FHyperAIUIFieldValue* WidgetName = FindField(Record, TEXT("widget_name"));
			if (!WidgetName || !ProjectedWidgetNamesByBlueprint.FindOrAdd(Record.BlueprintPath).Contains(
				WidgetName->StringValue))
			{
				Add(MakeIssue(TEXT("animation_binding_widget_missing"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Animation binding target does not resolve to a projected widget.")));
			}
		}
		else if ((Record.Kind == TEXT("legacy_binding") && Options.bCheckBindings)
			|| (Record.Kind == TEXT("mvvm_binding") && Options.bCheckMVVM))
		{
			TSet<FString>& Ids = BindingIdsByBlueprint.FindOrAdd(Record.BlueprintPath);
			if (Record.StableId.IsEmpty() || Ids.Contains(Record.StableId))
			{
				Add(MakeIssue(TEXT("binding_identity_duplicate"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("Binding stable ids must be non-empty and unique per blueprint.")));
			}
			Ids.Add(Record.StableId);
			if (Record.Kind == TEXT("legacy_binding"))
			{
				const FHyperAIUIFieldValue* Target = FindField(Record, TEXT("target_widget"));
				const FHyperAIUIFieldValue* Property = FindField(Record, TEXT("target_property"));
				const FHyperAIUIFieldValue* Function = FindField(Record, TEXT("source_function"));
				if (!Target || !Property || !Function || !IsNameToken(Property->StringValue)
					|| !IsNameToken(Function->StringValue)
					|| !ProjectedWidgetNamesByBlueprint.FindOrAdd(Record.BlueprintPath).Contains(Target->StringValue))
				{
					Add(MakeIssue(TEXT("legacy_binding_endpoint_invalid"), TEXT("error"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("Legacy binding requires a projected widget, property, and source function.")));
				}
			}
			else
			{
				const FHyperAIUIFieldValue* SourceEndpoint = FindField(Record, TEXT("source_endpoint"));
				const FHyperAIUIFieldValue* SourcePath = FindField(Record, TEXT("source_path"));
				const FHyperAIUIFieldValue* DestinationEndpoint = FindField(Record, TEXT("destination_endpoint"));
				const FHyperAIUIFieldValue* DestinationPath = FindField(Record, TEXT("destination_path"));
				const TSet<FString>& WidgetNames =
					ProjectedWidgetNamesByBlueprint.FindOrAdd(Record.BlueprintPath);
				const TSet<FString>& ViewModelIds =
					ViewModelIdsByBlueprint.FindOrAdd(Record.BlueprintPath);
				if (!SourceEndpoint || !SourcePath || !DestinationEndpoint || !DestinationPath
					|| !IsResolvedMVVMEndpoint(SourceEndpoint->StringValue, WidgetNames, ViewModelIds)
					|| !IsClosedFieldPath(SourcePath->StringValue)
					|| !IsResolvedMVVMEndpoint(DestinationEndpoint->StringValue, WidgetNames, ViewModelIds)
					|| !IsClosedFieldPath(DestinationPath->StringValue))
				{
					Add(MakeIssue(TEXT("mvvm_binding_endpoint_invalid"), TEXT("error"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("MVVM endpoints must resolve in the detached snapshot and paths must satisfy the closed segment grammar.")));
				}
			}
		}
		else if (Record.Kind == TEXT("mvvm_view") && Options.bCheckMVVM)
		{
			const FHyperAIUIFieldValue* ViewPresent = FindField(Record, TEXT("view_present"));
			const FHyperAIUIFieldValue* SettingsPresent = FindField(Record, TEXT("settings_present"));
			const FHyperAIUIFieldValue* ViewModelCount = FindField(Record, TEXT("viewmodel_count"));
			const FHyperAIUIFieldValue* BindingCount = FindField(Record, TEXT("binding_count"));
			const FHyperAIUIFieldValue* EventCount = FindField(Record, TEXT("event_count"));
			const FHyperAIUIFieldValue* ConditionCount = FindField(Record, TEXT("condition_count"));
			const FHyperAIUIFieldValue* ViewClass = FindField(Record, TEXT("view_class"));
			const bool bDuplicate = MVVMViewBlueprints.Contains(Record.BlueprintPath);
			MVVMViewBlueprints.Add(Record.BlueprintPath);
			if (bDuplicate || Record.StableId != TEXT("existing_extension")
				|| !ViewPresent || ViewPresent->Type != TEXT("bool") || !ViewPresent->bBoolValue
				|| !SettingsPresent || SettingsPresent->Type != TEXT("bool")
				|| !SettingsPresent->bBoolValue
				|| !ViewModelCount || ViewModelCount->Type != TEXT("int")
				|| ViewModelCount->IntValue
					!= MVVMViewModelCountsByBlueprint.FindRef(Record.BlueprintPath)
				|| !BindingCount || BindingCount->Type != TEXT("int")
				|| BindingCount->IntValue
					!= MVVMBindingCountsByBlueprint.FindRef(Record.BlueprintPath)
				|| !EventCount || EventCount->Type != TEXT("int") || EventCount->IntValue != 0
				|| !ConditionCount || ConditionCount->Type != TEXT("int")
				|| ConditionCount->IntValue != 0
				|| !ViewClass || ViewClass->Type != TEXT("path")
				|| ViewClass->StringValue.IsEmpty())
			{
				Add(MakeIssue(TEXT("mvvm_view_inventory_invalid"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("The existing MVVM extension/view inventory must be unique, present, and count-exact.")));
			}
			for (const TCHAR* SettingId : {TEXT("initialize_sources_on_construct"),
				TEXT("initialize_bindings_on_construct"),
				TEXT("initialize_events_on_construct"),
				TEXT("create_view_without_bindings")})
			{
				const FHyperAIUIFieldValue* Setting = FindField(Record, SettingId);
				if (!Setting || Setting->Type != TEXT("bool"))
				{
					Add(MakeIssue(TEXT("mvvm_view_setting_missing"), TEXT("error"),
						Record.BlueprintPath, Record.RecordKey,
						TEXT("The existing MVVM view settings projection is incomplete.")));
				}
			}
		}
		else if (Record.Kind == TEXT("mvvm_viewmodel") && Options.bCheckMVVM)
		{
			TSet<FString>& SeenIds = SeenViewModelIdsByBlueprint.FindOrAdd(Record.BlueprintPath);
			if (Record.StableId.IsEmpty() || SeenIds.Contains(Record.StableId))
			{
				Add(MakeIssue(TEXT("mvvm_viewmodel_identity_duplicate"), TEXT("error"),
					Record.BlueprintPath, Record.RecordKey,
					TEXT("MVVM view-model context ids must be non-empty and unique.")));
			}
			SeenIds.Add(Record.StableId);
		}
	}
	if (Options.bCheckMVVM)
	{
		TSet<FString> MVVMContentBlueprints;
		for (const TPair<FString, int32>& Pair : MVVMBindingCountsByBlueprint)
		{
			if (Pair.Value > 0) MVVMContentBlueprints.Add(Pair.Key);
		}
		for (const TPair<FString, int32>& Pair : MVVMViewModelCountsByBlueprint)
		{
			if (Pair.Value > 0) MVVMContentBlueprints.Add(Pair.Key);
		}
		for (const FString& BlueprintPath : MVVMContentBlueprints)
		{
			if (!MVVMViewBlueprints.Contains(BlueprintPath))
			{
				Add(MakeIssue(TEXT("mvvm_view_inventory_missing"), TEXT("error"),
					BlueprintPath, FString(),
					TEXT("MVVM bindings or view-model contexts require one projected existing MVVM view.")));
			}
		}
	}

	for (const TPair<FString, TMap<FString, FString>>& BlueprintParents : ParentByBlueprint)
	{
		if (bOutTruncated) break;
		const TSet<FString>& WidgetIds = WidgetIdsByBlueprint.FindChecked(BlueprintParents.Key);
		int32 RootCount = 0;
		for (const TPair<FString, FString>& Pair : BlueprintParents.Value)
		{
			if (bOutTruncated) break;
			if (Pair.Value.IsEmpty())
			{
				++RootCount;
				continue;
			}
			if (!WidgetIds.Contains(Pair.Value))
			{
				Add(MakeIssue(TEXT("widget_parent_missing"), TEXT("error"), BlueprintParents.Key,
					Pair.Key, TEXT("Widget parent id is absent from the closed tree.")));
				continue;
			}
			TSet<FString> Visited;
			FString Cursor = Pair.Key;
			for (int32 Step = 0; Step <= BlueprintParents.Value.Num(); ++Step)
			{
				if (Visited.Contains(Cursor))
				{
					Add(MakeIssue(TEXT("widget_parent_cycle"), TEXT("error"), BlueprintParents.Key,
						Pair.Key, TEXT("Widget parent links contain a cycle.")));
					break;
				}
				Visited.Add(Cursor);
				const FString* Parent = BlueprintParents.Value.Find(Cursor);
				if (!Parent || Parent->IsEmpty()) break;
				Cursor = *Parent;
			}
		}
		if (!BlueprintParents.Value.IsEmpty() && RootCount != 1)
		{
			Add(MakeIssue(TEXT("widget_tree_root_count_invalid"), TEXT("error"),
				BlueprintParents.Key, FString(),
				TEXT("A closed WidgetTree projection requires exactly one root widget.")));
		}
	}
	return Issues;
}

bool FHyperAIStudioUIValueContracts::ReplayShadowPlan(
	const FHyperAIStudioUIValueSnapshot& Base,
	const TArray<FHyperAIStudioUIBackendOperation>& Operations,
	FHyperAIStudioUIValueSnapshot& OutDesired,
	FHyperAIUIPlanEffects& OutEffects,
	TArray<FHyperAIUIIssue>& OutIssues,
	FString& OutError)
{
	using namespace HyperAIStudio::UI::ValueModel::Private;
	OutDesired = Base;
	OutDesired.CaptureIssues.Reset();
	OutEffects = {};
	OutEffects.OperationCount = Operations.Num();
	OutEffects.bWouldSaveOnce = true;
	OutEffects.bWouldCompileOnce = true;
	OutEffects.bWouldValidateOnce = true;
	OutEffects.bWouldVerifyFreshOnce = true;
	OutIssues.Reset();
	OutError.Reset();
	const FString BlueprintPath = BlueprintPathFromSnapshot(Base);
	if (BlueprintPath.IsEmpty())
	{
		OutError = TEXT("blueprint_record_missing");
		return false;
	}
	for (const FHyperAIStudioUIBackendOperation& Operation : Operations)
	{
		switch (Operation.Kind)
		{
		case EHyperAIStudioUIOperationKind::AnimationCreate:
		{
			if (FindRecord(OutDesired.Records, TEXT("animation"), Operation.Name) != INDEX_NONE)
			{
				OutError = TEXT("animation_already_exists");
				return false;
			}
			FHyperAIUIRecord Record;
			Record.Kind = TEXT("animation");
			Record.BlueprintPath = BlueprintPath;
			Record.StableId = Operation.Name;
			Record.Name = Operation.Name;
			Record.RecordKey = TEXT("animation:") + BlueprintPath + TEXT(":") + Operation.Name;
			FHyperAIUIFieldValue& Start = Record.Fields.AddDefaulted_GetRef();
			Start.Id = TEXT("start_frame"); Start.Type = TEXT("int"); Start.IntValue = Operation.StartFrame;
			FHyperAIUIFieldValue& End = Record.Fields.AddDefaulted_GetRef();
			End.Id = TEXT("end_frame"); End.Type = TEXT("int"); End.IntValue = Operation.EndFrame;
			OutDesired.Records.Add(MoveTemp(Record));
			++OutEffects.AnimationCreates;
			break;
		}
		case EHyperAIStudioUIOperationKind::AnimationRename:
		case EHyperAIStudioUIOperationKind::AnimationDelete:
		{
			const int32 Index = FindRecord(OutDesired.Records, TEXT("animation"), Operation.SubjectId);
			if (Index == INDEX_NONE)
			{
				OutError = TEXT("animation_not_found");
				return false;
			}
			FHyperAIUIRecord& Record = OutDesired.Records[Index];
			if (ComputeRecordFingerprint(Record) != Operation.ExpectedElementFingerprint)
			{
				OutError = TEXT("stale_animation_element");
				return false;
			}
			const FString OldStableId = Record.StableId;
			if (Operation.Kind == EHyperAIStudioUIOperationKind::AnimationDelete)
			{
				OutDesired.Records.RemoveAt(Index);
				OutDesired.Records.RemoveAll([&](const FHyperAIUIRecord& Candidate)
				{
					return Candidate.Kind == TEXT("animation_binding")
						&& Candidate.ParentStableId == OldStableId;
				});
				++OutEffects.AnimationDeletes;
			}
			else
			{
				if (FindRecord(OutDesired.Records, TEXT("animation"), Operation.Name) != INDEX_NONE)
				{
					OutError = TEXT("animation_rename_collision");
					return false;
				}
				Record.Name = Operation.Name;
				Record.StableId = Operation.Name;
				Record.RecordKey = TEXT("animation:") + BlueprintPath + TEXT(":") + Operation.Name;
				for (FHyperAIUIRecord& Candidate : OutDesired.Records)
				{
					if (Candidate.Kind == TEXT("animation_binding")
						&& Candidate.ParentStableId == OldStableId)
					{
						Candidate.ParentStableId = Operation.Name;
						Candidate.RecordKey = TEXT("animation_binding:") + BlueprintPath
							+ TEXT(":") + Operation.Name + TEXT(":") + Candidate.StableId;
					}
				}
				++OutEffects.AnimationEdits;
			}
			break;
		}
		case EHyperAIStudioUIOperationKind::WidgetSetProperty:
		case EHyperAIStudioUIOperationKind::WidgetSetStyle:
		{
			const int32 Index = FindRecord(OutDesired.Records, TEXT("widget"), Operation.SubjectId);
			if (Index == INDEX_NONE)
			{
				OutError = TEXT("widget_not_found");
				return false;
			}
			FHyperAIUIRecord& Record = OutDesired.Records[Index];
			if (ComputeRecordFingerprint(Record) != Operation.ExpectedElementFingerprint)
			{
				OutError = TEXT("stale_widget_element");
				return false;
			}
			const bool bStyle = Operation.Kind == EHyperAIStudioUIOperationKind::WidgetSetStyle;
			if (!ClassSupportsProperty(Record.ClassPath, Operation.Name, bStyle))
			{
				OutError = TEXT("unsupported_widget_class_property_pair");
				return false;
			}
			if (Operation.bHasBoolValue) SetBoolField(Record, Operation.Name, Operation.bBoolValue);
			else if (Operation.bHasNumberValue) SetNumberField(Record, Operation.Name, Operation.NumberValue);
			else if (Operation.bHasColorValue)
			{
				SetColorField(Record, Operation.Name, Operation.ColorValue);
				if (Operation.Name == TEXT("color_and_opacity")
					&& Record.ClassPath.EndsWith(TEXT(".TextBlock")))
				{
					SetStringField(Record, TEXT("color_and_opacity_mode"),
						TEXT("enum"), TEXT("specified"));
					Record.Fields.RemoveAll([](const FHyperAIUIFieldValue& Field)
					{
						return Field.Id == TEXT("color_and_opacity_table_id");
					});
				}
			}
			else SetStringField(Record, Operation.Name,
				Operation.Name == TEXT("visibility") || Operation.Name == TEXT("checked_state")
					? TEXT("enum") : TEXT("string"), Operation.StringValue);
			if (bStyle) ++OutEffects.StyleEdits; else ++OutEffects.PropertyEdits;
			break;
		}
		case EHyperAIStudioUIOperationKind::BindingCreateLegacy:
		case EHyperAIStudioUIOperationKind::BindingCreateMVVM:
		case EHyperAIStudioUIOperationKind::BindingUpdateLegacy:
		case EHyperAIStudioUIOperationKind::BindingUpdateMVVM:
		case EHyperAIStudioUIOperationKind::BindingReplaceLegacy:
		case EHyperAIStudioUIOperationKind::BindingReplaceMVVM:
		case EHyperAIStudioUIOperationKind::BindingDeleteLegacy:
		case EHyperAIStudioUIOperationKind::BindingDeleteMVVM:
		{
			const FString Kind = BindingRecordKind(Operation.Kind);
			const bool bMVVM = Kind == TEXT("mvvm_binding");
			const bool bCreate = Operation.Kind == EHyperAIStudioUIOperationKind::BindingCreateLegacy
				|| Operation.Kind == EHyperAIStudioUIOperationKind::BindingCreateMVVM;
			const bool bDelete = Operation.Kind == EHyperAIStudioUIOperationKind::BindingDeleteLegacy
				|| Operation.Kind == EHyperAIStudioUIOperationKind::BindingDeleteMVVM;
			const bool bReplace = Operation.Kind == EHyperAIStudioUIOperationKind::BindingReplaceLegacy
				|| Operation.Kind == EHyperAIStudioUIOperationKind::BindingReplaceMVVM;
			FHyperAIUIRecord* MVVMView = bMVVM
				? OutDesired.Records.FindByPredicate([](const FHyperAIUIRecord& Candidate)
				{
					return Candidate.Kind == TEXT("mvvm_view");
				})
				: nullptr;
			if (bMVVM && !MVVMView)
			{
				OutError = TEXT("existing_mvvm_view_required");
				return false;
			}
			FHyperAIUIFieldValue* BindingCount = MVVMView
				? FindField(*MVVMView, TEXT("binding_count")) : nullptr;
			if (bMVVM && (!BindingCount || BindingCount->Type != TEXT("int")
				|| BindingCount->IntValue < 0
				|| BindingCount->IntValue > FHyperAIStudioUIContracts::MaxMVVMBindings))
			{
				OutError = TEXT("mvvm_binding_count_invalid");
				return false;
			}
			const int32 Index = FindRecord(OutDesired.Records, Kind, Operation.SubjectId);
			if (bCreate)
			{
				if (Index != INDEX_NONE)
				{
					OutError = TEXT("binding_already_exists");
					return false;
				}
				if (BindingCount
					&& BindingCount->IntValue == FHyperAIStudioUIContracts::MaxMVVMBindings)
				{
					OutError = TEXT("mvvm_binding_bound_exceeded");
					return false;
				}
				FHyperAIUIRecord Record;
				PopulateBindingRecord(Record, Operation, BlueprintPath, Kind);
				if (BindingCount) SetIntField(*MVVMView, TEXT("binding_count"),
					BindingCount->IntValue + 1);
				OutDesired.Records.Add(MoveTemp(Record));
				++OutEffects.BindingCreates;
				break;
			}
			if (Index == INDEX_NONE)
			{
				OutError = TEXT("binding_not_found");
				return false;
			}
			if (ComputeRecordFingerprint(OutDesired.Records[Index]) != Operation.ExpectedElementFingerprint)
			{
				OutError = TEXT("stale_binding_element");
				return false;
			}
			if (bDelete)
			{
				if (BindingCount && BindingCount->IntValue == 0)
				{
					OutError = TEXT("mvvm_binding_count_underflow");
					return false;
				}
				if (BindingCount) SetIntField(*MVVMView, TEXT("binding_count"),
					BindingCount->IntValue - 1);
				OutDesired.Records.RemoveAt(Index);
				++OutEffects.BindingDeletesOrReplaces;
			}
			else
			{
				PopulateBindingRecord(OutDesired.Records[Index], Operation, BlueprintPath, Kind);
				if (bReplace) ++OutEffects.BindingDeletesOrReplaces;
				else ++OutEffects.BindingEdits;
			}
			break;
		}
		default:
			OutError = TEXT("operation_kind_unhandled");
			return false;
		}
	}
	if (!ComputeFingerprints(OutDesired, OutError)) return false;
	FHyperAIStudioUIValidationOptions Validation;
	Validation.MaxIssues = FHyperAIStudioUIContracts::MaxIssues;
	bool bTruncated = false;
	OutIssues = FHyperAIStudioUIValueValidator::Validate(OutDesired, Validation, bTruncated);
	if (bTruncated)
	{
		OutError = TEXT("shadow_validation_issue_bound_exceeded");
		return false;
	}
	return true;
}
