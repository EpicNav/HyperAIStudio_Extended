// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "Misc/AssetRegistryInterface.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioCharacterToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAICharacterIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Detail;
};

/** Frozen source/review evidence. It is never a dispatch table. */
USTRUCT(BlueprintType)
struct FHyperAICharacterAuthorityRow
{
	GENERATED_BODY()

	UPROPERTY() FString Source;
	UPROPERTY() FString SourceId;
	UPROPERTY() FString Lifecycle;
	UPROPERTY() FString Access;
	UPROPERTY() FString Disposition;
	UPROPERTY() FString EpicEquivalent;
	UPROPERTY() FString HyperAIContract;
	UPROPERTY() FString SourceCoordinate;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterCapabilityStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Family = TEXT("metahuman_character");
	UPROPERTY() FString State = TEXT("source_candidate_semantic_adapter_required");
	UPROPERTY() bool bLoadedOnly = true;
	UPROPERTY() bool bEpicGeneratorDelegated = true;
	UPROPERTY() bool bIndependentValueValidator = true;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() bool bMutationExecutionImplemented = false;
	UPROPERTY() int32 DelegatedEpicCallableCount = 9;
	UPROPERTY() TArray<FString> DelegatedEpicCallables;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString AuthorityFingerprint;
	UPROPERTY() FString Remediation;
};

/** Exact already-loaded top-level MetaHuman character identity; no generator state is reflected. */
USTRUCT(BlueprintType)
struct FHyperAICharacterAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString DiskExistence = TEXT("unknown");
	UPROPERTY() FString PackageSavedHash;
	UPROPERTY() int64 DiskSize = -1;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bWasLoadedFromDisk = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bTopLevelPrimaryObject = false;
	UPROPERTY() bool bCharacterClassMatched = false;
	UPROPERTY() bool bPersistedIdentityComplete = false;
	/** Always false in this no-hard-link base adapter. */
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterDetachedSnapshot
{
	GENERATED_BODY()

	UPROPERTY() FString RequestFingerprint;
	UPROPERTY() FString PersistedFingerprint;
	UPROPERTY() FString VolatileFingerprint;
	UPROPERTY() bool bIdentityProjectionComplete = false;
	UPROPERTY() bool bSemanticProjectionComplete = false;
	UPROPERTY() int32 TotalRecords = 0;
	UPROPERTY() TArray<FHyperAICharacterAssetRecord> Records;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterInspectRequest
{
	GENERATED_BODY()

	/** Canonical top-level /Game object paths. Empty returns delegation/capability evidence only. */
	UPROPERTY() TArray<FString> TargetPaths;
	UPROPERTY() int32 PageSize = 16;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("loaded_exact_identity_only");
	UPROPERTY() FString NextCursor;
	UPROPERTY() FHyperAICharacterCapabilityStatus Capability;
	UPROPERTY() FHyperAICharacterDetachedSnapshot Snapshot;
	UPROPERTY() TArray<FHyperAICharacterIssue> Issues;
};

/** Independently designed closed value vocabulary for an edit-session preflight. */
USTRUCT(BlueprintType)
struct FHyperAICharacterDesiredValue
{
	GENERATED_BODY()

	/** body.sex_blend, body.fat, body.muscularity, body.height_cm,
	 * skin.lightness, skin.redness, eyes.temperature, or eyes.brightness. */
	UPROPERTY() FString Key;
	UPROPERTY() double Value = 0.0;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	/** Empty for dry-run; non-dry intent must echo one canonical prior plan hash. */
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedPersistedFingerprint;
	UPROPERTY() TArray<FHyperAICharacterDesiredValue> DesiredValues;
	UPROPERTY() int32 DeadlineMs = 1000;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 DesiredValueCount = 0;
	UPROPERTY() bool bIntentValueModelValid = false;
	UPROPERTY() bool bLoadedSemanticProjectionComplete = false;
	UPROPERTY() bool bWouldBeginEpicEditSessionOnce = false;
	UPROPERTY() bool bWouldEndEpicEditSessionOnce = false;
	UPROPERTY() bool bWouldSaveOnce = false;
	UPROPERTY() bool bWouldValidateOnce = false;
	UPROPERTY() int32 PhysicalEffectCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTypedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString SafetyClass = TEXT("edit");
	UPROPERTY() FString VariantId;
	UPROPERTY() FString BasePersistedFingerprint;
	UPROPERTY() FString DesiredValueFingerprint;
	UPROPERTY() FString SemanticFingerprint;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FHyperAICharacterPlanEffects Effects;
	UPROPERTY() TArray<FHyperAICharacterIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FHyperAICharacterDetachedSnapshot Snapshot;
	UPROPERTY() int32 MaxIssues = 64;
	UPROPERTY() int32 DeadlineMs = 500;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAICharacterValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bComplete = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString ValidatorFingerprint;
	UPROPERTY() FString RecomputedRequestFingerprint;
	UPROPERTY() FString RecomputedPersistedFingerprint;
	UPROPERTY() FString RecomputedVolatileFingerprint;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAICharacterIssue> Issues;
};

/** Exactly three functions form the Character atomic cohort. */
UCLASS()
class HYPERAISTUDIOCHARACTER_API UHyperAIStudioCharacterToolset final
	: public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Character")
	static FHyperAICharacterInspectReport hyper_character_inspect(
		const FHyperAICharacterInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Character")
	static FHyperAICharacterApplyPlanReport hyper_character_apply_plan(
		const FHyperAICharacterApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Character")
	static FHyperAICharacterValidateReport hyper_character_validate(
		const FHyperAICharacterValidateRequest& Request);
};

class FHyperAIStudioCharacterInspectPayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAICharacterInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCharacterValidatePayload final
	: public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAICharacterValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCharacterPlanPayload final
	: public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BasePersistedFingerprint;
	TArray<FHyperAICharacterDesiredValue> DesiredValues;
	FString DesiredValueFingerprint;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override { return SemanticFingerprint; }
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioCharacterInspectResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAICharacterInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCharacterValidateResultPayload final
	: public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAICharacterValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioCharacterDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioCharacterDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioCharacterManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioCharacterContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("character");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudiocharactertoolset.v1");
	static constexpr const TCHAR* PluginRequirementGroupId = TEXT("character");
	static constexpr const TCHAR* ExpectedCharacterClassPath =
		TEXT("/Script/MetaHumanCharacter.MetaHumanCharacter");
	static constexpr const TCHAR* InspectVariantId = TEXT("loaded_exact_character_identity.v1");
	static constexpr const TCHAR* ApplyVariantId = TEXT("character_edit_intent_preflight.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("detached_character_identity_validation.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.character.inspect.v1");
	static constexpr const TCHAR* ApplyPayloadTypeId = TEXT("hyperai.payload.character.edit-intent.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.character.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.character.inspect.v1");
	static constexpr const TCHAR* ApplyResultTypeId = TEXT("hyperai.result.character.apply-blocked.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.character.validate.v1");
	static constexpr const TCHAR* SemanticProjectionBlocker =
		TEXT("character_semantic_projection_backend_required");
	static constexpr const TCHAR* NonDryCallableState =
		TEXT("character_mutation_backend_required");
	static constexpr const TCHAR* PythonAccessReviewId =
		TEXT("epic-ue58-python-access-2026-08-15");
	static constexpr const TCHAR* PythonAccessReviewRecordsSha256 =
		TEXT("sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10");
	static constexpr int32 MaxTargetPaths = 16;
	static constexpr int32 MaxDesiredValues = 8;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxCursorCharacters = 256;
	static constexpr int32 MaxPageSize = 16;
	static constexpr int32 MaxIssues = 64;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int32 MinOutputBytes = 12 * 1024;
	static constexpr int32 MaxDeadlineMs = 2000;
	static constexpr int32 MaxContainerAllocatedBytes = 256 * 1024;
	static constexpr int32 MaxCanonicalCharacters = 64 * 1024;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioCharacterManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static const TArray<FHyperAICharacterAuthorityRow>& GetAuthorityRows();
	static const TArray<FString>& GetEpicDelegates();
	static FHyperAICharacterCapabilityStatus GetCapabilityStatus();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool IsSafeOperationId(const FString& Value);
	static FString ClassifyAssetRegistryExistence(UE::AssetRegistry::EExists State);
	static FString ComputeRecordPersistedFingerprint(
		const FHyperAICharacterAssetRecord& Record);
	static FString ComputeRecordVolatileFingerprint(
		const FHyperAICharacterAssetRecord& Record);
	static FString ComputeRequestFingerprint(const TArray<FString>& SortedPaths);
	static FString ComputeSnapshotPersistedFingerprint(
		const TArray<FHyperAICharacterAssetRecord>& Records);
	static FString ComputeSnapshotVolatileFingerprint(
		const TArray<FHyperAICharacterAssetRecord>& Records);
	static bool ValidateDesiredValues(
		const TArray<FHyperAICharacterDesiredValue>& Values,
		FString& OutFingerprint, FString& OutError);
	static FString BuildCursor(int32 Offset, const FString& RequestFingerprint,
		const FString& PersistedFingerprint, const FString& VolatileFingerprint);
	static bool ParseCursor(const FString& Cursor, const FString& RequestFingerprint,
		const FString& PersistedFingerprint, const FString& VolatileFingerprint,
		int32& OutOffset);
	static FString InspectPayloadSchemaFingerprint();
	static FString ApplyPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ApplyResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAICharacterInspectReport Inspect(
		const FHyperAICharacterInspectRequest& Request);
	static FHyperAICharacterApplyPlanReport BuildPlan(
		const FHyperAICharacterApplyPlanRequest& Request);
	static FHyperAICharacterValidateReport Validate(
		const FHyperAICharacterValidateRequest& Request);
	static FString ComputePlanSemanticFingerprint(
		const FHyperAIStudioCharacterPlanPayload& Payload);
};

class FHyperAIStudioCharacterRegistration final
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
	TSharedPtr<FHyperAIStudioCharacterDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
