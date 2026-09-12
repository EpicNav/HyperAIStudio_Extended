// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDataToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIDataIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 RecordIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Closed, value-only record. Kind defines the exact meaning of every scalar. */
USTRUCT(BlueprintType)
struct FHyperAIDataRecord
{
	GENERATED_BODY()

	/** asset_state, data_asset, data_table, data_table_row, curve_table,
	 * curve_table_row, string_table, user_struct, user_struct_field,
	 * user_enum, user_enum_value, or unsupported_asset. */
	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString TypeId;

	UPROPERTY()
	FString Value;

	UPROPERTY()
	FString SecondaryValue;

	/** exists, does_not_exist, or unknown; meaningful only for asset_state. */
	UPROPERTY()
	FString PackageState;

	/** Fingerprint of persisted/authored fields only; volatile fields are excluded. */
	UPROPERTY()
	FString ElementFingerprint;

	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	int64 IntegerValue = 0;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	double SecondaryNumberValue = 0.0;

	UPROPERTY()
	bool bFlag = false;

	/** Volatile observation fields, used only by the volatile fingerprint. */
	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bWasLoaded = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDataDetachedSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	FString RequestFingerprint;

	UPROPERTY()
	FString PersistedFingerprint;

	UPROPERTY()
	FString VolatileObservationFingerprint;

	UPROPERTY()
	bool bSnapshotComplete = false;

	/** Count before paging. Detached validation requires Records.Num() == TotalRecords. */
	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	TArray<FHyperAIDataRecord> Records;
};

USTRUCT(BlueprintType)
struct FHyperAIDataInspectRequest
{
	GENERATED_BODY()

	/** Explicit canonical /Game primary-object paths; empty scope is rejected. */
	UPROPERTY()
	TArray<FString> TargetPaths;

	/** String-table entries and arbitrary DataAsset fields remain delegated when true. */
	UPROPERTY()
	bool bRequestDelegatedValues = false;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 DeadlineMs = 500;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDataInspectReport
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
	bool bTruncated = false;

	UPROPERTY()
	int32 SourceObjectsScanned = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	FHyperAIDataDetachedSnapshot Snapshot;

	UPROPERTY()
	TArray<FHyperAIDataIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIDataValidateRequest
{
	GENERATED_BODY()

	/** Full detached snapshot copied from an unpaged inspect response. */
	UPROPERTY()
	FHyperAIDataDetachedSnapshot Snapshot;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 DeadlineMs = 500;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDataValidateReport
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
	FString RecomputedPersistedFingerprint;

	UPROPERTY()
	FString RecomputedVolatileObservationFingerprint;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	TArray<FHyperAIDataIssue> Issues;
};

/** Closed struct/enum shadow operation; there is no generic property or class route. */
USTRUCT(BlueprintType)
struct FHyperAIDataPlanOperation
{
	GENERATED_BODY()

	/** user_struct.create|add_field|rename_field|remove_field or
	 * user_enum.create|add_value|rename_value|remove_value. */
	UPROPERTY()
	FString Kind;

	/** Existing field GUID or enum value name for rename/remove. */
	UPROPERTY()
	FString SubjectId;

	/** Mandatory element CAS for rename/remove. */
	UPROPERTY()
	FString ExpectedElementFingerprint;

	/** New field/value name. */
	UPROPERTY()
	FString Name;

	/** Struct add only: bool, int32, float, name, string, text, vector, or rotator. */
	UPROPERTY()
	FString ValueType;

	/** Optional enum display name; no localized file or culture operation is implied. */
	UPROPERTY()
	FString DisplayName;
};

USTRUCT(BlueprintType)
struct FHyperAIDataApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	/** Exact hash from a fresh dry-run. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString TargetPath;

	/** Whole loaded projection or proven whole-package absence CAS. */
	UPROPERTY()
	FString ExpectedPersistedFingerprint;

	UPROPERTY()
	TArray<FHyperAIDataPlanOperation> Operations;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDataPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 CreateCount = 0;

	UPROPERTY()
	int32 AddCount = 0;

	UPROPERTY()
	int32 RenameCount = 0;

	UPROPERTY()
	int32 RemoveCount = 0;

	UPROPERTY()
	bool bWouldCompileOnce = false;

	UPROPERTY()
	bool bWouldSaveOnce = false;

	UPROPERTY()
	bool bWouldValidateOnce = false;

	UPROPERTY()
	bool bWouldVerifyFreshOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDataApplyPlanReport
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
	FHyperAIDataPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIDataIssue> Issues;
};

/** Exact atomic pack. Equivalent Epic callables remain delegation evidence only. */
UCLASS()
class HYPERAISTUDIODATA_API UHyperAIStudioDataToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataInspectReport hyper_data_inspect(const FHyperAIDataInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataApplyPlanReport hyper_data_apply_plan(const FHyperAIDataApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataValidateReport hyper_data_validate(const FHyperAIDataValidateRequest& Request);
};

enum class EHyperAIStudioDataPlanSafety : uint8
{
	Edit,
	Destructive
};

enum class EHyperAIStudioDataOperationKind : uint8
{
	UserStructCreate,
	UserStructAddField,
	UserStructRenameField,
	UserStructRemoveField,
	UserEnumCreate,
	UserEnumAddValue,
	UserEnumRenameValue,
	UserEnumRemoveValue
};

struct FHyperAIStudioDataBackendOperation
{
	EHyperAIStudioDataOperationKind Kind = EHyperAIStudioDataOperationKind::UserStructCreate;
	EHyperAIStudioDataPlanSafety Safety = EHyperAIStudioDataPlanSafety::Edit;
	FString SubjectId;
	FString ExpectedElementFingerprint;
	FString Name;
	FString ValueType;
	FString DisplayName;
};

class FHyperAIStudioDataPlanPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedFingerprint;
	FString DesiredPersistedFingerprint;
	FString SemanticFingerprint;
	EHyperAIStudioDataPlanSafety Safety = EHyperAIStudioDataPlanSafety::Edit;
	TArray<FString> CanonicalOperations;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIStudioDataManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioDataContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("data_config_localization");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiodatatoolset.v1");
	static constexpr const TCHAR* RequirementGroupId = TEXT("data");
	static constexpr const TCHAR* EditVariantId = TEXT("data.struct_enum.edit-plan.v1");
	static constexpr const TCHAR* DestructiveVariantId = TEXT("data.struct_enum.destructive-plan.v1");
	static constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.data.struct-enum-plan.v1");
	static constexpr const TCHAR* NonDryCallableState = TEXT("staged_backend_required");
	static constexpr int32 MaxTargetPaths = 32;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxTextCharacters = 512;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxRecords = 512;
	static constexpr int32 MaxRowsPerTable = 256;
	static constexpr int32 MaxFieldsPerStruct = 128;
	static constexpr int32 MaxValuesPerEnum = 128;
	static constexpr int32 MaxContainerAllocatedBytes = 1024 * 1024;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxCanonicalCharacters = 512 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioDataManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString ClassifyPackageExistence(int32 StateValue);
	static bool IsCreateExistenceAdmitted(int32 StateValue, bool bLoadedPackagePresent);
	static int64 EstimateWorstCaseJsonStringBytes(const FString& Value);
	static FString ComputeElementFingerprint(const FHyperAIDataRecord& Record);
	static FString ComputePersistedFingerprint(const TArray<FHyperAIDataRecord>& Records);
	static FString ComputeVolatileFingerprint(const TArray<FHyperAIDataRecord>& Records);
	static bool DecodeCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint,
		const FString& VolatileFingerprint,
		int32& OutOffset);
	static FString EncodeCursor(
		int32 Offset,
		const FString& RequestFingerprint,
		const FString& PersistedFingerprint,
		const FString& VolatileFingerprint);
	static bool ClassifyOperation(
		const FString& Kind,
		EHyperAIStudioDataOperationKind& OutKind,
		EHyperAIStudioDataPlanSafety& OutSafety);
	static bool ValidateOperationShape(
		const FHyperAIDataPlanOperation& Operation,
		FHyperAIStudioDataBackendOperation& OutOperation,
		FString& OutError);
	/** Fast mode currently admits exactly one existing-user-struct field rename. */
	static bool IsFastReversibleEditPlan(
		const TArray<FHyperAIStudioDataBackendOperation>& Operations);
	static bool ReplayShadowForTest(
		const FString& TargetPath,
		const TArray<FHyperAIDataRecord>& BaseRecords,
		const TArray<FHyperAIStudioDataBackendOperation>& Operations,
		TArray<FHyperAIDataRecord>& OutDesired,
		FHyperAIDataPlanEffects& OutEffects,
		FString& OutError);
	static FString PayloadSchemaFingerprint();
	static FString ComputePayloadSemanticFingerprint(const FHyperAIStudioDataPlanPayload& Payload);
	static const FHyperAIStudioDomainAdapterDescriptor& GetPreparationDescriptor();
	static FHyperAIDataApplyPlanReport BuildPlan(const FHyperAIDataApplyPlanRequest& Request);
};

class FHyperAIStudioDataRegistration final
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
