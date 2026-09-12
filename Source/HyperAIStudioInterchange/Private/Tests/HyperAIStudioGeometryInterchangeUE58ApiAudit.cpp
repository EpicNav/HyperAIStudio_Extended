// Games by Hyper 2026.

#include "HyperAIStudioInterchangeToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/EngineTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "IO/IoHash.h"
#include "InterchangeAssetImportData.h"
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"

#include <type_traits>
#include <utility>

// One compile-time sweep over every public UE 5.8 value API used by pack 19.
static_assert(std::is_same_v<decltype(std::declval<const UStaticMesh&>().IsCompiling()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const UStaticMesh&>().GetNumSourceModels()), int32>);
static_assert(std::is_same_v<decltype(std::declval<const UStaticMesh&>().GetSourceModel(0)), const FStaticMeshSourceModel&>);
static_assert(std::is_same_v<decltype(std::declval<const UStaticMesh&>().GetStaticMaterials()), const TArray<FStaticMaterial>&>);
static_assert(std::is_same_v<decltype(std::declval<const UStaticMesh&>().GetAssetImportData()), UAssetImportData*>);
static_assert(std::is_same_v<decltype(std::declval<const USkeletalMesh&>().IsCompiling()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const USkeletalMesh&>().GetNumSourceModels()), int32>);
static_assert(std::is_same_v<decltype(std::declval<const USkeletalMesh&>().GetMaterials()), const TArray<FSkeletalMaterial>&>);
static_assert(std::is_same_v<decltype(std::declval<const USkeletalMesh&>().GetAssetImportData()), UAssetImportData*>);
static_assert(std::is_member_object_pointer_v<decltype(&FStaticMeshSourceModel::BuildSettings)>);
static_assert(std::is_member_object_pointer_v<decltype(&FMeshBuildSettings::BuildScale3D)>);
static_assert(std::is_member_object_pointer_v<decltype(&FMeshBuildSettings::DistanceFieldResolutionScale)>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAssetRegistry&>().TryGetAssetPackageData(
		FName(), std::declval<FAssetPackageData&>(), true)),
	UE::AssetRegistry::EExists>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_same_v<
	decltype(std::declval<const FAssetPackageData&>().GetPackageSavedHash()), FIoHash>);
#endif
static_assert(std::is_same_v<
	decltype(std::declval<const UAssetImportData&>().GetSourceData()), const FAssetImportInfo&>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetImportInfo::SourceFiles)>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetImportInfo::FSourceFile::RelativeFilename)>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetImportInfo::FSourceFile::Timestamp)>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetImportInfo::FSourceFile::FileHash)>);
static_assert(std::is_member_object_pointer_v<decltype(&FAssetImportInfo::FSourceFile::DisplayLabelName)>);
static_assert(std::is_member_object_pointer_v<decltype(&UInterchangeAssetImportData::NodeUniqueID)>);
static_assert(std::is_member_object_pointer_v<decltype(&UInterchangeAssetImportData::SceneImportAsset)>);
static_assert(std::is_same_v<decltype(std::declval<const UObject&>().HasAnyFlags(RF_WasLoaded)), bool>);
static_assert(std::is_same_v<decltype(std::declval<const UPackage&>().IsDirty()), bool>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioGeometryInterchangeUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.GeometryInterchange.UE58PublicApiSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioGeometryInterchangeUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestNotNull(TEXT("UStaticMesh public class"), UStaticMesh::StaticClass());
	TestNotNull(TEXT("USkeletalMesh public class"), USkeletalMesh::StaticClass());
	TestNotNull(TEXT("UInterchangeAssetImportData public class"),
		UInterchangeAssetImportData::StaticClass());
	TestEqual(TEXT("Asset Registry Unknown pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Unknown), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
