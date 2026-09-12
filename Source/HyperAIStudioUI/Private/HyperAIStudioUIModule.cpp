// Games by Hyper 2026.

#include "HyperAIStudioUIToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioUIModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool UI source cohort. */
class FHyperAIStudioUIModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioUIToolset::StaticClass(),
			FHyperAIStudioUIContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioUIModule, Error,
				TEXT("UI optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioUIRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioUIModule, Error,
					TEXT("UI module shutdown retained live ownership after fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioUIToolset::StaticClass(),
				FHyperAIStudioUIContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioUIRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioUIModule, HyperAIStudioUI)
