// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioToolInventoryViewModel.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioToolInventoryPresentationTest,
	"HyperAIStudio.UI.ToolInventoryPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioToolInventoryPresentationTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioToolInventoryView View;
	View.DispatcherCount = 3;
	View.LiveEpicToolCount = 264;
	View.AvailableHyperToolCount = 109;
	TestEqual(TEXT("Dispatchers remain separate from callable tools"), View.DispatcherCount, 3);
	TestEqual(TEXT("Current available total combines Epic and HyperAI tools"), View.GetAvailableToolCount(), 373);
	View.LiveEpicToolCount = 255;
	View.AvailableHyperToolCount = 91;
	TestEqual(TEXT("A partial live session reports its actual count, not the 109-tool catalog"), View.GetAvailableToolCount(), 346);
	View.LiveEpicToolCount = 264;
	View.AvailableHyperToolCount = 109;
	TestEqual(TEXT("UE 5.8 installed-source declarations are labeled separately"),
		FHyperAIStudioToolInventoryView::KnownEpicUE58DeclarationCount,
		876);

	FHyperAIStudioToolInventoryRow EpicRow;
	EpicRow.Name = TEXT("get_current_level");
	EpicRow.QualifiedName = TEXT("editor_toolset.toolsets.scene.SceneTools.get_current_level");
	EpicRow.Toolset = TEXT("editor_toolset.toolsets.scene.SceneTools");
	EpicRow.Description = TEXT("Get the current Unreal level.");
	EpicRow.Capability = TEXT("Epic tool");
	EpicRow.Source = EHyperAIStudioToolInventorySource::Epic;
	TestTrue(TEXT("Search matches the short tool name"),
		FHyperAIStudioToolInventoryViewModel::Matches(EpicRow, TEXT("current_level"), {}));
	TestTrue(TEXT("Search matches a description"),
		FHyperAIStudioToolInventoryViewModel::Matches(EpicRow, TEXT("Unreal level"), {}));
	TestTrue(TEXT("Epic source filter includes Epic rows"),
		FHyperAIStudioToolInventoryViewModel::Matches(
			EpicRow,
			FString(),
			EHyperAIStudioToolInventorySource::Epic));
	TestFalse(TEXT("HyperAI source filter excludes Epic rows"),
		FHyperAIStudioToolInventoryViewModel::Matches(
			EpicRow,
			FString(),
			EHyperAIStudioToolInventorySource::HyperAI));
	return true;
}

#endif
