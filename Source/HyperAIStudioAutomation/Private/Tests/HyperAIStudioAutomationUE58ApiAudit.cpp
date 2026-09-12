// Games by Hyper 2026.

#include "HyperAIStudioAutomationToolset.h"

#include "IAutomationControllerManager.h"
#include "IAutomationControllerModule.h"
#include "IAutomationReport.h"
#include "Misc/AutomationTest.h"

#include <type_traits>
#include <utility>

// Compile-time pins for the narrow UE 5.8 public read APIs used by this module.
static_assert(std::is_same_v<
	decltype(std::declval<IAutomationControllerModule&>().GetAutomationController()),
	IAutomationControllerManagerRef>);
static_assert(std::is_same_v<
	decltype(std::declval<IAutomationControllerManager&>().IsReadyForTests()), bool>);
static_assert(std::is_same_v<
	decltype(std::declval<IAutomationControllerManager&>().GetFilteredReports()),
	TArray<TSharedPtr<IAutomationReport>>&>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().GetEnabledTestsNum()),
	int32>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().GetTestState()),
	EAutomationControllerModuleState::Type>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>()
		.CheckTestResultsAvailable()), bool>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().ReportsHaveErrors()),
	bool>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().ReportsHaveWarnings()),
	bool>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().ReportsHaveLogs()),
	bool>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().GetNumDeviceClusters()),
	int32>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationControllerManager&>().IsTestRunnable(
		std::declval<IAutomationReportPtr>())), bool>);
static_assert(std::is_same_v<
	decltype(std::declval<IAutomationReport&>().IsParent()), bool>);
static_assert(std::is_same_v<
	decltype(std::declval<IAutomationReport&>().GetFilteredChildren()),
	TArray<TSharedPtr<IAutomationReport>>&>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAutomationReport&>().GetFullTestPath()),
	const FString&>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.Automation.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAutomationUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("UE 5.8 AutomationController read API pins compiled"), true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
