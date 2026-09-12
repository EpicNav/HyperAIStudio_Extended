// Games by Hyper 2026.

#include "HyperAIStudioPlanValidateToolset.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioTypedPlan.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioPlanValidateToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPlanValidateTool, Log, All);

namespace HyperAIStudio::PlanValidate::Private
{
	constexpr const TCHAR* ToolName = TEXT("hyper_plan_validate");
	constexpr const TCHAR* PackId = TEXT("shared_foundation");

	bool IsSha256Fingerprint(const FString& Value)
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

	bool HasWellFormedUtf16(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character >= 0xd800 && Character <= 0xdbff)
			{
				if (Index + 1 >= Value.Len()
					|| Value[Index + 1] < 0xdc00
					|| Value[Index + 1] > 0xdfff)
				{
					return false;
				}
				++Index;
			}
			else if (Character >= 0xdc00 && Character <= 0xdfff)
			{
				return false;
			}
		}
		return true;
	}

	FString HashUtf8Sha256(const FString& Value)
	{
		if (Value.Len() > FHyperAIStudioPlanValidateContracts::MaxHashInputUtf8Bytes
			|| !HasWellFormedUtf16(Value))
		{
			return FString();
		}
		const FTCHARToUTF8 Utf8(*Value);
		static constexpr uint32 Constants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
		uint32 State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
		const int64 ByteCount = Utf8.Length();
		if (ByteCount < 0 || ByteCount > FHyperAIStudioPlanValidateContracts::MaxHashInputUtf8Bytes)
		{
			return FString();
		}
		TArray<uint8> Padded;
		Padded.Append(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<int32>(ByteCount));
		Padded.Add(0x80u);
		while ((Padded.Num() % 64) != 56)
		{
			Padded.Add(0u);
		}
		const uint64 BitCount = static_cast<uint64>(ByteCount) * 8u;
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Padded.Add(static_cast<uint8>((BitCount >> Shift) & 0xffu));
		}

		auto RotateRight = [](const uint32 Input, const uint32 Shift)
		{
			return (Input >> Shift) | (Input << (32u - Shift));
		};
		for (int32 Block = 0; Block < Padded.Num(); Block += 64)
		{
			uint32 Words[64]{};
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const int32 Offset = Block + Index * 4;
				Words[Index] = (static_cast<uint32>(Padded[Offset]) << 24)
					| (static_cast<uint32>(Padded[Offset + 1]) << 16)
					| (static_cast<uint32>(Padded[Offset + 2]) << 8)
					| static_cast<uint32>(Padded[Offset + 3]);
			}
			for (int32 Index = 16; Index < 64; ++Index)
			{
				const uint32 Small0 = RotateRight(Words[Index - 15], 7)
					^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const uint32 Small1 = RotateRight(Words[Index - 2], 17)
					^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + Small0 + Words[Index - 7] + Small1;
			}

			uint32 A = State[0];
			uint32 B = State[1];
			uint32 C = State[2];
			uint32 D = State[3];
			uint32 E = State[4];
			uint32 F = State[5];
			uint32 G = State[6];
			uint32 H = State[7];
			for (int32 Index = 0; Index < 64; ++Index)
			{
				const uint32 Big1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choice = (E & F) ^ ((~E) & G);
				const uint32 Temp1 = H + Big1 + Choice + Constants[Index] + Words[Index];
				const uint32 Big0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = Big0 + Majority;
				H = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}
			State[0] += A;
			State[1] += B;
			State[2] += C;
			State[3] += D;
			State[4] += E;
			State[5] += F;
			State[6] += G;
			State[7] += H;
		}

		FString Hex = TEXT("sha256:");
		Hex.Reserve(71);
		for (const uint32 Word : State)
		{
			Hex += FString::Printf(TEXT("%08x"), Word);
		}
		return Hex;
	}

	void AppendToken(FString& Buffer, const FString& Value)
	{
		Buffer.Appendf(TEXT("%d:"), Value.Len());
		Buffer += Value;
		Buffer += TEXT("|");
	}

	FString ClipText(const FString& Value, const int32 MaxCharacters)
	{
		if (MaxCharacters <= 0)
		{
			return FString();
		}
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	void AddDiagnostic(
		FHyperAIPlanValidateReport& Report,
		const FString& Code,
		const FString& Path,
		const FString& Message)
	{
		if (Report.Diagnostics.Num() >= FHyperAIStudioPlanLimits::MaxDiagnostics)
		{
			return;
		}
		FHyperAIPlanValidateDiagnostic& Diagnostic = Report.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = ClipText(Code, FHyperAIStudioPlanLimits::MaxFieldChars);
		Diagnostic.Path = ClipText(Path, FHyperAIStudioPlanLimits::MaxTargetChars);
		Diagnostic.Message = ClipText(Message, FHyperAIStudioPlanValidateContracts::MaxDiagnosticCharacters);
	}

	FString PreconditionKindToString(const EHyperAIStudioPlanPreconditionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanPreconditionKind::ObjectExists: return TEXT("object_exists");
		case EHyperAIStudioPlanPreconditionKind::ObjectAbsent: return TEXT("object_absent");
		case EHyperAIStudioPlanPreconditionKind::RevisionEquals: return TEXT("revision_equals");
		case EHyperAIStudioPlanPreconditionKind::PropertyEquals: return TEXT("property_equals");
		case EHyperAIStudioPlanPreconditionKind::PluginAvailable: return TEXT("plugin_available");
		case EHyperAIStudioPlanPreconditionKind::EditorStateEquals: return TEXT("editor_state_equals");
		default: return TEXT("invalid");
		}
	}

	FString EffectKindToString(const EHyperAIStudioPlanEffectKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioPlanEffectKind::ObjectCreated: return TEXT("object_created");
		case EHyperAIStudioPlanEffectKind::ObjectUpdated: return TEXT("object_updated");
		case EHyperAIStudioPlanEffectKind::ObjectDeleted: return TEXT("object_deleted");
		case EHyperAIStudioPlanEffectKind::RuntimeExternalEffect: return TEXT("runtime_external_effect");
		default: return TEXT("invalid");
		}
	}

	const FHyperAIStudioCapabilityToolDefinition* FindPlanValidateTool(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		int32& OutMatchCount)
	{
		OutMatchCount = 0;
		const FHyperAIStudioCapabilityToolDefinition* Found = nullptr;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name == ToolName)
			{
				++OutMatchCount;
				Found = &Tool;
			}
		}
		return OutMatchCount == 1 ? Found : nullptr;
	}

	EHyperAIStudioPlanValidateAdmissionState ToPlanValidateAdmissionState(
		const EHyperAIStudioCapabilityAdmissionState State)
	{
		switch (State)
		{
		case EHyperAIStudioCapabilityAdmissionState::SourceCandidate:
			return EHyperAIStudioPlanValidateAdmissionState::SourceCandidate;
		case EHyperAIStudioCapabilityAdmissionState::Admitted:
			return EHyperAIStudioPlanValidateAdmissionState::Admitted;
		case EHyperAIStudioCapabilityAdmissionState::Planned:
		default:
			return EHyperAIStudioPlanValidateAdmissionState::Planned;
		}
	}

	struct FFrozenValidationContext
	{
		FHyperAIStudioTypedOperationRegistry Registry;
		FString CapabilityFingerprint;
		FString CatalogFingerprint;
		TArray<FString> CatalogErrors;
		bool bCatalogValid = false;
	};

	const FFrozenValidationContext& GetFrozenValidationContext()
	{
		static const FFrozenValidationContext Context = []
		{
			FFrozenValidationContext Value;
			Value.Registry = FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
			Value.CapabilityFingerprint = Value.Registry.ComputeCapabilityHash();
			Value.CatalogFingerprint = FHyperAIStudioCapabilityPackRegistry::GetCatalog().GeneratedFingerprint;
			Value.bCatalogValid =
				FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(Value.CatalogErrors);
			return Value;
		}();
		return Context;
	}

	void CopyValidatorDiagnostics(
		const FHyperAIStudioPlanDryRunResult& Source,
		FHyperAIPlanValidateReport& Report)
	{
		for (const FHyperAIStudioPlanDiagnostic& Item : Source.Diagnostics)
		{
			AddDiagnostic(Report, Item.Code, Item.Path, Item.Message);
		}
	}

	void PopulateValidatedDeclarations(
		const FHyperAIStudioValidatedPlan& Plan,
		const FHyperAIStudioPlanDryRunResult& DryRun,
		FHyperAIPlanValidateReport& Report)
	{
		Report.OrderedStepIds.Reserve(DryRun.OrderedStepIds.Num());
		for (const FString& StepId : DryRun.OrderedStepIds)
		{
			Report.OrderedStepIds.Add(ClipText(StepId, FHyperAIStudioPlanLimits::MaxStepIdChars));
		}

		Report.OrderedSchedule.Reserve(DryRun.Schedule.Num());
		for (int32 Index = 0; Index < DryRun.Schedule.Num(); ++Index)
		{
			const FHyperAIStudioPlanScheduledAction& Action = DryRun.Schedule[Index];
			FHyperAIPlanValidateScheduleItem& Item = Report.OrderedSchedule.AddDefaulted_GetRef();
			Item.Order = Index;
			Item.Kind = FHyperAIStudioTypedPlanValidator::ActionKindToString(Action.Kind);
			Item.StepId = ClipText(Action.StepId, FHyperAIStudioPlanLimits::MaxStepIdChars);
			Item.OperationType = ClipText(Action.OperationType, FHyperAIStudioPlanLimits::MaxFieldChars);
			Item.Safety = FHyperAIStudioTypedPlanValidator::SafetyToString(Action.Safety);
			Item.MaxNativeOperations = Action.Budget.MaxNativeOperations;
			Item.MaxGameThreadMs = Action.Budget.MaxGameThreadMs;
			Item.MaxOutputBytes = Action.Budget.MaxOutputBytes;
		}

		Report.Preconditions.Reserve(FMath::Min(
			FHyperAIStudioPlanLimits::MaxTotalPreconditions,
			Plan.Steps.Num() * FHyperAIStudioPlanLimits::MaxPreconditionsPerStep));
		Report.Effects.Reserve(FMath::Min(
			FHyperAIStudioPlanLimits::MaxTotalEffects,
			Plan.Steps.Num() * FHyperAIStudioPlanLimits::MaxEffectsPerStep));
		for (const int32 StepIndex : Plan.OrderedStepIndices)
		{
			if (!Plan.Steps.IsValidIndex(StepIndex))
			{
				continue;
			}
			const FHyperAIStudioPlanStep& Step = Plan.Steps[StepIndex];
			for (const FHyperAIStudioPlanPrecondition& Source : Step.Preconditions)
			{
				FHyperAIPlanValidatePrecondition& Item = Report.Preconditions.AddDefaulted_GetRef();
				Item.StepId = Step.StepId;
				Item.Kind = PreconditionKindToString(Source.Kind);
				Item.Target = Source.Target;
				Item.Field = Source.Field;
				Item.Expected = Source.Expected;
			}
			for (const FHyperAIStudioPlanEffect& Source : Step.Effects)
			{
				FHyperAIPlanValidateEffect& Item = Report.Effects.AddDefaulted_GetRef();
				Item.StepId = Step.StepId;
				Item.Kind = EffectKindToString(Source.Kind);
				Item.Target = Source.Target;
				Item.ValidatorId = Source.ValidatorId;
				Item.Expected = Source.Expected;
			}
		}
	}
}

FHyperAIPlanValidateReport UHyperAIStudioPlanValidateToolset::hyper_plan_validate(
	const FString& PlanJson,
	const FString& ExpectedPlanHash)
{
	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(
		FHyperAIStudioService::GetProjectRoot());
	return FHyperAIStudioPlanValidateContracts::ValidateForProject(
		PlanJson,
		ExpectedPlanHash,
		CanonicalProjectId);
}

FString FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioPlanValidateToolset");
}

FString FHyperAIStudioPlanValidateContracts::GetSelectedBackend()
{
	return TEXT("hyperai.validation-only.typed-plan.foundation.v1");
}

FString FHyperAIStudioPlanValidateContracts::ComputeBoundedSha256(const FString& Value)
{
	return HyperAIStudio::PlanValidate::Private::HashUtf8Sha256(Value);
}

bool FHyperAIStudioPlanValidateContracts::IsPendingNativeToolsTestEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

FHyperAIPlanValidateReport FHyperAIStudioPlanValidateContracts::ValidateForProject(
	const FString& PlanJson,
	const FString& ExpectedPlanHash,
	const FString& CanonicalProjectId)
{
	using namespace HyperAIStudio::PlanValidate::Private;
	FHyperAIPlanValidateReport Report;
	Report.SelectedBackend = ClipText(GetSelectedBackend(), MaxBackendCharacters);
	Report.ProjectIdentityHash = CanonicalProjectId;
	Report.bExpectedPlanHashProvided = !ExpectedPlanHash.IsEmpty();

	const FFrozenValidationContext& Context = GetFrozenValidationContext();
	Report.CapabilityFingerprint = Context.CapabilityFingerprint;
	Report.CatalogFingerprint = Context.CatalogFingerprint;
	if (!Context.bCatalogValid)
	{
		Report.Status = TEXT("capability_catalog_invalid");
		Report.Diagnostic = TEXT("The immutable HyperAI capability catalog failed semantic validation.");
		for (const FString& Error : Context.CatalogErrors)
		{
			AddDiagnostic(Report, TEXT("capability_catalog_invalid"), TEXT("$.capability_catalog"), Error);
		}
		return Report;
	}

	if (!IsSha256Fingerprint(CanonicalProjectId))
	{
		Report.Status = TEXT("project_identity_unavailable");
		Report.Diagnostic = TEXT("A strong canonical project identity is required before plan validation.");
		AddDiagnostic(Report, Report.Status, TEXT("$.project"), Report.Diagnostic);
		return Report;
	}
	if (!IsSha256Fingerprint(Report.CapabilityFingerprint))
	{
		Report.Status = TEXT("capability_registry_invalid");
		Report.Diagnostic = TEXT("The closed typed-operation registry could not produce a stable fingerprint.");
		AddDiagnostic(Report, Report.Status, TEXT("$.capability_hash"), Report.Diagnostic);
		return Report;
	}
	if (!ExpectedPlanHash.IsEmpty() && !IsSha256Fingerprint(ExpectedPlanHash))
	{
		Report.Status = TEXT("invalid_expected_plan_hash");
		Report.Diagnostic = TEXT("expected_plan_hash must be an exact lowercase sha256 token when supplied.");
		AddDiagnostic(Report, Report.Status, TEXT("$.expected_plan_hash"), Report.Diagnostic);
		return Report;
	}

	FHyperAIStudioValidatedPlan Plan;
	FHyperAIStudioPlanDryRunResult DryRun;
	if (!FHyperAIStudioTypedPlanValidator::ValidateJson(PlanJson, Context.Registry, Plan, DryRun))
	{
		CopyValidatorDiagnostics(DryRun, Report);
		Report.Status = Report.Diagnostics.IsEmpty() ? TEXT("invalid_plan") : Report.Diagnostics[0].Code;
		Report.Diagnostic = Report.Diagnostics.IsEmpty()
			? TEXT("The strict typed plan was rejected.")
			: Report.Diagnostics[0].Message;
		return Report;
	}
	Report.bInputPlanDryRunKnown = true;
	Report.bInputPlanDryRun = Plan.bDryRun;
	Report.OperationId = Plan.OperationId;
	Report.CanonicalPlanHash = Plan.PlanHash;
	Report.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
	Report.EffectFingerprint = Plan.EffectFingerprint;
	Report.MaximumSafety = FHyperAIStudioTypedPlanValidator::SafetyToString(Plan.MaximumSafety);
	Report.bWouldMutate = DryRun.bWouldMutate;
	Report.bRequiresOperationId = DryRun.bRequiresOperationId;
	Report.bRequiresDestructiveAuthorization = DryRun.bRequiresDestructiveAuthorization;
	Report.bRequiresExternalEffectAuthorization = DryRun.bRequiresExternalEffectAuthorization;
	Report.bRequiresAuthorization = Report.bRequiresDestructiveAuthorization
		|| Report.bRequiresExternalEffectAuthorization;
	Report.StepCount = DryRun.StepCount;
	Report.MutationStepCount = DryRun.MutationStepCount;
	Report.Budget.DeadlineMs = Plan.Budget.DeadlineMs;
	Report.Budget.MaxSteps = Plan.Budget.MaxSteps;
	Report.Budget.MaxMutations = Plan.Budget.MaxMutations;
	Report.Budget.MaxNativeOperations = Plan.Budget.MaxNativeOperations;
	Report.Budget.MaxGameThreadMs = Plan.Budget.MaxGameThreadMs;
	Report.Budget.MaxOutputBytes = Plan.Budget.MaxOutputBytes;
	Report.Budget.PlannedNativeOperations = DryRun.PlannedNativeOperationBudget;
	Report.Budget.PlannedGameThreadMs = DryRun.PlannedGameThreadBudgetMs;
	Report.Budget.PlannedOutputBytes = DryRun.PlannedOutputBudgetBytes;
	PopulateValidatedDeclarations(Plan, DryRun, Report);
	CopyValidatorDiagnostics(DryRun, Report);
	Report.bExpectedPlanHashMatched = ExpectedPlanHash.IsEmpty() || ExpectedPlanHash == Plan.PlanHash;
	if (!ExpectedPlanHash.IsEmpty() && ExpectedPlanHash != Plan.PlanHash)
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("The separately supplied expected hash does not match the canonical plan hash.");
		AddDiagnostic(Report, Report.Status, TEXT("$.expected_plan_hash"), Report.Diagnostic);
		return Report;
	}

	Report.bOk = true;
	Report.Status = TEXT("valid");
	Report.Diagnostic = TEXT("The project-bound typed plan is valid; no execution, mutation, or authorization issuance occurred.");
	Report.ProjectBindingFingerprint = BuildProjectBindingFingerprint(
		CanonicalProjectId,
		Plan.OperationId,
		Plan.PlanHash,
		Plan.AuthorizationPlanHash,
		Plan.EffectFingerprint,
		Report.CapabilityFingerprint,
		Report.CatalogFingerprint,
		Report.SelectedBackend);
	if (!IsSha256Fingerprint(Report.ProjectBindingFingerprint))
	{
		Report.bOk = false;
		Report.Status = TEXT("project_binding_failure");
		Report.Diagnostic = TEXT("The platform could not compute the project-bound validation fingerprint.");
		AddDiagnostic(Report, Report.Status, TEXT("$.project_binding_fingerprint"), Report.Diagnostic);
	}
	return Report;
}

FString FHyperAIStudioPlanValidateContracts::BuildProjectBindingFingerprint(
	const FString& CanonicalProjectId,
	const FString& OperationId,
	const FString& PlanHash,
	const FString& AuthorizationPlanHash,
	const FString& EffectFingerprint,
	const FString& CapabilityFingerprint,
	const FString& CatalogFingerprint,
	const FString& SelectedBackend)
{
	using namespace HyperAIStudio::PlanValidate::Private;
	if (!IsSha256Fingerprint(CanonicalProjectId)
		|| !IsSha256Fingerprint(PlanHash)
		|| !IsSha256Fingerprint(AuthorizationPlanHash)
		|| !IsSha256Fingerprint(EffectFingerprint)
		|| !IsSha256Fingerprint(CapabilityFingerprint)
		|| !IsSha256Fingerprint(CatalogFingerprint)
		|| (!OperationId.IsEmpty() && !FHyperAIStudioOperationJournal::IsValidOperationId(OperationId))
		|| SelectedBackend.IsEmpty()
		|| SelectedBackend.Len() > MaxBackendCharacters)
	{
		return FString();
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.plan-validation-binding.v1"));
	AppendToken(Canonical, CanonicalProjectId);
	AppendToken(Canonical, OperationId);
	AppendToken(Canonical, PlanHash);
	AppendToken(Canonical, AuthorizationPlanHash);
	AppendToken(Canonical, EffectFingerprint);
	AppendToken(Canonical, CapabilityFingerprint);
	AppendToken(Canonical, CatalogFingerprint);
	AppendToken(Canonical, SelectedBackend);
	return HashUtf8Sha256(Canonical);
}

FHyperAIStudioPlanValidateAdmissionEvidence
FHyperAIStudioPlanValidateContracts::GetCompiledAdmissionEvidence()
{
	using namespace HyperAIStudio::PlanValidate::Private;
	FHyperAIStudioPlanValidateAdmissionEvidence Evidence;
	Evidence.ToolName = ToolName;
	Evidence.QualifiedToolsetName = GetQualifiedToolsetName();
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	const FFrozenValidationContext& Context = GetFrozenValidationContext();
	Evidence.CatalogFingerprint = Context.CatalogFingerprint;
	Evidence.CapabilityFingerprint = Context.CapabilityFingerprint;
	int32 MatchCount = 0;
	if (const FHyperAIStudioCapabilityToolDefinition* Tool = FindPlanValidateTool(Catalog, MatchCount))
	{
		Evidence.AdmissionState = ToPlanValidateAdmissionState(Tool->AdmissionState);
		Evidence.SourceArtifactCount = Tool->SourceArtifactCount;
		Evidence.SourceArtifactFingerprint = Tool->SourceArtifactFingerprint;
	}
	return Evidence;
}

bool FHyperAIStudioPlanValidateContracts::IsRegistrationEvidenceAllowed(
	const FHyperAIStudioPlanValidateAdmissionEvidence& Evidence,
	const FHyperAIStudioCapabilityToolDefinition& CatalogTool,
	const FString& CurrentCatalogFingerprint,
	const FString& CurrentCapabilityFingerprint,
	const bool bAllowSourceCandidateForDev)
{
	using namespace HyperAIStudio::PlanValidate::Private;
	if (Evidence.ToolName != ToolName
		|| Evidence.QualifiedToolsetName != GetQualifiedToolsetName()
		|| CatalogTool.Name != ToolName
		|| CatalogTool.PackId != PackId
		|| CatalogTool.bMayCauseExternalEffects
		|| !IsSha256Fingerprint(CurrentCatalogFingerprint)
		|| Evidence.CatalogFingerprint != CurrentCatalogFingerprint
		|| !IsSha256Fingerprint(CurrentCapabilityFingerprint)
		|| Evidence.CapabilityFingerprint != CurrentCapabilityFingerprint)
	{
		return false;
	}
	const EHyperAIStudioPlanValidateAdmissionState ExpectedAdmission =
		ToPlanValidateAdmissionState(CatalogTool.AdmissionState);
	if (Evidence.AdmissionState != ExpectedAdmission)
	{
		return false;
	}
	if (bAllowSourceCandidateForDev)
	{
		return Evidence.AdmissionState == EHyperAIStudioPlanValidateAdmissionState::SourceCandidate
			|| Evidence.AdmissionState == EHyperAIStudioPlanValidateAdmissionState::Admitted;
	}
	return Evidence.AdmissionState == EHyperAIStudioPlanValidateAdmissionState::Admitted
		&& CatalogTool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted
		&& Evidence.SourceArtifactCount > 0
		&& Evidence.SourceArtifactCount == CatalogTool.SourceArtifactCount
		&& IsSha256Fingerprint(Evidence.SourceArtifactFingerprint)
		&& Evidence.SourceArtifactFingerprint == CatalogTool.SourceArtifactFingerprint;
}

bool FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(
	const FHyperAIStudioPlanValidateAdmissionEvidence& Evidence,
	const bool bAllowSourceCandidateForDev)
{
	using namespace HyperAIStudio::PlanValidate::Private;
	const FFrozenValidationContext& Context = GetFrozenValidationContext();
	if (!Context.bCatalogValid)
	{
		return false;
	}
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	int32 MatchCount = 0;
	const FHyperAIStudioCapabilityToolDefinition* Tool = FindPlanValidateTool(Catalog, MatchCount);
	if (!Tool || MatchCount != 1)
	{
		return false;
	}
	return IsRegistrationEvidenceAllowed(
		Evidence,
		*Tool,
		Context.CatalogFingerprint,
		Context.CapabilityFingerprint,
		bAllowSourceCandidateForDev);
}

void FHyperAIStudioPlanValidateToolRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this,
		&FHyperAIStudioPlanValidateToolRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPlanValidateToolRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration
		&& IsInGameThread()
		&& UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPlanValidateToolset::StaticClass(),
			FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioPlanValidateTool, Warning,
				TEXT("Could not unregister the owned plan-validate toolset: %s"), *Error);
		}
	}
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioPlanValidateToolRegistration::IsRegistered() const
{
	if (!UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return false;
	}
	const FHyperAIStudioPlanValidateAdmissionEvidence Evidence =
		FHyperAIStudioPlanValidateContracts::GetCompiledAdmissionEvidence();
	return FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(
		Evidence,
		FHyperAIStudioPlanValidateContracts::IsPendingNativeToolsTestEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPlanValidateToolset::StaticClass(),
			FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioPlanValidateToolRegistration::RegisterAfterEngineInit()
{
	if (!bStarted
		|| !IsInGameThread()
		|| IsEngineExitRequested()
		|| !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	const FHyperAIStudioPlanValidateAdmissionEvidence Evidence =
		FHyperAIStudioPlanValidateContracts::GetCompiledAdmissionEvidence();
	if (!FHyperAIStudioPlanValidateContracts::IsRegistrationAllowed(
		Evidence,
		FHyperAIStudioPlanValidateContracts::IsPendingNativeToolsTestEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioPlanValidateToolset::StaticClass(),
		FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioPlanValidateToolset::StaticClass(),
			FHyperAIStudioPlanValidateContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioPlanValidateTool, Error,
				TEXT("ToolsetRegistry rejected the owned hyper_plan_validate SourceCandidate: %s"),
				*Error);
		}
	}
}
