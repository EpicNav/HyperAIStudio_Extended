// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioExtensionRuntime.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioExtensionRuntimeFacadeTest,
	"HyperAIStudio.NativeTools.ExtensionRuntime.FacadeBoundsAndAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioExtensionRuntimeFacadeTest::RunTest(const FString& Parameters)
{
	const TArray<FString> EnhancedInputCohort = {
		TEXT("hyper_input_inspect"),
		TEXT("hyper_input_apply_plan"),
		TEXT("hyper_input_validate")
	};
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("Facade resolves the exact generated optional cohort"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			TEXT("enhanced_input"),
			TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1"),
			EnhancedInputCohort,
			Admission));
	TestTrue(TEXT("Catalog validation remains inside core"), Admission.bCatalogValid);
	TestTrue(TEXT("No hidden fourth name is accepted"), Admission.bExactCohortMatch);
	TestTrue(TEXT("Facade reports the generated optional tier"), Admission.bOptionalPack);
	TestNotEqual(TEXT("A valid cohort has a concrete generated admission state"),
		Admission.State, EHyperAIStudioExtensionAdmissionState::Invalid);

	TArray<FString> IncompleteCohort = EnhancedInputCohort;
	IncompleteCohort.Pop();
	FHyperAIStudioExtensionCohortAdmission Rejected;
	TestFalse(TEXT("A partial cohort fails closed"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			TEXT("enhanced_input"),
			TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1"),
			IncompleteCohort,
			Rejected));

	const bool bProductionAllowed =
		FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
			TEXT("enhanced_input"),
			TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1"),
			EnhancedInputCohort,
			false);
	TestEqual(TEXT("Unreal MCP Only suppresses even admitted HyperAI cohorts"), bProductionAllowed,
		FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled()
			&& Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted);

	const bool bDevAllowed =
		FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
			TEXT("enhanced_input"),
			TEXT("cohort.source.hyperaistudioenhancedinputtoolset.v1"),
			EnhancedInputCohort,
			true);
	const bool bExpectedDevAllowed = FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled()
		&& (Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted
			|| (Admission.State == EHyperAIStudioExtensionAdmissionState::SourceCandidate
				&& FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled()));
	TestEqual(TEXT("Source candidate additionally requires Extended Hyper Tools or the legacy test override"),
		bDevAllowed, bExpectedDevAllowed);
	TestTrue(TEXT("Extended Hyper Tools enables source candidates without changing evidence metadata"),
		FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled(
			EHyperAIStudioNativeToolChannel::Preview, false));
	TestFalse(TEXT("Unreal MCP Only disables source candidates without an override"),
		FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled(
			EHyperAIStudioNativeToolChannel::StableOnly, false));
	TestTrue(TEXT("Legacy automation flag remains an explicit test override"),
		FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled(
			EHyperAIStudioNativeToolChannel::StableOnly, true));
	TestTrue(TEXT("Extended tool-set helper accepts the default product mode"),
		FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled(
			EHyperAIStudioNativeToolChannel::Preview, false));
	TestFalse(TEXT("Unreal MCP Only disables every HyperAI tool regardless of admission"),
		FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled(
			EHyperAIStudioNativeToolChannel::StableOnly, false));
	TestEqual(TEXT("Facade reports the configured native tool channel"),
		FHyperAIStudioExtensionRuntime::GetNativeToolChannel(),
		GetDefault<UHyperAIStudioSettings>()->NativeToolChannel);

	TestEqual(TEXT("Facade preserves the shared bounded SHA-256 contract"),
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("abc")),
		FString(TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	FString MalformedUtf16;
	MalformedUtf16.AppendChar(static_cast<TCHAR>(0xd800));
	TestTrue(TEXT("Malformed UTF-16 fails closed"),
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(MalformedUtf16).IsEmpty());
	TestTrue(TEXT("Facade preserves the journal operation-id grammar"),
		FHyperAIStudioExtensionRuntime::IsValidOperationId(TEXT("enhanced-input-001")));
	TestFalse(TEXT("Unsafe operation-id characters remain rejected"),
		FHyperAIStudioExtensionRuntime::IsValidOperationId(TEXT("enhanced/input")));

	TestEqual(TEXT("Facade reports the configured native execution mode"),
		FHyperAIStudioExtensionRuntime::GetNativeExecutionMode(),
		GetDefault<UHyperAIStudioSettings>()->NativeExecutionMode);
	TestFalse(TEXT("Fast reads avoid strict safety work"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::Fast, EHyperAIStudioDomainSafety::Read));
	TestFalse(TEXT("Fast reversible edits avoid strict safety work"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::Fast, EHyperAIStudioDomainSafety::Edit));
	TestTrue(TEXT("Destructive effects remain strict in Fast mode"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::Fast, EHyperAIStudioDomainSafety::Destructive));
	TestTrue(TEXT("External effects remain strict in Fast mode"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::Fast, EHyperAIStudioDomainSafety::ExternalEffect));
	TestTrue(TEXT("Strict Safety opts reads into deep safety work"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::StrictSafety, EHyperAIStudioDomainSafety::Read));
	TestTrue(TEXT("Strict Safety opts edits into deep safety work"),
		FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioNativeExecutionMode::StrictSafety, EHyperAIStudioDomainSafety::Edit));

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	TestEqual(TEXT("Current project gets the shared strong SHA-1 identity width"),
		ProjectId.Len(), 40);
	for (const TCHAR Character : ProjectId)
	{
		TestTrue(TEXT("Project identity is canonical lowercase hexadecimal"),
			(Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
