// Games by Hyper 2026.

#include "HyperAIStudioAudioToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudioModule, Log, All);

/** Explicit owner for the descriptor's LoadingPhase=None three-tool audio cohort. */
class FHyperAIStudioAudioModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioAudioToolset::StaticClass(),
			FHyperAIStudioAudioContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudioModule, Error,
				TEXT("Audio optional publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioAudioRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			Registration.Reset();
		}
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioAudioToolset::StaticClass(),
				FHyperAIStudioAudioContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioAudioRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAudioModule, HyperAIStudioAudio)
