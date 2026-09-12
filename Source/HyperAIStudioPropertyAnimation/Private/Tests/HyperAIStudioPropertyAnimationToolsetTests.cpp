// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPropertyAnimationToolset.h"

#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIPropertyAnimationContractTest,
	"HyperAIStudio.PropertyAnimation.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIPropertyAnimationContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioPropertyAnimationManifestEntry>& Manifest =
		FHyperAIStudioPropertyAnimationContracts::GetManifest();
	TestEqual(TEXT("exact three tools"), Manifest.Num(), 3);
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioPropertyAnimationContracts::PackId),
		FString(TEXT("actor_modifier_property_animation")));
	TestEqual(TEXT("blocking group"),
		FString(FHyperAIStudioPropertyAnimationContracts::PluginRequirementGroupId),
		FString(TEXT("modifier_plugin")));
	TestTrue(TEXT("inspect exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_property_animation_inspect");
	}));
	TestTrue(TEXT("apply exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_property_animation_apply_plan");
	}));
	TestTrue(TEXT("validate exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_property_animation_validate");
	}));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioPropertyAnimationContracts::GetAdapterDescriptor();
	TestEqual(TEXT("three exact variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking group never downgraded"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("request namespace"), Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("result namespace"), Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
	}
	const FHyperAIPropertyAnimationCapabilityStatus Status =
		FHyperAIStudioPropertyAnimationContracts::GetCapabilityStatus();
	TestEqual(TEXT("native review"), Status.ReviewedEpicNativeCallableCount, 278);
	TestEqual(TEXT("python review"), Status.ReviewedEpicPythonCallableCount, 598);
	TestEqual(TEXT("no duplicate Epic MCP route"), Status.MatchingEpicCallableCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIPropertyAnimationValueModelTest,
	"HyperAIStudio.PropertyAnimation.ValueModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIPropertyAnimationValueModelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIPropertyAnimationPlanOperation Create;
	Create.Action = TEXT("create");
	Create.AnimatorClassPath = TEXT("/Script/PropertyAnimator.PropertyAnimatorWiggle");
	TArray<FHyperAIPropertyAnimationPlanOperation> Operations = {Create};
	FString Fingerprint;
	FString Error;
	TestTrue(TEXT("closed create valid"),
		FHyperAIStudioPropertyAnimationContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	TestTrue(TEXT("sealed"),
		FHyperAIStudioPropertyAnimationContracts::IsCanonicalSha256(Fingerprint));
	Operations.Add(Create);
	TestFalse(TEXT("duplicate exact action rejected"),
		FHyperAIStudioPropertyAnimationContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	Operations = {Create};
	Operations[0].Action = TEXT("execute_script");
	TestFalse(TEXT("unbounded dispatch rejected"),
		FHyperAIStudioPropertyAnimationContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIPropertyAnimationDetachedValidationTest,
	"HyperAIStudio.PropertyAnimation.DetachedValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIPropertyAnimationDetachedValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIPropertyAnimationSnapshot Snapshot;
	Snapshot.ActorPath = TEXT("/Game/HyperAI/MissingMap.MissingMap:PersistentLevel.MissingActor");
	Snapshot.ActorClassPath = TEXT("/Script/Engine.Actor");
	Snapshot.PackageName = FSoftObjectPath(Snapshot.ActorPath).GetLongPackageName();
	Snapshot.DiskExistence = TEXT("does_not_exist");
	Snapshot.PersistedFingerprint =
		FHyperAIStudioPropertyAnimationContracts::ComputePersistedFingerprint(Snapshot);
	Snapshot.VolatileFingerprint =
		FHyperAIStudioPropertyAnimationContracts::ComputeVolatileFingerprint(Snapshot);
	FHyperAIPropertyAnimationValidateRequest Request;
	Request.Snapshot = Snapshot;
	const FHyperAIPropertyAnimationValidateReport Valid =
		FHyperAIStudioPropertyAnimationContracts::Validate(Request);
	TestTrue(TEXT("detached envelope valid"), Valid.bValid);
	TestFalse(TEXT("absence is not complete"), Valid.bComplete);
	Request.Snapshot.VolatileFingerprint = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	const FHyperAIPropertyAnimationValidateReport Tampered =
		FHyperAIStudioPropertyAnimationContracts::Validate(Request);
	TestFalse(TEXT("tamper rejected"), Tampered.bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIPropertyAnimationNonDryZeroEffectTest,
	"HyperAIStudio.PropertyAnimation.NonDryZeroEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIPropertyAnimationNonDryZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Missing =
		TEXT("/Game/HyperAI/MissingMap.MissingMap:PersistentLevel.MissingActor");
	TestNull(TEXT("fixture unresolved"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIPropertyAnimationApplyPlanRequest Request;
	Request.bDryRun = false;
	Request.OperationId = TEXT("property-animation-zero-effect");
	Request.ExpectedPlanHash = TEXT("sha256:") + FString::ChrN(64, TEXT('b'));
	Request.ActorPath = Missing;
	Request.ExpectedPersistedFingerprint =
		TEXT("sha256:") + FString::ChrN(64, TEXT('c'));
	FHyperAIPropertyAnimationPlanOperation Create;
	Create.Action = TEXT("create");
	Create.AnimatorClassPath = TEXT("/Script/PropertyAnimator.PropertyAnimatorWiggle");
	Request.Operations = {Create};
	const FHyperAIPropertyAnimationApplyPlanReport Report =
		FHyperAIStudioPropertyAnimationContracts::BuildPlan(Request);
	TestEqual(TEXT("truthful blocker"), Report.Status,
		FString(FHyperAIStudioPropertyAnimationContracts::NonDryCallableState));
	TestFalse(TEXT("no effect"), Report.bEffectStarted);
	TestEqual(TEXT("zero physical effects"), Report.Effects.PhysicalEffectCount, 0);
	TestNull(TEXT("still unresolved"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif
