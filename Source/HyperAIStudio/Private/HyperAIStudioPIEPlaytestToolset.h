// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioTypedPlan.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPIEPlaytestToolset.generated.h"

class AActor;
class UClass;
struct FHyperAIStudioPIEExecutionBinding;

/** A run either owns a new, strictly in-process single-client PIE session or attaches read-only/effect-authorized work to one. */
UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestMode : uint8
{
	StartInProcess,
	AttachOnly
};

/** Deterministic cleanup policy. Attach-only runs can never claim ownership of an existing session. */
UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestTeardown : uint8
{
	StopOwnedSession,
	LeaveSessionRunning
};

/** Closed runtime operations. There is deliberately no console, script, raw tool, reflection-write, or file operation. */
UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestStepKind : uint8
{
	WaitForWorld,
	QueryActors,
	QueryComponents,
	QueryScalarProperty,
	TeleportActor,
	SpawnAllowlistedActor,
	DestroyAllowlistedActor,
	AIMoveTo,
	AIStop,
	BlackboardRead,
	BlackboardWrite,
	InjectEnhancedAction,
	InjectAllowlistedKey,
	CaptureScreenshotHash,
	LogMarker,
	AssertActorExists,
	AssertScalarEquals,
	PauseSession,
	ResumeSession,
	ReplayCaptureStart,
	ReplayCaptureStop,
	ReplayPlaybackCaptured,
	StopSession
};

/** Exact key allowlist; callers never provide an arbitrary FName. */
UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestKey : uint8
{
	SpaceBar,
	W,
	A,
	S,
	D,
	Up,
	Down,
	Left,
	Right,
	Enter,
	Escape,
	LeftMouseButton,
	RightMouseButton
};

UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestKeyEvent : uint8
{
	Pressed,
	Released
};

/** Typed value used only by allowlisted APIs and scalar reads; it is never passed to a raw property setter. */
UENUM(BlueprintType)
enum class EHyperAIPIEPlaytestValueType : uint8
{
	None,
	Boolean,
	Integer,
	Float,
	Name,
	String,
	Vector
};

USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestValue
{
	GENERATED_BODY()

	UPROPERTY()
	EHyperAIPIEPlaytestValueType Type = EHyperAIPIEPlaytestValueType::None;

	UPROPERTY()
	bool bBoolean = false;

	UPROPERTY()
	int32 Integer = 0;

	UPROPERTY()
	double Float = 0.0;

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FString String;

	UPROPERTY()
	FVector Vector = FVector::ZeroVector;
};

/** One bounded typed step. NormalizeRequest rejects irrelevant populated fields for every operation kind. */
USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestStep
{
	GENERATED_BODY()

	UPROPERTY()
	FString StepId;

	UPROPERTY()
	EHyperAIPIEPlaytestStepKind Kind = EHyperAIPIEPlaytestStepKind::WaitForWorld;

	/** Exact loaded UObject path. ResolveObject is used; synchronous loading is prohibited. */
	UPROPERTY()
	FString TargetObjectPath;

	/** Exact loaded InputAction path for InjectEnhancedAction only. */
	UPROPERTY()
	FString SecondaryObjectPath;

	/** Opaque server allowlist id. It is not a class path and resolves only through the trusted seam. */
	UPROPERTY()
	FString ClassId;

	/** Scalar property, blackboard key, or bounded marker depending on Kind. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	FHyperAIPIEPlaytestValue Value;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	EHyperAIPIEPlaytestKey Key = EHyperAIPIEPlaytestKey::SpaceBar;

	UPROPERTY()
	EHyperAIPIEPlaytestKeyEvent KeyEvent = EHyperAIPIEPlaytestKeyEvent::Pressed;

	UPROPERTY()
	int32 PlayerIndex = 0;

	UPROPERTY()
	int32 MaxItems = 32;

	UPROPERTY()
	int32 TimeoutMs = 5000;
};

USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	EHyperAIPIEPlaytestMode Mode = EHyperAIPIEPlaytestMode::AttachOnly;

	UPROPERTY()
	EHyperAIPIEPlaytestTeardown Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;

	/** -1 chooses the one unambiguous primary in-process world; otherwise 0..64. */
	UPROPERTY()
	int32 PieInstance = -1;

	/** Opaque exact-plan grant. A populated token is never treated as authority by itself. */
	UPROPERTY()
	FString AuthorizationToken;

	UPROPERTY()
	TArray<FHyperAIPIEPlaytestStep> Steps;

	UPROPERTY()
	int32 WholeTimeoutMs = 120000;

	UPROPERTY()
	int32 MaxEvidenceItems = 128;

	UPROPERTY()
	int32 MaxOutputBytes = 262144;
};

/** Immutable bounded evidence produced by one lifecycle/step transition. */
USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestEvidence
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	FString StepId;

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString DiagnosticCode;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString ValuePreview;

	UPROPERTY()
	int32 ItemCount = 0;

	UPROPERTY()
	int32 ElapsedMs = 0;

	UPROPERTY()
	bool bEffect = false;

	UPROPERTY()
	bool bAssertion = false;

	UPROPERTY()
	bool bPassed = false;

	UPROPERTY()
	FString ScreenshotHash;

	UPROPERTY()
	int32 ScreenshotWidth = 0;

	UPROPERTY()
	int32 ScreenshotHeight = 0;

	UPROPERTY()
	int64 LogSequenceBefore = 0;

	UPROPERTY()
	int64 LogSequenceAfter = 0;
};

/** Immediate acknowledgement. The call never waits for PIE startup, world ticks, AI movement, replay, or teardown. */
USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestRunResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.playtest-run.v1");

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bAccepted = false;

	UPROPERTY()
	bool bTerminal = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bOutcomeUnknown = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString State;

	UPROPERTY()
	FString SessionFingerprint;

	UPROPERTY()
	bool bOwnsSession = false;

	UPROPERTY()
	int32 CompletedSteps = 0;

	UPROPERTY()
	int32 TotalSteps = 0;
};

/** Paged immutable evidence plus current operation/session identity. */
USTRUCT(BlueprintType)
struct FHyperAIPIEPlaytestStatusResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.playtest-status.v1");

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bFound = false;

	UPROPERTY()
	bool bTerminal = false;

	UPROPERTY()
	bool bReplay = false;

	UPROPERTY()
	bool bOutcomeUnknown = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString State;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString SessionFingerprint;

	UPROPERTY()
	FString SessionContextHandle;

	UPROPERTY()
	FString TopologySource;

	UPROPERTY()
	int32 PieInstance = INDEX_NONE;

	UPROPERTY()
	bool bOwnsSession = false;

	UPROPERTY()
	bool bSessionActive = false;

	UPROPERTY()
	int32 CompletedSteps = 0;

	UPROPERTY()
	int32 TotalSteps = 0;

	UPROPERTY()
	FString StartedUtc;

	UPROPERTY()
	FString UpdatedUtc;

	UPROPERTY()
	FString EvidenceFingerprint;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 PageSize = 0;

	UPROPERTY()
	int32 TotalEvidenceItems = 0;

	UPROPERTY()
	int32 ReturnedEvidenceItems = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIPIEPlaytestEvidence> Evidence;
};

/** One atomic two-tool SourceCandidate cohort on the single Epic MCP endpoint. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioPIEPlaytestToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Accept one bounded typed PIE lifecycle plan and return immediately with operation_id/status. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PIE")
	static FHyperAIPIEPlaytestRunResult hyper_playtest_run(const FHyperAIPIEPlaytestRequest& Request);

	/** Poll bounded immutable step/session evidence; no UObject mutation or journal reconciliation is performed here. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PIE")
	static FHyperAIPIEPlaytestStatusResult hyper_playtest_status(
		const FString& OperationId,
		int32 PageSize = 32,
		const FString& Cursor = TEXT(""));
};

struct FHyperAIStudioPIEPlaytestManifestEntry
{
	FString Name;
	FString Toolset;
};

struct FHyperAIStudioPIENormalizedPlan
{
	FHyperAIPIEPlaytestRequest Request;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	bool bHasEffects = false;
	bool bHasDestructive = false;
};

/** Exact identity comes from the active session's frozen OriginalRequestParams and one in-process FWorldContext. */
struct FHyperAIStudioPIESessionIdentity
{
	bool bActive = false;
	bool bMultiprocess = false;
	bool bExternalDestination = false;
	FString TopologySource;
	FString ContextHandle;
	FString WorldPath;
	FString WorldPackage;
	FString SourcePackage;
	FString NetMode;
	int32 PieInstance = INDEX_NONE;
	int32 ObservableWorldCount = 0;
	FString Fingerprint;
};

enum class EHyperAIStudioPIEDriverOutcome : uint8
{
	Succeeded,
	Pending,
	FailedBeforeEffect,
	FailedAfterKnownEffect,
	OutcomeUnknown,
	UnsupportedMultiprocess
};

struct FHyperAIStudioPIEDriverResult
{
	EHyperAIStudioPIEDriverOutcome Outcome = EHyperAIStudioPIEDriverOutcome::Succeeded;
	FString Code;
	FString Diagnostic;
	FHyperAIPIEPlaytestEvidence Evidence;
	FHyperAIStudioPIESessionIdentity Session;
	/** True only after the typed driver actually queued/created the session for this run. */
	bool bOwnedSessionClaimed = false;
};

/** Optional module families never linked by the core PIE toolset. */
enum class EHyperAIStudioPIEOptionalVariant : uint8
{
	GameplayAI,
	EnhancedInput,
	Replay
};

/**
 * Typed adapter installed by a capability-gated module after its prerequisite is loaded.
 * Core never loads the module, performs reflected dispatch, or silently substitutes a backend.
 */
class IHyperAIStudioPIEOptionalVariantAdapter
{
public:
	virtual ~IHyperAIStudioPIEOptionalVariantAdapter() = default;
	virtual FHyperAIStudioPIEDriverResult Begin(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		const FHyperAIPIEPlaytestStep& Step,
		const FHyperAIStudioPIESessionIdentity& Session,
		const FHyperAIStudioPIEExecutionBinding& Binding,
		class IHyperAIStudioPIETrustedExecutionGate* TrustedGate) = 0;
	virtual FHyperAIStudioPIEDriverResult Poll(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		const FHyperAIPIEPlaytestStep& Step,
		const FHyperAIStudioPIESessionIdentity& Session) = 0;
	virtual void Teardown(const FHyperAIStudioPIENormalizedPlan& Plan) = 0;
};

/** Process-local non-AICallable adapter registry; absent variant means prerequisite_unavailable. */
class FHyperAIStudioPIEOptionalVariantRegistry final
{
public:
	static void SetAdapter(
		EHyperAIStudioPIEOptionalVariant Variant,
		TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter> Adapter);
	static void ResetAdapter(EHyperAIStudioPIEOptionalVariant Variant);
	static void ResetAll();
	static TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter> GetAdapter(
		EHyperAIStudioPIEOptionalVariant Variant);
};

/** Public-API-only game-thread seam. Tests use value fakes; production uses the UE 5.8 editor/PIE implementation. */
class IHyperAIStudioPIEPlaytestDriver
{
public:
	virtual ~IHyperAIStudioPIEPlaytestDriver() = default;
	/** Read/preparation-only start validation. It must never queue PIE or cross an external-effect boundary. */
	virtual FHyperAIStudioPIEDriverResult PreflightStart(const FHyperAIStudioPIENormalizedPlan& Plan) = 0;
	virtual FHyperAIStudioPIEDriverResult RequestStart(const FHyperAIStudioPIENormalizedPlan& Plan) = 0;
	virtual FHyperAIStudioPIEDriverResult ObserveWorld(const FHyperAIStudioPIENormalizedPlan& Plan) = 0;
	virtual FHyperAIStudioPIEDriverResult BeginStep(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		const FHyperAIPIEPlaytestStep& Step,
		const FHyperAIStudioPIEExecutionBinding& Binding,
		class IHyperAIStudioPIETrustedExecutionGate* TrustedGate) = 0;
	virtual FHyperAIStudioPIEDriverResult PollStep(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		const FHyperAIPIEPlaytestStep& Step) = 0;
	virtual FHyperAIStudioPIEDriverResult BeginTeardown(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		bool bOwnsSession) = 0;
	virtual FHyperAIStudioPIEDriverResult PollTeardown(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		bool bOwnsSession) = 0;
	virtual void ForceTeardown(const FHyperAIStudioPIENormalizedPlan& Plan, bool bOwnsSession) = 0;
};

class IHyperAIStudioPIEClock
{
public:
	virtual ~IHyperAIStudioPIEClock() = default;
	virtual int64 NowMonotonicMs() const = 0;
	virtual FString NowUtc() const = 0;
};

struct FHyperAIStudioPIEExecutionBinding
{
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString AuthorizationPlanHash;
	FString CapabilityHash;
	FString EffectFingerprint;
	FString AuthorizationToken;
	EHyperAIStudioPlanSafety Safety = EHyperAIStudioPlanSafety::Read;
};

/**
 * Typed staging seam for the already-existing authorization, journal-v3, mutation-lease, and
 * validator-receipt runtime. There is intentionally no local fallback implementation: no seam means deny.
 */
class IHyperAIStudioPIETrustedExecutionGate
{
public:
	virtual ~IHyperAIStudioPIETrustedExecutionGate() = default;
	virtual EHyperAIStudioPlanJournalBegin Begin(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		FString& OutError) = 0;
	virtual bool InspectAuthorization(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		FString& OutError) = 0;
	virtual bool ConsumeAuthorization(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		FString& OutError) = 0;
	virtual bool Transition(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		EHyperAIStudioPlanJournalTransition Transition,
		const FHyperAIStudioPlanOutcomeEvidence& Evidence,
		FString& OutError) = 0;
	virtual bool IssueTerminalEvidence(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		const FString& RuntimeEvidenceHash,
		bool bRollbackProof,
		FHyperAIStudioPlanOutcomeEvidence& OutEvidence,
		FString& OutError) = 0;
	virtual bool ApproveRuntimeTarget(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		EHyperAIPIEPlaytestStepKind Kind,
		const AActor& Actor,
		FString& OutError) = 0;
	virtual UClass* ResolveAllowlistedSpawnClass(
		const FHyperAIStudioPIEExecutionBinding& Binding,
		const FString& ClassId,
		FString& OutError) = 0;
};

enum class EHyperAIStudioPIERuntimeState : uint8
{
	Idle,
	Accepted,
	StartingSession,
	WaitingForWorld,
	ExecutingStep,
	Teardown,
	Completed,
	Failed,
	Partial,
	RolledBack,
	OutcomeUnknown,
	ReplayCompleted
};

struct FHyperAIStudioPIERuntimeSnapshot
{
	EHyperAIStudioPIERuntimeState State = EHyperAIStudioPIERuntimeState::Idle;
	FString Status;
	FString Diagnostic;
	FString StartedUtc;
	FString UpdatedUtc;
	FHyperAIStudioPIENormalizedPlan Plan;
	FHyperAIStudioPIESessionIdentity Session;
	TArray<FHyperAIPIEPlaytestEvidence> Evidence;
	int32 CompletedSteps = 0;
	bool bAccepted = false;
	bool bReplay = false;
	bool bOutcomeUnknown = false;
	bool bOwnsSession = false;
	bool bCommitStarted = false;
};

/** Serial pull state-machine: one transition/poll per Tick, no waiting and no overlapping Unreal work. */
class FHyperAIStudioPIEPlaytestRuntime final
{
public:
	bool Start(
		const FHyperAIStudioPIENormalizedPlan& Plan,
		const FString& CanonicalProjectId,
		TSharedPtr<IHyperAIStudioPIEPlaytestDriver> Driver,
		TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> TrustedGate,
		TSharedPtr<IHyperAIStudioPIEClock> Clock,
		FString& OutError);
	void Tick();
	bool RequestCancel(const FString& Reason, FString& OutError);
	void Shutdown();
	bool IsTerminal() const;
	FHyperAIStudioPIERuntimeSnapshot Snapshot() const;

private:
	bool EnsureEffectBarrier(FString& OutError);
	void HandleDriverResult(const FHyperAIStudioPIEDriverResult& Result, bool bWasEffect);
	void BeginTeardown(const FString& TerminalStatus, const FString& Diagnostic, bool bAmbiguous);
	void FinishAfterTeardown(const FHyperAIStudioPIEDriverResult& Result);
	void FinishTerminal(EHyperAIStudioPIERuntimeState TerminalState, const FString& Status, const FString& Diagnostic);
	void AppendEvidence(FHyperAIPIEPlaytestEvidence Evidence);
	FString ComputeEvidenceHash() const;
	FHyperAIStudioPIEExecutionBinding MakeBinding() const;

	FHyperAIStudioPIERuntimeSnapshot Current;
	TSharedPtr<IHyperAIStudioPIEPlaytestDriver> ActiveDriver;
	TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> ActiveTrustedGate;
	TSharedPtr<IHyperAIStudioPIEClock> ActiveClock;
	FString CanonicalProjectId;
	FString PendingTerminalStatus;
	FString PendingTerminalDiagnostic;
	int64 WholeDeadlineMs = 0;
	int64 WorldDeadlineMs = 0;
	int64 StepStartedMs = 0;
	int64 TeardownDeadlineMs = 0;
	int32 ActiveStepIndex = 0;
	bool bStepDispatched = false;
	bool bTeardownDispatched = false;
	bool bPendingAmbiguous = false;
	bool bAuthorizationConsumed = false;
};

class FHyperAIStudioPIEPlaytestContracts final
{
public:
	static constexpr int32 MaxSteps = 32;
	static constexpr int32 MaxStepIdCharacters = 64;
	static constexpr int32 MaxObjectPathCharacters = 1024;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxMarkerCharacters = 256;
	static constexpr int32 MaxValueCharacters = 512;
	static constexpr int32 MaxClassIdCharacters = 128;
	static constexpr int32 MaxAuthorizationCharacters = 256;
	static constexpr int32 MinStepTimeoutMs = 100;
	static constexpr int32 MaxStepTimeoutMs = 30000;
	static constexpr int32 MinWholeTimeoutMs = 1000;
	static constexpr int32 MaxWholeTimeoutMs = 300000;
	static constexpr int32 MaxEvidenceItems = 128;
	static constexpr int32 MaxOutputBytes = 512 * 1024;
	static constexpr int32 MaxActorScan = 4096;
	static constexpr int32 MaxActorResults = 128;
	static constexpr int32 MaxComponentScan = 1024;
	static constexpr int32 MaxComponentResults = 128;
	static constexpr int32 MaxScreenshotWidth = 1920;
	static constexpr int32 MaxScreenshotHeight = 1080;
	static constexpr int32 MaxScreenshotPixels = MaxScreenshotWidth * MaxScreenshotHeight;

	static const TArray<FHyperAIStudioPIEPlaytestManifestEntry>& GetManifest();
	static FString GetQualifiedToolsetName();
	static bool IsPendingNativeToolsTestEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool EvaluateCatalogAdmission(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		bool bAllowSourceCandidateForDev,
		FString& OutError);
	static bool NormalizeRequest(
		const FHyperAIPIEPlaytestRequest& Request,
		FHyperAIStudioPIENormalizedPlan& OutPlan,
		FString& OutErrorCode,
		FString& OutError);
	static bool IsEffectStep(EHyperAIPIEPlaytestStepKind Kind);
	static bool IsDestructiveStep(EHyperAIPIEPlaytestStepKind Kind);
	static FString StepKindToString(EHyperAIPIEPlaytestStepKind Kind);
	static FString RuntimeStateToString(EHyperAIStudioPIERuntimeState State);
	static FString ComputeEvidenceFingerprint(const TArray<FHyperAIPIEPlaytestEvidence>& Evidence);
	static FString MakeCursor(const FString& EvidenceFingerprint, int32 Offset);
	static bool ParseCursor(
		const FString& Cursor,
		const FString& EvidenceFingerprint,
		int32 TotalItems,
		int32& OutOffset,
		FString& OutError);
};

/** Process service; trusted shared-runtime binding is non-AICallable and defaults to null/deny. */
class FHyperAIStudioPIEPlaytestService final
{
public:
	static void Startup();
	static void Shutdown();
	static FHyperAIPIEPlaytestRunResult Submit(const FHyperAIPIEPlaytestRequest& Request);
	static FHyperAIPIEPlaytestStatusResult GetStatus(
		const FString& OperationId,
		int32 PageSize,
		const FString& Cursor);
	static bool Cancel(const FString& OperationId, const FString& Reason, FString& OutError);
	static void SetTrustedExecutionGate(TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> Gate);
	static void ResetTrustedExecutionGate();
};

class FHyperAIStudioPIEPlaytestRegistration final
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
