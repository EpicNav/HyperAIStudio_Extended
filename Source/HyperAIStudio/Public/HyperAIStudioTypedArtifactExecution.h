// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"

/** Hard shared bounds; optional packs may only request smaller envelopes. */
struct FHyperAIStudioTypedArtifactLimits
{
	static constexpr int32 MaxStagedArtifacts = 32;
	static constexpr int64 MaxStageLifetimeMs = 30 * 1000;
	/** This synchronous core slice is deliberately not a long/awaiting workflow host. */
	static constexpr int32 MaxSynchronousDeadlineMs = 2000;
	static constexpr int32 MaxSynchronousGameThreadMs = 250;
	static constexpr int32 MaxEffectTargetChars = 512;
	static constexpr int32 MaxDiagnosticChars = 2048;
};

/**
 * A concrete optional-pack DTO implements this in addition to the normal domain payload contract.
 * Its semantic fingerprint must be recomputed from its closed typed fields; it is never a hash of
 * raw client JSON, a script body, or a delegated tool name.
 */
class HYPERAISTUDIO_API IHyperAIStudioTypedArtifactPayload : public IHyperAIStudioDomainRequestPayload
{
public:
	virtual FString GetSemanticFingerprint() const = 0;
	/** Return a detached, deep immutable execution snapshot; returning this object is rejected. */
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const = 0;
};

/** Closed compile/save/verify contract prepared by core before an artifact can be staged. */
struct FHyperAIStudioTypedArtifactContract
{
	FHyperAIStudioDomainBinding Binding;
	FString ArtifactTypeId;
	FString ArtifactSchemaFingerprint;
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

/** Core-sealed hashes; callers must return this exact object at stage and claim. */
struct FHyperAIStudioPreparedTypedArtifact
{
	FHyperAIStudioTypedArtifactContract Contract;
	FString ContractFingerprint;
	FString BindingFingerprint;
	FString CapabilityHash;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString EffectFingerprint;
};

struct FHyperAIStudioTypedArtifactStageReceipt
{
	FString StageId;
	FString CanonicalProjectId;
	FString OperationId;
	FString PackId;
	FString ToolName;
	FString VariantId;
	FString ArtifactTypeId;
	FString ArtifactSchemaFingerprint;
	FString ArtifactSemanticFingerprint;
	FString AdapterFingerprint;
	FString AdmissionFingerprint;
	FString PrerequisiteFingerprint;
	FString ContractFingerprint;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
	int64 ExpiresUtcMs = 0;
};

enum class EHyperAIStudioTypedArtifactExecutionState : uint8
{
	Completed,
	ReplayCompleted,
	Failed,
	Partial,
	RolledBack,
	OutcomeUnknown
};

struct FHyperAIStudioTypedArtifactExecutionResult
{
	EHyperAIStudioTypedArtifactExecutionState State = EHyperAIStudioTypedArtifactExecutionState::Failed;
	FString Status;
	FString Diagnostic;
	FString OperationId;
	FString CanonicalProjectId;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	int32 CompletedActionCount = 0;
	int32 ScheduledActionCount = 0;
	int32 NativeOperationCount = 0;
	int32 GameThreadMs = 0;
	int32 OutputBytes = 0;
	bool bReplay = false;
	bool bOutcomeUnknown = false;
	/** Deliberately invariant: an admitted/partial/unknown execution never selects fallback. */
	bool bFallbackPermitted = false;
};

/** Bounded public status DTO; only TrustedExecutionFacade can drive its mutation lifecycle. */
struct FHyperAIStudioTypedArtifactOperationStatus
{
	bool bFound = false;
	bool bAccepted = false;
	bool bTerminal = false;
	bool bReplay = false;
	bool bOutcomeUnknown = false;
	bool bDurable = false;
	bool bReconciled = false;
	bool bFallbackPermitted = false;
	FString Status;
	FString Diagnostic;
	FString StageId;
	FString OperationId;
	FString CanonicalProjectId;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString ActiveActionKind;
	FString ActiveStepId;
	int32 CompletedActionCount = 0;
	int32 ScheduledActionCount = 0;
	int32 NativeOperationCount = 0;
	int32 GameThreadMs = 0;
	int32 OutputBytes = 0;
	int32 LateResultCount = 0;
};

/** Immediate exact response DTO. The private core host never pumps mutation before returning it. */
struct FHyperAIStudioTypedArtifactSubmissionReceipt
{
	bool bOk = false;
	FHyperAIStudioTypedArtifactStageReceipt Stage;
	FHyperAIStudioTypedArtifactOperationStatus Operation;
};

/**
 * Public pure sealer only. Store, claim and execution services are private core implementation;
 * optional packs enter mutation exclusively through FHyperAIStudioTrustedExecutionFacade.
 */
class HYPERAISTUDIO_API FHyperAIStudioTypedArtifactExecutor final
{
public:
	/** Pure dry-run/sealing operation. No adapter is loaded and no artifact is staged. */
	static bool Prepare(
		const FHyperAIStudioTypedArtifactContract& Contract,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError);
};
