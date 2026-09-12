// Games by Hyper 2026.

#include "HyperAIStudioGameplaySystemsToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGameplaySystemsModule, Log, All);

/** LoadingPhase=None owner for one atomic, three-tool Gameplay Systems cohort. */
class FHyperAIStudioGameplaySystemsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioGameplaySystemsToolset::StaticClass(),
			FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioGameplaySystemsModule, Error,
				TEXT("Gameplay Systems optional publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioGameplaySystemsRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioGameplaySystemsModule, Error,
					TEXT("Gameplay Systems shutdown retained live ownership after fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioGameplaySystemsToolset::StaticClass(),
				FHyperAIStudioGameplaySystemsContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioGameplaySystemsRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioGameplaySystemsModule, HyperAIStudioGameplaySystems)
