// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class UClass;

/** Runtime-only binding for a generated HyperAI capability contract. */
struct HYPERAISTUDIO_API FHyperAIStudioRuntimeToolBinding
{
	FString ToolName;
	FString QualifiedToolset;
	FString Description;
	bool bImplementationLoaded = false;
	bool bToolsetRegistered = false;
	/** Exact full tool exists, its toolset is enabled, and Epic's per-tool filters allow it. */
	bool bToolEnabled = false;
};

/**
 * Central, bounded view of loaded HyperAI ToolsetRegistry implementations.
 *
 * This index never decides scope or admission. Every published reflected tool
 * must first match the immutable generated capability catalog; the catalog
 * remains the sole authority for Planned/SourceCandidate/Admitted state.
 */
class HYPERAISTUDIO_API FHyperAIStudioCapabilityRuntimeIndex final
{
public:
	static constexpr int32 MaxRuntimeToolBindings = 109;
	static constexpr int32 MaxDiagnostics = 32;

	/**
	 * Registers exactly one HyperAI-owned toolset on the initialized game thread.
	 * Success is proven by Epic's synchronous registry-change notification, not by
	 * filtered discovery helpers which hide disabled toolsets. UE 5.8 cannot return
	 * a newly created handler publicly, so new ownership fails closed when registry
	 * filters already exist; filters added after handler capture remain supported.
	 */
	static bool RegisterOwnedToolsetClass(
		UClass* ToolsetClass,
		const FString& QualifiedToolset,
		FString& OutError);

	/**
	 * Unregisters exactly a registration created by RegisterOwnedToolsetClass.
	 * The exact visible handler captured by every successful registration is retained so later Epic filters cannot
	 * make owned shutdown depend on filtered Find/IsRegistered helpers.
	 */
	static bool UnregisterOwned(
		UClass* ToolsetClass,
		const FString& QualifiedToolset,
		FString& OutError);

	/** Returns central owner truth; callable/filter state is intentionally separate. */
	static bool IsOwned(
		UClass* ToolsetClass,
		const FString& QualifiedToolset);

	/** Publishes one optional-module toolset without creating a core dependency on that module. */
	static bool PublishOptionalToolset(
		UClass* ToolsetClass,
		const FString& QualifiedToolset,
		FString& OutError);

	/** Withdraws exactly one optional-module publication during module shutdown. */
	static void WithdrawOptionalToolset(
		UClass* ToolsetClass,
		const FString& QualifiedToolset);

	/**
	 * Returns core implementations plus currently loaded optional publications.
	 * Registration state is sampled from Epic ToolsetRegistry on the game thread.
	 */
	static bool BuildSnapshot(
		TArray<FHyperAIStudioRuntimeToolBinding>& OutBindings,
		TArray<FString>& OutDiagnostics);
};
