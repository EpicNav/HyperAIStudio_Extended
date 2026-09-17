// Games by Hyper 2026.

#pragma once

#include "Containers/Ticker.h"
#include "HyperAIStudioTrustedExecution.h"
#include "Templates/UnrealTemplate.h"

/**
 * Keeps one live probe observation fresh. The trusted host treats an observation older than
 * FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs as missing, and never calls pack code to refresh it, so an
 * owner that publishes once at startup blocks every typed mutation a few seconds later. Game thread only; the
 * publisher must stay at a stable address while started.
 */
class FHyperAIStudioLiveProbePublisher : FNoncopyable
{
public:
	using FObserve = TFunction<FHyperAIStudioTrustedProbeResult()>;

	~FHyperAIStudioLiveProbePublisher()
	{
		Stop();
	}

	/** Publishes once immediately, then on a timer. Returns false, and does not start, if the first publish fails. */
	bool Start(const FHyperAIStudioTrustedProbeRegistrationHandle& InHandle, FObserve InObserve, FString& OutError)
	{
		Stop();
		Handle = InHandle;
		Observe = MoveTemp(InObserve);
		if (!Publish(OutError))
		{
			Observe = nullptr;
			return false;
		}
		// A third of the freshness window, so a frame hitch or one failed refresh cannot let the observation lapse.
		const float IntervalSeconds = static_cast<float>(FHyperAIStudioTrustedExecutionLimits::ProbeFreshnessMs) / 3000.0f;
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			FString Ignored;
			// A refresh that fails simply lets the observation go stale, which blocks mutations: fail closed.
			Publish(Ignored);
			return true;
		}), IntervalSeconds);
		return true;
	}

	/** Stop refreshing before unregistering the probe handle. */
	void Stop()
	{
		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
		Observe = nullptr;
	}

private:
	bool Publish(FString& OutError)
	{
		return Observe && FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(Handle, Observe(), OutError);
	}

	FHyperAIStudioTrustedProbeRegistrationHandle Handle;
	FObserve Observe;
	FTSTicker::FDelegateHandle TickerHandle;
};
