// Games by Hyper 2026.

#include "HyperAIStudioNiagaraToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNiagaraModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool Niagara source cohort. */
class FHyperAIStudioNiagaraModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioNiagaraToolset::StaticClass(),
			FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioNiagaraModule, Error,
				TEXT("Niagara optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioNiagaraRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioNiagaraModule, Error,
					TEXT("Niagara module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioNiagaraToolset::StaticClass(),
				FHyperAIStudioNiagaraContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// Registered adapter vtables and an eventual pinned async session may not outlive this DLL.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioNiagaraRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioNiagaraModule, HyperAIStudioNiagara)
