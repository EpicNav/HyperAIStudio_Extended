// Games by Hyper 2026.

#include "HyperAIStudioPropertyAnimationToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPropertyAnimationModule, Log, All);

class FHyperAIStudioPropertyAnimationModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioPropertyAnimationToolset::StaticClass(),
			FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPropertyAnimationModule, Error,
				TEXT("Property Animation optional publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioPropertyAnimationRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioPropertyAnimationModule, Error,
					TEXT("Property Animation shutdown retained exact runtime ownership."));
				return;
			}
			Registration.Reset();
		}
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioPropertyAnimationToolset::StaticClass(),
				FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioPropertyAnimationRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioPropertyAnimationModule, HyperAIStudioPropertyAnimation)
