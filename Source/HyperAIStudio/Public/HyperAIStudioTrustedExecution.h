// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioTypedArtifactExecution.h"

struct FHyperAIStudioTrustedPreparedArtifactState;

enum class EHyperAIStudioTrustedCatalogAdmission : uint8
{
	Planned,
	SourceCandidate,
	Admitted
};

struct FHyperAIStudioTrustedExecutionLimits
{
	static constexpr int32 MaxProbeIdChars = 128;
	static constexpr int32 MaxProbeDiagnosticChars = 512;
	static constexpr int32 MaxRegisteredProbes = 64;
	static constexpr int32 MaxActiveAuthorizationGrants = 64;
	static constexpr int32 MaxTrustedPreparations = 64;
	static constexpr int32 MaxTrustedStageContexts = 32;
	static constexpr int64 ProbeFreshnessMs = 5000;
	static constexpr int64 AuthorizationGrantLifetimeMs = 60 * 1000;
};

/**
 * A probe publication reports only a bounded cached live fact. Plugin scans, module loads and
 * editor work belong in an asynchronous producer; preparation never calls optional probe code.
 */
struct FHyperAIStudioTrustedProbeResult
{
	bool bReady = false;
	FString StatusCode;
	FString Diagnostic;
};

struct FHyperAIStudioTrustedProbeRegistrationHandle
{
	FString ProbeId;
	FString OwnerPackId;
	FString OwnerAdapterId;
	FString OwnerAdapterFingerprint;
	uint64 OwnerAdapterGeneration = 0;
	uint64 Generation = 0;

	bool IsValid() const
	{
		return !ProbeId.IsEmpty() && !OwnerPackId.IsEmpty() && !OwnerAdapterId.IsEmpty()
			&& !OwnerAdapterFingerprint.IsEmpty()
			&& OwnerAdapterGeneration != 0 && Generation != 0;
	}
};

/** Domain-owned fresh-result verifier. It cannot author readiness, admission, paths or dispatch. */
class HYPERAISTUDIO_API IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual ~IHyperAIStudioTrustedFreshVerifier() = default;
	virtual FString GetOwnerAdapterFingerprint() const = 0;
	/** Resolve a canonical domain target from the immutable DTO; caller text is only an assertion. */
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) = 0;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) = 0;
};

/** Caller-selected semantic intent and budgets. Every authority-bearing field is built by core. */
struct FHyperAIStudioTrustedArtifactRequest
{
	FString PackId;
	FString ToolName;
	FString VariantId;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit;
	FString ArtifactSemanticFingerprint;
	FString EffectTarget;
	int32 DeadlineMs = 1000;
	int32 MaxNativeOperations = 64;
	int32 MaxGameThreadMs = 200;
	int32 MaxOutputBytes = 256 * 1024;
	int32 MaxResultBytes = 256;
	int64 StageLifetimeMs = 15000;
	bool bCompileOnce = false;
	bool bSaveOnce = true;
	bool bValidateOnce = true;
	bool bVerifyFreshOnce = true;
};

struct FHyperAIStudioTrustedExecutionDiagnostic
{
	FString StatusCode;
	FString Diagnostic;
	bool bPrerequisiteConflict = false;
	bool bAdmissionConflict = false;
	TArray<FString> BlockingPrerequisiteIds;
};

struct FHyperAIStudioTrustedPrepareReport
{
	bool bPrepared = false;
	bool bMutationAdmitted = false;
	/** Legacy field name: true for an exact SourceCandidate Preview/automation-override route. */
	bool bPreAdmissionEvidenceExecution = false;
	EHyperAIStudioTrustedCatalogAdmission Admission =
		EHyperAIStudioTrustedCatalogAdmission::Planned;
	FString ContractFingerprint;
	FString PlanHash;
	FString CatalogFingerprint;
	uint64 CatalogGeneration = 0;
	FHyperAIStudioTrustedExecutionDiagnostic Status;
};

/** Opaque immutable core preparation. Optional modules cannot inspect or replace its binding. */
class HYPERAISTUDIO_API FHyperAIStudioTrustedPreparedArtifact final
{
public:
	FHyperAIStudioTrustedPreparedArtifact() = default;
	bool IsValid() const { return State.IsValid(); }
	void Reset() { State.Reset(); }

private:
	friend class FHyperAIStudioTrustedExecutionFacade;
	friend class FHyperAIStudioTrustedExecutionHost;
	TSharedPtr<const FHyperAIStudioTrustedPreparedArtifactState, ESPMode::ThreadSafe> State;
};

/**
 * Narrow production facade for optional packs. It exposes no project identity, snapshots,
 * registry, state gate, loader, raw dispatcher, script or filesystem route.
 */
class HYPERAISTUDIO_API FHyperAIStudioTrustedExecutionFacade final
{
public:
	static bool IsAvailable();

	/** Registration is catalog-exact. SourceCandidate registration additionally requires dev mode. */
	static bool RegisterAdapter(
		const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
		FHyperAIStudioDomainRegistrationHandle& OutHandle,
		FString& OutError);
	static EHyperAIStudioDomainUnregisterResult UnregisterAdapter(
		const FHyperAIStudioDomainRegistrationHandle& Handle,
		FString& OutError);

	static bool RegisterLiveProbe(
		const FHyperAIStudioDomainRegistrationHandle& OwnerAdapter,
		const FString& ProbeId,
		FHyperAIStudioTrustedProbeRegistrationHandle& OutHandle,
		FString& OutError);
	/** Publish a value-only cached observation; production prepare paths never call pack code. */
	static bool PublishLiveProbeExact(
		const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
		const FHyperAIStudioTrustedProbeResult& Observation,
		FString& OutError);
	static bool UnregisterLiveProbe(
		const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
		FString& OutError);

	/**
	 * Pure adapter dry-run/sealing. SourceCandidate mutates only in a non-shipping
	 * pre_admission_candidate process under the explicit existing development gate.
	 */
	static bool PrepareDryRun(
		const FHyperAIStudioTrustedArtifactRequest& Request,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		const TSharedRef<IHyperAIStudioTrustedFreshVerifier, ESPMode::ThreadSafe>& FreshVerifier,
		FHyperAIStudioTrustedPreparedArtifact& OutPrepared,
		FHyperAIStudioTrustedPrepareReport& OutReport,
		FString& OutError);

	/** Exact response-loss-safe stage. OperationId is caller intent; all other identity is sealed. */
	static bool StageExact(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError);

	/** Edit is tokenless. Risky variants accept only an existing trusted server-issued grant. */
	static bool SubmitExact(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& AuthorizationToken,
		FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError);

	static bool QueryStatus(
		const FString& OperationId,
		FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
		FString& OutError);
};
