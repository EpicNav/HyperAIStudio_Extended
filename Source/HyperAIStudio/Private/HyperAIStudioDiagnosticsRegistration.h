// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioViewportCaptureToolset.h"

/** Exact tool-name/class binding used only after generated-catalog admission resolves true. */
struct FHyperAIStudioDiagnosticsToolBinding
{
	FString ToolName;
	FString QualifiedToolset;
	UClass* ToolsetClass = nullptr;
};

/** Owns four independent diagnostics source-candidate registrations and their bounded services. */
class FHyperAIStudioDiagnosticsRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

	/** Trusted C++/UI seam; not reflected and therefore never callable through MCP. */
	bool IssueViewportCaptureGrant(
		FHyperAIStudioDiagnosticsViewportCaptureRequest& InOutRequest,
		FTimespan Lifetime,
		FHyperAIStudioViewportCaptureGrant& OutGrant,
		FString& OutErrorCode,
		FString& OutError) const;

	static const TArray<FHyperAIStudioDiagnosticsToolBinding>& GetBindings();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(const FString& ToolName, bool bAllowPendingForTests);

private:
	void RegisterAfterEngineInit();
	void RefreshMcpToolsIfSafe() const;

	FDelegateHandle PostEngineInitHandle;
	TArray<UClass*> OwnedToolsets;
	bool bStarted = false;
	bool bAuditStoreStarted = false;
	bool bLogBufferStarted = false;
	TSharedPtr<FHyperAIStudioViewportCaptureGrantIssuer, ESPMode::ThreadSafe> ViewportCaptureGrantIssuer;
};
