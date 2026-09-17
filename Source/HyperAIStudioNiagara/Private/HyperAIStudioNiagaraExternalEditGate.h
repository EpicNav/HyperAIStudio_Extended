// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

class UNiagaraSystem;

/**
 * The only door to UE 5.8's UNiagaraExternalEditUtilities, which Epic marks experimental and subject to change.
 * Nothing here exposes an FNiagaraExt_* type, so when that API moves the fix stays inside the gate's .cpp.
 * Every function must run on the game thread and builds its own edit context for the duration of the call.
 */
namespace HyperAIStudio::Niagara::ExternalEditGate
{
	static constexpr int32 MaxTopologyModules = 512;
	static constexpr int32 MaxInputNamesPerModule = 64;
	static constexpr int32 MaxInputNameDepth = 8;

	struct FDiagnostics
	{
		bool bCompileStateKnown = false;
		bool bCompiling = false;
		bool bCompileHasErrors = false;
		bool bCompileHasWarnings = false;
		/** Undismissed stack errors only; the edit baseline compares this. */
		int32 StackErrorCount = 0;
		/** Stack issues plus compile events, counted before the Issues list is truncated. */
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		int32 InfoCount = 0;
		/** Undismissed stack issues, then compile events at warning or error severity. */
		TArray<FHyperAINiagaraIssue> Issues;
		bool bTruncated = false;
	};

	bool ReadTopology(
		UNiagaraSystem& System,
		TArray<FHyperAINiagaraEmitterTopology>& OutEmitters,
		bool& bOutTruncated,
		FString& OutError);

	bool ReadDiagnostics(UNiagaraSystem& System, int32 MaxIssues, FDiagnostics& OutDiagnostics, FString& OutError);

	/**
	 * Checks one op's shape. With bResolveAssets it also resolves the asset or class the op names, loading a module
	 * script or emitter template if needed. It never loads or resolves the Niagara System being edited.
	 */
	bool ValidateOp(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic);

	/**
	 * Applies ops in order inside one undo transaction. Stops at the first failure; OutAppliedCount is how many ops
	 * landed before it, which decides whether the failure left an effect behind.
	 */
	bool ApplyOps(
		UNiagaraSystem& System,
		const TArray<FHyperAINiagaraEditOp>& Ops,
		int32& OutAppliedCount,
		TArray<FString>& OutPerOpStatus,
		FString& OutError);

	/** False if the experimental API's reflected class is missing from this engine build. */
	bool IsApiAvailable();
}
