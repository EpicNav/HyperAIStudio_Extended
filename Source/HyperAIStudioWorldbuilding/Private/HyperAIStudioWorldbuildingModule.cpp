// Games by Hyper 2026.

#include "HyperAIStudioWorldbuildingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioWorldbuildingModule, Log, All);

/** Explicit owner for the descriptor's LoadingPhase=None six-tool atomic cohort. */
class FHyperAIStudioWorldbuildingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioWorldbuildingToolset::StaticClass(),
			FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioWorldbuildingModule, Error,
				TEXT("Worldbuilding publication failed closed: %s"), *Error);
			return;
		}
		bWorldPublished = true;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioNavigationToolset::StaticClass(),
			FHyperAIStudioNavigationContracts::GetQualifiedToolsetName(), Error))
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioWorldbuildingToolset::StaticClass(),
				FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName());
			bWorldPublished = false;
			UE_LOG(LogHyperAIStudioWorldbuildingModule, Error,
				TEXT("Navigation publication failed; six-tool cohort rolled back: %s"), *Error);
			return;
		}
		bNavigationPublished = true;
		Registration = MakeUnique<FHyperAIStudioWorldbuildingRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			Registration.Reset();
		}
		if (bNavigationPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioNavigationToolset::StaticClass(),
				FHyperAIStudioNavigationContracts::GetQualifiedToolsetName());
			bNavigationPublished = false;
		}
		if (bWorldPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioWorldbuildingToolset::StaticClass(),
				FHyperAIStudioWorldbuildingContracts::GetQualifiedToolsetName());
			bWorldPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// The central owner retains exact handlers across later Epic filter changes.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioWorldbuildingRegistration> Registration;
	bool bWorldPublished = false;
	bool bNavigationPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioWorldbuildingModule, HyperAIStudioWorldbuilding)
