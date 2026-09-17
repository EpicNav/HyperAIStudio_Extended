// Games by Hyper 2026.

#include "HyperAIStudioFoundationProbe.h"

#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioFoundationProbe, Log, All);

namespace HyperAIStudio::FoundationProbe::Private
{
	class FProbeOwnerAdapter final : public IHyperAIStudioDomainAdapter
	{
	public:
		virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override
		{
			return FHyperAIStudioFoundationProbe::GetDescriptor();
		}

		virtual FHyperAIStudioDomainAdapterResult Execute(
			const FHyperAIStudioDomainDispatchContext& Context,
			const IHyperAIStudioDomainRequestPayload& Payload) override
		{
			FHyperAIStudioDomainAdapterResult Result;
			Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
			Result.StatusCode = TEXT("probe_owner_only");
			Result.Diagnostic = TEXT("This adapter only owns probe.live_toolset_registry and never dispatches.");
			return Result;
		}
	};
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioFoundationProbe::GetDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = TEXT("shared_foundation");
		Value.AdapterId = TEXT("adapter.shared_foundation.probe_owner");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_plan_validate"), TEXT("probe_owner.v1"),
			TEXT("hyperai.payload.foundation.probe_owner.v1"),
			FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("foundation.probe_owner.payload.v1|never_dispatched")),
			TEXT("hyperai.result.foundation.probe_owner.v1"),
			FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT("foundation.probe_owner.result.v1|never_dispatched")),
			EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIStudioTrustedProbeResult FHyperAIStudioFoundationProbe::Observe()
{
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = UToolsetRegistry::IsAvailable()
		&& FModuleManager::Get().IsModuleLoaded(TEXT("ToolsetRegistry"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("ModelContextProtocolEngine"));
	Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("toolset_registry_unavailable");
	Observation.Diagnostic = Observation.bReady
		? TEXT("Epic ToolsetRegistry and the MCP engine module are live.")
		: TEXT("Epic ToolsetRegistry or the MCP engine module is not live; typed mutations stay blocked.");
	return Observation;
}

void FHyperAIStudioFoundationProbe::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioFoundationProbe::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioFoundationProbe::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBack();
	bStarted = false;
}

void FHyperAIStudioFoundationProbe::RegisterAfterEngineInit()
{
	if (!bStarted || Adapter.IsValid() || !IsInGameThread() || IsEngineExitRequested()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}

	Adapter = MakeShared<HyperAIStudio::FoundationProbe::Private::FProbeOwnerAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioFoundationProbe, Warning,
			TEXT("Foundation probe owner registration failed closed; optional-pack mutations stay blocked: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, ProbeId, ProbeHandle, Error)
		|| !Publisher.Start(ProbeHandle, &FHyperAIStudioFoundationProbe::Observe, Error))
	{
		UE_LOG(LogHyperAIStudioFoundationProbe, Warning,
			TEXT("probe.live_toolset_registry publication failed closed; optional-pack mutations stay blocked: %s"), *Error);
		RollBack();
		return;
	}
	UE_LOG(LogHyperAIStudioFoundationProbe, Log, TEXT("Publishing probe.live_toolset_registry."));
}

void FHyperAIStudioFoundationProbe::RollBack()
{
	if (!IsInGameThread())
	{
		return;
	}
	Publisher.Stop();
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioFoundationProbe, Error, TEXT("Foundation probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioFoundationProbe, Error, TEXT("Foundation adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
	}
	Adapter.Reset();
}
