// Games by Hyper 2026.

#include "HyperAIStudioLiveProductionToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLiveProductionModule, Log, All);

/** LoadingPhase=None owner for one atomic three-tool live-production cohort. */
class FHyperAIStudioLiveProductionModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioLiveProductionToolset::StaticClass(),
			FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLiveProductionModule, Error,
				TEXT("Live-production optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioLiveProductionRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioLiveProductionModule, Error,
					TEXT("Live-production shutdown retained exact ToolsetRegistry ownership."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioLiveProductionToolset::StaticClass(),
				FHyperAIStudioLiveProductionContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioLiveProductionRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioLiveProductionModule, HyperAIStudioLiveProduction)
