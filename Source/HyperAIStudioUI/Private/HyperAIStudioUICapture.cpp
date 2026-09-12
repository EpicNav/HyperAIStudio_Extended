// Games by Hyper 2026.

#include "HyperAIStudioUIValueModel.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Text.h"
#include "IO/IoHash.h"
#include "Misc/AssetRegistryInterface.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "Styling/StyleColors.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintExtension.h"

namespace HyperAIStudio::UI::Capture::Private
{
	constexpr int32 MaxFieldsPerRecord = 1024;

	struct FCaptureContext
	{
		FHyperAIStudioUIValueSnapshot& Snapshot;
		double DeadlineSeconds = 0.0;
		FString BlueprintPath;
		IAssetRegistry* AssetRegistry = nullptr;
		bool bDeadlineReported = false;

		bool CheckDeadline()
		{
			if (FPlatformTime::Seconds() <= DeadlineSeconds) return true;
			if (!bDeadlineReported)
			{
				bDeadlineReported = true;
				MarkIncomplete(TEXT("deadline_exceeded"), TEXT("error"), FString(),
					TEXT("Loaded-only UI projection exceeded the caller's monotonic deadline."));
			}
			return false;
		}

		void MarkIncomplete(
			const FString& Code,
			const FString& Severity,
			const FString& RecordKey,
			const FString& Message)
		{
			Snapshot.bComplete = false;
			if (Snapshot.CaptureIssues.Num() >= FHyperAIStudioUIContracts::MaxIssues) return;
			FHyperAIUIIssue& Issue = Snapshot.CaptureIssues.AddDefaulted_GetRef();
			Issue.Code = Code.Left(96);
			Issue.Severity = Severity;
			Issue.BlueprintPath = BlueprintPath.Left(FHyperAIStudioUIContracts::MaxPathCharacters);
			Issue.RecordKey = RecordKey.Left(512);
			Issue.Message = Message.Left(1024);
		}

		bool AddRecord(FHyperAIUIRecord&& Record)
		{
			if (Snapshot.Records.Num() >= FHyperAIStudioUIContracts::MaxRecords)
			{
				MarkIncomplete(TEXT("record_bound_exceeded"), TEXT("error"), Record.RecordKey,
					TEXT("The closed record inventory exceeded its hard cap."));
				return false;
			}
			Snapshot.Records.Add(MoveTemp(Record));
			return true;
		}
	};

	struct FPackageEvidence
	{
		FString Existence = TEXT("unknown");
		FString SavedHash;
		int64 DiskSize = -1;
	};

	FPackageEvidence CapturePackageEvidence(
		const FName PackageName,
		FCaptureContext& Context,
		const FString& RecordKey)
	{
		FPackageEvidence Evidence;
		if (!Context.AssetRegistry)
		{
			Context.Snapshot.bPackageEvidenceComplete = false;
			Context.MarkIncomplete(TEXT("asset_registry_unavailable"), TEXT("error"), RecordKey,
				TEXT("Asset Registry package evidence is unavailable; absence is not inferred."));
			return Evidence;
		}
		FAssetPackageData PackageData;
		const UE::AssetRegistry::EExists State = Context.AssetRegistry->TryGetAssetPackageData(
			PackageName, PackageData, true);
		Evidence.Existence = FHyperAIStudioUIContracts::ClassifyAssetRegistryExistence(
			static_cast<int32>(State));
		if (State == UE::AssetRegistry::EExists::Unknown)
		{
			Context.Snapshot.bPackageEvidenceComplete = false;
			Context.MarkIncomplete(TEXT("asset_registry_lock_unavailable"), TEXT("error"), RecordKey,
				TEXT("Non-blocking package evidence returned Unknown because registry state or its read lock was unavailable."));
			return Evidence;
		}
		if (State == UE::AssetRegistry::EExists::DoesNotExist)
		{
			Context.Snapshot.bPackageEvidenceComplete = false;
			Context.MarkIncomplete(TEXT("package_not_on_disk"), TEXT("error"), RecordKey,
				TEXT("Non-blocking package evidence reports no saved package on disk."));
			return Evidence;
		}
#if WITH_EDITORONLY_DATA
		const FIoHash SavedHash = PackageData.GetPackageSavedHash();
		if (!SavedHash.IsZero()) Evidence.SavedHash = LexToString(SavedHash);
#endif
		Evidence.DiskSize = PackageData.DiskSize;
		if (Evidence.SavedHash.IsEmpty() || Evidence.DiskSize <= 0)
		{
			Context.Snapshot.bPackageEvidenceComplete = false;
			Context.MarkIncomplete(TEXT("package_saved_identity_incomplete"), TEXT("error"), RecordKey,
				TEXT("Existing package evidence lacks a nonzero saved hash or positive disk size."));
		}
		return Evidence;
	}

	bool IsBoundedString(const FString& Value, const int32 Limit)
	{
		return Value.Len() <= Limit;
	}

	bool IsBoundedName(const FName& Value, const int32 Limit)
	{
		return Value.GetStringLength() <= static_cast<uint32>(Limit);
	}

	bool TryBoundedTopLevelObjectPath(const UObject* Object, FString& OutValue)
	{
		OutValue.Reset();
		if (!Object) return false;
		const UPackage* Package = Object->GetOutermost();
		const FName ObjectName = Object->GetFName();
		const FName PackageName = Package ? Package->GetFName() : NAME_None;
		const uint32 ObjectLength = ObjectName.GetStringLength();
		const uint32 PackageLength = PackageName.GetStringLength();
		if (!Package || ObjectName.IsNone() || PackageName.IsNone()
			|| ObjectLength > static_cast<uint32>(FHyperAIStudioUIContracts::MaxNameCharacters)
			|| PackageLength > static_cast<uint32>(FHyperAIStudioUIContracts::MaxPathCharacters)
			|| PackageLength + 1u + ObjectLength
				> static_cast<uint32>(FHyperAIStudioUIContracts::MaxPathCharacters))
		{
			return false;
		}
		OutValue = Object->GetPathName();
		return IsBoundedString(OutValue, FHyperAIStudioUIContracts::MaxPathCharacters);
	}

	FString BoundedTopLevelObjectPath(
		const UObject* Object,
		FCaptureContext& Context,
		const FString& RecordKey,
		const FString& Label)
	{
		if (!Object)
		{
			Context.MarkIncomplete(TEXT("object_path_missing"), TEXT("error"), RecordKey,
				Label + TEXT(" object is missing."));
			return FString();
		}
		FString Value;
		if (!TryBoundedTopLevelObjectPath(Object, Value))
		{
			Context.MarkIncomplete(TEXT("object_path_pre_copy_bound_exceeded"), TEXT("error"),
				RecordKey, Label + TEXT(" package/object identity exceeded its pre-copy cap."));
			return FString();
		}
		return Value;
	}

	bool CopyTextSource(
		const FText& Text,
		FString& OutValue,
		FCaptureContext& Context,
		const FString& RecordKey,
		const FString& FieldId)
	{
		const FString* Source = FTextInspector::GetSourceString(Text);
		if (!Source)
		{
			OutValue.Reset();
			return true;
		}
		if (Source->Len() > FHyperAIStudioUIContracts::MaxTextCharacters)
		{
			Context.MarkIncomplete(TEXT("text_pre_copy_bound_exceeded"), TEXT("error"), RecordKey,
				TEXT("Persisted text for field '") + FieldId + TEXT("' exceeded the pre-copy cap."));
			return false;
		}
		OutValue = *Source;
		return true;
	}

	bool AddField(FHyperAIUIRecord& Record, FHyperAIUIFieldValue&& Field, FCaptureContext& Context)
	{
		if (Field.Id.IsEmpty() || Field.Id.Len() > 256
			|| Field.Type.IsEmpty() || Field.Type.Len() > 16
			|| Field.StringValue.Len() > FHyperAIStudioUIContracts::MaxTextCharacters
			|| Record.Fields.Num() >= MaxFieldsPerRecord)
		{
			Context.MarkIncomplete(TEXT("field_bound_exceeded"), TEXT("error"), Record.RecordKey,
				TEXT("A projected UI field failed its pre-copy identity/value bound."));
			return false;
		}
		Record.Fields.Add(MoveTemp(Field));
		return true;
	}

	void AddString(
		FHyperAIUIRecord& Record,
		const FString& Id,
		const FString& Type,
		const FString& Value,
		FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = Type; Field.StringValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddBool(FHyperAIUIRecord& Record, const FString& Id, const bool Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("bool"); Field.bBoolValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddInt(FHyperAIUIRecord& Record, const FString& Id, const int32 Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("int"); Field.IntValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddNumber(FHyperAIUIRecord& Record, const FString& Id, const double Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("number"); Field.NumberValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddColor(FHyperAIUIRecord& Record, const FString& Id, const FLinearColor& Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("color"); Field.ColorValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddVector(FHyperAIUIRecord& Record, const FString& Id, const FVector2D& Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("vector2"); Field.Vector2Value = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	void AddMargin(FHyperAIUIRecord& Record, const FString& Id, const FMargin& Value, FCaptureContext& Context)
	{
		FHyperAIUIFieldValue Field;
		Field.Id = Id; Field.Type = TEXT("margin"); Field.MarginValue = Value;
		AddField(Record, MoveTemp(Field), Context);
	}

	FString ClassPath(const UObject* Object, FCaptureContext& Context, const FString& RecordKey)
	{
		if (!Object || !Object->GetClass()) return FString();
		return BoundedTopLevelObjectPath(Object->GetClass(), Context, RecordKey,
			TEXT("Projected class"));
	}

	FString WidgetStableId(const UWidgetBlueprint* Blueprint, const UWidget* Widget)
	{
		if (!Widget) return FString();
		if (!IsBoundedName(Widget->GetFName(), FHyperAIStudioUIContracts::MaxNameCharacters))
		{
			return FString();
		}
#if WITH_EDITORONLY_DATA
		if (Blueprint)
		{
			if (const FGuid* Guid = Blueprint->WidgetVariableNameToGuidMap.Find(Widget->GetFName()))
			{
				if (Guid->IsValid()) return TEXT("guid:") + Guid->ToString(EGuidFormats::Digits);
			}
		}
#endif
		return TEXT("name:") + Widget->GetName();
	}

	FString SlateVisibilityToken(const ESlateVisibility Value)
	{
		switch (Value)
		{
		case ESlateVisibility::Visible: return TEXT("visible");
		case ESlateVisibility::Collapsed: return TEXT("collapsed");
		case ESlateVisibility::Hidden: return TEXT("hidden");
		case ESlateVisibility::HitTestInvisible: return TEXT("hit_test_invisible");
		case ESlateVisibility::SelfHitTestInvisible: return TEXT("self_hit_test_invisible");
		default: return TEXT("unknown");
		}
	}

	FString CheckStateToken(const ECheckBoxState Value)
	{
		switch (Value)
		{
		case ECheckBoxState::Unchecked: return TEXT("unchecked");
		case ECheckBoxState::Checked: return TEXT("checked");
		case ECheckBoxState::Undetermined: return TEXT("undetermined");
		default: return TEXT("unknown");
		}
	}

	void CaptureSlateColor(
		const FSlateColor& Color,
		FHyperAIUIRecord& Record,
		const FString& Id,
		FCaptureContext& Context)
	{
		if (Color == FSlateColor::UseForeground())
		{
			AddString(Record, Id + TEXT("_mode"), TEXT("enum"), TEXT("foreground"), Context);
			return;
		}
		if (Color == FSlateColor::UseSubduedForeground())
		{
			AddString(Record, Id + TEXT("_mode"), TEXT("enum"), TEXT("subdued_foreground"), Context);
			return;
		}
		if (Color == FSlateColor::UseStyle())
		{
			AddString(Record, Id + TEXT("_mode"), TEXT("enum"), TEXT("style"), Context);
			return;
		}
		for (int32 Index = 0; Index < static_cast<int32>(EStyleColor::MAX); ++Index)
		{
			if (Color == FSlateColor(static_cast<EStyleColor>(Index)))
			{
				AddString(Record, Id + TEXT("_mode"), TEXT("enum"), TEXT("color_table"), Context);
				AddInt(Record, Id + TEXT("_table_id"), Index, Context);
				return;
			}
		}
		AddString(Record, Id + TEXT("_mode"), TEXT("enum"), TEXT("specified"), Context);
		AddColor(Record, Id, Color.GetSpecifiedColor(), Context);
	}

	void CaptureWidgetFields(
		const UWidget* Widget,
		FHyperAIUIRecord& Record,
		FCaptureContext& Context)
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		AddString(Record, TEXT("visibility"), TEXT("enum"),
			SlateVisibilityToken(Widget->Visibility), Context);
		AddBool(Record, TEXT("is_enabled"), Widget->bIsEnabled, Context);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		AddBool(Record, TEXT("is_variable"), Widget->bIsVariable, Context);
#if WITH_EDITORONLY_DATA
		AddBool(Record, TEXT("override_accessible_defaults"),
			Widget->bOverrideAccessibleDefaults, Context);
		if (Widget->bOverrideAccessibleDefaults)
		{
			FString AccessibleText;
			if (CopyTextSource(Widget->AccessibleText, AccessibleText, Context,
				Record.RecordKey, TEXT("accessible_text")))
			{
				AddString(Record, TEXT("accessible_text"), TEXT("string"), AccessibleText, Context);
			}
			AddInt(Record, TEXT("accessible_behavior"),
				static_cast<int32>(Widget->AccessibleBehavior), Context);
			AddBool(Record, TEXT("children_accessible"),
				Widget->bCanChildrenBeAccessible, Context);
		}
#endif
		if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			FString Value;
			if (CopyTextSource(Text->Text, Value, Context, Record.RecordKey, TEXT("text")))
			{
				AddString(Record, TEXT("text"), TEXT("string"), Value, Context);
			}
			CaptureSlateColor(Text->ColorAndOpacity, Record, TEXT("color_and_opacity"), Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const UImage* Image = Cast<UImage>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddColor(Record, TEXT("color_and_opacity"), Image->ColorAndOpacity, Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const UBorder* Border = Cast<UBorder>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddColor(Record, TEXT("brush_color"), Border->BrushColor, Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const UProgressBar* Progress = Cast<UProgressBar>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddNumber(Record, TEXT("percent"), Progress->Percent, Context);
			AddColor(Record, TEXT("fill_color_and_opacity"),
				Progress->FillColorAndOpacity, Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const USlider* Slider = Cast<USlider>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddNumber(Record, TEXT("value"), Slider->Value, Context);
			AddNumber(Record, TEXT("min_value"), Slider->MinValue, Context);
			AddNumber(Record, TEXT("max_value"), Slider->MaxValue, Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const UCheckBox* CheckBox = Cast<UCheckBox>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddString(Record, TEXT("checked_state"), TEXT("enum"),
				CheckStateToken(CheckBox->CheckedState), Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		else if (const UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(Widget))
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			AddInt(Record, TEXT("active_widget_index"), Switcher->ActiveWidgetIndex, Context);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
	}

	void CaptureSlotFields(
		const UPanelSlot* Slot,
		FHyperAIUIRecord& Record,
		FCaptureContext& Context)
	{
		if (const UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
		{
			const FAnchorData Layout = Canvas->GetLayout();
			AddVector(Record, TEXT("anchors_min"), Layout.Anchors.Minimum, Context);
			AddVector(Record, TEXT("anchors_max"), Layout.Anchors.Maximum, Context);
			AddMargin(Record, TEXT("offsets"), Layout.Offsets, Context);
			AddVector(Record, TEXT("alignment"), Layout.Alignment, Context);
			AddBool(Record, TEXT("auto_size"), Canvas->GetAutoSize(), Context);
			AddInt(Record, TEXT("z_order"), Canvas->GetZOrder(), Context);
		}
		else if (const UHorizontalBoxSlot* Horizontal = Cast<UHorizontalBoxSlot>(Slot))
		{
			AddMargin(Record, TEXT("padding"), Horizontal->GetPadding(), Context);
			AddInt(Record, TEXT("horizontal_alignment"), Horizontal->GetHorizontalAlignment(), Context);
			AddInt(Record, TEXT("vertical_alignment"), Horizontal->GetVerticalAlignment(), Context);
		}
		else if (const UVerticalBoxSlot* Vertical = Cast<UVerticalBoxSlot>(Slot))
		{
			AddMargin(Record, TEXT("padding"), Vertical->GetPadding(), Context);
			AddInt(Record, TEXT("horizontal_alignment"), Vertical->GetHorizontalAlignment(), Context);
			AddInt(Record, TEXT("vertical_alignment"), Vertical->GetVerticalAlignment(), Context);
		}
		else if (const UOverlaySlot* Overlay = Cast<UOverlaySlot>(Slot))
		{
			AddMargin(Record, TEXT("padding"), Overlay->GetPadding(), Context);
			AddInt(Record, TEXT("horizontal_alignment"), Overlay->GetHorizontalAlignment(), Context);
			AddInt(Record, TEXT("vertical_alignment"), Overlay->GetVerticalAlignment(), Context);
		}
		else if (const UGridSlot* Grid = Cast<UGridSlot>(Slot))
		{
			AddMargin(Record, TEXT("padding"), Grid->GetPadding(), Context);
			AddInt(Record, TEXT("row"), Grid->GetRow(), Context);
			AddInt(Record, TEXT("column"), Grid->GetColumn(), Context);
			AddInt(Record, TEXT("row_span"), Grid->GetRowSpan(), Context);
			AddInt(Record, TEXT("column_span"), Grid->GetColumnSpan(), Context);
			AddInt(Record, TEXT("layer"), Grid->GetLayer(), Context);
			AddVector(Record, TEXT("nudge"), Grid->GetNudge(), Context);
			AddInt(Record, TEXT("horizontal_alignment"), Grid->GetHorizontalAlignment(), Context);
			AddInt(Record, TEXT("vertical_alignment"), Grid->GetVerticalAlignment(), Context);
		}
		else
		{
			AddString(Record, TEXT("layout_projection"), TEXT("enum"),
				TEXT("unsupported_slot_class"), Context);
			Context.MarkIncomplete(TEXT("unsupported_slot_class"), TEXT("warning"),
				Record.RecordKey,
				TEXT("This slot class has no closed typed layout projection in UI pack v1."));
		}
	}

	struct FPendingWidget
	{
		UWidget* Widget = nullptr;
		FString ParentStableId;
		int32 Index = -1;
		FString NamedSlotName;
	};

	bool CaptureTree(
		const UWidgetBlueprint* Blueprint,
		const bool bIncludeLayout,
		FCaptureContext& Context)
	{
		const UWidgetTree* Tree = Blueprint ? Blueprint->WidgetTree.Get() : nullptr;
		if (!Tree)
		{
			Context.MarkIncomplete(TEXT("widget_tree_missing"), TEXT("error"), FString(),
				TEXT("The loaded Widget Blueprint has no WidgetTree."));
			return false;
		}
		TArray<FPendingWidget> Queue;
		Queue.Reserve(FHyperAIStudioUIContracts::MaxWidgetsPerBlueprint);
		if (Tree->RootWidget)
		{
			Queue.Add({Tree->RootWidget.Get(), FString(), 0, FString()});
		}
		if (Tree->NamedSlotBindings.Num() > FHyperAIStudioUIContracts::MaxNamedSlotBindings)
		{
			Context.MarkIncomplete(TEXT("named_slot_binding_bound_exceeded"), TEXT("error"), FString(),
				TEXT("NamedSlotBindings exceeded the hard pre-iteration cap."));
			return false;
		}
		const FString RootStableId = WidgetStableId(Blueprint, Tree->RootWidget.Get());
		for (const TPair<FName, TObjectPtr<UWidget>>& Pair : Tree->NamedSlotBindings)
		{
			if (!Context.CheckDeadline()) return false;
			if (!Pair.Value) continue;
			if (!IsBoundedName(Pair.Key, FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("named_slot_name_bound_exceeded"), TEXT("error"), FString(),
					TEXT("A named-slot identifier exceeded its pre-copy cap."));
				continue;
			}
			const FString SlotName = Pair.Key.ToString();
			Queue.Add({Pair.Value.Get(), RootStableId, Queue.Num(), SlotName});
		}
		TSet<const UWidget*> Seen;
		for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
		{
			if (!Context.CheckDeadline()) return false;
			if (Queue.Num() > FHyperAIStudioUIContracts::MaxWidgetsPerBlueprint)
			{
				Context.MarkIncomplete(TEXT("widget_bound_exceeded"), TEXT("error"), FString(),
					TEXT("WidgetTree traversal exceeded its hard queue cap."));
				return false;
			}
			const FPendingWidget& Pending = Queue[QueueIndex];
			UWidget* Widget = Pending.Widget;
			if (!Widget)
			{
				Context.MarkIncomplete(TEXT("null_widget"), TEXT("error"), FString(),
					TEXT("WidgetTree contains a null child."));
				continue;
			}
			if (Seen.Contains(Widget))
			{
				Context.MarkIncomplete(TEXT("widget_cycle_or_alias"), TEXT("error"), Widget->GetName(),
					TEXT("A widget pointer appeared more than once in the closed tree."));
				continue;
			}
			Seen.Add(Widget);
			if (!IsBoundedName(Widget->GetFName(), FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("widget_name_bound_exceeded"), TEXT("error"), FString(),
					TEXT("A widget name exceeded its pre-copy cap."));
				continue;
			}
			FHyperAIUIRecord WidgetRecord;
			WidgetRecord.Kind = TEXT("widget");
			WidgetRecord.BlueprintPath = Context.BlueprintPath;
			WidgetRecord.StableId = WidgetStableId(Blueprint, Widget);
			WidgetRecord.ParentStableId = Pending.ParentStableId;
			WidgetRecord.Name = Widget->GetName();
			WidgetRecord.Index = Pending.Index;
			WidgetRecord.RecordKey = TEXT("widget:") + Context.BlueprintPath
				+ TEXT(":") + WidgetRecord.StableId;
			WidgetRecord.ClassPath = ClassPath(Widget, Context, WidgetRecord.RecordKey);
			if (!Pending.NamedSlotName.IsEmpty())
			{
				AddString(WidgetRecord, TEXT("named_slot_name"), TEXT("string"),
					Pending.NamedSlotName, Context);
			}
			CaptureWidgetFields(Widget, WidgetRecord, Context);
			if (!Context.AddRecord(MoveTemp(WidgetRecord))) return false;

			if (bIncludeLayout && Widget->Slot)
			{
				const UPanelSlot* Slot = Widget->Slot.Get();
				FHyperAIUIRecord SlotRecord;
				SlotRecord.Kind = TEXT("slot");
				SlotRecord.BlueprintPath = Context.BlueprintPath;
				SlotRecord.StableId = WidgetStableId(Blueprint, Widget);
				SlotRecord.ParentStableId = Pending.ParentStableId;
				SlotRecord.Name = Widget->GetName();
				SlotRecord.Index = Pending.Index;
				SlotRecord.RecordKey = TEXT("slot:") + Context.BlueprintPath
					+ TEXT(":") + SlotRecord.StableId;
				SlotRecord.ClassPath = ClassPath(Slot, Context, SlotRecord.RecordKey);
				AddBool(SlotRecord, TEXT("content_matches_widget"), Slot->Content.Get() == Widget, Context);
				if (Slot->Parent)
				{
					if (IsBoundedName(Slot->Parent->GetFName(),
						FHyperAIStudioUIContracts::MaxNameCharacters))
					{
						AddString(SlotRecord, TEXT("parent_widget_name"), TEXT("string"),
							Slot->Parent->GetName(), Context);
					}
					else
					{
						Context.MarkIncomplete(TEXT("slot_parent_name_bound_exceeded"),
							TEXT("error"), SlotRecord.RecordKey,
							TEXT("Slot parent name exceeded its pre-copy cap."));
					}
				}
				CaptureSlotFields(Slot, SlotRecord, Context);
				if (!Context.AddRecord(MoveTemp(SlotRecord))) return false;
			}

			if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				const int32 ChildCount = Panel->GetChildrenCount();
				if (ChildCount < 0 || ChildCount > FHyperAIStudioUIContracts::MaxChildrenPerPanel
					|| Queue.Num() > FHyperAIStudioUIContracts::MaxWidgetsPerBlueprint - ChildCount)
				{
					Context.MarkIncomplete(TEXT("panel_child_bound_exceeded"), TEXT("error"),
						Widget->GetName(), TEXT("A panel child count failed its pre-iteration cap."));
					continue;
				}
				const FString ParentStableId = WidgetStableId(Blueprint, Widget);
				for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
				{
					if (!Context.CheckDeadline()) return false;
					Queue.Add({Panel->GetChildAt(ChildIndex), ParentStableId, ChildIndex, FString()});
				}
			}
		}
		return Context.Snapshot.bComplete;
	}

	void CaptureAnimation(
		const UWidgetAnimation* Animation,
		const int32 AnimationIndex,
		FCaptureContext& Context)
	{
		if (!Animation)
		{
			Context.MarkIncomplete(TEXT("null_animation"), TEXT("error"), FString(),
				TEXT("Animations contains a null entry."));
			return;
		}
		if (!IsBoundedName(Animation->GetFName(), FHyperAIStudioUIContracts::MaxNameCharacters))
		{
			Context.MarkIncomplete(TEXT("animation_pre_copy_bound_exceeded"), TEXT("error"), FString(),
				TEXT("Animation object name exceeded its pre-copy cap."));
			return;
		}
		const FString Name = Animation->GetName();
		const FString& DisplayLabelSource = Animation->GetDisplayLabel();
		if (Name.Len() > FHyperAIStudioUIContracts::MaxNameCharacters
			|| DisplayLabelSource.Len() > FHyperAIStudioUIContracts::MaxTextCharacters)
		{
			Context.MarkIncomplete(TEXT("animation_pre_copy_bound_exceeded"), TEXT("error"), Name,
				TEXT("Animation name or label exceeded its pre-copy cap."));
			return;
		}
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("animation");
		Record.BlueprintPath = Context.BlueprintPath;
		Record.StableId = Name;
		Record.Name = Name;
		Record.Index = AnimationIndex;
		Record.RecordKey = TEXT("animation:") + Context.BlueprintPath + TEXT(":") + Name;
		Record.ClassPath = ClassPath(Animation, Context, Record.RecordKey);
		AddString(Record, TEXT("display_label"), TEXT("string"), DisplayLabelSource, Context);
		const UMovieScene* MovieScene = Animation->GetMovieScene();
		if (!MovieScene)
		{
			Context.MarkIncomplete(TEXT("animation_movie_scene_missing"), TEXT("error"),
				Record.RecordKey, TEXT("A WidgetAnimation has no MovieScene."));
			Context.AddRecord(MoveTemp(Record));
			return;
		}
		const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
		if (!Playback.HasLowerBound() || !Playback.HasUpperBound())
		{
			Context.MarkIncomplete(TEXT("animation_open_playback_range"), TEXT("error"),
				Record.RecordKey, TEXT("Open animation playback ranges are unsupported in v1."));
		}
		else
		{
			AddInt(Record, TEXT("start_frame"), Playback.GetLowerBoundValue().Value, Context);
			AddInt(Record, TEXT("end_frame"), Playback.GetUpperBoundValue().Value, Context);
		}
		const FFrameRate Tick = MovieScene->GetTickResolution();
		const FFrameRate Display = MovieScene->GetDisplayRate();
		AddInt(Record, TEXT("tick_numerator"), Tick.Numerator, Context);
		AddInt(Record, TEXT("tick_denominator"), Tick.Denominator, Context);
		AddInt(Record, TEXT("display_numerator"), Display.Numerator, Context);
		AddInt(Record, TEXT("display_denominator"), Display.Denominator, Context);
		const TArray<FMovieSceneBinding>& SceneBindings = MovieScene->GetBindings();
		if (SceneBindings.Num() > FHyperAIStudioUIContracts::MaxMovieSceneBindings)
		{
			Context.MarkIncomplete(TEXT("movie_scene_binding_bound_exceeded"), TEXT("error"),
				Record.RecordKey, TEXT("MovieScene object bindings exceeded the hard pre-iteration cap."));
			Context.AddRecord(MoveTemp(Record));
			return;
		}
		const TArray<UMovieSceneTrack*>& RootTracks = MovieScene->GetTracks();
		if (RootTracks.Num() > FHyperAIStudioUIContracts::MaxTracksPerBinding)
		{
			Context.MarkIncomplete(TEXT("root_track_bound_exceeded"), TEXT("error"),
				Record.RecordKey, TEXT("Root MovieScene tracks exceeded the hard pre-iteration cap."));
		}
		if (!RootTracks.IsEmpty())
		{
			Context.MarkIncomplete(TEXT("animation_track_content_delegated"), TEXT("warning"),
				Record.RecordKey,
				TEXT("Arbitrary MovieScene track/channel content remains delegated; no unsafe generic projection ran."));
		}
		AddInt(Record, TEXT("movie_scene_binding_count"), SceneBindings.Num(), Context);
		AddInt(Record, TEXT("root_track_count"), RootTracks.Num(), Context);
		Context.AddRecord(MoveTemp(Record));

		const TArray<FWidgetAnimationBinding>& WidgetBindings = Animation->GetBindings();
		if (WidgetBindings.Num() > FHyperAIStudioUIContracts::MaxAnimationBindings)
		{
			Context.MarkIncomplete(TEXT("widget_animation_binding_bound_exceeded"), TEXT("error"),
				Name, TEXT("WidgetAnimation bindings exceeded the hard pre-iteration cap."));
			return;
		}
		for (int32 Index = 0; Index < WidgetBindings.Num(); ++Index)
		{
			if (!Context.CheckDeadline()) return;
			const FWidgetAnimationBinding& Binding = WidgetBindings[Index];
			if (!IsBoundedName(Binding.WidgetName, FHyperAIStudioUIContracts::MaxNameCharacters)
				|| !IsBoundedName(Binding.SlotWidgetName,
					FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("animation_binding_name_bound_exceeded"), TEXT("error"),
					Name, TEXT("Animation widget/slot name exceeded its pre-copy cap."));
				continue;
			}
			const FString WidgetName = Binding.WidgetName.ToString();
			const FString SlotWidgetName = Binding.SlotWidgetName.ToString();
			FHyperAIUIRecord BindingRecord;
			BindingRecord.Kind = TEXT("animation_binding");
			BindingRecord.BlueprintPath = Context.BlueprintPath;
			BindingRecord.ParentStableId = Name;
			BindingRecord.StableId = Binding.AnimationGuid.IsValid()
				? Binding.AnimationGuid.ToString(EGuidFormats::Digits)
				: FString::Printf(TEXT("index:%d"), Index);
			BindingRecord.Name = WidgetName;
			BindingRecord.Index = Index;
			BindingRecord.RecordKey = TEXT("animation_binding:") + Context.BlueprintPath
				+ TEXT(":") + Name + TEXT(":") + BindingRecord.StableId;
			AddString(BindingRecord, TEXT("widget_name"), TEXT("string"),
				WidgetName, Context);
			AddString(BindingRecord, TEXT("slot_widget_name"), TEXT("string"),
				SlotWidgetName, Context);
			AddString(BindingRecord, TEXT("animation_guid"), TEXT("guid"),
				Binding.AnimationGuid.ToString(EGuidFormats::Digits), Context);
			AddBool(BindingRecord, TEXT("is_root_widget"), Binding.bIsRootWidget, Context);
			const FMovieSceneBinding* SceneBinding = SceneBindings.FindByPredicate(
				[&](const FMovieSceneBinding& Candidate)
				{
					return Candidate.GetObjectGuid() == Binding.AnimationGuid;
				});
			if (SceneBinding)
			{
				const TArray<UMovieSceneTrack*>& Tracks = SceneBinding->GetTracks();
				if (Tracks.Num() > FHyperAIStudioUIContracts::MaxTracksPerBinding)
				{
					Context.MarkIncomplete(TEXT("binding_track_bound_exceeded"), TEXT("error"),
						BindingRecord.RecordKey,
						TEXT("Animation binding tracks exceeded the hard pre-iteration cap."));
				}
				else
				{
					AddInt(BindingRecord, TEXT("track_count"), Tracks.Num(), Context);
					for (int32 TrackIndex = 0; TrackIndex < Tracks.Num(); ++TrackIndex)
					{
						if (!Context.CheckDeadline()) return;
						const UMovieSceneTrack* Track = Tracks[TrackIndex];
						AddString(BindingRecord,
							FString::Printf(TEXT("track_class_%03d"), TrackIndex), TEXT("path"),
							Track ? ClassPath(Track, Context, BindingRecord.RecordKey) : FString(), Context);
					}
					if (!Tracks.IsEmpty())
					{
						Context.MarkIncomplete(TEXT("animation_track_content_delegated"), TEXT("warning"),
							BindingRecord.RecordKey,
							TEXT("Track sections/channels remain delegated; no generic property serialization ran."));
					}
				}
			}
			else
			{
				AddInt(BindingRecord, TEXT("track_count"), 0, Context);
			}
			Context.AddRecord(MoveTemp(BindingRecord));
		}
	}

	FString RawMVVMPath(
		const FMVVMBlueprintPropertyPath& Path,
		FCaptureContext& Context,
		const FString& RecordKey,
		const FString& Label)
	{
		const TArrayView<const FMVVMBlueprintFieldPath> Fields = Path.GetFieldPaths();
		if (Fields.Num() > FHyperAIStudioUIContracts::MaxMVVMPathSegments)
		{
			Context.MarkIncomplete(TEXT("mvvm_path_bound_exceeded"), TEXT("error"), RecordKey,
				Label + TEXT(" path exceeded the hard pre-iteration segment cap."));
			return FString();
		}
		FString Result;
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			const FName RawName = Fields[Index].GetRawFieldName();
			if (!IsBoundedName(RawName, FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("mvvm_path_pre_copy_bound_exceeded"), TEXT("error"),
					RecordKey, Label + TEXT(" path segment exceeded its pre-copy cap."));
				return FString();
			}
			const FString Segment = RawName.ToString();
			if (Result.Len() + Segment.Len() + 1 > FHyperAIStudioUIContracts::MaxTextCharacters)
			{
				Context.MarkIncomplete(TEXT("mvvm_path_pre_copy_bound_exceeded"), TEXT("error"),
					RecordKey, Label + TEXT(" path exceeded its pre-copy string cap."));
				return FString();
			}
			if (!Result.IsEmpty()) Result.AppendChar(TEXT('.'));
			Result += Segment;
		}
		return Result;
	}

	FString MVVMEndpoint(
		const FMVVMBlueprintPropertyPath& Path,
		FCaptureContext& Context,
		const FString& RecordKey,
		const FString& Label)
	{
		const FName WidgetName = Path.GetWidgetName();
		if (!WidgetName.IsNone())
		{
			if (!IsBoundedName(WidgetName, FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("mvvm_endpoint_name_bound_exceeded"), TEXT("error"),
					RecordKey, Label + TEXT(" widget endpoint exceeded its pre-copy cap."));
				return TEXT("widget_name_bound_exceeded");
			}
			return TEXT("widget:") + WidgetName.ToString();
		}
		if (Path.GetViewModelId().IsValid())
		{
			return TEXT("viewmodel:") + Path.GetViewModelId().ToString(EGuidFormats::Digits);
		}
		// Calling GetSource would run a deprecation migration on old data. Preserve the
		// no-mutation read boundary and report the ambiguity instead.
		Context.MarkIncomplete(TEXT("mvvm_self_or_none_source_ambiguous"), TEXT("warning"), RecordKey,
			Label + TEXT(" endpoint cannot be distinguished without a mutating deprecation update."));
		return TEXT("self_or_none_unresolved");
	}

	void CaptureMVVM(const UWidgetBlueprint* Blueprint, FCaptureContext& Context)
	{
		UMVVMWidgetBlueprintExtension_View* Extension =
			UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(Blueprint);
		if (!Extension)
		{
			return; // Absence is an exact already-existing-extension observation.
		}
		FHyperAIUIRecord ViewRecord;
		ViewRecord.Kind = TEXT("mvvm_view");
		ViewRecord.BlueprintPath = Context.BlueprintPath;
		ViewRecord.StableId = TEXT("existing_extension");
		ViewRecord.Name = TEXT("existing_extension");
		ViewRecord.RecordKey = TEXT("mvvm_view:") + Context.BlueprintPath;
		ViewRecord.ClassPath = ClassPath(Extension, Context, ViewRecord.RecordKey);
		UMVVMBlueprintView* View = Extension->GetBlueprintView();
		AddBool(ViewRecord, TEXT("view_present"), View != nullptr, Context);
		if (!View)
		{
			Context.MarkIncomplete(TEXT("mvvm_extension_view_missing"), TEXT("error"), FString(),
				TEXT("An existing MVVM WidgetBlueprint extension has no existing BlueprintView."));
			Context.AddRecord(MoveTemp(ViewRecord));
			return;
		}
		const TArrayView<const FMVVMBlueprintViewModelContext> ViewModels = View->GetViewModels();
		const TArrayView<const FMVVMBlueprintViewBinding> Bindings = View->GetBindings();
		const TArrayView<const TObjectPtr<UMVVMBlueprintViewEvent>> Events = View->GetEvents();
		const TArrayView<const TObjectPtr<UMVVMBlueprintViewCondition>> Conditions =
			View->GetConditions();
		AddString(ViewRecord, TEXT("view_class"), TEXT("path"),
			ClassPath(View, Context, ViewRecord.RecordKey), Context);
		AddInt(ViewRecord, TEXT("viewmodel_count"), ViewModels.Num(), Context);
		AddInt(ViewRecord, TEXT("binding_count"), Bindings.Num(), Context);
		AddInt(ViewRecord, TEXT("event_count"), Events.Num(), Context);
		AddInt(ViewRecord, TEXT("condition_count"), Conditions.Num(), Context);
		const UMVVMBlueprintViewSettings* Settings = View->GetSettings();
		AddBool(ViewRecord, TEXT("settings_present"), Settings != nullptr, Context);
		if (Settings)
		{
			AddBool(ViewRecord, TEXT("initialize_sources_on_construct"),
				Settings->bInitializeSourcesOnConstruct, Context);
			AddBool(ViewRecord, TEXT("initialize_bindings_on_construct"),
				Settings->bInitializeBindingsOnConstruct, Context);
			AddBool(ViewRecord, TEXT("initialize_events_on_construct"),
				Settings->bInitializeEventsOnConstruct, Context);
			AddBool(ViewRecord, TEXT("create_view_without_bindings"),
				Settings->bCreateViewWithoutBindings, Context);
		}
		else
		{
			Context.MarkIncomplete(TEXT("mvvm_view_settings_missing"), TEXT("error"),
				ViewRecord.RecordKey,
				TEXT("The existing MVVM BlueprintView has no persisted settings object."));
		}
		Context.AddRecord(MoveTemp(ViewRecord));
		if (!Events.IsEmpty() || !Conditions.IsEmpty())
		{
			Context.MarkIncomplete(TEXT("mvvm_event_condition_projection_delegated"),
				TEXT("warning"), FString(),
				TEXT("Existing MVVM events/conditions are outside the closed v1 binding projection."));
		}
		if (ViewModels.Num() > FHyperAIStudioUIContracts::MaxMVVMViewModels)
		{
			Context.MarkIncomplete(TEXT("mvvm_viewmodel_bound_exceeded"), TEXT("error"), FString(),
				TEXT("Existing MVVM contexts exceeded the hard pre-iteration cap."));
			return;
		}
		for (int32 Index = 0; Index < ViewModels.Num(); ++Index)
		{
			if (!Context.CheckDeadline()) return;
			const FMVVMBlueprintViewModelContext& ViewModel = ViewModels[Index];
			if (!ViewModel.GetViewModelId().IsValid())
			{
				Context.MarkIncomplete(TEXT("mvvm_viewmodel_id_invalid"), TEXT("error"), FString(),
					TEXT("An existing MVVM view-model context has no valid stable guid."));
			}
			FHyperAIUIRecord Record;
			Record.Kind = TEXT("mvvm_viewmodel");
			Record.BlueprintPath = Context.BlueprintPath;
			Record.StableId = ViewModel.GetViewModelId().ToString(EGuidFormats::Digits);
			Record.Index = Index;
			Record.RecordKey = TEXT("mvvm_viewmodel:") + Context.BlueprintPath
				+ TEXT(":") + Record.StableId;
			const FName ViewModelName = ViewModel.GetViewModelName();
			if (!IsBoundedName(ViewModelName, FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("mvvm_viewmodel_name_bound_exceeded"), TEXT("error"),
					Record.RecordKey, TEXT("MVVM view-model name exceeded its pre-copy cap."));
			}
			else
			{
				Record.Name = ViewModelName.ToString();
			}
			if (const UClass* Class = ViewModel.GetViewModelClass())
			{
				Record.ClassPath = BoundedTopLevelObjectPath(Class, Context, Record.RecordKey,
					TEXT("MVVM view-model class"));
			}
			AddInt(Record, TEXT("creation_type"), static_cast<int32>(ViewModel.CreationType), Context);
			AddBool(Record, TEXT("optional"), ViewModel.bOptional, Context);
			AddBool(Record, TEXT("create_getter"), ViewModel.bCreateGetterFunction, Context);
			AddBool(Record, TEXT("create_setter"), ViewModel.bCreateSetterFunction, Context);
			AddBool(Record, TEXT("expose_instance_in_editor"),
				ViewModel.bExposeInstanceInEditor, Context);
			AddBool(Record, TEXT("global_collection_update"),
				ViewModel.bGlobalViewModelCollectionUpdate, Context);
			AddBool(Record, TEXT("override_force_execute_on_set_source"),
				ViewModel.bOverrideForceExecuteBindingsOnSetSource, Context);
			AddBool(Record, TEXT("force_execute_on_set_source"),
				ViewModel.bForceExecuteBindingsOnSetSource, Context);
			AddBool(Record, TEXT("can_rename"), ViewModel.bCanRename, Context);
			AddBool(Record, TEXT("can_edit"), ViewModel.bCanEdit, Context);
			AddBool(Record, TEXT("can_remove"), ViewModel.bCanRemove, Context);
			AddBool(Record, TEXT("use_as_interface"), ViewModel.bUseAsInterface, Context);
			if (ViewModel.Resolver || ViewModel.InstancedViewModel)
			{
				Context.MarkIncomplete(TEXT("mvvm_context_object_projection_delegated"),
					TEXT("warning"), Record.RecordKey,
					TEXT("Resolver/instanced view-model object content is outside the closed v1 projection."));
			}
			if (IsBoundedName(ViewModel.GlobalViewModelIdentifier,
				FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				AddString(Record, TEXT("global_identifier"), TEXT("string"),
					ViewModel.GlobalViewModelIdentifier.ToString(), Context);
			}
			else
			{
				Context.MarkIncomplete(TEXT("mvvm_global_identifier_bound_exceeded"), TEXT("error"),
					Record.RecordKey, TEXT("MVVM global identifier exceeded its pre-copy cap."));
			}
			if (ViewModel.ViewModelPropertyPath.Len() <= FHyperAIStudioUIContracts::MaxTextCharacters)
			{
				AddString(Record, TEXT("viewmodel_property_path"), TEXT("string"),
					ViewModel.ViewModelPropertyPath, Context);
			}
			else
			{
				Context.MarkIncomplete(TEXT("mvvm_context_path_bound_exceeded"), TEXT("error"),
					Record.RecordKey, TEXT("MVVM context property path exceeded its pre-copy cap."));
			}
			Context.AddRecord(MoveTemp(Record));
		}

		if (Bindings.Num() > FHyperAIStudioUIContracts::MaxMVVMBindings)
		{
			Context.MarkIncomplete(TEXT("mvvm_binding_bound_exceeded"), TEXT("error"), FString(),
				TEXT("Existing MVVM bindings exceeded the hard pre-iteration cap."));
			return;
		}
		for (int32 Index = 0; Index < Bindings.Num(); ++Index)
		{
			if (!Context.CheckDeadline()) return;
			const FMVVMBlueprintViewBinding& Binding = Bindings[Index];
			if (!Binding.BindingId.IsValid())
			{
				Context.MarkIncomplete(TEXT("mvvm_binding_id_invalid"), TEXT("error"), FString(),
					TEXT("An existing MVVM binding has no valid stable guid."));
			}
			FHyperAIUIRecord Record;
			Record.Kind = TEXT("mvvm_binding");
			Record.BlueprintPath = Context.BlueprintPath;
			Record.StableId = Binding.BindingId.ToString(EGuidFormats::Digits);
			Record.Name = Record.StableId;
			Record.Index = Index;
			Record.RecordKey = TEXT("mvvm_binding:") + Context.BlueprintPath
				+ TEXT(":") + Record.StableId;
			AddString(Record, TEXT("source_endpoint"), TEXT("string"),
				MVVMEndpoint(Binding.SourcePath, Context, Record.RecordKey, TEXT("source")), Context);
			AddString(Record, TEXT("source_path"), TEXT("string"),
				RawMVVMPath(Binding.SourcePath, Context, Record.RecordKey, TEXT("source")), Context);
			AddString(Record, TEXT("destination_endpoint"), TEXT("string"),
				MVVMEndpoint(Binding.DestinationPath, Context, Record.RecordKey, TEXT("destination")), Context);
			AddString(Record, TEXT("destination_path"), TEXT("string"),
				RawMVVMPath(Binding.DestinationPath, Context, Record.RecordKey, TEXT("destination")), Context);
			AddInt(Record, TEXT("binding_mode"), static_cast<int32>(Binding.BindingType), Context);
			AddBool(Record, TEXT("enabled"), Binding.bEnabled, Context);
			AddBool(Record, TEXT("compile"), Binding.bCompile, Context);
			AddBool(Record, TEXT("override_execution_mode"),
				Binding.bOverrideExecutionMode, Context);
			if (Binding.bOverrideExecutionMode)
			{
				AddInt(Record, TEXT("execution_mode"),
					static_cast<int32>(Binding.OverrideExecutionMode), Context);
			}
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			const bool bHasDeprecatedConversionIdentity =
				!Binding.Conversion.DestinationToSourceWrapper_DEPRECATED.IsNone()
				|| !Binding.Conversion.SourceToDestinationWrapper_DEPRECATED.IsNone()
				|| !Binding.Conversion.DestinationToSourceFunction_DEPRECATED.GetMemberName().IsNone()
				|| !Binding.Conversion.SourceToDestinationFunction_DEPRECATED.GetMemberName().IsNone();
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			if (Binding.Conversion.GetConversionFunction(true)
				|| Binding.Conversion.GetConversionFunction(false)
				|| bHasDeprecatedConversionIdentity)
			{
				Context.MarkIncomplete(TEXT("mvvm_conversion_projection_delegated"),
					TEXT("warning"), Record.RecordKey,
					TEXT("MVVM conversion graphs/functions remain delegated in UI pack v1."));
			}
			Context.AddRecord(MoveTemp(Record));
		}
	}

	void CaptureLegacyBindings(const UWidgetBlueprint* Blueprint, FCaptureContext& Context)
	{
#if WITH_EDITORONLY_DATA
		if (Blueprint->Bindings.Num() > FHyperAIStudioUIContracts::MaxLegacyBindings)
		{
			Context.MarkIncomplete(TEXT("legacy_binding_bound_exceeded"), TEXT("error"), FString(),
				TEXT("Legacy editor bindings exceeded the hard pre-iteration cap."));
			return;
		}
		for (int32 Index = 0; Index < Blueprint->Bindings.Num(); ++Index)
		{
			if (!Context.CheckDeadline()) return;
			const FDelegateEditorBinding& Binding = Blueprint->Bindings[Index];
			if (Binding.ObjectName.Len() > FHyperAIStudioUIContracts::MaxNameCharacters
				|| !IsBoundedName(Binding.PropertyName, FHyperAIStudioUIContracts::MaxNameCharacters)
				|| !IsBoundedName(Binding.FunctionName, FHyperAIStudioUIContracts::MaxNameCharacters)
				|| !IsBoundedName(Binding.SourceProperty, FHyperAIStudioUIContracts::MaxNameCharacters))
			{
				Context.MarkIncomplete(TEXT("legacy_binding_name_bound_exceeded"), TEXT("error"),
					FString(), TEXT("Legacy binding target name exceeded its pre-copy cap."));
				continue;
			}
			FHyperAIUIRecord Record;
			Record.Kind = TEXT("legacy_binding");
			Record.BlueprintPath = Context.BlueprintPath;
			Record.StableId = Binding.MemberGuid.IsValid()
				? Binding.MemberGuid.ToString(EGuidFormats::Digits)
				: FString::Printf(TEXT("%s:%s:%d"), *Binding.ObjectName,
					*Binding.PropertyName.ToString(), Index);
			Record.Name = Record.StableId;
			Record.Index = Index;
			Record.RecordKey = TEXT("legacy_binding:") + Context.BlueprintPath
				+ TEXT(":") + Record.StableId;
			AddString(Record, TEXT("target_widget"), TEXT("string"), Binding.ObjectName, Context);
			AddString(Record, TEXT("target_property"), TEXT("string"),
				Binding.PropertyName.ToString(), Context);
			AddString(Record, TEXT("source_function"), TEXT("string"),
				Binding.FunctionName.ToString(), Context);
			AddString(Record, TEXT("source_property"), TEXT("string"),
				Binding.SourceProperty.ToString(), Context);
			AddString(Record, TEXT("member_guid"), TEXT("guid"),
				Binding.MemberGuid.ToString(EGuidFormats::Digits), Context);
			AddInt(Record, TEXT("binding_kind"), static_cast<int32>(Binding.Kind), Context);
			if (Binding.SourcePath.Segments.Num() > FHyperAIStudioUIContracts::MaxMVVMPathSegments)
			{
				Context.MarkIncomplete(TEXT("legacy_source_path_bound_exceeded"), TEXT("error"),
					Record.RecordKey, TEXT("Legacy binding source path exceeded its segment cap."));
			}
			else
			{
				for (int32 SegmentIndex = 0;
					SegmentIndex < Binding.SourcePath.Segments.Num(); ++SegmentIndex)
				{
					const FEditorPropertyPathSegment& Segment = Binding.SourcePath.Segments[SegmentIndex];
					const FName MemberName = Segment.GetMemberName();
					if (!IsBoundedName(MemberName, FHyperAIStudioUIContracts::MaxNameCharacters))
					{
						Context.MarkIncomplete(TEXT("legacy_source_segment_bound_exceeded"), TEXT("error"),
							Record.RecordKey, TEXT("Legacy source segment exceeded its pre-copy cap."));
						continue;
					}
					const FString Value = MemberName.ToString() + TEXT("#")
						+ Segment.GetMemberGuid().ToString(EGuidFormats::Digits);
					AddString(Record, FString::Printf(TEXT("source_segment_%03d"), SegmentIndex),
						TEXT("string"), Value, Context);
				}
			}
			Context.AddRecord(MoveTemp(Record));
		}
#endif
	}

	void CaptureBlueprint(
		const UWidgetBlueprint* Blueprint,
		const bool bIncludeTree,
		const bool bIncludeLayout,
		const bool bIncludeAnimations,
		const bool bIncludeLegacyBindings,
		const bool bIncludeMVVM,
		const bool bIncludeVolatile,
		FCaptureContext& Context)
	{
		Context.BlueprintPath = BoundedTopLevelObjectPath(Blueprint, Context, FString(),
			TEXT("Widget Blueprint"));
		if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Context.BlueprintPath))
		{
			Context.MarkIncomplete(TEXT("resolved_path_not_canonical"), TEXT("error"), FString(),
				TEXT("A loaded Widget Blueprint resolved outside the canonical /Game object namespace."));
			return;
		}
		FHyperAIUIRecord BlueprintRecord;
		BlueprintRecord.Kind = TEXT("blueprint");
		BlueprintRecord.BlueprintPath = Context.BlueprintPath;
		BlueprintRecord.StableId = Context.BlueprintPath;
		BlueprintRecord.Name = Blueprint->GetName();
		BlueprintRecord.RecordKey = TEXT("blueprint:") + Context.BlueprintPath;
		BlueprintRecord.ClassPath = ClassPath(Blueprint, Context, BlueprintRecord.RecordKey);
		const UPackage* Package = Blueprint->GetOutermost();
		const bool bWasLoadedFromDisk = Blueprint->HasAnyFlags(RF_WasLoaded);
		const bool bPackageClean = Package && !Package->IsDirty();
		const bool bClassExact = Blueprint->GetClass() == UWidgetBlueprint::StaticClass();
		Context.Snapshot.bAllTargetsLoadedFromDisk &= bWasLoadedFromDisk;
		Context.Snapshot.bAllTargetPackagesClean &= bPackageClean;
		Context.Snapshot.bAllTargetClassesExact &= bClassExact;
		const FName PackageName = FSoftObjectPath(Context.BlueprintPath).GetAssetPath().GetPackageName();
		const FPackageEvidence PackageEvidence = CapturePackageEvidence(
			PackageName, Context, BlueprintRecord.RecordKey);
		AddString(BlueprintRecord, TEXT("disk_existence"), TEXT("enum"),
			PackageEvidence.Existence, Context);
		AddString(BlueprintRecord, TEXT("package_saved_hash"), TEXT("string"),
			PackageEvidence.SavedHash, Context);
		AddString(BlueprintRecord, TEXT("package_disk_size"), TEXT("string"),
			LexToString(PackageEvidence.DiskSize), Context);
		if (Blueprint->ParentClass)
		{
			const FString ParentPath = BoundedTopLevelObjectPath(Blueprint->ParentClass, Context,
				BlueprintRecord.RecordKey, TEXT("Parent class"));
			if (!ParentPath.IsEmpty())
			{
				AddString(BlueprintRecord, TEXT("parent_class"), TEXT("path"), ParentPath, Context);
			}
		}
#if WITH_EDITORONLY_DATA
		if (Blueprint->WidgetVariableNameToGuidMap.Num()
			> FHyperAIStudioUIContracts::MaxWidgetsPerBlueprint
				+ FHyperAIStudioUIContracts::MaxAnimationsPerBlueprint)
		{
			Context.MarkIncomplete(TEXT("widget_variable_guid_bound_exceeded"), TEXT("error"),
				BlueprintRecord.RecordKey,
				TEXT("Widget/animation variable guid map exceeded its hard pre-iteration cap."));
		}
		else
		{
			for (const TPair<FName, FGuid>& Pair : Blueprint->WidgetVariableNameToGuidMap)
			{
				if (!IsBoundedName(Pair.Key, FHyperAIStudioUIContracts::MaxNameCharacters))
				{
					Context.MarkIncomplete(TEXT("variable_name_bound_exceeded"), TEXT("error"),
						BlueprintRecord.RecordKey, TEXT("Variable name exceeded its pre-copy cap."));
					continue;
				}
				const FString VariableName = Pair.Key.ToString();
				AddString(BlueprintRecord, TEXT("variable_guid_") + VariableName, TEXT("guid"),
					Pair.Value.ToString(EGuidFormats::Digits), Context);
			}
		}
#endif
		Context.AddRecord(MoveTemp(BlueprintRecord));
		if (bIncludeTree) CaptureTree(Blueprint, bIncludeLayout, Context);
#if WITH_EDITORONLY_DATA
		if (bIncludeAnimations)
		{
			if (Blueprint->Animations.Num() > FHyperAIStudioUIContracts::MaxAnimationsPerBlueprint)
			{
				Context.MarkIncomplete(TEXT("animation_bound_exceeded"), TEXT("error"), FString(),
					TEXT("Animations exceeded the hard pre-iteration cap."));
			}
			else
			{
				for (int32 Index = 0; Index < Blueprint->Animations.Num(); ++Index)
				{
					if (!Context.CheckDeadline()) break;
					CaptureAnimation(Blueprint->Animations[Index].Get(), Index, Context);
				}
			}
		}
#endif
		if (bIncludeLegacyBindings) CaptureLegacyBindings(Blueprint, Context);
		if (bIncludeMVVM) CaptureMVVM(Blueprint, Context);
		if (bIncludeVolatile)
		{
			FHyperAIUIRecord Volatile;
			Volatile.Kind = TEXT("blueprint");
			Volatile.BlueprintPath = Context.BlueprintPath;
			Volatile.StableId = Context.BlueprintPath + TEXT(":volatile");
			Volatile.Name = Blueprint->GetName();
			Volatile.RecordKey = TEXT("volatile_blueprint:") + Context.BlueprintPath;
			Volatile.bPersisted = false;
			AddBool(Volatile, TEXT("package_dirty"),
				!bPackageClean, Context);
			AddBool(Volatile, TEXT("was_loaded_from_disk"), bWasLoadedFromDisk, Context);
			AddBool(Volatile, TEXT("widget_blueprint_class_exact"), bClassExact, Context);
			AddInt(Volatile, TEXT("blueprint_status"), static_cast<int32>(Blueprint->Status), Context);
			AddBool(Volatile, TEXT("generated_class_present"),
				Blueprint->GeneratedClass != nullptr, Context);
			Context.Snapshot.bContainsVolatile = true;
			Context.AddRecord(MoveTemp(Volatile));
		}
	}

	void AddAssetStateRecord(
		const FString& Path,
		const FPackageEvidence& Evidence,
		FCaptureContext& Context)
	{
		FHyperAIUIRecord Record;
		Record.Kind = TEXT("asset_state");
		Record.BlueprintPath = Path;
		Record.StableId = Path;
		Record.Name = FSoftObjectPath(Path).GetAssetName();
		Record.RecordKey = TEXT("asset_state:") + Path;
		AddString(Record, TEXT("loaded_state"), TEXT("enum"), TEXT("not_loaded"), Context);
		AddString(Record, TEXT("disk_existence"), TEXT("enum"), Evidence.Existence, Context);
		AddString(Record, TEXT("package_saved_hash"), TEXT("string"), Evidence.SavedHash, Context);
		AddString(Record, TEXT("package_disk_size"), TEXT("string"),
			LexToString(Evidence.DiskSize), Context);
		Context.AddRecord(MoveTemp(Record));
		Context.MarkIncomplete(TEXT("target_not_already_loaded"), TEXT("error"),
			TEXT("asset_state:") + Path,
			TEXT("The exact Widget Blueprint target is not already loaded; no load was attempted."));
	}
}

bool FHyperAIStudioUICapture::Capture(
	const TArray<FString>& TargetPaths,
	const bool bIncludeTree,
	const bool bIncludeLayout,
	const bool bIncludeAnimations,
	const bool bIncludeLegacyBindings,
	const bool bIncludeMVVM,
	const bool bIncludeVolatile,
	const int32 DeadlineMs,
	FHyperAIStudioUIValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	using namespace HyperAIStudio::UI::Capture::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded-only Widget Blueprint capture requires the Unreal game thread.");
		return false;
	}
	if (DeadlineMs < 10 || DeadlineMs > 2000
		|| TargetPaths.Num() > FHyperAIStudioUIContracts::MaxTargetPaths)
	{
		OutStatus = TEXT("invalid_capture_bounds");
		OutDiagnostic = TEXT("Capture deadline or exact target count is outside the closed contract.");
		return false;
	}
	TSet<FString> UniqueTargetPaths;
	for (const FString& Path : TargetPaths)
	{
		if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Path)
			|| UniqueTargetPaths.Contains(Path))
		{
			OutStatus = TEXT("invalid_target_path");
			OutDiagnostic = TEXT("Every target must be one unique canonical exact /Game object path.");
			return false;
		}
		UniqueTargetPaths.Add(Path);
	}
	FAssetRegistryModule* AssetRegistryModule =
		FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry* AssetRegistry = AssetRegistryModule ? AssetRegistryModule->TryGet() : nullptr;
	FCaptureContext Context{OutSnapshot, FPlatformTime::Seconds()
		+ static_cast<double>(DeadlineMs) / 1000.0, FString(), AssetRegistry, false};
	TArray<UWidgetBlueprint*> Blueprints;
	if (!TargetPaths.IsEmpty())
	{
		Blueprints.Reserve(TargetPaths.Num());
		for (const FString& Path : TargetPaths)
		{
			if (!Context.CheckDeadline()) break;
			++OutSnapshot.LoadedObjectsScanned;
			UWidgetBlueprint* Blueprint = FindObject<UWidgetBlueprint>(
				nullptr, Path, EFindObjectFlags::None);
			if (!IsValid(Blueprint)
				|| Blueprint->HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
			{
				Context.BlueprintPath = Path;
				OutSnapshot.bAllTargetsLoadedFromDisk = false;
				OutSnapshot.bAllTargetPackagesClean = false;
				OutSnapshot.bAllTargetClassesExact = false;
				const FName PackageName = FSoftObjectPath(Path).GetAssetPath().GetPackageName();
				const FPackageEvidence Evidence = CapturePackageEvidence(
					PackageName, Context, TEXT("asset_state:") + Path);
				AddAssetStateRecord(Path, Evidence, Context);
				continue;
			}
			Blueprints.Add(Blueprint);
		}
	}
	else
	{
		int32 RawSlotsScanned = 0;
		for (FRawObjectIterator It;
			It && RawSlotsScanned < FHyperAIStudioUIContracts::MaxRawObjectSlotsScanned
				&& Blueprints.Num() < FHyperAIStudioUIContracts::MaxLoadedBlueprintsScanned;
			++It, ++RawSlotsScanned)
		{
			if (!Context.CheckDeadline()) break;
			FUObjectItem* Item = *It;
			UObject* Object = Item ? static_cast<UObject*>(Item->GetObject()) : nullptr;
			if (!IsValid(Object)) continue;
			if (UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Object))
			{
				FString CandidatePath;
				if (!Blueprint->HasAnyFlags(RF_ClassDefaultObject | RF_Transient)
					&& TryBoundedTopLevelObjectPath(Blueprint, CandidatePath)
					&& FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(CandidatePath))
				{
					Blueprints.Add(Blueprint);
				}
			}
		}
		OutSnapshot.LoadedObjectsScanned = RawSlotsScanned;
	}
	Blueprints.Sort([](const UWidgetBlueprint& A, const UWidgetBlueprint& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	for (const UWidgetBlueprint* Blueprint : Blueprints)
	{
		if (!Context.CheckDeadline()) break;
		CaptureBlueprint(Blueprint, bIncludeTree, bIncludeLayout, bIncludeAnimations,
			bIncludeLegacyBindings, bIncludeMVVM, bIncludeVolatile, Context);
	}
	FString FingerprintError;
	if (!FHyperAIStudioUIValueContracts::ComputeFingerprints(OutSnapshot, FingerprintError))
	{
		OutStatus = TEXT("fingerprint_failed");
		OutDiagnostic = FingerprintError;
		return false;
	}
	Context.CheckDeadline();
	OutStatus = OutSnapshot.bComplete ? TEXT("snapshot_complete") : TEXT("snapshot_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Bounded loaded-only Widget Blueprint values were captured without loading assets or creating MVVM state.")
		: TEXT("Loaded-only capture returned explicit bounded evidence but one or more values remain unsupported or incomplete.");
	return true;
}
