// Games by Hyper 2026.

#include "HyperAIStudioGeometryToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGeometryModule, Log, All);

/** LoadingPhase=None owner for one SourceCandidate, atomic three-tool Geometry cohort. */
class FHyperAIStudioGeometryModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioGeometryToolset::StaticClass(),
			FHyperAIStudioGeometryContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioGeometryModule, Error,
				TEXT("Geometry optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioGeometryRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioGeometryModule, Error,
					TEXT("Geometry shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioGeometryToolset::StaticClass(),
				FHyperAIStudioGeometryContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override { return false; }

private:
	TUniquePtr<FHyperAIStudioGeometryRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioGeometryModule, HyperAIStudioGeometry)
