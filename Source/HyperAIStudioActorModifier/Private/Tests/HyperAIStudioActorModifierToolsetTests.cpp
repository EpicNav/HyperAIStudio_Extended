// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioActorModifierToolset.h"

#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIActorModifierContractTest,
	"HyperAIStudio.ActorModifier.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIActorModifierContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioActorModifierManifestEntry>& Manifest =
		FHyperAIStudioActorModifierContracts::GetManifest();
	TestEqual(TEXT("exact three tools"), Manifest.Num(), 3);
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioActorModifierContracts::PackId),
		FString(TEXT("actor_modifier_property_animation")));
	TestEqual(TEXT("blocking group"),
		FString(FHyperAIStudioActorModifierContracts::PluginRequirementGroupId),
		FString(TEXT("modifier_plugin")));
	TestTrue(TEXT("inspect exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_actor_modifier_inspect");
	}));
	TestTrue(TEXT("apply exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_actor_modifier_apply_plan");
	}));
	TestTrue(TEXT("validate exact"), Manifest.ContainsByPredicate([](const auto& Entry)
	{
		return Entry.Name == TEXT("hyper_actor_modifier_validate");
	}));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioActorModifierContracts::GetAdapterDescriptor();
	TestEqual(TEXT("three exact variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("blocking group never downgraded"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("request namespace"), Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("result namespace"), Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
	}
	const FHyperAIActorModifierCapabilityStatus Status =
		FHyperAIStudioActorModifierContracts::GetCapabilityStatus();
	TestEqual(TEXT("native review"), Status.ReviewedEpicNativeCallableCount, 278);
	TestEqual(TEXT("python review"), Status.ReviewedEpicPythonCallableCount, 598);
	TestEqual(TEXT("no duplicate Epic MCP route"), Status.MatchingEpicCallableCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIActorModifierValueModelTest,
	"HyperAIStudio.ActorModifier.ValueModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIActorModifierValueModelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIActorModifierPlanOperation Add;
	Add.Action = TEXT("add");
	Add.ModifierName = TEXT("Arrangement");
	TArray<FHyperAIActorModifierPlanOperation> Operations = {Add};
	FString Fingerprint;
	FString Error;
	TestTrue(TEXT("closed add valid"),
		FHyperAIStudioActorModifierContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	TestTrue(TEXT("sealed"),
		FHyperAIStudioActorModifierContracts::IsCanonicalSha256(Fingerprint));
	Operations.Add(Add);
	TestFalse(TEXT("duplicate exact action rejected"),
		FHyperAIStudioActorModifierContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	Operations = {Add};
	Operations[0].Action = TEXT("execute_script");
	TestFalse(TEXT("unbounded dispatch rejected"),
		FHyperAIStudioActorModifierContracts::ValidateOperations(
			Operations, Fingerprint, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIActorModifierDetachedValidationTest,
	"HyperAIStudio.ActorModifier.DetachedValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIActorModifierDetachedValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIActorModifierSnapshot Snapshot;
	Snapshot.ActorPath = TEXT("/Game/HyperAI/MissingMap.MissingMap:PersistentLevel.MissingActor");
	Snapshot.ActorClassPath = TEXT("/Script/Engine.Actor");
	Snapshot.PackageName = FSoftObjectPath(Snapshot.ActorPath).GetLongPackageName();
	Snapshot.DiskExistence = TEXT("does_not_exist");
	Snapshot.PersistedFingerprint =
		FHyperAIStudioActorModifierContracts::ComputePersistedFingerprint(Snapshot);
	Snapshot.VolatileFingerprint =
		FHyperAIStudioActorModifierContracts::ComputeVolatileFingerprint(Snapshot);
	FHyperAIActorModifierValidateRequest Request;
	Request.Snapshot = Snapshot;
	const FHyperAIActorModifierValidateReport Valid =
		FHyperAIStudioActorModifierContracts::Validate(Request);
	TestTrue(TEXT("detached envelope valid"), Valid.bValid);
	TestFalse(TEXT("absence is not complete"), Valid.bComplete);
	Request.Snapshot.PersistedFingerprint = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	const FHyperAIActorModifierValidateReport Tampered =
		FHyperAIStudioActorModifierContracts::Validate(Request);
	TestFalse(TEXT("tamper rejected"), Tampered.bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIActorModifierNonDryZeroEffectTest,
	"HyperAIStudio.ActorModifier.NonDryZeroEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIActorModifierNonDryZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Missing =
		TEXT("/Game/HyperAI/MissingMap.MissingMap:PersistentLevel.MissingActor");
	TestNull(TEXT("fixture unresolved"), FSoftObjectPath(Missing).ResolveObject());
	FHyperAIActorModifierApplyPlanRequest Request;
	Request.bDryRun = false;
	Request.OperationId = TEXT("actor-modifier-zero-effect");
	Request.ExpectedPlanHash = TEXT("sha256:") + FString::ChrN(64, TEXT('b'));
	Request.ActorPath = Missing;
	Request.ExpectedPersistedFingerprint =
		TEXT("sha256:") + FString::ChrN(64, TEXT('c'));
	FHyperAIActorModifierPlanOperation Add;
	Add.Action = TEXT("add");
	Add.ModifierName = TEXT("Arrangement");
	Request.Operations = {Add};
	const FHyperAIActorModifierApplyPlanReport Report =
		FHyperAIStudioActorModifierContracts::BuildPlan(Request);
	TestEqual(TEXT("truthful blocker"), Report.Status,
		FString(FHyperAIStudioActorModifierContracts::NonDryCallableState));
	TestFalse(TEXT("no effect"), Report.bEffectStarted);
	TestEqual(TEXT("zero physical effects"), Report.Effects.PhysicalEffectCount, 0);
	TestNull(TEXT("still unresolved"), FSoftObjectPath(Missing).ResolveObject());
	return true;
}

#endif
