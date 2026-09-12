// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Layout/Margin.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioUIToolset.generated.h"

/** One closed scalar from a UI value snapshot. Type selects exactly one value field. */
USTRUCT(BlueprintType)
struct FHyperAIUIFieldValue
{
	GENERATED_BODY()

	/** Closed field id; no raw reflection handle or reflection path is accepted. */
	UPROPERTY()
	FString Id;

	/** string, bool, int, number, color, vector2, margin, enum, guid, or path. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	FLinearColor ColorValue = FLinearColor::Transparent;

	UPROPERTY()
	FVector2D Vector2Value = FVector2D::ZeroVector;

	UPROPERTY()
	FMargin MarginValue;
};

/** Stable, value-only row from a loaded Widget Blueprint snapshot. */
USTRUCT(BlueprintType)
struct FHyperAIUIRecord
{
	GENERATED_BODY()

	/** asset_state, blueprint, widget, slot, animation, animation_binding, legacy_binding, mvvm_view, mvvm_viewmodel, or mvvm_binding. */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString RecordKey;

	UPROPERTY()
	FString BlueprintPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ParentStableId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	bool bPersisted = true;

	UPROPERTY()
	TArray<FHyperAIUIFieldValue> Fields;
};

USTRUCT(BlueprintType)
struct FHyperAIUIIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString BlueprintPath;

	UPROPERTY()
	FString RecordKey;

	UPROPERTY()
	FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIUIInspectRequest
{
	GENERATED_BODY()

	/** Canonical /Game object paths. Empty scans a bounded prefix of already-loaded Widget Blueprints. */
	UPROPERTY()
	TArray<FString> TargetPaths;

	UPROPERTY()
	bool bIncludeTree = true;

	UPROPERTY()
	bool bIncludeLayout = true;

	UPROPERTY()
	bool bIncludeAnimations = true;

	UPROPERTY()
	bool bIncludeLegacyBindings = true;

	UPROPERTY()
	bool bIncludeMVVM = true;

	/** Package dirtiness and compile status are volatile and deliberately make paging unavailable. */
	UPROPERTY()
	bool bIncludeVolatile = false;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 DeadlineMs = 750;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	bool bSnapshotComplete = false;

	UPROPERTY()
	bool bVolatileIncluded = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIUIRecord> Records;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIUIValidateRequest
{
	GENERATED_BODY()

	/** Canonical exact object paths. Empty validates a bounded loaded-only scope. */
	UPROPERTY()
	TArray<FString> TargetPaths;

	UPROPERTY()
	bool bCheckLayout = true;

	UPROPERTY()
	bool bCheckAccessibility = true;

	UPROPERTY()
	bool bCheckAnimations = true;

	UPROPERTY()
	bool bCheckBindings = true;

	UPROPERTY()
	bool bCheckMVVM = true;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 DeadlineMs = 750;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	bool bSnapshotComplete = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 LoadedObjectsScanned = 0;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

/** Closed operation; fields unused by Kind must retain their defaults. */
USTRUCT(BlueprintType)
struct FHyperAIUIPlanOperation
{
	GENERATED_BODY()

	/**
	 * animation.create|animation.rename|animation.delete,
	 * widget.set_property|widget.set_style,
	 * binding.create_legacy|binding.update_legacy|binding.replace_legacy|binding.delete_legacy,
	 * binding.create_mvvm|binding.update_mvvm|binding.replace_mvvm|binding.delete_mvvm.
	 */
	UPROPERTY()
	FString Kind;

	/** Widget name, animation name, or stable binding id depending on Kind. */
	UPROPERTY()
	FString SubjectId;

	/** Exact per-element fingerprint when updating, replacing, renaming, or deleting. */
	UPROPERTY()
	FString ExpectedElementFingerprint;

	/** New animation name or allowlisted property/style id. */
	UPROPERTY()
	FString Name;

	/** animation name, legacy event/property, or MVVM endpoint/path metadata. */
	UPROPERTY()
	FString SecondaryName;

	UPROPERTY()
	FString SourceEndpoint;

	UPROPERTY()
	FString SourcePath;

	UPROPERTY()
	FString DestinationEndpoint;

	UPROPERTY()
	FString DestinationPath;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	bool bHasBoolValue = false;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	bool bHasNumberValue = false;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	bool bHasColorValue = false;

	UPROPERTY()
	FLinearColor ColorValue = FLinearColor::Transparent;

	UPROPERTY()
	int32 StartFrame = 0;

	UPROPERTY()
	int32 EndFrame = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIUIApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Required for a non-dry retry envelope; no mutation or stage is submitted by this source candidate. */
	UPROPERTY()
	FString OperationId;

	/** Exact plan hash returned by dry-run. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString TargetPath;

	/** Exact persisted snapshot CAS. */
	UPROPERTY()
	FString ExpectedPersistedFingerprint;

	/** structural, layout_accessibility, or compile_ready. */
	UPROPERTY()
	FString ValidationPolicy = TEXT("layout_accessibility");

	UPROPERTY()
	TArray<FHyperAIUIPlanOperation> Operations;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUIPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 AnimationCreates = 0;

	UPROPERTY()
	int32 AnimationEdits = 0;

	UPROPERTY()
	int32 AnimationDeletes = 0;

	UPROPERTY()
	int32 PropertyEdits = 0;

	UPROPERTY()
	int32 StyleEdits = 0;

	UPROPERTY()
	int32 BindingCreates = 0;

	UPROPERTY()
	int32 BindingEdits = 0;

	UPROPERTY()
	int32 BindingDeletesOrReplaces = 0;

	UPROPERTY()
	bool bWouldSaveOnce = false;

	UPROPERTY()
	bool bWouldCompileOnce = false;

	UPROPERTY()
	bool bWouldValidateOnce = false;

	UPROPERTY()
	bool bWouldVerifyFreshOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIUIApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bTypedPrepared = false;

	UPROPERTY()
	bool bStaged = false;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass;

	UPROPERTY()
	FString VariantId;

	UPROPERTY()
	FString BasePersistedFingerprint;

	UPROPERTY()
	FString DesiredPersistedFingerprint;

	UPROPERTY()
	FString SemanticFingerprint;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FHyperAIUIPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIUIIssue> Issues;
};

/** Exact atomic pack: no Epic callable is re-exposed under a second name. */
UCLASS()
class HYPERAISTUDIOUI_API UHyperAIStudioUIToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIInspectReport hyper_ui_inspect(const FHyperAIUIInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIApplyPlanReport hyper_ui_apply_plan(const FHyperAIUIApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|UI")
	static FHyperAIUIValidateReport hyper_ui_validate(const FHyperAIUIValidateRequest& Request);
};

enum class EHyperAIStudioUIPlanSafety : uint8
{
	Edit,
	Destructive
};

enum class EHyperAIStudioUIOperationKind : uint8
{
	AnimationCreate,
	AnimationRename,
	AnimationDelete,
	WidgetSetProperty,
	WidgetSetStyle,
	BindingCreateLegacy,
	BindingUpdateLegacy,
	BindingReplaceLegacy,
	BindingDeleteLegacy,
	BindingCreateMVVM,
	BindingUpdateMVVM,
	BindingReplaceMVVM,
	BindingDeleteMVVM
};

struct FHyperAIStudioUIBackendOperation
{
	EHyperAIStudioUIOperationKind Kind = EHyperAIStudioUIOperationKind::WidgetSetProperty;
	EHyperAIStudioUIPlanSafety Safety = EHyperAIStudioUIPlanSafety::Edit;
	FString SubjectId;
	FString ExpectedElementFingerprint;
	FString Name;
	FString SecondaryName;
	FString SourceEndpoint;
	FString SourcePath;
	FString DestinationEndpoint;
	FString DestinationPath;
	FString StringValue;
	bool bHasBoolValue = false;
	bool bBoolValue = false;
	bool bHasNumberValue = false;
	double NumberValue = 0.0;
	bool bHasColorValue = false;
	FLinearColor ColorValue = FLinearColor::Transparent;
	int32 StartFrame = 0;
	int32 EndFrame = 0;
};

/** Deep-copyable closed payload used only by the public pure typed-artifact sealer. */
class FHyperAIStudioUIPlanPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedFingerprint;
	FString DesiredPersistedFingerprint;
	FString ValidationPolicy;
	FString SemanticFingerprint;
	EHyperAIStudioUIPlanSafety Safety = EHyperAIStudioUIPlanSafety::Edit;
	TArray<FHyperAIStudioUIBackendOperation> Operations;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIStudioUIManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioUIContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("ui_slate_mvvm");
	static constexpr const TCHAR* RequiredModuleType = TEXT("Editor");
	static constexpr const TCHAR* RequiredLoadingPhase = TEXT("None");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiouitoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("ui_plugin");
	static constexpr const TCHAR* BackendRequirementGroupId = TEXT("ui_backend");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.widget_blueprint_editor");
	static constexpr const TCHAR* EditVariantId = TEXT("ui.apply_edit_plan.v1");
	static constexpr const TCHAR* DestructiveVariantId = TEXT("ui.apply_destructive_plan.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.ui.compound-plan.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("staged_backend_required");
	static constexpr int32 MaxTargetPaths = 32;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxTextCharacters = 4096;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxLoadedBlueprintsScanned = 256;
	static constexpr int32 MaxRawObjectSlotsScanned = 8192;
	static constexpr int32 MaxWidgetsPerBlueprint = 512;
	static constexpr int32 MaxChildrenPerPanel = 256;
	static constexpr int32 MaxNamedSlotBindings = 128;
	static constexpr int32 MaxAnimationsPerBlueprint = 128;
	static constexpr int32 MaxAnimationBindings = 256;
	static constexpr int32 MaxMovieSceneBindings = 256;
	static constexpr int32 MaxTracksPerBinding = 128;
	static constexpr int32 MaxLegacyBindings = 256;
	static constexpr int32 MaxMVVMViewModels = 128;
	static constexpr int32 MaxMVVMBindings = 256;
	static constexpr int32 MaxMVVMPathSegments = 32;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxIssues = 256;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxCaptureCanonicalCharacters = 512 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioUIManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyAssetRegistryExistence(int32 StateValue);
	static bool DecodeCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint,
		int32& OutOffset);
	static FString EncodeCursor(
		int32 Offset,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint);
	static bool ClassifyOperation(
		const FString& Kind,
		EHyperAIStudioUIOperationKind& OutKind,
		EHyperAIStudioUIPlanSafety& OutSafety);
	static bool ValidateOperationShape(
		const FHyperAIUIPlanOperation& Operation,
		FHyperAIStudioUIBackendOperation& OutOperation,
		FString& OutError);
	static FString PayloadSchemaFingerprint();
	static FString ComputePayloadSemanticFingerprint(const FHyperAIStudioUIPlanPayload& Payload);
	static const FHyperAIStudioDomainAdapterDescriptor& GetPreparationDescriptor();
	static FHyperAIUIApplyPlanReport BuildPlan(const FHyperAIUIApplyPlanRequest& Request);
};

class FHyperAIStudioUIRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const { return bOwnsToolset; }

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
