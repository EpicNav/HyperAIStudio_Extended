// Games by Hyper 2026.

#include "HyperAIStudioMaterialsToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMaterialsModule, Log, All);

class FHyperAIStudioMaterialsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// This source candidate is descriptor-owned with LoadingPhase=None. Core may explicitly load it
		// only after exact generated cohort admission and the MaterialEditor prerequisite agree.
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioMaterialsToolset::StaticClass(),
			FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMaterialsModule, Error,
				TEXT("Materials capability publication failed closed: %s"), *Error);
			return;
		}

		bPublished = true;
		Registration = MakeUnique<FHyperAIStudioMaterialsRegistration>();
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
				UHyperAIStudioMaterialsToolset::StaticClass(),
				FHyperAIStudioMaterialsContracts::GetQualifiedToolsetName());
			bPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioMaterialsRegistration> Registration;
	bool bPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioMaterialsModule, HyperAIStudioMaterials)
