// Games by Hyper 2026.

#include "HyperAIStudioAnimBlueprintToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimBlueprintModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool Animation Blueprint graph source cohort. */
class FHyperAIStudioAnimBlueprintModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioAnimBlueprintToolset::StaticClass(),
			FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAnimBlueprintModule, Error, TEXT("Animation Blueprint optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioAnimBlueprintRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioAnimBlueprintModule, Error,
					TEXT("Animation Blueprint module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioAnimBlueprintToolset::StaticClass(),
				FHyperAIStudioAnimBlueprintContracts::GetQualifiedToolsetName());
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
	TUniquePtr<FHyperAIStudioAnimBlueprintRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAnimBlueprintModule, HyperAIStudioAnimBlueprint)
