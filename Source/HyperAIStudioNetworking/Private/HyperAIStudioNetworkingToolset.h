// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioNetworkingToolset.generated.h"

class UBlueprint;

/** One stable bounded finding from either half of this atomic pack. */
USTRUCT(BlueprintType)
struct FHyperAINetworkingIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Domain;
	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString StableId;
	UPROPERTY() int32 OperationIndex = -1;
	UPROPERTY() FString Message;
};

/** Truthful coverage/delegation statement for one closed variant. */
USTRUCT(BlueprintType)
struct FHyperAINetworkingVariantStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Domain;
	UPROPERTY() FString Variant;
	UPROPERTY() TArray<FString> RequiredModules;
	UPROPERTY() bool bModulesLoaded = false;
	UPROPERTY() bool bInspectImplemented = false;
	UPROPERTY() bool bPlanSchemaImplemented = false;
	UPROPERTY() bool bApplyBackendExecutable = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State;
	UPROPERTY() FString Remediation;
};

/** Replicated property, RPC, component, class-default, actor, world, or driver detail. */
USTRUCT(BlueprintType)
struct FHyperAINetworkElementView
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Name;
	UPROPERTY() FString TypePath;
	UPROPERTY() FString Condition;
	UPROPERTY() FString RepNotifyFunction;
	UPROPERTY() FString RpcMode;
	/** Exact Blueprint declaration surface: new_variable, function_graph, custom_event, or scs. */
	UPROPERTY() FString DeclarationSource;
	UPROPERTY() FString OwnerClassPath;
	UPROPERTY() int32 Index = -1;
	UPROPERTY() bool bReplicated = false;
	UPROPERTY() bool bReliable = false;
	UPROPERTY() bool bWithValidation = false;
	UPROPERTY() bool bAuthoritativeDeclaration = false;
	UPROPERTY() bool bSignatureValid = false;
};

/** Immutable no-load network projection from loaded objects or Asset Registry metadata. */
USTRUCT(BlueprintType)
struct FHyperAINetworkRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString StableId;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString ParentClassPath;
	UPROPERTY() FString WorldPath;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bOnDiskMetadata = false;
	UPROPERTY() bool bDetailsComplete = false;
	/** Output projection flag only; excluded from semantic revision/CAS. */
	UPROPERTY() bool bOutputDetailsTruncated = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bBlueprintCompileError = false;
	UPROPERTY() FString BlueprintCompileStatus;
	UPROPERTY() bool bGeneratedClassCurrent = false;
	UPROPERTY() bool bReplicates = false;
	UPROPERTY() bool bAlwaysRelevant = false;
	UPROPERTY() bool bOnlyRelevantToOwner = false;
	UPROPERTY() bool bUseOwnerRelevancy = false;
	UPROPERTY() FString Dormancy;
	UPROPERTY() double NetUpdateFrequency = 0.0;
	UPROPERTY() double MinNetUpdateFrequency = 0.0;
	UPROPERTY() double NetPriority = 0.0;
	UPROPERTY() bool bHasAuthority = false;
	UPROPERTY() FString LocalRole;
	UPROPERTY() FString RemoteRole;
	UPROPERTY() FString OwnerPath;
	UPROPERTY() FString NetMode;
	UPROPERTY() FString NetDriverName;
	UPROPERTY() int32 ClientConnectionCount = 0;
	UPROPERTY() bool bHasServerConnection = false;
	UPROPERTY() int32 ReplicatedPropertyCount = 0;
	UPROPERTY() int32 RpcCount = 0;
	UPROPERTY() int32 ReplicatedComponentCount = 0;
	UPROPERTY() TArray<FHyperAINetworkElementView> Elements;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkInspectRequest
{
	GENERATED_BODY()

	/** Exact /Game asset/generated-class paths or already-loaded /Script class paths. */
	UPROPERTY() TArray<FString> TargetPaths;
	/** all, class_defaults, blueprint_declarations, world_instances, net_drivers, on_disk_assets. */
	UPROPERTY() FString Scope = TEXT("all");
	/** summary or details. */
	UPROPERTY() FString Projection = TEXT("details");
	UPROPERTY() bool bIncludeOnDiskMetadata = true;
	UPROPERTY() bool bIncludeVariants = true;
	UPROPERTY() int32 PageSize = 64;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("loaded_and_asset_registry_no_load");
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ObjectsScanned = 0;
	UPROPERTY() int32 TotalRecords = 0;
	UPROPERTY() int32 ReturnedRecords = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() TArray<FHyperAINetworkRecord> Records;
	UPROPERTY() TArray<FHyperAINetworkingIssue> Issues;
	UPROPERTY() TArray<FHyperAINetworkingVariantStatus> Variants;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> TargetPaths;
	UPROPERTY() FString Scope = TEXT("all");
	UPROPERTY() bool bIncludeOnDiskMetadata = false;
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() bool bRequirePackagesClean = false;
	UPROPERTY() bool bRequirePIEMultiplayerEvidence = false;
	UPROPERTY() bool bIncludeVariants = true;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkValidateReport
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
	UPROPERTY() int32 InfoCount = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FHyperAINetworkingIssue> Issues;
	UPROPERTY() TArray<FHyperAINetworkingVariantStatus> Variants;
};

/** Closed networking operation; unused discriminant fields must retain their sentinels. */
USTRUCT(BlueprintType)
struct FHyperAINetworkPlanOperation
{
	GENERATED_BODY()

	/** class.set_replication|set_relevancy|set_update_policy|set_dormancy,
	 * component.set_replicated, property.set_replication, or rpc.configure_existing. */
	UPROPERTY() FString Type;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() FString MemberName;
	UPROPERTY() int32 Replicates = -1;
	UPROPERTY() int32 AlwaysRelevant = -1;
	UPROPERTY() int32 OnlyRelevantToOwner = -1;
	UPROPERTY() int32 UseOwnerRelevancy = -1;
	UPROPERTY() double NetUpdateFrequency = -1.0;
	UPROPERTY() double MinNetUpdateFrequency = -1.0;
	UPROPERTY() double NetPriority = -1.0;
	UPROPERTY() FString Dormancy;
	UPROPERTY() FString ReplicationCondition;
	UPROPERTY() FString RepNotifyFunction;
	UPROPERTY() FString RpcMode;
	UPROPERTY() int32 Reliable = -1;
	UPROPERTY() int32 WithValidation = -1;
};

/** Immutable Game Framework class/default/session/online observation. */
USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Role;
	UPROPERTY() FString StableId;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString ParentClassPath;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bLoaded = false;
	UPROPERTY() bool bOnDiskMetadata = false;
	UPROPERTY() bool bDetailsComplete = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bBlueprintCompileError = false;
	UPROPERTY() FString BlueprintCompileStatus;
	UPROPERTY() bool bGeneratedClassCurrent = false;
	UPROPERTY() FString WorldPath;
	UPROPERTY() FString WorldSettingsPath;
	UPROPERTY() FString PackageName;
	UPROPERTY() FString GameDefaultMap;
	UPROPERTY() FString TransitionMap;
	UPROPERTY() FString GameModeClassPath;
	UPROPERTY() FString GameStateClassPath;
	UPROPERTY() FString PlayerControllerClassPath;
	UPROPERTY() FString PlayerStateClassPath;
	UPROPERTY() FString PawnClassPath;
	UPROPERTY() FString HUDClassPath;
	UPROPERTY() FString GameSessionClassPath;
	UPROPERTY() FString SpectatorClassPath;
	UPROPERTY() FString GameInstanceClassPath;
	UPROPERTY() int32 MaxPlayers = -1;
	UPROPERTY() int32 MaxSpectators = -1;
	UPROPERTY() int32 MaxSplitscreensPerConnection = -1;
	UPROPERTY() bool bRequiresPushToTalk = false;
	UPROPERTY() FString SessionName;
	UPROPERTY() FString OnlineSubsystemName;
	UPROPERTY() bool bOnlineInterfaceObserved = false;
	UPROPERTY() bool bOnlineSubsystemLoaded = false;
	UPROPERTY() int32 PlayerControllerCount = 0;
	UPROPERTY() int32 PlayerStateCount = 0;
	UPROPERTY() int32 PawnCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> TargetPaths;
	/** all, project_settings, class_defaults, world_runtime, world_settings, session, online, on_disk_assets. */
	UPROPERTY() FString Scope = TEXT("all");
	UPROPERTY() FString Projection = TEXT("details");
	UPROPERTY() bool bIncludeOnDiskMetadata = true;
	UPROPERTY() bool bIncludeVariants = true;
	UPROPERTY() int32 PageSize = 64;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("loaded_and_asset_registry_no_load");
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ObjectsScanned = 0;
	UPROPERTY() int32 TotalRecords = 0;
	UPROPERTY() int32 ReturnedRecords = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() TArray<FHyperAIGameFrameworkRecord> Records;
	UPROPERTY() TArray<FHyperAINetworkingIssue> Issues;
	UPROPERTY() TArray<FHyperAINetworkingVariantStatus> Variants;
};

USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> TargetPaths;
	UPROPERTY() FString Scope = TEXT("all");
	UPROPERTY() bool bIncludeOnDiskMetadata = false;
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() bool bRequirePackagesClean = false;
	UPROPERTY() bool bRequirePIEWorldEvidence = false;
	UPROPERTY() bool bIncludeVariants = true;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkValidateReport
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
	UPROPERTY() int32 InfoCount = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FHyperAINetworkingIssue> Issues;
	UPROPERTY() TArray<FHyperAINetworkingVariantStatus> Variants;
};

/** Closed Framework plan; no raw config keys, INI text, class loading, or reflected property names. */
USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkPlanOperation
{
	GENERATED_BODY()

	/** framework.create_class, project.set_defaults, world.set_game_mode_override,
	 * game_mode.set_class_defaults, or game_session.set_limits. */
	UPROPERTY() FString Type;
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() FString Role;
	UPROPERTY() FString ParentClassPath;
	UPROPERTY() FString GameModeClassPath;
	UPROPERTY() FString GameStateClassPath;
	UPROPERTY() FString PlayerControllerClassPath;
	UPROPERTY() FString PlayerStateClassPath;
	UPROPERTY() FString PawnClassPath;
	UPROPERTY() FString HUDClassPath;
	UPROPERTY() FString GameSessionClassPath;
	UPROPERTY() FString SpectatorClassPath;
	UPROPERTY() FString GameInstanceClassPath;
	UPROPERTY() int32 MaxPlayers = -1;
	UPROPERTY() int32 MaxSpectators = -1;
	UPROPERTY() int32 MaxSplitscreensPerConnection = -1;
	UPROPERTY() int32 RequiresPushToTalk = -1;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkingApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	/** Required only for idempotent staging; Edit plans expose no client authorization token. */
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() int32 DeadlineMs = 2000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
	UPROPERTY() TArray<FHyperAINetworkPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIGameFrameworkApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() int32 DeadlineMs = 2000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
	UPROPERTY() TArray<FHyperAIGameFrameworkPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkingPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OperationCount = 0;
	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 ClassesCreated = 0;
	UPROPERTY() int32 ClassDefaultChanges = 0;
	UPROPERTY() int32 PropertyChanges = 0;
	UPROPERTY() int32 RpcChanges = 0;
	UPROPERTY() int32 ComponentChanges = 0;
	UPROPERTY() int32 ProjectSettingChanges = 0;
	UPROPERTY() int32 WorldSettingChanges = 0;
	UPROPERTY() int32 SessionChanges = 0;
	UPROPERTY() bool bTransactionOnce = false;
	UPROPERTY() bool bCompileOnce = false;
	UPROPERTY() bool bSaveOnce = false;
	UPROPERTY() bool bValidateOnce = false;
	UPROPERTY() bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAINetworkingApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bReplay = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Domain;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FString StageId;
	UPROPERTY() FHyperAINetworkingPlanEffects Effects;
	UPROPERTY() TArray<FHyperAINetworkingIssue> Issues;
	UPROPERTY() TArray<FHyperAINetworkingVariantStatus> Variants;
};

/** Exactly six tools form one all-or-none Networking/Game Framework cohort. */
UCLASS()
class HYPERAISTUDIONETWORKING_API UHyperAIStudioNetworkingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Networking")
	static FHyperAINetworkInspectReport hyper_network_inspect(
		const FHyperAINetworkInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Networking")
	static FHyperAINetworkingApplyPlanReport hyper_network_apply_plan(
		const FHyperAINetworkingApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|Networking")
	static FHyperAINetworkValidateReport hyper_network_validate(
		const FHyperAINetworkValidateRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameFramework")
	static FHyperAIGameFrameworkInspectReport hyper_game_framework_inspect(
		const FHyperAIGameFrameworkInspectRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameFramework")
	static FHyperAINetworkingApplyPlanReport hyper_game_framework_apply_plan(
		const FHyperAIGameFrameworkApplyPlanRequest& Request);

	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameFramework")
	static FHyperAIGameFrameworkValidateReport hyper_game_framework_validate(
		const FHyperAIGameFrameworkValidateRequest& Request);
};

enum class EHyperAIStudioNetworkingOperationKind : uint8
{
	NetworkClassSetReplication,
	NetworkClassSetRelevancy,
	NetworkClassSetUpdatePolicy,
	NetworkClassSetDormancy,
	NetworkComponentSetReplicated,
	NetworkPropertySetReplication,
	NetworkRpcConfigureExisting,
	FrameworkCreateClass,
	FrameworkProjectSetDefaults,
	FrameworkWorldSetGameModeOverride,
	FrameworkGameModeSetClassDefaults,
	FrameworkGameSessionSetLimits
};

/** Internal typed operation sealed into the shared artifact contract. */
struct FHyperAIStudioNetworkingBackendOperation
{
	EHyperAIStudioNetworkingOperationKind Kind =
		EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication;
	FString Domain;
	FString Type;
	FString TargetPath;
	FString ExpectedRevision;
	FString MemberName;
	FString Role;
	FString ParentClassPath;
	int32 Replicates = -1;
	int32 AlwaysRelevant = -1;
	int32 OnlyRelevantToOwner = -1;
	int32 UseOwnerRelevancy = -1;
	double NetUpdateFrequency = -1.0;
	double MinNetUpdateFrequency = -1.0;
	double NetPriority = -1.0;
	FString Dormancy;
	FString ReplicationCondition;
	FString RepNotifyFunction;
	FString RpcMode;
	int32 Reliable = -1;
	int32 WithValidation = -1;
	FString GameModeClassPath;
	FString GameStateClassPath;
	FString PlayerControllerClassPath;
	FString PlayerStateClassPath;
	FString PawnClassPath;
	FString HUDClassPath;
	FString GameSessionClassPath;
	FString SpectatorClassPath;
	FString GameInstanceClassPath;
	int32 MaxPlayers = -1;
	int32 MaxSpectators = -1;
	int32 MaxSplitscreensPerConnection = -1;
	int32 RequiresPushToTalk = -1;
	bool bCreatesTarget = false;
};

struct FHyperAIStudioNetworkValueSnapshot
{
	FString ScopeFingerprint;
	FString Revision;
	bool bComplete = true;
	int32 ObjectsScanned = 0;
	double WorkDeadlineSeconds = 0.0;
	TArray<FHyperAINetworkRecord> Records;
	TArray<FHyperAINetworkingIssue> CaptureIssues;
};

struct FHyperAIStudioGameFrameworkValueSnapshot
{
	FString ScopeFingerprint;
	FString Revision;
	bool bComplete = true;
	int32 ObjectsScanned = 0;
	double WorkDeadlineSeconds = 0.0;
	TArray<FHyperAIGameFrameworkRecord> Records;
	TArray<FHyperAINetworkingIssue> CaptureIssues;
};

/** Immutable, closed DTO accepted only by shared TypedArtifact Prepare/staging. */
class FHyperAIStudioNetworkingTypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString Domain;
	FString ToolName;
	TArray<FHyperAIStudioNetworkingBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioNetworkingResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Domain;
	FString Phase;
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Exact typed adapter deliberately refuses execution until an independent backend is hosted. */
class FHyperAIStudioNetworkingDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioNetworkingDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioNetworkingVariantDescriptor
{
	FString Domain;
	FString Variant;
	TArray<FString> RequiredModules;
	bool bInspectImplemented = false;
	bool bPlanSchemaImplemented = false;
	bool bApplyBackendExecutable = false;
	TArray<FString> SupportedCases;
	TArray<FString> DelegatedEpicCases;
	TArray<FString> UnsupportedCases;
};

class FHyperAIStudioNetworkingFacade final
{
public:
	static const TArray<FHyperAIStudioNetworkingVariantDescriptor>& GetVariantDescriptors();
	static TArray<FHyperAINetworkingVariantStatus> ResolveVariantStatuses(const FString& Domain);
	static bool CaptureNetwork(
		const TArray<FString>& TargetPaths,
		const FString& Scope,
		bool bIncludeDetails,
		bool bIncludeOnDisk,
		FHyperAIStudioNetworkValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic,
		int32 MaxWorkMs = 100);
	static bool CaptureGameFramework(
		const TArray<FString>& TargetPaths,
		const FString& Scope,
		bool bIncludeDetails,
		bool bIncludeOnDisk,
		FHyperAIStudioGameFrameworkValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic,
		int32 MaxWorkMs = 100);
};

struct FHyperAIStudioNetworkingStagedArtifact
{
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TSharedPtr<const FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe> Payload;
	FString CanonicalProjectId;
	FString OperationId;
	FString StageId;
	int64 ExpiresMonotonicMs = 0;
};

/** Bounded side-effect-free staging. No public claim/execute route exists in this pack. */
class FHyperAIStudioNetworkingStagingService final
{
public:
	static bool Stage(
		const FHyperAIStudioNetworkingStagedArtifact& Artifact,
		bool& bOutReplay,
		FString& OutError);
	static int32 NumStaged();
	static void Reset();
};

struct FHyperAIStudioNetworkingManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioNetworkingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("networking_game_framework");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudionetworkingtoolset.v1");
	static constexpr const TCHAR* NetworkMutationVariantId = TEXT("network_configuration.v1");
	static constexpr const TCHAR* FrameworkMutationVariantId =
		TEXT("game_framework_configuration.v1");
	static constexpr const TCHAR* PayloadTypeId =
		TEXT("hyperai.payload.networking_game_framework.plan.v1");
	static constexpr const TCHAR* ResultTypeId =
		TEXT("hyperai.result.networking_game_framework.plan.v1");
	static constexpr int32 MaxTargetPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxRecords = 2048;
	static constexpr int32 MaxElementsPerRecord = 1024;
	static constexpr int32 MaxObjectsScanned = 8192;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioNetworkingManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalTargetPath(const FString& Path, bool bAllowScriptClass = true);
	static bool IsCanonicalProjectAssetPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static bool ResolveGeneratedClassAssetPath(const FString& GeneratedClassPath, FString& OutAssetPath);
	static bool IsBlueprintCompileStatusEditable(const FString& Status);
	static bool IsCurrentGeneratedClass(const UBlueprint* Blueprint, const UClass* Class);
	static FString ClassifyLoadedFrameworkRole(const UClass* Class);
	static bool IsCaptureTraversalAllowed(
		int32 ObjectsScanned,
		int32 RecordCount,
		double NowSeconds,
		double DeadlineSeconds);
	static bool HasAuthoritativeDeclaredSelector(
		const FHyperAINetworkRecord& Record,
		const FString& KindA,
		const FString& KindB,
		const FString& Name,
		const FString& DeclarationSource,
		bool bRequireValidSignature);
	static FString PayloadSchemaFingerprint();
	static FString ResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetBaseAdapterDescriptor();
	static bool ValidateNetworkOperationShape(
		const FHyperAINetworkPlanOperation& Operation,
		FHyperAIStudioNetworkingBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError);
	static bool ValidateGameFrameworkOperationShape(
		const FHyperAIGameFrameworkPlanOperation& Operation,
		FHyperAIStudioNetworkingBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError);
	static FString ComputeNetworkSnapshotRevision(FHyperAIStudioNetworkValueSnapshot& Snapshot);
	static FString ComputeGameFrameworkSnapshotRevision(
		FHyperAIStudioGameFrameworkValueSnapshot& Snapshot);
	static TArray<FHyperAINetworkingIssue> ValidateNetworkSnapshot(
		const FHyperAIStudioNetworkValueSnapshot& Snapshot,
		bool bRequirePackagesClean,
		bool bRequirePIEMultiplayerEvidence,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static TArray<FHyperAINetworkingIssue> ValidateGameFrameworkSnapshot(
		const FHyperAIStudioGameFrameworkValueSnapshot& Snapshot,
		bool bRequirePackagesClean,
		bool bRequirePIEWorldEvidence,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static FString ComputePayloadSemanticFingerprint(
		const FString& Domain,
		const FString& ToolName,
		const TArray<FHyperAIStudioNetworkingBackendOperation>& Operations,
		const FString& BaseRevision);
	static FString ComputeStageId(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		const FString& EffectFingerprint);
	static FHyperAINetworkingApplyPlanReport BuildNetworkPlan(
		const FHyperAINetworkingApplyPlanRequest& Request);
	static FHyperAINetworkingApplyPlanReport BuildGameFrameworkPlan(
		const FHyperAIGameFrameworkApplyPlanRequest& Request);
};

class FHyperAIStudioNetworkingRegistration final
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
