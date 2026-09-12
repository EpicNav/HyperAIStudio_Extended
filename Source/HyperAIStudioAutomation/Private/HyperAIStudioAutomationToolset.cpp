// Games by Hyper 2026.

#include "HyperAIStudioAutomationToolset.h"

#include "Engine/Engine.h"
#include "HAL/PlatformProperties.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IAutomationControllerManager.h"
#include "IAutomationControllerModule.h"
#include "IAutomationReport.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAutomation, Log, All);

namespace HyperAIStudio::Automation::Private
{
	constexpr int64 BaseReportBytes = 6144;
	constexpr int64 RecordBytes = 768;
	constexpr int64 IssueBytes = 512;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += LexToString(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("|");
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("1") : TEXT("0");
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ')) return true;
		}
		return false;
	}

	bool IsSafeTestNameCharacter(const TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TEXT('.') || Character == TEXT('_')
			|| Character == TEXT('-') || Character == TEXT('/') || Character == TEXT(' ')
			|| Character == TEXT(':') || Character == TEXT('[') || Character == TEXT(']')
			|| Character == TEXT('(') || Character == TEXT(')');
	}

	bool IsSafeTestName(const FString& Name)
	{
		if (Name.IsEmpty()
			|| Name.Len() > FHyperAIStudioAutomationContracts::MaxTestNameCharacters
			|| Name.TrimStartAndEnd() != Name || HasControlCharacter(Name)) return false;
		for (const TCHAR Character : Name)
		{
			if (!IsSafeTestNameCharacter(Character)) return false;
		}
		return true;
	}

	void AddIssue(TArray<FHyperAIAutomationIssue>& Issues, const TCHAR* Code,
		const TCHAR* Severity, const FString& StableId, const TCHAR* Detail)
	{
		if (Issues.Num() >= FHyperAIStudioAutomationContracts::MaxIssues) return;
		FHyperAIAutomationIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(
			FHyperAIStudioAutomationContracts::MaxTestNameCharacters);
		Issue.Detail = FString(Detail).Left(512);
	}

	FString ControllerStateToken(const EAutomationControllerModuleState::Type State)
	{
		switch (State)
		{
		case EAutomationControllerModuleState::Ready: return TEXT("Ready");
		case EAutomationControllerModuleState::Running: return TEXT("Running");
		default: return TEXT("Disabled");
		}
	}

	FString BuildConfigurationToken()
	{
#if UE_BUILD_DEBUG
		return TEXT("Debug");
	#elif UE_BUILD_TEST
		return TEXT("Test");
#elif UE_BUILD_SHIPPING
		return TEXT("Shipping");
#else
		return TEXT("Development");
#endif
	}

	FString AuthorityFingerprint(const TArray<FHyperAIAutomationAuthorityRow>& Rows)
	{
		FString Canonical(TEXT("hyperai.automation.authority.v1|"));
		AppendToken(Canonical, FHyperAIStudioAutomationContracts::NativeAccessReviewId);
		AppendToken(Canonical,
			FHyperAIStudioAutomationContracts::NativeAccessReviewRecordsSha256);
		AppendToken(Canonical, FHyperAIStudioAutomationContracts::PythonAccessReviewId);
		AppendToken(Canonical,
			FHyperAIStudioAutomationContracts::PythonAccessReviewRecordsSha256);
		for (const FHyperAIAutomationAuthorityRow& Row : Rows)
		{
			AppendToken(Canonical, Row.Source);
			AppendToken(Canonical, Row.SourceId);
			AppendToken(Canonical, Row.Lifecycle);
			AppendToken(Canonical, Row.Access);
			AppendToken(Canonical, Row.Disposition);
			AppendToken(Canonical, Row.DirectRoute);
			AppendToken(Canonical, Row.FrozenCoverage);
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool DeadlineExceeded(const double Deadline)
	{
		return FPlatformTime::Seconds() > Deadline;
	}

	bool CaptureExactNames(
		const TArray<FString>& SortedNames,
		const int32 MaxVisits,
		const int32 DeadlineMs,
		FHyperAIAutomationDetachedSnapshot& OutSnapshot,
		TArray<FHyperAIAutomationIssue>& OutIssues)
	{
		OutSnapshot = {};
		FString NameError;
		if (!FHyperAIStudioAutomationContracts::ValidateTestNames(
			SortedNames, OutSnapshot.RequestedNamesFingerprint, NameError))
		{
			AddIssue(OutIssues, TEXT("invalid_test_names"), TEXT("error"), FString(),
				TEXT("Exact requested test names failed their closed value contract."));
			return false;
		}
		OutSnapshot.Tests.Reserve(SortedNames.Num());
		TMap<FString, int32> IndexByName;
		for (const FString& Name : SortedNames)
		{
			FHyperAIAutomationTestRecord& Record = OutSnapshot.Tests.AddDefaulted_GetRef();
			Record.TestName = Name;
			IndexByName.Add(Name, OutSnapshot.Tests.Num() - 1);
		}

		IAutomationControllerModule* Module =
			FModuleManager::GetModulePtr<IAutomationControllerModule>(TEXT("AutomationController"));
		OutSnapshot.bControllerLoaded = Module != nullptr;
		if (!Module)
		{
			OutSnapshot.ControllerState = TEXT("not_loaded");
			AddIssue(OutIssues, TEXT("automation_controller_not_loaded"), TEXT("error"),
				FString(), TEXT("The AutomationController module is not already loaded; HyperAI never loads it from a tool call."));
			for (FHyperAIAutomationTestRecord& Record : OutSnapshot.Tests)
			{
				Record.Fingerprint =
					FHyperAIStudioAutomationContracts::ComputeTestRecordFingerprint(Record);
			}
			OutSnapshot.CatalogFingerprint =
				FHyperAIStudioAutomationContracts::ComputeCatalogFingerprint(OutSnapshot.Tests);
			OutSnapshot.ObservationFingerprint =
				FHyperAIStudioAutomationContracts::ComputeObservationFingerprint(OutSnapshot);
			return false;
		}

		IAutomationControllerManagerRef Controller = Module->GetAutomationController();
		OutSnapshot.ControllerState = ControllerStateToken(Controller->GetTestState());
		OutSnapshot.bControllerReady = Controller->IsReadyForTests();
		OutSnapshot.bResultsAvailable = Controller->CheckTestResultsAvailable();
		OutSnapshot.bReportsHaveErrors = Controller->ReportsHaveErrors();
		OutSnapshot.bReportsHaveWarnings = Controller->ReportsHaveWarnings();
		OutSnapshot.bReportsHaveLogs = Controller->ReportsHaveLogs();
		OutSnapshot.EnabledTestCount = Controller->GetEnabledTestsNum();
		OutSnapshot.NumPasses = Controller->GetNumPasses();
		OutSnapshot.DeviceClusterCount = Controller->GetNumDeviceClusters();

		TArray<TSharedPtr<IAutomationReport>>& Roots = Controller->GetFilteredReports();
		TArray<TSharedPtr<IAutomationReport>> Stack;
		Stack.Reserve(FMath::Min(MaxVisits, 128));
		bool bTraversalComplete = true;
		for (int32 Index = Roots.Num() - 1; Index >= 0; --Index)
		{
			if (Stack.Num() >= MaxVisits)
			{
				bTraversalComplete = false;
				break;
			}
			Stack.Add(Roots[Index]);
		}
		const double Deadline = FPlatformTime::Seconds()
			+ static_cast<double>(DeadlineMs) / 1000.0;
		while (!Stack.IsEmpty())
		{
			if (OutSnapshot.VisitedReportCount >= MaxVisits || DeadlineExceeded(Deadline))
			{
				bTraversalComplete = false;
				break;
			}
			TSharedPtr<IAutomationReport> Report = Stack.Pop(EAllowShrinking::No);
			++OutSnapshot.VisitedReportCount;
			if (!Report.IsValid())
			{
				bTraversalComplete = false;
				continue;
			}
			if (Report->IsParent())
			{
				TArray<TSharedPtr<IAutomationReport>>& Children = Report->GetFilteredChildren();
				for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
				{
					if (OutSnapshot.VisitedReportCount + Stack.Num() >= MaxVisits)
					{
						bTraversalComplete = false;
						break;
					}
					Stack.Add(Children[Index]);
				}
				continue;
			}
			const FString& FullPath = Report->GetFullTestPath();
			if (FullPath.Len() > FHyperAIStudioAutomationContracts::MaxTestNameCharacters)
			{
				bTraversalComplete = false;
				continue;
			}
			if (const int32* RecordIndex = IndexByName.Find(FullPath))
			{
				FHyperAIAutomationTestRecord& Record = OutSnapshot.Tests[*RecordIndex];
				Record.bPresent = true;
				Record.bRunnable = Controller->IsTestRunnable(Report);
			}
		}
		if (DeadlineExceeded(Deadline))
		{
			AddIssue(OutIssues, TEXT("report_traversal_deadline"), TEXT("error"),
				FString(), TEXT("Bounded report-tree traversal exceeded its monotonic deadline."));
		}
		if (!bTraversalComplete)
		{
			AddIssue(OutIssues, TEXT("report_traversal_incomplete"), TEXT("error"),
				FString(), TEXT("The current filtered report tree exceeded the visit/deadline bound or contained an invalid node."));
		}
		bool bAllFoundRunnable = true;
		for (FHyperAIAutomationTestRecord& Record : OutSnapshot.Tests)
		{
			if (!Record.bPresent || !Record.bRunnable)
			{
				bAllFoundRunnable = false;
				AddIssue(OutIssues, Record.bPresent ? TEXT("test_not_runnable")
					: TEXT("test_not_present"), TEXT("error"), Record.TestName,
					Record.bPresent ? TEXT("The exact current report is not runnable.")
					: TEXT("The exact test name is absent from the current filtered report tree."));
			}
			Record.Fingerprint =
				FHyperAIStudioAutomationContracts::ComputeTestRecordFingerprint(Record);
		}
		OutSnapshot.bTraversalComplete = bTraversalComplete;
		OutSnapshot.bSnapshotComplete = bTraversalComplete
			&& OutSnapshot.bControllerReady && !Roots.IsEmpty() && bAllFoundRunnable;
		OutSnapshot.CatalogFingerprint =
			FHyperAIStudioAutomationContracts::ComputeCatalogFingerprint(OutSnapshot.Tests);
		OutSnapshot.ObservationFingerprint =
			FHyperAIStudioAutomationContracts::ComputeObservationFingerprint(OutSnapshot);
		return OutSnapshot.bSnapshotComplete;
	}

	bool ValidateSnapshotValue(
		const FHyperAIAutomationDetachedSnapshot& Snapshot,
		const int32 MaxIssues,
		const int32 MaxOutputBytes,
		const int32 DeadlineMs,
		FHyperAITestValidateReport& OutReport)
	{
		OutReport = {};
		if (Snapshot.Tests.IsEmpty()
			|| Snapshot.Tests.Num() > FHyperAIStudioAutomationContracts::MaxTestNames
			|| Snapshot.Tests.GetAllocatedSize()
				> FHyperAIStudioAutomationContracts::MaxContainerAllocatedBytes)
		{
			OutReport.Status = TEXT("invalid_detached_envelope");
			OutReport.Diagnostic = TEXT("Detached test records are empty or exceed storage/count bounds.");
			return false;
		}
		for (const FHyperAIAutomationTestRecord& Record : Snapshot.Tests)
		{
			if (!IsSafeTestName(Record.TestName) || Record.Fingerprint.Len() > 71)
			{
				OutReport.Status = TEXT("detached_nested_bound");
				OutReport.Diagnostic = TEXT("A detached test record exceeded nested bounds before materialization.");
				return false;
			}
		}
		int64 UsedBytes = BaseReportBytes;
		auto Issue = [&](const TCHAR* Code, const TCHAR* Severity,
			const FString& Id, const TCHAR* Detail)
		{
			if (FCString::Strcmp(Severity, TEXT("error")) == 0) ++OutReport.ErrorCount;
			else ++OutReport.WarningCount;
			if (OutReport.Issues.Num() >= MaxIssues
				|| UsedBytes + IssueBytes > MaxOutputBytes)
			{
				OutReport.bTruncated = true;
				return;
			}
			FHyperAIAutomationIssue& Added = OutReport.Issues.AddDefaulted_GetRef();
			Added.Code = Code;
			Added.Severity = Severity;
			Added.StableId = Id.Left(
				FHyperAIStudioAutomationContracts::MaxTestNameCharacters);
			Added.Detail = FString(Detail).Left(512);
			UsedBytes += IssueBytes;
		};

		const double Deadline = FPlatformTime::Seconds()
			+ static_cast<double>(DeadlineMs) / 1000.0;
		TArray<FString> Names;
		Names.Reserve(Snapshot.Tests.Num());
		FString Previous;
		bool bComplete = Snapshot.bSnapshotComplete && Snapshot.bTraversalComplete
			&& Snapshot.bControllerLoaded && Snapshot.bControllerReady;
		for (const FHyperAIAutomationTestRecord& Record : Snapshot.Tests)
		{
			if (DeadlineExceeded(Deadline))
			{
				bComplete = false;
				Issue(TEXT("validator_deadline"), TEXT("error"), Record.TestName,
					TEXT("Detached validation exceeded its monotonic deadline."));
				break;
			}
			if (Record.TestName <= Previous || !Record.bPresent || !Record.bRunnable)
			{
				bComplete = false;
				Issue(TEXT("test_record_invalid"), TEXT("error"), Record.TestName,
					TEXT("Test records must be strictly ordered, present, and runnable."));
			}
			Previous = Record.TestName;
			Names.Add(Record.TestName);
			if (Record.Fingerprint !=
				FHyperAIStudioAutomationContracts::ComputeTestRecordFingerprint(Record))
			{
				bComplete = false;
				Issue(TEXT("test_record_seal_mismatch"), TEXT("error"), Record.TestName,
					TEXT("A detached per-test value seal drifted."));
			}
		}
		FString NameError;
		if (!FHyperAIStudioAutomationContracts::ValidateTestNames(
			Names, OutReport.RecomputedRequestedNamesFingerprint, NameError))
		{
			bComplete = false;
			Issue(TEXT("requested_names_invalid"), TEXT("error"), FString(),
				TEXT("Detached exact test names failed canonical validation."));
		}
		OutReport.RecomputedCatalogFingerprint =
			FHyperAIStudioAutomationContracts::ComputeCatalogFingerprint(Snapshot.Tests);
		OutReport.RecomputedObservationFingerprint =
			FHyperAIStudioAutomationContracts::ComputeObservationFingerprint(Snapshot);
		if (OutReport.RecomputedRequestedNamesFingerprint
				!= Snapshot.RequestedNamesFingerprint
			|| OutReport.RecomputedCatalogFingerprint != Snapshot.CatalogFingerprint
			|| OutReport.RecomputedObservationFingerprint != Snapshot.ObservationFingerprint)
		{
			bComplete = false;
			Issue(TEXT("snapshot_seal_mismatch"), TEXT("error"), FString(),
				TEXT("Detached requested-name, catalog, or controller-observation seal drifted."));
		}
		FString Canonical(TEXT("hyperai.automation.validator.v1|"));
		AppendToken(Canonical, OutReport.RecomputedRequestedNamesFingerprint);
		AppendToken(Canonical, OutReport.RecomputedCatalogFingerprint);
		AppendToken(Canonical, OutReport.RecomputedObservationFingerprint);
		AppendToken(Canonical, LexToString(OutReport.ErrorCount));
		OutReport.ValidatorFingerprint =
			FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		OutReport.bOk = true;
		OutReport.bComplete = bComplete && !OutReport.bTruncated;
		OutReport.bValid = OutReport.bComplete && OutReport.ErrorCount == 0;
		OutReport.Status = OutReport.bValid ? TEXT("valid") : TEXT("invalid");
		OutReport.Diagnostic = OutReport.bValid
			? TEXT("Detached exact-name automation snapshot and all value seals are valid.")
			: TEXT("Detached automation snapshot is partial, unavailable, stale, tampered, or truncated.");
		return OutReport.bValid;
	}

	bool PrepareExternalIntent(
		const FString& ToolName,
		const FString& VariantId,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		const FString& EffectTarget,
		const int32 DeadlineMs,
		const int32 NativeOperationBudget,
		const int32 MaxOutputBytes,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioAutomationContracts::PackId;
		Binding.ToolName = ToolName;
		Binding.VariantId = VariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
		Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		Binding.ExpectedAdapterFingerprint =
			FHyperAIStudioAutomationContracts::GetAdapterDescriptor().AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = FHyperAIStudioAutomationContracts::PackId;
		Binding.Prerequisites.bPackEnabled = false;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Binding.Prerequisites);
		Binding.Admission.PackId = FHyperAIStudioAutomationContracts::PackId;
		Binding.Admission.bPackAdmitted = false;
		Binding.Admission.bExternalEffectAdmitted = false;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
				Binding.Admission);

		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = MoveTemp(Binding);
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = EffectTarget;
		Contract.DeadlineMs = DeadlineMs;
		Contract.MaxNativeOperations = NativeOperationBudget;
		Contract.MaxGameThreadMs = FMath::Min(200, DeadlineMs);
		Contract.MaxOutputBytes = MaxOutputBytes;
		Contract.MaxResultBytes = 512;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = false;
		Contract.bSaveOnce = false;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(
			Contract, OutPrepared, OutError);
	}
}

FString FHyperAIStudioAutomationContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAutomation.HyperAIStudioAutomationToolset");
}

const TArray<FHyperAIStudioAutomationManifestEntry>&
FHyperAIStudioAutomationContracts::GetManifest()
{
	static const TArray<FHyperAIStudioAutomationManifestEntry> Manifest = {
		{TEXT("hyper_test_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_test_run"), GetQualifiedToolsetName()},
		{TEXT("hyper_test_validate"), GetQualifiedToolsetName()},
		{TEXT("hyper_profile_capture"), GetQualifiedToolsetName()},
		{TEXT("hyper_build_diagnose"), GetQualifiedToolsetName()}};
	return Manifest;
}

const TArray<FHyperAIAutomationAuthorityRow>&
FHyperAIStudioAutomationContracts::GetAuthorityRows()
{
	static const TArray<FHyperAIAutomationAuthorityRow> Rows = {
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:39:DiscoverTests"), TEXT("discover"), TEXT("external_effect"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.DiscoverTests"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:47:ListTests"), TEXT("discover"), TEXT("read"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.ListTests"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:54:RunTests"), TEXT("runtime"), TEXT("external_effect"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.RunTests"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:72:RunTestsByFilter"), TEXT("runtime"), TEXT("external_effect"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.RunTestsByFilter"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:78:GetTestResults"), TEXT("inspect"), TEXT("read"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.GetTestResults"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:85:GetTestStatus"), TEXT("inspect"), TEXT("read"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.GetTestStatus"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("AutomationTestToolset:Source/AutomationTestToolset/Public/AutomationTestToolset.h:91:StopTests"), TEXT("runtime"), TEXT("external_effect"), TEXT("epic_delegate"), TEXT("AutomationTestToolset.AutomationTestToolset.StopTests"), TEXT("exact_delegate")},
		{TEXT("epic"), TEXT("EditorToolset:Content/Python/editor_toolset/toolsets/programmatic.py:887:get_execution_environment"), TEXT("inspect"), TEXT("read"), TEXT("epic_delegate"), TEXT("EditorToolset.ProgrammaticToolset.get_execution_environment"), TEXT("baseline_delegate")},
		{TEXT("epic"), TEXT("EditorToolset:Content/Python/editor_toolset/toolsets/programmatic.py:906:execute_tool_script"), TEXT("runtime"), TEXT("external_effect"), TEXT("epic_delegate"), TEXT("EditorToolset.ProgrammaticToolset.execute_tool_script"), TEXT("baseline_only_no_script_surface")},
		{TEXT("legacy_hyperai"), TEXT("cook_project"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("hyper_build_diagnose"), TEXT("runtime_context_only_no_cook")},
		{TEXT("legacy_hyperai"), TEXT("export_cook_build_audit"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("hyper_build_diagnose"), TEXT("no_file_export")},
		{TEXT("legacy_hyperai"), TEXT("get_cook_status"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("hyper_build_diagnose"), TEXT("trusted_result_store_required")},
		{TEXT("legacy_hyperai"), TEXT("get_last_build_result"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("hyper_build_diagnose"), TEXT("trusted_result_store_required")},
		{TEXT("legacy_hyperai"), TEXT("get_package_size"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("hyper_build_diagnose"), TEXT("no_filesystem_measurement")},
		{TEXT("legacy_hyperai"), TEXT("get_test_results"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("AutomationTestToolset.AutomationTestToolset.GetTestResults"), TEXT("epic_delegate")},
		{TEXT("legacy_hyperai"), TEXT("profile_runtime"), TEXT("runtime"), TEXT("edit"), TEXT("hyperai_add"), TEXT("hyper_profile_capture"), TEXT("closed_intent_only")},
		{TEXT("legacy_hyperai"), TEXT("run_automation_test"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("AutomationTestToolset.AutomationTestToolset.RunTests"), TEXT("epic_delegate")},
		{TEXT("legacy_hyperai"), TEXT("run_test"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("AutomationTestToolset.AutomationTestToolset.RunTests"), TEXT("epic_delegate")},
		{TEXT("legacy_hyperai"), TEXT("run_test_suite"), TEXT("inspect"), TEXT("read"), TEXT("hyperai_add"), TEXT("AutomationTestToolset.AutomationTestToolset.RunTestsByFilter"), TEXT("epic_delegate")}};
	return Rows;
}

const TArray<FString>& FHyperAIStudioAutomationContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("AutomationTestToolset.AutomationTestToolset.DiscoverTests"),
		TEXT("AutomationTestToolset.AutomationTestToolset.ListTests"),
		TEXT("AutomationTestToolset.AutomationTestToolset.RunTests"),
		TEXT("AutomationTestToolset.AutomationTestToolset.RunTestsByFilter"),
		TEXT("AutomationTestToolset.AutomationTestToolset.GetTestResults"),
		TEXT("AutomationTestToolset.AutomationTestToolset.GetTestStatus"),
		TEXT("AutomationTestToolset.AutomationTestToolset.StopTests"),
		TEXT("EditorToolset.ProgrammaticToolset.get_execution_environment"),
		TEXT("EditorToolset.ProgrammaticToolset.execute_tool_script")};
	return Delegates;
}

FHyperAIAutomationCapabilityStatus
FHyperAIStudioAutomationContracts::GetCapabilityStatus()
{
	FHyperAIAutomationCapabilityStatus Status;
	Status.bAutomationControllerLoaded =
		FModuleManager::Get().IsModuleLoaded(TEXT("AutomationController"));
	Status.DelegatedEpicCallables = GetEpicDelegates();
	Status.DelegatedEpicCallableCount = Status.DelegatedEpicCallables.Num();
	Status.AuthorityFingerprint =
		HyperAIStudio::Automation::Private::AuthorityFingerprint(GetAuthorityRows());
	Status.SupportedCases = {
		TEXT("bounded exact-name observation of an already-populated filtered report tree"),
		TEXT("detached exact-name catalog and controller-state validation"),
		TEXT("pure test-run and profile-capture intent preparation"),
		TEXT("runtime-only build environment diagnosis without file or log parsing")};
	Status.UnsupportedCases = {
		TEXT("automation discovery, listing, run, results, status, and stop remain Epic delegates"),
		TEXT("arbitrary commands, console, scripts, filters, files, exports, and log interpretation"),
		TEXT("cook, package, trace, or test execution without a trusted async continuation backend")};
	Status.Remediation = TEXT("Use Epic AutomationTestToolset for direct automation lifecycle calls. HyperAI source candidates only seal bounded intents until a core journaled continuation backend is admitted.");
	return Status;
}

bool FHyperAIStudioAutomationContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioAutomationManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 5 || GetAuthorityRows().Num() != 19
		|| GetEpicDelegates().Num() != 9) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioAutomationManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioAutomationContracts::IsCanonicalSha256(const FString& Value)
{
	if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioAutomationContracts::IsSafeOperationId(const FString& Value)
{
	return !Value.IsEmpty()
		&& Value.Len() <= FHyperAIStudioDomainLimits::MaxOperationIdChars
		&& Value.TrimStartAndEnd() == Value
		&& !HyperAIStudio::Automation::Private::HasControlCharacter(Value);
}

bool FHyperAIStudioAutomationContracts::ValidateTestNames(
	const TArray<FString>& Names,
	FString& OutFingerprint,
	FString& OutError)
{
	using namespace HyperAIStudio::Automation::Private;
	OutFingerprint.Reset();
	OutError.Reset();
	if (Names.IsEmpty() || Names.Num() > MaxTestNames
		|| Names.GetAllocatedSize() > MaxContainerAllocatedBytes)
	{
		OutError = TEXT("Exact test names are empty or exceed count/storage bounds.");
		return false;
	}
	for (const FString& Name : Names)
	{
		if (!IsSafeTestName(Name))
		{
			OutError = TEXT("A test name is empty, unbounded, non-canonical, or contains a disallowed character.");
			return false;
		}
	}
	TArray<FString> Sorted = Names;
	Sorted.Sort();
	FString Previous;
	FString Canonical(TEXT("hyperai.automation.requested-tests.v1|"));
	for (const FString& Name : Sorted)
	{
		if (Name == Previous)
		{
			OutError = TEXT("Exact test names must be unique.");
			return false;
		}
		Previous = Name;
		AppendToken(Canonical, Name);
	}
	if (Canonical.Len() > MaxCanonicalCharacters)
	{
		OutError = TEXT("Exact test-name canonicalization exceeded its hard bound.");
		return false;
	}
	OutFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	return IsCanonicalSha256(OutFingerprint);
}

bool FHyperAIStudioAutomationContracts::ValidateProfileIntent(
	const FHyperAIProfileCaptureRequest& Request,
	FString& OutFingerprint,
	FString& OutError)
{
	using namespace HyperAIStudio::Automation::Private;
	OutFingerprint.Reset();
	OutError.Reset();
	if (Request.Categories.IsEmpty() || Request.Categories.Num() > MaxCategories
		|| Request.Categories.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.DurationMs < 100 || Request.DurationMs > 60000
		|| Request.MaxCaptureBytes < 1024 * 1024
		|| Request.MaxCaptureBytes > 256ll * 1024 * 1024)
	{
		OutError = TEXT("Profile categories, duration, capture bytes, or storage exceed closed bounds.");
		return false;
	}
	static const TSet<FString> Allowed = {
		TEXT("bookmark"), TEXT("cpu"), TEXT("frame"), TEXT("memory")};
	for (const FString& Category : Request.Categories)
	{
		if (Category.Len() > 16 || !Allowed.Contains(Category))
		{
			OutError = TEXT("Profile categories must use the closed v1 allowlist.");
			return false;
		}
	}
	TArray<FString> Sorted = Request.Categories;
	Sorted.Sort();
	FString Previous;
	FString Canonical(TEXT("hyperai.profile.intent.v1|"));
	for (const FString& Category : Sorted)
	{
		if (Category == Previous)
		{
			OutError = TEXT("Profile categories must be unique.");
			return false;
		}
		Previous = Category;
		AppendToken(Canonical, Category);
	}
	AppendToken(Canonical, LexToString(Request.DurationMs));
	AppendToken(Canonical, LexToString(Request.MaxCaptureBytes));
	AppendToken(Canonical, BoolToken(Request.bIncludePIE));
	OutFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	return IsCanonicalSha256(OutFingerprint);
}

FString FHyperAIStudioAutomationContracts::ComputeTestRecordFingerprint(
	const FHyperAIAutomationTestRecord& Record)
{
	using namespace HyperAIStudio::Automation::Private;
	FString Canonical(TEXT("hyperai.automation.test-record.v1|"));
	AppendToken(Canonical, Record.TestName);
	AppendToken(Canonical, BoolToken(Record.bPresent));
	AppendToken(Canonical, BoolToken(Record.bRunnable));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAutomationContracts::ComputeCatalogFingerprint(
	const TArray<FHyperAIAutomationTestRecord>& Records)
{
	FString Canonical(TEXT("hyperai.automation.catalog.v1|"));
	HyperAIStudio::Automation::Private::AppendToken(Canonical, LexToString(Records.Num()));
	for (const FHyperAIAutomationTestRecord& Record : Records)
	{
		const FString Fingerprint = ComputeTestRecordFingerprint(Record);
		if (!IsCanonicalSha256(Fingerprint)) return FString();
		HyperAIStudio::Automation::Private::AppendToken(Canonical, Fingerprint);
	}
	if (Canonical.Len() > MaxCanonicalCharacters) return FString();
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAutomationContracts::ComputeObservationFingerprint(
	const FHyperAIAutomationDetachedSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Automation::Private;
	FString Canonical(TEXT("hyperai.automation.observation.v1|"));
	AppendToken(Canonical, Snapshot.ControllerState);
	AppendToken(Canonical, BoolToken(Snapshot.bControllerLoaded));
	AppendToken(Canonical, BoolToken(Snapshot.bControllerReady));
	AppendToken(Canonical, BoolToken(Snapshot.bResultsAvailable));
	AppendToken(Canonical, BoolToken(Snapshot.bReportsHaveErrors));
	AppendToken(Canonical, BoolToken(Snapshot.bReportsHaveWarnings));
	AppendToken(Canonical, BoolToken(Snapshot.bReportsHaveLogs));
	AppendToken(Canonical, LexToString(Snapshot.EnabledTestCount));
	AppendToken(Canonical, LexToString(Snapshot.NumPasses));
	AppendToken(Canonical, LexToString(Snapshot.DeviceClusterCount));
	AppendToken(Canonical, LexToString(Snapshot.VisitedReportCount));
	AppendToken(Canonical, BoolToken(Snapshot.bTraversalComplete));
	AppendToken(Canonical, BoolToken(Snapshot.bSnapshotComplete));
	AppendToken(Canonical, Snapshot.RequestedNamesFingerprint);
	AppendToken(Canonical, Snapshot.CatalogFingerprint);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAutomationContracts::ComputeRunSemanticFingerprint(
	const FHyperAIAutomationTestRunPayload& Payload)
{
	FString Canonical(TEXT("hyperai.automation.run.semantic.v1|"));
	HyperAIStudio::Automation::Private::AppendToken(Canonical, Payload.CatalogFingerprint);
	TArray<FString> Sorted = Payload.TestNames;
	Sorted.Sort();
	for (const FString& Name : Sorted)
	{
		HyperAIStudio::Automation::Private::AppendToken(Canonical, Name);
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAutomationContracts::ComputeProfileSemanticFingerprint(
	const FHyperAIAutomationProfilePayload& Payload)
{
	using namespace HyperAIStudio::Automation::Private;
	FString Canonical(TEXT("hyperai.profile.semantic.v1|"));
	AppendToken(Canonical, Payload.IntentFingerprint);
	TArray<FString> Sorted = Payload.Categories;
	Sorted.Sort();
	for (const FString& Category : Sorted) AppendToken(Canonical, Category);
	AppendToken(Canonical, LexToString(Payload.DurationMs));
	AppendToken(Canonical, LexToString(Payload.MaxCaptureBytes));
	AppendToken(Canonical, BoolToken(Payload.bIncludePIE));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAutomationContracts::SchemaFingerprint(const FString& StableSchema)
{
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(StableSchema);
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioAutomationContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.automation.bounded.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// The automation probe group is blocking and remains core-owned.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_test_inspect"), InspectVariantId,
			InspectPayloadTypeId, SchemaFingerprint(TEXT("automation.test-inspect.request.v1")),
			InspectResultTypeId, SchemaFingerprint(TEXT("automation.test-inspect.result.v1")),
			EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_test_run"), RunVariantId,
			RunPayloadTypeId, SchemaFingerprint(TEXT("automation.test-run.intent.v1")),
			RunResultTypeId, SchemaFingerprint(TEXT("automation.test-run.blocked-result.v1")),
			EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_test_validate"), ValidateVariantId,
			ValidatePayloadTypeId, SchemaFingerprint(TEXT("automation.test-validate.request.v1")),
			ValidateResultTypeId, SchemaFingerprint(TEXT("automation.test-validate.result.v1")),
			EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_profile_capture"), ProfileVariantId,
			ProfilePayloadTypeId, SchemaFingerprint(TEXT("automation.profile.intent.v1")),
			ProfileResultTypeId, SchemaFingerprint(TEXT("automation.profile.blocked-result.v1")),
			EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_build_diagnose"), BuildVariantId,
			BuildPayloadTypeId, SchemaFingerprint(TEXT("automation.build-diagnose.request.v1")),
			BuildResultTypeId, SchemaFingerprint(TEXT("automation.build-diagnose.result.v1")),
			EHyperAIStudioDomainSafety::ExternalEffect});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAITestInspectReport FHyperAIStudioAutomationContracts::Inspect(
	const FHyperAITestInspectRequest& Request)
{
	using namespace HyperAIStudio::Automation::Private;
	FHyperAITestInspectReport Report;
	Report.Capability = GetCapabilityStatus();
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread()) return Reject(TEXT("game_thread_required"),
		TEXT("Automation controller observation is game-thread only."));
	if (Request.TestNames.Num() > MaxTestNames
		|| Request.TestNames.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.MaxVisitedReports < 1 || Request.MaxVisitedReports > MaxVisitedReports
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_envelope"),
			TEXT("Exact names, traversal, deadline, or output bounds are invalid."));
	}
	if (Request.TestNames.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("capability_only");
		Report.Diagnostic = TEXT("Returned exact Epic delegation and bounded backend status without discovery, report traversal, test execution, files, or logs.");
		return Report;
	}
	FString NamesFingerprint;
	FString NamesError;
	if (!ValidateTestNames(Request.TestNames, NamesFingerprint, NamesError))
	{
		return Reject(TEXT("invalid_test_names"), *NamesError);
	}
	const int64 WorstCaseBytes = BaseReportBytes
		+ static_cast<int64>(Request.TestNames.Num()) * RecordBytes
		+ static_cast<int64>(FMath::Min(MaxIssues,
			Request.TestNames.Num() + 4)) * IssueBytes;
	if (WorstCaseBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("The exact-name report cannot fit the selected output envelope."));
	}
	TArray<FString> Sorted = Request.TestNames;
	Sorted.Sort();
	CaptureExactNames(Sorted, Request.MaxVisitedReports, Request.DeadlineMs,
		Report.Snapshot, Report.Issues);
	Report.bOk = true;
	Report.Status = Report.Snapshot.bSnapshotComplete
		? TEXT("snapshot_complete") : TEXT("snapshot_partial");
	Report.Diagnostic = Report.Snapshot.bSnapshotComplete
		? TEXT("Exact requested tests are present and runnable in one bounded current filtered report-tree snapshot.")
		: TEXT("Returned bounded controller/catalog evidence, but readiness, exact names, or traversal completeness is missing.");
	return Report;
}

FHyperAITestValidateReport FHyperAIStudioAutomationContracts::Validate(
	const FHyperAITestValidateRequest& Request)
{
	FHyperAITestValidateReport Report;
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_validate_envelope");
		Report.Diagnostic = TEXT("Issue, deadline, or output bounds are invalid.");
		return Report;
	}
	if (HyperAIStudio::Automation::Private::BaseReportBytes
		+ static_cast<int64>(Request.MaxIssues)
			* HyperAIStudio::Automation::Private::IssueBytes > Request.MaxOutputBytes)
	{
		Report.Status = TEXT("worst_case_output_bound");
		Report.Diagnostic = TEXT("The validator issue cap cannot fit the output envelope.");
		return Report;
	}
	HyperAIStudio::Automation::Private::ValidateSnapshotValue(Request.Snapshot,
		Request.MaxIssues, Request.MaxOutputBytes, Request.DeadlineMs, Report);
	return Report;
}

FHyperAITestRunReport FHyperAIStudioAutomationContracts::BuildRunPlan(
	const FHyperAITestRunRequest& Request)
{
	using namespace HyperAIStudio::Automation::Private;
	FHyperAITestRunReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bExecutionSubmitted = false;
		Report.Effects.PhysicalEffectCount = 0;
		return Report;
	};
	if (!IsInGameThread()) return Reject(TEXT("game_thread_required"),
		TEXT("Automation run intent preparation is game-thread only."));
	if (!IsSafeOperationId(Request.OperationId)
		|| Request.TestNames.Num() > MaxTestNames
		|| Request.TestNames.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.ExpectedSnapshot.Tests.GetAllocatedSize() > MaxContainerAllocatedBytes
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_run_envelope"),
			TEXT("Operation, exact names/snapshot storage, deadline, or output bounds are invalid."));
	}
	if ((Request.bDryRun && !Request.ExpectedPlanHash.IsEmpty())
		|| (!Request.bDryRun && !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_plan_hash_contract"),
			TEXT("Dry-run takes no expected plan hash; non-dry intent must echo one canonical hash."));
	}
	FString NamesFingerprint;
	FString NamesError;
	if (!ValidateTestNames(Request.TestNames, NamesFingerprint, NamesError))
	{
		return Reject(TEXT("invalid_test_names"), *NamesError);
	}
	FHyperAITestValidateReport DetachedValidation;
	if (!ValidateSnapshotValue(Request.ExpectedSnapshot, MaxIssues,
		Request.MaxOutputBytes, Request.DeadlineMs, DetachedValidation)
		|| DetachedValidation.RecomputedRequestedNamesFingerprint != NamesFingerprint)
	{
		Report.Issues = MoveTemp(DetachedValidation.Issues);
		return Reject(TEXT("detached_catalog_invalid"),
			TEXT("Run intent requires one valid complete detached snapshot for exactly the requested names."));
	}
	TArray<FString> Sorted = Request.TestNames;
	Sorted.Sort();
	FHyperAIAutomationDetachedSnapshot Fresh;
	TArray<FHyperAIAutomationIssue> FreshIssues;
	if (!CaptureExactNames(Sorted, MaxVisitedReports, Request.DeadlineMs,
		Fresh, FreshIssues)
		|| Fresh.CatalogFingerprint != Request.ExpectedSnapshot.CatalogFingerprint
		|| Fresh.ObservationFingerprint != Request.ExpectedSnapshot.ObservationFingerprint)
	{
		Report.Issues = MoveTemp(FreshIssues);
		return Reject(TEXT("fresh_catalog_cas_mismatch"),
			TEXT("The fresh bounded controller/catalog observation is incomplete or differs from the detached CAS."));
	}
	Report.Effects.TestCount = Sorted.Num();
	Report.Effects.bCatalogCasValid = true;

	const TSharedRef<FHyperAIAutomationTestRunPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIAutomationTestRunPayload, ESPMode::ThreadSafe>();
	Payload->TestNames = Sorted;
	Payload->CatalogFingerprint = Fresh.CatalogFingerprint;
	Payload->SemanticFingerprint = ComputeRunSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || !IsCanonicalSha256(Payload->SemanticFingerprint)
		|| Clone->GetSemanticFingerprint() != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("immutable_payload_seal_failed"),
			TEXT("The exact-name test-run intent failed detached immutable sealing."));
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!PrepareExternalIntent(TEXT("hyper_test_run"), RunVariantId, Clone,
		TEXT("automation-tests:") + Fresh.CatalogFingerprint, Request.DeadlineMs,
		Sorted.Num() + 4, Request.MaxOutputBytes, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), *PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.bWouldInvokeEpicRunOnce = true;
	Report.Effects.bWouldPollTerminalState = true;
	Report.Effects.bWouldValidateResultsOnce = true;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_zero_effect");
		Report.Diagnostic = TEXT("Pure Prepare sealed one exact catalog-CAS test-run intent. No discovery, test, stop, script, file, or console effect ran.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("Non-dry intent does not echo the exact pure dry-run plan hash."));
	}
	return Reject(RunBackendBlocker,
		TEXT("No test ran. A trusted journaled continuation backend must invoke the exact Epic run once, poll/cancel asynchronously, and validate terminal results."));
}

FHyperAIProfileCaptureReport FHyperAIStudioAutomationContracts::BuildProfilePlan(
	const FHyperAIProfileCaptureRequest& Request)
{
	using namespace HyperAIStudio::Automation::Private;
	FHyperAIProfileCaptureReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		Report.bCaptureStarted = false;
		Report.PhysicalEffectCount = 0;
		return Report;
	};
	if (!IsInGameThread()) return Reject(TEXT("game_thread_required"),
		TEXT("Profile intent preparation is game-thread only."));
	if (!IsSafeOperationId(Request.OperationId)
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_profile_envelope"),
			TEXT("Operation, deadline, or output bounds are invalid."));
	}
	if ((Request.bDryRun && !Request.ExpectedPlanHash.IsEmpty())
		|| (!Request.bDryRun && !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_plan_hash_contract"),
			TEXT("Dry-run takes no expected plan hash; non-dry intent must echo one canonical hash."));
	}
	FString IntentFingerprint;
	FString IntentError;
	if (!ValidateProfileIntent(Request, IntentFingerprint, IntentError))
	{
		return Reject(TEXT("invalid_profile_intent"), *IntentError);
	}
	TArray<FString> Sorted = Request.Categories;
	Sorted.Sort();
	const TSharedRef<FHyperAIAutomationProfilePayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIAutomationProfilePayload, ESPMode::ThreadSafe>();
	Payload->Categories = Sorted;
	Payload->DurationMs = Request.DurationMs;
	Payload->MaxCaptureBytes = Request.MaxCaptureBytes;
	Payload->bIncludePIE = Request.bIncludePIE;
	Payload->IntentFingerprint = IntentFingerprint;
	Payload->SemanticFingerprint = ComputeProfileSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || !IsCanonicalSha256(Payload->SemanticFingerprint)
		|| Clone->GetSemanticFingerprint() != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("immutable_payload_seal_failed"),
			TEXT("The bounded profile intent failed detached immutable sealing."));
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!PrepareExternalIntent(TEXT("hyper_profile_capture"), ProfileVariantId, Clone,
		TEXT("profile:") + IntentFingerprint, Request.DeadlineMs,
		Sorted.Num() + 4, Request.MaxOutputBytes, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), *PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.IntentFingerprint = IntentFingerprint;
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.CategoryCount = Sorted.Num();
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_zero_effect");
		Report.Diagnostic = TEXT("Pure Prepare sealed one bounded profile-capture intent. No trace, PIE, file, console, or external effect ran.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("Non-dry intent does not echo the exact pure dry-run plan hash."));
	}
	return Reject(ProfileBackendBlocker,
		TEXT("No profile capture started. A trusted journaled continuation backend must begin once, enforce byte/time caps, observe/cancel terminal state, and retain typed evidence without exposing arbitrary trace commands."));
}

FHyperAIBuildDiagnoseReport FHyperAIStudioAutomationContracts::DiagnoseBuild(
	const FHyperAIBuildDiagnoseRequest& Request)
{
	using namespace HyperAIStudio::Automation::Private;
	FHyperAIBuildDiagnoseReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Build runtime-context diagnosis is game-thread only.");
		return Report;
	}
	if (Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_diagnose_envelope");
		Report.Diagnostic = TEXT("Deadline or output bounds are invalid.");
		return Report;
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	Report.EngineVersion = FEngineVersion::Current().ToString().Left(64);
	Report.Platform = FString(FPlatformProperties::PlatformName()).Left(64);
	Report.BuildConfiguration = BuildConfigurationToken();
	Report.ProjectName = FString(FApp::GetProjectName()).Left(128);
	Report.CanonicalProjectId =
		FHyperAIStudioExtensionRuntime::GetCanonicalProjectId().Left(128);
#if WITH_EDITOR
	Report.bEditorProcess = true;
#else
	Report.bEditorProcess = false;
#endif
	Report.bCommandlet = IsRunningCommandlet();
	Report.bUnattended = FApp::IsUnattended();
	Report.bAutomationControllerLoaded =
		FModuleManager::Get().IsModuleLoaded(TEXT("AutomationController"));
	Report.bTraceLogLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("TraceLog"));
	Report.bToolsetRegistryLoaded =
		FModuleManager::Get().IsModuleLoaded(TEXT("ToolsetRegistry"));
	Report.bModelContextProtocolLoaded =
		FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocol"));
	if (DeadlineExceeded(Deadline))
	{
		Report.Status = TEXT("diagnose_deadline_exceeded");
		Report.Diagnostic = TEXT("Runtime-context diagnosis exceeded its monotonic deadline.");
		return Report;
	}
	AddIssue(Report.Issues, TEXT("trusted_build_result_unavailable"), TEXT("warning"),
		FString(), TEXT("No core-owned typed build/cook/package result store is bound; HyperAI does not infer results from logs or files."));
	AddIssue(Report.Issues, TEXT("package_size_unavailable"), TEXT("info"), FString(),
		TEXT("Package size remains unavailable because unrestricted filesystem measurement is prohibited."));
	FString Canonical(TEXT("hyperai.build.runtime-context.v1|"));
	AppendToken(Canonical, Report.EngineVersion);
	AppendToken(Canonical, Report.Platform);
	AppendToken(Canonical, Report.BuildConfiguration);
	AppendToken(Canonical, Report.ProjectName);
	AppendToken(Canonical, Report.CanonicalProjectId);
	AppendToken(Canonical, BoolToken(Report.bEditorProcess));
	AppendToken(Canonical, BoolToken(Report.bCommandlet));
	AppendToken(Canonical, BoolToken(Report.bUnattended));
	AppendToken(Canonical, BoolToken(Report.bAutomationControllerLoaded));
	AppendToken(Canonical, BoolToken(Report.bTraceLogLoaded));
	AppendToken(Canonical, BoolToken(Report.bToolsetRegistryLoaded));
	AppendToken(Canonical, BoolToken(Report.bModelContextProtocolLoaded));
	Report.ObservationFingerprint =
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	Report.bOk = true;
	Report.Status = TEXT("runtime_context_only");
	Report.Diagnostic = TEXT("Returned bounded process/module/build context with zero effects. No compile, cook, package, trace, command, file read, export, or log interpretation occurred; historical result claims remain unavailable.");
	return Report;
}

FString FHyperAIAutomationTestRunPayload::GetTypeId() const
{
	return FHyperAIStudioAutomationContracts::RunPayloadTypeId;
}

FString FHyperAIAutomationTestRunPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioAutomationContracts::SchemaFingerprint(
		TEXT("automation.test-run.intent.v1"));
}

int32 FHyperAIAutomationTestRunPayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (CatalogFingerprint.Len() + SemanticFingerprint.Len());
	for (const FString& Name : TestNames) Size += 32ll + 2ll * Name.Len();
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIAutomationTestRunPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIAutomationTestRunPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIAutomationTestRunPayload, ESPMode::ThreadSafe>();
	Clone->TestNames = TestNames;
	Clone->CatalogFingerprint = CatalogFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIAutomationProfilePayload::GetTypeId() const
{
	return FHyperAIStudioAutomationContracts::ProfilePayloadTypeId;
}

FString FHyperAIAutomationProfilePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioAutomationContracts::SchemaFingerprint(
		TEXT("automation.profile.intent.v1"));
}

int32 FHyperAIAutomationProfilePayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (IntentFingerprint.Len() + SemanticFingerprint.Len());
	for (const FString& Category : Categories) Size += 32ll + 2ll * Category.Len();
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIAutomationProfilePayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIAutomationProfilePayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIAutomationProfilePayload, ESPMode::ThreadSafe>();
	Clone->Categories = Categories;
	Clone->DurationMs = DurationMs;
	Clone->MaxCaptureBytes = MaxCaptureBytes;
	Clone->bIncludePIE = bIncludePIE;
	Clone->IntentFingerprint = IntentFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FHyperAITestInspectReport UHyperAIStudioAutomationToolset::hyper_test_inspect(
	const FHyperAITestInspectRequest& Request)
{
	return FHyperAIStudioAutomationContracts::Inspect(Request);
}

FHyperAITestRunReport UHyperAIStudioAutomationToolset::hyper_test_run(
	const FHyperAITestRunRequest& Request)
{
	return FHyperAIStudioAutomationContracts::BuildRunPlan(Request);
}

FHyperAITestValidateReport UHyperAIStudioAutomationToolset::hyper_test_validate(
	const FHyperAITestValidateRequest& Request)
{
	return FHyperAIStudioAutomationContracts::Validate(Request);
}

FHyperAIProfileCaptureReport UHyperAIStudioAutomationToolset::hyper_profile_capture(
	const FHyperAIProfileCaptureRequest& Request)
{
	return FHyperAIStudioAutomationContracts::BuildProfilePlan(Request);
}

FHyperAIBuildDiagnoseReport UHyperAIStudioAutomationToolset::hyper_build_diagnose(
	const FHyperAIBuildDiagnoseRequest& Request)
{
	return FHyperAIStudioAutomationContracts::DiagnoseBuild(Request);
}

FHyperAIStudioAutomationDomainAdapter::FHyperAIStudioAutomationDomainAdapter()
	: Descriptor(FHyperAIStudioAutomationContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioAutomationDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioAutomationDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&Result](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Automation adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_test_run")
		&& Context.Binding.VariantId == FHyperAIStudioAutomationContracts::RunVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioAutomationContracts::RunPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioAutomationContracts::SchemaFingerprint(
				TEXT("automation.test-run.intent.v1")))
	{
		const FHyperAIAutomationTestRunPayload& Typed =
			static_cast<const FHyperAIAutomationTestRunPayload&>(Payload);
		if (FHyperAIStudioAutomationContracts::ComputeRunSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached exact-name run intent drifted."));
		}
		return Reject(FHyperAIStudioAutomationContracts::RunBackendBlocker,
			TEXT("No test ran; async journaled execution is not implemented in this adapter."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_profile_capture")
		&& Context.Binding.VariantId == FHyperAIStudioAutomationContracts::ProfileVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioAutomationContracts::ProfilePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioAutomationContracts::SchemaFingerprint(
				TEXT("automation.profile.intent.v1")))
	{
		const FHyperAIAutomationProfilePayload& Typed =
			static_cast<const FHyperAIAutomationProfilePayload&>(Payload);
		if (FHyperAIStudioAutomationContracts::ComputeProfileSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached profile-capture intent drifted."));
		}
		return Reject(FHyperAIStudioAutomationContracts::ProfileBackendBlocker,
			TEXT("No profile capture started; async journaled capture is not implemented in this adapter."));
	}
	const TSharedRef<FHyperAIAutomationResultPayload, ESPMode::ThreadSafe> Output =
		MakeShared<FHyperAIAutomationResultPayload, ESPMode::ThreadSafe>();
	if (Context.Binding.ToolName == TEXT("hyper_test_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioAutomationContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioAutomationContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioAutomationContracts::SchemaFingerprint(
				TEXT("automation.test-inspect.request.v1")))
	{
		const FHyperAIAutomationRequestPayload& Typed =
			static_cast<const FHyperAIAutomationRequestPayload&>(Payload);
		Output->InspectReport = FHyperAIStudioAutomationContracts::Inspect(Typed.InspectRequest);
		Output->TypeId = FHyperAIStudioAutomationContracts::InspectResultTypeId;
		Output->SchemaFingerprint = FHyperAIStudioAutomationContracts::SchemaFingerprint(
			TEXT("automation.test-inspect.result.v1"));
		Output->BoundedByteSize = 4096 + Output->InspectReport.Snapshot.Tests.Num() * 768;
		Result.StatusCode = Output->InspectReport.Status;
		Result.Diagnostic = Output->InspectReport.Diagnostic;
	}
	else if (Context.Binding.ToolName == TEXT("hyper_test_validate")
		&& Context.Binding.VariantId == FHyperAIStudioAutomationContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioAutomationContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioAutomationContracts::SchemaFingerprint(
				TEXT("automation.test-validate.request.v1")))
	{
		const FHyperAIAutomationRequestPayload& Typed =
			static_cast<const FHyperAIAutomationRequestPayload&>(Payload);
		Output->ValidateReport = FHyperAIStudioAutomationContracts::Validate(Typed.ValidateRequest);
		Output->TypeId = FHyperAIStudioAutomationContracts::ValidateResultTypeId;
		Output->SchemaFingerprint = FHyperAIStudioAutomationContracts::SchemaFingerprint(
			TEXT("automation.test-validate.result.v1"));
		Output->BoundedByteSize = 4096 + Output->ValidateReport.Issues.Num() * 512;
		Result.StatusCode = Output->ValidateReport.Status;
		Result.Diagnostic = Output->ValidateReport.Diagnostic;
	}
	else if (Context.Binding.ToolName == TEXT("hyper_build_diagnose")
		&& Context.Binding.VariantId == FHyperAIStudioAutomationContracts::BuildVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioAutomationContracts::BuildPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioAutomationContracts::SchemaFingerprint(
				TEXT("automation.build-diagnose.request.v1")))
	{
		const FHyperAIAutomationRequestPayload& Typed =
			static_cast<const FHyperAIAutomationRequestPayload&>(Payload);
		Output->BuildReport = FHyperAIStudioAutomationContracts::DiagnoseBuild(Typed.BuildRequest);
		Output->TypeId = FHyperAIStudioAutomationContracts::BuildResultTypeId;
		Output->SchemaFingerprint = FHyperAIStudioAutomationContracts::SchemaFingerprint(
			TEXT("automation.build-diagnose.result.v1"));
		Output->BoundedByteSize = 6144 + Output->BuildReport.Issues.Num() * 512;
		Result.StatusCode = Output->BuildReport.Status;
		Result.Diagnostic = Output->BuildReport.Diagnostic;
	}
	else
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Automation adapter rejected a non-exact tool, variant, safety, or DTO binding."));
	}
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
	Result.StatusCode = Result.StatusCode.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
	Result.Diagnostic = Result.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
	Result.Payload = Output;
	return Result;
}

void FHyperAIStudioAutomationRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioAutomationRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioAutomationRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioAutomationRegistration::IsRegistered() const
{
	return FHyperAIStudioAutomationContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioAutomationToolset::StaticClass(),
			FHyperAIStudioAutomationContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioAutomationRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioAutomationRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable()) return;
	if (!FHyperAIStudioAutomationContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())) return;
	Adapter = MakeShared<FHyperAIStudioAutomationDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioAutomation, Error,
			TEXT("Automation adapter registration failed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioAutomationContracts::LiveProbeId,
		ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioAutomation, Error,
			TEXT("Automation probe registration failed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = FModuleManager::GetModulePtr<IAutomationControllerModule>(
		TEXT("AutomationController")) != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_controller") : TEXT("automation_controller_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("AutomationController is already loaded; the probe triggered no discovery or execution.")
		: TEXT("AutomationController is not loaded; the probe never loads it from a tool call.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioAutomation, Error,
			TEXT("Automation probe publication failed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioAutomationToolset::StaticClass(),
		FHyperAIStudioAutomationContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioAutomation, Error,
			TEXT("Automation exact five-tool registration failed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioAutomationRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioAutomationToolset::StaticClass(),
			FHyperAIStudioAutomationContracts::GetQualifiedToolsetName(), Error)) return;
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error)) return;
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound) return;
		AdapterHandle = {};
		Adapter.Reset();
	}
}
