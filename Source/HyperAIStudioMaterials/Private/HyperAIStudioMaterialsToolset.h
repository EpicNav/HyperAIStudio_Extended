// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMaterialsToolset.generated.h"

class UMaterial;

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

/** One typed setting on a node or material, e.g. {"texture", "/Game/T/T_Rock.T_Rock"}. Keys are closed per kind. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialNodeProperty
{
	GENERATED_BODY()

	UPROPERTY() FString Key;
	UPROPERTY() FString Value;
};

/** A property change on an existing (guid:) or plan-local node. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialPropertyEdit
{
	GENERATED_BODY()

	UPROPERTY() FString NodeId;
	UPROPERTY() FString Key;
	UPROPERTY() FString Value;
};

/** A material instance parameter. Type scalar "0.5", vector "r,g,b[,a]", texture object path, static_switch "true". */
USTRUCT(BlueprintType)
struct FHyperAIMaterialParameterValue
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Type;
	UPROPERTY() FString Value;
};

/** Shader cost of a compiled material. Status is compiling until every shader map has finished. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialCompileStats
{
	GENERATED_BODY()

	UPROPERTY() bool bAvailable = false;
	/** compiling, compiled, or compile_errors. */
	UPROPERTY() FString Status;
	UPROPERTY() int32 NumPixelShaderInstructions = 0;
	UPROPERTY() int32 NumVertexShaderInstructions = 0;
	UPROPERTY() int32 NumSamplers = 0;
	UPROPERTY() int32 NumPixelTextureSamples = 0;
	UPROPERTY() int32 NumVertexTextureSamples = 0;
	UPROPERTY() int32 NumVirtualTextureSamples = 0;
	UPROPERTY() int32 NumUVScalars = 0;
	UPROPERTY() int32 NumInterpolatorScalars = 0;
	UPROPERTY() TArray<FString> CompileErrors;
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
	/** Typed values of the node kind's keys; empty for opaque nodes outside the node catalog. */
	UPROPERTY() TArray<FHyperAIMaterialNodeProperty> Properties;
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
	/** Nodes whose class is outside the node catalog: their pins and wiring are captured, their settings are not. */
	UPROPERTY() int32 OpaqueNodeCount = 0;
	/** Saved package hash; sealed into the revision whenever opaque nodes exist, so saved edits to them still count. */
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() TArray<FString> CustomHlslNodeIds;
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
	/** Instruction and sampler counts; "compiling" until shaders finish, so re-run validate to read them. */
	UPROPERTY() FHyperAIMaterialCompileStats Stats;
	/** A PNG of the material, written when its last edit finished compiling. Empty until then. */
	UPROPERTY() FString PreviewImagePath;
	UPROPERTY() TArray<FString> CustomHlslNodeIds;
};

/**
 * One node to create. Kind is a node catalog kind: constant, constant2-4, scalar/vector/static_switch/texture
 * parameter, texture_sample, texcoord, panner, rotator, time, add, subtract, multiply, divide, min, max, lerp,
 * clamp, power, sine, cosine, one_minus, saturate, abs, frac, floor, ceil, square_root, normalize, dot, cross,
 * distance, append, component_mask, desaturation, fresnel, world_position, object_position, vertex_color,
 * camera_vector, pixel_normal_ws, function_call, custom_hlsl. compound_create_configure_graph also takes the
 * legacy Name/Group/Scalar/Vector fields for its five original kinds.
 */
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
	/** The kind's typed settings, e.g. {"texture", path}, {"code", hlsl}. */
	UPROPERTY() TArray<FHyperAIMaterialNodeProperty> Properties;
	/** Required for custom_hlsl in Hybrid mode: what the graph nodes could not express. */
	UPROPERTY() FString Justification;
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

	/**
	 * base_color, metallic, specular, roughness, emissive, opacity, opacity_mask, normal, ao; plus, for the node
	 * catalog operations, world_position_offset, subsurface_color, refraction, pixel_depth_offset, anisotropy, tangent.
	 */
	UPROPERTY() FString Property;
	UPROPERTY() FString FromNodeId;
	UPROPERTY() FString FromOutput;
};

/** Strict discriminated operation. No raw class, property, script, file, or reflection field exists. */
USTRUCT(BlueprintType)
struct FHyperAIMaterialPlanOperation
{
	GENERATED_BODY()

	/**
	 * create_material: new material from Nodes/Edges/Outputs/MaterialSettings.
	 * edit_graph: change an existing material at ExpectedRevision. Order: RemoveNodeIds, Disconnects, Nodes,
	 *   PropertyEdits, Edges, Outputs, MaterialSettings. Existing nodes are named by their inspect id (guid:...).
	 * create_material_instance: new instance of ParentPath with Parameters.
	 * set_instance_parameters: Parameters on an existing instance.
	 * compound_create_configure_graph: the original five-kind create. repair_semantic_graph: dry-run evidence only.
	 */
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
	/** edit_graph: existing node ids to delete. */
	UPROPERTY() TArray<FString> RemoveNodeIds;
	/** edit_graph: inputs to cut, by ToNodeId + ToInput; ToNodeId $material_output with a property cuts an output. */
	UPROPERTY() TArray<FHyperAIMaterialEdgeSpec> Disconnects;
	/** edit_graph: settings on existing or new nodes. */
	UPROPERTY() TArray<FHyperAIMaterialPropertyEdit> PropertyEdits;
	/** blend_mode, shading_model, domain, two_sided. */
	UPROPERTY() TArray<FHyperAIMaterialNodeProperty> MaterialSettings;
	/** create_material_instance: the parent material or instance, which may be created earlier in the same plan. */
	UPROPERTY() FString ParentPath;
	UPROPERTY() TArray<FHyperAIMaterialParameterValue> Parameters;
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
	UPROPERTY() int32 NodesRemoved = 0;
	UPROPERTY() int32 PropertiesSet = 0;
	UPROPERTY() int32 InstancesCreated = 0;
	UPROPERTY() int32 ParametersSet = 0;
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
	UPROPERTY() bool bTrustedPrepared = false;
	/** nodes, hlsl or hybrid, as sealed into this plan. */
	UPROPERTY() FString AuthoringMode;
	/** Every Custom HLSL node this plan adds or leaves in a touched material, with its stated reason. */
	UPROPERTY() TArray<FString> CustomHlslNodes;
	/** One PNG per edited asset, written after it compiles. Open them to see the result. */
	UPROPERTY() TArray<FString> PreviewImagePaths;
};

/** Exactly three functions form the core Materials atomic cohort. */
UCLASS()
class HYPERAISTUDIOMATERIALS_API UHyperAIStudioMaterialsToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Reads a loaded material or material function: every node with its id (guid:...), kind, typed properties and
	 * pins, every connection, the material outputs, and the revision edit_graph needs. Nodes outside the node
	 * catalog show as kind opaque. Scope on_disk_index checks existence without loading.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialInspectReport hyper_material_inspect(
		const FHyperAIMaterialInspectRequest& Request);

	/**
	 * Builds materials from real graph nodes. Operations: create_material, edit_graph (at the inspect revision),
	 * create_material_instance, set_instance_parameters. Prefer graph nodes; the Materials authoring mode decides
	 * whether custom_hlsl is refused (Nodes), needs a justification for math nodes cannot express (Hybrid), or is
	 * free (Hlsl). Dry-run first, then resubmit with bDryRun false, an operation_id and expected_plan_hash =
	 * plan_hash. Poll hyper_operation_status, then hyper_material_validate for compile errors, instruction counts
	 * and the preview PNG (open it to see the result). Close the material's editor tab before editing.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialApplyPlanReport hyper_material_apply_plan(
		const FHyperAIMaterialApplyPlanRequest& Request);

	/**
	 * Checks a material's graph and, once its shaders finish compiling, reports compile errors, pixel/vertex shader
	 * instruction counts, samplers and texture samples, lists its Custom HLSL nodes, and writes a preview PNG to
	 * preview_image_path. Stats.status is compiling until shaders finish; call again after a moment.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Materials")
	static FHyperAIMaterialValidateReport hyper_material_validate(
		const FHyperAIMaterialValidateRequest& Request);
};

enum class EHyperAIStudioMaterialOperationKind : uint8
{
	CompoundCreateConfigureGraph,
	RepairSemanticGraph,
	CreateMaterial,
	EditGraph,
	CreateMaterialInstance,
	SetInstanceParameters
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
	TArray<FString> RemoveNodeIds;
	TArray<FHyperAIMaterialEdgeSpec> Disconnects;
	TArray<FHyperAIMaterialPropertyEdit> PropertyEdits;
	TArray<FHyperAIMaterialNodeProperty> MaterialSettings;
	FString ParentPath;
	TArray<FHyperAIMaterialParameterValue> Parameters;
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
	/** nodes, hlsl or hybrid; sealed so a changed setting cannot slip between review and apply. */
	FString AuthoringMode;
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
	/** Content key of every target after the phase. */
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMaterialInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIMaterialInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMaterialValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIMaterialValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMaterialInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIMaterialInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMaterialValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIMaterialValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMaterialsFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};

/** Typed UE 5.8 adapter: reads directly, edits through the trusted executor's phases. */
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
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.material_editor");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_material_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_material_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_material_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("exact_semantic_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("persisted_semantic_graph.v2");
	static constexpr const TCHAR* ValidateVariantId = TEXT("independent_semantic_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.material.inspect.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.material.semantic_graph.v2");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.material.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.material.inspect.v1");
	static constexpr const TCHAR* ResultTypeId = TEXT("hyperai.result.material.semantic_graph.v2");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.material.validate.v1");
	/** Custom HLSL budgets: Hybrid nodes and bytes per node; Hlsl nodes and total bytes per plan. */
	static constexpr int32 MaxHybridCustomNodes = 4;
	static constexpr int32 MaxHybridCustomCodeBytes = 8 * 1024;
	static constexpr int32 MaxHlslCustomNodes = 16;
	static constexpr int32 MaxHlslCustomCodeBytes = 32 * 1024;
	static constexpr int32 MaxParametersPerPlan = 64;
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
	static FString InspectPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	/** The current authoring mode as sealed into plans: nodes, hlsl or hybrid. */
	static FString GetAuthoringModeName();
	/** Content key over every target of a payload, in target order. */
	static FString ComputePlanContentKey(const FHyperAIStudioMaterialTypedPayload& Payload);
	/** One revision over every target (inspect revision, instance content key, or absent); Apply recomputes it. */
	static bool ComputePlanBaseRevision(
		const TArray<FHyperAIStudioMaterialBackendOperation>& Operations, int32 MaxWorkMs, FString& OutBase, FString& OutError);
	/** The reviewable plan hash: ordered operations, base revision and authoring mode. */
	static FString ComputeSealedPlanFingerprint(const FHyperAIStudioMaterialTypedPayload& Payload);
	static FHyperAIMaterialInspectReport Inspect(const FHyperAIMaterialInspectRequest& Request);
	static FHyperAIMaterialValidateReport Validate(const FHyperAIMaterialValidateRequest& Request);
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool ValidateOperationShape(
		const FHyperAIMaterialPlanOperation& Operation,
		FHyperAIStudioMaterialBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError,
		double Deadline = MAX_dbl);
	/**
	 * Node-catalog checks for create_material and edit_graph against the engine's own node objects: kinds, keys,
	 * values, pins, parameter names, and the authoring mode's Custom HLSL rules. Existing is the loaded target
	 * for edit_graph. Adds each Custom HLSL node it accepts to OutCustomNodes.
	 */
	static bool ValidateGraphOperation(
		const FHyperAIStudioMaterialBackendOperation& Operation,
		const UMaterial* Existing,
		EHyperAIStudioMaterialAuthoringMode Mode,
		int32& InOutCustomNodes,
		int32& InOutCustomBytes,
		TArray<FString>& OutCustomNodes,
		FString& OutErrorCode,
		FString& OutError);
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
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioMaterialsDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
