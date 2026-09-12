// Games by Hyper 2026.

#include "HyperAIStudioGameplayAIToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGameplayAIModule, Log, All);

/**
 * Explicit-load owner for the descriptor's LoadingPhase=None Gameplay AI base adapter.
 * Optional StateTree/EQS/SmartObject/Behavior variants remain gated future facade modules.
 */
class FHyperAIStudioGameplayAIModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString PublicationError;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioGameplayAIToolset::StaticClass(),
			FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName(),
			PublicationError))
		{
			UE_LOG(LogHyperAIStudioGameplayAIModule, Error,
				TEXT("Gameplay AI runtime capability publication failed closed: %s"),
				*PublicationError);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioGameplayAIRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioGameplayAIToolset::StaticClass(),
				FHyperAIStudioGameplayAIContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override
	{
		return false;
	}

	virtual bool SupportsAutomaticShutdown() override
	{
		// Epic's UE 5.8 filtered registry cannot reliably unregister a fully hidden toolset.
		// Keep this explicitly loaded optional DLL resident until process teardown.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioGameplayAIRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioGameplayAIModule, HyperAIStudioGameplayAI)
