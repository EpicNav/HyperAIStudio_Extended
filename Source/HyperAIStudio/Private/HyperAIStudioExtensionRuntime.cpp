// Games by Hyper 2026.

#include "HyperAIStudioExtensionRuntime.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioService.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

static_assert(FHyperAIStudioExtensionRuntime::MaxHashInputBytes
	== FHyperAIStudioPlanExecuteContracts::MaxHashInputBytes,
	"The public extension hash bound must remain identical to the core executor bound.");

namespace HyperAIStudio::ExtensionRuntime::Private
{
	bool HasLegacyPendingNativeToolsOverride()
	{
	#if WITH_DEV_AUTOMATION_TESTS
		return FParse::Param(FCommandLine::Get(), TEXT("HyperAIEnablePendingNativeTools"));
	#else
		return false;
	#endif
	}

	EHyperAIStudioExtensionAdmissionState TranslateAdmission(
		const EHyperAIStudioCapabilityAdmissionState State)
	{
		switch (State)
		{
		case EHyperAIStudioCapabilityAdmissionState::Planned:
			return EHyperAIStudioExtensionAdmissionState::Planned;
		case EHyperAIStudioCapabilityAdmissionState::SourceCandidate:
			return EHyperAIStudioExtensionAdmissionState::SourceCandidate;
		case EHyperAIStudioCapabilityAdmissionState::Admitted:
			return EHyperAIStudioExtensionAdmissionState::Admitted;
		default:
			return EHyperAIStudioExtensionAdmissionState::Invalid;
		}
	}
}

bool FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()
{
	// Compatibility for optional modules that still use the historical helper name.
	return AreSourceCandidateToolsEnabled();
}

EHyperAIStudioNativeToolChannel FHyperAIStudioExtensionRuntime::GetNativeToolChannel()
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return Settings ? Settings->NativeToolChannel : EHyperAIStudioNativeToolChannel::Preview;
}

bool FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled()
{
	return AreExtendedHyperToolsEnabled(
		GetNativeToolChannel(),
		HyperAIStudio::ExtensionRuntime::Private::HasLegacyPendingNativeToolsOverride());
}

bool FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled(
	const EHyperAIStudioNativeToolChannel Channel,
	const bool bLegacyDevAutomationOverride)
{
	return Channel == EHyperAIStudioNativeToolChannel::Preview || bLegacyDevAutomationOverride;
}

bool FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled()
{
	return AreExtendedHyperToolsEnabled();
}

bool FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled(
	const EHyperAIStudioNativeToolChannel Channel,
	const bool bLegacyDevAutomationOverride)
{
	return AreExtendedHyperToolsEnabled(Channel, bLegacyDevAutomationOverride);
}

EHyperAIStudioNativeExecutionMode FHyperAIStudioExtensionRuntime::GetNativeExecutionMode()
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return Settings ? Settings->NativeExecutionMode : EHyperAIStudioNativeExecutionMode::Fast;
}

bool FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
	const EHyperAIStudioDomainSafety Safety)
{
	return UsesStrictNativeSafety(GetNativeExecutionMode(), Safety);
}

bool FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
	const EHyperAIStudioNativeExecutionMode Mode,
	const EHyperAIStudioDomainSafety Safety)
{
	return Mode == EHyperAIStudioNativeExecutionMode::StrictSafety
		|| Safety == EHyperAIStudioDomainSafety::Destructive
		|| Safety == EHyperAIStudioDomainSafety::ExternalEffect;
}

bool FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
	const FString& PackId,
	const FString& AtomicCohortId,
	const TArray<FString>& ExactToolNames,
	FHyperAIStudioExtensionCohortAdmission& OutAdmission)
{
	using namespace HyperAIStudio::ExtensionRuntime::Private;
	OutAdmission = {};
	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		return false;
	}
	OutAdmission.bCatalogValid = true;
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	OutAdmission.CatalogFingerprint = Catalog.GeneratedFingerprint;
	if (PackId.IsEmpty() || AtomicCohortId.IsEmpty() || ExactToolNames.IsEmpty())
	{
		return false;
	}

	TSet<FString> ExpectedNames;
	for (const FString& Name : ExactToolNames)
	{
		if (Name.IsEmpty() || ExpectedNames.Contains(Name))
		{
			return false;
		}
		ExpectedNames.Add(Name);
	}

	const FHyperAIStudioCapabilityPackDefinition* Pack = nullptr;
	int32 PackMatches = 0;
	for (const FHyperAIStudioCapabilityPackDefinition& Candidate : Catalog.Packs)
	{
		if (Candidate.Id == PackId)
		{
			Pack = &Candidate;
			++PackMatches;
		}
	}
	int32 CohortIdMatches = 0;
	if (Pack)
	{
		for (const FString& CandidateCohortId : Pack->AtomicCohortIds)
		{
			CohortIdMatches += CandidateCohortId == AtomicCohortId ? 1 : 0;
		}
	}
	if (!Pack || PackMatches != 1 || CohortIdMatches != 1)
	{
		return false;
	}

	TSet<FString> ActualNames;
	TOptional<EHyperAIStudioCapabilityAdmissionState> CohortState;
	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		if (Tool.PackId != PackId || Tool.AtomicCohortId != AtomicCohortId)
		{
			continue;
		}
		if (ActualNames.Contains(Tool.Name))
		{
			return false;
		}
		ActualNames.Add(Tool.Name);
		if (!CohortState.IsSet())
		{
			CohortState = Tool.AdmissionState;
		}
		else if (CohortState.GetValue() != Tool.AdmissionState)
		{
			return false;
		}
	}
	if (!CohortState.IsSet() || ActualNames.Num() != ExpectedNames.Num()
		|| !ActualNames.Includes(ExpectedNames)
		|| !ExpectedNames.Includes(ActualNames)
		|| Pack->AdmissionState != CohortState.GetValue())
	{
		return false;
	}
	for (const FString& Name : ExactToolNames)
	{
		int32 PackNameMatches = 0;
		for (const FString& PackToolName : Pack->ToolNames)
		{
			PackNameMatches += PackToolName == Name ? 1 : 0;
		}
		if (PackNameMatches != 1)
		{
			return false;
		}
	}

	OutAdmission.bExactCohortMatch = true;
	OutAdmission.bOptionalPack = Pack->Tier == EHyperAIStudioCapabilityPackTier::Optional;
	OutAdmission.State = TranslateAdmission(CohortState.GetValue());
	return true;
}

bool FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
	const FString& PackId,
	const FString& AtomicCohortId,
	const TArray<FString>& ExactToolNames,
	const bool bAllowSourceCandidateForDev)
{
	if (!AreExtendedHyperToolsEnabled())
	{
		return false;
	}
	FHyperAIStudioExtensionCohortAdmission Admission;
	if (!QueryExactGeneratedCohort(PackId, AtomicCohortId, ExactToolNames, Admission)
		|| !Admission.bExactCohortMatch)
	{
		return false;
	}
	if (Admission.State == EHyperAIStudioExtensionAdmissionState::Admitted)
	{
		return true;
	}
	return Admission.State == EHyperAIStudioExtensionAdmissionState::SourceCandidate
		&& bAllowSourceCandidateForDev && AreSourceCandidateToolsEnabled();
}

FString FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(const FString& Value)
{
	return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Value);
}

bool FHyperAIStudioExtensionRuntime::IsValidOperationId(const FString& OperationId)
{
	return FHyperAIStudioOperationJournal::IsValidOperationId(OperationId);
}

FString FHyperAIStudioExtensionRuntime::GetCanonicalProjectId()
{
	return FHyperAIStudioOperationJournal::MakeCanonicalProjectId(
		FHyperAIStudioService::GetProjectRoot());
}
