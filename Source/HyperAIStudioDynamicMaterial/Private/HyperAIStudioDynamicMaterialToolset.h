// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDynamicMaterialToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString ComponentPath;
	UPROPERTY() int32 OperationIndex = -1;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family;
	UPROPERTY() TArray<FString> RequiredPlugins;
	UPROPERTY() TArray<FString> RequiredModules;
	UPROPERTY() bool bPluginsInstalled = false;
	UPROPERTY() bool bPluginsEnabled = false;
	UPROPERTY() bool bModulesLoaded = false;
	UPROPERTY() bool bInspectImplemented = true;
	UPROPERTY() bool bIndependentValidationImplemented = true;
	UPROPERTY() bool bTypedBackendImplemented = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State;
	UPROPERTY() FString Remediation;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialValueView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() FString ValueKind;
	UPROPERTY() int32 ValueType = -1;
	UPROPERTY() FString ParameterName;
	UPROPERTY() int32 ParameterGroup = -1;
	UPROPERTY() bool bHasExplicitParameter = false;
	UPROPERTY() FString ParameterComponentPath;
	UPROPERTY() FString ParameterClassPath;
	UPROPERTY() int32 ParameterLifetimeState = -1;
	UPROPERTY() FString ParameterParentComponentPath;
	UPROPERTY() FString ExplicitParameterName;
	UPROPERTY() bool bLocal = false;
	UPROPERTY() bool bExposed = false;
	UPROPERTY() bool bBoolValue = false;
	UPROPERTY() double ScalarValue = 0.0;
	UPROPERTY() FVector4 VectorValue = FVector4(0, 0, 0, 0);
	UPROPERTY() bool bDefaultBoolValue = false;
	UPROPERTY() double DefaultScalarValue = 0.0;
	UPROPERTY() FVector4 DefaultVectorValue = FVector4(0, 0, 0, 0);
	/** Public UDMMaterialValueFloat setter range, sealed into CAS and target normalization. */
	UPROPERTY() bool bHasValueRange = false;
	UPROPERTY() double ValueRangeMin = 0.0;
	UPROPERTY() double ValueRangeMax = 0.0;
	UPROPERTY() FString ObjectValuePath;
	UPROPERTY() FString DefaultObjectValuePath;
	UPROPERTY() bool bMediaBridgeValue = false;
	UPROPERTY() bool bMediaSourceIdentityKnown = true;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialConnectorView
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = -1;
	UPROPERTY() FString Name;
	UPROPERTY() int32 ValueType = -1;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialConnectionChannelView
{
	GENERATED_BODY()

	UPROPERTY() int32 SourceIndex = -1;
	UPROPERTY() int32 MaterialProperty = -1;
	UPROPERTY() int32 OutputIndex = -1;
	UPROPERTY() int32 OutputChannel = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialConnectionView
{
	GENERATED_BODY()

	UPROPERTY() int32 InputIndex = -1;
	UPROPERTY() TArray<FHyperAIDynamicMaterialConnectionChannelView> Channels;
};

/** Closed public model-property surface and its bounded input-channel mapping. */
USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialPropertyView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() int32 MaterialProperty = -1;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bMaterialPin = false;
	UPROPERTY() int32 InputConnectorType = -1;
	UPROPERTY() FString OutputProcessorPath;
	/** Public built-in AlphaValue component identity; arbitrary hidden map keys are not inferred. */
	UPROPERTY() bool bHasAlphaValueComponent = false;
	UPROPERTY() FString AlphaValueComponentIdentity;
	UPROPERTY() TArray<FHyperAIDynamicMaterialConnectionChannelView> InputChannels;
	UPROPERTY() FString SlotComponentPath;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

/** Bounded public slot state that owns the ordered layer collection. */
USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialSlotView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() int32 SlotIndex = -1;
	/** One entry for every finite EDMMaterialPropertyType, in enum order. */
	UPROPERTY() TArray<FString> OutputConnectorTypeSets;
	/** Sorted exact component-path/count pairs from the public reference map. */
	UPROPERTY() TArray<FString> ReferencedBySlotCounts;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialComponentView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() FString Description;
	UPROPERTY() bool bInputRequired = false;
	UPROPERTY() bool bAllowsNestedInputs = false;
	UPROPERTY() FString ValueComponentPath;
	UPROPERTY() FString SlotComponentPath;
	UPROPERTY() int32 MaterialProperty = -1;
	UPROPERTY() int32 ChannelOverride = -1;
	UPROPERTY() TArray<FString> EditablePropertyNames;
	UPROPERTY() TArray<FHyperAIDynamicMaterialConnectorView> InputConnectors;
	UPROPERTY() TArray<FHyperAIDynamicMaterialConnectorView> OutputConnectors;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialStageView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() int32 StageIndex = -1;
	UPROPERTY() FString SourceClassPath;
	UPROPERTY() int32 StageType = -1;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bCanChangeSource = false;
	UPROPERTY() FHyperAIDynamicMaterialComponentView Source;
	UPROPERTY() TArray<FHyperAIDynamicMaterialComponentView> Inputs;
	UPROPERTY() TArray<FHyperAIDynamicMaterialConnectionView> InputConnections;
	UPROPERTY() bool bSemanticProjectionComplete = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialLayerView
{
	GENERATED_BODY()

	UPROPERTY() FString ComponentPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() int32 LifetimeState = -1;
	UPROPERTY() FString Name;
	UPROPERTY() int32 SlotIndex = -1;
	UPROPERTY() int32 LayerIndex = -1;
	UPROPERTY() int32 MaterialProperty = -1;
	UPROPERTY() bool bEnabled = false;
	UPROPERTY() bool bTextureUVLinkEnabled = false;
	UPROPERTY() FString EffectStackComponentPath;
	UPROPERTY() FString EffectStackClassPath;
	UPROPERTY() int32 EffectStackLifetimeState = -1;
	UPROPERTY() bool bEffectStackEnabled = false;
	UPROPERTY() int32 EffectCount = 0;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() TArray<FHyperAIDynamicMaterialStageView> Stages;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Family;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString ModelPath;
	UPROPERTY() FString ModelClassPath;
	UPROPERTY() FString AssociatedInstancePath;
	UPROPERTY() FString GeneratedMaterialPath;
	UPROPERTY() FString GeneratedMaterialStateId;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	/** exists, does_not_exist, or unknown. Unknown is never reported as absence. */
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() bool bExistsOnDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bGeneratedMaterialPackageDirty = false;
	UPROPERTY() bool bModelValid = false;
	UPROPERTY() bool bEditorModelDataAvailable = false;
	UPROPERTY() int32 EditorState = -1;
	UPROPERTY() bool bBuildRequested = false;
	UPROPERTY() bool bNeedsWizard = false;
	UPROPERTY() bool bPreviewModified = false;
	UPROPERTY() int32 MaterialDomain = -1;
	UPROPERTY() int32 BlendMode = -1;
	UPROPERTY() int32 ShadingModel = -1;
	UPROPERTY() int32 UsageFlags = 0;
	UPROPERTY() int32 GeneralFlags = 0;
	UPROPERTY() int32 LightingFlags = 0;
	UPROPERTY() int32 TranslucencyFlags = 0;
	UPROPERTY() int32 MotionFlags = 0;
	UPROPERTY() int32 ForwardRendererFlags = 0;
	UPROPERTY() double OpacityMaskClipValue = 0.0;
	UPROPERTY() double DisplacementCenter = 0.0;
	UPROPERTY() double DisplacementMagnitude = 0.0;
	UPROPERTY() bool bCompileStateKnown = false;
	UPROPERTY() bool bCompiling = false;
	UPROPERTY() bool bCompileError = false;
	UPROPERTY() int64 DiskSize = -1;
	/** Exact typed object-value/generated-material references; AR fanout is delegated. */
	UPROPERTY() FString ReferenceEvidence =
		TEXT("typed_object_paths_and_generated_material_exact;asset_registry_fanout_delegated");
	/** Public closed source/input component projection; unknown instance state blocks a complete revision. */
	UPROPERTY() FString ComponentProjectionEvidence =
		TEXT("closed_public_projection_with_owner_parent_roundtrip_v2;mutation_hidden_membership_fail_closed");
	UPROPERTY() int32 ValueCount = 0;
	UPROPERTY() int32 MaterialPropertyCount = 0;
	UPROPERTY() int32 SlotCount = 0;
	UPROPERTY() int32 LayerCount = 0;
	UPROPERTY() int32 StageCount = 0;
	UPROPERTY() bool bDetailsTruncated = false;
	/** Read projection may be complete while edit CAS remains incomplete on public UE 5.8 APIs. */
	UPROPERTY() bool bMutationRevisionComplete = false;
	UPROPERTY() TArray<FString> MutationRevisionBlockers;
	/** Exact path round-trip plus bounded UObject outer and component-parent chain evidence. */
	UPROPERTY() bool bComponentOwnershipProjectionComplete = false;
	UPROPERTY() TArray<FString> ComponentOwnershipIdentities;
	UPROPERTY() TArray<FHyperAIDynamicMaterialValueView> Values;
	UPROPERTY() TArray<FHyperAIDynamicMaterialPropertyView> MaterialProperties;
	UPROPERTY() TArray<FHyperAIDynamicMaterialSlotView> Slots;
	/** Sorted exact membership identity for the model's public runtime-component set. */
	UPROPERTY() TArray<FString> RuntimeComponentIdentities;
	UPROPERTY() TArray<FHyperAIDynamicMaterialLayerView> Layers;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	/** dynamic_material_instance or dynamic_material_model. */
	UPROPERTY() FString TargetFamily = TEXT("dynamic_material_instance");
	/** loaded_only or on_disk_index. */
	UPROPERTY() FString Scope = TEXT("loaded_only");
	UPROPERTY() int32 PageSize = 128;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxComponents = 1024;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialInspectReport
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
	UPROPERTY() FHyperAIDynamicMaterialAssetRecord Record;
	UPROPERTY() TArray<FHyperAIDynamicMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIDynamicMaterialCapabilityStatus> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString TargetFamily = TEXT("dynamic_material_instance");
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() bool bRequirePackageClean = false;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialValidateReport
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
	UPROPERTY() TArray<FHyperAIDynamicMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIDynamicMaterialCapabilityStatus> Capabilities;
};

/** Strict type-specific component edit. No class, property, script, or reflection selector exists. */
USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialPlanOperation
{
	GENERATED_BODY()

	/** value.set_bool|scalar|vector2|vector3|rotator|color or layer.set_enabled|stage.set_enabled. */
	UPROPERTY() FString Type;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString TargetFamily = TEXT("dynamic_material_instance");
	UPROPERTY() FString ExpectedRevision;
	/** Exact inspector-issued model component path. */
	UPROPERTY() FString ComponentPath;
	UPROPERTY() bool bBoolValue = false;
	UPROPERTY() double ScalarValue = 0.0;
	UPROPERTY() FVector4 VectorValue = FVector4(0, 0, 0, 0);
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() int32 DeadlineMs = 2000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
	UPROPERTY() TArray<FHyperAIDynamicMaterialPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OperationCount = 0;
	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 ValueChanges = 0;
	UPROPERTY() int32 LayerChanges = 0;
	UPROPERTY() int32 StageChanges = 0;
	UPROPERTY() bool bTypedShadowReplayComplete = false;
	UPROPERTY() bool bTransactionOnce = false;
	UPROPERTY() bool bCompileOnce = false;
	UPROPERTY() bool bSaveOnce = false;
	UPROPERTY() bool bValidateOnce = false;
	UPROPERTY() bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDynamicMaterialApplyPlanReport
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
	UPROPERTY() FHyperAIDynamicMaterialPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIDynamicMaterialIssue> Issues;
	UPROPERTY() TArray<FHyperAIDynamicMaterialCapabilityStatus> Capabilities;
};

UCLASS()
class HYPERAISTUDIODYNAMICMATERIAL_API UHyperAIStudioDynamicMaterialToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|DynamicMaterial")
	static FHyperAIDynamicMaterialInspectReport hyper_dynamic_material_inspect(
		const FHyperAIDynamicMaterialInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|DynamicMaterial")
	static FHyperAIDynamicMaterialApplyPlanReport hyper_dynamic_material_apply_plan(
		const FHyperAIDynamicMaterialApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|DynamicMaterial")
	static FHyperAIDynamicMaterialValidateReport hyper_dynamic_material_validate(
		const FHyperAIDynamicMaterialValidateRequest& Request);
};

enum class EHyperAIStudioDynamicMaterialOperationKind : uint8
{
	SetBool,
	SetScalar,
	SetVector2,
	SetVector3,
	SetRotator,
	SetColor,
	SetLayerEnabled,
	SetStageEnabled
};

struct FHyperAIStudioDynamicMaterialBackendOperation
{
	EHyperAIStudioDynamicMaterialOperationKind Kind = EHyperAIStudioDynamicMaterialOperationKind::SetBool;
	FString Type;
	FString TargetPath;
	FString TargetFamily;
	FString ExpectedRevision;
	FString ComponentPath;
	bool bBoolValue = false;
	double ScalarValue = 0.0;
	FVector4 VectorValue = FVector4(0, 0, 0, 0);
};

struct FHyperAIStudioDynamicMaterialValueSnapshot
{
	FString Revision;
	bool bComplete = false;
	FHyperAIDynamicMaterialAssetRecord Record;
	TArray<FHyperAIDynamicMaterialIssue> CaptureIssues;
};

class FHyperAIStudioDynamicMaterialTypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FHyperAIStudioDynamicMaterialBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioDynamicMaterialResultPayload final : public IHyperAIStudioDomainResultPayload
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

class FHyperAIStudioDynamicMaterialDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioDynamicMaterialDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioDynamicMaterialManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioDynamicMaterialContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("materials_dynamic_material");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiodynamicmaterialtoolset.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("dynamic_model_component_edit.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.dynamic_material.component_edit.v1");
	static constexpr const TCHAR* ResultTypeId = TEXT("hyperai.result.dynamic_material.component_edit.v1");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("bounded_compile_or_runtime_cas_backend_required");
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxComponentPathCharacters = 2048;
	/** Aggregate capture/seal budget, deliberately below the shared 1 MiB hash ceiling. */
	static constexpr int32 MaxSnapshotMaterializedBytes = 512 * 1024;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxComponents = 2048;
	static constexpr int32 MaxConnectorsPerComponent = 16;
	static constexpr int32 MaxChannelsPerConnection = 4;
	static constexpr int32 MaxEditablePropertiesPerComponent = 16;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 256;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioDynamicMaterialManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsExactLoadedFamily(const UObject* Object, const FString& Family);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString ExpectedMaterialPropertyClassPath(int32 MaterialProperty);
	static bool IsComponentCollectionWithinBound(int32 Count, int32 RequestedMaxComponents);
	static bool IsBoundedSimpleSemanticText(const FText& Text, int32 MaxCharacters);
	static bool IsExactValueOperationMatch(
		const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
		const FHyperAIDynamicMaterialValueView& Value);
	static bool NormalizeOperationForCapturedValue(
		FHyperAIStudioDynamicMaterialBackendOperation& Operation,
		const FHyperAIDynamicMaterialValueView& Value,
		FString& OutErrorCode);
	/** Mirrors the exact public setter no-op gates against pure captured shadow state. */
	static bool WouldValueSetterHaveEffect(
		const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
		const FHyperAIDynamicMaterialValueView& Value);
	static bool IsClosedMutationProjection(
		const FHyperAIStudioDynamicMaterialBackendOperation& Operation,
		const FHyperAIDynamicMaterialAssetRecord& Record);
	static TArray<FHyperAIDynamicMaterialCapabilityStatus> GetCapabilityMatrix();
	static FString PayloadSchemaFingerprint();
	static FString ResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static bool ValidateOperationShape(
		const FHyperAIDynamicMaterialPlanOperation& Operation,
		FHyperAIStudioDynamicMaterialBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError,
		double Deadline = MAX_dbl);
	static bool CaptureExact(
		const FString& Path,
		const FString& Family,
		const FString& Scope,
		int32 MaxComponents,
		int32 MaxWorkMs,
		FHyperAIStudioDynamicMaterialValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FString ComputeSnapshotRevision(
		FHyperAIStudioDynamicMaterialValueSnapshot& Snapshot,
		double Deadline = MAX_dbl);
	static TArray<FHyperAIDynamicMaterialIssue> ValidateValueSnapshot(
		const FHyperAIStudioDynamicMaterialValueSnapshot& Snapshot,
		bool bRequirePackageClean,
		int32 MaxIssueCount,
		bool& bOutTruncated,
		double Deadline = MAX_dbl);
	static FString ComputePayloadSemanticFingerprint(
		const TArray<FHyperAIStudioDynamicMaterialBackendOperation>& Operations,
		const FString& BaseRevision);
	static FHyperAIDynamicMaterialApplyPlanReport BuildPlan(
		const FHyperAIDynamicMaterialApplyPlanRequest& Request);
};

class FHyperAIStudioDynamicMaterialRegistration final
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
