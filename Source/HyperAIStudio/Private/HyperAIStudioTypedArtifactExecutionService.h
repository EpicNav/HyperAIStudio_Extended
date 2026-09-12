// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioTypedArtifactExecutionInternal.h"

struct FHyperAIStudioTypedArtifactExecutionServiceState;

struct FHyperAIStudioTypedArtifactHostLimits
{
	static constexpr int32 MaxArchivedStatuses = 64;
	static constexpr int32 MaxRejectedSubmissionReceipts = 32;
	static constexpr int32 MaxStatusDiagnosticChars = 2048;
};

/** Private one-lane core host. Optional modules can use only TrustedExecutionFacade. */
class FHyperAIStudioTypedArtifactExecutionService final
{
public:
	FHyperAIStudioTypedArtifactExecutionService(
		const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>& Registry,
		const FString& TrustedProjectRoot);
	FHyperAIStudioTypedArtifactExecutionService(
		const TSharedRef<FHyperAIStudioDomainAdapterRegistry, ESPMode::ThreadSafe>& Registry,
		const FString& TrustedProjectRoot,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock);
	~FHyperAIStudioTypedArtifactExecutionService();

	FHyperAIStudioTypedArtifactExecutionService(
		const FHyperAIStudioTypedArtifactExecutionService&) = delete;
	FHyperAIStudioTypedArtifactExecutionService& operator=(
		const FHyperAIStudioTypedArtifactExecutionService&) = delete;

	bool Startup(FString& OutError);
	bool StageExact(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const FString& OperationId,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
		FString& OutError);
	bool SubmitExact(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		const FString& AuthorizationToken,
		const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
		FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
		FString& OutError);
	bool StageAndSubmitExact(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const FString& OperationId,
		const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		const FString& AuthorizationToken,
		const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
		FHyperAIStudioTypedArtifactSubmissionReceipt& OutSubmission,
		FString& OutError);
	bool QueryStatus(
		const FString& OperationId,
		FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
		FString& OutError) const;
	bool Cancel(
		const FString& OperationId,
		const FString& Reason,
		FHyperAIStudioTypedArtifactOperationStatus& OutStatus,
		FString& OutError);
	int32 NumStaged() const;
	bool IsQuiescing() const;
	bool CanShutdownSafely() const;
	void Shutdown();

private:
	TSharedPtr<FHyperAIStudioTypedArtifactExecutionServiceState, ESPMode::ThreadSafe> State;
};
