// Games by Hyper 2026.

#include "HyperAIStudioCharacterToolset.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Character::Tests
{
	FString Sha(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FHyperAICharacterDetachedSnapshot MakeValidSnapshot()
	{
		FHyperAICharacterAssetRecord Record;
		Record.TargetPath = TEXT("/Game/Characters/MH_Test.MH_Test");
		Record.PackageName = TEXT("/Game/Characters/MH_Test");
		Record.ClassPath = FHyperAIStudioCharacterContracts::ExpectedCharacterClassPath;
		Record.DiskExistence = TEXT("exists");
		Record.PackageSavedHash = TEXT("0123456789abcdef");
		Record.DiskSize = 4096;
		Record.bLoaded = true;
		Record.bWasLoadedFromDisk = true;
		Record.bPackageDirty = false;
		Record.bTopLevelPrimaryObject = true;
		Record.bCharacterClassMatched = true;
		Record.bPersistedIdentityComplete = true;
		Record.bSemanticProjectionComplete = false;
		Record.PersistedFingerprint =
			FHyperAIStudioCharacterContracts::ComputeRecordPersistedFingerprint(Record);
		Record.VolatileFingerprint =
			FHyperAIStudioCharacterContracts::ComputeRecordVolatileFingerprint(Record);

		FHyperAICharacterDetachedSnapshot Snapshot;
		Snapshot.Records = {Record};
		Snapshot.TotalRecords = 1;
		Snapshot.bIdentityProjectionComplete = true;
		Snapshot.bSemanticProjectionComplete = false;
		const TArray<FString> Paths = {Record.TargetPath};
		Snapshot.RequestFingerprint =
			FHyperAIStudioCharacterContracts::ComputeRequestFingerprint(Paths);
		Snapshot.PersistedFingerprint =
			FHyperAIStudioCharacterContracts::ComputeSnapshotPersistedFingerprint(
				Snapshot.Records);
		Snapshot.VolatileFingerprint =
			FHyperAIStudioCharacterContracts::ComputeSnapshotVolatileFingerprint(
				Snapshot.Records);
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCharacterContractTest,
	"HyperAIStudio.NativeTools.Character.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCharacterContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioCharacterManifestEntry>& Manifest =
		FHyperAIStudioCharacterContracts::GetManifest();
	TestEqual(TEXT("Exact atomic manifest"), Manifest.Num(), 3);
	TestEqual(TEXT("Exact pack id"), FString(FHyperAIStudioCharacterContracts::PackId),
		FString(TEXT("character")));
	TestEqual(TEXT("Exact cohort"),
		FString(FHyperAIStudioCharacterContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiocharactertoolset.v1")));
	TestEqual(TEXT("Qualified toolset"),
		FHyperAIStudioCharacterContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioCharacter.HyperAIStudioCharacterToolset")));

	TSet<FString> Names;
	for (const FHyperAIStudioCharacterManifestEntry& Entry : Manifest)
	{
		Names.Add(Entry.Name);
		TestEqual(TEXT("Manifest qualifier"), Entry.QualifiedToolset,
			FHyperAIStudioCharacterContracts::GetQualifiedToolsetName());
	}
	TestTrue(TEXT("Inspect present"), Names.Contains(TEXT("hyper_character_inspect")));
	TestTrue(TEXT("Apply present"), Names.Contains(TEXT("hyper_character_apply_plan")));
	TestTrue(TEXT("Validate present"), Names.Contains(TEXT("hyper_character_validate")));

	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioCharacterContracts::GetAdapterDescriptor();
	TestEqual(TEXT("Descriptor variant count"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("No blocking group mislabeled nonblocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("Request namespace"),
			Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("Result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		TestTrue(TEXT("Request/result disjoint"),
			Variant.RequestTypeId != Variant.ResultTypeId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCharacterAuthorityTest,
	"HyperAIStudio.NativeTools.Character.Authority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCharacterAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString>& Delegates = FHyperAIStudioCharacterContracts::GetEpicDelegates();
	TestEqual(TEXT("All Epic MetaHumanGenerator functions delegated"), Delegates.Num(), 9);
	TestTrue(TEXT("begin_edit exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.begin_edit")));
	TestTrue(TEXT("end_edit exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.end_edit")));
	TestTrue(TEXT("get_body_shape exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.get_body_shape")));
	TestTrue(TEXT("set_body_shape exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.set_body_shape")));
	TestTrue(TEXT("get_skin_tone exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.get_skin_tone")));
	TestTrue(TEXT("set_skin_tone exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.set_skin_tone")));
	TestTrue(TEXT("get_eye_color exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.get_eye_color")));
	TestTrue(TEXT("set_eye_color exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.set_eye_color")));
	TestTrue(TEXT("create exact"),
		Delegates.Contains(TEXT("MetaHumanGenerator.MetaHumanToolset.create")));
	TestEqual(TEXT("Platform delegates plus one product requirement"),
		FHyperAIStudioCharacterContracts::GetAuthorityRows().Num(), 10);
	const FHyperAICharacterCapabilityStatus Status =
		FHyperAIStudioCharacterContracts::GetCapabilityStatus();
	TestTrue(TEXT("Authority fingerprint sealed"),
		FHyperAIStudioCharacterContracts::IsCanonicalSha256(Status.AuthorityFingerprint));
	TestFalse(TEXT("No mutation claim"), Status.bMutationExecutionImplemented);
	TestFalse(TEXT("No semantic projection claim"), Status.bSemanticProjectionComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCharacterDetachedValidatorTest,
	"HyperAIStudio.NativeTools.Character.DetachedValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCharacterDetachedValidatorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAICharacterValidateRequest Request;
	Request.Snapshot = HyperAIStudio::Character::Tests::MakeValidSnapshot();
	Request.MaxIssues = 32;
	Request.MaxOutputBytes = 65536;
	const FHyperAICharacterValidateReport Valid =
		FHyperAIStudioCharacterContracts::Validate(Request);
	TestTrue(TEXT("Detached validator ran"), Valid.bOk);
	TestTrue(TEXT("Complete synthetic identity valid"), Valid.bValid);
	TestTrue(TEXT("Complete synthetic identity complete"), Valid.bComplete);

	Request.Snapshot.Records[0].ClassPath = TEXT("/Script/Engine.DataAsset");
	const FHyperAICharacterValidateReport Tampered =
		FHyperAIStudioCharacterContracts::Validate(Request);
	TestTrue(TEXT("Tampered validator ran"), Tampered.bOk);
	TestFalse(TEXT("Tampered snapshot rejected"), Tampered.bValid);
	TestTrue(TEXT("Tamper produces errors"), Tampered.ErrorCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCharacterIntentAndZeroEffectTest,
	"HyperAIStudio.NativeTools.Character.IntentAndZeroEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCharacterIntentAndZeroEffectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FHyperAICharacterDesiredValue> Values = {
		{TEXT("body.fat"), 0.4},
		{TEXT("eyes.brightness"), 0.8}};
	FString Fingerprint;
	FString Error;
	TestTrue(TEXT("Closed values valid"),
		FHyperAIStudioCharacterContracts::ValidateDesiredValues(
			Values, Fingerprint, Error));
	TestTrue(TEXT("Desired value fingerprint sealed"),
		FHyperAIStudioCharacterContracts::IsCanonicalSha256(Fingerprint));
	Values.Add({TEXT("body.fat"), 0.5});
	TestFalse(TEXT("Duplicate desired key rejected"),
		FHyperAIStudioCharacterContracts::ValidateDesiredValues(
			Values, Fingerprint, Error));

	FHyperAICharacterApplyPlanRequest Request;
	Request.bDryRun = false;
	Request.OperationId = TEXT("character-zero-effect-001");
	Request.ExpectedPlanHash = HyperAIStudio::Character::Tests::Sha(TEXT('a'));
	Request.TargetPath = TEXT("/Game/Characters/MH_Test.MH_Test");
	Request.ExpectedPersistedFingerprint =
		HyperAIStudio::Character::Tests::Sha(TEXT('b'));
	Request.DesiredValues = {{TEXT("skin.lightness"), 0.5}};
	const FHyperAICharacterApplyPlanReport Report =
		FHyperAIStudioCharacterContracts::BuildPlan(Request);
	TestEqual(TEXT("Stable non-dry blocker"), Report.Status,
		FString(FHyperAIStudioCharacterContracts::NonDryCallableState));
	TestFalse(TEXT("Non-dry blocker is not success"), Report.bOk);
	TestFalse(TEXT("Never staged"), Report.bStaged);
	TestFalse(TEXT("Never submitted"), Report.bExecutionSubmitted);
	TestEqual(TEXT("Zero physical effects"), Report.Effects.PhysicalEffectCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCharacterCapabilityOnlyTest,
	"HyperAIStudio.NativeTools.Character.CapabilityOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCharacterCapabilityOnlyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAICharacterInspectRequest Request;
	const FHyperAICharacterInspectReport Report =
		FHyperAIStudioCharacterContracts::Inspect(Request);
	TestTrue(TEXT("Capability-only succeeds"), Report.bOk);
	TestEqual(TEXT("Capability-only status"), Report.Status,
		FString(TEXT("capability_only")));
	TestEqual(TEXT("All nine delegates exposed"),
		Report.Capability.DelegatedEpicCallables.Num(), 9);
	TestEqual(TEXT("No records without scope"), Report.Snapshot.Records.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
