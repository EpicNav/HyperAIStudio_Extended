// Games by Hyper 2026.

#include "HyperAIStudioNetworkingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNetworkingModule, Log, All);

/** Explicit-load owner for one atomic Networking/Game Framework source cohort. */
class FHyperAIStudioNetworkingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString PublicationError;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioNetworkingToolset::StaticClass(),
			FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName(),
			PublicationError))
		{
			UE_LOG(LogHyperAIStudioNetworkingModule, Error,
				TEXT("Networking/Game Framework runtime publication failed closed: %s"),
				*PublicationError);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioNetworkingRegistration>();
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
				UHyperAIStudioNetworkingToolset::StaticClass(),
				FHyperAIStudioNetworkingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// Registry ownership and detached staged DTOs may not outlive this implementation DLL.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioNetworkingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioNetworkingModule, HyperAIStudioNetworking)
