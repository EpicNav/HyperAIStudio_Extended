// Games by Hyper 2026.

#include "HyperAIStudioInterchangeToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioInterchangeModule, Log, All);

/** LoadingPhase=None owner for one SourceCandidate, atomic three-tool Interchange cohort. */
class FHyperAIStudioInterchangeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioInterchangeToolset::StaticClass(),
			FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioInterchangeModule, Error,
				TEXT("Interchange optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioInterchangeRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioInterchangeModule, Error,
					TEXT("Interchange shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioInterchangeToolset::StaticClass(),
				FHyperAIStudioInterchangeContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioInterchangeRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioInterchangeModule, HyperAIStudioInterchange)
