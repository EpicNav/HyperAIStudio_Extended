// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"

namespace HyperAIStudio::CapabilityPacks::Tests
{
	const TCHAR* ValidSha256()
	{
		return TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	}

	FHyperAIStudioCapabilityCatalog MakeCatalog(
		EHyperAIStudioCapabilityAdmissionState Admission = EHyperAIStudioCapabilityAdmissionState::Admitted)
	{
		FHyperAIStudioCapabilityCatalog Catalog;
		Catalog.Schema = TEXT("test.catalog.v1");
		Catalog.GeneratedFingerprint = ValidSha256();
		Catalog.ApprovedPlanFingerprint = ValidSha256();
		Catalog.AdmissionMatrixFingerprint = ValidSha256();
		Catalog.SourceLedgerFingerprint = ValidSha256();
		Catalog.SourceArtifactFingerprint = ValidSha256();

		FHyperAIStudioCapabilityPrerequisiteDefinition& Plugin = Catalog.Prerequisites.AddDefaulted_GetRef();
		Plugin.Id = TEXT("plugin.test");
		Plugin.Kind = EHyperAIStudioCapabilityPrerequisiteKind::Plugin;
		FHyperAIStudioCapabilityPrerequisiteDefinition& Probe = Catalog.Prerequisites.AddDefaulted_GetRef();
		Probe.Id = TEXT("probe.test");
		Probe.Kind = EHyperAIStudioCapabilityPrerequisiteKind::Probe;

		FHyperAIStudioCapabilityPackDefinition& Pack = Catalog.Packs.AddDefaulted_GetRef();
		Pack.Id = TEXT("test_pack");
		Pack.Tier = EHyperAIStudioCapabilityPackTier::Core;
		Pack.AdmissionState = Admission;
		Pack.AtomicCohortIds.Add(TEXT("cohort.test.v1"));
		Pack.ToolNames.Add(TEXT("hyper_test_contract"));
		FHyperAIStudioCapabilityRequirementGroup& Requirement = Pack.Requirements.AddDefaulted_GetRef();
		Requirement.Id = TEXT("test_ready");
		Requirement.Mode = EHyperAIStudioCapabilityRequirementMode::AllOf;
		Requirement.PrerequisiteIds = { TEXT("plugin.test"), TEXT("probe.test") };

		FHyperAIStudioCapabilityToolDefinition& Tool = Catalog.Tools.AddDefaulted_GetRef();
		Tool.Name = TEXT("hyper_test_contract");
		Tool.PackId = Pack.Id;
		Tool.AtomicCohortId = TEXT("cohort.test.v1");
		Tool.AllowedSafetyClasses = {
			EHyperAIStudioCapabilitySafetyClass::Read,
			EHyperAIStudioCapabilitySafetyClass::Edit};
		Tool.AdmissionState = Admission;
		Tool.SourceArtifactFingerprint = ValidSha256();
		Tool.SourceLedgerFingerprint = ValidSha256();
		Tool.SourceLedgerCoverage = EHyperAIStudioCapabilityLedgerCoverage::ApprovedPlanExtension;
		Catalog.GeneratedFingerprint =
			FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(Catalog);
		return Catalog;
	}

	FHyperAIStudioCapabilityResolveInput ReadyInput(bool bReverseInsertion = false)
	{
		FHyperAIStudioCapabilityPrerequisiteObservation Plugin;
		Plugin.bPluginInstalled = true;
		Plugin.bPluginEnabled = true;
		FHyperAIStudioCapabilityPrerequisiteObservation Probe;
		Probe.bProbeSucceeded = true;
		FHyperAIStudioCapabilityResolveInput Input;
		if (bReverseInsertion)
		{
			Input.Observations.Add(TEXT("probe.test"), Probe);
			Input.Observations.Add(TEXT("plugin.test"), Plugin);
		}
		else
		{
			Input.Observations.Add(TEXT("plugin.test"), Plugin);
			Input.Observations.Add(TEXT("probe.test"), Probe);
		}
		return Input;
	}

	bool HasError(const TArray<FString>& Errors, const FString& Prefix)
	{
		return Errors.ContainsByPredicate([&Prefix](const FString& Error)
		{
			return Error.StartsWith(Prefix, ESearchCase::CaseSensitive);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCapabilityPackCatalogCoverageTest,
	"HyperAIStudio.NativeTools.CapabilityPacks.GeneratedCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCapabilityPackCatalogCoverageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::CapabilityPacks::Tests;
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	TArray<FString> Errors;
	const bool bCatalogValid = FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(Errors);
	TestTrue(TEXT("Generated catalog passes exact built-in validation"), bCatalogValid);
	for (const FString& Error : Errors)
	{
		AddError(TEXT("Catalog diagnostic: ") + Error);
	}
	TestEqual(TEXT("Validation produces no diagnostics"), Errors.Num(), 0);
	TestEqual(TEXT("Foundation plus 25 packs are present"), Catalog.Packs.Num(), 26);
	TestEqual(TEXT("Exactly 109 approved contracts are bound"), Catalog.Tools.Num(), 109);

	TSet<FString> ToolNames;
	TSet<FString> PackIds;
	int32 FoundationTools = 0;
	int32 SourceCandidates = 0;
	int32 ArtifactBoundSourceCandidates = 0;
	int32 SourceArtifactRows = 0;
	int32 Admitted = 0;
	int32 NativeReadCohort = 0;
	int32 LedgerBound = 0;
	int32 ExternalEffects = 0;
	for (const FHyperAIStudioCapabilityPackDefinition& Pack : Catalog.Packs)
	{
		TestFalse(TEXT("Pack IDs are unique"), PackIds.Contains(Pack.Id));
		PackIds.Add(Pack.Id);
		if (Pack.Id == TEXT("shared_foundation"))
		{
			FoundationTools = Pack.ToolNames.Num();
		}
	}
	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		TestFalse(TEXT("Tool wire names are unique"), ToolNames.Contains(Tool.Name));
		ToolNames.Add(Tool.Name);
		SourceCandidates += Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
		ArtifactBoundSourceCandidates +=
			Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate
			&& Tool.SourceArtifactCount > 0;
		SourceArtifactRows += Tool.SourceArtifactCount;
		Admitted += Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted;
		NativeReadCohort += Tool.AtomicCohortId == TEXT("cohort.source.hyperaistudionativereadtoolset.v1");
		LedgerBound += Tool.SourceLedgerRowCount > 0;
		ExternalEffects += Tool.bMayCauseExternalEffects;
		TestTrue(TEXT("Every generated tool has exact safety authority"),
			!Tool.AllowedSafetyClasses.IsEmpty());
		TestEqual(TEXT("External-effect disclosure matches exact safety authority"),
			Tool.bMayCauseExternalEffects,
			Tool.AllowedSafetyClasses.Contains(
				EHyperAIStudioCapabilitySafetyClass::ExternalEffect));
		TestEqual(TEXT("Aggregated evidence counts bind every mapped row"),
			Tool.SourceLedgerRowCount,
			Tool.PlatformEvidenceRows + Tool.ProductEvidenceRows
				+ Tool.SupplementalEvidenceRows);
	}
	TestEqual(TEXT("Shared foundation contains exactly five tools"), FoundationTools, 5);
	TestEqual(TEXT("All generated names remain unique"), ToolNames.Num(), 109);
	TestEqual(
		TEXT("Catalog source-artifact total matches the generated per-tool bindings"),
		Catalog.SourceArtifactCount,
		SourceArtifactRows);
	TestEqual(
		TEXT("Every current source candidate has a fingerprinted source artifact"),
		ArtifactBoundSourceCandidates,
		SourceCandidates);
	TestEqual(TEXT("No tool is falsely claimed admitted"), Admitted, 0);
	TestEqual(TEXT("Native-read class remains one atomic four-tool cohort"), NativeReadCohort, 4);
	TestTrue(TEXT("Clean-room ledger coverage is retained"), LedgerBound > 0);
	TestTrue(TEXT("External-effect contracts are explicitly flagged"), ExternalEffects > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCapabilityPackResolverTest,
	"HyperAIStudio.NativeTools.CapabilityPacks.Resolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCapabilityPackResolverTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::CapabilityPacks::Tests;
	const FHyperAIStudioCapabilityCatalog AdmittedCatalog = MakeCatalog();
	const FHyperAIStudioCapabilityResolution Ready =
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(AdmittedCatalog, ReadyInput());
	TestTrue(TEXT("Synthetic admitted catalog validates"), Ready.bCatalogValid);
	TestEqual(TEXT("Observed ready prerequisites resolve ready"), Ready.Packs[0].Status,
		EHyperAIStudioCapabilityPackStatus::Ready);
	TestEqual(TEXT("Ready wire status is stable"),
		FString(FHyperAIStudioCapabilityPackRegistry::LexToString(Ready.Packs[0].Status)), FString(TEXT("ready")));

	const FHyperAIStudioCapabilityResolution Reverse =
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(AdmittedCatalog, ReadyInput(true));
	TestEqual(TEXT("Observation map insertion order cannot change the fingerprint"),
		Ready.ResolutionFingerprint, Reverse.ResolutionFingerprint);

	FHyperAIStudioCapabilityResolveInput Disabled = ReadyInput();
	Disabled.Observations[TEXT("plugin.test")].bPluginEnabled = false;
	TestEqual(TEXT("Installed but disabled plugin resolves disabled"),
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(AdmittedCatalog, Disabled).Packs[0].Status,
		EHyperAIStudioCapabilityPackStatus::Disabled);

	FHyperAIStudioCapabilityResolveInput Unavailable = ReadyInput();
	Unavailable.Observations[TEXT("plugin.test")].bPluginInstalled = false;
	TestEqual(TEXT("Missing plugin resolves unavailable"),
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(AdmittedCatalog, Unavailable).Packs[0].Status,
		EHyperAIStudioCapabilityPackStatus::Unavailable);

	FHyperAIStudioCapabilityResolveInput Restart = ReadyInput();
	Restart.Observations[TEXT("plugin.test")].bRestartRequired = true;
	TestEqual(TEXT("Observed restart gate resolves restart_required"),
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(AdmittedCatalog, Restart).Packs[0].Status,
		EHyperAIStudioCapabilityPackStatus::RestartRequired);

	const FHyperAIStudioCapabilityCatalog PlannedCatalog =
		MakeCatalog(EHyperAIStudioCapabilityAdmissionState::Planned);
	TestEqual(TEXT("Ready prerequisites never bypass admission"),
		FHyperAIStudioCapabilityPackRegistry::ResolveCatalog(PlannedCatalog, ReadyInput()).Packs[0].Status,
		EHyperAIStudioCapabilityPackStatus::PendingAdmission);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCapabilityPackValidationTest,
	"HyperAIStudio.NativeTools.CapabilityPacks.FailClosedValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCapabilityPackValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::CapabilityPacks::Tests;
	TArray<FString> Errors;

	FHyperAIStudioCapabilityCatalog Duplicate = MakeCatalog();
	const FHyperAIStudioCapabilityToolDefinition DuplicateTool = Duplicate.Tools[0];
	Duplicate.Tools.Add(DuplicateTool);
	TestFalse(TEXT("Duplicate tool wire names fail closed"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(Duplicate, Errors));
	TestTrue(TEXT("Duplicate diagnostic is machine-readable"), HasError(Errors, TEXT("duplicate_or_empty_tool:")));

	FHyperAIStudioCapabilityCatalog UnknownPrerequisite = MakeCatalog();
	UnknownPrerequisite.Packs[0].Requirements[0].PrerequisiteIds.Add(TEXT("probe.unknown"));
	TestFalse(TEXT("Unknown prerequisite fails closed"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(UnknownPrerequisite, Errors));
	TestTrue(TEXT("Unknown prerequisite diagnostic is machine-readable"),
		HasError(Errors, TEXT("unknown_prerequisite:")));

	FHyperAIStudioCapabilityCatalog Cycle = MakeCatalog();
	FHyperAIStudioCapabilityPackDefinition SecondPack = Cycle.Packs[0];
	SecondPack.Id = TEXT("second_pack");
	SecondPack.ToolNames = { TEXT("hyper_second_contract") };
	SecondPack.AtomicCohortIds = { TEXT("cohort.second.v1") };
	SecondPack.DependsOnPackIds = { TEXT("test_pack") };
	Cycle.Packs[0].DependsOnPackIds = { TEXT("second_pack") };
	Cycle.Packs.Add(SecondPack);
	FHyperAIStudioCapabilityToolDefinition SecondTool = Cycle.Tools[0];
	SecondTool.Name = TEXT("hyper_second_contract");
	SecondTool.PackId = TEXT("second_pack");
	SecondTool.AtomicCohortId = TEXT("cohort.second.v1");
	Cycle.Tools.Add(SecondTool);
	TestFalse(TEXT("Pack dependency cycle fails closed"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(Cycle, Errors));
	TestTrue(TEXT("Cycle diagnostic is machine-readable"), HasError(Errors, TEXT("pack_dependency_cycle:")));

	FHyperAIStudioCapabilityCatalog MixedCohort = MakeCatalog();
	FHyperAIStudioCapabilityToolDefinition MixedTool = MixedCohort.Tools[0];
	MixedTool.Name = TEXT("hyper_mixed_contract");
	MixedTool.AdmissionState = EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
	MixedCohort.Tools.Add(MixedTool);
	MixedCohort.Packs[0].ToolNames.Add(MixedTool.Name);
	TestFalse(TEXT("Atomic cohort cannot mix admission states"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(MixedCohort, Errors));
	TestTrue(TEXT("Mixed-cohort diagnostic is machine-readable"),
		HasError(Errors, TEXT("mixed_atomic_cohort_admission:")));

	FHyperAIStudioCapabilityCatalog TamperedPrerequisite = MakeCatalog();
	TamperedPrerequisite.Prerequisites[0].Id = TEXT("plugin.tampered");
	TestFalse(TEXT("A stale fingerprint rejects a tampered prerequisite row"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(TamperedPrerequisite, Errors));
	TestTrue(TEXT("Tampered prerequisite reports the canonical fingerprint mismatch"),
		HasError(Errors, TEXT("catalog_fingerprint_mismatch")));

	FHyperAIStudioCapabilityCatalog TamperedTool = MakeCatalog();
	TamperedTool.Tools[0].SourceLedgerFingerprint =
		TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	TestFalse(TEXT("A stale fingerprint rejects a tampered tool evidence row"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(TamperedTool, Errors));
	TestTrue(TEXT("Tampered tool reports the canonical fingerprint mismatch"),
		HasError(Errors, TEXT("catalog_fingerprint_mismatch")));

	FHyperAIStudioCapabilityCatalog BlockingAuthority = MakeCatalog();
	BlockingAuthority.Tools[0].RequiredNonBlockingRequirementGroupIds.Add(TEXT("test_ready"));
	BlockingAuthority.GeneratedFingerprint =
		FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(BlockingAuthority);
	TestFalse(TEXT("A tool cannot bind a blocking group as limited-variant authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(BlockingAuthority, Errors));
	TestTrue(TEXT("Blocking authority failure is machine-readable"),
		HasError(Errors, TEXT("tool_nonblocking_requirement_invalid:")));

	FHyperAIStudioCapabilityCatalog MixedRequirements = MakeCatalog();
	MixedRequirements.Packs[0].Requirements[0].bBlocking = false;
	FHyperAIStudioCapabilityToolDefinition LimitedTool = MixedRequirements.Tools[0];
	LimitedTool.Name = TEXT("hyper_test_limited_contract");
	LimitedTool.RequiredNonBlockingRequirementGroupIds = { TEXT("test_ready") };
	MixedRequirements.Tools.Add(LimitedTool);
	MixedRequirements.Packs[0].ToolNames.Add(LimitedTool.Name);
	MixedRequirements.GeneratedFingerprint =
		FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(MixedRequirements);
	TestFalse(TEXT("One atomic cohort cannot mix generated nonblocking authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(MixedRequirements, Errors));
	TestTrue(TEXT("Mixed requirement authority failure is machine-readable"),
		HasError(Errors, TEXT("mixed_atomic_cohort_requirements:")));

	FHyperAIStudioCapabilityCatalog TamperedAuthority = MakeCatalog();
	TamperedAuthority.Tools[0].RequiredNonBlockingRequirementGroupIds.Add(TEXT("test_ready"));
	TestFalse(TEXT("A stale fingerprint rejects tampered nonblocking authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(TamperedAuthority, Errors));
	TestTrue(TEXT("Tampered nonblocking authority reports the canonical fingerprint mismatch"),
		HasError(Errors, TEXT("catalog_fingerprint_mismatch")));

	FHyperAIStudioCapabilityCatalog MissingSafety = MakeCatalog();
	MissingSafety.Tools[0].AllowedSafetyClasses.Reset();
	MissingSafety.GeneratedFingerprint =
		FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(MissingSafety);
	TestFalse(TEXT("A tool cannot omit generated safety authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(MissingSafety, Errors));
	TestTrue(TEXT("Missing safety authority is machine-readable"),
		HasError(Errors, TEXT("tool_safety_authority_missing:")));

	FHyperAIStudioCapabilityCatalog ExtraSafety = MakeCatalog();
	ExtraSafety.Tools[0].AllowedSafetyClasses.Add(
		EHyperAIStudioCapabilitySafetyClass::ExternalEffect);
	ExtraSafety.GeneratedFingerprint =
		FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(ExtraSafety);
	TestFalse(TEXT("External-effect safety cannot bypass its disclosure bit"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(ExtraSafety, Errors));
	TestTrue(TEXT("Extra external safety is machine-readable"),
		HasError(Errors, TEXT("tool_external_effect_safety_mismatch:")));

	FHyperAIStudioCapabilityCatalog TamperedSafety = MakeCatalog();
	TamperedSafety.Tools[0].AllowedSafetyClasses = {
		EHyperAIStudioCapabilitySafetyClass::Destructive};
	TestFalse(TEXT("A stale fingerprint rejects tampered safety authority"),
		FHyperAIStudioCapabilityPackRegistry::ValidateCatalog(TamperedSafety, Errors));
	TestTrue(TEXT("Tampered safety authority reports the canonical fingerprint mismatch"),
		HasError(Errors, TEXT("catalog_fingerprint_mismatch")));

	FHyperAIStudioCapabilityCatalog Reordered = MakeCatalog();
	Algo::Reverse(Reordered.Prerequisites);
	Algo::Reverse(Reordered.Packs[0].ToolNames);
	Algo::Reverse(Reordered.Packs[0].AtomicCohortIds);
	Algo::Reverse(Reordered.Packs[0].Requirements[0].PrerequisiteIds);
	Algo::Reverse(Reordered.Tools[0].AllowedSafetyClasses);
	TestEqual(TEXT("Set-like row order cannot change the canonical catalog fingerprint"),
		FHyperAIStudioCapabilityPackRegistry::ComputeCatalogFingerprint(Reordered),
		Reordered.GeneratedFingerprint);
	return true;
}

#endif
