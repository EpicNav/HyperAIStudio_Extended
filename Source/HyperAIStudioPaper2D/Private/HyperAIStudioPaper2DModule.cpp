// Games by Hyper 2026.

#include "HyperAIStudioPaper2DToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPaper2DModule, Log, All);

/** Explicit LoadingPhase=None owner for the atomic Paper2D source cohort. */
class FHyperAIStudioPaper2DModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioPaper2DToolset::StaticClass(),
			FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPaper2DModule, Error,
				TEXT("Paper2D optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioPaper2DRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioPaper2DModule, Error,
					TEXT("Paper2D shutdown retained live ownership after a fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioPaper2DToolset::StaticClass(),
				FHyperAIStudioPaper2DContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioPaper2DRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioPaper2DModule, HyperAIStudioPaper2D)
