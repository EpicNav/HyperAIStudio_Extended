// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"

/**
 * Publishes probe.live_toolset_registry, a blocking prerequisite of shared_foundation and therefore of every optional
 * pack's typed mutations. The host only lets an adapter of the probe's own pack register it, so this owns a minimal
 * adapter for the single-tool hyper_plan_validate cohort. That adapter never dispatches: hyper_plan_validate keeps
 * running through its own toolset, and the host resolves adapters only by exact typed binding, never by tool name.
 */
class FHyperAIStudioFoundationProbe final
{
public:
	static constexpr const TCHAR* ProbeId = TEXT("probe.live_toolset_registry");

	void Startup();
	void Shutdown();
	bool IsPublishing() const { return ProbeHandle.IsValid(); }

	static const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor();
	static FHyperAIStudioTrustedProbeResult Observe();

private:
	void RegisterAfterEngineInit();
	void RollBack();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher Publisher;
	bool bStarted = false;
};
