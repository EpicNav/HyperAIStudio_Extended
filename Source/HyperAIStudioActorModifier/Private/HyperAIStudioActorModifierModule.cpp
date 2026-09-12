// Games by Hyper 2026.

#include "HyperAIStudioActorModifierToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioActorModifierModule, Log, All);

class FHyperAIStudioActorModifierModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioActorModifierToolset::StaticClass(),
			FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioActorModifierModule, Error,
				TEXT("Actor Modifier optional publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioActorModifierRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioActorModifierModule, Error,
					TEXT("Actor Modifier shutdown retained exact runtime ownership."));
				return;
			}
			Registration.Reset();
		}
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioActorModifierToolset::StaticClass(),
				FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioActorModifierRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioActorModifierModule, HyperAIStudioActorModifier)
