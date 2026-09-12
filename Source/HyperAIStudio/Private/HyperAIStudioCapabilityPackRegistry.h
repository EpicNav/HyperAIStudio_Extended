// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

enum class EHyperAIStudioCapabilityPackTier : uint8
{
	Core,
	Optional
};

enum class EHyperAIStudioCapabilityAdmissionState : uint8
{
	Planned,
	SourceCandidate,
	Admitted
};

enum class EHyperAIStudioCapabilityPrerequisiteKind : uint8
{
	Plugin,
	Module,
	Probe
};

enum class EHyperAIStudioCapabilityRequirementMode : uint8
{
	AllOf,
	AnyOf
};

enum class EHyperAIStudioCapabilityLedgerCoverage : uint8
{
	MappedSourceRows,
	ApprovedPlanExtension
};

/** Generated safety authority for each compact HyperAI tool contract. */
enum class EHyperAIStudioCapabilitySafetyClass : uint8
{
	Read,
	Edit,
	Destructive,
	ExternalEffect
};

enum class EHyperAIStudioCapabilityPackStatus : uint8
{
	Ready,
	Disabled,
	Unavailable,
	RestartRequired,
	PendingAdmission
};

struct FHyperAIStudioCapabilityPrerequisiteDefinition
{
	FString Id;
	EHyperAIStudioCapabilityPrerequisiteKind Kind = EHyperAIStudioCapabilityPrerequisiteKind::Probe;
};

struct FHyperAIStudioCapabilityRequirementGroup
{
	FString Id;
	EHyperAIStudioCapabilityRequirementMode Mode = EHyperAIStudioCapabilityRequirementMode::AllOf;
	bool bBlocking = true;
	TArray<FString> PrerequisiteIds;
};

struct FHyperAIStudioCapabilityPackDefinition
{
	FString Id;
	EHyperAIStudioCapabilityPackTier Tier = EHyperAIStudioCapabilityPackTier::Optional;
	EHyperAIStudioCapabilityAdmissionState AdmissionState = EHyperAIStudioCapabilityAdmissionState::Planned;
	bool bContainsExternalEffects = false;
	TArray<FString> DependsOnPackIds;
	TArray<FString> AtomicCohortIds;
	TArray<FString> ToolNames;
	TArray<FHyperAIStudioCapabilityRequirementGroup> Requirements;
};

struct FHyperAIStudioCapabilityToolDefinition
{
	FString Name;
	FString PackId;
	FString AtomicCohortId;
	/** Generated authority; adapters may only assert this exact cohort-wide set. */
	TArray<FString> RequiredNonBlockingRequirementGroupIds;
	/** Exact generated set; every registered typed variant must remain inside this tool-bound set. */
	TArray<EHyperAIStudioCapabilitySafetyClass> AllowedSafetyClasses;
	EHyperAIStudioCapabilityAdmissionState AdmissionState = EHyperAIStudioCapabilityAdmissionState::Planned;
	bool bMayCauseExternalEffects = false;
	int32 SourceArtifactCount = 0;
	FString SourceArtifactFingerprint;
	int32 SourceLedgerRowCount = 0;
	FString SourceLedgerFingerprint;
	EHyperAIStudioCapabilityLedgerCoverage SourceLedgerCoverage =
		EHyperAIStudioCapabilityLedgerCoverage::ApprovedPlanExtension;
	/** Platform, product, and supplemental evidence remain aggregated in shipping metadata. */
	int32 PlatformEvidenceRows = 0;
	int32 ProductEvidenceRows = 0;
	int32 SupplementalEvidenceRows = 0;
};

struct FHyperAIStudioCapabilityCatalog
{
	FString Schema;
	FString GeneratedFingerprint;
	FString ApprovedPlanFingerprint;
	FString AdmissionMatrixFingerprint;
	FString SourceLedgerFingerprint;
	FString SourceArtifactFingerprint;
	int32 SourceArtifactCount = 0;
	TArray<FHyperAIStudioCapabilityPrerequisiteDefinition> Prerequisites;
	TArray<FHyperAIStudioCapabilityPackDefinition> Packs;
	TArray<FHyperAIStudioCapabilityToolDefinition> Tools;
};

// Callers observe state elsewhere and pass it in.  The resolver deliberately
// does not discover plugins or load modules, which keeps it deterministic and
// safe to call from capability-report code.
struct FHyperAIStudioCapabilityPrerequisiteObservation
{
	bool bPluginInstalled = false;
	bool bPluginEnabled = false;
	bool bModuleLoaded = false;
	bool bRestartRequired = false;
	bool bProbeSucceeded = false;
};

struct FHyperAIStudioCapabilityResolveInput
{
	TMap<FString, FHyperAIStudioCapabilityPrerequisiteObservation> Observations;
};

struct FHyperAIStudioCapabilityPackResolution
{
	FString PackId;
	EHyperAIStudioCapabilityPackTier Tier = EHyperAIStudioCapabilityPackTier::Optional;
	EHyperAIStudioCapabilityAdmissionState AdmissionState = EHyperAIStudioCapabilityAdmissionState::Planned;
	EHyperAIStudioCapabilityPackStatus Status = EHyperAIStudioCapabilityPackStatus::Unavailable;
	bool bContainsExternalEffects = false;
	FString DiagnosticCode;
	TArray<FString> BlockingPrerequisiteIds;
	TArray<FString> LimitedVariantPrerequisiteIds;
};

struct FHyperAIStudioCapabilityResolution
{
	bool bCatalogValid = false;
	FString CatalogFingerprint;
	FString ResolutionFingerprint;
	TArray<FString> ValidationErrors;
	TArray<FHyperAIStudioCapabilityPackResolution> Packs;
};

class FHyperAIStudioCapabilityPackRegistry final
{
public:
	static constexpr const TCHAR* CatalogSchema = TEXT("hyperai.capability-pack-catalog.v2");

	static const FHyperAIStudioCapabilityCatalog& GetCatalog();

	/** Recomputes the canonical SHA-256 over every semantic runtime catalog field. */
	static FString ComputeCatalogFingerprint(const FHyperAIStudioCapabilityCatalog& Catalog);

	static bool ValidateCatalog(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		TArray<FString>& OutErrors,
		int32 ExpectedToolCount = INDEX_NONE,
		int32 ExpectedPackCount = INDEX_NONE);

	static bool ValidateBuiltInCatalog(TArray<FString>& OutErrors);

	static FHyperAIStudioCapabilityResolution Resolve(
		const FHyperAIStudioCapabilityResolveInput& Input);

	// Exposed for pure automation fixtures. Production callers should use the
	// immutable generated catalog through Resolve(Input).
	static FHyperAIStudioCapabilityResolution ResolveCatalog(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FHyperAIStudioCapabilityResolveInput& Input);

	static const TCHAR* LexToString(EHyperAIStudioCapabilityPackStatus Status);
	static const TCHAR* LexToString(EHyperAIStudioCapabilityAdmissionState State);
};
