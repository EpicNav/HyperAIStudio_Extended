// Games by Hyper 2026.

#include "HyperAIStudioPropertyAnimationToolset.h"

#include "Animators/PropertyAnimatorCoreBase.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioTrustedExecution.h"
#include "IO/IoHash.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Properties/PropertyAnimatorCoreContext.h"
#include "Properties/PropertyAnimatorCoreData.h"
#include "Subsystems/PropertyAnimatorCoreSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPropertyAnimation, Log, All);

namespace HyperAIStudio::PropertyAnimation::Private
{
	constexpr int64 BaseReportBytes = 4096;
	constexpr int64 AnimatorBytes = 640;
	constexpr int64 ContextBytes = 480;
	constexpr int64 IssueBytes = 512;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += LexToString(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("|");
	}

	FString BoolToken(const bool bValue) { return bValue ? TEXT("1") : TEXT("0"); }

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ')) return true;
		}
		return false;
	}

	FString HashCanonical(const FString& Canonical)
	{
		if (Canonical.Len() > 128 * 1024) return {};
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	void AddIssue(TArray<FHyperAIPropertyAnimationIssue>& Issues, const TCHAR* Code,
		const TCHAR* Severity, const FString& StableId, const TCHAR* Detail)
	{
		if (Issues.Num() >= FHyperAIStudioPropertyAnimationContracts::MaxIssues) return;
		FHyperAIPropertyAnimationIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(
			FHyperAIStudioPropertyAnimationContracts::MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(512);
	}

	bool IsSafeObjectPath(const FString& Path)
	{
		if (Path.IsEmpty()
			|| Path.Len() > FHyperAIStudioPropertyAnimationContracts::MaxPathCharacters
			|| !Path.StartsWith(TEXT("/Game/")) || Path.Contains(TEXT("\\"))
			|| Path.Contains(TEXT("..")) || HasControlCharacter(Path)) return false;
		const FSoftObjectPath SoftPath(Path);
		return SoftPath.IsValid()
			&& FPackageName::IsValidLongPackageName(SoftPath.GetLongPackageName());
	}

	bool IsSafeClassPath(const FString& Path)
	{
		return !Path.IsEmpty()
			&& Path.Len() <= FHyperAIStudioPropertyAnimationContracts::MaxPathCharacters
			&& (Path.StartsWith(TEXT("/Script/")) || Path.StartsWith(TEXT("/Game/")))
			&& !Path.Contains(TEXT("\\")) && !Path.Contains(TEXT(".."))
			&& !HasControlCharacter(Path);
	}

	FString ModeToken(const EPropertyAnimatorCoreMode Mode)
	{
		return Mode == EPropertyAnimatorCoreMode::Additive
			? TEXT("additive") : TEXT("absolute");
	}

	bool CaptureSnapshot(const FString& ActorPath, FHyperAIPropertyAnimationSnapshot& Out,
		TArray<FHyperAIPropertyAnimationIssue>& Issues)
	{
		Out = {};
		Out.ActorPath = ActorPath;
		const FSoftObjectPath SoftPath(ActorPath);
		Out.PackageName = SoftPath.GetLongPackageName();

		IAssetRegistry* Registry = IAssetRegistry::Get();
		FAssetPackageData PackageData;
		UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
		if (Registry)
		{
			PackageState = Registry->TryGetAssetPackageData(
				FName(*Out.PackageName), PackageData, /*bFailIfLockHeld=*/true);
		}
		Out.DiskExistence =
			FHyperAIStudioPropertyAnimationContracts::ClassifyAssetRegistryExistence(PackageState);
		if (!Registry || PackageState == UE::AssetRegistry::EExists::Unknown)
		{
			AddIssue(Issues, TEXT("package_state_unknown"), TEXT("error"), ActorPath,
				TEXT("The fail-fast Asset Registry query was unavailable or lock-contended; Unknown is not absence."));
		}
		else if (PackageState == UE::AssetRegistry::EExists::Exists)
		{
			Out.DiskSize = PackageData.DiskSize;
			Out.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
		}
		else
		{
			AddIssue(Issues, TEXT("package_missing"), TEXT("error"), ActorPath,
				TEXT("The exact actor package does not exist on disk."));
		}

		AActor* Actor = Cast<AActor>(SoftPath.ResolveObject());
		if (!Actor || Actor->GetPathName() != ActorPath)
		{
			AddIssue(Issues, TEXT("actor_not_loaded"), TEXT("error"), ActorPath,
				TEXT("The exact actor is not already loaded; this adapter never searches, scans, opens, or loads it."));
			Out.PersistedFingerprint =
				FHyperAIStudioPropertyAnimationContracts::ComputePersistedFingerprint(Out);
			Out.VolatileFingerprint =
				FHyperAIStudioPropertyAnimationContracts::ComputeVolatileFingerprint(Out);
			return false;
		}

		Out.bActorLoaded = true;
		Out.ActorClassPath = Actor->GetClass()->GetPathName();
		UPackage* Package = Actor->GetOutermost();
		Out.bWasLoadedFromDisk = Actor->HasAnyFlags(RF_WasLoaded)
			|| (Package && Package->HasAnyFlags(RF_WasLoaded));
		Out.bPackageDirty = !Package || Package->IsDirty();
		if (!Package || Package == GetTransientPackage()
			|| Package->GetName() != Out.PackageName
			|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor))
		{
			AddIssue(Issues, TEXT("actor_package_mismatch"), TEXT("error"), ActorPath,
				TEXT("The actor is not in the exact persistent project package asserted by the request."));
		}

		UPropertyAnimatorCoreSubsystem* Subsystem = UPropertyAnimatorCoreSubsystem::Get();
		if (!Subsystem)
		{
			AddIssue(Issues, TEXT("property_animator_subsystem_unavailable"), TEXT("error"),
				ActorPath, TEXT("PropertyAnimatorCore is loaded but its public engine subsystem is unavailable."));
		}
		else
		{
			TArray<UPropertyAnimatorCoreBase*> Animators =
				Subsystem->GetExistingAnimators(Actor).Array();
			Animators.Sort([](const UPropertyAnimatorCoreBase& A,
				const UPropertyAnimatorCoreBase& B)
			{
				return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
			});
			if (Animators.Num() > FHyperAIStudioPropertyAnimationContracts::MaxAnimators)
			{
				AddIssue(Issues, TEXT("animator_bound_exceeded"), TEXT("error"), ActorPath,
					TEXT("The actor's Property Animator count exceeds the fixed inspection bound."));
			}
			else
			{
				int32 TotalContexts = 0;
				bool bProjectionValid = true;
				Out.Animators.Reserve(Animators.Num());
				for (int32 Index = 0; Index < Animators.Num(); ++Index)
				{
					UPropertyAnimatorCoreBase* Animator = Animators[Index];
					if (!Animator)
					{
						AddIssue(Issues, TEXT("null_animator"), TEXT("error"), ActorPath,
							TEXT("The public subsystem returned a null animator."));
						bProjectionValid = false;
						Out.Animators.Reset();
						break;
					}
					FHyperAIPropertyAnimationAnimatorEntry& Entry =
						Out.Animators.AddDefaulted_GetRef();
					Entry.Index = Index;
					Entry.AnimatorPath = Animator->GetPathName();
					Entry.ClassPath = Animator->GetClass()->GetPathName();
					Entry.DisplayName = Animator->GetAnimatorDisplayName().ToString();
					Entry.bEnabled = Animator->GetAnimatorEnabled();
					Entry.bOverrideTimeSource = Animator->GetOverrideTimeSource();
					Entry.TimeSourceName = Animator->GetTimeSourceName().ToString();
					TArray<UPropertyAnimatorCoreContext*> Contexts(
						Animator->GetLinkedPropertiesContext());
					Contexts.Sort([](const UPropertyAnimatorCoreContext& A,
						const UPropertyAnimatorCoreContext& B)
					{
						return A.GetAnimatedProperty().GetLocatorPath().Compare(
							B.GetAnimatedProperty().GetLocatorPath(),
							ESearchCase::CaseSensitive) < 0;
					});
					if (Contexts.Num()
						> FHyperAIStudioPropertyAnimationContracts::MaxContextsPerAnimator
						|| TotalContexts + Contexts.Num()
							> FHyperAIStudioPropertyAnimationContracts::MaxTotalContexts)
					{
						AddIssue(Issues, TEXT("context_bound_exceeded"), TEXT("error"),
							Entry.AnimatorPath, TEXT("Linked-property contexts exceed the fixed DTO bound."));
						bProjectionValid = false;
						Out.Animators.Reset();
						break;
					}
					TotalContexts += Contexts.Num();
					for (UPropertyAnimatorCoreContext* Context : Contexts)
					{
						if (!Context)
						{
							AddIssue(Issues, TEXT("null_context"), TEXT("error"),
								Entry.AnimatorPath, TEXT("An animator contains a null linked-property context."));
							bProjectionValid = false;
							Entry.Contexts.Reset();
							break;
						}
						const FPropertyAnimatorCoreData& Data = Context->GetAnimatedProperty();
						FHyperAIPropertyAnimationContextEntry& ContextEntry =
							Entry.Contexts.AddDefaulted_GetRef();
						ContextEntry.LocatorPath = Data.GetLocatorPath();
						ContextEntry.DisplayName = Data.GetPropertyDisplayName();
						ContextEntry.bResolved = Data.IsResolved();
						ContextEntry.bAnimated = Context->IsAnimated();
						ContextEntry.Magnitude = Context->GetMagnitude();
						ContextEntry.TimeOffset = Context->GetTimeOffset();
						ContextEntry.Mode = ModeToken(Context->GetMode());
						if (!FHyperAIStudioPropertyAnimationContracts::IsSafeLocator(
								ContextEntry.LocatorPath)
							|| ContextEntry.DisplayName.Len()
								> FHyperAIStudioPropertyAnimationContracts::MaxNameCharacters)
						{
							AddIssue(Issues, TEXT("context_identity_unbounded"), TEXT("error"),
								Entry.AnimatorPath, TEXT("A linked property locator exceeds the closed DTO bounds."));
							bProjectionValid = false;
							Entry.Contexts.Reset();
							break;
						}
					}
					if (!IsSafeObjectPath(Entry.AnimatorPath)
						|| !IsSafeClassPath(Entry.ClassPath)
						|| Entry.DisplayName.Len()
							> FHyperAIStudioPropertyAnimationContracts::MaxNameCharacters
						|| Entry.TimeSourceName.Len()
							> FHyperAIStudioPropertyAnimationContracts::MaxNameCharacters)
					{
						AddIssue(Issues, TEXT("animator_identity_unbounded"), TEXT("error"),
							Entry.AnimatorPath, TEXT("An animator identity exceeds the closed DTO bounds."));
						bProjectionValid = false;
						Out.Animators.Reset();
						break;
					}
				}
				Out.bSemanticProjectionComplete = bProjectionValid
					&& Out.Animators.Num() == Animators.Num();
			}
		}

		const bool bDiskIdentity = PackageState == UE::AssetRegistry::EExists::Exists
			&& Out.DiskSize > 0 && !PackageData.GetPackageSavedHash().IsZero()
			&& !Out.PackageSavedHash.IsEmpty() && Out.bWasLoadedFromDisk
			&& !Out.bPackageDirty && Package && Package->GetName() == Out.PackageName;
		Out.bPersistedProjectionComplete = bDiskIdentity
			&& Out.bSemanticProjectionComplete;
		Out.PersistedFingerprint =
			FHyperAIStudioPropertyAnimationContracts::ComputePersistedFingerprint(Out);
		Out.VolatileFingerprint =
			FHyperAIStudioPropertyAnimationContracts::ComputeVolatileFingerprint(Out);
		return Out.bSemanticProjectionComplete;
	}

	FString ComputeAuthorityFingerprint(
		const TArray<FHyperAIPropertyAnimationAuthorityRow>& Rows)
	{
		FString Canonical(TEXT("hyperai.property-animation.authority.v1|native:278|python:598|matches:0|"));
		for (const FHyperAIPropertyAnimationAuthorityRow& Row : Rows)
		{
			AppendToken(Canonical, Row.Source);
			AppendToken(Canonical, Row.SourceId);
			AppendToken(Canonical, Row.Lifecycle);
			AppendToken(Canonical, Row.Disposition);
			AppendToken(Canonical, Row.HyperAIContract);
			AppendToken(Canonical, Row.RequiredPlugin);
		}
		return HashCanonical(Canonical);
	}
}

FString FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPropertyAnimation.HyperAIStudioPropertyAnimationToolset");
}

const TArray<FHyperAIStudioPropertyAnimationManifestEntry>&
FHyperAIStudioPropertyAnimationContracts::GetManifest()
{
	static const TArray<FHyperAIStudioPropertyAnimationManifestEntry> Manifest = {
		{TEXT("hyper_property_animation_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_property_animation_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_property_animation_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

const TArray<FHyperAIPropertyAnimationAuthorityRow>&
FHyperAIStudioPropertyAnimationContracts::GetAuthorityRows()
{
	static const TArray<FHyperAIPropertyAnimationAuthorityRow> Rows = {
		{TEXT("hyperai_requirement"), TEXT("capability.property_animation.controller"), TEXT("discover/edit"),
			TEXT("capability_gated"), TEXT("hyper_property_animation_inspect/apply_plan/validate"),
			TEXT("PropertyAnimator")}};
	return Rows;
}

FHyperAIPropertyAnimationCapabilityStatus
FHyperAIStudioPropertyAnimationContracts::GetCapabilityStatus()
{
	FHyperAIPropertyAnimationCapabilityStatus Status;
	Status.AuthorityFingerprint =
		HyperAIStudio::PropertyAnimation::Private::ComputeAuthorityFingerprint(
			GetAuthorityRows());
	Status.SupportedCases = {
		TEXT("one exact loaded actor's public PropertyAnimatorCore controllers and contexts"),
		TEXT("controller/time-source/property-context persisted and volatile seals"),
		TEXT("closed create/remove/link/unlink/state transaction preflight and detached validation")};
	Status.UnsupportedCases = {
		TEXT("unloaded actor search, map loading, arbitrary reflection, presets, files, or evaluation"),
		TEXT("mutation until the bounded journaled transaction/save backend is admitted")};
	Status.Remediation = TEXT("Enable PropertyAnimator, load and save the exact map actor, then inspect. Non-dry plans remain zero-effect until the shared bounded transaction/save executor is connected.");
	return Status;
}

bool FHyperAIStudioPropertyAnimationContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioPropertyAnimationManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3 || GetAuthorityRows().Num() != 1) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioPropertyAnimationManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioPropertyAnimationContracts::IsSafeActorPath(const FString& Path)
{
	return HyperAIStudio::PropertyAnimation::Private::IsSafeObjectPath(Path);
}

bool FHyperAIStudioPropertyAnimationContracts::IsSafeLocator(const FString& Locator)
{
	return !Locator.IsEmpty() && Locator.Len() <= MaxPathCharacters
		&& !Locator.Contains(TEXT("\r")) && !Locator.Contains(TEXT("\n"))
		&& !HyperAIStudio::PropertyAnimation::Private::HasControlCharacter(Locator);
}

bool FHyperAIStudioPropertyAnimationContracts::IsCanonicalSha256(const FString& Value)
{
	if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

bool FHyperAIStudioPropertyAnimationContracts::IsSafeOperationId(const FString& Value)
{
	return !Value.IsEmpty() && Value.Len() <= FHyperAIStudioDomainLimits::MaxOperationIdChars
		&& Value.TrimStartAndEnd() == Value
		&& !HyperAIStudio::PropertyAnimation::Private::HasControlCharacter(Value);
}

FString FHyperAIStudioPropertyAnimationContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	if (State == UE::AssetRegistry::EExists::Exists) return TEXT("exists");
	if (State == UE::AssetRegistry::EExists::DoesNotExist) return TEXT("does_not_exist");
	return TEXT("unknown");
}

FString FHyperAIStudioPropertyAnimationContracts::ComputePersistedFingerprint(
	const FHyperAIPropertyAnimationSnapshot& Snapshot)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FString Canonical(TEXT("hyperai.property-animation.persisted.v1|"));
	AppendToken(Canonical, Snapshot.ActorPath);
	AppendToken(Canonical, Snapshot.ActorClassPath);
	AppendToken(Canonical, Snapshot.PackageName);
	AppendToken(Canonical, Snapshot.DiskExistence);
	AppendToken(Canonical, Snapshot.PackageSavedHash);
	AppendToken(Canonical, LexToString(Snapshot.DiskSize));
	AppendToken(Canonical, BoolToken(Snapshot.bSemanticProjectionComplete));
	for (const FHyperAIPropertyAnimationAnimatorEntry& Animator : Snapshot.Animators)
	{
		AppendToken(Canonical, LexToString(Animator.Index));
		AppendToken(Canonical, Animator.AnimatorPath);
		AppendToken(Canonical, Animator.ClassPath);
		AppendToken(Canonical, Animator.DisplayName);
		AppendToken(Canonical, BoolToken(Animator.bEnabled));
		AppendToken(Canonical, BoolToken(Animator.bOverrideTimeSource));
		AppendToken(Canonical, Animator.TimeSourceName);
		for (const FHyperAIPropertyAnimationContextEntry& Context : Animator.Contexts)
		{
			AppendToken(Canonical, Context.LocatorPath);
			AppendToken(Canonical, BoolToken(Context.bAnimated));
			AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Context.Magnitude));
			AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Context.TimeOffset));
			AppendToken(Canonical, Context.Mode);
		}
	}
	return HashCanonical(Canonical);
}

FString FHyperAIStudioPropertyAnimationContracts::ComputeVolatileFingerprint(
	const FHyperAIPropertyAnimationSnapshot& Snapshot)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FString Canonical(TEXT("hyperai.property-animation.volatile.v1|"));
	AppendToken(Canonical, Snapshot.ActorPath);
	AppendToken(Canonical, BoolToken(Snapshot.bActorLoaded));
	AppendToken(Canonical, BoolToken(Snapshot.bWasLoadedFromDisk));
	AppendToken(Canonical, BoolToken(Snapshot.bPackageDirty));
	for (const FHyperAIPropertyAnimationAnimatorEntry& Animator : Snapshot.Animators)
	{
		AppendToken(Canonical, Animator.AnimatorPath);
		for (const FHyperAIPropertyAnimationContextEntry& Context : Animator.Contexts)
		{
			AppendToken(Canonical, Context.LocatorPath);
			AppendToken(Canonical, BoolToken(Context.bResolved));
		}
	}
	return HashCanonical(Canonical);
}

bool FHyperAIStudioPropertyAnimationContracts::ValidateOperations(
	const TArray<FHyperAIPropertyAnimationPlanOperation>& Operations,
	FString& OutFingerprint, FString& OutError)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	OutFingerprint.Reset();
	OutError.Reset();
	if (Operations.IsEmpty() || Operations.Num() > MaxOperations
		|| Operations.GetAllocatedSize() > MaxContainerBytes)
	{
		OutError = TEXT("One to 32 bounded operations are required.");
		return false;
	}
	FString Canonical(TEXT("hyperai.property-animation.operations.v1|"));
	TSet<FString> ExactRows;
	for (const FHyperAIPropertyAnimationPlanOperation& Operation : Operations)
	{
		if (Operation.Action.Len() > 32 || Operation.AnimatorPath.Len() > MaxPathCharacters
			|| Operation.AnimatorClassPath.Len() > MaxPathCharacters
			|| Operation.PropertyLocator.Len() > MaxPathCharacters
			|| Operation.Mode.Len() > 16 || !FMath::IsFinite(Operation.Magnitude)
			|| !FMath::IsFinite(Operation.TimeOffset)
			|| Operation.Magnitude < 0.0 || Operation.Magnitude > 1.0
			|| Operation.TimeOffset < -86400.0 || Operation.TimeOffset > 86400.0)
		{
			OutError = TEXT("An operation exceeds its scalar or numeric bounds.");
			return false;
		}
		const bool bCreate = Operation.Action == TEXT("create");
		const bool bProperty = Operation.Action == TEXT("link")
			|| Operation.Action == TEXT("unlink") || Operation.Action == TEXT("set_context");
		const bool bExisting = bProperty || Operation.Action == TEXT("remove")
			|| Operation.Action == TEXT("set_animator_enabled");
		if ((!bCreate && !bExisting)
			|| (bCreate && (!Operation.AnimatorPath.IsEmpty()
				|| !IsSafeClassPath(Operation.AnimatorClassPath)
				|| !Operation.PropertyLocator.IsEmpty()))
			|| (bExisting && (!IsSafeObjectPath(Operation.AnimatorPath)
				|| !Operation.AnimatorClassPath.IsEmpty()))
			|| (bProperty && !IsSafeLocator(Operation.PropertyLocator))
			|| (!bProperty && !Operation.PropertyLocator.IsEmpty())
			|| (Operation.Action == TEXT("set_context")
				&& Operation.Mode != TEXT("absolute")
				&& Operation.Mode != TEXT("additive")))
		{
			OutError = TEXT("Operation fields do not match the closed Property Animator vocabulary.");
			return false;
		}
		FString Row;
		AppendToken(Row, Operation.Action);
		AppendToken(Row, Operation.AnimatorPath);
		AppendToken(Row, Operation.AnimatorClassPath);
		AppendToken(Row, Operation.PropertyLocator);
		AppendToken(Row, BoolToken(Operation.bEnabled));
		AppendToken(Row, FString::Printf(TEXT("%.17g"), Operation.Magnitude));
		AppendToken(Row, FString::Printf(TEXT("%.17g"), Operation.TimeOffset));
		AppendToken(Row, Operation.Mode);
		if (ExactRows.Contains(Row))
		{
			OutError = TEXT("Duplicate exact operations are prohibited.");
			return false;
		}
		ExactRows.Add(Row);
		AppendToken(Canonical, Row);
	}
	OutFingerprint = HashCanonical(Canonical);
	return IsCanonicalSha256(OutFingerprint);
}

#define HYPER_SCHEMA_FN(Name, Literal) \
	FString FHyperAIStudioPropertyAnimationContracts::Name() \
	{ static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT(Literal)); return Value; }

HYPER_SCHEMA_FN(InspectPayloadSchemaFingerprint,
	"property-animation.inspect-request.v1|actor_path|deadline|max_output")
HYPER_SCHEMA_FN(ApplyPayloadSchemaFingerprint,
	"property-animation.plan-request.v1|operation|actor|persisted_cas|closed_operations|deadline|max_output")
HYPER_SCHEMA_FN(ValidatePayloadSchemaFingerprint,
	"property-animation.validate-request.v1|detached_snapshot|issue_bound|deadline|max_output")
HYPER_SCHEMA_FN(InspectResultSchemaFingerprint,
	"property-animation.inspect-result.v1|capability|animators|contexts|persisted|volatile|issues")
HYPER_SCHEMA_FN(ApplyResultSchemaFingerprint,
	"property-animation.apply-blocked-result.v1|prepared|zero_effect|hashes|effects|issues")
HYPER_SCHEMA_FN(ValidateResultSchemaFingerprint,
	"property-animation.validate-result.v1|valid|complete|recomputed_hashes|issues")

#undef HYPER_SCHEMA_FN

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPropertyAnimationContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.property-animation.public-core.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// The generated modifier_plugin group is blocking; it is not a non-blocking hint.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_property_animation_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_property_animation_apply_plan"), ApplyVariantId,
			ApplyPayloadTypeId, ApplyPayloadSchemaFingerprint(), ApplyResultTypeId,
			ApplyResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_property_animation_validate"), ValidateVariantId,
			ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(), ValidateResultTypeId,
			ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIPropertyAnimationInspectReport
FHyperAIStudioPropertyAnimationContracts::Inspect(
	const FHyperAIPropertyAnimationInspectRequest& Request)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FHyperAIPropertyAnimationInspectReport Report;
	Report.Capability = GetCapabilityStatus();
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.ActorPath.Len() > MaxPathCharacters
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_envelope"),
			TEXT("Actor path, deadline, or output bounds are invalid."));
	}
	if (Request.ActorPath.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("capability_only");
		Report.Diagnostic = TEXT("Returned frozen source/Epic-review authority only; no UObject, subsystem, property, or Asset Registry state was accessed.");
		return Report;
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Exact loaded PropertyAnimatorCore inspection is game-thread only."));
	}
	if (!IsSafeActorPath(Request.ActorPath))
	{
		return Reject(TEXT("invalid_actor_path"),
			TEXT("ActorPath must be one bounded canonical /Game soft object path."));
	}
	const int64 WorstCase = BaseReportBytes + MaxAnimators * AnimatorBytes
		+ MaxTotalContexts * ContextBytes + 8 * IssueBytes;
	if (WorstCase > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("MaxOutputBytes cannot hold the bounded animator/context report."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	const bool bSemanticComplete = CaptureSnapshot(
		Request.ActorPath, Report.Snapshot, Report.Issues);
	int32 ActualContextCount = 0;
	for (const FHyperAIPropertyAnimationAnimatorEntry& Animator : Report.Snapshot.Animators)
	{
		ActualContextCount += Animator.Contexts.Num();
	}
	const int64 ActualBytes = BaseReportBytes
		+ Report.Snapshot.Animators.Num() * AnimatorBytes
		+ ActualContextCount * ContextBytes + Report.Issues.Num() * IssueBytes;
	if (ActualBytes > Request.MaxOutputBytes)
	{
		Report.Snapshot = {};
		Report.Issues.Reset();
		return Reject(TEXT("actual_output_bound"),
			TEXT("The bounded loaded snapshot does not fit MaxOutputBytes."));
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("inspect_deadline_exceeded"),
			TEXT("Loaded-only PropertyAnimatorCore capture exceeded its monotonic deadline."));
	}
	Report.bOk = bSemanticComplete;
	Report.Status = bSemanticComplete ? TEXT("loaded_snapshot") : TEXT("snapshot_incomplete");
	Report.Diagnostic = bSemanticComplete
		? TEXT("Captured one exact loaded actor through Epic's public PropertyAnimatorCore subsystem; no search, load, arbitrary reflection, evaluation, or mutation ran.")
		: TEXT("Loaded-only capture was incomplete; issues explain missing package, actor, subsystem, or bounded semantic evidence.");
	return Report;
}

FHyperAIPropertyAnimationValidateReport
FHyperAIStudioPropertyAnimationContracts::Validate(
	const FHyperAIPropertyAnimationValidateRequest& Request)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FHyperAIPropertyAnimationValidateReport Report;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Snapshot.Animators.Num() > MaxAnimators
		|| Request.Snapshot.Animators.GetAllocatedSize() > MaxContainerBytes)
	{
		return Reject(TEXT("invalid_validation_envelope"),
			TEXT("Detached container, issue, deadline, or output bounds are invalid."));
	}
	if (BaseReportBytes + static_cast<int64>(Request.MaxIssues) * IssueBytes
		> Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("MaxOutputBytes cannot hold the requested detached issue envelope."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	bool bShape = IsSafeActorPath(Request.Snapshot.ActorPath)
		&& IsSafeClassPath(Request.Snapshot.ActorClassPath)
		&& Request.Snapshot.PackageName
			== FSoftObjectPath(Request.Snapshot.ActorPath).GetLongPackageName()
		&& (Request.Snapshot.DiskExistence == TEXT("exists")
			|| Request.Snapshot.DiskExistence == TEXT("does_not_exist")
			|| Request.Snapshot.DiskExistence == TEXT("unknown"));
	int32 TotalContexts = 0;
	FString PreviousAnimator;
	for (int32 Index = 0; Index < Request.Snapshot.Animators.Num(); ++Index)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			return Reject(TEXT("validation_deadline_exceeded"),
				TEXT("Detached validation exceeded its monotonic deadline."));
		}
		const FHyperAIPropertyAnimationAnimatorEntry& Animator =
			Request.Snapshot.Animators[Index];
		bool bEntry = Animator.Index == Index && IsSafeObjectPath(Animator.AnimatorPath)
			&& IsSafeClassPath(Animator.ClassPath)
			&& Animator.DisplayName.Len() <= MaxNameCharacters
			&& Animator.TimeSourceName.Len() <= MaxNameCharacters
			&& (PreviousAnimator.IsEmpty()
				|| Animator.AnimatorPath.Compare(PreviousAnimator, ESearchCase::CaseSensitive) > 0)
			&& Animator.Contexts.Num() <= MaxContextsPerAnimator;
		PreviousAnimator = Animator.AnimatorPath;
		TotalContexts += Animator.Contexts.Num();
		FString PreviousLocator;
		for (const FHyperAIPropertyAnimationContextEntry& Context : Animator.Contexts)
		{
			bEntry = bEntry && IsSafeLocator(Context.LocatorPath)
				&& Context.DisplayName.Len() <= MaxNameCharacters
				&& FMath::IsFinite(Context.Magnitude) && Context.Magnitude >= 0.0
				&& Context.Magnitude <= 1.0 && FMath::IsFinite(Context.TimeOffset)
				&& Context.TimeOffset >= -86400.0 && Context.TimeOffset <= 86400.0
				&& (Context.Mode == TEXT("absolute") || Context.Mode == TEXT("additive"))
				&& (PreviousLocator.IsEmpty()
					|| Context.LocatorPath.Compare(PreviousLocator, ESearchCase::CaseSensitive) > 0);
			PreviousLocator = Context.LocatorPath;
		}
		if (!bEntry)
		{
			bShape = false;
			AddIssue(Report.Issues, TEXT("invalid_animator_entry"), TEXT("error"),
				Animator.AnimatorPath, TEXT("Detached animator/context identity, ordering, state, or bounds are invalid."));
		}
	}
	if (TotalContexts > MaxTotalContexts)
	{
		bShape = false;
		AddIssue(Report.Issues, TEXT("total_context_bound_exceeded"), TEXT("error"),
			Request.Snapshot.ActorPath, TEXT("Detached linked-property context count exceeds the hard total bound."));
	}
	const bool bExpectedPersistedComplete = Request.Snapshot.DiskExistence == TEXT("exists")
		&& Request.Snapshot.DiskSize > 0 && !Request.Snapshot.PackageSavedHash.IsEmpty()
		&& Request.Snapshot.bActorLoaded && Request.Snapshot.bWasLoadedFromDisk
		&& !Request.Snapshot.bPackageDirty && Request.Snapshot.bSemanticProjectionComplete;
	if (Request.Snapshot.bPersistedProjectionComplete != bExpectedPersistedComplete)
	{
		bShape = false;
		AddIssue(Report.Issues, TEXT("persisted_completeness_mismatch"), TEXT("error"),
			Request.Snapshot.ActorPath, TEXT("Persisted completeness does not follow from detached evidence."));
	}
	Report.RecomputedPersistedFingerprint = ComputePersistedFingerprint(Request.Snapshot);
	Report.RecomputedVolatileFingerprint = ComputeVolatileFingerprint(Request.Snapshot);
	if (Report.RecomputedPersistedFingerprint != Request.Snapshot.PersistedFingerprint
		|| Report.RecomputedVolatileFingerprint != Request.Snapshot.VolatileFingerprint)
	{
		bShape = false;
		AddIssue(Report.Issues, TEXT("fingerprint_mismatch"), TEXT("error"),
			Request.Snapshot.ActorPath, TEXT("Detached persisted or volatile fingerprint does not recompute."));
	}
	Report.ErrorCount = Report.Issues.Num();
	if (Report.Issues.Num() > Request.MaxIssues)
	{
		Report.Issues.SetNum(Request.MaxIssues, EAllowShrinking::No);
	}
	Report.bOk = true;
	Report.bValid = bShape && Report.ErrorCount == 0;
	Report.bComplete = Report.bValid && Request.Snapshot.bPersistedProjectionComplete;
	FString Canonical(TEXT("hyperai.property-animation.validator.v1|"));
	AppendToken(Canonical, Report.RecomputedPersistedFingerprint);
	AppendToken(Canonical, Report.RecomputedVolatileFingerprint);
	AppendToken(Canonical, BoolToken(Report.bValid));
	Report.ValidatorFingerprint = HashCanonical(Canonical);
	Report.Status = Report.bValid
		? (Report.bComplete ? TEXT("valid") : TEXT("valid_partial")) : TEXT("invalid");
	Report.Diagnostic = TEXT("Recomputed the detached Property Animation snapshot only; no UObject, subsystem, property, Asset Registry, or editor state was accessed.");
	return Report;
}

FString FHyperAIStudioPropertyAnimationContracts::ComputePlanSemanticFingerprint(
	const FHyperAIStudioPropertyAnimationPlanPayload& Payload)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FString Canonical(TEXT("hyperai.property-animation.plan-semantic.v1|"));
	AppendToken(Canonical, Payload.ActorPath);
	AppendToken(Canonical, Payload.BasePersistedFingerprint);
	AppendToken(Canonical, Payload.OperationFingerprint);
	return HashCanonical(Canonical);
}

FHyperAIPropertyAnimationApplyPlanReport
FHyperAIStudioPropertyAnimationContracts::BuildPlan(
	const FHyperAIPropertyAnimationApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::PropertyAnimation::Private;
	FHyperAIPropertyAnimationApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	Report.VariantId = ApplyVariantId;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsSafeOperationId(Request.OperationId) || !IsSafeActorPath(Request.ActorPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedFingerprint)
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_envelope"),
			TEXT("Operation id, actor, persisted CAS, deadline, or output bounds are invalid."));
	}
	if ((Request.bDryRun && !Request.ExpectedPlanHash.IsEmpty())
		|| (!Request.bDryRun && !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_plan_hash_contract"),
			TEXT("Dry-run takes no expected plan hash; non-dry intent must echo one canonical prior hash."));
	}
	if (!ValidateOperations(Request.Operations, Report.OperationFingerprint,
		Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_operation_model");
		return Report;
	}
	Report.Effects.OperationCount = Request.Operations.Num();
	if (!Request.bDryRun)
	{
		return Reject(NonDryCallableState,
			TEXT("No effect ran. PropertyAnimatorCore edits require one admitted journaled transaction, bounded rollback, one save, and fresh validation; this module did not create, remove, link, configure, transact, save, evaluate, or stage anything."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Loaded semantic PropertyAnimatorCore preflight is game-thread only."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	FHyperAIPropertyAnimationSnapshot Snapshot;
	if (!CaptureSnapshot(Request.ActorPath, Snapshot, Report.Issues)
		|| !Snapshot.bPersistedProjectionComplete)
	{
		return Reject(TEXT("persisted_semantic_cas_incomplete"),
			TEXT("Pure preflight requires one exact loaded, clean, disk-backed actor and complete animator/context projection."));
	}
	Report.BasePersistedFingerprint = Snapshot.PersistedFingerprint;
	if (Snapshot.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("persisted_semantic_cas_mismatch"),
			TEXT("The fresh actor/animator persisted fingerprint differs from the caller assertion."));
	}

	AActor* Actor = Cast<AActor>(FSoftObjectPath(Request.ActorPath).ResolveObject());
	UPropertyAnimatorCoreSubsystem* Subsystem = UPropertyAnimatorCoreSubsystem::Get();
	if (!Actor || !Subsystem)
	{
		return Reject(TEXT("fresh_semantic_source_unavailable"),
			TEXT("The exact loaded actor or public PropertyAnimatorCore subsystem became unavailable."));
	}
	TMap<FString, UPropertyAnimatorCoreBase*> Existing;
	for (UPropertyAnimatorCoreBase* Animator : Subsystem->GetExistingAnimators(Actor))
	{
		if (Animator) Existing.Add(Animator->GetPathName(), Animator);
	}
	for (const FHyperAIPropertyAnimationPlanOperation& Operation : Request.Operations)
	{
		if (Operation.Action == TEXT("create"))
		{
			UClass* AnimatorClass = FSoftClassPath(Operation.AnimatorClassPath).ResolveClass();
			if (!AnimatorClass || !Subsystem->IsAnimatorClassRegistered(AnimatorClass))
			{
				return Reject(TEXT("animator_class_not_registered"),
					TEXT("A create operation names a class not registered by Epic's public subsystem."));
			}
			continue;
		}
		UPropertyAnimatorCoreBase* const* AnimatorPtr = Existing.Find(Operation.AnimatorPath);
		if (!AnimatorPtr || !*AnimatorPtr)
		{
			return Reject(TEXT("animator_cas_missing"),
				TEXT("An existing-operation animator path is absent from fresh loaded state."));
		}
		UPropertyAnimatorCoreBase* Animator = *AnimatorPtr;
		if (Operation.Action == TEXT("set_animator_enabled")
			&& Animator->GetAnimatorEnabled() == Operation.bEnabled)
		{
			return Reject(TEXT("no_op_operation"),
				TEXT("A set_animator_enabled operation already matches fresh state."));
		}
		if (Operation.Action == TEXT("link") || Operation.Action == TEXT("unlink")
			|| Operation.Action == TEXT("set_context"))
		{
			const FPropertyAnimatorCoreData Data(Actor, Operation.PropertyLocator);
			const bool bLinked = Animator->IsPropertyLinked(Data);
			if (!Data.IsResolved())
			{
				return Reject(TEXT("property_locator_unresolved"),
					TEXT("An exact property locator could not be resolved on the loaded actor."));
			}
			if (Operation.Action == TEXT("link"))
			{
				if (bLinked || Animator->GetPropertySupport(Data)
					== EPropertyAnimatorPropertySupport::None)
				{
					return Reject(TEXT("property_link_not_applicable"),
						TEXT("The property is already linked or unsupported by the exact animator."));
				}
			}
			else if (!bLinked)
			{
				return Reject(TEXT("property_context_cas_missing"),
					TEXT("The unlink/set_context property is not linked to the exact animator."));
			}
		}
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("preflight_deadline_exceeded"),
			TEXT("Loaded PropertyAnimatorCore preflight exceeded its monotonic deadline."));
	}

	const TSharedRef<FHyperAIStudioPropertyAnimationPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioPropertyAnimationPlanPayload, ESPMode::ThreadSafe>();
	Payload->ActorPath = Request.ActorPath;
	Payload->BasePersistedFingerprint = Snapshot.PersistedFingerprint;
	Payload->Operations = Request.Operations;
	Payload->OperationFingerprint = Report.OperationFingerprint;
	Payload->SemanticFingerprint = ComputePlanSemanticFingerprint(*Payload);
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || Clone->GetSemanticFingerprint()
		!= Payload->SemanticFingerprint || !IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("immutable_payload_seal_failed"),
			TEXT("The closed Property Animation operation payload could not be independently sealed."));
	}

	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_property_animation_apply_plan");
	Binding.VariantId = ApplyVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	Binding.ExpectedAdapterFingerprint = GetAdapterDescriptor().AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = false;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = MoveTemp(Binding);
	Contract.ArtifactTypeId = Clone->GetTypeId();
	Contract.ArtifactSchemaFingerprint = Clone->GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Clone->GetSemanticFingerprint();
	Contract.EffectTarget = Request.ActorPath;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = Request.Operations.Num() + 4;
	Contract.MaxGameThreadMs = FMath::Min(Request.DeadlineMs, 200);
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 512;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), *PrepareError);
	}
	Report.bOk = true;
	Report.bTypedPrepared = true;
	Report.Status = TEXT("dry_run_valid_zero_effect");
	Report.Diagnostic = TEXT("Pure Prepare sealed one loaded PropertyAnimatorCore transaction/save intent; no editor effect, stage, evaluation, transaction, or save ran.");
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.bLoadedSemanticCasComplete = true;
	Report.Effects.bWouldUseOneTransaction = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	return Report;
}

FString FHyperAIStudioPropertyAnimationInspectPayload::GetTypeId() const
{
	return FHyperAIStudioPropertyAnimationContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioPropertyAnimationInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPropertyAnimationContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPropertyAnimationInspectPayload::GetBoundedByteSize() const
{
	return 128 + 2 * Request.ActorPath.Len();
}

FString FHyperAIStudioPropertyAnimationValidatePayload::GetTypeId() const
{
	return FHyperAIStudioPropertyAnimationContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioPropertyAnimationValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPropertyAnimationContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioPropertyAnimationValidatePayload::GetBoundedByteSize() const
{
	int64 Size = 512;
	for (const FHyperAIPropertyAnimationAnimatorEntry& Animator : Request.Snapshot.Animators)
	{
		Size += 640 + Animator.Contexts.Num() * 480ll;
	}
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

FString FHyperAIStudioPropertyAnimationPlanPayload::GetTypeId() const
{
	return FHyperAIStudioPropertyAnimationContracts::ApplyPayloadTypeId;
}

FString FHyperAIStudioPropertyAnimationPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPropertyAnimationContracts::ApplyPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPropertyAnimationPlanPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 512 + Operations.Num() * 1024);
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioPropertyAnimationPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioPropertyAnimationPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioPropertyAnimationPlanPayload, ESPMode::ThreadSafe>();
	Clone->ActorPath = ActorPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->Operations = Operations;
	Clone->OperationFingerprint = OperationFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioPropertyAnimationInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioPropertyAnimationContracts::InspectResultTypeId;
}

FString FHyperAIStudioPropertyAnimationInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPropertyAnimationContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioPropertyAnimationInspectResultPayload::GetBoundedByteSize() const
{
	int64 Size = 4096 + Report.Issues.Num() * 512ll;
	for (const FHyperAIPropertyAnimationAnimatorEntry& Animator : Report.Snapshot.Animators)
	{
		Size += 640 + Animator.Contexts.Num() * 480ll;
	}
	return static_cast<int32>(FMath::Min<int64>(MAX_int32, Size));
}

FString FHyperAIStudioPropertyAnimationValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioPropertyAnimationContracts::ValidateResultTypeId;
}

FString FHyperAIStudioPropertyAnimationValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPropertyAnimationContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioPropertyAnimationValidateResultPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 4096 + Report.Issues.Num() * 512);
}

FHyperAIPropertyAnimationInspectReport
UHyperAIStudioPropertyAnimationToolset::hyper_property_animation_inspect(
	const FHyperAIPropertyAnimationInspectRequest& Request)
{
	return FHyperAIStudioPropertyAnimationContracts::Inspect(Request);
}

FHyperAIPropertyAnimationApplyPlanReport
UHyperAIStudioPropertyAnimationToolset::hyper_property_animation_apply_plan(
	const FHyperAIPropertyAnimationApplyPlanRequest& Request)
{
	return FHyperAIStudioPropertyAnimationContracts::BuildPlan(Request);
}

FHyperAIPropertyAnimationValidateReport
UHyperAIStudioPropertyAnimationToolset::hyper_property_animation_validate(
	const FHyperAIPropertyAnimationValidateRequest& Request)
{
	return FHyperAIStudioPropertyAnimationContracts::Validate(Request);
}

FHyperAIStudioPropertyAnimationDomainAdapter::FHyperAIStudioPropertyAnimationDomainAdapter()
	: Descriptor(FHyperAIStudioPropertyAnimationContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPropertyAnimationDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioPropertyAnimationDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&Result](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Property Animation adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_property_animation_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioPropertyAnimationContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPropertyAnimationContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPropertyAnimationContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPropertyAnimationInspectPayload& Typed =
			static_cast<const FHyperAIStudioPropertyAnimationInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioPropertyAnimationInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPropertyAnimationInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPropertyAnimationContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_property_animation_validate")
		&& Context.Binding.VariantId == FHyperAIStudioPropertyAnimationContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPropertyAnimationContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPropertyAnimationContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioPropertyAnimationValidatePayload& Typed =
			static_cast<const FHyperAIStudioPropertyAnimationValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioPropertyAnimationValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPropertyAnimationValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPropertyAnimationContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_property_animation_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioPropertyAnimationContracts::ApplyVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioPropertyAnimationContracts::ApplyPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPropertyAnimationContracts::ApplyPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPropertyAnimationPlanPayload& Typed =
			static_cast<const FHyperAIStudioPropertyAnimationPlanPayload&>(Payload);
		FString RecomputedOperations;
		FString Error;
		if (!FHyperAIStudioPropertyAnimationContracts::ValidateOperations(
			Typed.Operations, RecomputedOperations, Error)
			|| RecomputedOperations != Typed.OperationFingerprint
			|| FHyperAIStudioPropertyAnimationContracts::ComputePlanSemanticFingerprint(Typed)
				!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Property Animation operation model or semantic seal drifted."));
		}
		return Reject(FHyperAIStudioPropertyAnimationContracts::NonDryCallableState,
			TEXT("No effect ran; the admitted bounded transaction/save backend is not connected."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Property Animation adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioPropertyAnimationRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPropertyAnimationRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioPropertyAnimationRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioPropertyAnimationRegistration::IsRegistered() const
{
	return FHyperAIStudioPropertyAnimationContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPropertyAnimationToolset::StaticClass(),
			FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioPropertyAnimationRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid();
}

void FHyperAIStudioPropertyAnimationRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable()) return;
	if (!FHyperAIStudioPropertyAnimationContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioPropertyAnimation, Verbose,
			TEXT("Property Animation exact source cohort remains catalog fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioPropertyAnimationDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPropertyAnimation, Error,
			TEXT("Property Animation adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioPropertyAnimationToolset::StaticClass(),
		FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPropertyAnimation, Error,
			TEXT("Property Animation atomic registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioPropertyAnimationRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPropertyAnimationToolset::StaticClass(),
			FHyperAIStudioPropertyAnimationContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPropertyAnimation, Error,
				TEXT("Property Animation owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioPropertyAnimation, Error,
				TEXT("Property Animation adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
	}
	Adapter.Reset();
}
