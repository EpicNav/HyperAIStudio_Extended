// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

class UNiagaraSystem;

/**
 * Niagara assets beside the System: effect types (scalability budgets), data channels, and simulation caches for
 * golden-output regression checks. FNiagaraSimCacheCompare is experimental in UE 5.8 and data channels expose no
 * setters, so both stay inside this file. Everything runs on the game thread.
 *
 * Module and function script authoring is absent on purpose: UE 5.8 exports no way to build a script graph.
 */
namespace HyperAIStudio::Niagara::AssetsGate
{
	/** set_effect_type, create_effect_type, set_effect_type_setting, create_data_channel, bake_sim_cache. */
	bool IsAssetOp(const FString& Kind);

	/**
	 * Checks an asset op. PlannedAssets are asset paths created by earlier ops in the same plan; SystemPath is the
	 * plan's System, which a baked capture must come from.
	 */
	bool ValidateAssetOp(const FHyperAINiagaraEditOp& Op, const FString& SystemPath, const TSet<FString>& PlannedAssets,
		FString& OutStatus, FString& OutDiagnostic);

	/** Applies one asset op inside the caller's transaction. */
	bool ApplyAssetOp(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError);

	/** Assets a plan creates or edits besides its System, in op order; they are saved and verified with it. */
	TArray<FString> SideAssetPaths(const TArray<FHyperAINiagaraEditOp>& Ops);

	/** Fills Record for an effect type, data channel or sim cache; false for anything else. */
	bool DescribeAsset(const UObject& Asset, FHyperAINiagaraAssetRecord& OutRecord);

	/** Effect type assigned to a System, or empty. */
	FString GetEffectTypePath(const UNiagaraSystem& System);

	/**
	 * validate policy sim_cache_capture. Without a capture id it starts capturing the System frame by frame in a
	 * preview scene and returns the id; with one it reports progress, and once complete compares against
	 * GoldenSimCachePath when given.
	 */
	FHyperAINiagaraValidateReport RunCapturePolicy(const FHyperAINiagaraValidateRequest& Request);

	/** Content key of the System a capture came from, for the bake check; empty for an unknown capture. */
	FString GetCaptureSystemKey(const FString& CaptureId);
}
