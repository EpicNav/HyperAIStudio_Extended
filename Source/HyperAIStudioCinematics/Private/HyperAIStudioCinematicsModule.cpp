// Games by Hyper 2026.

#include "HyperAIStudioCinematicsToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioCinematicsModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool cinematics source cohort. */
class FHyperAIStudioCinematicsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioCinematicsToolset::StaticClass(),
			FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioCinematicsModule, Error,
				TEXT("Cinematics optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioCinematicsRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioCinematicsModule, Error,
					TEXT("Cinematics module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioCinematicsToolset::StaticClass(),
				FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioCinematicsRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioCinematicsModule, HyperAIStudioCinematics)
