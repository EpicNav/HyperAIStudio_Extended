// Games by Hyper 2026.

#include "HyperAIStudioPhysicsToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPhysicsModule, Log, All);

/** LoadingPhase=None owner for one SourceCandidate, atomic three-tool Physics cohort. */
class FHyperAIStudioPhysicsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioPhysicsToolset::StaticClass(),
			FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPhysicsModule, Error,
				TEXT("Physics optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioPhysicsRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioPhysicsModule, Error,
					TEXT("Physics shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioPhysicsToolset::StaticClass(),
				FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioPhysicsRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioPhysicsModule, HyperAIStudioPhysics)
