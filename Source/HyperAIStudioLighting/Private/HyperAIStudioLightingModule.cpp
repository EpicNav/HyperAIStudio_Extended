// Games by Hyper 2026.

#include "HyperAIStudioLightingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLightingModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool lighting look-dev source cohort. */
class FHyperAIStudioLightingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioLightingToolset::StaticClass(),
			FHyperAIStudioLightingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLightingModule, Error, TEXT("Lighting optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioLightingRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioLightingModule, Error,
					TEXT("Lighting module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioLightingToolset::StaticClass(),
				FHyperAIStudioLightingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// Registered adapter vtables may not outlive this DLL.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioLightingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioLightingModule, HyperAIStudioLighting)
