// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioSettings.h"

/** Safety is declared by the registered variant, never selected by an MCP client. */
enum class EHyperAIStudioDomainSafety : uint8
{
	Read,
	Edit,
	Destructive,
	ExternalEffect
};

enum class EHyperAIStudioDomainPrerequisiteState : uint8
{
	Available,
	Disabled,
	Missing,
	RestartRequired
};

/** Stable states exposed to lightweight Toolset wrappers and capability reporting. */
enum class EHyperAIStudioDomainState : uint8
{
	Available,
	Disabled,
	MissingPrerequisite,
	RestartRequired,
	Loading,
	Ready,
	Busy,
	OutcomeUnknown
};

enum class EHyperAIStudioDomainDispatchOutcome : uint8
{
	Succeeded,
	RejectedBeforeEffect,
	FailedBeforeEffect,
	FailedAfterKnownEffect,
	OutcomeUnknown
};

/** Closed phases used by the shared typed-artifact executor; no tool-name/script dispatch exists. */
enum class EHyperAIStudioDomainExecutionActionKind : uint8
{
	Apply,
	Compile,
	Validate,
	Save,
	VerifyFresh
};

enum class EHyperAIStudioDomainAdapterLoadResult : uint8
{
	Loaded,
	Unavailable,
	Failed
};

enum class EHyperAIStudioDomainUnregisterResult : uint8
{
	Removed,
	NotFound,
	StaleHandle,
	Busy,
	OutcomeUnknown
};

struct FHyperAIStudioDomainLimits
{
	static constexpr int32 MaxPackIdChars = 64;
	static constexpr int32 MaxToolNameChars = 128;
	static constexpr int32 MaxVariantIdChars = 96;
	static constexpr int32 MaxTypeIdChars = 128;
	static constexpr int32 MaxAdapterIdChars = 96;
	static constexpr int32 MaxVersionChars = 32;
	static constexpr int32 MaxCanonicalProjectIdChars = 128;
	static constexpr int32 MaxOperationIdChars = 128;
	static constexpr int32 MaxAuthorizationTokenChars = 512;
	static constexpr int32 MaxAuthorizationNonceChars = 128;
	static constexpr int32 MaxPrerequisites = 64;
	static constexpr int32 MaxRequestBytes = 1024 * 1024;
	static constexpr int32 MaxResultBytes = 4 * 1024 * 1024;
	static constexpr int32 MaxStatusCodeChars = 96;
	static constexpr int32 MaxDiagnosticChars = 2048;
	static constexpr int64 MaxAuthorizationLifetimeMs = 5 * 60 * 1000;
};

struct FHyperAIStudioDomainPrerequisiteObservation
{
	FString Id;
	EHyperAIStudioDomainPrerequisiteState State = EHyperAIStudioDomainPrerequisiteState::Missing;
};

/**
 * Optional v2 core authority seal. SchemaVersion zero preserves the v1 deterministic/test contract;
 * production trusted execution always uses version 2 and validates every field independently.
 */
struct FHyperAIStudioDomainTrustSeal
{
	uint32 SchemaVersion = 0;
	FString CanonicalProjectId;
	FString CatalogFingerprint;
	FString ApprovedPlanFingerprint;
	FString AdmissionMatrixFingerprint;
	FString SourceLedgerFingerprint;
	FString SourceArtifactFingerprint;
	FString AtomicCohortId;
	FString CohortSourceArtifactFingerprint;
	uint64 CatalogGeneration = 0;
	uint64 AdapterRegistryGeneration = 0;
	int64 ObservedMonotonicMs = 0;
	int64 ExpiresMonotonicMs = 0;
	/** SourceCandidate Preview route. The trusted host, never client input, controls this bit. */
	bool bPreAdmissionEvidence = false;
};

/**
 * Trusted snapshot built by core prerequisite/capability code. Tool wrappers must not deserialize
 * these fields from client JSON. Fingerprint is recomputed by the registry before use.
 */
struct FHyperAIStudioDomainPrerequisiteSnapshot
{
	FString PackId;
	bool bPackEnabled = false;
	uint64 Revision = 0;
	FString Fingerprint;
	TArray<FHyperAIStudioDomainPrerequisiteObservation> Observations;
	FHyperAIStudioDomainTrustSeal TrustSeal;
};

/** One independently admitted switch per safety class; no mutation inherits read admission. */
struct FHyperAIStudioDomainAdmissionSnapshot
{
	FString PackId;
	bool bPackAdmitted = false;
	bool bReadAdmitted = false;
	bool bEditAdmitted = false;
	bool bDestructiveAdmitted = false;
	bool bExternalEffectAdmitted = false;
	uint64 Revision = 0;
	FString Fingerprint;
	FHyperAIStudioDomainTrustSeal TrustSeal;
};

struct FHyperAIStudioDomainVariantDescriptor
{
	FString ToolName;
	FString VariantId;
	/** Closed request DTO identity; must use the hyperai.payload.* namespace. */
	FString RequestTypeId;
	FString RequestSchemaFingerprint;
	/** Closed result DTO identity; must use the disjoint hyperai.result.* namespace. */
	FString ResultTypeId;
	FString ResultSchemaFingerprint;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read;
};

/** Immutable manifest returned by a clean-room optional-domain adapter. */
struct FHyperAIStudioDomainAdapterDescriptor
{
	FString PackId;
	FString AdapterId;
	FString SemanticVersion;
	uint64 AdapterVersion = 0;
	FString ContractFingerprint;
	FString AdapterFingerprint;
	/** Empty for a normal registration; exact old fingerprint for an explicit replacement. */
	FString ReplacesAdapterFingerprint;
	/**
	 * Exact generated non-blocking requirement groups used by this atomic cohort. Blocking groups
	 * always apply. Core validates and seals these ids; they are never supplied by an MCP client.
	 */
	TArray<FString> ApplicableNonBlockingRequirementGroupIds;
	TArray<FHyperAIStudioDomainVariantDescriptor> Variants;
};

/** Exact trusted binding assembled by the concrete Toolset wrapper. */
struct FHyperAIStudioDomainBinding
{
	FString PackId;
	FString ToolName;
	FString VariantId;
	EHyperAIStudioDomainSafety ExpectedSafety = EHyperAIStudioDomainSafety::Read;
	FString CanonicalProjectId;
	FString ExpectedAdapterFingerprint;
	/** Zero is valid only before the first exact lazy load. */
	uint64 ExpectedAdapterGeneration = 0;
	/** Zero means no previously observed registry epoch is being asserted. */
	uint64 ExpectedRegistryEpoch = 0;
	FHyperAIStudioDomainPrerequisiteSnapshot Prerequisites;
	FHyperAIStudioDomainAdmissionSnapshot Admission;
};

/**
 * Concrete wrappers must parse client JSON into a closed typed DTO implementing this interface.
 * A client-supplied canonical string/blob must never be forwarded as this payload without the
 * variant's schema validation and exact type/hash binding.
 */
class HYPERAISTUDIO_API IHyperAIStudioDomainRequestPayload
{
public:
	virtual ~IHyperAIStudioDomainRequestPayload() = default;
	virtual FString GetTypeId() const = 0;
	virtual FString GetSchemaFingerprint() const = 0;
	virtual int32 GetBoundedByteSize() const = 0;
};

class HYPERAISTUDIO_API IHyperAIStudioDomainResultPayload
{
public:
	virtual ~IHyperAIStudioDomainResultPayload() = default;
	virtual FString GetTypeId() const = 0;
	virtual FString GetSchemaFingerprint() const = 0;
	virtual int32 GetBoundedByteSize() const = 0;
};

struct FHyperAIStudioDomainDispatchContext
{
	FHyperAIStudioDomainBinding Binding;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read;
	FString OperationId;
	FString PlanHash;
	FString VerifiedAuthorizationNonce;
	EHyperAIStudioDomainExecutionActionKind ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	/** Generated by the serial coordinator and accepted once by an execution lease. */
	FString ActionNonce;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
};

struct FHyperAIStudioDomainExecutionAction
{
	EHyperAIStudioDomainExecutionActionKind Kind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FString ActionNonce;
};

struct FHyperAIStudioDomainAdapterResult
{
	EHyperAIStudioDomainDispatchOutcome Outcome = EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect;
	FString StatusCode;
	FString Diagnostic;
	TSharedPtr<const IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe> Payload;
};

class HYPERAISTUDIO_API IHyperAIStudioDomainAdapter
{
public:
	virtual ~IHyperAIStudioDomainAdapter() = default;
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const = 0;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) = 0;
};

struct FHyperAIStudioDomainLoadRequest
{
	FString PackId;
	FString ExpectedAdapterFingerprint;
};

class FHyperAIStudioDomainAdapterRegistry;

/**
 * Legacy source-compatibility seam. The production registry never invokes module code through it;
 * explicit core-owned module startup followed by exact registration is the only load authority.
 */
class HYPERAISTUDIO_API IHyperAIStudioDomainAdapterLoader
{
public:
	virtual ~IHyperAIStudioDomainAdapterLoader() = default;
	virtual EHyperAIStudioDomainAdapterLoadResult LoadExact(
		FHyperAIStudioDomainAdapterRegistry& Registry,
		const FHyperAIStudioDomainLoadRequest& Request,
		FString& OutError) = 0;
};

struct FHyperAIStudioDomainAuthorizationRequest
{
	FString OpaqueToken;
	FString CanonicalProjectId;
	FString OperationId;
	FString PlanHash;
	FString PackId;
	FString ToolName;
	FString VariantId;
	FString AdapterFingerprint;
	FString AdmissionFingerprint;
	FString PrerequisiteFingerprint;
	EHyperAIStudioDomainSafety Safety = EHyperAIStudioDomainSafety::Read;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
};

struct FHyperAIStudioDomainAuthorizationReceipt
{
	FHyperAIStudioDomainAuthorizationRequest BoundRequest;
	FString Nonce;
	int64 IssuedUtcMs = 0;
	int64 ExpiresUtcMs = 0;
	bool bConsumed = false;
};

/** Server-owned verification seam. Inspection never consumes; consumption is an exact second phase. */
class HYPERAISTUDIO_API IHyperAIStudioDomainAuthorizationGate
{
public:
	virtual ~IHyperAIStudioDomainAuthorizationGate() = default;
	virtual bool Inspect(
		const FHyperAIStudioDomainAuthorizationRequest& Request,
		int64 NowUtcMs,
		FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
		FString& OutError) = 0;
	virtual bool Consume(
		const FHyperAIStudioDomainAuthorizationRequest& Request,
		int64 NowUtcMs,
		FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
		FString& OutError) = 0;
};

struct FHyperAIStudioDomainRequestEnvelope
{
	FHyperAIStudioDomainBinding Binding;
	FString OperationId;
	FString PlanHash;
	FString AuthorizationToken;
	int32 MaxResultBytes = 256 * 1024;
	TSharedPtr<const IHyperAIStudioDomainRequestPayload, ESPMode::ThreadSafe> Payload;
};

struct FHyperAIStudioDomainResolveResult
{
	EHyperAIStudioDomainState State = EHyperAIStudioDomainState::MissingPrerequisite;
	FString DiagnosticCode;
	FString AdapterFingerprint;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
};

struct FHyperAIStudioDomainDispatchResult;

struct FHyperAIStudioDomainRegistrationHandle
{
	FString PackId;
	FString AdapterId;
	FString AdapterFingerprint;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;

	bool IsValid() const
	{
		return !PackId.IsEmpty() && !AdapterId.IsEmpty()
			&& !AdapterFingerprint.IsEmpty() && AdapterGeneration != 0;
	}
};

struct FHyperAIStudioDomainRegistryState;
namespace HyperAIStudio::TypedArtifact::Private
{
	class FExecutionAuthorizationGate;
	class FArtifactDispatcher;
	class FAsyncSession;
	class FRegistryExecutionAccess;
}

/** Move-only pin that prevents adapter unregister/module unload while a call is active. */
class HYPERAISTUDIO_API FHyperAIStudioDomainAdapterLease
{
public:
	FHyperAIStudioDomainAdapterLease();
	~FHyperAIStudioDomainAdapterLease();
	FHyperAIStudioDomainAdapterLease(FHyperAIStudioDomainAdapterLease&& Other) noexcept;
	FHyperAIStudioDomainAdapterLease& operator=(FHyperAIStudioDomainAdapterLease&& Other) noexcept;

	FHyperAIStudioDomainAdapterLease(const FHyperAIStudioDomainAdapterLease&) = delete;
	FHyperAIStudioDomainAdapterLease& operator=(const FHyperAIStudioDomainAdapterLease&) = delete;

	bool IsValid() const { return Adapter.IsValid(); }
	const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const
	{
		check(Descriptor != nullptr);
		return *Descriptor;
	}
	const FString& GetAdapterId() const { return AdapterId; }
	const FString& GetAdapterFingerprint() const { return AdapterFingerprint; }
	uint64 GetAdapterGeneration() const { return AdapterGeneration; }
	uint64 GetRegistryEpoch() const { return RegistryEpoch; }
	void Reset();

private:
	friend class FHyperAIStudioDomainAdapterRegistry;
	friend class FHyperAIStudioDomainExecutionLease;
	FHyperAIStudioDomainAdapterLease(
		const TSharedRef<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe>& InState,
		const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& InAdapter,
		const FHyperAIStudioDomainAdapterDescriptor& InDescriptor,
		const FString& InPackId,
		uint64 InAdapterGeneration,
		uint64 InRegistryEpoch);

	TSharedPtr<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe> State;
	TSharedPtr<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe> Adapter;
	/** Points into the immutable, pinned adapter; avoids copying its full variant manifest per call. */
	const FHyperAIStudioDomainAdapterDescriptor* Descriptor = nullptr;
	FString PackId;
	FString AdapterId;
	FString AdapterFingerprint;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
};

/** A successful direct read wraps its optional-module payload in a core-owned lifetime pin. */
struct HYPERAISTUDIO_API FHyperAIStudioDomainDispatchResult
{
	EHyperAIStudioDomainState State = EHyperAIStudioDomainState::MissingPrerequisite;
	EHyperAIStudioDomainDispatchOutcome Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	FString StatusCode;
	FString Diagnostic;
	FString AdapterFingerprint;
	uint64 AdapterGeneration = 0;
	uint64 RegistryEpoch = 0;
	/** Deliberately invariant: this runtime never chooses or authorizes a fallback backend. */
	bool bFallbackPermitted = false;
	TSharedPtr<const IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe> Payload;
};

/**
 * Move-only, exact execution pin. Acquisition validates the typed payload but performs no effect
 * and stores no bearer token. AuthorizeExact consumes one server grant, after which bounded serial
 * phases may execute while the optional adapter/module remains pinned. Registry/generation drift
 * and outcome_unknown permanently fail closed.
 */
class HYPERAISTUDIO_API FHyperAIStudioDomainExecutionLease
{
public:
	FHyperAIStudioDomainExecutionLease();
	~FHyperAIStudioDomainExecutionLease();
	FHyperAIStudioDomainExecutionLease(FHyperAIStudioDomainExecutionLease&& Other) noexcept;
	FHyperAIStudioDomainExecutionLease& operator=(FHyperAIStudioDomainExecutionLease&& Other) noexcept;

	FHyperAIStudioDomainExecutionLease(const FHyperAIStudioDomainExecutionLease&) = delete;
	FHyperAIStudioDomainExecutionLease& operator=(const FHyperAIStudioDomainExecutionLease&) = delete;

	bool IsValid() const { return AdapterLease.IsValid() && Request.Payload.IsValid(); }
	bool IsAuthorized() const { return bAuthorized; }
	const FHyperAIStudioDomainBinding& GetBinding() const { return Request.Binding; }
	const FString& GetOperationId() const { return Request.OperationId; }
	const FString& GetPlanHash() const { return Request.PlanHash; }
	const FString& GetVerifiedAuthorizationNonce() const { return VerifiedAuthorizationNonce; }
	int64 GetAuthorizationExpiresUtcMs() const { return AuthorizationExpiresUtcMs; }
	uint64 GetAdapterGeneration() const { return AdapterLease.GetAdapterGeneration(); }
	uint64 GetRegistryEpoch() const { return AdapterLease.GetRegistryEpoch(); }

	/** Observation-only exact registry/generation/unknown-state drift check. */
	FHyperAIStudioDomainResolveResult RevalidateExact() const;
	void Reset();

private:
	friend class FHyperAIStudioDomainAdapterRegistry;
	friend class HyperAIStudio::TypedArtifact::Private::FExecutionAuthorizationGate;
	friend class HyperAIStudio::TypedArtifact::Private::FArtifactDispatcher;
	friend class HyperAIStudio::TypedArtifact::Private::FAsyncSession;
	friend class FHyperAIStudioTypedArtifactExecutorInternal;
	FHyperAIStudioDomainExecutionLease(
		const TSharedRef<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe>& InState,
		FHyperAIStudioDomainAdapterLease&& InAdapterLease,
		const FHyperAIStudioDomainRequestEnvelope& InRequest,
		const FHyperAIStudioDomainVariantDescriptor& InVariant);
	/** Core-only after journal ProceedNew -> Running; client bearer material is prohibited. */
	bool AuthorizeJournalAdmittedEdit(FString& OutError);
	/** Core-only: risky grants are consumed by the coordinator after durable journal Running. */
	bool AuthorizeExact(
		const FString& OpaqueToken,
		int64 NowUtcMs,
		FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
		FString& OutError);
	/** Core-only non-consuming server lookup used before journal admission, including exact replay. */
	bool InspectAuthorizationExact(
		const FString& OpaqueToken,
		int64 NowUtcMs,
		FHyperAIStudioDomainAuthorizationReceipt& OutReceipt,
		FString& OutError) const;
	bool BuildAuthorizationRequest(
		const FString& OpaqueToken,
		FHyperAIStudioDomainAuthorizationRequest& OutRequest,
		FString& OutError) const;
	/** Core-only execution route after exact journal/authorization admission. */
	FHyperAIStudioDomainDispatchResult DispatchAction(
		const FHyperAIStudioDomainExecutionAction& Action);
	/** Core-only ambiguous timeout/cancel latch. */
	void LatchOutcomeUnknown();

	TSharedPtr<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe> State;
	FHyperAIStudioDomainAdapterLease AdapterLease;
	FHyperAIStudioDomainRequestEnvelope Request;
	FHyperAIStudioDomainVariantDescriptor Variant;
	FString VerifiedAuthorizationNonce;
	int64 AuthorizationExpiresUtcMs = 0;
	TSet<FString> ConsumedActionNonces;
	bool bAuthorized = false;
	bool bOutcomeUnknown = false;
};

/**
 * Thread-safe exact-pack registry. It never loads module code: ResolveStatus, WarmExact and dispatch
 * all require a pre-existing exact registration from the core-owned explicit module lifecycle.
 */
class HYPERAISTUDIO_API FHyperAIStudioDomainAdapterRegistry final
{
public:
	/** InLoader is ignored fail-closed and retained only for source compatibility. */
	explicit FHyperAIStudioDomainAdapterRegistry(
		IHyperAIStudioDomainAdapterLoader* InLoader = nullptr,
		TSharedPtr<IHyperAIStudioDomainAuthorizationGate, ESPMode::ThreadSafe> InAuthorizationGate = nullptr,
		TOptional<EHyperAIStudioNativeExecutionMode> InExecutionModeOverride = {});
	~FHyperAIStudioDomainAdapterRegistry();

	FHyperAIStudioDomainAdapterRegistry(const FHyperAIStudioDomainAdapterRegistry&) = delete;
	FHyperAIStudioDomainAdapterRegistry& operator=(const FHyperAIStudioDomainAdapterRegistry&) = delete;

	bool RegisterAdapter(
		const TSharedRef<IHyperAIStudioDomainAdapter, ESPMode::ThreadSafe>& Adapter,
		FHyperAIStudioDomainRegistrationHandle& OutHandle,
		FString& OutError);

	/** Busy/OutcomeUnknown is a hard module-unload veto; the caller must leave the module loaded. */
	EHyperAIStudioDomainUnregisterResult UnregisterAdapter(
		const FHyperAIStudioDomainRegistrationHandle& Handle,
		FString& OutError);

	FHyperAIStudioDomainResolveResult ResolveStatus(const FHyperAIStudioDomainBinding& Binding) const;
	FHyperAIStudioDomainResolveResult WarmExact(const FHyperAIStudioDomainBinding& Binding);
	/** Convenience dispatch for Read variants only; every mutation must use the typed journal executor. */
	FHyperAIStudioDomainDispatchResult DispatchExact(const FHyperAIStudioDomainRequestEnvelope& Request);

	/** Lifecycle-only ready pin; it exposes no Execute route and never invokes the loader. */
	FHyperAIStudioDomainResolveResult AcquireReadyLease(
		const FHyperAIStudioDomainBinding& Binding,
		FHyperAIStudioDomainAdapterLease& OutLease);

	uint64 GetRegistryEpoch() const;

	static FString ComputePrerequisiteFingerprint(const FHyperAIStudioDomainPrerequisiteSnapshot& Snapshot);
	static FString ComputeAdmissionFingerprint(const FHyperAIStudioDomainAdmissionSnapshot& Snapshot);
	static FString ComputeContractFingerprint(const FHyperAIStudioDomainAdapterDescriptor& Descriptor);
	static FString ComputeAdapterFingerprint(const FHyperAIStudioDomainAdapterDescriptor& Descriptor);
	static const TCHAR* LexToString(EHyperAIStudioDomainState State);

private:
	friend class HyperAIStudio::TypedArtifact::Private::FRegistryExecutionAccess;
	/** Core stage-store only: validates and pins one exact typed request without consuming a grant. */
	FHyperAIStudioDomainResolveResult AcquireExecutionLease(
		const FHyperAIStudioDomainRequestEnvelope& Request,
		FHyperAIStudioDomainExecutionLease& OutLease);
	FHyperAIStudioDomainResolveResult AcquireExact(
		const FHyperAIStudioDomainBinding& Binding,
		FHyperAIStudioDomainAdapterLease& OutLease,
		bool bAllowActiveLease = false);
	void MarkOutcomeUnknown(
		const FHyperAIStudioDomainAdapterLease& Lease,
		const FString& OperationId,
		const FString& PlanHash);

	TSharedRef<FHyperAIStudioDomainRegistryState, ESPMode::ThreadSafe> State;
};
