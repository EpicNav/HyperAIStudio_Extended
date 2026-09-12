// Games by Hyper 2026.

#include "HyperAIStudioEnhancedInputToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioEnhancedInputModule, Log, All);

class FHyperAIStudioEnhancedInputModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// LoadingPhase=None prevents implicit startup. Once an admitted capability loader explicitly
		// selects this module, the generated cohort policy below remains the sole registration gate:
		// Admitted works in production; SourceCandidate additionally requires explicit dev evidence.
		FString PublicationError;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioEnhancedInputToolset::StaticClass(),
			FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName(),
			PublicationError))
		{
			UE_LOG(LogHyperAIStudioEnhancedInputModule, Error,
				TEXT("Enhanced Input runtime capability publication failed closed: %s"),
				*PublicationError);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioEnhancedInputRegistration>();
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
				UHyperAIStudioEnhancedInputToolset::StaticClass(),
				FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override
	{
		return false;
	}

	virtual bool SupportsAutomaticShutdown() override
	{
		// A fully filtered UE 5.8 toolset is hidden from Epic's public unregister lookup.
		// Retain this optional module until process teardown to avoid a stale class/vtable.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioEnhancedInputRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioEnhancedInputModule, HyperAIStudioEnhancedInput)
