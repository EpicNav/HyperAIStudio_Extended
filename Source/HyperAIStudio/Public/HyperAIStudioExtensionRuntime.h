// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioSettings.h"

enum class EHyperAIStudioDomainSafety : uint8;

/** Public, dependency-free admission state exposed to optional HyperAIStudio editor modules. */
enum class EHyperAIStudioExtensionAdmissionState : uint8
{
	Invalid,
	Planned,
	SourceCandidate,
	Admitted
};

/** Exact generated-catalog result for one atomic optional-module cohort. */
struct HYPERAISTUDIO_API FHyperAIStudioExtensionCohortAdmission
{
	bool bCatalogValid = false;
	bool bExactCohortMatch = false;
	bool bOptionalPack = false;
	EHyperAIStudioExtensionAdmissionState State =
		EHyperAIStudioExtensionAdmissionState::Invalid;
	FString CatalogFingerprint;
};

/**
 * Narrow core facade for capability-gated editor modules.
 *
 * Optional modules must not include HyperAIStudio private headers. This facade deliberately exposes
 * only immutable catalog admission and bounded identity helpers; it is not a raw dispatcher or an
 * execution-backend escape hatch.
 */
class HYPERAISTUDIO_API FHyperAIStudioExtensionRuntime final
{
public:
	static constexpr int32 MaxHashInputBytes = 1024 * 1024;

	/** Legacy compatibility query. Prefer AreSourceCandidateToolsEnabled. */
	static bool IsPendingNativeToolsDevModeEnabled();

	/** Configured user-facing tool set. The underlying enum names are retained for config compatibility. */
	static EHyperAIStudioNativeToolChannel GetNativeToolChannel();

	/** True when HyperAI tools are enabled in addition to Epic Unreal MCP. */
	static bool AreExtendedHyperToolsEnabled();
	static bool AreExtendedHyperToolsEnabled(
		EHyperAIStudioNativeToolChannel Channel,
		bool bLegacyDevAutomationOverride);

	/** True for Preview, or for the legacy dev-automation command-line override. */
	static bool AreSourceCandidateToolsEnabled();
	static bool AreSourceCandidateToolsEnabled(
		EHyperAIStudioNativeToolChannel Channel,
		bool bLegacyDevAutomationOverride);

	/** Current project policy. Fast is the default; Strict Safety is an explicit opt-in. */
	static EHyperAIStudioNativeExecutionMode GetNativeExecutionMode();

	/**
	 * Pure policy query for optional packs. Destructive and external effects are always strict;
	 * Fast only relaxes repeated validation for reads and reversible editor transactions.
	 */
	static bool UsesStrictNativeSafety(EHyperAIStudioDomainSafety Safety);
	static bool UsesStrictNativeSafety(
		EHyperAIStudioNativeExecutionMode Mode,
		EHyperAIStudioDomainSafety Safety);

	/** Resolve one exact generated pack/cohort/tool-name set without loading any optional module. */
	static bool QueryExactGeneratedCohort(
		const FString& PackId,
		const FString& AtomicCohortId,
		const TArray<FString>& ExactToolNames,
		FHyperAIStudioExtensionCohortAdmission& OutAdmission);

	/** Exact generated cohorts register only when the Extended Hyper Tools set is enabled. */
	static bool IsExactGeneratedCohortRegistrationAllowed(
		const FString& PackId,
		const FString& AtomicCohortId,
		const TArray<FString>& ExactToolNames,
		bool bAllowSourceCandidateForDev);

	/** Canonical lowercase sha256:<64 hex>, or empty for malformed UTF-16 / oversized input. */
	static FString ComputeBoundedSha256(const FString& Value);

	/** Shared journal-safe operation-id grammar. */
	static bool IsValidOperationId(const FString& OperationId);

	/** Strong current-project filesystem identity, or empty when it cannot be proven. */
	static FString GetCanonicalProjectId();
};
