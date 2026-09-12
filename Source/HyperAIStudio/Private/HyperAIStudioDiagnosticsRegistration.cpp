// Games by Hyper 2026.

#include "HyperAIStudioDiagnosticsRegistration.h"

#include "HyperAIStudioAuditToolset.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDiagnoseToolset.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLogTailToolset.h"
#include "HyperAIStudioViewportCaptureToolset.h"
#include "IModelContextProtocolModule.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDiagnosticsRegistration, Log, All);

namespace HyperAIStudio::DiagnosticsRegistration::Private
{
	const FHyperAIStudioCapabilityToolDefinition* FindUniqueCatalogTool(const FString& ToolName)
	{
		const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
		const FHyperAIStudioCapabilityToolDefinition* Result = nullptr;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name != ToolName)
			{
				continue;
			}
			if (Result)
			{
				return nullptr;
			}
			Result = &Tool;
		}
		return Result;
	}

	const FHyperAIStudioDiagnosticsToolBinding* FindBinding(const FString& ToolName)
	{
		return FHyperAIStudioDiagnosticsRegistration::GetBindings().FindByPredicate(
			[&ToolName](const FHyperAIStudioDiagnosticsToolBinding& Binding)
			{
				return Binding.ToolName == ToolName;
			});
	}
}

bool FHyperAIStudioDiagnosticsRegistration::IssueViewportCaptureGrant(
	FHyperAIStudioDiagnosticsViewportCaptureRequest& InOutRequest,
	const FTimespan Lifetime,
	FHyperAIStudioViewportCaptureGrant& OutGrant,
	FString& OutErrorCode,
	FString& OutError) const
{
	OutGrant = FHyperAIStudioViewportCaptureGrant();
	OutErrorCode.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutErrorCode = TEXT("authorization_issuer_wrong_thread");
		OutError = TEXT("Viewport-capture grants may only be issued by the trusted game-thread UI/server lifecycle.");
		return false;
	}
	if (!bStarted || !ViewportCaptureGrantIssuer.IsValid())
	{
		OutErrorCode = TEXT("authorization_issuer_unavailable");
		OutError = TEXT("The diagnostics lifecycle does not own an active viewport-capture grant issuer.");
		return false;
	}
	if (!InOutRequest.AuthorizationToken.IsEmpty())
	{
		OutErrorCode = TEXT("client_authorization_token_rejected");
		OutError = TEXT("The trusted issuer only accepts requests without a caller-provided authorization token.");
		return false;
	}

	FHyperAIStudioDiagnosticsViewportCaptureRequest ValidationRequest = InOutRequest;
	ValidationRequest.AuthorizationToken = TEXT("server-owned-validation-placeholder");
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest Normalized;
	if (!FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
		ValidationRequest, Normalized, OutErrorCode, OutError))
	{
		return false;
	}

	TSharedPtr<FHyperAIStudioViewportCaptureGrantIssuer, ESPMode::ThreadSafe> Issuer =
		ViewportCaptureGrantIssuer;
	if (!Issuer->IssueForRequest(
		Normalized, Lifetime, OutGrant, OutErrorCode, OutError))
	{
		return false;
	}
	if (OutGrant.Token.IsEmpty()
		|| OutGrant.OperationId != Normalized.OperationId
		|| OutGrant.CanonicalProjectId.IsEmpty()
		|| OutGrant.EffectHash.IsEmpty())
	{
		OutGrant = FHyperAIStudioViewportCaptureGrant();
		OutErrorCode = TEXT("authorization_grant_binding_mismatch");
		OutError = TEXT("The lifecycle-owned issuer returned an incomplete or mismatched grant.");
		return false;
	}
	InOutRequest.AuthorizationToken = OutGrant.Token;
	return true;
}

const TArray<FHyperAIStudioDiagnosticsToolBinding>& FHyperAIStudioDiagnosticsRegistration::GetBindings()
{
	static const TArray<FHyperAIStudioDiagnosticsToolBinding> Bindings = {
		{
			TEXT("hyper_diagnose"),
			TEXT("HyperAIStudio.HyperAIStudioDiagnoseToolset"),
			UHyperAIStudioDiagnoseToolset::StaticClass()
		},
		{
			TEXT("hyper_run_audit"),
			TEXT("HyperAIStudio.HyperAIStudioAuditToolset"),
			UHyperAIStudioAuditToolset::StaticClass()
		},
		{
			TEXT("hyper_log_tail"),
			TEXT("HyperAIStudio.HyperAIStudioLogTailToolset"),
			UHyperAIStudioLogTailToolset::StaticClass()
		},
		{
			TEXT("hyper_viewport_capture"),
			TEXT("HyperAIStudio.HyperAIStudioViewportCaptureToolset"),
			UHyperAIStudioViewportCaptureToolset::StaticClass()
		}
	};
	return Bindings;
}

bool FHyperAIStudioDiagnosticsRegistration::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioDiagnosticsRegistration::IsRegistrationAllowed(
	const FString& ToolName,
	const bool bAllowPendingForTests)
{
	using namespace HyperAIStudio::DiagnosticsRegistration::Private;
	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		return false;
	}
	const FHyperAIStudioDiagnosticsToolBinding* Binding = FindBinding(ToolName);
	const FHyperAIStudioCapabilityToolDefinition* Tool = FindUniqueCatalogTool(ToolName);
	if (!Binding || !Binding->ToolsetClass || !Tool || Tool->PackId != TEXT("diagnostics")
		|| Binding->QualifiedToolset != TEXT("HyperAIStudio.") + Binding->ToolsetClass->GetName())
	{
		return false;
	}
	if (Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted)
	{
		return true;
	}
	return Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate
		&& bAllowPendingForTests;
}

void FHyperAIStudioDiagnosticsRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioDiagnosticsRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioDiagnosticsRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	// Close and wipe authorization before unregistering any callable surface. A concurrent holder of
	// the old shared gate will still observe bAccepting=false and cannot consume a stale grant.
	FHyperAIStudioViewportCaptureAuthorization::ResetGate();
	if (ViewportCaptureGrantIssuer.IsValid())
	{
		ViewportCaptureGrantIssuer->Shutdown();
		ViewportCaptureGrantIssuer.Reset();
	}
	bool bRegistryChanged = false;
	if (IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		for (UClass* ToolsetClass : OwnedToolsets)
		{
			const FHyperAIStudioDiagnosticsToolBinding* Binding = GetBindings().FindByPredicate(
				[ToolsetClass](const FHyperAIStudioDiagnosticsToolBinding& Candidate)
				{
					return Candidate.ToolsetClass == ToolsetClass;
				});
			if (ToolsetClass && Binding)
			{
				FString Error;
				if (FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
					ToolsetClass,
					Binding->QualifiedToolset,
					Error))
				{
					bRegistryChanged = true;
				}
				else
				{
					UE_LOG(LogHyperAIStudioDiagnosticsRegistration, Warning,
						TEXT("Could not unregister owned diagnostics toolset %s: %s"),
						*Binding->QualifiedToolset,
						*Error);
				}
			}
		}
	}
	OwnedToolsets.Reset();
	if (bLogBufferStarted)
	{
		FHyperAIStudioDiagnosticsLogBuffer::Get().Shutdown();
		bLogBufferStarted = false;
	}
	if (bAuditStoreStarted)
	{
		FHyperAIStudioDiagnosticsAuditStore::Get().Shutdown();
		bAuditStoreStarted = false;
	}
	if (bRegistryChanged && !IsEngineExitRequested())
	{
		RefreshMcpToolsIfSafe();
	}
	bStarted = false;
}

bool FHyperAIStudioDiagnosticsRegistration::IsRegistered() const
{
	if (!UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return false;
	}
	const bool bAllowPending = IsPendingTestRegistrationEnabled();
	bool bAnyRequired = false;
	for (const FHyperAIStudioDiagnosticsToolBinding& Binding : GetBindings())
	{
		const bool bRequired = IsRegistrationAllowed(Binding.ToolName, bAllowPending);
		bAnyRequired |= bRequired;
		if (bRequired && (!Binding.ToolsetClass
			|| !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				Binding.ToolsetClass,
				Binding.QualifiedToolset)))
		{
			return false;
		}
	}
	return bAnyRequired;
}

void FHyperAIStudioDiagnosticsRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	const bool bAllowPending = IsPendingTestRegistrationEnabled();
	const bool bAuditAllowed = IsRegistrationAllowed(TEXT("hyper_run_audit"), bAllowPending);
	const bool bLogAllowed = IsRegistrationAllowed(TEXT("hyper_log_tail"), bAllowPending);
	const bool bViewportCaptureAllowed =
		IsRegistrationAllowed(TEXT("hyper_viewport_capture"), bAllowPending);
	if (bAuditAllowed && !bAuditStoreStarted)
	{
		FHyperAIStudioDiagnosticsAuditStore::Get().Startup();
		bAuditStoreStarted = true;
	}
	if (bLogAllowed && !bLogBufferStarted)
	{
		FHyperAIStudioDiagnosticsLogBuffer::Get().Startup();
		bLogBufferStarted = true;
	}
	if (bViewportCaptureAllowed && !ViewportCaptureGrantIssuer.IsValid())
	{
		ViewportCaptureGrantIssuer = MakeShared<
			FHyperAIStudioViewportCaptureGrantIssuer, ESPMode::ThreadSafe>();
		FHyperAIStudioViewportCaptureAuthorization::SetGate(ViewportCaptureGrantIssuer);
	}

	bool bRegistryChanged = false;
	for (const FHyperAIStudioDiagnosticsToolBinding& Binding : GetBindings())
	{
		if (!Binding.ToolsetClass
			|| !IsRegistrationAllowed(Binding.ToolName, bAllowPending)
			|| FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
				Binding.ToolsetClass,
				Binding.QualifiedToolset))
		{
			continue;
		}
		FString Error;
		if (FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			Binding.ToolsetClass,
			Binding.QualifiedToolset,
			Error))
		{
			OwnedToolsets.Add(Binding.ToolsetClass);
			bRegistryChanged = true;
		}
		else
		{
			UE_LOG(
				LogHyperAIStudioDiagnosticsRegistration,
				Error,
				TEXT("ToolsetRegistry rejected diagnostics cohort %s for %s: %s"),
				*Binding.QualifiedToolset,
				*Binding.ToolName,
				*Error);
		}
	}
	if (bRegistryChanged)
	{
		RefreshMcpToolsIfSafe();
	}
}

void FHyperAIStudioDiagnosticsRegistration::RefreshMcpToolsIfSafe() const
{
	if (!IsInGameThread() || IsEngineExitRequested())
	{
		return;
	}
	if (IModelContextProtocolModule* ModelContextProtocol =
		FModuleManager::GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol")))
	{
		ModelContextProtocol->RefreshTools();
	}
}
