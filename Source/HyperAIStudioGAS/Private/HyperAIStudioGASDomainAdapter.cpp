// Games by Hyper 2026.

#include "HyperAIStudioGASToolset.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioGASDomainAdapter, Log, All);

FHyperAIStudioGASDomainAdapter::FHyperAIStudioGASDomainAdapter()
{
	Descriptor.PackId = FHyperAIStudioGASContracts::PackId;
	Descriptor.AdapterId = TEXT("hyperai.gas.ue58.editor");
	Descriptor.SemanticVersion = TEXT("1.0.0");
	Descriptor.AdapterVersion = 1;
	Descriptor.Variants.Add({
		TEXT("hyper_gas_apply_plan"),
		FHyperAIStudioGASContracts::MutationVariantId,
		FHyperAIStudioGASContracts::PayloadTypeId,
		FHyperAIStudioGASContracts::PayloadSchemaFingerprint,
		FHyperAIStudioGASContracts::ResultTypeId,
		FHyperAIStudioGASContracts::ResultSchemaFingerprint,
		EHyperAIStudioDomainSafety::Edit});
	Descriptor.ContractFingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Descriptor);
	Descriptor.AdapterFingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Descriptor);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioGASDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioGASDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	(void)Context;
	(void)Payload;
	// The available UE 5.8 editor path performs non-preemptible Blueprint compilation and
	// SavePackages may FullyLoad/source-control/write synchronously. The module therefore ships
	// no mutation helper at all: even accidental adapter registration remains zero-effect.
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	Result.StatusCode = FHyperAIStudioGASContracts::ExecutionBlocker;
	Result.Diagnostic = TEXT(
		"GAS execution requires a bounded non-loading compile/save backend and exact runtime-side-effect CAS.");
	return Result;
}

void FHyperAIStudioGASRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioGASRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioGASRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioGASToolset::StaticClass(),
			FHyperAIStudioGASContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioGASDomainAdapter, Warning,
				TEXT("Could not unregister the owned GAS toolset: %s"), *Error);
		}
	}
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioGASRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioGASContracts::IsRegistrationAllowed(
			FHyperAIStudioGASContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioGASToolset::StaticClass(),
			FHyperAIStudioGASContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioGASRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioGASContracts::IsRegistrationAllowed(
			FHyperAIStudioGASContracts::IsPendingTestRegistrationEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioGASToolset::StaticClass(),
		FHyperAIStudioGASContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioGASToolset::StaticClass(),
			FHyperAIStudioGASContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioGASDomainAdapter, Error,
				TEXT("ToolsetRegistry rejected the owned GAS cohort: %s"), *Error);
		}
	}
}
