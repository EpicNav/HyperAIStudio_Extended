// Games by Hyper 2026.

#include "HyperAIStudioAutomationToolset.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Automation::Tests
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIAutomationDetachedSnapshot MakeValidSnapshot()
	{
		FHyperAIAutomationTestRecord Record;
		Record.TestName = TEXT("HyperAIStudio.NativeTools.Sample");
		Record.bPresent = true;
		Record.bRunnable = true;
		Record.Fingerprint =
			FHyperAIStudioAutomationContracts::ComputeTestRecordFingerprint(Record);

		FHyperAIAutomationDetachedSnapshot Snapshot;
		Snapshot.ControllerState = TEXT("Ready");
		Snapshot.bControllerLoaded = true;
		Snapshot.bControllerReady = true;
		Snapshot.EnabledTestCount = 1;
		Snapshot.NumPasses = 1;
		Snapshot.DeviceClusterCount = 1;
		Snapshot.VisitedReportCount = 1;
		Snapshot.bTraversalComplete = true;
		Snapshot.bSnapshotComplete = true;
		Snapshot.Tests = {Record};
		FString Error;
		const TArray<FString> Names = {Record.TestName};
		FHyperAIStudioAutomationContracts::ValidateTestNames(
			Names, Snapshot.RequestedNamesFingerprint, Error);
		Snapshot.CatalogFingerprint =
			FHyperAIStudioAutomationContracts::ComputeCatalogFingerprint(Snapshot.Tests);
		Snapshot.ObservationFingerprint =
			FHyperAIStudioAutomationContracts::ComputeObservationFingerprint(Snapshot);
		return Snapshot;
	}

	FHyperAIStudioDomainDispatchContext MakeContext(
		const FHyperAIStudioAutomationDomainAdapter& Adapter,
		const TCHAR* ToolName,
		const TCHAR* VariantId)
	{
		FHyperAIStudioDomainDispatchContext Context;
		Context.Binding.PackId = FHyperAIStudioAutomationContracts::PackId;
		Context.Binding.ToolName = ToolName;
		Context.Binding.VariantId = VariantId;
		Context.Binding.ExpectedAdapterFingerprint =
			Adapter.GetDescriptor().AdapterFingerprint;
		Context.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
		return Context;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationContractTest,
	"HyperAIStudio.NativeTools.Automation.Contract",
	HyperAIStudio::Automation::Tests::Flags)

bool FHyperAIStudioAutomationContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FHyperAIStudioAutomationManifestEntry>& Manifest =
		FHyperAIStudioAutomationContracts::GetManifest();
	TestEqual(TEXT("Exact atomic manifest"), Manifest.Num(), 5);
	TestEqual(TEXT("Exact pack id"), FString(FHyperAIStudioAutomationContracts::PackId),
		FString(TEXT("automation_profiling_build")));
	TestEqual(TEXT("Exact cohort"),
		FString(FHyperAIStudioAutomationContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioautomationtoolset.v1")));

	const TSet<FString> Expected = {
		TEXT("hyper_test_inspect"),
		TEXT("hyper_test_run"),
		TEXT("hyper_test_validate"),
		TEXT("hyper_profile_capture"),
		TEXT("hyper_build_diagnose")};
	TSet<FString> Actual;
	for (const FHyperAIStudioAutomationManifestEntry& Entry : Manifest)
	{
		Actual.Add(Entry.Name);
		TestEqual(TEXT("Manifest qualifier"), Entry.QualifiedToolset,
			FHyperAIStudioAutomationContracts::GetQualifiedToolsetName());
	}
	TestTrue(TEXT("Exact five names"), Actual.Includes(Expected) && Expected.Includes(Actual));

	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioAutomationContracts::GetAdapterDescriptor();
	TestEqual(TEXT("Descriptor variant count"), Descriptor.Variants.Num(), 5);
	TestEqual(TEXT("Blocking automation group is never nonblocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.Num(), 0);
	int32 ReadCount = 0;
	int32 ExternalEffectCount = 0;
	for (const FHyperAIStudioDomainVariantDescriptor& Variant : Descriptor.Variants)
	{
		TestTrue(TEXT("Request namespace"),
			Variant.RequestTypeId.StartsWith(TEXT("hyperai.payload.")));
		TestTrue(TEXT("Result namespace"),
			Variant.ResultTypeId.StartsWith(TEXT("hyperai.result.")));
		TestTrue(TEXT("Request/result disjoint"),
			Variant.RequestTypeId != Variant.ResultTypeId);
		ReadCount += Variant.Safety == EHyperAIStudioDomainSafety::Read ? 1 : 0;
		ExternalEffectCount +=
			Variant.Safety == EHyperAIStudioDomainSafety::ExternalEffect ? 1 : 0;
	}
	TestEqual(TEXT("Two read contracts"), ReadCount, 2);
	TestEqual(TEXT("Three conservatively effect-classified contracts"),
		ExternalEffectCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationAuthorityTest,
	"HyperAIStudio.NativeTools.Automation.Authority",
	HyperAIStudio::Automation::Tests::Flags)

bool FHyperAIStudioAutomationAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString>& Delegates = FHyperAIStudioAutomationContracts::GetEpicDelegates();
	TestEqual(TEXT("Seven native and two programmatic delegates"), Delegates.Num(), 9);
	TestTrue(TEXT("Native discovery delegated"), Delegates.Contains(
		TEXT("AutomationTestToolset.AutomationTestToolset.DiscoverTests")));
	TestTrue(TEXT("Native run delegated"), Delegates.Contains(
		TEXT("AutomationTestToolset.AutomationTestToolset.RunTests")));
	TestTrue(TEXT("Native stop delegated"), Delegates.Contains(
		TEXT("AutomationTestToolset.AutomationTestToolset.StopTests")));
	TestTrue(TEXT("Programmatic execution delegated"), Delegates.Contains(
		TEXT("EditorToolset.ProgrammaticToolset.execute_tool_script")));
	TestEqual(TEXT("Frozen authority rows"),
		FHyperAIStudioAutomationContracts::GetAuthorityRows().Num(), 19);
	const FHyperAIAutomationCapabilityStatus Status =
		FHyperAIStudioAutomationContracts::GetCapabilityStatus();
	TestTrue(TEXT("Authority fingerprint sealed"),
		FHyperAIStudioAutomationContracts::IsCanonicalSha256(Status.AuthorityFingerprint));
	TestFalse(TEXT("No test execution claim"), Status.bTestExecutionImplemented);
	TestFalse(TEXT("No profile execution claim"), Status.bProfileExecutionImplemented);
	TestFalse(TEXT("No build execution claim"), Status.bBuildExecutionImplemented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationDetachedValidatorTest,
	"HyperAIStudio.NativeTools.Automation.DetachedValidator",
	HyperAIStudio::Automation::Tests::Flags)

bool FHyperAIStudioAutomationDetachedValidatorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAITestValidateRequest Request;
	Request.Snapshot = HyperAIStudio::Automation::Tests::MakeValidSnapshot();
	Request.MaxIssues = 32;
	Request.DeadlineMs = 500;
	Request.MaxOutputBytes = 65536;
	const FHyperAITestValidateReport Valid =
		FHyperAIStudioAutomationContracts::Validate(Request);
	TestTrue(TEXT("Detached validator ran"), Valid.bOk);
	TestTrue(TEXT("Synthetic exact-name snapshot valid"), Valid.bValid);
	TestTrue(TEXT("Synthetic exact-name snapshot complete"), Valid.bComplete);

	Request.Snapshot.Tests[0].bRunnable = false;
	const FHyperAITestValidateReport Tampered =
		FHyperAIStudioAutomationContracts::Validate(Request);
	TestTrue(TEXT("Tampered validator ran"), Tampered.bOk);
	TestFalse(TEXT("Tampered snapshot rejected"), Tampered.bValid);
	TestTrue(TEXT("Tamper produces errors"), Tampered.ErrorCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationClosedIntentAndZeroEffectTest,
	"HyperAIStudio.NativeTools.Automation.ClosedIntentAndZeroEffect",
	HyperAIStudio::Automation::Tests::Flags)

bool FHyperAIStudioAutomationClosedIntentAndZeroEffectTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString Fingerprint;
	FString Error;
	TArray<FString> Names = {TEXT("HyperAIStudio.NativeTools.Sample")};
	TestTrue(TEXT("Closed exact name valid"),
		FHyperAIStudioAutomationContracts::ValidateTestNames(Names, Fingerprint, Error));
	const FString DuplicateName = Names[0];
	Names.Add(DuplicateName);
	TestFalse(TEXT("Duplicate exact name rejected"),
		FHyperAIStudioAutomationContracts::ValidateTestNames(Names, Fingerprint, Error));

	FHyperAIProfileCaptureRequest ProfileRequest;
	ProfileRequest.Categories = {TEXT("cpu"), TEXT("frame")};
	ProfileRequest.DurationMs = 1000;
	ProfileRequest.MaxCaptureBytes = 1024 * 1024;
	TestTrue(TEXT("Closed profile intent valid"),
		FHyperAIStudioAutomationContracts::ValidateProfileIntent(
			ProfileRequest, Fingerprint, Error));
	ProfileRequest.Categories.Add(TEXT("console"));
	TestFalse(TEXT("Arbitrary profile category rejected"),
		FHyperAIStudioAutomationContracts::ValidateProfileIntent(
			ProfileRequest, Fingerprint, Error));

	FHyperAIStudioAutomationDomainAdapter Adapter;
	FHyperAIAutomationTestRunPayload RunPayload;
	RunPayload.TestNames = {TEXT("HyperAIStudio.NativeTools.Sample")};
	RunPayload.CatalogFingerprint =
		HyperAIStudio::Automation::Tests::MakeValidSnapshot().CatalogFingerprint;
	RunPayload.SemanticFingerprint =
		FHyperAIStudioAutomationContracts::ComputeRunSemanticFingerprint(RunPayload);
	const FHyperAIStudioDomainAdapterResult RunResult = Adapter.Execute(
		HyperAIStudio::Automation::Tests::MakeContext(Adapter, TEXT("hyper_test_run"),
			FHyperAIStudioAutomationContracts::RunVariantId),
		RunPayload);
	TestEqual(TEXT("Run rejects before effect"), RunResult.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("Stable async run blocker"), RunResult.StatusCode,
		FString(FHyperAIStudioAutomationContracts::RunBackendBlocker));
	TestFalse(TEXT("Run returns no effect payload"), RunResult.Payload.IsValid());

	FHyperAIAutomationProfilePayload ProfilePayload;
	ProfilePayload.Categories = {TEXT("cpu")};
	ProfilePayload.DurationMs = 1000;
	ProfilePayload.MaxCaptureBytes = 1024 * 1024;
	FHyperAIProfileCaptureRequest FingerprintRequest;
	FingerprintRequest.Categories = ProfilePayload.Categories;
	FingerprintRequest.DurationMs = ProfilePayload.DurationMs;
	FingerprintRequest.MaxCaptureBytes = ProfilePayload.MaxCaptureBytes;
	FHyperAIStudioAutomationContracts::ValidateProfileIntent(
		FingerprintRequest, ProfilePayload.IntentFingerprint, Error);
	ProfilePayload.SemanticFingerprint =
		FHyperAIStudioAutomationContracts::ComputeProfileSemanticFingerprint(ProfilePayload);
	const FHyperAIStudioDomainAdapterResult ProfileResult = Adapter.Execute(
		HyperAIStudio::Automation::Tests::MakeContext(Adapter,
			TEXT("hyper_profile_capture"),
			FHyperAIStudioAutomationContracts::ProfileVariantId),
		ProfilePayload);
	TestEqual(TEXT("Profile rejects before effect"), ProfileResult.Outcome,
		EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("Stable async profile blocker"), ProfileResult.StatusCode,
		FString(FHyperAIStudioAutomationContracts::ProfileBackendBlocker));
	TestFalse(TEXT("Profile returns no effect payload"), ProfileResult.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAutomationBuildDiagnoseTest,
	"HyperAIStudio.NativeTools.Automation.BuildDiagnoseZeroEffect",
	HyperAIStudio::Automation::Tests::Flags)

bool FHyperAIStudioAutomationBuildDiagnoseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FHyperAIBuildDiagnoseRequest Request;
	const FHyperAIBuildDiagnoseReport Report =
		FHyperAIStudioAutomationContracts::DiagnoseBuild(Request);
	TestTrue(TEXT("Runtime context diagnosis succeeds"), Report.bOk);
	TestEqual(TEXT("Truthful runtime-only status"), Report.Status,
		FString(TEXT("runtime_context_only")));
	TestFalse(TEXT("No build evidence completeness claim"), Report.bBuildEvidenceComplete);
	TestFalse(TEXT("No trusted build result claim"), Report.bTrustedBuildResultAvailable);
	TestEqual(TEXT("Zero physical effects"), Report.PhysicalEffectCount, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
