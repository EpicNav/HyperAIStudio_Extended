// Games by Hyper 2026.

#include "HyperAIStudioLiveProductionToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "IO/IoHash.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#include <type_traits>
#include <utility>

// Compile-time pins for the narrow public UE 5.8 APIs used by the clean-room
// base adapter. Optional live-production plugin APIs are deliberately not linked.
static_assert(std::is_same_v<
	decltype(std::declval<const IAssetRegistry&>().TryGetAssetPackageData(
		FName(), std::declval<FAssetPackageData&>(), true)),
	UE::AssetRegistry::EExists>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_same_v<
	decltype(std::declval<const FAssetPackageData&>().GetPackageSavedHash()), FIoHash>);
#endif
static_assert(std::is_same_v<decltype(std::declval<const UObject&>().HasAnyFlags(
	RF_WasLoaded)), bool>);
static_assert(std::is_same_v<decltype(std::declval<const UPackage&>().IsDirty()), bool>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLiveProductionUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.LiveProduction.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLiveProductionUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("Asset Registry DoesNotExist pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), 0);
	TestEqual(TEXT("Asset Registry Exists pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Exists), 1);
	TestEqual(TEXT("Asset Registry Unknown pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Unknown), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
