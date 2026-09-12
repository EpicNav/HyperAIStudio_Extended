// Games by Hyper 2026.

#include "HyperAIStudioAutomationToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAutomationModule, Log, All);

class FHyperAIStudioAutomationModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioAutomationToolset::StaticClass(),
			FHyperAIStudioAutomationContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAutomationModule, Error,
				TEXT("Automation optional publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioAutomationRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioAutomationModule, Error,
					TEXT("Automation module shutdown retained live ownership."));
				return;
			}
			Registration.Reset();
		}
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioAutomationToolset::StaticClass(),
				FHyperAIStudioAutomationContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioAutomationRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAutomationModule, HyperAIStudioAutomation)
