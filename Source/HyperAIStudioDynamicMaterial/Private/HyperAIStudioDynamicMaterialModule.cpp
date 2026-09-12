// Games by Hyper 2026.

#include "HyperAIStudioDynamicMaterialToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDynamicMaterialModule, Log, All);

class FHyperAIStudioDynamicMaterialModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// LoadingPhase=None: core explicitly loads this source candidate only after exact generated
		// cohort admission and the optional DynamicMaterial plugin/module prerequisites agree.
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioDynamicMaterialToolset::StaticClass(),
			FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDynamicMaterialModule, Error,
				TEXT("DynamicMaterial capability publication failed closed: %s"), *Error);
			return;
		}
		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioDynamicMaterialRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration) { Registration->Shutdown(); Registration.Reset(); }
		if (bPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioDynamicMaterialToolset::StaticClass(),
				FHyperAIStudioDynamicMaterialContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioDynamicMaterialRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioDynamicMaterialModule, HyperAIStudioDynamicMaterial)
