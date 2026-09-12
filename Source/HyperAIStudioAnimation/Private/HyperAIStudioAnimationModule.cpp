// Games by Hyper 2026.

#include "HyperAIStudioAnimationToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimationModule, Log, All);

/** Explicit-load owner for the source-candidate Animation/Rigging cohort. */
class FHyperAIStudioAnimationModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString PublicationError;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioAnimationToolset::StaticClass(),
			FHyperAIStudioAnimationContracts::GetQualifiedToolsetName(),
			PublicationError))
		{
			UE_LOG(LogHyperAIStudioAnimationModule, Error,
				TEXT("Animation runtime capability publication failed closed: %s"),
				*PublicationError);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioAnimationRegistration>();
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
				UHyperAIStudioAnimationToolset::StaticClass(),
				FHyperAIStudioAnimationContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// Keep the explicitly loaded optional DLL resident; staged payloads and registry
		// ownership must never outlive their implementation code during editor teardown.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioAnimationRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAnimationModule, HyperAIStudioAnimation)
