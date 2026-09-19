// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMaterialsToolset.h"

class UMaterial;
class UMaterialExpression;
class UMaterialInterface;

/**
 * The one place plan operations become material graph edits: Epic's exported UMaterialEditingLibrary plus typed
 * writes to a closed set of expression fields. Nothing is dispatched by a name the agent chose. Each node kind
 * lists its keys, and each key maps to one typed field in code.
 */
namespace HyperAIStudio::Materials::Gate
{
	struct FNodeKindInfo
	{
		const TCHAR* Kind;
		const TCHAR* ClassPath;
		TArray<FString> Keys;
	};

	const TArray<FNodeKindInfo>& GetNodeKinds();
	const FNodeKindInfo* FindNodeKind(const FString& Kind);
	UClass* ResolveKindClass(const FNodeKindInfo& Info);
	/** Every catalog class resolves; the live probe fails closed otherwise. */
	bool ResolveAllKinds(FString& OutMissing);
	/** Catalog kind of an expression, or empty for a class outside the catalog. */
	FString KindOf(const UMaterialExpression& Expression);
	bool IsCustomKind(const FString& Kind);
	bool IsParameterKind(const FString& Kind);
	/** "guid:<expression guid>", the id inspect reports and edit plans use for existing nodes. */
	FString NodeIdOf(const UMaterialExpression& Expression);

	/** The kind's keys with their current values, sorted by key: shown by inspect and sealed into the revision. */
	TArray<FHyperAIMaterialNodeProperty> ReadProperties(const UMaterialExpression& Expression, const FString& Kind);
	/**
	 * Writes one key. bNotify false is for plan-time shadow nodes, which have no material to notify. A referenced
	 * texture or function is resolved, or loaded if nothing has it loaded.
	 */
	bool WriteProperty(UMaterialExpression& Expression, const FString& Kind, const FString& Key, const FString& Value,
		bool bNotify, FString& OutError);

	bool ValidateMaterialSetting(const FString& Key, const FString& Value, FString& OutError);
	bool WriteMaterialSetting(UMaterial& Material, const FString& Key, const FString& Value, FString& OutError);
	/** base_color ... world_position_offset, or MP_MAX. */
	EMaterialProperty OutputPropertyFromName(const FString& Name);

	/** Builds a transient copy of a node spec, so pins and values are checked by the engine's own objects. */
	UMaterialExpression* MakeShadowNode(const FHyperAIMaterialNodeSpec& Spec, FString& OutError);
	/** Input/output name matching exactly as UMaterialEditingLibrary connects them. */
	bool HasInput(UMaterialExpression& Expression, const FString& Name);
	bool HasOutput(UMaterialExpression& Expression, const FString& Name);
	/** A function call's pins come from its function; a shadow node cannot build them without a material. */
	bool GetFunctionPins(const FString& FunctionPath, TArray<FString>& OutInputs, TArray<FString>& OutOutputs, FString& OutError);

	/** Parameter names and types an instance of Parent can set, keyed by name. */
	TMap<FName, FString> GetParentParameters(UMaterialInterface& Parent);
	/** Parses a parameter value for its declared type; a texture value must load as a texture. */
	bool CheckParameterValue(const FHyperAIMaterialParameterValue& Parameter, FString& OutError);

	/** Applies one validated operation inside the caller's transaction. OutChanged counts edits that landed. */
	bool ApplyOperation(const FHyperAIStudioMaterialBackendOperation& Operation, int32& OutChanged, FString& OutError);

	/** Saved package hash plus dirty state: stable while shaders compile, changes with every edit or save. */
	FString ComputeContentKey(const UObject& Asset);
	bool IsOpenInEditor(const UObject& Asset);
	/** Custom HLSL nodes in a material, as inspect node ids. */
	TArray<FString> FindCustomNodes(const UMaterial& Material);
	/** Hybrid mode: code that is one `return` of operators and intrinsics the node library already has. */
	bool IsExpressibleWithNodes(const FString& Code, const TArray<FString>& InputNames);

	/** True once no shader map for this material is still compiling. */
	bool IsCompileFinished(const UMaterialInterface& Material);
	/** Never blocks: reports "compiling" until IsCompileFinished, since GetStatistics would wait for the compile. */
	FHyperAIMaterialCompileStats ReadStats(UMaterialInterface& Material);
}
