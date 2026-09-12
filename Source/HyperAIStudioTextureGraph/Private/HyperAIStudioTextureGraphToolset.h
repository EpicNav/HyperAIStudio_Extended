// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioTextureGraphToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAITextureGraphIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error | warning | info */
	UPROPERTY() FString Severity;
	UPROPERTY() int32 NodeId = INDEX_NONE;
	UPROPERTY() FString Pin;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("texture_graph");
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State = TEXT("source_candidate_graph_edit_ops");
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphPin
{
	GENERATED_BODY()

	/** Use as pin / to_pin in edit ops. */
	UPROPERTY() FString Name;
	/** Display name; for graph parameters this is the parameter name. */
	UPROPERTY() FString Alias;
	/** input | output | setting | private */
	UPROPERTY() FString Direction;
	UPROPERTY() FString CppType;
	/** Current value text of an unconnected input or setting, in the form set_pin_value accepts. */
	UPROPERTY() FString Value;
	UPROPERTY() bool bConnected = false;
	UPROPERTY() bool bParam = false;
	/** Accepted values when the pin is an enum. */
	UPROPERTY() TArray<FString> EnumValues;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphNode
{
	GENERATED_BODY()

	/** Use as node / to_node in edit ops. */
	UPROPERTY() int32 NodeId = INDEX_NONE;
	UPROPERTY() FString ExpressionClass;
	UPROPERTY() FString Title;
	UPROPERTY() int32 PosX = 0;
	UPROPERTY() int32 PosY = 0;
	UPROPERTY() FString Comment;
	UPROPERTY() TArray<FHyperAITextureGraphPin> Pins;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphEdge
{
	GENERATED_BODY()

	UPROPERTY() int32 FromNode = INDEX_NONE;
	UPROPERTY() FString FromPin;
	UPROPERTY() int32 ToNode = INDEX_NONE;
	UPROPERTY() FString ToPin;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphOutput
{
	GENERATED_BODY()

	UPROPERTY() int32 NodeId = INDEX_NONE;
	UPROPERTY() FString OutputName;
	UPROPERTY() FString BaseName;
	UPROPERTY() FString FolderPath;
	/** Pixels; 0 means Auto. */
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	UPROPERTY() FString TextureFormat;
	UPROPERTY() bool bSRGB = false;
	UPROPERTY() bool bShouldExport = true;
	UPROPERTY() bool bSourceConnected = false;
	/** Object path the export writes. */
	UPROPERTY() FString TexturePath;
	/** missing | saved | dirty | conflict (a non-texture asset occupies the path) */
	UPROPERTY() FString TextureState = TEXT("missing");
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphInspectRequest
{
	GENERATED_BODY()

	/** One canonical /Game object path, e.g. /Game/Textures/TG_Rock.TG_Rock. */
	UPROPERTY() FString TargetPath;
	/** Load the asset if it is not loaded. Loading never modifies it. */
	UPROPERTY() bool bLoad = false;
	UPROPERTY() bool bIncludePins = true;
	/** Also list every expression class add_node accepts. */
	UPROPERTY() bool bIncludeExpressionCatalog = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** Pass as expected_revision to hyper_texture_graph_apply_plan. */
	UPROPERTY() FString Revision;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bExistsOnDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bOpenInEditor = false;
	UPROPERTY() bool bEngineAvailable = false;
	UPROPERTY() int32 ExportsInFlight = 0;
	UPROPERTY() TArray<FHyperAITextureGraphNode> Nodes;
	UPROPERTY() TArray<FHyperAITextureGraphEdge> Edges;
	UPROPERTY() TArray<FHyperAITextureGraphOutput> Outputs;
	UPROPERTY() TArray<FString> ExpressionClasses;
	UPROPERTY() TArray<FHyperAITextureGraphCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	/** Optional exact revision assertion from inspect. */
	UPROPERTY() FString ExpectedRevision;
	/** authoring fails on errors; exported also needs every exporting output saved as a texture, with no export running. */
	UPROPERTY() FString Policy = TEXT("authoring");
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Policy;
	UPROPERTY() FString Revision;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() int32 ExportsInFlight = 0;
	UPROPERTY() TArray<FHyperAITextureGraphOutput> Outputs;
	UPROPERTY() TArray<FHyperAITextureGraphIssue> Issues;
};

/** One closed edit; unused fields stay empty. Nodes are a node_id from inspect or an earlier add_node key. */
USTRUCT(BlueprintType)
struct FHyperAITextureGraphEditOp
{
	GENERATED_BODY()

	/** add_node | remove_node | connect | disconnect | set_pin_value | set_pin_alias | set_output | set_node_comment | move_node */
	UPROPERTY() FString Kind;
	/** add_node: plan-local key later ops use as node, e.g. noise. */
	UPROPERTY() FString NodeKey;
	/** add_node: expression class, e.g. TG_Expression_Noise. */
	UPROPERTY() FString ExpressionClass;
	/** The node to edit; connect/disconnect: the source node. */
	UPROPERTY() FString Node;
	/** The pin to edit; connect/disconnect: the source output pin. */
	UPROPERTY() FString Pin;
	/** connect/disconnect: destination node and input pin. */
	UPROPERTY() FString ToNode;
	UPROPERTY() FString ToPin;
	/** set_pin_value: 0.5 | 3 | true | (R=1,G=0.5,B=0,A=1) | 1,2,3,4 | enum name | /Game/T.T. set_pin_alias, set_node_comment: the text. */
	UPROPERTY() FString Value;
	/** set_output: texture asset name and /Game folder. */
	UPROPERTY() FString BaseName;
	UPROPERTY() FString FolderPath;
	/** set_output: 0 (Auto) or a power of two from 8 to 8192. */
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	/** set_output: ETG_TextureFormat name, e.g. BGRA8; empty keeps the current format. */
	UPROPERTY() FString TextureFormat;
	UPROPERTY() bool bSRGB = false;
	UPROPERTY() bool bShouldExport = true;
	/** move_node, or add_node when the plan disables auto layout. */
	UPROPERTY() int32 PosX = 0;
	UPROPERTY() int32 PosY = 0;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	/** Non-dry only: a fresh operation id, later passed to hyper_operation_status. */
	UPROPERTY() FString OperationId;
	/** Non-dry only: the plan_hash the dry run returned. */
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	/** Revision from inspect; must be empty when bCreate is set. */
	UPROPERTY() FString ExpectedRevision;
	/** Create a new Texture Graph at target_path. It starts with one Output node, node_id 0. */
	UPROPERTY() bool bCreate = false;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAITextureGraphEditOp> Ops;
	/** Place added nodes in columns left of the outputs they feed. */
	UPROPERTY() bool bAutoLayout = true;
	/** Export textures after saving. Runs asynchronously; confirm with hyper_texture_graph_validate policy exported. */
	UPROPERTY() bool bExport = false;
	UPROPERTY() bool bSave = true;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OpCount = 0;
	UPROPERTY() bool bCreatesAsset = false;
	UPROPERTY() bool bWouldSave = false;
	UPROPERTY() bool bWouldExport = false;
	/** Node ids add_node keys receive. */
	UPROPERTY() TMap<FString, int32> NodeKeyIds;
	UPROPERTY() TArray<FString> ExportTexturePaths;
	/** Existing textures the export would overwrite. */
	UPROPERTY() TArray<FString> OverwrittenTexturePaths;
};

USTRUCT(BlueprintType)
struct FHyperAITextureGraphApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FHyperAITextureGraphPlanEffects Effects;
	/** Validation of the graph as the plan would leave it. */
	UPROPERTY() TArray<FHyperAITextureGraphIssue> Issues;
};

/** Exactly three functions form the Texture Graph atomic cohort. */
UCLASS()
class HYPERAISTUDIOTEXTUREGRAPH_API UHyperAIStudioTextureGraphToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Nodes, pins with values, edges, outputs, export state and revision of a Texture Graph. Set bIncludeExpressionCatalog for the node classes add_node accepts. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|TextureGraph")
	static FHyperAITextureGraphInspectReport hyper_texture_graph_inspect(
		const FHyperAITextureGraphInspectRequest& Request);

	/** Create or edit a Texture Graph as one undo step: nodes, connections, pin values, outputs, then save and export. Dry-run, resubmit with operation_id and plan_hash, poll hyper_operation_status. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|TextureGraph")
	static FHyperAITextureGraphApplyPlanReport hyper_texture_graph_apply_plan(
		const FHyperAITextureGraphApplyPlanRequest& Request);

	/** Structural errors and export state of a Texture Graph. Policy exported confirms every output texture was written and saved. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|TextureGraph")
	static FHyperAITextureGraphValidateReport hyper_texture_graph_validate(
		const FHyperAITextureGraphValidateRequest& Request);
};

class FHyperAIStudioTextureGraphInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAITextureGraphInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioTextureGraphValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAITextureGraphValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioTextureGraphEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	/** Empty when bCreate. */
	FString BaseRevision;
	bool bCreate = false;
	TArray<FHyperAITextureGraphEditOp> Ops;
	bool bAutoLayout = true;
	bool bExport = false;
	bool bSave = true;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioTextureGraphInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAITextureGraphInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioTextureGraphValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAITextureGraphValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioTextureGraphMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioTextureGraphDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioTextureGraphFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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

class FHyperAIStudioTextureGraphContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("texture_graph");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiotexturegraphtoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.texture_graph_engine");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_texture_graph_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_texture_graph_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_texture_graph_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("exact_graph_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("graph_edit_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("graph_structure_and_exports.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.texture_graph.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.texture_graph.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.texture_graph.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.texture_graph.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.texture_graph.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.texture_graph.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 64;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxTextCharacters = 1024;
	static constexpr int32 MaxNodes = 1024;
	static constexpr int32 MaxIssues = 256;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FHyperAITextureGraphCapabilityStatus> GetCapabilityMatrix();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAITextureGraphInspectReport Inspect(const FHyperAITextureGraphInspectRequest& Request);
	static FHyperAITextureGraphValidateReport Validate(const FHyperAITextureGraphValidateRequest& Request);
	static FHyperAITextureGraphApplyPlanReport BuildPlan(const FHyperAITextureGraphApplyPlanRequest& Request);
	/** Order-sensitive: the same ops in a different order are a different plan. */
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioTextureGraphEditOpsPayload& Payload);
};

class FHyperAIStudioTextureGraphRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioTextureGraphDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
