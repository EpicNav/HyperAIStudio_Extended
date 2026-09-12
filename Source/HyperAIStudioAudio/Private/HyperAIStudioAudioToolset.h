// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAudioToolset.generated.h"

struct FMetasoundFrontendDocument;

USTRUCT(BlueprintType)
struct FHyperAIAudioIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Per-family capability evidence. State is available, disabled, not_loaded, or unavailable. */
USTRUCT(BlueprintType)
struct FHyperAIAudioPrerequisite
{
	GENERATED_BODY()

	UPROPERTY()
	FString Family;

	UPROPERTY()
	FString Plugin;

	UPROPERTY()
	FString Module;

	UPROPERTY()
	FString State;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString PackageName;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	int32 PrimaryCount = 0;

	UPROPERTY()
	int32 SecondaryCount = 0;

	UPROPERTY()
	int32 TertiaryCount = 0;

	UPROPERTY()
	double DurationSeconds = 0.0;

	UPROPERTY()
	double SampleRate = 0.0;

	UPROPERTY()
	int32 NumChannels = 0;

	UPROPERTY()
	TArray<FString> References;

	UPROPERTY()
	TArray<FString> Details;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioInspectRequest
{
	GENERATED_BODY()

	/** loaded_only, on_disk_index, or loaded_and_on_disk. */
	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	/** Empty selects every closed audio family. */
	UPROPERTY()
	TArray<FString> Variants;

	/** Exact /Game object paths only. */
	UPROPERTY()
	TArray<FString> ObjectPaths;

	/** Allowlist: identity, package, metrics, graph, references, details. */
	UPROPERTY()
	TArray<FString> Projection;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;

	UPROPERTY()
	int32 DeadlineMs = 100;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 SourceObjectsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioRecord> Records;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	UPROPERTY()
	TArray<FString> Variants;

	UPROPERTY()
	TArray<FString> ObjectPaths;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 DeadlineMs = 200;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioValidateReport
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
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

/** Closed typed value; exactly one value field is admitted by Type. */
USTRUCT(BlueprintType)
struct FHyperAIAudioParameterValue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** bool, int, float, string, or object. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	bool BoolValue = false;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	double FloatValue = 0.0;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FString ObjectPath;
};

/**
 * Closed audio plan operation. There is no raw DSP, sample buffer, filesystem path,
 * script, console command, arbitrary class, arbitrary MetaSound registry key, or playback command.
 */
USTRUCT(BlueprintType)
struct FHyperAIAudioPlanOperation
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString TargetPath;

	/** Required loaded-state CAS for existing targets; prohibited for creates. */
	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FString ReferencePath;

	UPROPERTY()
	TArray<FString> SourcePaths;

	UPROPERTY()
	FString MemberName;

	UPROPERTY()
	FString SecondaryName;

	UPROPERTY()
	FString DataType;

	/** Closed semantic allowlist, never an arbitrary class or registry key. */
	UPROPERTY()
	FString NodeKind;

	/** Exact closed descriptor schema version; currently 1 for add/configure_node only. */
	UPROPERTY()
	int32 NodeDescriptorVersion = 0;

	UPROPERTY()
	FGuid NodeId;

	UPROPERTY()
	FGuid VertexId;

	/** Semantic alternative to VertexId for a versioned descriptor node. */
	UPROPERTY()
	FString VertexName;

	UPROPERTY()
	FGuid FromNodeId;

	UPROPERTY()
	FGuid FromVertexId;

	/** Semantic alternative to FromVertexId for a versioned descriptor node. */
	UPROPERTY()
	FString FromPortName;

	UPROPERTY()
	FGuid ToNodeId;

	UPROPERTY()
	FGuid ToVertexId;

	/** Semantic alternative to ToVertexId for a versioned descriptor node. */
	UPROPERTY()
	FString ToPortName;

	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	double Value = 0.0;

	UPROPERTY()
	double SecondaryValue = 0.0;

	UPROPERTY()
	bool bSetEnabled = false;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Extent = FVector::ZeroVector;

	UPROPERTY()
	TArray<FHyperAIAudioParameterValue> Parameters;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	UPROPERTY()
	TArray<FHyperAIAudioPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 CreateCount = 0;

	UPROPERTY()
	int32 UpdateCount = 0;

	UPROPERTY()
	int32 RemoveCount = 0;

	UPROPERTY()
	int32 MetaSoundGraphOperationCount = 0;

	UPROPERTY()
	int32 OptionalPluginOperationCount = 0;

	UPROPERTY()
	bool bMayRemovePersistedData = false;

	UPROPERTY()
	bool bPlaybackRequested = false;

	UPROPERTY()
	bool bMicrophoneCaptureRequested = false;

	UPROPERTY()
	bool bTransactionOnce = true;

	UPROPERTY()
	bool bSaveOnce = true;

	UPROPERTY()
	bool bCompileOnce = true;

	UPROPERTY()
	bool bValidateOnce = true;

	UPROPERTY()
	bool bFreshVerifyOnce = true;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bRequiresTrustedAuthorization = true;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass = TEXT("external_effect");

	UPROPERTY()
	FString BaseRevision;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString PreparedContractFingerprint;

	UPROPERTY()
	FHyperAIAudioPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

UCLASS()
class HYPERAISTUDIOAUDIO_API UHyperAIStudioAudioToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIAudioInspectReport hyper_audio_inspect(const FHyperAIAudioInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIAudioApplyPlanReport hyper_audio_apply_plan(const FHyperAIAudioApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIAudioValidateReport hyper_audio_validate(const FHyperAIAudioValidateRequest& Request);
};

struct FHyperAIAudioBackendMetaPort
{
	FString Name;
	FName TypeName;
	FGuid VertexId;
	int32 AccessType = 0;
	bool bLiteralSettable = false;
	FString ConfigureParameter;
};

struct FHyperAIAudioBackendOperation
{
	FString Variant;
	FString TargetPath;
	FString ExpectedRevision;
	FString ReferencePath;
	TArray<FString> SourcePaths;
	FString MemberName;
	FString SecondaryName;
	FString DataType;
	FString NodeKind;
	int32 NodeDescriptorVersion = 0;
	/** Exact UE registry key and reflected class-interface seal used for authoring/replay. */
	FString MetaSoundRegistryKey;
	FString MetaSoundInterfaceFingerprint;
	FGuid NodeId;
	FGuid VertexId;
	FString VertexName;
	FGuid FromNodeId;
	FGuid FromVertexId;
	FString FromPortName;
	FGuid ToNodeId;
	FGuid ToVertexId;
	FString ToPortName;
	TArray<FHyperAIAudioBackendMetaPort> DescriptorInputs;
	TArray<FHyperAIAudioBackendMetaPort> DescriptorOutputs;
	int32 Index = -1;
	int32 Count = 0;
	double Value = 0.0;
	double SecondaryValue = 0.0;
	bool bSetEnabled = false;
	bool bEnabled = false;
	FVector Location = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	TArray<FHyperAIAudioParameterValue> Parameters;
	FString OptionalFamily;
};

class FHyperAIAudioTypedArtifactPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TypeId;
	FString SchemaFingerprint;
	FString PackId;
	FString BaseRevision;
	TArray<FString> CanonicalOperations;
	/** Final per-target typed state after ordered semantic replay. */
	TArray<FString> CanonicalShadowStates;

	virtual FString GetTypeId() const override { return TypeId; }
	virtual FString GetSchemaFingerprint() const override { return SchemaFingerprint; }
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIAudioValueSnapshot
{
	FString Scope;
	FString Revision;
	bool bComplete = true;
	int32 Scanned = 0;
	TArray<FHyperAIAudioPrerequisite> Prerequisites;
	TArray<FHyperAIAudioRecord> Records;
	TArray<FHyperAIAudioIssue> Issues;
};

struct FHyperAIAudioManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioAudioContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("audio_metasound");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioaudiotoolset.v1");
	static constexpr int32 MaxPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxVariants = 32;
	static constexpr int32 MaxProjectionFields = 8;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxParametersPerOperation = 64;
	static constexpr int32 MaxTotalParameters = 512;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxScannedObjects = 8192;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxInspectDeadlineMs = 500;
	static constexpr int32 MaxApplyDeadlineMs = 2000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIAudioManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsInspectVariant(const FString& Variant);
	static bool IsCanonicalProjectAssetPath(const FString& Path);
	static bool ValidateOperationShape(const FHyperAIAudioPlanOperation& In, FHyperAIAudioBackendOperation& Out, FString& OutError);
	static FString ComputeLoadedTargetRevision(const FString& ExactObjectPath);
	static bool Capture(const FHyperAIAudioInspectRequest& Request, FHyperAIAudioValueSnapshot& Out, FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIAudioValueSnapshot& Snapshot);
	static TArray<FHyperAIAudioIssue> ValidateSnapshot(const FHyperAIAudioValueSnapshot& Snapshot, int32 MaxIssueCount, bool& bOutTruncated);
	static FHyperAIAudioApplyPlanReport BuildPlan(const FHyperAIAudioApplyPlanRequest& Request);
#if WITH_DEV_AUTOMATION_TESTS
	static FString ComputeOnDiskEvidenceRevisionForTest(
		const FString& ObjectPath, const FString& PackageName, const FString& ClassPath,
		int64 DiskSize, const FString& SavedHash, bool bHasPackageData);
	static FString ClassifyClassEvidenceForTest(
		const FString& ExactClassPath, const TArray<FString>& AncestorClassPaths);
	static bool IsClassEvidenceCompatibleForTest(
		const FString& ParameterName, const FString& ExactClassPath,
		const TArray<FString>& AncestorClassPaths);
	static bool ValidateMetaSoundDocumentForTest(
		const FMetasoundFrontendDocument& Document, FString& OutRevision,
		bool& bOutTraversalComplete);
		static bool IsCreateExistenceStateAdmittedForTest(
			int32 ExistsState, bool bRegistryAvailable);
	static bool IsSparseContainerShapeBoundedForTest(int32 Num, int32 MaxIndex);
	static bool ValidateCoupledTupleForTest(
		const FString& ClassKind, const TMap<FString, double>& KnownValues,
		const TSet<FString>& TouchedKeys, FString& OutError);
	#endif
};

class FHyperAIStudioAudioRegistration final
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
