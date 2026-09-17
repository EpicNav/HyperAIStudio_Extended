// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTrustedExecution.h"

/** What the chat panel shows the user before an agent edit runs. */
struct FHyperAIStudioApprovalSummary
{
	FString PackId;
	FString ToolName;
	FString VariantId;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Edit;
	/** Object path of the asset the plan changes. */
	FString EffectTarget;
	/** The plan hash the agent echoed back, shown so a reviewed plan is the one that runs. */
	FString PlanHash;
	/** One line per effect, e.g. "set_module_enabled on Fire/ParticleUpdateScript/Colour". */
	TArray<FString> Effects;
	/** Assets the plan overwrites or creates, if any. */
	TArray<FString> Touches;
};

struct FHyperAIStudioPendingApproval
{
	FString OperationId;
	FHyperAIStudioApprovalSummary Summary;
	FDateTime RequestedUtc = FDateTime::MinValue();
};

/**
 * Holds an agent's prepared mutation until the user approves it in the chat panel.
 *
 * The plan is held prepared, not staged: a staged artifact expires in seconds, while a person may
 * take minutes. Staging and submitting happen on approval, so the reviewed plan is the one that
 * runs, and a risky variant's server grant is issued only from that click.
 *
 * Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioApprovalGate
{
public:
	static constexpr int32 MaxPending = 16;

	/** True when this safety class must be approved before it runs, per the user's settings. */
	static bool IsApprovalRequired(EHyperAIStudioDomainSafety Safety);

	/**
	 * Queue a prepared plan for approval. The pack should report awaiting_user_approval to the agent
	 * and stop; the gate submits it if and when the user approves.
	 */
	static bool Request(
		const FHyperAIStudioTrustedPreparedArtifact& Prepared,
		const FString& OperationId,
		const FHyperAIStudioApprovalSummary& Summary,
		FString& OutError);

	static TArray<FHyperAIStudioPendingApproval> GetPending();
	static bool HasPending(const FString& OperationId);

	/** Stage and submit the held plan. On failure the request is dropped and OutError explains why. */
	static bool Approve(const FString& OperationId, FString& OutError);
	static void Reject(const FString& OperationId, const FString& Reason);
	static void Clear();

	static FSimpleMulticastDelegate& OnChanged();
};
