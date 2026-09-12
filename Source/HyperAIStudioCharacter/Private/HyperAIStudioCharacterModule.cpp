// Games by Hyper 2026.

#include "HyperAIStudioCharacterToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioCharacterModule, Log, All);

/** Explicit owner for one LoadingPhase=None atomic Character cohort. */
class FHyperAIStudioCharacterModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioCharacterToolset::StaticClass(),
			FHyperAIStudioCharacterContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioCharacterModule, Error,
				TEXT("Character optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioCharacterRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioCharacterModule, Error,
					TEXT("Character module shutdown retained live ownership after fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioCharacterToolset::StaticClass(),
				FHyperAIStudioCharacterContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioCharacterRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioCharacterModule, HyperAIStudioCharacter)
