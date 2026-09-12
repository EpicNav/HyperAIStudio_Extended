// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMaterialsToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIMaterialIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString StableId;
	UPROPERTY() int32 OperationIndex = -1;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family;
	UPROPERTY() bool bInspectImplemented = true;
	UPROPERTY() bool bIndependentValidationImplemented = true;
	UPROPERTY() bool bCompoundBackendImplemented = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State;
	UPROPERTY() FString Remediation;
};

/** One bounded persisted expression input. Source identity is represented by the matching edge. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialInputPortView
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = -1;
	UPROPERTY() FString Name;
	UPROPERTY() FString PersistedName;
	UPROPERTY() int32 ValueType = -1;
	UPROPERTY() bool bConnected = false;
	UPROPERTY() int32 OutputIndex = -1;
	UPROPERTY() int32 Mask = 0;
	UPROPERTY() int32 MaskR = 0;
	UPROPERTY() int32 MaskG = 0;
	UPROPERTY() int32 MaskB = 0;
	UPROPERTY() int32 MaskA = 0;
};

/** One bounded persisted expression output, including its connection mask contract. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialOutputPortView
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = -1;
	UPROPERTY() FString Name;
	UPROPERTY() int32 ValueType = -1;
	UPROPERTY() int32 Mask = 0;
	UPROPERTY() int32 MaskR = 0;
	UPROPERTY() int32 MaskG = 0;
	UPROPERTY() int32 MaskB = 0;
	UPROPERTY() int32 MaskA = 0;
};

/** Persisted material-property state, including an unconnected constant and the closed input tuple. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialPropertyInputView
{
	GENERATED_BODY()

	UPROPERTY() int32 PropertyIndex = -1;
	UPROPERTY() FString Name;
	UPROPERTY() bool bAvailable = false;
	UPROPERTY() int32 ValueType = -1;
	UPROPERTY() bool bUseConstant = false;
	UPROPERTY() bool bHidden = false;
	UPROPERTY() FString ConstantState;
	UPROPERTY() FString PersistedInputName;
	UPROPERTY() bool bConnected = false;
	UPROPERTY() int32 OutputIndex = -1;
	UPROPERTY() int32 Mask = 0;
	UPROPERTY() int32 MaskR = 0;
	UPROPERTY() int32 MaskG = 0;
	UPROPERTY() int32 MaskB = 0;
	UPROPERTY() int32 MaskA = 0;
};

/** Persisted semantic node projection. Fields are closed and meaningful only for exact known classes. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialNodeView
{
	GENERATED_BODY()

	UPROPERTY() FString StableId;
	UPROPERTY() FString PersistedGuid;
	UPROPERTY() FString Kind;
	UPROPERTY() FString ClassPath;
	/** Actual UObject containment, sealed separately from semantic Material/Function owner pointers. */
	UPROPERTY() FString OuterPath;
	UPROPERTY() FString OuterClassPath;
	UPROPERTY() FString OutermostPackageName;
	/** Exact persisted expression owner links. Material nodes bind Material; function nodes bind Function. */
	UPROPERTY() FString MaterialOwnerPath;
	UPROPERTY() FString FunctionOwnerPath;
	UPROPERTY() int32 EditorX = 0;
	UPROPERTY() int32 EditorY = 0;
	UPROPERTY() FString Name;
	UPROPERTY() FString Group;
	UPROPERTY() FString Description;
	UPROPERTY() FString ParameterGuid;
	UPROPERTY() int32 SortPriority = 0;
	UPROPERTY() double Scalar = 0.0;
	UPROPERTY() FLinearColor Vector = FLinearColor::Black;
	UPROPERTY() int32 ScalarControlType = -1;
	UPROPERTY() double SliderMin = 0.0;
	UPROPERTY() double SliderMax = 0.0;
	UPROPERTY() FString EnumerationPath;
	UPROPERTY() int32 EnumerationIndex = 0;
	UPROPERTY() bool bUseCustomPrimitiveData = false;
	UPROPERTY() int32 PrimitiveDataIndex = 0;
	UPROPERTY() FString ChannelR;
	UPROPERTY() FString ChannelG;
	UPROPERTY() FString ChannelB;
	UPROPERTY() FString ChannelA;
	UPROPERTY() double ConstA = 0.0;
	UPROPERTY() double ConstB = 0.0;
	UPROPERTY() FString FunctionDescription;
	UPROPERTY() FString FunctionId;
	UPROPERTY() int32 FunctionInputType = -1;
	UPROPERTY() FVector4 FunctionPreviewValue = FVector4(0, 0, 0, 0);
	UPROPERTY() bool bUseFunctionPreviewValueAsDefault = false;
	UPROPERTY() int32 FunctionBlendInputRelevance = -1;
	UPROPERTY() bool bFunctionOutputLastPreviewed = false;
	UPROPERTY() FString SubgraphExpressionPath;
	UPROPERTY() FString SubgraphExpressionStableId;
	UPROPERTY() FString SubgraphRootStableId;
	UPROPERTY() bool bOwnerTopologyComplete = false;
	/** Persisted UMaterialExpression base flags and bounded config categories. */
	UPROPERTY() bool bRealtimePreview = false;
	UPROPERTY() bool bIsParameterExpression = false;
	UPROPERTY() bool bCommentBubbleVisible = false;
	UPROPERTY() bool bShowOutputNameOnPin = false;
	UPROPERTY() bool bShowMaskColorsOnPin = false;
	UPROPERTY() bool bHidePreviewWindow = false;
	UPROPERTY() bool bCollapsed = false;
	UPROPERTY() bool bShaderInputData = false;
	UPROPERTY() bool bShowInputs = false;
	UPROPERTY() bool bShowOutputs = false;
	UPROPERTY() TArray<FString> MenuCategories;
	UPROPERTY() TArray<FHyperAIMaterialInputPortView> InputPorts;
	UPROPERTY() TArray<FHyperAIMaterialOutputPortView> OutputPorts;
	UPROPERTY() bool bGuidValid = false;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialEdgeView
{
	GENERATED_BODY()

	UPROPERTY() FString FromStableId;
	UPROPERTY() int32 FromOutputIndex = 0;
	UPROPERTY() FString FromOutputName;
	UPROPERTY() FString ToStableId;
	UPROPERTY() int32 ToInputIndex = 0;
	UPROPERTY() FString ToInputName;
	UPROPERTY() int32 Mask = 0;
	UPROPERTY() int32 MaskR = 0;
	UPROPERTY() int32 MaskG = 0;
	UPROPERTY() int32 MaskB = 0;
	UPROPERTY() int32 MaskA = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Family;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString OuterPath;
	UPROPERTY() FString OutermostPackageName;
	UPROPERTY() bool bRootOwnershipComplete = false;
	UPROPERTY() FString StateId;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	/** exists, does_not_exist, or unknown. Unknown is never treated as absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() bool bExistsOnDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bCompileStateKnown = false;
	UPROPERTY() bool bCompiling = false;
	UPROPERTY() bool bCompileError = false;
	UPROPERTY() int64 DiskSize = -1;
	/** Bounded semantic edges only; dependency fanout is delegated to hyper_asset_dependency_graph. */
	UPROPERTY() FString ReferenceEvidence =
		TEXT("semantic_expression_edges_exact;asset_registry_fanout_delegated");
	UPROPERTY() int32 NodeCount = 0;
	UPROPERTY() int32 EdgeCount = 0;
	UPROPERTY() bool bGraphTruncated = false;
	UPROPERTY() FString ExpressionExecBeginStableId;
	UPROPERTY() FString ExpressionExecEndStableId;
	UPROPERTY() TArray<FHyperAIMaterialNodeView> Nodes;
	UPROPERTY() TArray<FHyperAIMaterialEdgeView> Edges;
	UPROPERTY() TArray<FHyperAIMaterialPropertyInputView> PropertyInputs;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialDiffEntry
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Before;
	UPROPERTY() FString After;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialInspectRequest
{
	GENERATED_BODY()

	/** One exact canonical /Game object path; never a folder or wildcard. */
	UPROPERTY() FString TargetPath;
	/** material or material_function. */
	UPROPERTY() FString TargetFamily = TEXT("material");
	/** loaded_only or on_disk_index. On-disk inspection never loads the asset. */
	UPROPERTY() FString Scope = TEXT("loaded_only");
	/** Optional exact second loaded asset of the same family for semantic diff. */
	UPROPERTY() FString ComparePath;
	UPROPERTY() int32 PageSize = 128;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxNodes = 512;
	UPROPERTY() int32 MaxEdges = 1024;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAIMaterialAssetRecord Target;
	UPROPERTY() FHyperAIMaterialAssetRecord Compare;
	UPROPERTY() TArray<FHyperAIMaterialDiffEntry> Diff;
	UPROPERTY() TArray<FHyperAIMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIMaterialCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString TargetFamily = TEXT("material");
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() bool bRequirePackageClean = false;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FHyperAIMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIMaterialCapabilityStatus> Capabilities;
};

/** Closed node vocabulary for the compound create/configure fast path. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialNodeSpec
{
	GENERATED_BODY()

	/** constant, scalar_parameter, vector_parameter, add, multiply, function_input, function_output. */
	UPROPERTY() FString Kind;
	/** Plan-local stable identifier; not a UObject name or class name. */
	UPROPERTY() FString NodeId;
	UPROPERTY() FString Name;
	UPROPERTY() FString Group;
	UPROPERTY() double Scalar = 0.0;
	UPROPERTY() FLinearColor Vector = FLinearColor::Black;
	UPROPERTY() int32 EditorX = 0;
	UPROPERTY() int32 EditorY = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialEdgeSpec
{
	GENERATED_BODY()

	UPROPERTY() FString FromNodeId;
	UPROPERTY() FString FromOutput;
	UPROPERTY() FString ToNodeId;
	UPROPERTY() FString ToInput;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialOutputSpec
{
	GENERATED_BODY()

	/** base_color, metallic, specular, roughness, emissive, opacity, opacity_mask, normal, ao. */
	UPROPERTY() FString Property;
	UPROPERTY() FString FromNodeId;
	UPROPERTY() FString FromOutput;
};

/** Strict discriminated operation. No raw class, property, script, file, or reflection field exists. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialPlanOperation
{
	GENERATED_BODY()

	/** compound_create_configure_graph or repair_semantic_graph. */
	UPROPERTY() FString Type;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString TargetFamily = TEXT("material");
	/** Empty only for create. */
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() TArray<FHyperAIMaterialNodeSpec> Nodes;
	UPROPERTY() TArray<FHyperAIMaterialEdgeSpec> Edges;
	UPROPERTY() TArray<FHyperAIMaterialOutputSpec> Outputs;
	/** regenerate_duplicate_guids, disconnect_dangling_inputs, disconnect_cycles. */
	UPROPERTY() TArray<FString> RepairKinds;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() int32 DeadlineMs = 2000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
	UPROPERTY() TArray<FHyperAIMaterialPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OperationCount = 0;
	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 AssetsCreated = 0;
	UPROPERTY() int32 AssetsRepaired = 0;
	UPROPERTY() int32 NodesCreated = 0;
	UPROPERTY() int32 ConnectionsCreated = 0;
	UPROPERTY() bool bTypedShadowReplayComplete = false;
	UPROPERTY() bool bTransactionOnce = false;
	UPROPERTY() bool bCompileOnce = false;
	UPROPERTY() bool bSaveOnce = false;
	UPROPERTY() bool bValidateOnce = false;
	UPROPERTY() bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMaterialApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	/** Pure typed-shadow evidence only; never authorizes or certifies execution. */
	UPROPERTY() FString ExpectedPostProjectionFingerprint;
	UPROPERTY() FHyperAIMaterialPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIMaterialCapabilityStatus> Capabilities;
};

/** Exactly three functions form the core Materials atomic cohort. */
UCLASS()
class HYPERAISTUDIOMATERIALS_API UHyperAIStudioMaterialsToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialInspectReport hyper_material_inspect(
		const FHyperAIMaterialInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialApplyPlanReport hyper_material_apply_plan(
		const FHyperAIMaterialApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialValidateReport hyper_material_validate(
		const FHyperAIMaterialValidateRequest& Request);
};

enum class EHyperAIStudioMaterialOperationKind : uint8
{
	CompoundCreateConfigureGraph,
	RepairSemanticGraph
};

struct FHyperAIStudioMaterialBackendOperation
{
	EHyperAIStudioMaterialOperationKind Kind =
		EHyperAIStudioMaterialOperationKind::CompoundCreateConfigureGraph;
	FString TargetPath;
	FString TargetFamily;
	FString ExpectedRevision;
	TArray<FHyperAIMaterialNodeSpec> Nodes;
	TArray<FHyperAIMaterialEdgeSpec> Edges;
	TArray<FHyperAIMaterialOutputSpec> Outputs;
	TArray<FString> RepairKinds;
};

struct FHyperAIStudioMaterialValueSnapshot
{
	FString Revision;
	bool bComplete = false;
	FHyperAIMaterialAssetRecord Record;
	TArray<FHyperAIMaterialIssue> CaptureIssues;
};

class FHyperAIStudioMaterialTypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FHyperAIStudioMaterialBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioMaterialResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Concrete typed UE 5.8 adapter. MCP remains disconnected until the central async host owns it. */
class FHyperAIStudioMaterialsDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioMaterialsDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioMaterialManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioMaterialsContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("materials_dynamic_material");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiomaterialstoolset.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("persisted_semantic_graph.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.material.semantic_graph.v1");
	static constexpr const TCHAR* ResultTypeId = TEXT("hyperai.result.material.semantic_graph.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_compile_or_runtime_cas_backend_required");
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxOperations = 8;
	static constexpr int32 MaxNodes = 512;
	static constexpr int32 MaxEdges = 1024;
	static constexpr int32 MaxPortsPerNode = 16;
	static constexpr int32 MaxMenuCategoriesPerNode = 16;
	static constexpr int32 MaxShaderValueComponents = 16;
	static constexpr int32 MaxSemanticTextCharacters = 2048;
	/** Aggregate capture/seal budget, deliberately below the shared 1 MiB hash ceiling. */
	static constexpr int32 MaxSnapshotMaterializedBytes = 512 * 1024;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 256;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioMaterialManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsExactLoadedFamily(const UObject* Object, const FString& Family);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static bool IsCreateAbsenceProven(
		bool bLoadedObjectPresent,
		UE::AssetRegistry::EExists State);
	static bool IsExpressionCollectionWithinBound(int32 Count, int32 RequestedMaxNodes);
	static bool IsBoundedSimpleSemanticText(const FText& Text);
	static TArray<FHyperAIMaterialCapabilityStatus> GetCapabilityMatrix();
	static FString PayloadSchemaFingerprint();
	static FString ResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool ValidateOperationShape(
		const FHyperAIMaterialPlanOperation& Operation,
		FHyperAIStudioMaterialBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError,
		double Deadline = MAX_dbl);
	static bool CaptureExact(
		const FString& Path,
		const FString& Family,
		const FString& Scope,
		int32 MaxNodes,
		int32 MaxEdges,
		int32 MaxWorkMs,
		FHyperAIStudioMaterialValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FString ComputeSnapshotRevision(
		FHyperAIStudioMaterialValueSnapshot& Snapshot,
		double Deadline = MAX_dbl);
	static TArray<FHyperAIMaterialIssue> ValidateValueSnapshot(
		const FHyperAIStudioMaterialValueSnapshot& Snapshot,
		bool bRequirePackageClean,
		int32 MaxIssueCount,
		bool& bOutTruncated,
		double Deadline = MAX_dbl);
	static FString ComputePayloadSemanticFingerprint(
		const TArray<FHyperAIStudioMaterialBackendOperation>& Operations,
		const FString& BaseRevision);
	static FString ComputeSealedNodeStableId(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		const FString& NodeId);
	static FString ComputeSealedParameterGuid(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		const FString& NodeId);
	static bool BuildExpectedCompoundSemanticGraph(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		TArray<FHyperAIMaterialNodeView>& OutNodes,
		TArray<FHyperAIMaterialEdgeView>& OutEdges,
		FString& OutError,
		double Deadline = MAX_dbl);
	static bool BuildExpectedCompoundPropertyState(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		TArray<FHyperAIMaterialPropertyInputView>& OutProperties,
		FString& OutError,
		double Deadline = MAX_dbl);
	/** Pure bounded repair replay used to seal an exact expected post-projection; never mutates UObjects. */
	static bool BuildExpectedRepairSemanticGraph(
		const FHyperAIStudioMaterialValueSnapshot& Before,
		const FHyperAIStudioMaterialBackendOperation& Operation,
		int32 MaxWorkMs,
		FHyperAIStudioMaterialValueSnapshot& OutAfter,
		FString& OutError);
	static bool VerifyCompoundPostconditions(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		const FHyperAIStudioMaterialValueSnapshot& Snapshot,
		FString& OutEffectFingerprint,
		FString& OutError);
	static bool VerifyRepairPostconditions(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		const FHyperAIStudioMaterialValueSnapshot& Snapshot,
		FString& OutEffectFingerprint,
		FString& OutError);
	static FHyperAIMaterialApplyPlanReport BuildPlan(
		const FHyperAIMaterialApplyPlanRequest& Request);
};

class FHyperAIStudioMaterialsRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
