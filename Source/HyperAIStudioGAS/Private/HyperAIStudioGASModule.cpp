// Games by Hyper 2026.

#include "HyperAIStudioGASToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGASModule, Log, All);

class FHyperAIStudioGASModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// LoadingPhase=None is required: core may explicitly load this module only after the optional
		// GameplayAbilities prerequisite and exact generated source-candidate/admission cohort agree.
		FString PublicationError;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioGASToolset::StaticClass(),
			FHyperAIStudioGASContracts::GetQualifiedToolsetName(),
			PublicationError))
		{
			UE_LOG(LogHyperAIStudioGASModule, Error,
				TEXT("GAS runtime capability publication failed closed: %s"),
				*PublicationError);
			return;
		}

		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioGASRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioGASToolset::StaticClass(),
				FHyperAIStudioGASContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override
	{
		return false;
	}

	virtual bool SupportsAutomaticShutdown() override
	{
		// UE 5.8 hides a fully filtered toolset from its public unregister lookup.
		// Keep the optional GAS DLL resident until process teardown.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioGASRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioGASModule, HyperAIStudioGAS)
