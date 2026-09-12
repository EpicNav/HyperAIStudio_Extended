// Games by Hyper 2026.

#include "HyperAIStudioPaper2DToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "IO/IoHash.h"
#include "Misc/AutomationTest.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"

#include <type_traits>
#include <utility>

// Compile-time pins for only the public UE 5.8 APIs used by this projection.
static_assert(std::is_member_object_pointer_v<decltype(&UPaperSprite::BakedRenderData)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperSprite::BodySetup)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::MapWidth)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::MapHeight)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::TileWidth)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::TileHeight)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::TileLayers)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::Material)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileMap::BodySetup)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPaperTileLayer::LayerName)>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperSprite&>().GetPixelsPerUnrealUnit()), float>);
#if WITH_EDITOR
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperSprite&>().GetSourceSize()), FVector2D>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperSprite&>().GetCollisionThickness()), float>);
#endif
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperFlipbook&>().GetFramesPerSecond()), float>);
static_assert(std::is_same_v<
	decltype((std::declval<FScopedFlipbookMutator&>().FramesPerSecond)), float&>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperFlipbook&>().GetNumKeyFrames()), int32>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperFlipbook&>().GetKeyFrameChecked(0)),
	const FPaperFlipbookKeyFrame&>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileSet&>().GetTileSize()), FIntPoint>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileSet&>().GetTileSheetTexture()), UTexture2D*>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileSet&>().GetMargin()), FIntMargin>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileSet&>().GetPerTileSpacing()), FIntPoint>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileSet&>().GetDrawingOffset()), FIntPoint>);
static_assert(std::is_same_v<
	decltype(std::declval<UPaperTileSet&>().SetTileSize(FIntPoint())), void>);
static_assert(std::is_same_v<
	decltype(std::declval<UPaperTileSet&>().SetPerTileSpacing(FIntPoint())), void>);
static_assert(std::is_same_v<
	decltype(std::declval<UPaperTileSet&>().SetDrawingOffset(FIntPoint())), void>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileMap&>().GetPixelsPerUnrealUnit()), float>);
static_assert(std::is_same_v<
	decltype(std::declval<UPaperTileMap&>().SetCollisionThickness(50.0f)), void>);
static_assert(std::is_same_v<
	decltype(std::declval<UPaperTileMap&>().SetCollisionDomain(
		ESpriteCollisionMode::Use3DPhysics)), void>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileLayer&>().GetLayerWidth()), int32>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileLayer&>().GetLayerHeight()), int32>);
static_assert(std::is_same_v<
	decltype(std::declval<const UPaperTileLayer&>().GetLayerCollides()), bool>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAssetRegistry&>().TryGetAssetPackageData(
		FName(), std::declval<FAssetPackageData&>(), true)),
	UE::AssetRegistry::EExists>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_same_v<
	decltype(std::declval<const FAssetPackageData&>().GetPackageSavedHash()), FIoHash>);
#endif

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPaper2DUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.Paper2D.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPaper2DUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestNotNull(TEXT("UPaperSprite public class"), UPaperSprite::StaticClass());
	TestNotNull(TEXT("UPaperFlipbook public class"), UPaperFlipbook::StaticClass());
	TestNotNull(TEXT("UPaperTileSet public class"), UPaperTileSet::StaticClass());
	TestNotNull(TEXT("UPaperTileMap public class"), UPaperTileMap::StaticClass());
	TestEqual(TEXT("Asset Registry DoesNotExist pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), 0);
	TestEqual(TEXT("Asset Registry Exists pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Exists), 1);
	TestEqual(TEXT("Asset Registry Unknown pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Unknown), 2);
	TestEqual(TEXT("Flipbook collision enum upper bound"),
		static_cast<int32>(EFlipbookCollisionMode::EachFrameCollision), 2);
	TestEqual(TEXT("TileMap projection enum upper bound"),
		static_cast<int32>(ETileMapProjectionMode::HexagonalStaggered), 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
