// Games by Hyper 2026.

#include "HyperAIStudioLevelDesignToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLevelDesignModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool level design source cohort. */
class FHyperAIStudioLevelDesignModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioLevelDesignToolset::StaticClass(),
			FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLevelDesignModule, Error, TEXT("Level design optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioLevelDesignRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioLevelDesignModule, Error,
					TEXT("Level design module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioLevelDesignToolset::StaticClass(),
				FHyperAIStudioLevelDesignContracts::GetQualifiedToolsetName());
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
	TUniquePtr<FHyperAIStudioLevelDesignRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioLevelDesignModule, HyperAIStudioLevelDesign)
