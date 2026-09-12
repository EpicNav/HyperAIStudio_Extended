// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAutomationToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIAutomationIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIAutomationAuthorityRow
{
	GENERATED_BODY()

	UPROPERTY() FString Source;
	UPROPERTY() FString SourceId;
	UPROPERTY() FString Lifecycle;
	UPROPERTY() FString Access;
	UPROPERTY() FString Disposition;
	UPROPERTY() FString DirectRoute;
	UPROPERTY() FString FrozenCoverage;
};

USTRUCT(BlueprintType)
struct FHyperAIAutomationCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString State = TEXT("source_candidate_async_backend_required");
	UPROPERTY() bool bAutomationControllerLoaded = false;
	UPROPERTY() bool bTypedReadImplemented = true;
	UPROPERTY() bool bDetachedValidatorImplemented = true;
	UPROPERTY() bool bTestExecutionImplemented = false;
	UPROPERTY() bool bProfileExecutionImplemented = false;
	UPROPERTY() bool bBuildExecutionImplemented = false;
	UPROPERTY() int32 DelegatedEpicCallableCount = 9;
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() FString AuthorityFingerprint;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIAutomationTestRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TestName;
	UPROPERTY() bool bPresent = false;
	UPROPERTY() bool bRunnable = false;
	UPROPERTY() FString Fingerprint;
};

/** Detached exact-name view of the already-populated current filtered report tree. */
USTRUCT(BlueprintType)
struct FHyperAIAutomationDetachedSnapshot
{
	GENERATED_BODY()

	UPROPERTY() FString ControllerState;
	UPROPERTY() bool bControllerLoaded = false;
	UPROPERTY() bool bControllerReady = false;
	UPROPERTY() bool bResultsAvailable = false;
	UPROPERTY() bool bReportsHaveErrors = false;
	UPROPERTY() bool bReportsHaveWarnings = false;
	UPROPERTY() bool bReportsHaveLogs = false;
	UPROPERTY() int32 EnabledTestCount = 0;
	UPROPERTY() int32 NumPasses = 0;
	UPROPERTY() int32 DeviceClusterCount = 0;
	UPROPERTY() int32 VisitedReportCount = 0;
	UPROPERTY() bool bTraversalComplete = false;
	UPROPERTY() bool bSnapshotComplete = false;
	UPROPERTY() FString RequestedNamesFingerprint;
	UPROPERTY() FString CatalogFingerprint;
	UPROPERTY() FString ObservationFingerprint;
	UPROPERTY() TArray<FHyperAIAutomationTestRecord> Tests;
};

USTRUCT(BlueprintType)
struct FHyperAITestInspectRequest
{
	GENERATED_BODY()

	/** Exact names to check; empty returns only controller/delegation status. */
	UPROPERTY() TArray<FString> TestNames;
	UPROPERTY() int32 MaxVisitedReports = 512;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITestInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FHyperAIAutomationCapabilityStatus Capability;
	UPROPERTY() FHyperAIAutomationDetachedSnapshot Snapshot;
	UPROPERTY() TArray<FHyperAIAutomationIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAITestValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FHyperAIAutomationDetachedSnapshot Snapshot;
	UPROPERTY() int32 MaxIssues = 64;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITestValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString RecomputedRequestedNamesFingerprint;
	UPROPERTY() FString RecomputedCatalogFingerprint;
	UPROPERTY() FString RecomputedObservationFingerprint;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIAutomationIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAITestRunRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() TArray<FString> TestNames;
	UPROPERTY() FHyperAIAutomationDetachedSnapshot ExpectedSnapshot;
	UPROPERTY() int32 DeadlineMs = 1500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITestRunEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TestCount = 0;
	UPROPERTY() bool bCatalogCasValid = false;
	UPROPERTY() bool bWouldInvokeEpicRunOnce = false;
	UPROPERTY() bool bWouldPollTerminalState = false;
	UPROPERTY() bool bWouldValidateResultsOnce = false;
	UPROPERTY() int32 PhysicalEffectCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAITestRunReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FHyperAITestRunEffects Effects;
	UPROPERTY() TArray<FHyperAIAutomationIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIProfileCaptureRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	/** cpu, frame, bookmark, and memory are the only v1 categories. */
	UPROPERTY() TArray<FString> Categories;
	UPROPERTY() int32 DurationMs = 5000;
	UPROPERTY() int64 MaxCaptureBytes = 64 * 1024 * 1024;
	UPROPERTY() bool bIncludePIE = false;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIProfileCaptureReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bCaptureStarted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString IntentFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() int32 CategoryCount = 0;
	UPROPERTY() int32 PhysicalEffectCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIBuildDiagnoseRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 DeadlineMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 32768;
};

USTRUCT(BlueprintType)
struct FHyperAIBuildDiagnoseReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bBuildEvidenceComplete = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString EngineVersion;
	UPROPERTY() FString Platform;
	UPROPERTY() FString BuildConfiguration;
	UPROPERTY() FString ProjectName;
	UPROPERTY() FString CanonicalProjectId;
	UPROPERTY() bool bEditorProcess = true;
	UPROPERTY() bool bCommandlet = false;
	UPROPERTY() bool bUnattended = false;
	UPROPERTY() bool bAutomationControllerLoaded = false;
	UPROPERTY() bool bTraceLogLoaded = false;
	UPROPERTY() bool bToolsetRegistryLoaded = false;
	UPROPERTY() bool bModelContextProtocolLoaded = false;
	UPROPERTY() bool bTrustedCookResultAvailable = false;
	UPROPERTY() bool bTrustedBuildResultAvailable = false;
	UPROPERTY() bool bTrustedPackageSizeAvailable = false;
	UPROPERTY() int32 PhysicalEffectCount = 0;
	UPROPERTY() FString ObservationFingerprint;
	UPROPERTY() TArray<FHyperAIAutomationIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOAUTOMATION_API UHyperAIStudioAutomationToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Automation")
	static FHyperAITestInspectReport hyper_test_inspect(const FHyperAITestInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Automation")
	static FHyperAITestRunReport hyper_test_run(const FHyperAITestRunRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Automation")
	static FHyperAITestValidateReport hyper_test_validate(const FHyperAITestValidateRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Automation")
	static FHyperAIProfileCaptureReport hyper_profile_capture(
		const FHyperAIProfileCaptureRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Automation")
	static FHyperAIBuildDiagnoseReport hyper_build_diagnose(
		const FHyperAIBuildDiagnoseRequest& Request);
};

class FHyperAIAutomationTestRunPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FString> TestNames;
	FString CatalogFingerprint;
	FString SemanticFingerprint;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIAutomationProfilePayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FString> Categories;
	int32 DurationMs = 0;
	int64 MaxCaptureBytes = 0;
	bool bIncludePIE = false;
	FString IntentFingerprint;
	FString SemanticFingerprint;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIAutomationRequestPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FString TypeId;
	FString SchemaFingerprint;
	int32 BoundedByteSize = 0;
	FHyperAITestInspectRequest InspectRequest;
	FHyperAITestValidateRequest ValidateRequest;
	FHyperAIBuildDiagnoseRequest BuildRequest;
	virtual FString GetTypeId() const override { return TypeId; }
	virtual FString GetSchemaFingerprint() const override { return SchemaFingerprint; }
	virtual int32 GetBoundedByteSize() const override { return BoundedByteSize; }
};

class FHyperAIAutomationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString TypeId;
	FString SchemaFingerprint;
	int32 BoundedByteSize = 0;
	FHyperAITestInspectReport InspectReport;
	FHyperAITestValidateReport ValidateReport;
	FHyperAIBuildDiagnoseReport BuildReport;
	virtual FString GetTypeId() const override { return TypeId; }
	virtual FString GetSchemaFingerprint() const override { return SchemaFingerprint; }
	virtual int32 GetBoundedByteSize() const override { return BoundedByteSize; }
};

class FHyperAIStudioAutomationDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioAutomationDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioAutomationManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioAutomationContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("automation_profiling_build");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioautomationtoolset.v1");
	static constexpr const TCHAR* BlockingRequirementGroupId = TEXT("automation");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.automation_controller");
	static constexpr const TCHAR* InspectVariantId = TEXT("exact_name_current_report_snapshot.v1");
	static constexpr const TCHAR* RunVariantId = TEXT("exact_catalog_test_run_intent.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("detached_test_snapshot_validation.v1");
	static constexpr const TCHAR* ProfileVariantId = TEXT("bounded_profile_capture_intent.v1");
	static constexpr const TCHAR* BuildVariantId = TEXT("bounded_build_context_diagnosis.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.automation.test-inspect.v1");
	static constexpr const TCHAR* RunPayloadTypeId = TEXT("hyperai.payload.automation.test-run.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.automation.test-validate.v1");
	static constexpr const TCHAR* ProfilePayloadTypeId = TEXT("hyperai.payload.automation.profile-capture.v1");
	static constexpr const TCHAR* BuildPayloadTypeId = TEXT("hyperai.payload.automation.build-diagnose.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.automation.test-inspect.v1");
	static constexpr const TCHAR* RunResultTypeId = TEXT("hyperai.result.automation.test-run-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.automation.test-validate.v1");
	static constexpr const TCHAR* ProfileResultTypeId = TEXT("hyperai.result.automation.profile-blocked.v1");
	static constexpr const TCHAR* BuildResultTypeId = TEXT("hyperai.result.automation.build-diagnose.v1");
	static constexpr const TCHAR* RunBackendBlocker =
		TEXT("automation_async_continuation_backend_required");
	static constexpr const TCHAR* ProfileBackendBlocker =
		TEXT("profile_async_continuation_backend_required");
	static constexpr const TCHAR* NativeAccessReviewId =
		TEXT("epic-ue5.8-native-aicallable-access-2026-08-15");
	static constexpr const TCHAR* NativeAccessReviewRecordsSha256 =
		TEXT("sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr int32 MaxTestNames = 32;
	static constexpr int32 MaxTestNameCharacters = 256;
	static constexpr int32 MaxVisitedReports = 1024;
	static constexpr int32 MaxCategories = 4;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 12 * 1024;
	static constexpr int32 MaxContainerAllocatedBytes = 256 * 1024;
	static constexpr int32 MaxCanonicalCharacters = 128 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioAutomationManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FHyperAIAutomationAuthorityRow>& GetAuthorityRows();
	static const TArray<FString>& GetEpicDelegates();
	static FHyperAIAutomationCapabilityStatus GetCapabilityStatus();
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static bool ValidateTestNames(const TArray<FString>& Names, FString& OutFingerprint,
		FString& OutError);
	static bool ValidateProfileIntent(const FHyperAIProfileCaptureRequest& Request,
		FString& OutFingerprint, FString& OutError);
	static FString ComputeTestRecordFingerprint(const FHyperAIAutomationTestRecord& Record);
	static FString ComputeCatalogFingerprint(const TArray<FHyperAIAutomationTestRecord>& Records);
	static FString ComputeObservationFingerprint(const FHyperAIAutomationDetachedSnapshot& Snapshot);
	static FString ComputeRunSemanticFingerprint(const FHyperAIAutomationTestRunPayload& Payload);
	static FString ComputeProfileSemanticFingerprint(const FHyperAIAutomationProfilePayload& Payload);
	static FString SchemaFingerprint(const FString& StableSchema);
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAITestInspectReport Inspect(const FHyperAITestInspectRequest& Request);
	static FHyperAITestValidateReport Validate(const FHyperAITestValidateRequest& Request);
	static FHyperAITestRunReport BuildRunPlan(const FHyperAITestRunRequest& Request);
	static FHyperAIProfileCaptureReport BuildProfilePlan(
		const FHyperAIProfileCaptureRequest& Request);
	static FHyperAIBuildDiagnoseReport DiagnoseBuild(
		const FHyperAIBuildDiagnoseRequest& Request);
};

class FHyperAIStudioAutomationRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;
private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();
	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioAutomationDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
