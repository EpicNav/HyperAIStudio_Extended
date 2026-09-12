// Games by Hyper 2026.

#include "HyperAIStudioTextureGraphToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioTextureGraphModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool Texture Graph source cohort. */
class FHyperAIStudioTextureGraphModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioTextureGraphToolset::StaticClass(),
			FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioTextureGraphModule, Error,
				TEXT("Texture Graph optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioTextureGraphRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioTextureGraphModule, Error,
					TEXT("Texture Graph module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioTextureGraphToolset::StaticClass(),
				FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName());
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
	TUniquePtr<FHyperAIStudioTextureGraphRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioTextureGraphModule, HyperAIStudioTextureGraph)
