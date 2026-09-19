// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLevelDesignToolset.generated.h"

/** What the player can do, in centimetres and seconds, and the level sizes that follow from it. */
USTRUCT(BlueprintType)
struct FHyperAIPlayerMetrics
{
	GENERATED_BODY()

	/** The pawn class measured, and whether its own defaults or engine Character defaults were used. */
	UPROPERTY() FString PawnClass;
	UPROPERTY() FString Source;
	UPROPERTY() float CapsuleRadius = 0.f;
	UPROPERTY() float PlayerHeight = 0.f;
	UPROPERTY() float CrouchedHeight = 0.f;
	/** Eye height above the floor. */
	UPROPERTY() float EyeHeight = 0.f;
	/** Tallest ledge the player walks up without jumping. */
	UPROPERTY() float MaxStepHeight = 0.f;
	UPROPERTY() float MaxWalkableSlopeDegrees = 0.f;
	UPROPERTY() float JumpHeight = 0.f;
	/** Longest horizontal gap cleared by a running jump onto the same height. */
	UPROPERTY() float MaxJumpGap = 0.f;
	UPROPERTY() float WalkSpeed = 0.f;
	UPROPERTY() float CrouchSpeed = 0.f;

	/** Recommended: doorways at least this wide, corridors two players can pass in, ceilings, cover and gaps. */
	UPROPERTY() float MinDoorWidth = 0.f;
	UPROPERTY() float MinCorridorWidth = 0.f;
	UPROPERTY() float MinCeilingHeight = 0.f;
	/** Low cover hides a crouched player and lets a standing one shoot over it. */
	UPROPERTY() float LowCoverMin = 0.f;
	UPROPERTY() float LowCoverMax = 0.f;
	/** High cover hides a standing player. */
	UPROPERTY() float HighCoverMin = 0.f;
	/** A gap a player clears without a perfect jump. */
	UPROPERTY() float SafeJumpGap = 0.f;
};

/** One blockout piece or marker this toolset placed. Location is the bottom-centre (blocks) or start (ramps, stairs). */
USTRUCT(BlueprintType)
struct FHyperAILevelDesignPiece
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** block, ramp, stairs or marker. */
	UPROPERTY() FString Shape;
	/** floor, wall, platform, pillar, cover_low, cover_high, generic; for markers start, goal, objective, spawn, cover, pickup, checkpoint. */
	UPROPERTY() FString Role;
	UPROPERTY() FString Location;
	UPROPERTY() FString Size;
	UPROPERTY() float Yaw = 0.f;
	/** Actors in the piece: one, or one per stair step. */
	UPROPERTY() int32 ActorCount = 0;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignInspectRequest
{
	GENERATED_BODY()

	/** Pawn to measure; empty uses the level's (or project's) game mode default pawn. */
	UPROPERTY() FString PawnClassPath;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	UPROPERTY() FHyperAIPlayerMetrics Metrics;
	UPROPERTY() TArray<FHyperAILevelDesignPiece> Pieces;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One closed blockout edit. Vectors are "x,y,z" in centimetres; Value is the yaw in degrees for adds and move.
 *   add_block: Role, Location (bottom-centre), Size (x,y,z)
 *   add_ramp: Location (bottom of the start), Size (length, width, rise); refused if steeper than the player can walk
 *   add_stairs: Location, Size (length, width, rise); split into steps the player can climb
 *   add_marker: Role (start, goal, objective, spawn, cover, pickup, checkpoint), Location (on the floor)
 *   move: Name, Location, optional Value (yaw); rebuilds the piece there
 *   delete: Name; only pieces this toolset placed
 */
USTRUCT(BlueprintType)
struct FHyperAILevelDesignOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	/** 1-48 letters, digits, _ or -; unique among pieces. */
	UPROPERTY() FString Name;
	UPROPERTY() FString Role;
	UPROPERTY() FString Location;
	UPROPERTY() FString Size;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAILevelBlockoutApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAILevelDesignOp> Ops;
	/** Stairs are split using this pawn's step height; empty uses the default pawn. */
	UPROPERTY() FString PawnClassPath;
	/** Saves the changed pieces. Off by default so layouts can be iterated before they are kept. */
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILevelBlockoutApplyPlanReport
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
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	/** What the plan will build, e.g. how many steps each staircase gets. */
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Message;
	UPROPERTY() FString Location;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignRoute
{
	GENERATED_BODY()

	UPROPERTY() FString From;
	UPROPERTY() FString To;
	UPROPERTY() bool bReachable = false;
	/** Walking distance along the navmesh, and the straight-line distance. */
	UPROPERTY() float PathLength = 0.f;
	UPROPERTY() float StraightDistance = 0.f;
	/** Narrowest free width found along the path. */
	UPROPERTY() float NarrowestWidth = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignSightline
{
	GENERATED_BODY()

	UPROPERTY() FString From;
	UPROPERTY() FString To;
	UPROPERTY() bool bVisible = false;
	UPROPERTY() float Distance = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString PawnClassPath;
	/** Open sightlines longer than this from a spawn are flagged. */
	UPROPERTY() float MaxSightline = 6000.f;
	/** Writes a top-down image: heights in grey, markers, routes and issues drawn on. */
	UPROPERTY() bool bCapturePreview = true;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILevelDesignValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FHyperAIPlayerMetrics Metrics;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAILevelDesignIssue> Issues;
	UPROPERTY() TArray<FHyperAILevelDesignRoute> Routes;
	UPROPERTY() TArray<FHyperAILevelDesignSightline> Sightlines;
	/** False when the level has no navmesh (routes are not checked) or it is still rebuilding. */
	UPROPERTY() bool bNavigationReady = false;
	UPROPERTY() FString PreviewImagePath;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Blockout at player scale: measure the player, place blocks, ramps, stairs and markers, then check the layout plays. */
UCLASS()
class HYPERAISTUDIOLEVELDESIGN_API UHyperAIStudioLevelDesignToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Player metrics from the pawn (height, step, jump, speeds) with recommended door, corridor, ceiling and cover sizes, plus the blockout pieces in the open level and its revision. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|LevelDesign")
	static FHyperAILevelDesignInspectReport hyper_level_design_inspect(const FHyperAILevelDesignInspectRequest& Request);

	/**
	 * Closed blockout edits as one undo step: blocks, walkable ramps, stairs split to the player's step height, and
	 * markers (start, goal, spawn, cover...). Dry-run, resubmit with operation_id and expected_plan_hash, poll
	 * hyper_operation_status, then hyper_level_design_validate. Unsaved unless bSave.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|LevelDesign")
	static FHyperAILevelBlockoutApplyPlanReport hyper_level_blockout_apply_plan(const FHyperAILevelBlockoutApplyPlanRequest& Request);

	/**
	 * Plays the layout against player metrics: markers on walkable floor with room to stand, navmesh routes from each
	 * start to every goal, corridor widths, long sightlines from spawns, cover heights. Writes a top-down image of it.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|LevelDesign")
	static FHyperAILevelDesignValidateReport hyper_level_design_validate(const FHyperAILevelDesignValidateRequest& Request);
};

class FHyperAIStudioLevelDesignInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILevelDesignInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLevelDesignValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILevelDesignValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLevelDesignEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString LevelPath;
	FString BaseRevision;
	TArray<FHyperAILevelDesignOp> Ops;
	/** Stair step height, fixed at planning so the plan builds what its dry run described. */
	float MaxStepHeight = 0.f;
	float MaxWalkableSlopeDegrees = 0.f;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioLevelDesignInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILevelDesignInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLevelDesignValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILevelDesignValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLevelDesignMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLevelDesignDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioLevelDesignFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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

class FHyperAIStudioLevelDesignContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("level_design");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioleveldesigntoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.level_design_editor_world");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_level_design_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_level_blockout_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_level_design_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("player_metrics_blockout_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("blockout_edit_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("playable_layout_checks.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.level_design.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.level_design.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.level_design.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.level_design.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.level_design.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.level_design.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 32;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAILevelDesignInspectReport Inspect(const FHyperAILevelDesignInspectRequest& Request);
	static FHyperAILevelDesignValidateReport Validate(const FHyperAILevelDesignValidateRequest& Request);
	static FHyperAILevelBlockoutApplyPlanReport BuildPlan(const FHyperAILevelBlockoutApplyPlanRequest& Request);
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioLevelDesignEditOpsPayload& Payload);
};

class FHyperAIStudioLevelDesignRegistration final
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
	TSharedPtr<FHyperAIStudioLevelDesignDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
