// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioTextureGraphToolset.h"

class UTextureGraph;

/**
 * The only file that includes Texture Graph headers. Everything takes and returns plugin types, so when Epic's
 * Beta API moves the fix stays inside the gate's .cpp. Every function runs on the game thread.
 */
namespace HyperAIStudio::TextureGraph::Gate
{
	/** The exact asset, or null. Loads only when bLoad; never creates. */
	UTextureGraph* ResolveGraph(const FString& TargetPath, bool bLoad);
	bool IsOpenInEditor(const UTextureGraph& Graph);
	/** Texture Graph's engine singleton is created by its editor module; exports need it, graph edits do not. */
	bool IsEngineAvailable();
	int32 CountExportsInFlight();
	TArray<FString> GetExpressionCatalog();

	/** Hash of package identity, dirty state, nodes, unconnected pin values and edges. */
	FString ComputeRevision(const UTextureGraph& Graph);
	/** Hash of saved package identity and dirty state only: stable while a graph evaluates. */
	FString ComputeContentKey(const UTextureGraph& Graph);
	void ReadGraph(
		const UTextureGraph& Graph,
		bool bIncludePins,
		TArray<FHyperAITextureGraphNode>& OutNodes,
		TArray<FHyperAITextureGraphEdge>& OutEdges);
	TArray<FHyperAITextureGraphOutput> ReadOutputs(const UTextureGraph& Graph);
	/** Returns every issue; bRequireExported adds the checks policy exported needs. */
	TArray<FHyperAITextureGraphIssue> Validate(const UTextureGraph& Graph, bool bRequireExported);

	/**
	 * Applies ops in order. Stops at the first failing op; earlier ops stay applied, OutApplied says how many.
	 * bTransact wraps the batch in one undo step. OutNodeKeyIds maps add_node keys to the node ids they received.
	 */
	bool ApplyOps(
		UTextureGraph& Graph,
		const TArray<FHyperAITextureGraphEditOp>& Ops,
		bool bAutoLayout,
		bool bTransact,
		TMap<FString, int32>& OutNodeKeyIds,
		int32& OutApplied,
		FString& OutError);

	/**
	 * Runs the plan against a transient copy (or a transient new graph when Source is null) and reports what the
	 * asset would become. The asset itself is never touched.
	 */
	bool SimulatePlan(
		const UTextureGraph* Source,
		const FString& TargetPath,
		const TArray<FHyperAITextureGraphEditOp>& Ops,
		bool bAutoLayout,
		TMap<FString, int32>& OutNodeKeyIds,
		TArray<FHyperAITextureGraphOutput>& OutOutputs,
		TArray<FHyperAITextureGraphIssue>& OutIssues,
		FString& OutError);

	/** New asset with Epic's default single Output node, its texture defaulting to T_<AssetName> beside the graph. */
	UTextureGraph* CreateGraphAsset(const FString& TargetPath, FString& OutError);
	/** Starts Epic's asynchronous export of every exporting output, overwriting and saving the textures. */
	bool StartExport(UTextureGraph& Graph, FString& OutError);
}
