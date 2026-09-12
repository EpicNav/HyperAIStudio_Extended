// Games by Hyper 2026.

#pragma once

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecutionService.h"

struct FHyperAIStudioTrustedCatalogAuthoritySnapshot
{
	FHyperAIStudioCapabilityCatalog Catalog;
	uint64 Generation = 0;
};

/** Private injectable authority seam. Production construction is fixed to the generated catalog. */
class IHyperAIStudioTrustedCatalogAuthority
{
public:
	virtual ~IHyperAIStudioTrustedCatalogAuthority() = default;
	virtual bool Snapshot(
		FHyperAIStudioTrustedCatalogAuthoritySnapshot& OutSnapshot,
		FString& OutError) const = 0;
};

struct FHyperAIStudioTrustedPluginObservation
{
	bool bInstalled = false;
	bool bEnabled = false;
	bool bRestartRequired = false;
};

/** Read-only engine observation seam. Implementations may never load a module or mutate a project. */
class IHyperAIStudioTrustedPrerequisiteEnvironment
{
public:
	virtual ~IHyperAIStudioTrustedPrerequisiteEnvironment() = default;
	virtual bool ObservePlugin(
		const FString& PluginName,
		FHyperAIStudioTrustedPluginObservation& OutObservation,
		FString& OutError) const = 0;
	virtual bool ObserveModuleLoaded(
		const FString& ModuleName,
		bool& bOutLoaded,
		FString& OutError) const = 0;
	virtual bool AreSourceCandidateToolsEnabled() const = 0;
};

struct FHyperAIStudioTrustedExecutionHostState;

/** Testable core implementation; optional modules see only FHyperAIStudioTrustedExecutionFacade. */
class FHyperAIStudioTrustedExecutionHost final
{
public:
	FHyperAIStudioTrustedExecutionHost(
		const FString& TrustedProjectRoot,
		const TSharedRef<IHyperAIStudioTrustedCatalogAuthority, ESPMode::ThreadSafe>& CatalogAuthority,
		const TSharedRef<IHyperAIStudioTrustedPrerequisiteEnvironment, ESPMode::ThreadSafe>& Environment,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock,
		TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> AuthorizationGate = nullptr);
	~FHyperAIStudioTrustedExecutionHost();

	FHyperAIStudioTrustedExecutionHost(const FHyperAIStudioTrustedExecutionHost&) = delete;
	FHyperAIStudioTrustedExecutionHost& operator=(const FHyperAIStudioTrustedExecutionHost&) = delete;

	bool Startup(FString& OutError);
	void Shutdown();
	bool IsAvailable() const;
	bool CanShutdownSafely() const;

	bool RegisterAdapter(
		const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
		FHyperAIStudioDomainRegistrationHandle& OutHandle,
		FString& OutError);
	EHyperAIStudioDomainUnregisterResult UnregisterAdapter(
		const FHyperAIStudioDomainRegistrationHandle& Handle,
		FString& OutError);
	bool RegisterLiveProbe(
		const FHyperAIStudioDomainRegistrationHandle& OwnerAdapter,
		const FString& ProbeId,
		FHyperAIStudioTrustedProbeRegistrationHandle& OutHandle,
		FString& OutError);
	bool PublishLiveProbeExact(
		const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
		const FHyperAIStudioTrustedProbeResult& Observation,
		FString& OutError);
	bool UnregisterLiveProbe(
		const FHyperAIStudioTrustedProbeRegistrationHandle& Handle,
		FString& OutError);

	bool PrepareDryRun(
		const FHyperAIStudioTrustedArtifactRequest& Request,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		const TSharedRef<IHyperAIStudioTrustedFreshVerifier, ESPMode::ThreadSafe>& FreshVerifier,
		FHyperAIStudioTrustedPreparedArtifact& OutPrepared,
		FHyperAIStudioTrustedPrepareReport& OutReport,
		FString& OutError);
	bool StageExact(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError);
	bool SubmitExact(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& AuthorizationToken,
		FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
		FHyperAIStudioTrustedExecutionDiagnostic& OutStatus,
		FString& OutError);
	/**
	 * Private non-AICallable server boundary. Core UI invokes this only after an explicit user
	 * approval; the exact live stage supplies every authority-bearing field.
	 */
	bool IssueAuthorizationFromCoreUiApproval(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FString& OutAuthorizationToken,
		FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
	/** Narrow test seam over the production gate; it cannot issue or consume a grant. */
	bool InspectCoreAuthorizationGrantForTests(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& AuthorizationToken,
		FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
		FString& OutError) const;
#endif
	bool QueryStatus(
		const FString& OperationId,
		FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
		FString& OutError) const;

private:
	TSharedPtr<FHyperAIStudioTrustedExecutionHostState, ESPMode::ThreadSafe> State;
};

namespace HyperAIStudio::TrustedExecution::Private
{
	/** Module-owned lifecycle. No optional pack can replace these production authorities. */
	bool StartupCore(const FString& TrustedProjectRoot, FString& OutError);
	void BeginEnginePreExit();
	void ShutdownCore();
	TSharedPtr<FHyperAIStudioTrustedExecutionHost, ESPMode::ThreadSafe> GetCoreHost();
	/** Private core-UI hook; deliberately absent from the exported optional-pack facade. */
	bool IssueCoreUiAuthorizationGrant(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FString& OutAuthorizationToken,
		FString& OutError);
}
