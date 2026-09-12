// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Misc/TVariant.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

enum class EHyperAIBlueprintPinDirection : uint8
{
	Input,
	Output
};

enum class EHyperAIBlueprintDeleteMode : uint8
{
	OrphanOnly,
	ExplicitConfirmed
};

enum class EHyperAIBlueprintPatchSafety : uint8
{
	Edit,
	Destructive
};

/** Closed clean-room K2 creation allowlist. No class path or reflected factory is accepted. */
enum class EHyperAIBlueprintCreateNodeKind : uint8
{
	Branch,
	Sequence,
	Reroute
};

struct FHyperAIBlueprintGraphId
{
	FGuid GraphGuid;
	FString GraphPath;

	bool IsValid() const;
	FString StableKey() const;
};

struct FHyperAIBlueprintNodeId
{
	FHyperAIBlueprintGraphId Graph;
	FGuid NodeGuid;

	bool IsValid() const;
	FString StableKey() const;
};

/** PersistentGuid is preferred. Name/direction/index is a revision-bound fallback. */
struct FHyperAIBlueprintPinId
{
	FHyperAIBlueprintNodeId Node;
	FGuid PersistentGuid;
	FName PinName;
	EHyperAIBlueprintPinDirection Direction = EHyperAIBlueprintPinDirection::Input;
	int32 PinIndex = INDEX_NONE;

	bool IsValid() const;
	bool UsesRevisionBoundFallback() const { return !PersistentGuid.IsValid(); }
	FString StableKey() const;
};

struct FHyperAIBlueprintPatchPinSnapshot
{
	FHyperAIBlueprintPinId Id;
	FString PinType;
	FString DefaultValue;
	bool bDefaultReadOnly = false;
	bool bDefaultIgnored = false;
	bool bNotConnectable = false;
	bool bOrphaned = false;
	TArray<FString> LinkedPinKeys;
};

struct FHyperAIBlueprintPatchNodeSnapshot
{
	FHyperAIBlueprintNodeId Id;
	FString NodeClassPath;
	int32 X = 0;
	int32 Y = 0;
	bool bCanUserDelete = false;
	TArray<FHyperAIBlueprintPatchPinSnapshot> Pins;
};

struct FHyperAIBlueprintPatchGraphSnapshot
{
	FHyperAIBlueprintGraphId Id;
	FString SchemaClassPath;
	TArray<FHyperAIBlueprintPatchNodeSnapshot> Nodes;
};

/** Value-only, immutable-by-convention authoring snapshot. */
struct FHyperAIBlueprintPatchSnapshot
{
	bool bComplete = false;
	FString Status;
	FString Diagnostic;
	FString BlueprintAssetPath;
	FString CompileStatus;
	FString GeneratedClassPath;
	FString Revision;
	int32 GraphCount = 0;
	int32 NodeCount = 0;
	int32 PinCount = 0;
	int32 LinkCount = 0;
	TArray<FHyperAIBlueprintPatchGraphSnapshot> Graphs;
};

struct FHyperAIBlueprintMoveNode
{
	FHyperAIBlueprintNodeId Node;
	int32 X = 0;
	int32 Y = 0;
};

/** Client chooses a collision-checked stable GUID so later operations in the same patch can address the new pins. */
struct FHyperAIBlueprintCreateNode
{
	FHyperAIBlueprintGraphId Graph;
	FGuid NewNodeGuid;
	EHyperAIBlueprintCreateNodeKind Kind = EHyperAIBlueprintCreateNodeKind::Branch;
	int32 X = 0;
	int32 Y = 0;

	FHyperAIBlueprintNodeId NodeId() const { return { Graph, NewNodeGuid }; }
};

/** Internal typed workflow marker; never accepted as a generic client operation. */
struct FHyperAIBlueprintCompileOnly
{
};

struct FHyperAIBlueprintConnectPins
{
	FHyperAIBlueprintPinId A;
	FHyperAIBlueprintPinId B;
};

struct FHyperAIBlueprintBreakPinLink
{
	FHyperAIBlueprintPinId A;
	FHyperAIBlueprintPinId B;
};

struct FHyperAIBlueprintSetLiteralDefault
{
	FHyperAIBlueprintPinId Pin;
	FString Value;
};

struct FHyperAIBlueprintDeleteNode
{
	FHyperAIBlueprintNodeId Node;
	EHyperAIBlueprintDeleteMode Mode = EHyperAIBlueprintDeleteMode::OrphanOnly;
};

/** Nodes are placed in stable-key order on a bounded row-major grid. */
struct FHyperAIBlueprintLayoutNodes
{
	FHyperAIBlueprintGraphId Graph;
	TArray<FHyperAIBlueprintNodeId> Nodes;
	int32 OriginX = 0;
	int32 OriginY = 0;
	int32 Columns = 4;
	int32 HorizontalSpacing = 400;
	int32 VerticalSpacing = 240;
};

using FHyperAIBlueprintPatchOperationValue = TVariant<
	FHyperAIBlueprintMoveNode,
	FHyperAIBlueprintCreateNode,
	FHyperAIBlueprintCompileOnly,
	FHyperAIBlueprintConnectPins,
	FHyperAIBlueprintBreakPinLink,
	FHyperAIBlueprintSetLiteralDefault,
	FHyperAIBlueprintDeleteNode,
	FHyperAIBlueprintLayoutNodes>;

struct FHyperAIBlueprintPatchOperation
{
	FHyperAIBlueprintPatchOperationValue Value;

	FString Kind() const;
};

struct FHyperAIBlueprintPatch
{
	FString TargetAssetPath;
	FString ExpectedRevision;
	TArray<FHyperAIBlueprintPatchOperation> Operations;
};

struct FHyperAIBlueprintPatchIssue
{
	FString Code;
	FString Severity;
	int32 OperationIndex = INDEX_NONE;
	FString Target;
	FString Message;
};

struct FHyperAIBlueprintPatchEffectSummary
{
	int32 OperationCount = 0;
	int32 NoOpCount = 0;
	int32 NodesCreated = 0;
	int32 CompileRequests = 0;
	int32 NodesMoved = 0;
	int32 LinksAdded = 0;
	int32 LinksBroken = 0;
	int32 DefaultsChanged = 0;
	int32 NodesDeleted = 0;
	int32 LayoutNodesMoved = 0;
	bool bRequiresCompile = false;
	bool bRequiresCallerSave = false;
};

/** Pure planner output. It holds no UObject pointers. */
struct FHyperAIBlueprintPatchPlan
{
	bool bValid = false;
	FString Status;
	FString BaseRevision;
	FString PlanHash;
	EHyperAIBlueprintPatchSafety Safety = EHyperAIBlueprintPatchSafety::Edit;
	FHyperAIBlueprintPatchEffectSummary Effects;
	TArray<FHyperAIBlueprintPatchIssue> Issues;
	TMap<FString, FIntPoint> PlannedNodePositions;
};

struct FHyperAIBlueprintPatchResult
{
	bool bOk = false;
	FString Status;
	FString Diagnostic;
	FString RevisionBefore;
	FString RevisionAfter;
	FString PlanHash;
	FHyperAIBlueprintPatchEffectSummary Effects;
	TArray<FHyperAIBlueprintPatchIssue> Issues;
	bool bDryRun = false;
	bool bMutationStarted = false;
	bool bCompileAttempted = false;
	int32 CompileErrors = 0;
	int32 CompileWarnings = 0;
	bool bPreSaveValidated = false;
	bool bCallerSaveRequired = false;
	bool bSavePerformed = false;
	FString TransactionContext;
	FString TransactionTitle;
	FString RollbackState = TEXT("not_needed");
	bool bOwnedUndoVerified = false;
	bool bAuthoringSnapshotRestored = false;
	bool bGeneratedClassStateVerified = false;
};

/**
 * Clean-room Blueprint patch backend. It never loads an asset and never saves.
 * All UObject entry points require the game thread and an already-loaded UBlueprint.
 */
class FHyperAIStudioBlueprintPatch final
{
public:
	static constexpr const TCHAR* EditTypedOperationType = TEXT("blueprint.apply_patch");
	static constexpr const TCHAR* DeleteTypedOperationType = TEXT("blueprint.delete_patch");
	static constexpr int32 MaxGraphs = 128;
	static constexpr int32 MaxNodes = 4096;
	static constexpr int32 MaxPins = 32768;
	static constexpr int32 MaxLinks = 65536;
	static constexpr int32 MaxOperations = 256;
	static constexpr int32 MaxLayoutNodes = 1024;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxLiteralCharacters = 2048;
	static constexpr int32 MaxCoordinate = 1000000;
	static constexpr int32 AllowedCreateNodeKindCount = 3;

	static FHyperAIBlueprintPatchSnapshot CaptureLoaded(UBlueprint* Blueprint);

	/** Pure validation, simulation, deterministic layout, and effect summarization. */
	static FHyperAIBlueprintPatchPlan PlanSnapshot(
		const FHyperAIBlueprintPatchSnapshot& Snapshot,
		const FHyperAIBlueprintPatch& Patch);

	/** Binds the classified adapter plan to exactly one TypedPlan operation family. */
	static bool ValidatePlanSafety(
		const FHyperAIBlueprintPatchPlan& Plan,
		EHyperAIBlueprintPatchSafety RequiredSafety,
		FString& OutError);

	static bool TrySafetyForTypedOperation(
		const FString& OperationType,
		EHyperAIBlueprintPatchSafety& OutSafety);

	/** Dry-run against the current loaded revision; performs live schema checks but no mutation. */
	static FHyperAIBlueprintPatchResult DryRunLoaded(
		UBlueprint* Blueprint,
		const FHyperAIBlueprintPatch& Patch,
		const FString& TypedOperationType);

	/**
	 * Applies one prevalidated patch in one FScopedTransaction, compiles once,
	 * validates before caller save, and never saves. Failure undo is attempted
	 * only when the exact owned top transaction can be proven.
	 */
	static FHyperAIBlueprintPatchResult ApplyLoaded(
		UBlueprint* Blueprint,
		const FHyperAIBlueprintPatch& Patch,
		const FString& TypedOperationType);

	static TMap<FString, FIntPoint> ComputeDeterministicLayout(
		const FHyperAIBlueprintLayoutNodes& Layout,
		FString& OutError);

	/** Pure exact template used by planning, action discovery, and postcondition validation. */
	static bool BuildAllowedNodeTemplate(
		const FHyperAIBlueprintCreateNode& Create,
		FHyperAIBlueprintPatchNodeSnapshot& OutNode,
		FString& OutError);

	static const TCHAR* LexToString(EHyperAIBlueprintCreateNodeKind Kind);

	/** Strong semantic digest of the classified effect summary, distinct from the full plan hash. */
	static FString ComputeEffectFingerprint(const FHyperAIBlueprintPatchPlan& Plan);

private:
	static FHyperAIBlueprintPatchResult ValidateLoaded(
		UBlueprint* Blueprint,
		const FHyperAIBlueprintPatch& Patch,
		EHyperAIBlueprintPatchSafety RequiredSafety,
		FHyperAIBlueprintPatchSnapshot& OutSnapshot,
		FHyperAIBlueprintPatchPlan& OutPlan);
};
