// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPlanValidateToolset.generated.h"

/** One bounded validation diagnostic. */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidateDiagnostic
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString Message;
};

/** Declared aggregate envelope and the budget consumed by the derived schedule. */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidateBudget
{
	GENERATED_BODY()

	UPROPERTY()
	int32 DeadlineMs = 0;

	UPROPERTY()
	int32 MaxSteps = 0;

	UPROPERTY()
	int32 MaxMutations = 0;

	UPROPERTY()
	int32 MaxNativeOperations = 0;

	UPROPERTY()
	int32 MaxGameThreadMs = 0;

	UPROPERTY()
	int32 MaxOutputBytes = 0;

	UPROPERTY()
	int32 PlannedNativeOperations = 0;

	UPROPERTY()
	int32 PlannedGameThreadMs = 0;

	UPROPERTY()
	int32 PlannedOutputBytes = 0;
};

/** One immutable action in the serial schedule produced by validation. */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidateScheduleItem
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Order = 0;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString StepId;

	UPROPERTY()
	FString OperationType;

	UPROPERTY()
	FString Safety;

	UPROPERTY()
	int32 MaxNativeOperations = 0;

	UPROPERTY()
	int32 MaxGameThreadMs = 0;

	UPROPERTY()
	int32 MaxOutputBytes = 0;
};

/** One target-bound precondition from a validated typed step. */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidatePrecondition
{
	GENERATED_BODY()

	UPROPERTY()
	FString StepId;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString Target;

	UPROPERTY()
	FString Field;

	UPROPERTY()
	FString Expected;
};

/** One target-bound declared effect from a validated typed step. */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidateEffect
{
	GENERATED_BODY()

	UPROPERTY()
	FString StepId;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString Target;

	UPROPERTY()
	FString ValidatorId;

	UPROPERTY()
	FString Expected;
};

/**
 * Project-bound, validation-only response. It deliberately has no bearer-token,
 * executor, raw-dispatch, script, filesystem-write, or fallback fields.
 */
USTRUCT(BlueprintType)
struct FHyperAIPlanValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Schema = TEXT("hyperai.plan-validation.v1");

	/** This callable always performs a dry-run, even when inspecting an execute-mode read plan. */
	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bNoMutation = true;

	UPROPERTY()
	bool bExecutionPerformed = false;

	UPROPERTY()
	bool bAuthorizationIssued = false;

	UPROPERTY()
	bool bInputPlanDryRunKnown = false;

	UPROPERTY()
	bool bInputPlanDryRun = false;

	/** Strong path-independent identity hash; the project path itself is never returned. */
	UPROPERTY()
	FString ProjectIdentityHash;

	/** Binds project, plan, operation registry, catalog, effects, and selected backend. */
	UPROPERTY()
	FString ProjectBindingFingerprint;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString CanonicalPlanHash;

	UPROPERTY()
	bool bExpectedPlanHashProvided = false;

	UPROPERTY()
	bool bExpectedPlanHashMatched = false;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString CatalogFingerprint;

	UPROPERTY()
	FString CapabilityFingerprint;

	UPROPERTY()
	FString SelectedBackend;

	UPROPERTY()
	bool bSelectedBackendValidationOnly = true;

	UPROPERTY()
	FString MaximumSafety;

	UPROPERTY()
	bool bWouldMutate = false;

	UPROPERTY()
	bool bRequiresOperationId = false;

	UPROPERTY()
	bool bRequiresAuthorization = false;

	UPROPERTY()
	bool bRequiresDestructiveAuthorization = false;

	UPROPERTY()
	bool bRequiresExternalEffectAuthorization = false;

	UPROPERTY()
	int32 StepCount = 0;

	UPROPERTY()
	int32 MutationStepCount = 0;

	UPROPERTY()
	FHyperAIPlanValidateBudget Budget;

	UPROPERTY()
	TArray<FString> OrderedStepIds;

	UPROPERTY()
	TArray<FHyperAIPlanValidateScheduleItem> OrderedSchedule;

	UPROPERTY()
	TArray<FHyperAIPlanValidatePrecondition> Preconditions;

	UPROPERTY()
	TArray<FHyperAIPlanValidateEffect> Effects;

	UPROPERTY()
	TArray<FHyperAIPlanValidateDiagnostic> Diagnostics;
};

/** A one-function Epic ToolsetRegistry class; validation never executes the plan. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioPlanValidateToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Validate one strict bounded typed plan against the current native operation registry and catalog. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Workflow")
	static FHyperAIPlanValidateReport hyper_plan_validate(
		const FString& PlanJson,
		const FString& ExpectedPlanHash = TEXT(""));
};

enum class EHyperAIStudioPlanValidateAdmissionState : uint8
{
	Planned,
	SourceCandidate,
	Admitted
};

/** Server-owned build evidence; it is never supplied through the MCP call. */
struct FHyperAIStudioPlanValidateAdmissionEvidence
{
	FString ToolName;
	FString QualifiedToolsetName;
	EHyperAIStudioPlanValidateAdmissionState AdmissionState =
		EHyperAIStudioPlanValidateAdmissionState::Planned;
	FString CatalogFingerprint;
	FString CapabilityFingerprint;
	int32 SourceArtifactCount = 0;
	FString SourceArtifactFingerprint;
};

struct FHyperAIStudioCapabilityToolDefinition;

/** Pure contract helpers used by the callable and automation coverage. */
class FHyperAIStudioPlanValidateContracts final
{
public:
	static constexpr int32 MaxExpectedHashCharacters = 71;
	static constexpr int32 MaxDiagnosticCharacters = 512;
	static constexpr int32 MaxBackendCharacters = 96;
	static constexpr int32 MaxHashInputUtf8Bytes = 16 * 1024;

	static FString GetQualifiedToolsetName();
	static FString GetSelectedBackend();
	static bool IsPendingNativeToolsTestEnabled();
	/** Bounded standards-based SHA-256 used for project-validation bindings. */
	static FString ComputeBoundedSha256(const FString& Value);

	/** Pure after CanonicalProjectId is supplied; no journal, token, executor, module load, or mutation seam is reachable. */
	static FHyperAIPlanValidateReport ValidateForProject(
		const FString& PlanJson,
		const FString& ExpectedPlanHash,
		const FString& CanonicalProjectId);

	static FString BuildProjectBindingFingerprint(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		const FString& AuthorizationPlanHash,
		const FString& EffectFingerprint,
		const FString& CapabilityFingerprint,
		const FString& CatalogFingerprint,
		const FString& SelectedBackend);

	/** The checked-in state remains SourceCandidate until exact packaged/live evidence is frozen. */
	static FHyperAIStudioPlanValidateAdmissionEvidence GetCompiledAdmissionEvidence();

	/** Pure one-tool policy. Dev mode admits only SourceCandidate/Admitted builds with exact identities. */
	static bool IsRegistrationEvidenceAllowed(
		const FHyperAIStudioPlanValidateAdmissionEvidence& Evidence,
		const FHyperAIStudioCapabilityToolDefinition& CatalogTool,
		const FString& CurrentCatalogFingerprint,
		const FString& CurrentCapabilityFingerprint,
		bool bAllowSourceCandidateForDev);

	/** Resolves the immutable built-in catalog and applies the one-tool policy. */
	static bool IsRegistrationAllowed(
		const FHyperAIStudioPlanValidateAdmissionEvidence& Evidence,
		bool bAllowSourceCandidateForDev);
};

/** Owned fail-closed registration; root bootstrap may call Startup/Shutdown later. */
class FHyperAIStudioPlanValidateToolRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
