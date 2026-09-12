// Games by Hyper 2026.

/**
 * Compile-time UE 5.8 API audit for every directly linked base type. Optional ControlRig,
 * RigVM, IKRig, Retargeter, and PoseSearch headers are intentionally absent: their variants
 * cannot silently become reflection fallbacks or hard module dependencies.
 */

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateTransitionNode.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

static_assert(TIsDerivedFrom<UAnimBlueprint, UBlueprint>::Value,
	"UE 5.8 audit: UAnimBlueprint must remain a UBlueprint.");
static_assert(TIsDerivedFrom<UAnimMontage, UAnimCompositeBase>::Value,
	"UE 5.8 audit: UAnimMontage must remain a composite timeline.");
static_assert(TIsDerivedFrom<UAnimComposite, UAnimCompositeBase>::Value,
	"UE 5.8 audit: UAnimComposite must remain a composite timeline.");
static_assert(TIsDerivedFrom<UAnimSequence, UAnimSequenceBase>::Value,
	"UE 5.8 audit: UAnimSequence must remain a sequence timeline.");
static_assert(TIsDerivedFrom<UBlendSpace, UAnimationAsset>::Value,
	"UE 5.8 audit: UBlendSpace must remain an animation asset.");
static_assert(TIsDerivedFrom<UPoseAsset, UAnimationAsset>::Value,
	"UE 5.8 audit: UPoseAsset must remain an animation asset.");
static_assert(TIsDerivedFrom<UAnimationStateMachineGraph, UEdGraph>::Value,
	"UE 5.8 audit: state-machine inspection requires an editor graph.");
static_assert(TIsDerivedFrom<UAnimStateTransitionNode, UEdGraphNode>::Value,
	"UE 5.8 audit: transition endpoints require editor graph nodes.");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAnimationUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.AnimationRigging.UE58ApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAnimationUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	// Taking these exact member pointers makes signature drift a compile failure rather than
	// a runtime best-effort branch.
	auto BlueprintGraphs = &UBlueprint::GetAllGraphs;
	auto BlendSamples = &UBlendSpace::GetBlendSamples;
	auto BlendParameter = &UBlendSpace::GetBlendParameter;
		auto PoseNames = &UPoseAsset::GetPoseFNames;
		const FName (UPoseAsset::*PoseNameByIndex)(int32) const =
			&UPoseAsset::GetPoseNameByIndex;
		int32 (UPoseAsset::*BasePoseIndex)() const = &UPoseAsset::GetBasePoseIndex;
		const TArray<FAnimCurveBase>& (UPoseAsset::*PoseCurveData)() const =
			&UPoseAsset::GetCurveData;
	auto CompatibleSkeletons = &USkeleton::GetCompatibleSkeletons;
	auto CompatibleMesh = &USkeleton::IsCompatibleMesh;
	auto PreviousState = &UAnimStateTransitionNode::GetPreviousState;
	auto NextState = &UAnimStateTransitionNode::GetNextState;
	UE::AssetRegistry::EExists (IAssetRegistry::*TryPackageNonBlocking)(
		FName, FAssetPackageData&, bool) const = &IAssetRegistry::TryGetAssetPackageData;
	TestTrue(TEXT("UBlueprint::GetAllGraphs signature exists"), BlueprintGraphs != nullptr);
	TestTrue(TEXT("UBlendSpace::GetBlendSamples signature exists"), BlendSamples != nullptr);
	TestTrue(TEXT("UBlendSpace::GetBlendParameter signature exists"), BlendParameter != nullptr);
		TestTrue(TEXT("UPoseAsset::GetPoseFNames signature exists"), PoseNames != nullptr);
		TestTrue(TEXT("Base-pose projection uses UE 5.8's inline pose-name accessor"),
			PoseNameByIndex != nullptr);
		TestTrue(TEXT("Base-pose projection uses UE 5.8's inline base-index accessor"),
			BasePoseIndex != nullptr);
	TestTrue(TEXT("Pose curves bind to UE 5.8's reference-returning data view"),
		PoseCurveData != nullptr);
	TestTrue(TEXT("USkeleton::GetCompatibleSkeletons signature exists"), CompatibleSkeletons != nullptr);
	TestTrue(TEXT("USkeleton::IsCompatibleMesh signature exists"), CompatibleMesh != nullptr);
	TestTrue(TEXT("UAnimStateTransitionNode previous endpoint exists"), PreviousState != nullptr);
	TestTrue(TEXT("UAnimStateTransitionNode next endpoint exists"), NextState != nullptr);
	TestTrue(TEXT("Create absence binds UE 5.8's nonblocking package query"),
		TryPackageNonBlocking != nullptr);
	return true;
}

#endif
