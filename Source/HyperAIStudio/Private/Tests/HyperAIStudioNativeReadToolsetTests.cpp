// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDependencyGraphToolset.h"
#include "HyperAIStudioNativeReadToolset.h"

#include "Dom/JsonObject.h"
#include "HyperAIStudioCapabilityCatalogCounts.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::NativeReadTools::Tests
{
	// Catalog sizes follow the generator. Every catalog tool is a source candidate with a dev
	// implementation today; change these only when a cohort's admission state changes.
	constexpr int32 CurrentCatalogToolCount = HyperAIStudio::CapabilityCatalog::GeneratedToolCount;
	constexpr int32 CurrentPlannedToolCount = 0;
	constexpr int32 CurrentSourceCandidateToolCount = CurrentCatalogToolCount;
	constexpr int32 CurrentAdmittedToolCount = 0;
	constexpr int32 CurrentCoreImplementedToolCount = 27;
	constexpr int32 CurrentDevImplementedToolCount = CurrentCatalogToolCount;
	constexpr int32 CurrentCoreToolsetCount = 15;
	// Runtime evidence, not catalog size: one per loaded optional toolset (texture_graph made it 41).
	constexpr int32 CurrentDevToolsetCount = 41;

	struct FExpectedTool
	{
		const TCHAR* Name;
		const UScriptStruct* ReturnStruct;
	};

	struct FExpectedManifestTool
	{
		const TCHAR* Name;
		const TCHAR* Toolset;
		FHyperAIStudioNativeToolManifestEntry::EAdmissionState AdmissionState;
	};

	const TArray<FExpectedTool>& ExpectedTools()
	{
		static const TArray<FExpectedTool> Tools = {
			{ TEXT("hyper_capability_report"), FHyperAICapabilityReport::StaticStruct() },
			{ TEXT("hyper_operation_status"), FHyperAIOperationStatus::StaticStruct() },
			{ TEXT("hyper_blueprint_debug_inspect"), FHyperAIBlueprintDebugReport::StaticStruct() },
			{ TEXT("hyper_pie_query"), FHyperAIPIEQueryReport::StaticStruct() }
		};
		return Tools;
	}

	const TArray<FExpectedManifestTool>& ExpectedManifestTools()
	{
		static const TArray<FExpectedManifestTool> Tools = {
			{ TEXT("hyper_capability_report"), TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset"), FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate },
			{ TEXT("hyper_operation_status"), TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset"), FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate },
			{ TEXT("hyper_blueprint_debug_inspect"), TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset"), FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate },
			{ TEXT("hyper_pie_query"), TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset"), FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate },
			{ TEXT("hyper_asset_dependency_graph"), TEXT("HyperAIStudio.HyperAIStudioDependencyGraphToolset"), FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate }
		};
		return Tools;
	}

	bool MutateReflectedRecordField(FProperty* Property, void* Record)
	{
		if (!Property || !Record)
		{
			return false;
		}
		void* Value = Property->ContainerPtrToValuePtr<void>(Record);
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(Value, StringProperty->GetPropertyValue(Value) + TEXT("_changed"));
			return true;
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(Value, !BoolProperty->GetPropertyValue(Value));
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(Value, NumericProperty->GetSignedIntPropertyValue(Value) + 1);
				return true;
			}
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(Value, NumericProperty->GetFloatingPointPropertyValue(Value) + 0.125);
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeReadContractsTest,
	"HyperAIStudio.NativeTools.NativeRead.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeReadContractsTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIStudioNativeToolManifestEntry>& Manifest =
		FHyperAIStudioNativeReadContracts::GetCallableManifest();
	const TArray<HyperAIStudio::NativeReadTools::Tests::FExpectedManifestTool>& Expected =
		HyperAIStudio::NativeReadTools::Tests::ExpectedManifestTools();
	TestEqual(TEXT("The native source manifest has exactly five source candidates"), Manifest.Num(), Expected.Num());

	TSet<FString> UniqueNames;
	for (int32 Index = 0; Index < Manifest.Num() && Index < Expected.Num(); ++Index)
	{
		TestEqual(TEXT("Manifest wire name is exact and ordered"), Manifest[Index].Name, FString(Expected[Index].Name));
		TestEqual(TEXT("Manifest toolset ownership is exact"), Manifest[Index].Toolset, FString(Expected[Index].Toolset));
		TestTrue(TEXT("Manifest admission is explicit and fail-closed"),
			Manifest[Index].AdmissionState == Expected[Index].AdmissionState);
		TestTrue(TEXT("Manifest wire names are unique"), !UniqueNames.Contains(Manifest[Index].Name));
		UniqueNames.Add(Manifest[Index].Name);
		TestFalse(TEXT("Every manifest entry declares safety"), Manifest[Index].Safety.IsEmpty());
		TestFalse(TEXT("Every manifest entry has a bounded description"), Manifest[Index].Description.IsEmpty());
		TestTrue(TEXT("Manifest descriptions stay bounded"), Manifest[Index].Description.Len() <= 256);
	}
	TestEqual(TEXT("Qualified native toolset name is stable"),
		FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudio.HyperAIStudioNativeReadToolset")));
	TestEqual(TEXT("Qualified dependency-graph toolset name is stable"),
		FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(),
		FString(TEXT("HyperAIStudio.HyperAIStudioDependencyGraphToolset")));
	TestEqual(TEXT("Operation status does not claim to be a pure read"), Manifest[1].Safety, FString(TEXT("reconciliation_read")));
	TestEqual(TEXT("Planned admission string is stable"),
		FHyperAIStudioNativeReadContracts::AdmissionStateToString(
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Planned),
		FString(TEXT("planned")));
	TestEqual(TEXT("Source-candidate admission string is stable"),
		FHyperAIStudioNativeReadContracts::AdmissionStateToString(
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate),
		FString(TEXT("source_candidate")));
	TestEqual(TEXT("Admitted admission string is stable"),
		FHyperAIStudioNativeReadContracts::AdmissionStateToString(
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted),
		FString(TEXT("admitted")));
	TestFalse(TEXT("Native source candidates are not registration-eligible by default"),
		FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(), false));
	TestFalse(TEXT("Dependency source candidate is not registration-eligible by default"),
		FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(), false));
	TestTrue(TEXT("Explicit dev-test gate can enable the native source candidates"),
		FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName(), true));
	TestTrue(TEXT("Explicit dev-test gate can enable the dependency source candidate"),
		FHyperAIStudioNativeReadContracts::IsToolsetRegistrationAllowed(
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName(), true));
	const TArray<FHyperAIStudioNativeToolManifestEntry> MixedCohort = {
		{ TEXT("admitted_member"), TEXT("HyperAIStudio.MixedCohort"), TEXT("read"), TEXT("test"),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted },
		{ TEXT("pending_member"), TEXT("HyperAIStudio.MixedCohort"), TEXT("read"), TEXT("test"),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::SourceCandidate }
	};
	TestFalse(TEXT("A mixed class-wide admission cohort fails closed in production"),
		FHyperAIStudioNativeReadContracts::IsAdmissionCohortRegistrationAllowed(
			MixedCohort, TEXT("HyperAIStudio.MixedCohort"), false));
	TestFalse(TEXT("A mixed class-wide admission cohort also fails closed under the dev-test gate"),
		FHyperAIStudioNativeReadContracts::IsAdmissionCohortRegistrationAllowed(
			MixedCohort, TEXT("HyperAIStudio.MixedCohort"), true));
	const TArray<FHyperAIStudioNativeToolManifestEntry> AdmittedCohort = {
		{ TEXT("member_a"), TEXT("HyperAIStudio.AdmittedCohort"), TEXT("read"), TEXT("test"),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted },
		{ TEXT("member_b"), TEXT("HyperAIStudio.AdmittedCohort"), TEXT("read"), TEXT("test"),
			FHyperAIStudioNativeToolManifestEntry::EAdmissionState::Admitted }
	};
	TestTrue(TEXT("A homogeneous admitted cohort is registration-eligible without a test flag"),
		FHyperAIStudioNativeReadContracts::IsAdmissionCohortRegistrationAllowed(
			AdmittedCohort, TEXT("HyperAIStudio.AdmittedCohort"), false));

	TestEqual(TEXT("Maximum page size is fixed"), FHyperAIStudioNativeReadContracts::MaxPageSize, 64);
	TestEqual(TEXT("Cursor input is bounded"), FHyperAIStudioNativeReadContracts::MaxCursorCharacters, 160);
	TestEqual(TEXT("Diagnostics are bounded"), FHyperAIStudioNativeReadContracts::MaxDiagnosticCharacters, 512);
	TestEqual(TEXT("Paths are bounded"), FHyperAIStudioNativeReadContracts::MaxPathCharacters, 1024);
	TestEqual(TEXT("Blueprint graph scan is bounded"), FHyperAIStudioNativeReadContracts::MaxBlueprintGraphs, 256);
	TestEqual(TEXT("Blueprint node scan is bounded"), FHyperAIStudioNativeReadContracts::MaxBlueprintNodes, 10000);
	TestEqual(TEXT("Blueprint debug output is bounded"), FHyperAIStudioNativeReadContracts::MaxBlueprintDebugItems, 2048);
	TestEqual(TEXT("Breakpoint scan is bounded"), FHyperAIStudioNativeReadContracts::MaxBlueprintBreakpoints, 512);
	TestEqual(TEXT("Watch scan is bounded"), FHyperAIStudioNativeReadContracts::MaxBlueprintWatches, 512);
	TestEqual(TEXT("PIE source scan is bounded"), FHyperAIStudioNativeReadContracts::MaxPieWorlds, 64);
	TestNull(TEXT("PIE response has no tick-stale continuation cursor"),
		FHyperAIPIEQueryReport::StaticStruct()->FindPropertyByName(TEXT("NextCursor")));
	TestNull(TEXT("PIE response has no paged offset"),
		FHyperAIPIEQueryReport::StaticStruct()->FindPropertyByName(TEXT("PageOffset")));
	TestNotNull(TEXT("PIE response retains the complete bounded world array"),
		FHyperAIPIEQueryReport::StaticStruct()->FindPropertyByName(TEXT("Worlds")));

	FString Diagnostic;
	TestTrue(TEXT("Smallest page is valid"), FHyperAIStudioNativeReadContracts::ValidatePageRequest(1, {}, Diagnostic));
	TestTrue(TEXT("Largest page is valid"), FHyperAIStudioNativeReadContracts::ValidatePageRequest(64, {}, Diagnostic));
	TestFalse(TEXT("Zero-sized page fails closed"), FHyperAIStudioNativeReadContracts::ValidatePageRequest(0, {}, Diagnostic));
	TestFalse(TEXT("Oversized page fails closed"), FHyperAIStudioNativeReadContracts::ValidatePageRequest(65, {}, Diagnostic));
	TestFalse(TEXT("Oversized cursor fails closed"), FHyperAIStudioNativeReadContracts::ValidatePageRequest(
		1,
		FString::ChrN(FHyperAIStudioNativeReadContracts::MaxCursorCharacters + 1, TEXT('x')),
		Diagnostic));

	const FString Fingerprint = FHyperAIStudioNativeReadContracts::HashTokens({ TEXT("asset"), TEXT("state") });
	TestTrue(TEXT("Snapshot fingerprints use a bounded lowercase SHA-1 token"),
		Fingerprint.StartsWith(TEXT("sha1:")) && Fingerprint.Len() == 45);
	const FString Cursor = FHyperAIStudioNativeReadContracts::MakeCursor(Fingerprint, 7);
	int32 Offset = INDEX_NONE;
	FString CursorStatus;
	FString CursorDiagnostic;
	TestTrue(TEXT("A cursor round-trips within the same snapshot"),
		FHyperAIStudioNativeReadContracts::ParseCursor(Cursor, Fingerprint, 10, Offset, CursorStatus, CursorDiagnostic));
	TestEqual(TEXT("Cursor preserves its offset"), Offset, 7);
	TestFalse(TEXT("A cursor fails closed against a different snapshot"),
		FHyperAIStudioNativeReadContracts::ParseCursor(Cursor, TEXT("sha1:different"), 10, Offset, CursorStatus, CursorDiagnostic));
	TestEqual(TEXT("Snapshot mismatch has a stable status"), CursorStatus, FString(TEXT("stale_cursor")));
	TestFalse(TEXT("A malformed cursor fails closed"),
		FHyperAIStudioNativeReadContracts::ParseCursor(TEXT("malformed"), Fingerprint, 10, Offset, CursorStatus, CursorDiagnostic));
	TestEqual(TEXT("Malformed cursor has a stable status"), CursorStatus, FString(TEXT("invalid_cursor")));

	bool bClipped = false;
	TestEqual(TEXT("Text clipping is deterministic"),
		FHyperAIStudioNativeReadContracts::ClipText(TEXT("abcdef"), 3, &bClipped),
		FString(TEXT("abc")));
	TestTrue(TEXT("Text clipping reports truncation"), bClipped);

	TestEqual(TEXT("Queued state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::Queued), FString(TEXT("queued")));
	TestEqual(TEXT("Running state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::Running), FString(TEXT("running")));
	TestEqual(TEXT("Commit-started state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::CommitStarted), FString(TEXT("commit_started")));
	TestEqual(TEXT("Completed state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::Completed), FString(TEXT("completed")));
	TestEqual(TEXT("Failed state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::Failed), FString(TEXT("failed")));
	TestEqual(TEXT("Cancel-requested state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::CancelRequested), FString(TEXT("cancel_requested")));
	TestEqual(TEXT("Rolled-back state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::RolledBack), FString(TEXT("rolled_back")));
	TestEqual(TEXT("Partial state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::Partial), FString(TEXT("partial")));
	TestEqual(TEXT("Unknown-outcome state mapping"), FHyperAIStudioNativeReadContracts::OperationStateToStatus(EHyperAIStudioOperationState::OutcomeUnknown), FString(TEXT("outcome_unknown")));
	TestEqual(TEXT("Not-needed rollback mapping"), FHyperAIStudioNativeReadContracts::RollbackStateToStatus(EHyperAIStudioRollbackState::NotNeeded), FString(TEXT("not_needed")));
	TestEqual(TEXT("Complete rollback mapping"), FHyperAIStudioNativeReadContracts::RollbackStateToStatus(EHyperAIStudioRollbackState::Complete), FString(TEXT("complete")));
	TestEqual(TEXT("Partial rollback mapping"), FHyperAIStudioNativeReadContracts::RollbackStateToStatus(EHyperAIStudioRollbackState::Partial), FString(TEXT("partial")));
	TestEqual(TEXT("Unsupported rollback mapping"), FHyperAIStudioNativeReadContracts::RollbackStateToStatus(EHyperAIStudioRollbackState::Unsupported), FString(TEXT("unsupported")));
	TestEqual(TEXT("Unknown rollback mapping"), FHyperAIStudioNativeReadContracts::RollbackStateToStatus(EHyperAIStudioRollbackState::Unknown), FString(TEXT("unknown")));
	TestEqual(TEXT("Concurrent journal ownership maps to busy"),
		FHyperAIStudioNativeReadContracts::ClassifyJournalLoadFailure(
			TEXT("Another HyperAIStudio journal owner is active for this project.")),
		FString(TEXT("busy")));
	TestEqual(TEXT("Other journal failures do not masquerade as contention"),
		FHyperAIStudioNativeReadContracts::ClassifyJournalLoadFailure(TEXT("corrupt generation")),
		FString(TEXT("journal_unavailable")));
	TestEqual(TEXT("Boolean watch fields use the bounded scalar policy"),
		FHyperAIStudioNativeReadContracts::ClassifyWatchPreviewProperty(
			FHyperAIBlueprintDebugItem::StaticStruct()->FindPropertyByName(TEXT("bEnabled"))),
		FString(TEXT("bounded_scalar")));
	TestEqual(TEXT("FString watch fields require a pre-length check"),
		FHyperAIStudioNativeReadContracts::ClassifyWatchPreviewProperty(
			FHyperAIBlueprintDebugItem::StaticStruct()->FindPropertyByName(TEXT("Message"))),
		FString(TEXT("bounded_string")));
	TestEqual(TEXT("Container watch fields are never exported"),
		FHyperAIStudioNativeReadContracts::ClassifyWatchPreviewProperty(
			FHyperAICapabilityReport::StaticStruct()->FindPropertyByName(TEXT("NativeTools"))),
		FString(TEXT("complex_or_unsupported")));

	const FHyperAICapabilityReport InvalidCapability =
		UHyperAIStudioNativeReadToolset::hyper_capability_report(0, {});
	TestEqual(TEXT("Capability input failure is structured"), InvalidCapability.Status, FString(TEXT("invalid_input")));
	TestFalse(TEXT("Capability input failure includes plain-language guidance"),
		InvalidCapability.DiagnosticSummary.IsEmpty());
	TestNotNull(TEXT("Capability report exposes the extended-tools product alias"),
		FHyperAICapabilityReport::StaticStruct()->FindPropertyByName(TEXT("bExtendedHyperToolsEnabled")));
	TestNotNull(TEXT("Capability report exposes the included-tools product alias"),
		FHyperAICapabilityReport::StaticStruct()->FindPropertyByName(TEXT("IncludedHyperAIToolCount")));
	TestNotNull(TEXT("Capability report retains the registered-tools count"),
		FHyperAICapabilityReport::StaticStruct()->FindPropertyByName(TEXT("RegisteredHyperAIToolCount")));
	TestNotNull(TEXT("Capability report exposes plain-language diagnostics"),
		FHyperAICapabilityReport::StaticStruct()->FindPropertyByName(TEXT("DiagnosticSummary")));
	TestNotNull(TEXT("Per-tool report exposes execution support"),
		FHyperAINativeToolSummary::StaticStruct()->FindPropertyByName(TEXT("ExecutionSupport")));
	const FHyperAIOperationStatus InvalidOperation =
		UHyperAIStudioNativeReadToolset::hyper_operation_status({});
	TestEqual(TEXT("Operation-id input failure is structured"), InvalidOperation.Status, FString(TEXT("invalid_operation_id")));
	TestFalse(TEXT("Operation status never reconciles during a read"), InvalidOperation.bJournalLoadMayReconcile);
	TestEqual(TEXT("Operation journal access mode is explicitly read-only"), InvalidOperation.JournalAccessMode, FString(TEXT("read_only_generation_snapshot")));
	const FHyperAIBlueprintDebugReport InvalidBlueprint =
		UHyperAIStudioNativeReadToolset::hyper_blueprint_debug_inspect({}, {}, 32, {});
	TestEqual(TEXT("Blueprint input failure is structured"), InvalidBlueprint.Status, FString(TEXT("invalid_input")));
	const FHyperAIBlueprintDebugReport UnloadedBlueprint =
		UHyperAIStudioNativeReadToolset::hyper_blueprint_debug_inspect(
			TEXT("/Game/__HyperAIStudioTests__/DefinitelyNotLoaded.DefinitelyNotLoaded"), {}, 32, {});
	TestEqual(TEXT("Blueprint inspection never synchronously loads an asset"),
		UnloadedBlueprint.Status, FString(TEXT("asset_not_loaded")));
	TestTrue(TEXT("Unloaded Blueprint remediation tells callers to open through Unreal/Epic"),
		UnloadedBlueprint.Diagnostic.Contains(TEXT("Epic"))
		&& UnloadedBlueprint.Diagnostic.Contains(TEXT("Open")));
	const FHyperAIPIEQueryReport InvalidPie =
		UHyperAIStudioNativeReadToolset::hyper_pie_query(-2);
	TestEqual(TEXT("PIE input failure is structured"), InvalidPie.Status, FString(TEXT("invalid_input")));

	FHyperAIStudioPIETopologyConfig CurrentTopology;
	CurrentTopology.PlayNetMode = TEXT("standalone");
	CurrentTopology.bRunUnderOneProcess = true;
	CurrentTopology.ClientCount = 1;
	FHyperAIStudioPIETopologyConfig FrozenTopology;
	FrozenTopology.PlayNetMode = TEXT("client");
	FrozenTopology.bRunUnderOneProcess = false;
	FrozenTopology.ClientCount = 3;
	FrozenTopology.bLaunchSeparateServer = true;
	FrozenTopology.bServerWasLaunched = true;
	FrozenTopology.bExternalSessionDestination = true;
	const FHyperAIStudioPIETopologyResolution ActiveTopology =
		FHyperAIStudioNativeReadContracts::ResolvePieTopology(
			true, true, FrozenTopology, CurrentTopology);
	TestTrue(TEXT("Active topology uses the frozen original request"), ActiveTopology.bValid);
	TestTrue(TEXT("Active topology records frozen-source selection"), ActiveTopology.bUsedFrozenActiveSession);
	TestEqual(TEXT("Active topology ignores a later safe current CDO"),
		ActiveTopology.Effective.PlayNetMode, FString(TEXT("client")));
	TestEqual(TEXT("Active topology retains the frozen client count"), ActiveTopology.Effective.ClientCount, 3);
	TestTrue(TEXT("Frozen external/server topology remains multiprocess"), ActiveTopology.bMultiprocess);
	const FHyperAIStudioPIETopologyResolution InactiveTopology =
		FHyperAIStudioNativeReadContracts::ResolvePieTopology(
			false, false, FrozenTopology, CurrentTopology);
	TestTrue(TEXT("Inactive topology uses current editor settings"), InactiveTopology.bValid);
	TestFalse(TEXT("Inactive current topology is single-process"), InactiveTopology.bMultiprocess);
	TestEqual(TEXT("Inactive topology source is explicit"),
		InactiveTopology.Source, FString(TEXT("current_editor_settings")));
	const FHyperAIStudioPIETopologyResolution MissingFrozenTopology =
		FHyperAIStudioNativeReadContracts::ResolvePieTopology(
			true, false, FrozenTopology, CurrentTopology);
	TestFalse(TEXT("Active session without frozen topology fails closed"), MissingFrozenTopology.bValid);
	TestEqual(TEXT("Missing frozen topology has a stable source status"),
		MissingFrozenTopology.Source, FString(TEXT("active_original_request_unavailable")));
	FHyperAIStudioPIETopologyConfig OneProcessListenServer;
	OneProcessListenServer.PlayNetMode = TEXT("listen_server");
	OneProcessListenServer.bRunUnderOneProcess = false;
	OneProcessListenServer.ClientCount = 1;
	const FHyperAIStudioPIETopologyResolution ObservableListenServer =
		FHyperAIStudioNativeReadContracts::ResolvePieTopology(
			true, true, OneProcessListenServer, CurrentTopology);
	TestTrue(TEXT("One in-editor listen-server instance has valid frozen topology"),
		ObservableListenServer.bValid);
	TestFalse(TEXT("One in-editor listen-server instance is not falsely classified multiprocess"),
		ObservableListenServer.bMultiprocess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeReadCursorFingerprintTest,
	"HyperAIStudio.NativeTools.NativeRead.CursorFingerprintCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeReadCursorFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeReadTools::Tests;

	FHyperAIBlueprintDebugReport BlueprintReport;
	BlueprintReport.AssetPath = TEXT("/Game/Test/BP_Test.BP_Test");
	BlueprintReport.AssetClass = TEXT("/Script/Engine.Blueprint");
	BlueprintReport.CompileStatus = TEXT("up_to_date");
	BlueprintReport.GeneratedClassPath = TEXT("/Game/Test/BP_Test.BP_Test_C");
	BlueprintReport.bHasDebuggingData = true;
	BlueprintReport.DebugObjectPath = TEXT("/Game/Test/UEDPIE_0_Test.Test:PersistentLevel.Actor");
	BlueprintReport.GraphFilter = TEXT("EventGraph");
	BlueprintReport.GraphCount = 4;
	BlueprintReport.ScannedGraphCount = 3;
	BlueprintReport.ScannedNodeCount = 20;
	BlueprintReport.BreakpointCount = 2;
	BlueprintReport.WatchCount = 3;
	BlueprintReport.WatchValuePolicy = TEXT("identity_and_bounded_scalar");
	BlueprintReport.DiagnosticNodeCount = 1;
	BlueprintReport.TotalItemCount = 1;
	BlueprintReport.bScanTruncated = true;
	BlueprintReport.DebugPieIdentity.bPresent = true;
	BlueprintReport.DebugPieIdentity.ContextHandle = TEXT("PIE_0");

	FHyperAIBlueprintDebugItem BlueprintItem;
	BlueprintItem.Kind = TEXT("watch");
	BlueprintItem.State = TEXT("valid");
	BlueprintItem.GraphName = TEXT("EventGraph");
	BlueprintItem.GraphPath = TEXT("/Game/Test/BP_Test.BP_Test:EventGraph");
	BlueprintItem.GraphGuid = TEXT("11111111-1111-1111-1111-111111111111");
	BlueprintItem.NodeGuid = TEXT("22222222-2222-2222-2222-222222222222");
	BlueprintItem.PinGuid = TEXT("33333333-3333-3333-3333-333333333333");
	BlueprintItem.NodeClass = TEXT("/Script/BlueprintGraph.K2Node_VariableGet");
	BlueprintItem.NodeTitle = TEXT("Get Health");
	BlueprintItem.PinName = TEXT("Health");
	BlueprintItem.PinDirection = TEXT("output");
	BlueprintItem.Severity = TEXT("info");
	BlueprintItem.Message = TEXT("watch value");
	BlueprintItem.bEnabled = true;
	BlueprintItem.bValid = true;
	BlueprintItem.PropertyPath = TEXT("Stats.Health");
	BlueprintItem.bPropertyPathTruncated = true;
	BlueprintItem.ValuePreview = TEXT("100");
	BlueprintItem.bValueTruncated = true;
	const TArray<FHyperAIBlueprintDebugItem> BlueprintItems = { BlueprintItem };
	const FString BlueprintFingerprint = FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
		BlueprintReport, 16, BlueprintItems);
	const FString BlueprintCursor = FHyperAIStudioNativeReadContracts::MakeCursor(BlueprintFingerprint, 0);
	int32 BlueprintFieldCount = 0;
	for (TFieldIterator<FProperty> PropertyIt(FHyperAIBlueprintDebugItem::StaticStruct()); PropertyIt; ++PropertyIt)
	{
		FHyperAIBlueprintDebugItem ChangedItem = BlueprintItem;
		FProperty* Property = *PropertyIt;
		TestTrue(*FString::Printf(TEXT("Blueprint record field %s has a supported test mutation"), *Property->GetName()),
			MutateReflectedRecordField(Property, &ChangedItem));
		const FString ChangedFingerprint = FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
			BlueprintReport, 16, { ChangedItem });
		TestNotEqual(*FString::Printf(TEXT("Blueprint cursor binds record field %s"), *Property->GetName()),
			ChangedFingerprint, BlueprintFingerprint);
		int32 Offset = INDEX_NONE;
		FString Status;
		FString Diagnostic;
		TestFalse(*FString::Printf(TEXT("Blueprint field %s change rejects the old cursor"), *Property->GetName()),
			FHyperAIStudioNativeReadContracts::ParseCursor(
				BlueprintCursor, ChangedFingerprint, 1, Offset, Status, Diagnostic));
		TestEqual(*FString::Printf(TEXT("Blueprint field %s change is stale_cursor"), *Property->GetName()),
			Status, FString(TEXT("stale_cursor")));
		++BlueprintFieldCount;
	}
	TestEqual(TEXT("Every reflected Blueprint item field was cursor-tested"), BlueprintFieldCount, 19);

	FHyperAIPIEDebugIdentity DebugPieIdentity;
	DebugPieIdentity.bPresent = true;
	DebugPieIdentity.ContextHandle = TEXT("PIE_0");
	DebugPieIdentity.PieInstance = 0;
	DebugPieIdentity.PackagePieInstance = 0;
	DebugPieIdentity.bPrimaryInstance = true;
	DebugPieIdentity.bDedicatedServer = false;
	DebugPieIdentity.WorldType = TEXT("pie");
	DebugPieIdentity.NetMode = TEXT("standalone");
	DebugPieIdentity.WorldPath = TEXT("/Game/Test/UEDPIE_0_Test.Test");
	DebugPieIdentity.WorldPackageName = TEXT("/Game/Test/UEDPIE_0_Test");
	DebugPieIdentity.SourcePackageName = TEXT("/Game/Test/Test");
	BlueprintReport.DebugPieIdentity = DebugPieIdentity;
	const FString BlueprintWithPieFingerprint =
		FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
			BlueprintReport, 16, BlueprintItems);
	const FString BlueprintWithPieCursor =
		FHyperAIStudioNativeReadContracts::MakeCursor(BlueprintWithPieFingerprint, 0);
	int32 PieIdentityFieldCount = 0;
	for (TFieldIterator<FProperty> PropertyIt(FHyperAIPIEDebugIdentity::StaticStruct()); PropertyIt; ++PropertyIt)
	{
		FHyperAIBlueprintDebugReport ChangedReport = BlueprintReport;
		FProperty* Property = *PropertyIt;
		TestTrue(*FString::Printf(TEXT("Blueprint debug PIE field %s has a supported test mutation"), *Property->GetName()),
			MutateReflectedRecordField(Property, &ChangedReport.DebugPieIdentity));
		const FString ChangedFingerprint =
			FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
				ChangedReport, 16, BlueprintItems);
		TestNotEqual(*FString::Printf(TEXT("Blueprint cursor binds debug PIE field %s"), *Property->GetName()),
			ChangedFingerprint, BlueprintWithPieFingerprint);
		int32 Offset = INDEX_NONE;
		FString Status;
		FString Diagnostic;
		TestFalse(*FString::Printf(TEXT("Blueprint debug PIE field %s change rejects the old cursor"), *Property->GetName()),
			FHyperAIStudioNativeReadContracts::ParseCursor(
				BlueprintWithPieCursor, ChangedFingerprint, 1, Offset, Status, Diagnostic));
		TestEqual(*FString::Printf(TEXT("Blueprint debug PIE field %s change is stale_cursor"), *Property->GetName()),
			Status, FString(TEXT("stale_cursor")));
		++PieIdentityFieldCount;
	}
	TestEqual(TEXT("Every reflected stable debug PIE identity field was cursor-tested"), PieIdentityFieldCount, 11);

	FHyperAIBlueprintDebugReport ChangedBlueprintCounts = BlueprintReport;
	++ChangedBlueprintCounts.ScannedNodeCount;
	const FString ChangedBlueprintCountsFingerprint =
		FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
			ChangedBlueprintCounts, 16, BlueprintItems);
	TestNotEqual(TEXT("Blueprint cursor binds scan counts"),
		ChangedBlueprintCountsFingerprint, BlueprintWithPieFingerprint);
	FHyperAIBlueprintDebugReport ChangedWatchPolicy = BlueprintReport;
	ChangedWatchPolicy.WatchValuePolicy = TEXT("unsafe_policy_change");
	const FString ChangedWatchPolicyFingerprint =
		FHyperAIStudioNativeReadContracts::MakeBlueprintDebugFingerprint(
			ChangedWatchPolicy, 16, BlueprintItems);
	TestNotEqual(TEXT("Blueprint cursor binds its watch-value safety policy"),
		ChangedWatchPolicyFingerprint, BlueprintWithPieFingerprint);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeReadSchemaRegistrationTest,
	"HyperAIStudio.NativeTools.NativeRead.SchemaAndRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeReadSchemaRegistrationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeReadTools::Tests;
	const UClass* ToolsetClass = UHyperAIStudioNativeReadToolset::StaticClass();
	const TArray<FExpectedTool>& Expected = ExpectedTools();
	TSet<FString> ReflectedCallableNames;

	for (TFieldIterator<UFunction> FunctionIt(ToolsetClass, EFieldIterationFlags::None); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		if (!Function || !Function->HasMetaData(TEXT("AICallable")))
		{
			continue;
		}
		ReflectedCallableNames.Add(Function->GetName());
		TestTrue(TEXT("Every callable is static"), Function->HasAllFunctionFlags(FUNC_Static));
		TObjectPtr<UFunction> FunctionObject = Function;
		const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(FunctionObject);
		TestTrue(TEXT("Epic ToolsetRegistry accepts each reflected callable"), Callability.HasValue());
		if (Callability.HasValue())
		{
			TestTrue(TEXT("Epic ToolsetRegistry marks each reflected callable as callable"), Callability.GetValue());
		}
	}
	TestEqual(TEXT("No unmanifested AICallable function is reflected"), ReflectedCallableNames.Num(), Expected.Num());

	TSet<FString> ExpectedQualifiedNames;
	for (const FExpectedTool& Tool : Expected)
	{
		UFunction* Function = ToolsetClass->FindFunctionByName(Tool.Name);
		TestNotNull(TEXT("Manifest function is reflected"), Function);
		if (!Function)
		{
			continue;
		}
		TestTrue(TEXT("Reflected manifest function is AICallable"), ReflectedCallableNames.Contains(Tool.Name));
		const FStructProperty* ReturnProperty = CastField<FStructProperty>(Function->GetReturnProperty());
		TestNotNull(TEXT("Callable has a reflected USTRUCT return DTO"), ReturnProperty);
		if (ReturnProperty)
		{
			TestTrue(TEXT("Callable return DTO is exact"), ReturnProperty->Struct == Tool.ReturnStruct);
			TestNotNull(TEXT("Every return DTO exposes status"), ReturnProperty->Struct->FindPropertyByName(TEXT("Status")));
			TestNotNull(TEXT("Every return DTO exposes diagnostic"), ReturnProperty->Struct->FindPropertyByName(TEXT("Diagnostic")));
		}
		ExpectedQualifiedNames.Add(FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName() + TEXT(".") + Tool.Name);
	}

	const FString JsonSchema = UToolsetRegistry::GetToolsetJsonSchema(UHyperAIStudioNativeReadToolset::StaticClass());
	TSharedPtr<FJsonObject> SchemaObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonSchema);
	TestTrue(TEXT("Epic ToolsetRegistry emits valid JSON schema"), FJsonSerializer::Deserialize(Reader, SchemaObject));
	TestTrue(TEXT("Epic ToolsetRegistry schema is an object"), SchemaObject.IsValid());
	if (SchemaObject.IsValid())
	{
		TestEqual(TEXT("Schema toolset name is exact"),
			SchemaObject->GetStringField(TEXT("name")),
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName());
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		TestTrue(TEXT("Schema contains a tools array"), SchemaObject->TryGetArrayField(TEXT("tools"), Tools));
		if (Tools)
		{
			TestEqual(TEXT("Schema contains exactly the reflected source-candidate tools"), Tools->Num(), Expected.Num());
			TSet<FString> ActualQualifiedNames;
			for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
			{
				const TSharedPtr<FJsonObject> ToolObject = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				TestTrue(TEXT("Every schema tool is an object"), ToolObject.IsValid());
				if (ToolObject.IsValid())
				{
					ActualQualifiedNames.Add(ToolObject->GetStringField(TEXT("name")));
				}
			}
			TestTrue(TEXT("Schema contains no planned or foreign HyperAI tools"), ActualQualifiedNames.Difference(ExpectedQualifiedNames).IsEmpty());
			TestTrue(TEXT("Schema contains every reflected source-candidate tool"), ExpectedQualifiedNames.Difference(ActualQualifiedNames).IsEmpty());
		}
	}

	UFunction* DependencyFunction = UHyperAIStudioDependencyGraphToolset::StaticClass()->FindFunctionByName(
		TEXT("hyper_asset_dependency_graph"));
	TestNotNull(TEXT("Dependency-graph callable is reflected"), DependencyFunction);
	if (DependencyFunction)
	{
		TestTrue(TEXT("Dependency-graph callable is static"), DependencyFunction->HasAllFunctionFlags(FUNC_Static));
		TestTrue(TEXT("Dependency-graph callable has AICallable metadata"), DependencyFunction->HasMetaData(TEXT("AICallable")));
		TObjectPtr<UFunction> FunctionObject = DependencyFunction;
		const TValueOrError<bool, FString> Callability = UToolsetDefinition::IsFunctionAICallable(FunctionObject);
		TestTrue(TEXT("Epic ToolsetRegistry accepts the dependency-graph callable"),
			Callability.HasValue() && Callability.GetValue());
		const FStructProperty* ReturnProperty = CastField<FStructProperty>(DependencyFunction->GetReturnProperty());
		TestNotNull(TEXT("Dependency-graph callable has a reflected USTRUCT return DTO"), ReturnProperty);
		if (ReturnProperty)
		{
			TestTrue(TEXT("Dependency-graph return DTO is exact"),
				ReturnProperty->Struct == FHyperAIStudioDependencyGraphResult::StaticStruct());
			TestNotNull(TEXT("Dependency-graph return DTO exposes status"),
				ReturnProperty->Struct->FindPropertyByName(TEXT("Status")));
			TestNotNull(TEXT("Dependency-graph return DTO exposes diagnostic records"),
				ReturnProperty->Struct->FindPropertyByName(TEXT("Diagnostics")));
		}
	}

	const FString DependencyJsonSchema =
		UToolsetRegistry::GetToolsetJsonSchema(UHyperAIStudioDependencyGraphToolset::StaticClass());
	TSharedPtr<FJsonObject> DependencySchemaObject;
	const TSharedRef<TJsonReader<>> DependencyReader = TJsonReaderFactory<>::Create(DependencyJsonSchema);
	TestTrue(TEXT("Dependency-graph ToolsetRegistry schema is valid JSON"),
		FJsonSerializer::Deserialize(DependencyReader, DependencySchemaObject));
	if (DependencySchemaObject.IsValid())
	{
		TestEqual(TEXT("Dependency-graph schema toolset name is exact"),
			DependencySchemaObject->GetStringField(TEXT("name")),
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName());
		const TArray<TSharedPtr<FJsonValue>>* DependencyTools = nullptr;
		TestTrue(TEXT("Dependency-graph schema contains a tools array"),
			DependencySchemaObject->TryGetArrayField(TEXT("tools"), DependencyTools));
		if (DependencyTools)
		{
			TestEqual(TEXT("Dependency-graph schema contains one reflected source-candidate tool"), DependencyTools->Num(), 1);
			if (DependencyTools->Num() == 1 && (*DependencyTools)[0].IsValid())
			{
				const TSharedPtr<FJsonObject> DependencyToolObject = (*DependencyTools)[0]->AsObject();
				TestTrue(TEXT("Dependency-graph schema tool is an object"), DependencyToolObject.IsValid());
				if (DependencyToolObject.IsValid())
				{
					TestEqual(TEXT("Dependency-graph wire name is exact"),
						DependencyToolObject->GetStringField(TEXT("name")),
						FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()
							+ TEXT(".hyper_asset_dependency_graph"));
				}
			}
		}
	}

	TestTrue(TEXT("ToolsetRegistry is available in the supported editor target"), UToolsetRegistry::IsAvailable());
	const bool bPendingTestGate = FHyperAIStudioNativeReadContracts::IsPendingNativeToolsTestEnabled();
	TestEqual(TEXT("Native source-candidate registration exactly follows the explicit dev-test gate"),
		FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioNativeReadToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetQualifiedToolsetName()),
		bPendingTestGate);
	TestEqual(TEXT("Dependency source-candidate registration exactly follows the explicit dev-test gate"),
		FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioDependencyGraphToolset::StaticClass(),
			FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()),
		bPendingTestGate);
	const FHyperAICapabilityReport Capability =
		UHyperAIStudioNativeReadToolset::hyper_capability_report(1, {});
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	TestTrue(TEXT("Capability runtime index binds every loaded implementation uniquely"),
		Capability.bRuntimeIndexValid);
	TestEqual(TEXT("Capability report exposes the complete generated catalog"),
		Capability.NativeTools.Num(), CurrentCatalogToolCount);
	TestEqual(TEXT("Capability report publishes the exact generated total"),
		Capability.TotalKnownHyperAIToolCount, CurrentCatalogToolCount);
	TestEqual(TEXT("Capability report counts exact planned contracts"),
		Capability.PlannedHyperAIToolCount, CurrentPlannedToolCount);
	TestEqual(TEXT("Capability report counts exact source candidates"),
		Capability.SourceCandidateHyperAIToolCount, CurrentSourceCandidateToolCount);
	TestEqual(TEXT("Capability report has no admitted entries before live proof"),
		Capability.AdmittedHyperAIToolCount, CurrentAdmittedToolCount);
	TestEqual(TEXT("Capability report is bound to the generated catalog fingerprint"),
		Capability.CapabilityCatalogFingerprint, Catalog.GeneratedFingerprint);
	TestEqual(TEXT("All three admission counts cover every generated contract"),
		Capability.PlannedHyperAIToolCount + Capability.SourceCandidateHyperAIToolCount
			+ Capability.AdmittedHyperAIToolCount,
		Capability.TotalKnownHyperAIToolCount);
	TestEqual(TEXT("Capability report mirrors the source-candidate activation policy"),
		Capability.bSourceCandidateToolsEnabled, bPendingTestGate);
	TestEqual(TEXT("Capability report exposes the configured product channel"),
		Capability.NativeToolChannel,
		FHyperAIStudioExtensionRuntime::GetNativeToolChannel()
			== EHyperAIStudioNativeToolChannel::Preview ? FString(TEXT("preview")) : FString(TEXT("stable_only")));
	TestEqual(TEXT("Capability report exposes the clear extended-tools alias"),
		Capability.bExtendedHyperToolsEnabled,
		FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled());
	TestEqual(TEXT("Included tool count follows the selected product tool set"),
		Capability.IncludedHyperAIToolCount,
		Capability.bExtendedHyperToolsEnabled ? CurrentCatalogToolCount : 0);
	TestFalse(TEXT("User-facing capability summary is populated"),
		Capability.DiagnosticSummary.IsEmpty());
	TestFalse(TEXT("User-facing capability summary hides Preview terminology"),
		Capability.DiagnosticSummary.Contains(TEXT("Preview"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("User-facing capability summary hides Stable terminology"),
		Capability.DiagnosticSummary.Contains(TEXT("Stable"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("User-facing capability summary hides admission terminology"),
		Capability.DiagnosticSummary.Contains(TEXT("SourceCandidate"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("Capability report advertises only registration/admission-authorized callables"),
		Capability.CallableHyperAIToolCount,
		bPendingTestGate ? CurrentSourceCandidateToolCount + CurrentAdmittedToolCount
			: CurrentAdmittedToolCount);
	TestEqual(TEXT("Core implementations are always indexed and the optional cohort only when loaded"),
		Capability.RuntimeImplementedHyperAIToolCount,
		bPendingTestGate ? CurrentDevImplementedToolCount : CurrentCoreImplementedToolCount);
	TestEqual(TEXT("Loaded runtime toolsets are exact"),
		Capability.LoadedHyperAIToolsetCount,
		bPendingTestGate ? CurrentDevToolsetCount : CurrentCoreToolsetCount);
	TestEqual(TEXT("Only dev-enabled source-candidate toolsets are registered"),
		Capability.RegisteredHyperAIToolsetCount,
		bPendingTestGate ? CurrentDevToolsetCount : 0);
	TestEqual(TEXT("Only dev-enabled source-candidate tools are registered"),
		Capability.RegisteredHyperAIToolCount,
		bPendingTestGate ? CurrentDevImplementedToolCount : 0);
	TestEqual(TEXT("Expected registration only includes loaded admission-authorized implementations"),
		Capability.ExpectedRegisteredHyperAIToolCount,
		bPendingTestGate ? CurrentDevImplementedToolCount : 0);
	TestEqual(TEXT("The live capability cohort has no registration-state mismatch"),
		Capability.RegistrationMismatchCount, 0);
	TestTrue(TEXT("Current loaded/unloaded cohorts produce a valid capability report"),
		Capability.bOk);
	FString InvalidPublicationError;
	TestFalse(TEXT("Runtime index rejects a forged qualified toolset identity"),
		FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioNativeReadToolset::StaticClass(),
			TEXT("HyperAIStudio.ForgedNativeReadToolset"),
			InvalidPublicationError));
	TestEqual(TEXT("Forged runtime publication fails with a stable code"),
		InvalidPublicationError,
		FString(TEXT("runtime_toolset_qualified_name_mismatch")));
	TestEqual(TEXT("Capability report retains the versioned project identity meaning"),
		Capability.ProjectIdentityScheme,
		FString(TEXT("hyperai.canonical-project-directory-id.v1")));
	if (Capability.ProjectIdentityHash.IsEmpty())
	{
		TestEqual(TEXT("Unavailable identity has an explicit algorithm state"),
			Capability.ProjectIdentityHashAlgorithm, FString(TEXT("unavailable")));
	}
	else
	{
		TestEqual(TEXT("Current journal project identity is explicitly SHA-1"),
			Capability.ProjectIdentityHashAlgorithm, FString(TEXT("sha1")));
		TestEqual(TEXT("Legacy SHA-1 project identity is explicit rather than bare"),
			Capability.ProjectIdentityStatus, FString(TEXT("available_legacy_sha1")));
	}

	TSet<FString> ReportNames;
	const TSet<FString> ExpectedPlanOnlyNames = {
		TEXT("hyper_actor_modifier_apply_plan"), TEXT("hyper_animation_apply_plan"),
		TEXT("hyper_audio_apply_plan"), TEXT("hyper_character_apply_plan"),
		TEXT("hyper_cinematics_apply_plan"), TEXT("hyper_dynamic_material_apply_plan"),
		TEXT("hyper_game_framework_apply_plan"), TEXT("hyper_gameplay_ai_apply_plan"),
		TEXT("hyper_gameplay_systems_apply_plan"), TEXT("hyper_gas_apply_plan"),
		TEXT("hyper_geometry_apply_plan"), TEXT("hyper_interchange_apply_plan"),
		TEXT("hyper_live_production_apply_plan"), TEXT("hyper_material_apply_plan"),
		TEXT("hyper_navigation_apply_plan"), TEXT("hyper_network_apply_plan"),
		TEXT("hyper_niagara_apply_plan"), TEXT("hyper_pcg_apply_plan"),
		TEXT("hyper_physics_apply_plan"), TEXT("hyper_property_animation_apply_plan"),
		TEXT("hyper_scene_apply_plan"), TEXT("hyper_ui_apply_plan"),
		TEXT("hyper_worldbuilding_apply_plan")
	};
	const TSet<FString> ExpectedLimitedDirectEditNames = {
		TEXT("hyper_data_apply_plan"),
		TEXT("hyper_input_apply_plan"),
		TEXT("hyper_paper2d_apply_plan")
	};
	const TSet<FString> AllowedExecutionSupport = {
		TEXT("Read"), TEXT("Validate"), TEXT("PlanOnly"),
		TEXT("LimitedDirectEdit"), TEXT("OperationSpecific")
	};
	int32 ReportedPlanOnlyTools = 0;
	int32 ReportedLimitedDirectEditTools = 0;
	int32 ReportedLoadedImplementations = 0;
	int32 ReportedRegisteredTools = 0;
	for (const FHyperAINativeToolSummary& Tool : Capability.NativeTools)
	{
		TestFalse(TEXT("Generated capability report contains no duplicate names"),
			ReportNames.Contains(Tool.Name));
		ReportNames.Add(Tool.Name);
		const FHyperAIStudioCapabilityToolDefinition* CatalogTool = Catalog.Tools.FindByPredicate(
			[&Tool](const FHyperAIStudioCapabilityToolDefinition& Candidate)
			{
				return Candidate.Name == Tool.Name;
			});
		TestNotNull(TEXT("Every reported name comes from the generated catalog"), CatalogTool);
		if (CatalogTool)
		{
			TestEqual(TEXT("Reported pack id comes from generated catalog"),
				Tool.PackId, CatalogTool->PackId);
			TestEqual(TEXT("Reported cohort id comes from generated catalog"),
				Tool.AtomicCohortId, CatalogTool->AtomicCohortId);
			TestEqual(TEXT("Reported source-artifact count comes from generated catalog"),
				Tool.SourceArtifactCount, CatalogTool->SourceArtifactCount);
		}
		TestTrue(TEXT("Every report description is bounded"), Tool.Description.Len() <= 256);
		TestTrue(TEXT("Every qualified runtime toolset is bounded"), Tool.Toolset.Len() <= 256);
		TestTrue(TEXT("External-effect policy is explicit and never aliases a safety class"),
			Tool.ExternalEffectPolicy == TEXT("may_cause_external_effects")
				|| Tool.ExternalEffectPolicy == TEXT("no_external_effect_flag"));
		TestTrue(TEXT("Every tool has a supported plain-language execution level"),
			AllowedExecutionSupport.Contains(Tool.ExecutionSupport));
		if (Tool.ExecutionSupport == TEXT("PlanOnly"))
		{
			++ReportedPlanOnlyTools;
			TestTrue(TEXT("Only audited plan-only apply tools use PlanOnly"),
				ExpectedPlanOnlyNames.Contains(Tool.Name));
		}
		if (Tool.ExecutionSupport == TEXT("LimitedDirectEdit"))
		{
			++ReportedLimitedDirectEditTools;
			TestTrue(TEXT("Only audited limited edit tools use LimitedDirectEdit"),
				ExpectedLimitedDirectEditNames.Contains(Tool.Name));
		}
		ReportedLoadedImplementations += Tool.bImplementationLoaded ? 1 : 0;
		ReportedRegisteredTools += Tool.bToolsetRegistered ? 1 : 0;
		if (Tool.bCallable)
		{
			TestTrue(TEXT("Every callable row passes Epic's exact per-tool runtime filter"),
				Tool.bRuntimeEnabled);
		}
		if (Tool.AdmissionState == TEXT("planned"))
		{
			TestFalse(TEXT("Planned contracts are never callable"), Tool.bCallable);
			TestFalse(TEXT("Planned contracts have no loaded source implementation"),
				Tool.bImplementationLoaded);
		}
	}
	TestEqual(TEXT("Every generated catalog name appears exactly once"),
		ReportNames.Num(), Catalog.Tools.Num());
	TestEqual(TEXT("Exactly 23 audited apply tools are plan-only"),
		ReportedPlanOnlyTools, ExpectedPlanOnlyNames.Num());
	TestEqual(TEXT("Exactly three audited apply tools have limited direct editing"),
		ReportedLimitedDirectEditTools, ExpectedLimitedDirectEditNames.Num());
	TestEqual(TEXT("Implementation aggregate equals per-tool runtime state"),
		Capability.RuntimeImplementedHyperAIToolCount, ReportedLoadedImplementations);
	TestEqual(TEXT("Registration aggregate equals per-tool runtime state"),
		Capability.RegisteredHyperAIToolCount, ReportedRegisteredTools);
	TestTrue(TEXT("Runtime diagnostics are hard bounded"),
		Capability.RuntimeIndexDiagnostics.Num()
			<= FHyperAIStudioCapabilityRuntimeIndex::MaxDiagnostics);
	TestTrue(TEXT("Catalog output is hard bounded"),
		Capability.NativeTools.Num()
			<= FHyperAIStudioNativeReadContracts::MaxCatalogToolCount);
	TestTrue(TEXT("Epic inventory page remains bounded by requested size"),
		Capability.ReturnedEpicToolsetCount <= 1);
	if (bPendingTestGate)
	{
		TestTrue(TEXT("Test-enabled capability report sees dependency registration"),
			Capability.bDependencyGraphToolsetRegistered);
		TestTrue(TEXT("Preview-enabled dependency tool is explicitly labeled callable_preview"),
			Capability.CallableHyperAITools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.Name == TEXT("hyper_asset_dependency_graph")
					&& Tool.Toolset == FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()
					&& Tool.Availability == TEXT("callable_preview")
					&& Tool.bCallable;
			}));
		TestFalse(TEXT("No source candidate remains non-callable in the full dev cohort"),
			Capability.NativeTools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.AdmissionState == TEXT("source_candidate") && !Tool.bCallable;
			}));
		TestTrue(TEXT("Loaded optional Enhanced Input cohort is reported callable"),
			Capability.NativeTools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.Name == TEXT("hyper_input_inspect")
					&& Tool.Toolset == TEXT("HyperAIStudioEnhancedInput.HyperAIStudioEnhancedInputToolset")
					&& Tool.bImplementationLoaded
					&& Tool.bToolsetRegistered
					&& Tool.bCallable
					&& Tool.Availability == TEXT("callable_preview");
			}));
	}
	else
	{
		TestFalse(TEXT("Default capability report does not see native registration"),
			Capability.bNativeToolsetRegistered);
		TestFalse(TEXT("Default capability report does not see dependency registration"),
			Capability.bDependencyGraphToolsetRegistered);
		TestEqual(TEXT("Stable-only capability report remains fail-closed"),
			Capability.Status, FString(TEXT("source_candidates_stable_only")));
		TestTrue(TEXT("Stable-only source candidates remain visibly non-callable"),
			Capability.NativeTools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.Name == TEXT("hyper_asset_dependency_graph")
					&& Tool.Toolset == FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()
					&& Tool.Availability == TEXT("source_candidate_stable_only")
					&& !Tool.bCallable;
			}));
		TestFalse(TEXT("Default report exposes no callable source candidate"),
			Capability.NativeTools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.AdmissionState == TEXT("source_candidate") && Tool.bCallable;
			}));
		TestTrue(TEXT("Default report distinguishes the unloaded optional module"),
			Capability.NativeTools.ContainsByPredicate([](const FHyperAINativeToolSummary& Tool)
			{
				return Tool.Name == TEXT("hyper_input_inspect")
					&& Tool.PackId == TEXT("enhanced_input")
					&& !Tool.bImplementationLoaded
					&& !Tool.bToolsetRegistered
					&& !Tool.bCallable
					&& Tool.Availability == TEXT("source_candidate_implementation_unloaded");
			}));
	}

	FHyperAINativeToolSummary UnavailableOptionalCandidate;
	UnavailableOptionalCandidate.Name = TEXT("hyper_optional_fixture_inspect");
	UnavailableOptionalCandidate.AdmissionState = TEXT("source_candidate");
	UnavailableOptionalCandidate.bImplementationLoaded = false;
	UnavailableOptionalCandidate.bToolsetRegistered = false;
	UnavailableOptionalCandidate.bCallable = false;
	const TArray<FHyperAINativeToolSummary> UnavailableOptionalTools = {
		UnavailableOptionalCandidate
	};
	int32 FixtureExpectedRegisteredCount = INDEX_NONE;
	TestFalse(TEXT("A dev-authorized but unloaded optional source candidate is not expected to register"),
		FHyperAIStudioNativeReadContracts::IsCapabilityRegistrationExpected(
			UnavailableOptionalCandidate,
			true));
	int32 FixtureRegistrationMismatchCount = INDEX_NONE;
	TestTrue(TEXT("An unavailable optional source candidate leaves the runtime report valid"),
		FHyperAIStudioNativeReadContracts::AreCapabilityRuntimeRowsConsistent(
			UnavailableOptionalTools,
			true,
			FixtureExpectedRegisteredCount,
			FixtureRegistrationMismatchCount));
	TestEqual(TEXT("The unavailable optional fixture contributes no expected registration"),
		FixtureExpectedRegisteredCount, 0);
	TestEqual(TEXT("The unavailable optional fixture contributes no registration mismatch"),
		FixtureRegistrationMismatchCount, 0);
	TestFalse(TEXT("The unavailable optional fixture remains non-callable"),
		UnavailableOptionalCandidate.bCallable);

	FHyperAINativeToolSummary FilteredCandidate;
	FilteredCandidate.Name = TEXT("hyper_filtered_fixture_inspect");
	FilteredCandidate.AdmissionState = TEXT("source_candidate");
	FilteredCandidate.bImplementationLoaded = true;
	FilteredCandidate.bToolsetRegistered = true;
	FilteredCandidate.bRuntimeEnabled = false;
	FilteredCandidate.bCallable = false;
	int32 FilteredExpectedRegisteredCount = INDEX_NONE;
	int32 FilteredMismatchCount = INDEX_NONE;
	TestTrue(TEXT("A registered but Epic-filtered tool remains a consistent non-callable row"),
		FHyperAIStudioNativeReadContracts::AreCapabilityRuntimeRowsConsistent(
			{ FilteredCandidate }, true, FilteredExpectedRegisteredCount, FilteredMismatchCount));
	TestEqual(TEXT("Filtering does not misreport class registration"), FilteredExpectedRegisteredCount, 1);
	TestEqual(TEXT("Filtering is not a registration mismatch"), FilteredMismatchCount, 0);

	if (bPendingTestGate)
	{
		auto RegistrySubsystem = UToolsetRegistrySubsystem::Get();
		TestTrue(TEXT("Epic registry subsystem is available for the exact filter fixture"),
			RegistrySubsystem.HasValue());
		if (RegistrySubsystem.HasValue())
		{
			const FString QualifiedTool =
				FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName()
				+ TEXT(".hyper_asset_dependency_graph");
			RegistrySubsystem.GetValue()->ToolsetRegistry.AddBlockedName(QualifiedTool);
			const FHyperAICapabilityReport FilteredReport =
				UHyperAIStudioNativeReadToolset::hyper_capability_report(1, {});
			RegistrySubsystem.GetValue()->ToolsetRegistry.RemoveBlockedName(QualifiedTool);
			TestTrue(TEXT("a legitimate Epic per-tool filter keeps the report internally valid"),
				FilteredReport.bOk);
			TestTrue(TEXT("a legitimate Epic per-tool filter is not reported as an integrity error"),
				FilteredReport.Status != TEXT("capability_report_inconsistent")
					&& FilteredReport.Status != TEXT("runtime_index_invalid")
					&& FilteredReport.Status != TEXT("registration_unavailable"));
			const FHyperAINativeToolSummary* FilteredTool = FilteredReport.NativeTools.FindByPredicate(
				[](const FHyperAINativeToolSummary& Tool)
				{
					return Tool.Name == TEXT("hyper_asset_dependency_graph");
				});
			TestNotNull(TEXT("filtered dependency tool remains visible as a catalog row"), FilteredTool);
			if (FilteredTool)
			{
				TestTrue(TEXT("filtered toolset remains registered"), FilteredTool->bToolsetRegistered);
				TestFalse(TEXT("Epic per-tool block is reflected"), FilteredTool->bRuntimeEnabled);
				TestFalse(TEXT("Epic-filtered tool is never reported callable"), FilteredTool->bCallable);
				TestEqual(TEXT("filter state is machine-readable"), FilteredTool->Availability,
					FString(TEXT("tool_filtered_or_disabled")));
			}

			const FString QualifiedToolset =
				FHyperAIStudioNativeReadContracts::GetDependencyGraphQualifiedToolsetName();
			RegistrySubsystem.GetValue()->ToolsetRegistry.AddBlockedName(QualifiedToolset);
			FString FilteredIdempotencyError;
			TestTrue(TEXT("an existing owner remains idempotent after later Epic filtering"),
				FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
					UHyperAIStudioDependencyGraphToolset::StaticClass(),
					QualifiedToolset,
					FilteredIdempotencyError));
			const FHyperAICapabilityReport DisabledToolsetReport =
				UHyperAIStudioNativeReadToolset::hyper_capability_report(1, {});
			const FHyperAINativeToolSummary* DisabledToolsetRow =
				DisabledToolsetReport.NativeTools.FindByPredicate(
					[](const FHyperAINativeToolSummary& Tool)
					{
						return Tool.Name == TEXT("hyper_asset_dependency_graph");
					});
			TestTrue(TEXT("a user-disabled whole toolset keeps the report internally valid"),
				DisabledToolsetReport.bOk);
			TestTrue(TEXT("top-level dependency registration uses actual presence, not filtered lookup"),
				DisabledToolsetReport.bDependencyGraphToolsetRegistered);
			TestNotNull(TEXT("disabled toolset remains an actually registered runtime row"),
				DisabledToolsetRow);
			if (DisabledToolsetRow)
			{
				TestTrue(TEXT("whole-toolset filtering does not masquerade as unregistration"),
					DisabledToolsetRow->bToolsetRegistered);
				TestFalse(TEXT("whole-toolset filtering disables exact callability"),
					DisabledToolsetRow->bRuntimeEnabled);
				TestFalse(TEXT("disabled whole toolset is never callable"),
					DisabledToolsetRow->bCallable);
				TestEqual(TEXT("whole-toolset filter state is machine-readable"),
					DisabledToolsetRow->Availability,
					FString(TEXT("tool_filtered_or_disabled")));
			}

			FString UnregisterError;
			const bool bUnregisteredForPreexistingFilter =
				FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
					UHyperAIStudioDependencyGraphToolset::StaticClass(),
					QualifiedToolset,
					UnregisterError);
			RegistrySubsystem.GetValue()->ToolsetRegistry.RemoveBlockedName(QualifiedToolset);
			TestTrue(TEXT("owner fixture can remove its captured handler while the whole toolset is hidden"),
				bUnregisteredForPreexistingFilter);
			TestFalse(TEXT("removing the filter after hidden unregistration exposes no handler"),
				RegistrySubsystem.GetValue()->ToolsetRegistry.Find(QualifiedToolset).IsValid());
			if (bUnregisteredForPreexistingFilter)
			{
				RegistrySubsystem.GetValue()->ToolsetRegistry.AddBlockedName(QualifiedToolset);
				bool bUnexpectedRegistryChange = false;
				const FDelegateHandle ChangeProbe =
					RegistrySubsystem.GetValue()->ToolsetRegistry.OnToolsetRegistered().AddLambda(
						[&bUnexpectedRegistryChange]()
						{
							bUnexpectedRegistryChange = true;
						});
				FString FilteredRegistrationError;
				const bool bRegisteredWhilePreFiltered =
					FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
						UHyperAIStudioDependencyGraphToolset::StaticClass(),
						QualifiedToolset,
						FilteredRegistrationError);
				RegistrySubsystem.GetValue()->ToolsetRegistry.OnToolsetRegistered().Remove(ChangeProbe);
				TestFalse(TEXT("a preexisting Epic filter is rejected before registration"),
					bRegisteredWhilePreFiltered);
				TestEqual(TEXT("preexisting-filter rejection is machine-readable"),
					FilteredRegistrationError,
					FString(TEXT("owned_registration_requires_unfiltered_registry")));
				TestFalse(TEXT("preexisting-filter rejection emits no registry mutation"),
					bUnexpectedRegistryChange);
				TestFalse(TEXT("preexisting-filter rejection creates no owner record"),
					FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
						UHyperAIStudioDependencyGraphToolset::StaticClass(),
						QualifiedToolset));
				RegistrySubsystem.GetValue()->ToolsetRegistry.RemoveBlockedName(QualifiedToolset);
				auto UnexpectedHandler =
					RegistrySubsystem.GetValue()->ToolsetRegistry.Find(QualifiedToolset);
				TestFalse(TEXT("removing the preexisting filter exposes no hidden ghost registration"),
					UnexpectedHandler.IsValid());
				if (UnexpectedHandler.IsValid())
				{
					RegistrySubsystem.GetValue()->ToolsetRegistry.UnregisterToolset(UnexpectedHandler);
					}
					FString RestoreError;
					TestTrue(TEXT("owner fixture restores the dependency toolset after filter proof"),
						FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
							UHyperAIStudioDependencyGraphToolset::StaticClass(),
							QualifiedToolset,
							RestoreError));
					const TSharedPtr<UE::ToolsetRegistry::FToolset> RestoredHandler =
						RegistrySubsystem.GetValue()->ToolsetRegistry.Find(QualifiedToolset);
					TestTrue(TEXT("every successful owner registration captures an exact removable handler"),
						RestoredHandler.IsValid()
							&& RestoredHandler->GetToolsetClass()
								== UHyperAIStudioDependencyGraphToolset::StaticClass());
				}
		}
	}
	return true;
}

#endif
