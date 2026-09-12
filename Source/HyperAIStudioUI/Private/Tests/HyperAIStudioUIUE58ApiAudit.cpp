// Games by Hyper 2026.

#include "HyperAIStudioUIToolset.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetData.h"
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
#include "Components/Widget.h"
#include "Components/WidgetSwitcher.h"
#include "IO/IoHash.h"
#include "Internationalization/Text.h"
#include "Misc/AutomationTest.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintExtension.h"

#include <type_traits>
#include <utility>

// Compile-time pins for the exact UE 5.8 public typed API used by this pack.
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetBlueprint::WidgetTree)>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetBlueprint::Animations)>);
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetBlueprint::Bindings)>);
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetBlueprint::WidgetVariableNameToGuidMap)>);
#endif
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetTree::RootWidget)>);
static_assert(std::is_member_object_pointer_v<decltype(&UWidgetTree::NamedSlotBindings)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPanelWidget::GetChildrenCount)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPanelWidget::GetChildAt)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPanelSlot::Parent)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPanelSlot::Content)>);
static_assert(std::is_member_function_pointer_v<decltype(&UCanvasPanelSlot::GetLayout)>);
static_assert(std::is_member_function_pointer_v<decltype(&UGridSlot::GetRow)>);
static_assert(std::is_member_function_pointer_v<decltype(&UGridSlot::GetColumn)>);
static_assert(std::is_member_function_pointer_v<decltype(&UGridSlot::GetRowSpan)>);
static_assert(std::is_member_function_pointer_v<decltype(&UGridSlot::GetColumnSpan)>);
static_assert(std::is_member_function_pointer_v<decltype(&UHorizontalBoxSlot::GetPadding)>);
static_assert(std::is_member_function_pointer_v<decltype(&UVerticalBoxSlot::GetPadding)>);
static_assert(std::is_member_function_pointer_v<decltype(&UOverlaySlot::GetPadding)>);
PRAGMA_DISABLE_DEPRECATION_WARNINGS
static_assert(std::is_same_v<decltype(std::declval<const UWidget&>().Visibility), ESlateVisibility>);
static_assert(std::is_same_v<decltype(std::declval<const UWidget&>().bIsEnabled), uint8>);
static_assert(std::is_same_v<decltype(std::declval<const UWidget&>().bIsVariable), uint8>);
static_assert(std::is_same_v<decltype(std::declval<const UTextBlock&>().Text), FText>);
static_assert(std::is_same_v<
	decltype(std::declval<const UTextBlock&>().ColorAndOpacity), FSlateColor>);
static_assert(std::is_same_v<
	decltype(std::declval<const UImage&>().ColorAndOpacity), FLinearColor>);
static_assert(std::is_same_v<decltype(std::declval<const UBorder&>().BrushColor), FLinearColor>);
static_assert(std::is_same_v<decltype(std::declval<const UProgressBar&>().Percent), float>);
static_assert(std::is_same_v<
	decltype(std::declval<const UProgressBar&>().FillColorAndOpacity), FLinearColor>);
static_assert(std::is_same_v<decltype(std::declval<const USlider&>().Value), float>);
static_assert(std::is_same_v<decltype(std::declval<const USlider&>().MinValue), float>);
static_assert(std::is_same_v<decltype(std::declval<const USlider&>().MaxValue), float>);
static_assert(std::is_same_v<
	decltype(std::declval<const UCheckBox&>().CheckedState), ECheckBoxState>);
static_assert(std::is_same_v<
	decltype(std::declval<const UWidgetSwitcher&>().ActiveWidgetIndex), int32>);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
static_assert(std::is_member_function_pointer_v<decltype(&UWidgetAnimation::GetBindings)>);
static_assert(std::is_member_function_pointer_v<decltype(&UWidgetAnimation::GetMovieScene)>);
static_assert(std::is_member_function_pointer_v<decltype(&UWidgetAnimation::GetDisplayLabel)>);
static_assert(std::is_member_function_pointer_v<decltype(&UMovieScene::GetPlaybackRange)>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMovieScene>().GetBindings()),
	const TArray<FMovieSceneBinding>&>);
static_assert(std::is_member_function_pointer_v<decltype(&FMovieSceneBinding::GetTracks)>);
static_assert(std::is_same_v<
	decltype(UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(
		static_cast<const UWidgetBlueprint*>(nullptr))),
	UMVVMWidgetBlueprintExtension_View*>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMVVMWidgetBlueprintExtension_View>().GetBlueprintView()),
	const UMVVMBlueprintView*>);
static_assert(std::is_same_v<
	decltype(std::declval<UMVVMWidgetBlueprintExtension_View>().GetBlueprintView()),
	UMVVMBlueprintView*>);
static_assert(std::is_same_v<
	decltype(std::declval<UMVVMBlueprintView>().GetSettings()),
	UMVVMBlueprintViewSettings*>);
static_assert(std::is_member_object_pointer_v<
	decltype(&UMVVMBlueprintViewSettings::bInitializeSourcesOnConstruct)>);
static_assert(std::is_member_object_pointer_v<
	decltype(&UMVVMBlueprintViewSettings::bInitializeBindingsOnConstruct)>);
static_assert(std::is_member_object_pointer_v<
	decltype(&UMVVMBlueprintViewSettings::bInitializeEventsOnConstruct)>);
static_assert(std::is_member_object_pointer_v<
	decltype(&UMVVMBlueprintViewSettings::bCreateViewWithoutBindings)>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMVVMBlueprintView>().GetViewModels()),
	const TArrayView<const FMVVMBlueprintViewModelContext>>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMVVMBlueprintView>().GetBindings()),
	const TArrayView<const FMVVMBlueprintViewBinding>>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMVVMBlueprintView>().GetEvents()),
	const TArrayView<const TObjectPtr<UMVVMBlueprintViewEvent>>>);
static_assert(std::is_same_v<
	decltype(std::declval<const UMVVMBlueprintView>().GetConditions()),
	const TArrayView<const TObjectPtr<UMVVMBlueprintViewCondition>>>);
static_assert(std::is_member_function_pointer_v<
	decltype(&FMVVMBlueprintViewConversionPath::GetConversionFunction)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintViewModelContext::GetViewModelId)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintViewModelContext::GetViewModelName)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintPropertyPath::GetWidgetName)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintPropertyPath::GetViewModelId)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintPropertyPath::GetFieldPaths)>);
static_assert(std::is_member_function_pointer_v<decltype(&FMVVMBlueprintFieldPath::GetRawFieldName)>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAssetRegistry&>().TryGetAssetPackageData(
		FName(), std::declval<FAssetPackageData&>(), true)),
	UE::AssetRegistry::EExists>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetPackageData::DiskSize)>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_same_v<
	decltype(std::declval<const FAssetPackageData&>().GetPackageSavedHash()), FIoHash>);
#endif
static_assert(std::is_same_v<
	decltype(std::declval<const UObject&>().HasAnyFlags(RF_WasLoaded)), bool>);
static_assert(std::is_same_v<decltype(std::declval<const UPackage&>().IsDirty()), bool>);
static_assert(std::is_same_v<
	decltype(FTextInspector::GetSourceString(std::declval<const FText&>())),
	const FString*>);
static_assert(std::is_default_constructible_v<FRawObjectIterator>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.UI.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("WidgetBlueprint public class is present"), UWidgetBlueprint::StaticClass());
	TestNotNull(TEXT("WidgetTree public class is present"), UWidgetTree::StaticClass());
	TestNotNull(TEXT("WidgetAnimation public class is present"), UWidgetAnimation::StaticClass());
	TestNotNull(TEXT("MVVM existing-extension class is present"),
		UMVVMWidgetBlueprintExtension_View::StaticClass());
	TestNotNull(TEXT("MVVM existing-view class is present"), UMVVMBlueprintView::StaticClass());
	TestEqual(TEXT("AssetRegistry DoesNotExist numeric pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), 0);
	TestEqual(TEXT("AssetRegistry Exists numeric pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Exists), 1);
	TestEqual(TEXT("AssetRegistry Unknown numeric pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Unknown), 2);
	TestEqual(TEXT("Exists remains positive package evidence"),
		FHyperAIStudioUIContracts::ClassifyAssetRegistryExistence(
			static_cast<int32>(UE::AssetRegistry::EExists::Exists)),
		FString(TEXT("exists")));
	TestEqual(TEXT("DoesNotExist is the only absence state"),
		FHyperAIStudioUIContracts::ClassifyAssetRegistryExistence(
			static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist)),
		FString(TEXT("does_not_exist")));
	TestEqual(TEXT("Lock-unavailable Unknown never becomes absence"),
		FHyperAIStudioUIContracts::ClassifyAssetRegistryExistence(
			static_cast<int32>(UE::AssetRegistry::EExists::Unknown)),
		FString(TEXT("unknown")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
