// Games by Hyper 2026.

#include "HyperAIStudioTypedArtifactExecution.h"

#include "HyperAIStudioTypedArtifactExecutionInternal.h"

#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioTypedPlan.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::TypedArtifact::Private
{
	constexpr const TCHAR* OperationType = TEXT("hyperai.typed_artifact.execute");
	constexpr const TCHAR* PostconditionValidator = TEXT("hyperai.typed_artifact.postcondition");

	class FSystemClock final : public IHyperAIStudioTypedArtifactClock
	{
	public:
		virtual int64 NowMonotonicMs() const override
		{
			return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
		}
		virtual int64 NowUtcMs() const override
		{
			const FDateTime Now = FDateTime::UtcNow();
			return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
		}
	};

	TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> GetSystemClock()
	{
		static const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FSystemClock, ESPMode::ThreadSafe>();
		return Clock;
	}

	class FPlanClockAdapter final : public IHyperAIStudioPlanClock
	{
	public:
		explicit FPlanClockAdapter(IHyperAIStudioTypedArtifactClock& InClock) : Clock(InClock) {}
		virtual int64 NowMonotonicMs() const override { return Clock.NowMonotonicMs(); }
		virtual int64 NowUtcMs() const override { return Clock.NowUtcMs(); }
	private:
		IHyperAIStudioTypedArtifactClock& Clock;
	};

	bool IsSha256(const FString& Value)
	{
		if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeId(const FString& Value, const int32 MaxChars)
	{
		if (Value.IsEmpty() || Value.Len() > MaxChars)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!((Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(':')))
			{
				return false;
			}
		}
		return true;
	}

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical += TEXT("|");
	}

	void AppendUInt(FString& Canonical, const uint64 Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%llu"), Value));
	}

	void AppendInt(FString& Canonical, const int64 Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Value));
	}

	void AppendBool(FString& Canonical, const bool bValue)
	{
		AppendToken(Canonical, bValue ? TEXT("1") : TEXT("0"));
	}

	EHyperAIStudioPlanSafety ToPlanSafety(const EHyperAIStudioDomainSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIStudioDomainSafety::Edit: return EHyperAIStudioPlanSafety::Edit;
		case EHyperAIStudioDomainSafety::Destructive: return EHyperAIStudioPlanSafety::Destructive;
		case EHyperAIStudioDomainSafety::ExternalEffect: return EHyperAIStudioPlanSafety::ExternalEffect;
		default: return EHyperAIStudioPlanSafety::Read;
		}
	}

	EHyperAIStudioPlanEffectKind ToPlanEffect(const EHyperAIStudioDomainSafety Safety)
	{
		switch (Safety)
		{
		case EHyperAIStudioDomainSafety::Destructive: return EHyperAIStudioPlanEffectKind::ObjectDeleted;
		case EHyperAIStudioDomainSafety::ExternalEffect: return EHyperAIStudioPlanEffectKind::RuntimeExternalEffect;
		default: return EHyperAIStudioPlanEffectKind::ObjectUpdated;
		}
	}

	EHyperAIStudioDomainExecutionActionKind ToDomainAction(const EHyperAIStudioPlanActionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanActionKind::CompileOnce: return EHyperAIStudioDomainExecutionActionKind::Compile;
		case EHyperAIStudioPlanActionKind::ValidateOnce: return EHyperAIStudioDomainExecutionActionKind::Validate;
		case EHyperAIStudioPlanActionKind::SaveOnce: return EHyperAIStudioDomainExecutionActionKind::Save;
		case EHyperAIStudioPlanActionKind::VerifyFreshOnce: return EHyperAIStudioDomainExecutionActionKind::VerifyFresh;
		default: return EHyperAIStudioDomainExecutionActionKind::Apply;
		}
	}

	bool SamePreparedIdentity(
		const FHyperAIStudioPreparedTypedArtifact& A,
		const FHyperAIStudioPreparedTypedArtifact& B)
	{
		return A.ContractFingerprint == B.ContractFingerprint
			&& A.BindingFingerprint == B.BindingFingerprint
			&& A.CapabilityHash == B.CapabilityHash
			&& A.PlanHash == B.PlanHash
			&& A.AuthorizationPlanHash == B.AuthorizationPlanHash
			&& A.EffectFingerprint == B.EffectFingerprint;
	}

	bool MatchesSealedPayload(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const IHyperAIStudioTypedArtifactPayload& Payload)
	{
		const int32 PayloadBytes = Payload.GetBoundedByteSize();
		return Payload.GetTypeId() == Prepared.Contract.ArtifactTypeId
			&& Payload.GetSchemaFingerprint() == Prepared.Contract.ArtifactSchemaFingerprint
			&& Payload.GetSemanticFingerprint() == Prepared.Contract.ArtifactSemanticFingerprint
			&& PayloadBytes >= 0
			&& PayloadBytes <= FHyperAIStudioDomainLimits::MaxRequestBytes;
	}

	bool SameStageReceipt(
		const FHyperAIStudioTypedArtifactStageReceipt& A,
		const FHyperAIStudioTypedArtifactStageReceipt& B)
	{
		return A.StageId == B.StageId && A.CanonicalProjectId == B.CanonicalProjectId
			&& A.OperationId == B.OperationId && A.PackId == B.PackId
			&& A.ToolName == B.ToolName && A.VariantId == B.VariantId
			&& A.ArtifactTypeId == B.ArtifactTypeId
			&& A.ArtifactSchemaFingerprint == B.ArtifactSchemaFingerprint
			&& A.ArtifactSemanticFingerprint == B.ArtifactSemanticFingerprint
			&& A.AdapterFingerprint == B.AdapterFingerprint
			&& A.AdmissionFingerprint == B.AdmissionFingerprint
			&& A.PrerequisiteFingerprint == B.PrerequisiteFingerprint
			&& A.ContractFingerprint == B.ContractFingerprint && A.PlanHash == B.PlanHash
			&& A.AuthorizationPlanHash == B.AuthorizationPlanHash
			&& A.CapabilityHash == B.CapabilityHash && A.EffectFingerprint == B.EffectFingerprint
			&& A.AdapterGeneration == B.AdapterGeneration && A.RegistryEpoch == B.RegistryEpoch
			&& A.ExpiresUtcMs == B.ExpiresUtcMs;
	}

	bool ValidateStageReceiptBounds(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
		FString& OutError)
	{
		OutError.Reset();
		if (!IsSafeId(Receipt.StageId, 64)
			|| !IsSafeId(
				Receipt.CanonicalProjectId,
				FHyperAIStudioDomainLimits::MaxCanonicalProjectIdChars)
			|| !FHyperAIStudioOperationJournal::IsValidOperationId(Receipt.OperationId)
			|| !IsSafeId(Receipt.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
			|| !IsSafeId(Receipt.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars)
			|| !IsSafeId(Receipt.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
			|| !IsSafeId(Receipt.ArtifactTypeId, FHyperAIStudioDomainLimits::MaxTypeIdChars)
			|| !IsSha256(Receipt.ArtifactSchemaFingerprint)
			|| !IsSha256(Receipt.ArtifactSemanticFingerprint)
			|| !IsSha256(Receipt.AdapterFingerprint)
			|| !IsSha256(Receipt.AdmissionFingerprint)
			|| !IsSha256(Receipt.PrerequisiteFingerprint)
			|| !IsSha256(Receipt.ContractFingerprint)
			|| !IsSha256(Receipt.PlanHash)
			|| !IsSha256(Receipt.AuthorizationPlanHash)
			|| !IsSha256(Receipt.CapabilityHash)
			|| !IsSha256(Receipt.EffectFingerprint)
			|| Receipt.AdapterGeneration == 0 || Receipt.RegistryEpoch == 0
			|| Receipt.ExpiresUtcMs <= 0)
		{
			OutError = TEXT("The typed-artifact stage receipt contains an invalid or unbounded binding.");
			return false;
		}
		return true;
	}

	FString BuildStageReceiptFingerprint(
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt)
	{
		FString ReceiptError;
		if (!ValidateStageReceiptBounds(Receipt, ReceiptError))
		{
			return {};
		}
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.typed-artifact-stage-receipt.v1"));
		AppendToken(Canonical, Receipt.StageId);
		AppendToken(Canonical, Receipt.CanonicalProjectId);
		AppendToken(Canonical, Receipt.OperationId);
		AppendToken(Canonical, Receipt.PackId);
		AppendToken(Canonical, Receipt.ToolName);
		AppendToken(Canonical, Receipt.VariantId);
		AppendToken(Canonical, Receipt.ArtifactTypeId);
		AppendToken(Canonical, Receipt.ArtifactSchemaFingerprint);
		AppendToken(Canonical, Receipt.ArtifactSemanticFingerprint);
		AppendToken(Canonical, Receipt.AdapterFingerprint);
		AppendToken(Canonical, Receipt.AdmissionFingerprint);
		AppendToken(Canonical, Receipt.PrerequisiteFingerprint);
		AppendToken(Canonical, Receipt.ContractFingerprint);
		AppendToken(Canonical, Receipt.PlanHash);
		AppendToken(Canonical, Receipt.AuthorizationPlanHash);
		AppendToken(Canonical, Receipt.CapabilityHash);
		AppendToken(Canonical, Receipt.EffectFingerprint);
		AppendUInt(Canonical, Receipt.AdapterGeneration);
		AppendUInt(Canonical, Receipt.RegistryEpoch);
		AppendInt(Canonical, Receipt.ExpiresUtcMs);
		return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
	}

	FString BuildReplayCredentialFingerprint(
		const FString& AuthorizationToken,
		const FHyperAIStudioTypedArtifactStageReceipt& Receipt)
	{
		FString ReceiptError;
		if (AuthorizationToken.Len() < 16
			|| AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars
			|| !ValidateStageReceiptBounds(Receipt, ReceiptError))
		{
			return {};
		}
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.typed-artifact-replay-credential.v1"));
		AppendToken(Canonical, AuthorizationToken);
		AppendToken(Canonical, Receipt.StageId);
		AppendToken(Canonical, Receipt.CanonicalProjectId);
		AppendToken(Canonical, Receipt.OperationId);
		AppendToken(Canonical, Receipt.PackId);
		AppendToken(Canonical, Receipt.ToolName);
		AppendToken(Canonical, Receipt.VariantId);
		AppendToken(Canonical, Receipt.AdapterFingerprint);
		AppendToken(Canonical, Receipt.ContractFingerprint);
		AppendToken(Canonical, Receipt.PlanHash);
		AppendToken(Canonical, Receipt.AuthorizationPlanHash);
		AppendToken(Canonical, Receipt.CapabilityHash);
		AppendToken(Canonical, Receipt.EffectFingerprint);
		return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
	}

	bool BuildBindingFingerprint(
		const FHyperAIStudioTypedArtifactContract& Contract,
		FString& OutFingerprint,
		FString& OutError)
	{
		const FHyperAIStudioDomainBinding& Binding = Contract.Binding;
		const FString PrerequisiteFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		const FString AdmissionFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
		if (!IsSafeId(Binding.PackId, FHyperAIStudioDomainLimits::MaxPackIdChars)
			|| !IsSafeId(Binding.ToolName, FHyperAIStudioDomainLimits::MaxToolNameChars)
			|| !IsSafeId(Binding.VariantId, FHyperAIStudioDomainLimits::MaxVariantIdChars)
			|| !IsSafeId(Binding.CanonicalProjectId, FHyperAIStudioDomainLimits::MaxCanonicalProjectIdChars)
			|| !IsSha256(Binding.ExpectedAdapterFingerprint)
			|| Binding.ExpectedAdapterGeneration == 0 || Binding.ExpectedRegistryEpoch == 0
			|| PrerequisiteFingerprint != Binding.Prerequisites.Fingerprint
			|| AdmissionFingerprint != Binding.Admission.Fingerprint
			|| Binding.Prerequisites.PackId != Binding.PackId
			|| Binding.Admission.PackId != Binding.PackId)
		{
			OutError = TEXT("Typed-artifact binding is incomplete, stale, or not exactly fingerprinted.");
			return false;
		}

		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.typed-artifact-binding.v1"));
		AppendToken(Canonical, Binding.PackId);
		AppendToken(Canonical, Binding.ToolName);
		AppendToken(Canonical, Binding.VariantId);
		AppendUInt(Canonical, static_cast<uint64>(Binding.ExpectedSafety));
		AppendToken(Canonical, Binding.CanonicalProjectId);
		AppendToken(Canonical, Binding.ExpectedAdapterFingerprint);
		AppendUInt(Canonical, Binding.ExpectedAdapterGeneration);
		AppendUInt(Canonical, Binding.ExpectedRegistryEpoch);
		AppendToken(Canonical, Binding.Prerequisites.Fingerprint);
		AppendUInt(Canonical, Binding.Prerequisites.Revision);
		AppendToken(Canonical, Binding.Admission.Fingerprint);
		AppendUInt(Canonical, Binding.Admission.Revision);
		AppendToken(Canonical, Contract.ArtifactTypeId);
		AppendToken(Canonical, Contract.ArtifactSchemaFingerprint);
		AppendToken(Canonical, Contract.ArtifactSemanticFingerprint);
		AppendToken(Canonical, Contract.EffectTarget);
		AppendInt(Canonical, Contract.DeadlineMs);
		AppendInt(Canonical, Contract.MaxNativeOperations);
		AppendInt(Canonical, Contract.MaxGameThreadMs);
		AppendInt(Canonical, Contract.MaxOutputBytes);
		AppendInt(Canonical, Contract.MaxResultBytes);
		AppendInt(Canonical, Contract.StageLifetimeMs);
		AppendBool(Canonical, Contract.bCompileOnce);
		AppendBool(Canonical, Contract.bSaveOnce);
		AppendBool(Canonical, Contract.bValidateOnce);
		AppendBool(Canonical, Contract.bVerifyFreshOnce);
		OutFingerprint = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
		if (!IsSha256(OutFingerprint))
		{
			OutError = TEXT("Typed-artifact binding fingerprint could not be computed.");
			return false;
		}
		return true;
	}

	bool BuildPlan(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const FString& OperationId,
		const FString& AuthorizationToken,
		FHyperAIStudioTypedOperationRegistry& OutRegistry,
		FHyperAIStudioValidatedPlan& OutPlan,
		FString& OutError)
	{
		if (AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars)
		{
			OutError = TEXT("Typed-artifact authorization exceeds the bounded server grant envelope.");
			return false;
		}
		const FHyperAIStudioTypedArtifactContract& Contract = Prepared.Contract;
		const EHyperAIStudioPlanSafety Safety = ToPlanSafety(Contract.Binding.ExpectedSafety);
		const EHyperAIStudioPlanEffectKind EffectKind = ToPlanEffect(Contract.Binding.ExpectedSafety);
		FHyperAIStudioTypedOperationMetadata Metadata;
		Metadata.TypeId = OperationType;
		Metadata.Safety = Safety;
		Metadata.Arguments = {
			{TEXT("semantic_fingerprint"), EHyperAIStudioPlanValueType::String, true, 71},
			{TEXT("binding_fingerprint"), EHyperAIStudioPlanValueType::String, true, 71}};
		Metadata.AllowedPreconditions = {EHyperAIStudioPlanPreconditionKind::PluginAvailable};
		Metadata.RequiredPreconditions = Metadata.AllowedPreconditions;
		Metadata.AllowedEffects = {EffectKind};
		Metadata.RequiredEffects = Metadata.AllowedEffects;
		Metadata.PreconditionTargetBindings = {
			{EHyperAIStudioPlanPreconditionKind::PluginAvailable, FString(), Contract.Binding.PackId}};
		Metadata.EffectTargetBindings = {
			{EffectKind, FString(), Contract.EffectTarget}};
		Metadata.PostconditionValidatorId = PostconditionValidator;
		Metadata.EstimatedNativeOperations = 1;
		Metadata.EstimatedGameThreadMs = 1;
		Metadata.bCompileOnce = Contract.bCompileOnce;
		Metadata.bSaveOnce = Contract.bSaveOnce;
		Metadata.bValidateOnce = Contract.bValidateOnce;
		Metadata.bSupportsDryRun = true;
		Metadata.bVerifyFreshOnce = Contract.bVerifyFreshOnce;
		if (!OutRegistry.Add(Metadata, OutError))
		{
			return false;
		}

		FHyperAIStudioPlanStep Step;
		Step.StepId = TEXT("typed-artifact");
		Step.OperationType = OperationType;
		Step.Safety = Safety;
		Step.Arguments.Add(TEXT("semantic_fingerprint"),
			{EHyperAIStudioPlanValueType::String, Contract.ArtifactSemanticFingerprint});
		Step.Arguments.Add(TEXT("binding_fingerprint"),
			{EHyperAIStudioPlanValueType::String, Prepared.BindingFingerprint});
		Step.Preconditions.Add({
			EHyperAIStudioPlanPreconditionKind::PluginAvailable,
			Contract.Binding.PackId, FString(), Prepared.BindingFingerprint});
		Step.Effects.Add({
			EffectKind, Contract.EffectTarget, PostconditionValidator,
			Contract.ArtifactSemanticFingerprint});
		const int32 ReservedNativeOperations = (Contract.bCompileOnce ? 1 : 0)
			+ (Contract.bValidateOnce ? 1 : 0) + (Contract.bSaveOnce ? 1 : 0)
			+ (Contract.bVerifyFreshOnce ? 1 : 0);
		// Only contracts that ask for artifact-scale work get the measured editor-asset finalizer budgets; synchronous
		// contracts keep the default reserves, and with them their step budgets and plan hashes.
		FHyperAIStudioPlanFinalizerBudgets FinalizerBudgets;
		if (Contract.MaxGameThreadMs > FHyperAIStudioTypedArtifactLimits::MaxSynchronousGameThreadMs)
		{
			FinalizerBudgets = {
				FHyperAIStudioTypedArtifactLimits::CompileGameThreadMs,
				FHyperAIStudioTypedArtifactLimits::ValidateGameThreadMs,
				FHyperAIStudioTypedArtifactLimits::SaveGameThreadMs,
				FHyperAIStudioTypedArtifactLimits::VerifyFreshGameThreadMs};
		}
		const int32 ReservedGameThreadMs =
			(Contract.bCompileOnce ? FinalizerBudgets.CompileGameThreadMs : 0)
			+ (Contract.bValidateOnce ? FinalizerBudgets.ValidateGameThreadMs : 0)
			+ (Contract.bSaveOnce ? FinalizerBudgets.SaveGameThreadMs : 0)
			+ (Contract.bVerifyFreshOnce ? FinalizerBudgets.VerifyFreshGameThreadMs : 0);
		const int32 ReservedOutputBytes = (Contract.bCompileOnce ? 256 : 0)
			+ (Contract.bValidateOnce ? 512 : 0) + (Contract.bSaveOnce ? 256 : 0)
			+ (Contract.bVerifyFreshOnce ? 512 : 0);
		const int32 StepNativeOperations = Contract.MaxNativeOperations - ReservedNativeOperations;
		const int32 StepGameThreadMs = Contract.MaxGameThreadMs - ReservedGameThreadMs;
		const int32 StepOutputBytes = Contract.MaxOutputBytes - ReservedOutputBytes;
		if (StepNativeOperations < 1 || StepGameThreadMs < 1
			|| StepOutputBytes < Contract.MaxResultBytes)
		{
			OutError = TEXT("Typed-artifact budgets do not leave a bounded apply phase after finalizer reserves.");
			return false;
		}
		Step.Budget = {StepNativeOperations, StepGameThreadMs, StepOutputBytes};
		Step.bCompileOnce = Contract.bCompileOnce;
		Step.bSaveOnce = Contract.bSaveOnce;
		Step.bValidateOnce = Contract.bValidateOnce;
		Step.bVerifyFreshOnce = Contract.bVerifyFreshOnce;

		OutPlan = FHyperAIStudioValidatedPlan{};
		// Must match the reserves above: the scheduler gives each finalizer exactly this budget.
		OutPlan.FinalizerBudgets = FinalizerBudgets;
		OutPlan.bValidated = true;
		OutPlan.bDryRun = false;
		OutPlan.bHasMutation = true;
		OutPlan.bHasDestructive = Safety == EHyperAIStudioPlanSafety::Destructive;
		OutPlan.bHasExternalEffect = Safety == EHyperAIStudioPlanSafety::ExternalEffect;
		OutPlan.OperationId = OperationId;
		OutPlan.AuthorizationToken = AuthorizationToken;
		OutPlan.MaximumSafety = Safety;
		OutPlan.Budget = {
			Contract.DeadlineMs, 1, 1, Contract.MaxNativeOperations,
			Contract.MaxGameThreadMs, Contract.MaxOutputBytes};
		OutPlan.Steps = {MoveTemp(Step)};
		OutPlan.OrderedStepIndices = {0};
		OutPlan.CapabilityHash = OutRegistry.ComputeCapabilityHash();
		OutPlan.PlanHash = FHyperAIStudioTypedPlanValidator::ComputePlanHash(OutPlan);
		OutPlan.ExpectedPlanHash = OutPlan.PlanHash;
		OutPlan.AuthorizationPlanHash = FHyperAIStudioTypedPlanValidator::ComputeAuthorizationPlanHash(OutPlan);
		OutPlan.EffectFingerprint = FHyperAIStudioTypedPlanValidator::ComputeEffectFingerprint(OutPlan);
		if (!IsSha256(OutPlan.CapabilityHash) || !IsSha256(OutPlan.PlanHash)
			|| !IsSha256(OutPlan.AuthorizationPlanHash) || !IsSha256(OutPlan.EffectFingerprint))
		{
			OutError = TEXT("Typed-artifact plan hashes could not be sealed.");
			return false;
		}
		return true;
	}

	bool ExactSnapshots(
		const FHyperAIStudioPreparedTypedArtifact& Prepared,
		const FHyperAIStudioDomainPrerequisiteSnapshot& Prerequisites,
		const FHyperAIStudioDomainAdmissionSnapshot& Admission)
	{
		const FHyperAIStudioDomainBinding& Expected = Prepared.Contract.Binding;
		return Prerequisites.PackId == Expected.PackId
			&& Admission.PackId == Expected.PackId
			&& Prerequisites.Revision == Expected.Prerequisites.Revision
			&& Admission.Revision == Expected.Admission.Revision
			&& FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Prerequisites)
				== Expected.Prerequisites.Fingerprint
			&& FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Admission)
				== Expected.Admission.Fingerprint;
	}
}

struct FHyperAIStudioTypedArtifactClaimState
{
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioDomainExecutionLease ExecutionLease;
	TSharedPtr<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload;
	TSharedPtr<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock;
	int64 ExpiresMonotonicMs = 0;
};

struct FHyperAIStudioTypedArtifactStoreState
{
	struct FEntry
	{
		FHyperAIStudioPreparedTypedArtifact Prepared;
		FHyperAIStudioTypedArtifactStageReceipt Receipt;
		FHyperAIStudioDomainExecutionLease ExecutionLease;
		TSharedPtr<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload;
		int64 ExpiresMonotonicMs = 0;
	};

	explicit FHyperAIStudioTypedArtifactStoreState(
		FHyperAIStudioDomainAdapterRegistry& InRegistry,
		const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& InClock)
		: Registry(InRegistry), Clock(InClock)
	{
	}

	void PurgeExpiredLocked(const int64 NowMonotonicMs, TArray<FEntry>& OutExpired)
	{
		TArray<FString> ExpiredStageIds;
		for (const TPair<FString, FEntry>& Pair : Entries)
		{
			if (Pair.Value.ExpiresMonotonicMs <= NowMonotonicMs)
			{
				ExpiredStageIds.Add(Pair.Key);
			}
		}
		for (const FString& StageId : ExpiredStageIds)
		{
			FEntry Expired;
			if (Entries.RemoveAndCopyValue(StageId, Expired))
			{
				OutExpired.Add(MoveTemp(Expired));
			}
		}
	}

	mutable FCriticalSection Mutex;
	FHyperAIStudioDomainAdapterRegistry& Registry;
	TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe> Clock;
	TMap<FString, FEntry> Entries;
	/** Non-waiting per project/operation reservations; callers retry after stage_in_progress_retry. */
	TSet<FString> InFlightStageKeys;
};

FHyperAIStudioTypedArtifactClaim::FHyperAIStudioTypedArtifactClaim() = default;
FHyperAIStudioTypedArtifactClaim::~FHyperAIStudioTypedArtifactClaim() { Reset(); }
FHyperAIStudioTypedArtifactClaim::FHyperAIStudioTypedArtifactClaim(
	FHyperAIStudioTypedArtifactClaim&& Other) noexcept : State(MoveTemp(Other.State)) {}
FHyperAIStudioTypedArtifactClaim& FHyperAIStudioTypedArtifactClaim::operator=(
	FHyperAIStudioTypedArtifactClaim&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		State = MoveTemp(Other.State);
	}
	return *this;
}
bool FHyperAIStudioTypedArtifactClaim::IsValid() const
{
	return State.IsValid() && State->Payload.IsValid() && State->ExecutionLease.IsValid()
		&& State->Clock.IsValid();
}
void FHyperAIStudioTypedArtifactClaim::Reset() { State.Reset(); }

FHyperAIStudioTypedArtifactStore::FHyperAIStudioTypedArtifactStore(
	FHyperAIStudioDomainAdapterRegistry& Registry)
	: State(MakeUnique<FHyperAIStudioTypedArtifactStoreState>(
		Registry, HyperAIStudio::TypedArtifact::Private::GetSystemClock()))

{
}

FHyperAIStudioTypedArtifactStore::FHyperAIStudioTypedArtifactStore(
	FHyperAIStudioDomainAdapterRegistry& Registry,
	const TSharedRef<IHyperAIStudioTypedArtifactClock, ESPMode::ThreadSafe>& Clock)
	: State(MakeUnique<FHyperAIStudioTypedArtifactStoreState>(Registry, Clock))
{
}

FHyperAIStudioTypedArtifactStore::~FHyperAIStudioTypedArtifactStore() { Reset(); }

bool FHyperAIStudioTypedArtifactStore::StageExact(
	const FHyperAIStudioPreparedTypedArtifact& Prepared,
	const FString& OperationId,
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
	FHyperAIStudioTypedArtifactStageReceipt& OutReceipt,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifact::Private;
	OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
	OutError.Reset();
	FHyperAIStudioPreparedTypedArtifact Rebuilt;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Prepared.Contract, Rebuilt, OutError)
		|| !SamePreparedIdentity(Prepared, Rebuilt))
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Prepared typed-artifact hashes do not match the closed contract."); }
		return false;
	}
	if (!FHyperAIStudioOperationJournal::IsValidOperationId(OperationId)
		|| !MatchesSealedPayload(Prepared, Payload.Get()))
	{
		OutError = TEXT("Concrete typed-artifact payload or operation identity does not match the sealed contract.");
		return false;
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> ImmutablePayload =
		Payload->CloneImmutable();
	if (&ImmutablePayload.Get() == &Payload.Get()
		|| !MatchesSealedPayload(Prepared, ImmutablePayload.Get()))
	{
		OutError = TEXT("Typed-artifact payload did not produce an exact detached immutable snapshot.");
		return false;
	}
	const FString StageOperationKey = Prepared.Contract.Binding.CanonicalProjectId
		+ TEXT("|") + OperationId;
	bool bOwnsStageReservation = false;

	const int64 InitialNowMonotonicMs = State->Clock->NowMonotonicMs();
	{
		TArray<FHyperAIStudioTypedArtifactStoreState::FEntry> Expired;
		FScopeLock Lock(&State->Mutex);
		State->PurgeExpiredLocked(InitialNowMonotonicMs, Expired);
		for (const TPair<FString, FHyperAIStudioTypedArtifactStoreState::FEntry>& Pair : State->Entries)
		{
			if (Pair.Value.Receipt.CanonicalProjectId == Prepared.Contract.Binding.CanonicalProjectId
				&& Pair.Value.Receipt.OperationId == OperationId)
			{
				// The detached payload was sealed before insertion. Never invoke pack code under this lock.
				if (SamePreparedIdentity(Pair.Value.Prepared, Prepared)
					&& Pair.Value.Payload.IsValid())
				{
					OutReceipt = Pair.Value.Receipt;
					return true;
				}
				OutError = TEXT("A conflicting unclaimed artifact already owns this project/operation identity.");
				return false;
			}
		}
		if (State->InFlightStageKeys.Contains(StageOperationKey))
		{
			OutError = TEXT("stage_in_progress_retry");
			return false;
		}
		if (State->Entries.Num() + State->InFlightStageKeys.Num()
			>= FHyperAIStudioTypedArtifactLimits::MaxStagedArtifacts)
		{
			OutError = TEXT("The bounded typed-artifact stage store is full.");
			return false;
		}
		State->InFlightStageKeys.Add(StageOperationKey);
		bOwnsStageReservation = true;
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsStageReservation)
		{
			FScopeLock Lock(&State->Mutex);
			State->InFlightStageKeys.Remove(StageOperationKey);
		}
	};

	FHyperAIStudioDomainRequestEnvelope Envelope;
	Envelope.Binding = Prepared.Contract.Binding;
	Envelope.OperationId = OperationId;
	Envelope.PlanHash = Prepared.PlanHash;
	Envelope.MaxResultBytes = Prepared.Contract.MaxResultBytes;
	Envelope.Payload = ImmutablePayload;
	FHyperAIStudioDomainExecutionLease ExecutionLease;
	const FHyperAIStudioDomainResolveResult Resolution =
		HyperAIStudio::TypedArtifact::Private::FRegistryExecutionAccess::Acquire(
			State->Registry, Envelope, ExecutionLease);
	if (Resolution.State != EHyperAIStudioDomainState::Ready || !ExecutionLease.IsValid())
	{
		OutError = TEXT("Exact typed adapter could not be pinned for staging: ") + Resolution.DiagnosticCode;
		return false;
	}

	const int64 NowUtcMs = State->Clock->NowUtcMs();
	const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
	OutReceipt.StageId = TEXT("stage-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	OutReceipt.CanonicalProjectId = Prepared.Contract.Binding.CanonicalProjectId;
	OutReceipt.OperationId = OperationId;
	OutReceipt.PackId = Prepared.Contract.Binding.PackId;
	OutReceipt.ToolName = Prepared.Contract.Binding.ToolName;
	OutReceipt.VariantId = Prepared.Contract.Binding.VariantId;
	OutReceipt.ArtifactTypeId = Prepared.Contract.ArtifactTypeId;
	OutReceipt.ArtifactSchemaFingerprint = Prepared.Contract.ArtifactSchemaFingerprint;
	OutReceipt.ArtifactSemanticFingerprint = Prepared.Contract.ArtifactSemanticFingerprint;
	OutReceipt.AdapterFingerprint = Prepared.Contract.Binding.ExpectedAdapterFingerprint;
	OutReceipt.AdmissionFingerprint = Prepared.Contract.Binding.Admission.Fingerprint;
	OutReceipt.PrerequisiteFingerprint = Prepared.Contract.Binding.Prerequisites.Fingerprint;
	OutReceipt.ContractFingerprint = Prepared.ContractFingerprint;
	OutReceipt.PlanHash = Prepared.PlanHash;
	OutReceipt.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	OutReceipt.CapabilityHash = Prepared.CapabilityHash;
	OutReceipt.EffectFingerprint = Prepared.EffectFingerprint;
	OutReceipt.AdapterGeneration = ExecutionLease.GetAdapterGeneration();
	OutReceipt.RegistryEpoch = ExecutionLease.GetRegistryEpoch();
	OutReceipt.ExpiresUtcMs = NowUtcMs > TNumericLimits<int64>::Max() - Prepared.Contract.StageLifetimeMs
		? TNumericLimits<int64>::Max()
		: NowUtcMs + Prepared.Contract.StageLifetimeMs;

	FHyperAIStudioTypedArtifactStoreState::FEntry Entry;
	Entry.Prepared = Prepared;
	Entry.Receipt = OutReceipt;
	Entry.ExecutionLease = MoveTemp(ExecutionLease);
	Entry.Payload = ImmutablePayload;
	Entry.ExpiresMonotonicMs = NowMonotonicMs > TNumericLimits<int64>::Max() - Prepared.Contract.StageLifetimeMs
		? TNumericLimits<int64>::Max()
		: NowMonotonicMs + Prepared.Contract.StageLifetimeMs;
	{
		TArray<FHyperAIStudioTypedArtifactStoreState::FEntry> Expired;
		FScopeLock Lock(&State->Mutex);
		State->PurgeExpiredLocked(NowMonotonicMs, Expired);
		for (const TPair<FString, FHyperAIStudioTypedArtifactStoreState::FEntry>& Pair : State->Entries)
		{
			if (Pair.Value.Receipt.CanonicalProjectId == OutReceipt.CanonicalProjectId
				&& Pair.Value.Receipt.OperationId == OutReceipt.OperationId)
			{
				// Compare only core-sealed values while holding the store mutex.
				if (SamePreparedIdentity(Pair.Value.Prepared, Prepared)
					&& Pair.Value.Payload.IsValid())
				{
					OutReceipt = Pair.Value.Receipt;
					return true;
				}
				OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
				OutError = TEXT("A conflicting project/operation artifact was staged concurrently.");
				return false;
			}
		}
		if (State->Entries.Num() >= FHyperAIStudioTypedArtifactLimits::MaxStagedArtifacts
			|| State->Entries.Contains(OutReceipt.StageId))
		{
			OutReceipt = FHyperAIStudioTypedArtifactStageReceipt{};
			OutError = TEXT("The bounded stage store changed before the exact artifact could be committed.");
			return false;
		}
		State->Entries.Add(Entry.Receipt.StageId, MoveTemp(Entry));
	}
	return true;
}

bool FHyperAIStudioTypedArtifactStore::ClaimExact(
	const FHyperAIStudioTypedArtifactStageReceipt& Receipt,
	FHyperAIStudioTypedArtifactClaim& OutClaim,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifact::Private;
	OutClaim.Reset();
	OutError.Reset();
	if (!ValidateStageReceiptBounds(Receipt, OutError))
	{
		return false;
	}
	FHyperAIStudioTypedArtifactStoreState::FEntry Entry;
	TArray<FHyperAIStudioTypedArtifactStoreState::FEntry> Expired;
	const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
	{
		FScopeLock Lock(&State->Mutex);
		State->PurgeExpiredLocked(NowMonotonicMs, Expired);
		const FHyperAIStudioTypedArtifactStoreState::FEntry* Existing = State->Entries.Find(Receipt.StageId);
		if (!Existing || !SameStageReceipt(Existing->Receipt, Receipt))
		{
			OutError = TEXT("No exact unexpired staged artifact matches every claim binding.");
			return false;
		}
		if (!State->Entries.RemoveAndCopyValue(Receipt.StageId, Entry))
		{
			OutError = TEXT("The staged artifact was claimed concurrently.");
			return false;
		}
	}
	OutClaim.State = MakeUnique<FHyperAIStudioTypedArtifactClaimState>();
	OutClaim.State->Prepared = MoveTemp(Entry.Prepared);
	OutClaim.State->Receipt = MoveTemp(Entry.Receipt);
	OutClaim.State->ExecutionLease = MoveTemp(Entry.ExecutionLease);
	OutClaim.State->Payload = MoveTemp(Entry.Payload);
	OutClaim.State->Clock = State->Clock;
	OutClaim.State->ExpiresMonotonicMs = Entry.ExpiresMonotonicMs;
	return OutClaim.IsValid();
}

int32 FHyperAIStudioTypedArtifactStore::NumStaged() const
{
	TArray<FHyperAIStudioTypedArtifactStoreState::FEntry> Expired;
	const int64 NowMonotonicMs = State->Clock->NowMonotonicMs();
	FScopeLock Lock(&State->Mutex);
	State->PurgeExpiredLocked(NowMonotonicMs, Expired);
	return State->Entries.Num();
}

void FHyperAIStudioTypedArtifactStore::Reset()
{
	if (State.IsValid())
	{
		TMap<FString, FHyperAIStudioTypedArtifactStoreState::FEntry> ReleasedEntries;
		{
			FScopeLock Lock(&State->Mutex);
			ReleasedEntries = MoveTemp(State->Entries);
		}
	}
}

bool FHyperAIStudioTypedArtifactExecutor::Prepare(
	const FHyperAIStudioTypedArtifactContract& Contract,
	FHyperAIStudioPreparedTypedArtifact& OutPrepared,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifact::Private;
	OutPrepared = FHyperAIStudioPreparedTypedArtifact{};
	OutError.Reset();
	if (Contract.Binding.ExpectedSafety == EHyperAIStudioDomainSafety::Read
		|| static_cast<uint8>(Contract.Binding.ExpectedSafety)
			> static_cast<uint8>(EHyperAIStudioDomainSafety::ExternalEffect)
		|| !IsSafeId(Contract.ArtifactTypeId, FHyperAIStudioDomainLimits::MaxTypeIdChars)
		|| !Contract.ArtifactTypeId.StartsWith(TEXT("hyperai.payload."), ESearchCase::CaseSensitive)
		|| !IsSha256(Contract.ArtifactSchemaFingerprint)
		|| !IsSha256(Contract.ArtifactSemanticFingerprint)
		|| Contract.EffectTarget.IsEmpty()
		|| Contract.EffectTarget.Len() > FHyperAIStudioTypedArtifactLimits::MaxEffectTargetChars
		|| Contract.DeadlineMs < 100
		|| Contract.DeadlineMs > FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs
		|| Contract.MaxNativeOperations < 5 || Contract.MaxNativeOperations > FHyperAIStudioPlanLimits::MaxNativeOperations
		|| Contract.MaxGameThreadMs < 50
		|| Contract.MaxGameThreadMs > FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs
		|| Contract.MaxOutputBytes < 2048 || Contract.MaxOutputBytes > FHyperAIStudioPlanLimits::MaxOutputBytes
		|| Contract.MaxResultBytes < 128 || Contract.MaxResultBytes > 256
		|| Contract.StageLifetimeMs < 100 || Contract.StageLifetimeMs > FHyperAIStudioTypedArtifactLimits::MaxStageLifetimeMs
		|| !Contract.bValidateOnce || !Contract.bVerifyFreshOnce
		|| (Contract.bCompileOnce && !Contract.bSaveOnce))
	{
		OutError = TEXT("Typed-artifact contract violates shared mutation, finalizer, lifetime, or budget bounds.");
		return false;
	}
	OutPrepared.Contract = Contract;
	if (!BuildBindingFingerprint(Contract, OutPrepared.BindingFingerprint, OutError))
	{
		return false;
	}
	FHyperAIStudioTypedOperationRegistry Registry;
	FHyperAIStudioValidatedPlan Plan;
	if (!BuildPlan(OutPrepared, TEXT("artifact-prepare-0001"), FString(), Registry, Plan, OutError))
	{
		return false;
	}
	OutPrepared.CapabilityHash = Plan.CapabilityHash;
	OutPrepared.PlanHash = Plan.PlanHash;
	OutPrepared.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
	OutPrepared.EffectFingerprint = Plan.EffectFingerprint;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.typed-artifact-contract.v1"));
	AppendToken(Canonical, OutPrepared.BindingFingerprint);
	AppendToken(Canonical, OutPrepared.CapabilityHash);
	AppendToken(Canonical, OutPrepared.PlanHash);
	AppendToken(Canonical, OutPrepared.AuthorizationPlanHash);
	AppendToken(Canonical, OutPrepared.EffectFingerprint);
	OutPrepared.ContractFingerprint = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
	if (!IsSha256(OutPrepared.ContractFingerprint))
	{
		OutError = TEXT("Typed-artifact contract fingerprint could not be computed.");
		return false;
	}
	return true;
}

namespace HyperAIStudio::TypedArtifact::Private
{
	class FExecutionAuthorizationGate final : public IHyperAIStudioPlanAuthorizationGate
	{
	public:
		FExecutionAuthorizationGate(
			FHyperAIStudioDomainExecutionLease& InLease,
			const FString& InExternalToken)
			: Lease(InLease), ExternalToken(InExternalToken)
		{
		}

		virtual bool Inspect(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			if (Request.Token != ExternalToken || Request.CanonicalProjectId != Lease.GetBinding().CanonicalProjectId
				|| Request.OperationId != Lease.GetOperationId() || Request.Token.Len() < 16)
			{
				OutError = TEXT("Exact typed-artifact authorization inspection failed.");
				return false;
			}
			FHyperAIStudioDomainAuthorizationReceipt DomainReceipt;
			if (!Lease.InspectAuthorizationExact(ExternalToken, NowUtcMs, DomainReceipt, OutError))
			{
				return false;
			}
			if (!Bound.Token.IsEmpty()
				&& (Request.Token != Bound.Token
					|| Request.AuthorizationPlanHash != Bound.AuthorizationPlanHash
					|| Request.Safety != Bound.Safety
					|| DomainReceipt.Nonce != Nonce
					|| DomainReceipt.ExpiresUtcMs != ExpiresUtcMs))
			{
				OutError = TEXT("Typed-artifact authorization request drifted after inspection.");
				return false;
			}
			Bound = Request;
			Nonce = DomainReceipt.Nonce;
			ExpiresUtcMs = DomainReceipt.ExpiresUtcMs;
			OutReceipt = {
				Bound,
				Nonce,
				ExpiresUtcMs,
				DomainReceipt.bConsumed
					? EHyperAIStudioPlanAuthorizationState::AlreadyConsumed
					: EHyperAIStudioPlanAuthorizationState::Available};
			return true;
		}

		virtual bool Consume(
			const FHyperAIStudioPlanAuthorizationRequest& Request,
			const int64 NowUtcMs,
			FHyperAIStudioPlanAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			if (Bound.Token.IsEmpty()
				|| Request.Token != Bound.Token
				|| Request.AuthorizationPlanHash != Bound.AuthorizationPlanHash
				|| Request.Safety != Bound.Safety)
			{
				OutError = TEXT("Typed-artifact authorization was not inspected exactly before consumption.");
				return false;
			}
			FHyperAIStudioDomainAuthorizationReceipt DomainReceipt;
			if (!Lease.AuthorizeExact(ExternalToken, NowUtcMs, DomainReceipt, OutError)
				|| DomainReceipt.Nonce != Nonce
				|| DomainReceipt.ExpiresUtcMs != ExpiresUtcMs)
			{
				return false;
			}
			OutReceipt = {Bound, Nonce, ExpiresUtcMs, EHyperAIStudioPlanAuthorizationState::AlreadyConsumed};
			return true;
		}

		void ForgetBearer()
		{
			ExternalToken.Reset();
			Bound.Token.Reset();
		}

	private:
		FHyperAIStudioDomainExecutionLease& Lease;
		FString ExternalToken;
		FHyperAIStudioPlanAuthorizationRequest Bound;
		FString Nonce;
		int64 ExpiresUtcMs = 0;
	};

	class FValidatorReceiptServer final : public IHyperAIStudioPlanValidatorReceiptGate
	{
	public:
		explicit FValidatorReceiptServer(IHyperAIStudioTypedArtifactClock& InClock) : Clock(InClock) {}

		FHyperAIStudioPlanValidatorReceipt Issue(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& CanonicalProjectId,
			const FString& ActionNonce,
			const FString& PostconditionHash)
		{
			FHyperAIStudioPlanValidatorReceipt Receipt;
			Receipt.ServerReceiptToken = TEXT("fresh-receipt-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			Receipt.CanonicalProjectId = CanonicalProjectId;
			Receipt.OperationId = Plan.OperationId;
			Receipt.PlanHash = Plan.PlanHash;
			Receipt.CapabilityHash = Plan.CapabilityHash;
			Receipt.EffectFingerprint = Plan.EffectFingerprint;
			Receipt.ActionNonce = ActionNonce;
			Receipt.ValidatorId = FHyperAIStudioTypedPlanValidator::FreshValidatorId();
			Receipt.ApprovedValidatorFingerprint = FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint();
			Receipt.PostconditionHash = PostconditionHash;
			Receipt.IssuedUtcMs = Clock.NowUtcMs();
			Receipt.ExpiresUtcMs = Receipt.IssuedUtcMs + 60 * 1000;
			FString Canonical;
			AppendToken(Canonical, Receipt.ServerReceiptToken);
			AppendToken(Canonical, Receipt.CanonicalProjectId);
			AppendToken(Canonical, Receipt.OperationId);
			AppendToken(Canonical, Receipt.PlanHash);
			AppendToken(Canonical, Receipt.CapabilityHash);
			AppendToken(Canonical, Receipt.EffectFingerprint);
			AppendToken(Canonical, Receipt.ActionNonce);
			AppendToken(Canonical, Receipt.ValidatorId);
			AppendToken(Canonical, Receipt.ApprovedValidatorFingerprint);
			AppendToken(Canonical, Receipt.PostconditionHash);
			AppendInt(Canonical, Receipt.IssuedUtcMs);
			AppendInt(Canonical, Receipt.ExpiresUtcMs);
			Receipt.ReceiptFingerprint = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
			Issued.Add(Receipt.ServerReceiptToken, Receipt);
			return Receipt;
		}

		virtual bool Verify(
			const FHyperAIStudioPlanValidatorReceipt& Receipt,
			const int64 NowUtcMs,
			FString& OutError) override
		{
			const FHyperAIStudioPlanValidatorReceipt* Exact = Issued.Find(Receipt.ServerReceiptToken);
			if (!Exact || Consumed.Contains(Receipt.ServerReceiptToken)
				|| Exact->ReceiptFingerprint != Receipt.ReceiptFingerprint
				|| Exact->CanonicalProjectId != Receipt.CanonicalProjectId
				|| Exact->OperationId != Receipt.OperationId || Exact->PlanHash != Receipt.PlanHash
				|| Exact->CapabilityHash != Receipt.CapabilityHash
				|| Exact->EffectFingerprint != Receipt.EffectFingerprint
				|| Exact->ActionNonce != Receipt.ActionNonce
				|| Exact->ValidatorId != Receipt.ValidatorId
				|| Exact->ApprovedValidatorFingerprint != Receipt.ApprovedValidatorFingerprint
				|| Exact->PostconditionHash != Receipt.PostconditionHash
				|| Exact->IssuedUtcMs != Receipt.IssuedUtcMs || Exact->ExpiresUtcMs != Receipt.ExpiresUtcMs
				|| NowUtcMs < Receipt.IssuedUtcMs || NowUtcMs >= Receipt.ExpiresUtcMs)
			{
				OutError = TEXT("Server-issued fresh-verification receipt was not exact, current, or single-use.");
				return false;
			}
			Consumed.Add(Receipt.ServerReceiptToken);
			return true;
		}

	private:
		IHyperAIStudioTypedArtifactClock& Clock;
		TMap<FString, FHyperAIStudioPlanValidatorReceipt> Issued;
		TSet<FString> Consumed;
	};

	class FArtifactDispatcher final : public IHyperAIStudioPlanAsyncDispatcher
	{
	public:
		FArtifactDispatcher(
			FHyperAIStudioTypedArtifactClaimState& InClaim,
			IHyperAIStudioTypedArtifactStateGate& InStateGate,
			FValidatorReceiptServer& InReceiptServer,
			IHyperAIStudioTypedArtifactClock& InClock,
			TSharedPtr<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe> InDispatchPermit)
			: Claim(InClaim)
			, StateGate(InStateGate)
			, ReceiptServer(InReceiptServer)
			, Clock(InClock)
			, DispatchPermit(MoveTemp(InDispatchPermit))
		{
		}

		virtual bool Prepare(
			const FHyperAIStudioValidatedPlan& Plan,
			const FString& CanonicalProjectId,
			FString& OutError) override
		{
			if (CanonicalProjectId != Claim.Receipt.CanonicalProjectId
				|| Plan.OperationId != Claim.Receipt.OperationId || Plan.PlanHash != Claim.Receipt.PlanHash)
			{
				OutError = TEXT("Dispatcher preparation identity does not match the single-use claim.");
				return false;
			}
			return Revalidate(EHyperAIStudioDomainExecutionActionKind::Apply, OutError);
		}

		virtual bool PreflightBeforeCommit(
			const FHyperAIStudioValidatedPlan& Plan,
			const FHyperAIStudioPlanScheduledAction& Action,
			FString& OutError) override
		{
			bApplyCommitPreflightPassed = false;
			CommitDispatchLease.Reset();
			if (Plan.OperationId != Claim.Receipt.OperationId
				|| Action.Kind != EHyperAIStudioPlanActionKind::Step
				|| Action.Safety == EHyperAIStudioPlanSafety::Read
				|| !Revalidate(EHyperAIStudioDomainExecutionActionKind::Apply, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("The exact typed-artifact commit preflight was not valid.");
				}
				return false;
			}
			if (DispatchPermit.IsValid()
				&& !DispatchPermit->AcquireBeforeCommit(CommitDispatchLease, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("The exact typed-artifact commit permit was not admitted.");
				}
				return false;
			}
			bApplyCommitPreflightPassed = true;
			return true;
		}

		virtual bool Dispatch(
			const FHyperAIStudioValidatedPlan& Plan,
			const FHyperAIStudioPlanScheduledAction& Action,
			FCompletion Completion,
			FString& OutError) override
		{
			const EHyperAIStudioDomainExecutionActionKind DomainAction = ToDomainAction(Action.Kind);
			const int64 StartedMs = Clock.NowMonotonicMs();
			FHyperAIStudioPlanActionResult Result;
			FHyperAIStudioPlanBackendTelemetry Telemetry;
			Telemetry.NativeOperations = 1;
			Telemetry.CompileCount = DomainAction == EHyperAIStudioDomainExecutionActionKind::Compile ? 1 : 0;
			Telemetry.ValidateCount = DomainAction == EHyperAIStudioDomainExecutionActionKind::Validate ? 1 : 0;
			Telemetry.SaveCount = DomainAction == EHyperAIStudioDomainExecutionActionKind::Save ? 1 : 0;
			Telemetry.FreshVerifyCount = DomainAction == EHyperAIStudioDomainExecutionActionKind::VerifyFresh ? 1 : 0;

			const bool bUseCommitPreflight = bApplyCommitPreflightPassed
				&& DomainAction == EHyperAIStudioDomainExecutionActionKind::Apply;
			bApplyCommitPreflightPassed = false;
			TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe> DispatchLease;
			if (bUseCommitPreflight)
			{
				DispatchLease = MoveTemp(CommitDispatchLease);
			}
			else
			{
				CommitDispatchLease.Reset();
			}
			if (!bUseCommitPreflight && !Revalidate(DomainAction, OutError))
			{
				Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
				if (bKnownEffect) { Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown; }
				const int64 ElapsedMs = FMath::Max<int64>(0, Clock.NowMonotonicMs() - StartedMs);
				Telemetry.GameThreadMs = static_cast<int32>(
					FMath::Min<int64>(ElapsedMs, TNumericLimits<int32>::Max()));
				Completion(MoveTemp(Result), Telemetry);
				return true;
			}

			const bool bDispatchPermitMissing = bUseCommitPreflight
				&& DispatchPermit.IsValid() && !DispatchLease.IsValid();
			if (bDispatchPermitMissing
				|| (!bUseCommitPreflight && DispatchPermit.IsValid()
					&& !DispatchPermit->AcquireBeforeDispatch(DispatchLease, OutError)))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("The exact typed-artifact dispatch permit was not available.");
				}
				Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
				if (bKnownEffect) { Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown; }
				const int64 ElapsedMs = FMath::Max<int64>(0, Clock.NowMonotonicMs() - StartedMs);
				Telemetry.GameThreadMs = static_cast<int32>(
					FMath::Min<int64>(ElapsedMs, TNumericLimits<int32>::Max()));
				Completion(MoveTemp(Result), Telemetry);
				return true;
			}
			const FHyperAIStudioDomainDispatchResult DomainResult = Claim.ExecutionLease.DispatchAction(
				{DomainAction, Action.ActionNonce});
			DispatchLease.Reset();
			const int64 ElapsedMs = FMath::Max<int64>(0, Clock.NowMonotonicMs() - StartedMs);
			Telemetry.GameThreadMs = static_cast<int32>(
				FMath::Min<int64>(ElapsedMs, TNumericLimits<int32>::Max()));
			Telemetry.OutputBytes = DomainResult.Payload.IsValid()
				? DomainResult.Payload->GetBoundedByteSize() : 0;
			switch (DomainResult.Outcome)
			{
			case EHyperAIStudioDomainDispatchOutcome::Succeeded:
				Result.Outcome = EHyperAIStudioPlanActionOutcome::Succeeded;
				break;
			case EHyperAIStudioDomainDispatchOutcome::OutcomeUnknown:
				Result.Outcome = EHyperAIStudioPlanActionOutcome::OutcomeUnknown;
				break;
			case EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect:
				if (Action.Safety == EHyperAIStudioPlanSafety::Read)
				{
					Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
					Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
				}
				else
				{
					Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedAfterKnownEffect;
					Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
				}
				break;
			default:
				Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
				if (bKnownEffect) { Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown; }
				break;
			}

			if (Result.Outcome == EHyperAIStudioPlanActionOutcome::Succeeded
				&& DomainAction == EHyperAIStudioDomainExecutionActionKind::Apply)
			{
				bKnownEffect = true;
			}
			if (Result.Outcome == EHyperAIStudioPlanActionOutcome::Succeeded
				&& DomainAction == EHyperAIStudioDomainExecutionActionKind::VerifyFresh)
			{
				FString PostconditionHash;
				if (!DomainResult.Payload.IsValid()
					|| !StateGate.VerifyFresh(
						Claim.Prepared, *Claim.Payload, *DomainResult.Payload, PostconditionHash, OutError)
					|| !IsSha256(PostconditionHash))
				{
					Result.Outcome = EHyperAIStudioPlanActionOutcome::FailedBeforeEffect;
					Result.Evidence.RollbackState = EHyperAIStudioPlanRollbackState::Unknown;
				}
				else
				{
					Result.Evidence.ValidatorReceipt = ReceiptServer.Issue(
						Plan, Claim.Receipt.CanonicalProjectId, Action.ActionNonce, PostconditionHash);
				}
			}
			Completion(MoveTemp(Result), Telemetry);
			return true;
		}

		virtual void Finish(const EHyperAIStudioPlanCoordinatorState FinalState) override
		{
			bApplyCommitPreflightPassed = false;
			CommitDispatchLease.Reset();
			if (FinalState == EHyperAIStudioPlanCoordinatorState::OutcomeUnknown)
			{
				Claim.ExecutionLease.LatchOutcomeUnknown();
			}
		}

	private:
		bool Revalidate(const EHyperAIStudioDomainExecutionActionKind Action, FString& OutError)
		{
			if (!MatchesSealedPayload(Claim.Prepared, *Claim.Payload))
			{
				OutError = TEXT("Detached typed-artifact payload drifted after staging.");
				return false;
			}
			const FHyperAIStudioDomainResolveResult AdapterState = Claim.ExecutionLease.RevalidateExact();
			if (AdapterState.State != EHyperAIStudioDomainState::Ready)
			{
				OutError = TEXT("Typed adapter binding drifted: ") + AdapterState.DiagnosticCode;
				return false;
			}
			FHyperAIStudioDomainPrerequisiteSnapshot Prerequisites;
			FHyperAIStudioDomainAdmissionSnapshot Admission;
			if (!StateGate.Revalidate(
					Claim.Prepared, *Claim.Payload, Action, Prerequisites, Admission, OutError)
				|| !ExactSnapshots(Claim.Prepared, Prerequisites, Admission))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("Admission or prerequisite snapshot drifted."); }
				return false;
			}
			return true;
		}

		FHyperAIStudioTypedArtifactClaimState& Claim;
		IHyperAIStudioTypedArtifactStateGate& StateGate;
		FValidatorReceiptServer& ReceiptServer;
		IHyperAIStudioTypedArtifactClock& Clock;
		TSharedPtr<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe> DispatchPermit;
		TSharedPtr<IHyperAIStudioTypedArtifactDispatchLease, ESPMode::ThreadSafe> CommitDispatchLease;
		bool bKnownEffect = false;
		bool bApplyCommitPreflightPassed = false;
	};

	EHyperAIStudioTypedArtifactExecutionState MapState(const EHyperAIStudioPlanCoordinatorState State)
	{
		switch (State)
		{
		case EHyperAIStudioPlanCoordinatorState::Completed: return EHyperAIStudioTypedArtifactExecutionState::Completed;
		case EHyperAIStudioPlanCoordinatorState::ReplayCompleted: return EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted;
		case EHyperAIStudioPlanCoordinatorState::Partial: return EHyperAIStudioTypedArtifactExecutionState::Partial;
		case EHyperAIStudioPlanCoordinatorState::RolledBack: return EHyperAIStudioTypedArtifactExecutionState::RolledBack;
		case EHyperAIStudioPlanCoordinatorState::OutcomeUnknown: return EHyperAIStudioTypedArtifactExecutionState::OutcomeUnknown;
		default: return EHyperAIStudioTypedArtifactExecutionState::Failed;
		}
	}

	void CopyStatus(
		const FHyperAIStudioPlanExecutionStatus& Status,
		FHyperAIStudioTypedArtifactExecutionResult& Out)
	{
		Out.State = MapState(Status.CoordinatorState);
		Out.Status = Status.Status;
		Out.Diagnostic = Status.Diagnostic.Left(FHyperAIStudioTypedArtifactLimits::MaxDiagnosticChars);
		Out.OperationId = Status.OperationId;
		Out.CanonicalProjectId = Status.CanonicalProjectId;
		Out.PlanHash = Status.PlanHash;
		Out.AuthorizationPlanHash = Status.AuthorizationPlanHash;
		Out.CapabilityHash = Status.CapabilityHash;
		Out.EffectFingerprint = Status.EffectFingerprint;
		Out.CompletedActionCount = Status.CompletedActionCount;
		Out.ScheduledActionCount = Status.ScheduledActionCount;
		Out.NativeOperationCount = Status.Telemetry.NativeOperationCount;
		Out.GameThreadMs = Status.Telemetry.GameThreadMs;
		Out.OutputBytes = Status.Telemetry.OutputBytes;
		Out.bReplay = Status.bReplay;
		Out.bOutcomeUnknown = Status.bOutcomeUnknown;
		Out.bFallbackPermitted = false;
	}

	class FAsyncSession final : public IHyperAIStudioTypedArtifactAsyncSession
	{
	public:
		FAsyncSession(
			TUniquePtr<FHyperAIStudioTypedArtifactClaimState>&& InClaimed,
			FString InProjectRoot,
			FString InAuthorizationToken,
			const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& InStateGate,
			const TSharedRef<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe>& InDispatchPermit,
			FHyperAIStudioTypedOperationRegistry&& InRegistry,
			FHyperAIStudioValidatedPlan&& InPlan)
			: Claimed(MoveTemp(InClaimed))
			, ProjectRoot(MoveTemp(InProjectRoot))
			, AuthorizationToken(MoveTemp(InAuthorizationToken))
			, StateGate(InStateGate)
			, DispatchPermit(InDispatchPermit)
			, Registry(MoveTemp(InRegistry))
			, Plan(MoveTemp(InPlan))
		{
			InitializeIdentitySnapshot(TEXT("prepared"), TEXT("The exact immutable artifact is prepared for durable admission."));
		}

		virtual EHyperAIStudioTypedArtifactAsyncStartResult Start(FString& OutError) override
		{
			OutError.Reset();
			if (bStartAttempted)
			{
				OutError = TEXT("A typed-artifact async session may be started exactly once.");
				return Snapshot.bTerminal
					? EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal
					: EHyperAIStudioTypedArtifactAsyncStartResult::Started;
			}
			bStartAttempted = true;
			if (!IsInGameThread())
			{
				OutError = TEXT("Typed-artifact durable admission must run on the Unreal game thread.");
				Reject(TEXT("game_thread_required"), OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}
			if (!Claimed.IsValid() || !Claimed->Clock.IsValid()
				|| Claimed->ExpiresMonotonicMs <= Claimed->Clock->NowMonotonicMs())
			{
				OutError = TEXT("The exact staged artifact expired before durable admission.");
				Reject(TEXT("stage_expired"), OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}

			Journal = MakeUnique<FHyperAIStudioOperationJournal>(ProjectRoot);
			if (!Journal->Load(OutError))
			{
				Reject(
					OutError.Contains(TEXT("Another HyperAIStudio journal owner"))
						? TEXT("project_execution_busy") : TEXT("journal_unavailable"),
					OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}
			if (Journal->GetCanonicalProjectId() != Claimed->Receipt.CanonicalProjectId)
			{
				OutError = TEXT("The trusted project journal no longer matches the exact staged project identity.");
				Reject(TEXT("project_identity_changed"), OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}

			if (const TOptional<FHyperAIStudioOperationRecord> Existing = Journal->Find(Plan.OperationId);
				Existing.IsSet())
			{
				bDurableIdentityExists = true;
				if (Existing->PlanHash != Plan.PlanHash || Existing->CapabilityHash != Plan.CapabilityHash)
				{
					OutError = TEXT("operation_id is durably bound to a different exact plan or capability inventory.");
					Reject(TEXT("operation_id_conflict"), OutError);
					return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
				}
				if (Plan.bHasDestructive || Plan.bHasExternalEffect)
				{
					if (AuthorizationToken.IsEmpty())
					{
						OutError = TEXT("Risky durable replay requires the exact nonempty originally consumed server grant.");
						Reject(TEXT("authorization_replay_mismatch"), OutError);
						return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
					}
					FHyperAIStudioDomainAuthorizationReceipt ReplayAuthorization;
					if (!Claimed->ExecutionLease.InspectAuthorizationExact(
							AuthorizationToken,
							Claimed->Clock->NowUtcMs(),
							ReplayAuthorization,
							OutError)
						|| !ReplayAuthorization.bConsumed)
					{
						if (OutError.IsEmpty())
						{
							OutError = TEXT("Risky durable replay requires the exact originally consumed server grant.");
						}
						Reject(TEXT("authorization_replay_mismatch"), OutError);
						return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
					}
				}
				if (AdoptDurableTerminal(Existing.GetValue(), OutError))
				{
					ForgetAuthorizationMaterial();
					return EHyperAIStudioTypedArtifactAsyncStartResult::ReplayTerminal;
				}
				Reject(
					Existing->IsTerminal()
						? TEXT("replay_evidence_invalid")
						: TEXT("operation_already_in_progress"),
					OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}
			if ((Plan.MaximumSafety == EHyperAIStudioPlanSafety::Destructive
					|| Plan.MaximumSafety == EHyperAIStudioPlanSafety::ExternalEffect)
				&& (AuthorizationToken.Len() < 16
					|| AuthorizationToken.Len() > FHyperAIStudioDomainLimits::MaxAuthorizationTokenChars))
			{
				OutError = TEXT("Destructive/external typed artifacts default-deny without an exact bounded server grant.");
				Reject(TEXT("authorization_required"), OutError);
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}

			JournalAdapter = MakeUnique<FHyperAIStudioOperationJournalPlanAdapter>(*Journal);
			PlanClock = MakeUnique<FPlanClockAdapter>(*Claimed->Clock);
			AuthorizationGate = MakeUnique<FExecutionAuthorizationGate>(
				Claimed->ExecutionLease, AuthorizationToken);
			ReceiptServer = MakeUnique<FValidatorReceiptServer>(*Claimed->Clock);
			Dispatcher = MakeUnique<FArtifactDispatcher>(
				*Claimed, *StateGate, *ReceiptServer, *Claimed->Clock, DispatchPermit);
			FString StartError;
			const bool bStarted = Runtime.StartTypedArtifact(
				Plan,
				Registry,
				Claimed->Receipt.CanonicalProjectId,
				JournalAdapter.Get(),
				(Plan.bHasDestructive || Plan.bHasExternalEffect) ? AuthorizationGate.Get() : nullptr,
				ReceiptServer.Get(),
				PlanClock.Get(),
				Dispatcher.Get(),
				StartError);
			bRuntimeStarted = bStarted || Runtime.GetStatus().bTerminal;
			bDurableIdentityExists = Journal->Find(Plan.OperationId).IsSet();
			ForgetAuthorizationMaterial();
			if (!bStarted && Runtime.GetStatus().CoordinatorState
				!= EHyperAIStudioPlanCoordinatorState::ReplayCompleted)
			{
				OutError = StartError.IsEmpty()
					? TEXT("The exact typed-artifact runtime rejected durable admission.") : StartError;
				RefreshSnapshot();
				if (!Snapshot.bTerminal)
				{
					Reject(Runtime.IsOutcomeUnknown() ? TEXT("outcome_unknown") : TEXT("execution_rejected"), OutError);
				}
				return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
			}
			if (Runtime.GetStatus().bReplay)
			{
				RefreshSnapshot();
				return EHyperAIStudioTypedArtifactAsyncStartResult::ReplayTerminal;
			}

			// Edit accepts no bearer. Its core nonce is issued only after durable Begin -> Running,
			// and before the ticker is allowed to acquire a commit marker or invoke the adapter.
			if (Plan.MaximumSafety == EHyperAIStudioPlanSafety::Edit)
			{
				FString AuthorizationError;
				if (!Claimed->ExecutionLease.AuthorizeJournalAdmittedEdit(AuthorizationError))
				{
					FString AbortError;
					Runtime.AbortBeforeDispatch(AuthorizationError, AbortError);
					OutError = AbortError.IsEmpty() ? AuthorizationError : AbortError;
					RefreshSnapshot();
					return EHyperAIStudioTypedArtifactAsyncStartResult::RejectedTerminal;
				}
			}
			RefreshSnapshot();
			return EHyperAIStudioTypedArtifactAsyncStartResult::Started;
		}

		virtual bool PumpOne(FString& OutError) override
		{
			OutError.Reset();
			if (!IsInGameThread() || !bRuntimeStarted || Runtime.IsTerminal())
			{
				OutError = TEXT("The typed-artifact session is not available for another game-thread action.");
				RefreshSnapshot();
				return false;
			}
			if (Runtime.HasInFlightAction())
			{
				OutError = TEXT("Exactly one typed-artifact action is already in flight.");
				RefreshSnapshot();
				return false;
			}
			const bool bPumped = Runtime.Pump(OutError);
			RefreshSnapshot();
			return bPumped;
		}

		virtual void TickDeadline() override
		{
			if (IsInGameThread() && bRuntimeStarted)
			{
				Runtime.Tick();
				RefreshSnapshot();
			}
		}

		virtual bool RequestCancel(const FString& Reason, FString& OutError) override
		{
			OutError.Reset();
			if (!IsInGameThread() || !bRuntimeStarted || Runtime.IsTerminal())
			{
				OutError = TEXT("Only an active typed-artifact game-thread session can be cancelled.");
				return false;
			}
			const bool bCancelled = Runtime.RequestCancel(Reason, OutError);
			RefreshSnapshot();
			return bCancelled;
		}

		virtual FHyperAIStudioTypedArtifactAsyncSnapshot GetSnapshot() const override
		{
			if (bRuntimeStarted)
			{
				const_cast<FAsyncSession*>(this)->RefreshSnapshot();
			}
			return Snapshot;
		}

		virtual bool IsTerminal() const override
		{
			return Snapshot.bTerminal || (bRuntimeStarted && Runtime.IsTerminal());
		}

		virtual bool HasOutstandingDispatch() const override
		{
			return bRuntimeStarted && Runtime.HasOutstandingDispatch();
		}

	private:
		void InitializeIdentitySnapshot(const FString& Status, const FString& Diagnostic)
		{
			const FString ExistingReplayCredentialFingerprint = Snapshot.ReplayCredentialFingerprint;
			Snapshot = FHyperAIStudioTypedArtifactAsyncSnapshot{};
			Snapshot.Result.Status = Status;
			Snapshot.Result.Diagnostic = Diagnostic.Left(FHyperAIStudioTypedArtifactLimits::MaxDiagnosticChars);
			Snapshot.Result.OperationId = Claimed->Receipt.OperationId;
			Snapshot.Result.CanonicalProjectId = Claimed->Receipt.CanonicalProjectId;
			Snapshot.Result.PlanHash = Claimed->Receipt.PlanHash;
			Snapshot.Result.AuthorizationPlanHash = Claimed->Receipt.AuthorizationPlanHash;
			Snapshot.Result.CapabilityHash = Claimed->Receipt.CapabilityHash;
			Snapshot.Result.EffectFingerprint = Claimed->Receipt.EffectFingerprint;
			Snapshot.Result.bFallbackPermitted = false;
			Snapshot.Safety = Claimed->Prepared.Contract.Binding.ExpectedSafety;
			Snapshot.ReplayCredentialFingerprint = AuthorizationToken.IsEmpty()
				? ExistingReplayCredentialFingerprint
				: BuildReplayCredentialFingerprint(AuthorizationToken, Claimed->Receipt);
		}

		void Reject(const FString& Status, const FString& Diagnostic)
		{
			InitializeIdentitySnapshot(Status, Diagnostic);
			Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::Failed;
			Snapshot.bStarted = bRuntimeStarted;
			Snapshot.bAccepted = false;
			Snapshot.bTerminal = true;
			Snapshot.bDurable = bDurableIdentityExists;
			ForgetAuthorizationMaterial();
		}

		void ForgetAuthorizationMaterial()
		{
			AuthorizationToken.Reset();
			Plan.AuthorizationToken.Reset();
			Runtime.ForgetAuthorizationToken();
			if (AuthorizationGate)
			{
				AuthorizationGate->ForgetBearer();
			}
		}

		bool AdoptDurableTerminal(
			const FHyperAIStudioOperationRecord& Record,
			FString& OutError)
		{
			InitializeIdentitySnapshot(FHyperAIStudioOperationJournal::LexToString(Record.State), Record.StatusCode);
			Snapshot.bStarted = true;
			Snapshot.bAccepted = true;
			Snapshot.bTerminal = Record.IsTerminal();
			Snapshot.bDurable = true;
			switch (Record.State)
			{
			case EHyperAIStudioOperationState::Completed:
				if (!HasExactTerminalEvidence(
						Record,
						FHyperAIStudioTypedPlanValidator::FreshValidatorId(),
						FHyperAIStudioTypedPlanValidator::FreshValidatorFingerprint()))
				{
					OutError = TEXT("Completed journal state lacks current fully-bound fresh evidence.");
					return false;
				}
				Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted;
				Snapshot.Result.Status = TEXT("replay_completed");
				Snapshot.Result.bReplay = true;
				break;
			case EHyperAIStudioOperationState::Failed:
				Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::Failed;
				break;
			case EHyperAIStudioOperationState::Partial:
				Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::Partial;
				break;
			case EHyperAIStudioOperationState::RolledBack:
				if (!HasExactTerminalEvidence(
						Record,
						FHyperAIStudioTypedPlanValidator::RollbackValidatorId(),
						FHyperAIStudioTypedPlanValidator::RollbackValidatorFingerprint()))
				{
					OutError = TEXT("Rolled-back journal state lacks current fully-bound rollback evidence.");
					return false;
				}
				Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::RolledBack;
				break;
			case EHyperAIStudioOperationState::OutcomeUnknown:
				Snapshot.Result.State = EHyperAIStudioTypedArtifactExecutionState::OutcomeUnknown;
				Snapshot.Result.Status = TEXT("outcome_unknown");
				Snapshot.Result.bOutcomeUnknown = true;
				Claimed->ExecutionLease.LatchOutcomeUnknown();
				break;
			default:
				OutError = TEXT("The existing exact journal operation is non-terminal and cannot be reacquired.");
				return false;
			}
			return true;
		}

		bool HasExactTerminalEvidence(
			const FHyperAIStudioOperationRecord& Record,
			const FString& ExpectedValidatorId,
			const FString& ExpectedValidatorFingerprint) const
		{
			const FHyperAIStudioOperationEvidence& Evidence = Record.TerminalEvidence;
			return Evidence.Version == FHyperAIStudioOperationEvidence::CurrentVersion
				&& Evidence.CanonicalProjectId == Claimed->Receipt.CanonicalProjectId
				&& Evidence.OperationId == Plan.OperationId
				&& Evidence.PlanHash == Plan.PlanHash
				&& Evidence.CapabilityHash == Plan.CapabilityHash
				&& Evidence.EffectFingerprint == Plan.EffectFingerprint
				&& !Evidence.ActionNonce.IsEmpty()
				&& Evidence.ValidatorId == ExpectedValidatorId
				&& Evidence.ApprovedValidatorFingerprint == ExpectedValidatorFingerprint
				&& IsSha256(Evidence.PostconditionHash)
				&& IsSha256(Evidence.ReceiptFingerprint);
		}

		void RefreshSnapshot()
		{
			if (!bRuntimeStarted)
			{
				return;
			}
			const FHyperAIStudioPlanExecutionStatus RuntimeStatus = Runtime.GetStatus();
			CopyStatus(RuntimeStatus, Snapshot.Result);
			Snapshot.ActiveActionKind = RuntimeStatus.ActiveActionKind;
			Snapshot.ActiveStepId = RuntimeStatus.ActiveStepId;
			Snapshot.bStarted = true;
			Snapshot.bTerminal = RuntimeStatus.bTerminal;
			Snapshot.bOutstandingDispatch = Runtime.HasOutstandingDispatch();
			const TOptional<FHyperAIStudioOperationRecord> Durable = Journal.IsValid()
				? Journal->Find(Plan.OperationId) : TOptional<FHyperAIStudioOperationRecord>{};
			Snapshot.bDurable = Durable.IsSet();
			Snapshot.bAccepted = RuntimeStatus.bAccepted
				|| (Durable.IsSet() && Durable->State != EHyperAIStudioOperationState::Queued);
			Snapshot.LateResultCount = RuntimeStatus.Telemetry.LateResultCount;
		}

		// Destruction order is intentional: Runtime goes first; the execution claim and its adapter
		// lease remain alive until every collaborator and callback-facing dispatcher is gone.
		TUniquePtr<FHyperAIStudioTypedArtifactClaimState> Claimed;
		FString ProjectRoot;
		FString AuthorizationToken;
		TSharedPtr<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe> StateGate;
		TSharedPtr<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe> DispatchPermit;
		FHyperAIStudioTypedOperationRegistry Registry;
		FHyperAIStudioValidatedPlan Plan;
		TUniquePtr<FHyperAIStudioOperationJournal> Journal;
		TUniquePtr<FHyperAIStudioOperationJournalPlanAdapter> JournalAdapter;
		TUniquePtr<FPlanClockAdapter> PlanClock;
		TUniquePtr<FExecutionAuthorizationGate> AuthorizationGate;
		TUniquePtr<FValidatorReceiptServer> ReceiptServer;
		TUniquePtr<FArtifactDispatcher> Dispatcher;
		FHyperAIStudioPlanExecutionRuntime Runtime;
		FHyperAIStudioTypedArtifactAsyncSnapshot Snapshot;
		bool bStartAttempted = false;
		bool bRuntimeStarted = false;
		bool bDurableIdentityExists = false;
	};
}

bool FHyperAIStudioTypedArtifactExecutorInternal::CreateAsyncSession(
	FHyperAIStudioTypedArtifactClaim&& Claim,
	const FString& TrustedProjectRoot,
	const FString& AuthorizationToken,
	const TSharedRef<IHyperAIStudioTypedArtifactStateGate, ESPMode::ThreadSafe>& StateGate,
	const TSharedRef<IHyperAIStudioTypedArtifactDispatchPermit, ESPMode::ThreadSafe>& DispatchPermit,
	TSharedPtr<IHyperAIStudioTypedArtifactAsyncSession, ESPMode::ThreadSafe>& OutSession,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifact::Private;
	OutSession.Reset();
	OutError.Reset();
	TUniquePtr<FHyperAIStudioTypedArtifactClaimState> Claimed = MoveTemp(Claim.State);
	if (!Claimed.IsValid() || !Claimed->Payload.IsValid() || !Claimed->ExecutionLease.IsValid()
		|| !Claimed->Clock.IsValid())
	{
		OutError = TEXT("A valid single-use typed-artifact claim is required for async hosting.");
		return false;
	}
	if (!MatchesSealedPayload(Claimed->Prepared, *Claimed->Payload))
	{
		OutError = TEXT("Detached typed-artifact payload no longer matches its staged semantic identity.");
		return false;
	}
	FHyperAIStudioPreparedTypedArtifact Rebuilt;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Claimed->Prepared.Contract, Rebuilt, OutError)
		|| !SamePreparedIdentity(Claimed->Prepared, Rebuilt))
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Claimed artifact contract drifted after staging."); }
		return false;
	}
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(TrustedProjectRoot);
	if (CanonicalProjectId.IsEmpty() || CanonicalProjectId != Claimed->Receipt.CanonicalProjectId)
	{
		OutError = TEXT("The service-owned project root does not match the exact staged project identity.");
		return false;
	}
	const EHyperAIStudioDomainSafety Safety = Claimed->Prepared.Contract.Binding.ExpectedSafety;
	if (Safety == EHyperAIStudioDomainSafety::Edit && !AuthorizationToken.IsEmpty())
	{
		OutError = TEXT("Edit typed artifacts reject bearer tokens; durable journal admission issues the internal nonce.");
		return false;
	}
	FHyperAIStudioTypedOperationRegistry Registry;
	FHyperAIStudioValidatedPlan Plan;
	if (!BuildPlan(
			Claimed->Prepared,
			Claimed->Receipt.OperationId,
			AuthorizationToken,
			Registry,
			Plan,
			OutError)
		|| Plan.PlanHash != Claimed->Receipt.PlanHash
		|| Plan.AuthorizationPlanHash != Claimed->Receipt.AuthorizationPlanHash
		|| Plan.CapabilityHash != Claimed->Receipt.CapabilityHash
		|| Plan.EffectFingerprint != Claimed->Receipt.EffectFingerprint)
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Async execution plan no longer matches every staged hash."); }
		return false;
	}
	OutSession = MakeShared<FAsyncSession, ESPMode::ThreadSafe>(
		MoveTemp(Claimed), TrustedProjectRoot, AuthorizationToken, StateGate, DispatchPermit,
		MoveTemp(Registry), MoveTemp(Plan));
	return true;
}

bool FHyperAIStudioTypedArtifactExecutorInternal::ExecuteClaimed(
	FHyperAIStudioTypedArtifactClaim&& Claim,
	const FString& ProjectRoot,
	const FString& AuthorizationToken,
	IHyperAIStudioTypedArtifactStateGate& StateGate,
	FHyperAIStudioTypedArtifactExecutionResult& OutResult,
	FString& OutError)
{
	using namespace HyperAIStudio::TypedArtifact::Private;
	OutResult = FHyperAIStudioTypedArtifactExecutionResult{};
	OutError.Reset();
	TUniquePtr<FHyperAIStudioTypedArtifactClaimState> Claimed = MoveTemp(Claim.State);
	if (!Claimed.IsValid() || !Claimed->Payload.IsValid() || !Claimed->ExecutionLease.IsValid())
	{
		OutError = TEXT("A valid single-use typed-artifact claim is required.");
		return false;
	}
	if (!MatchesSealedPayload(Claimed->Prepared, *Claimed->Payload))
	{
		OutError = TEXT("Detached typed-artifact payload no longer matches the staged semantic identity.");
		return false;
	}
	if (!Claimed->Clock.IsValid())
	{
		OutError = TEXT("The claim no longer owns its server monotonic clock.");
		return false;
	}
	IHyperAIStudioTypedArtifactClock& ActiveClock = *Claimed->Clock;
	if (Claimed->ExpiresMonotonicMs <= ActiveClock.NowMonotonicMs())
	{
		OutError = TEXT("The staged typed-artifact claim expired on the monotonic server clock before execution.");
		return false;
	}
	FHyperAIStudioPreparedTypedArtifact Rebuilt;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Claimed->Prepared.Contract, Rebuilt, OutError)
		|| !SamePreparedIdentity(Claimed->Prepared, Rebuilt))
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Claimed artifact contract drifted after staging."); }
		return false;
	}
	if (Claimed->Prepared.Contract.Binding.ExpectedSafety == EHyperAIStudioDomainSafety::Edit
		&& !AuthorizationToken.IsEmpty())
	{
		OutError = TEXT("Edit typed artifacts reject client authorization tokens; journal admission is the authority.");
		return false;
	}

	FHyperAIStudioOperationJournal Journal(ProjectRoot);
	if (!Journal.Load(OutError)
		|| Journal.GetCanonicalProjectId() != Claimed->Receipt.CanonicalProjectId)
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Project journal identity does not match the exact staged project."); }
		return false;
	}
	FHyperAIStudioTypedOperationRegistry Registry;
	FHyperAIStudioValidatedPlan Plan;
	if (!BuildPlan(
			Claimed->Prepared, Claimed->Receipt.OperationId, AuthorizationToken,
			Registry, Plan, OutError)
		|| Plan.PlanHash != Claimed->Receipt.PlanHash
		|| Plan.AuthorizationPlanHash != Claimed->Receipt.AuthorizationPlanHash
		|| Plan.CapabilityHash != Claimed->Receipt.CapabilityHash
		|| Plan.EffectFingerprint != Claimed->Receipt.EffectFingerprint)
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Execution plan no longer matches every staged hash."); }
		return false;
	}

	FHyperAIStudioOperationJournalPlanAdapter JournalGate(Journal);
	FPlanClockAdapter PlanClock(ActiveClock);
	FExecutionAuthorizationGate AuthorizationGate(Claimed->ExecutionLease, AuthorizationToken);
	FValidatorReceiptServer ReceiptServer(ActiveClock);
	FArtifactDispatcher Dispatcher(*Claimed, StateGate, ReceiptServer, ActiveClock, nullptr);
	FHyperAIStudioPlanExecutionRuntime Runtime;
	if (!Runtime.StartTypedArtifact(
			Plan, Registry, Claimed->Receipt.CanonicalProjectId, &JournalGate,
			&AuthorizationGate, &ReceiptServer, &PlanClock, &Dispatcher, OutError))
	{
		CopyStatus(Runtime.GetStatus(), OutResult);
		return false;
	}
	// Edit is intentionally not classified as risky by the generic coordinator. Issue its internal
	// execution nonce only after journal Begin+Running proved ProceedNew, but before AcquireNextAction
	// writes commit_started or calls the adapter. No client bearer is accepted or consumed here.
	if (Plan.MaximumSafety == EHyperAIStudioPlanSafety::Edit && !Runtime.IsTerminal())
	{
		FString AuthorizationError;
		if (!Claimed->ExecutionLease.AuthorizeJournalAdmittedEdit(AuthorizationError))
		{
			FString AbortError;
			Runtime.AbortBeforeDispatch(AuthorizationError, AbortError);
			OutError = AbortError.IsEmpty() ? AuthorizationError : AbortError;
			CopyStatus(Runtime.GetStatus(), OutResult);
			return false;
		}
	}
	for (int32 Guard = 0; Guard < 16 && !Runtime.IsTerminal(); ++Guard)
	{
		Runtime.Tick();
		if (Runtime.IsTerminal()) { break; }
		FString PumpError;
		if (!Runtime.Pump(PumpError) && !Runtime.IsTerminal())
		{
			OutError = PumpError;
			break;
		}
		Runtime.Tick();
	}
	if (!Runtime.IsTerminal())
	{
		FString CancelError;
		Runtime.RequestCancel(TEXT("Typed-artifact serial action bound was exceeded."), CancelError);
		if (OutError.IsEmpty()) { OutError = CancelError; }
	}
	CopyStatus(Runtime.GetStatus(), OutResult);
	if (OutResult.State == EHyperAIStudioTypedArtifactExecutionState::OutcomeUnknown)
	{
		Claimed->ExecutionLease.LatchOutcomeUnknown();
	}
	if (OutError.IsEmpty() && OutResult.State != EHyperAIStudioTypedArtifactExecutionState::Completed
		&& OutResult.State != EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted)
	{
		OutError = OutResult.Diagnostic.IsEmpty()
			? TEXT("Typed-artifact execution ended without a successful terminal proof.")
			: OutResult.Diagnostic;
	}
	return OutResult.State == EHyperAIStudioTypedArtifactExecutionState::Completed
		|| OutResult.State == EHyperAIStudioTypedArtifactExecutionState::ReplayCompleted;
}
