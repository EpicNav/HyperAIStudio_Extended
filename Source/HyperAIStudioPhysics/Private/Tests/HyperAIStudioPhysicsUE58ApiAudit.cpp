// Games by Hyper 2026.

#include "HyperAIStudioPhysicsToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "IO/IoHash.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/RigidBodyIndexPair.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UObject/Package.h"

#include <type_traits>
#include <utility>

// Compile-time pins for the exact public UE 5.8 APIs used by the clean-room projection.
static_assert(std::is_member_object_pointer_v<decltype(&UPhysicsAsset::SkeletalBodySetups)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPhysicsAsset::ConstraintSetup)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPhysicsAsset::CollisionDisableTable)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPhysicsAsset::SolverSettings)>);
static_assert(std::is_member_object_pointer_v<decltype(&USkeletalBodySetup::AggGeom)>);
static_assert(std::is_member_object_pointer_v<decltype(&USkeletalBodySetup::DefaultInstance)>);
static_assert(std::is_member_object_pointer_v<decltype(&USkeletalBodySetup::BodySetupGuid)>);
static_assert(std::is_member_object_pointer_v<decltype(&USkeletalBodySetup::BoneName)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPhysicsConstraintTemplate::DefaultInstance)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::SphereElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::BoxElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::SphylElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::ConvexElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::TaperedCapsuleElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::LevelSetElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::SkinnedLevelSetElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::MLLevelSetElems)>);
static_assert(std::is_member_object_pointer_v<decltype(&FKAggregateGeom::SkinnedTriangleMeshElems)>);
static_assert(std::is_same_v<
	decltype(std::declval<const IAssetRegistry&>().TryGetAssetPackageData(
		FName(), std::declval<FAssetPackageData&>(), true)),
	UE::AssetRegistry::EExists>);
#if WITH_EDITORONLY_DATA
static_assert(std::is_same_v<
	decltype(std::declval<const FAssetPackageData&>().GetPackageSavedHash()), FIoHash>);
#endif
static_assert(std::is_same_v<
	decltype(std::declval<const UBodySetupCore&>().GetCollisionTraceFlag()),
	TEnumAsByte<ECollisionTraceFlag>>);
static_assert(std::is_same_v<
	decltype(std::declval<const FBodyInstance&>().GetCollisionEnabled(false)),
	ECollisionEnabled::Type>);
static_assert(std::is_same_v<
	decltype(std::declval<const FConstraintInstance&>().GetChildBoneName()), const FName&>);
static_assert(std::is_same_v<
	decltype(std::declval<const FConstraintInstance&>().GetParentBoneName()), const FName&>);
static_assert(std::is_same_v<
	decltype(std::declval<const FConstraintInstance&>().GetRefFrame(EConstraintFrame::Frame1)),
	FTransform>);
static_assert(std::is_same_v<decltype(std::declval<const UObject&>().HasAnyFlags(RF_WasLoaded)), bool>);
static_assert(std::is_same_v<decltype(std::declval<const UPackage&>().IsDirty()), bool>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhysicsUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.Physics.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPhysicsUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestNotNull(TEXT("UPhysicsAsset public class"), UPhysicsAsset::StaticClass());
	TestNotNull(TEXT("USkeletalBodySetup public class"), USkeletalBodySetup::StaticClass());
	TestNotNull(TEXT("UPhysicsConstraintTemplate public class"),
		UPhysicsConstraintTemplate::StaticClass());
	TestEqual(TEXT("Asset Registry DoesNotExist pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), 0);
	TestEqual(TEXT("Asset Registry Exists pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Exists), 1);
	TestEqual(TEXT("Asset Registry Unknown pin"),
		static_cast<int32>(UE::AssetRegistry::EExists::Unknown), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
