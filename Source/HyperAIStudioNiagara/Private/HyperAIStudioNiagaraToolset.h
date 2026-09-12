// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioNiagaraToolset.generated.h"

class UNiagaraValidationRuleSet;

USTRUCT(BlueprintType)
struct FHyperAINiagaraIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString RuleClass;
	UPROPERTY() FString SourcePath;
	UPROPERTY() FString Summary;
	UPROPERTY() FString Description;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("niagara_system");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bIndependentValidationImplemented = false;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State = TEXT("source_candidate_async_host_required");
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraUserParameterIdentity
{
	GENERATED_BODY()

	UPROPERTY() FString VariableGuid;
	UPROPERTY() FString Name;
	UPROPERTY() FString TypeId;
	UPROPERTY() FString TypeFingerprint;
	UPROPERTY() FString DefaultFingerprint;
	UPROPERTY() FString MetadataChangeId;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraHealthRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	/** exists, does_not_exist, or unknown. Unknown is never absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() bool bExistsOnDisk = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() FString AssetGuid;
	UPROPERTY() bool bCompileStateKnown = false;
	UPROPERTY() bool bCompileActive = false;
	UPROPERTY() bool bCompileStale = false;
	UPROPERTY() bool bCompileHasErrors = false;
	UPROPERTY() bool bCompileHasWarnings = false;
	UPROPERTY() bool bSystemValid = false;
	UPROPERTY() bool bReadyToRun = false;
	UPROPERTY() bool bExistingSystemViewModel = false;
	UPROPERTY() int32 EmitterCount = 0;
	UPROPERTY() int32 ParameterCount = 0;
	UPROPERTY() int32 ScriptCount = 0;
	UPROPERTY() int32 GraphCount = 0;
	UPROPERTY() int32 GraphNodeCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraInspectRequest
{
	GENERATED_BODY()

	/** One exact canonical loaded /Game object path; never a folder, wildcard, or load request. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 PageSize = 32;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAINiagaraHealthRecord Health;
	UPROPERTY() TArray<FHyperAINiagaraUserParameterIdentity> UserParameters;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	/** Optional exact revision assertion from inspect. */
	UPROPERTY() FString ExpectedRevision;
	/** authoring or runtime_ready. */
	UPROPERTY() FString Policy = TEXT("authoring");
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 150;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Policy;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() int32 InfoCount = 0;
	UPROPERTY() int32 ExecutedRuleCount = 0;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraUserParameterRename
{
	GENERATED_BODY()

	UPROPERTY() FString ExpectedVariableGuid;
	UPROPERTY() FString OldName;
	UPROPERTY() FString NewName;
	UPROPERTY() FString ExpectedTypeFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() FHyperAINiagaraUserParameterRename Rename;
	UPROPERTY() int32 DeadlineMs = 60000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 RenameCount = 0;
	UPROPERTY() bool bTypedPayloadSealed = false;
	UPROPERTY() bool bWouldUseExistingViewModel = false;
	UPROPERTY() bool bWouldTransactionOnce = false;
	UPROPERTY() bool bWouldCompileOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() bool bWouldFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAINiagaraApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAINiagaraPlanEffects Effects;
	UPROPERTY() TArray<FHyperAINiagaraIssue> Issues;
	UPROPERTY() TArray<FHyperAINiagaraCapabilityStatus> Capabilities;
};

/** Exactly three functions form the Niagara atomic cohort. */
UCLASS()
class HYPERAISTUDIONIAGARA_API UHyperAIStudioNiagaraToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraInspectReport hyper_niagara_inspect(
		const FHyperAINiagaraInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraApplyPlanReport hyper_niagara_apply_plan(
		const FHyperAINiagaraApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Niagara")
	static FHyperAINiagaraValidateReport hyper_niagara_validate(
		const FHyperAINiagaraValidateRequest& Request);
};

struct FHyperAIStudioNiagaraValueSnapshot
{
	FHyperAINiagaraHealthRecord Health;
	TArray<FHyperAINiagaraUserParameterIdentity> Parameters;
	TArray<FHyperAINiagaraIssue> CaptureIssues;
	bool bComplete = false;
};

class FHyperAIStudioNiagaraInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAINiagaraInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAINiagaraValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraRenamePayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BaseRevision;
	FString ExpectedVariableGuid;
	FString OldName;
	FString NewName;
	FString TypeFingerprint;
	FString DefaultFingerprint;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioNiagaraInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAINiagaraInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAINiagaraValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioNiagaraDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioNiagaraDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

class FHyperAIStudioNiagaraFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	explicit FHyperAIStudioNiagaraFreshVerifier(FString InOwnerAdapterFingerprint);
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;

private:
	FString OwnerAdapterFingerprint;
};

struct FHyperAIStudioNiagaraManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioNiagaraContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("niagara_vfx");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioniagaratoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("niagara_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("niagara_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.niagara_editor");
	static constexpr const TCHAR* InspectVariantId = TEXT("exact_health_cas.v1");
	static constexpr const TCHAR* MutationVariantId =
		TEXT("user_parameter_rename_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("loaded_native_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId =
		TEXT("hyperai.payload.niagara.inspect.v1");
	static constexpr const TCHAR* RenamePayloadTypeId =
		TEXT("hyperai.payload.niagara.user_parameter_rename.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId =
		TEXT("hyperai.payload.niagara.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId =
		TEXT("hyperai.result.niagara.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId =
		TEXT("hyperai.result.niagara.user_parameter_rename.v1");
	static constexpr const TCHAR* ValidateResultTypeId =
		TEXT("hyperai.result.niagara.validate.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("async_continuation_host_required");
	static constexpr const TCHAR* FullReferenceInventoryState =
		TEXT("full_reference_inventory_incomplete");
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxPageSize = 64;
	static constexpr int32 MaxEmitters = 128;
	static constexpr int32 MaxParameters = 256;
	static constexpr int32 MaxScripts = 512;
	static constexpr int32 MaxGraphs = 256;
	static constexpr int32 MaxGraphNodes = 4096;
	static constexpr int32 MaxGraphPins = 16384;
	static constexpr int32 MaxGraphMetadata = 8192;
	static constexpr int32 MaxEmitterVersions = 256;
	static constexpr int32 MaxRenderers = 512;
	static constexpr int32 MaxSimulationStages = 512;
	static constexpr int32 MaxStackEntries = 2048;
	static constexpr int32 MaxValidationRules = 128;
	static constexpr int32 MaxInternalValidationResults = 512;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxReadGameThreadMs = 150;
	static constexpr int32 MaxMutationGameThreadMs = 250;
	static constexpr int32 MaxAsyncDeadlineMs = 120000;
	static constexpr int32 MinAsyncDeadlineMs = 5000;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioNiagaraManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FHyperAINiagaraCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsUserParameterName(const FString& Name);
	static bool ValidateRenameIdentityAgainstSnapshot(
		const TArray<FHyperAINiagaraUserParameterIdentity>& Parameters,
		const FHyperAINiagaraUserParameterRename& Rename,
		const FHyperAINiagaraUserParameterIdentity*& OutParameter,
		FString& OutError);
	static bool AreFullReferenceDomainsProven(
		bool bExposedStore,
		bool bGraphMetadataAndPins,
		bool bEditorOnlyParameterAdapter,
		bool bUserParameterBindings,
		bool bRendererAttributeBindings,
		bool bSystemEmitterHandleRenameClosure,
		bool bReflectionAndCollectionsBoundedBeforeMaterialization);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString InspectPayloadSchemaFingerprint();
	static FString RenamePayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	/** Testable, exact no-load seam used by independent validation. */
	static const UNiagaraValidationRuleSet* ResolveAlreadyLoadedRuleSet(
		const TSoftObjectPtr<UNiagaraValidationRuleSet>& RuleSet);
	/** Closed UE 5.8 native class authority; subclasses/custom rules are not admitted. */
	static bool IsSealedNativeValidationRuleClass(const UClass* Class);
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool CaptureExact(
		const FString& TargetPath,
		int32 MaxWorkMs,
		FHyperAIStudioNiagaraValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FHyperAINiagaraInspectReport Inspect(const FHyperAINiagaraInspectRequest& Request);
	static FHyperAINiagaraValidateReport Validate(const FHyperAINiagaraValidateRequest& Request);
	static FHyperAINiagaraApplyPlanReport BuildPlan(const FHyperAINiagaraApplyPlanRequest& Request);
	static FString ComputeRenameSemanticFingerprint(const FHyperAIStudioNiagaraRenamePayload& Payload);
};

class FHyperAIStudioNiagaraRegistration final
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
	TSharedPtr<FHyperAIStudioNiagaraDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
