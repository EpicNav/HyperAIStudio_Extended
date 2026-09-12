// Games by Hyper 2026.

#include "HyperAIStudioActorModifierToolset.h"

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
#include "Modifiers/ActorModifierCoreBase.h"
#include "Modifiers/ActorModifierCoreStack.h"
#include "Subsystems/ActorModifierCoreSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioActorModifier, Log, All);

namespace HyperAIStudio::ActorModifier::Private
{
	constexpr int64 BaseReportBytes = 4096;
	constexpr int64 EntryBytes = 640;
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
		if (Canonical.Len() > 64 * 1024) return {};
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	void AddIssue(TArray<FHyperAIActorModifierIssue>& Issues, const TCHAR* Code,
		const TCHAR* Severity, const FString& StableId, const TCHAR* Detail)
	{
		if (Issues.Num() >= FHyperAIStudioActorModifierContracts::MaxIssues) return;
		FHyperAIActorModifierIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.StableId = StableId.Left(
			FHyperAIStudioActorModifierContracts::MaxPathCharacters);
		Issue.Detail = FString(Detail).Left(512);
	}

	bool IsSafeObjectPath(const FString& Path)
	{
		if (Path.IsEmpty()
			|| Path.Len() > FHyperAIStudioActorModifierContracts::MaxPathCharacters
			|| !Path.StartsWith(TEXT("/Game/")) || Path.Contains(TEXT("\\"))
			|| Path.Contains(TEXT("..")) || HasControlCharacter(Path)) return false;
		const FSoftObjectPath SoftPath(Path);
		return SoftPath.IsValid()
			&& FPackageName::IsValidLongPackageName(SoftPath.GetLongPackageName());
	}

	bool CaptureSnapshot(const FString& ActorPath, FHyperAIActorModifierSnapshot& Out,
		TArray<FHyperAIActorModifierIssue>& Issues)
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
			FHyperAIStudioActorModifierContracts::ClassifyAssetRegistryExistence(PackageState);
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
				FHyperAIStudioActorModifierContracts::ComputePersistedFingerprint(Out);
			Out.VolatileFingerprint =
				FHyperAIStudioActorModifierContracts::ComputeVolatileFingerprint(Out);
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

		UActorModifierCoreSubsystem* Subsystem = UActorModifierCoreSubsystem::Get();
		if (!Subsystem)
		{
			AddIssue(Issues, TEXT("actor_modifier_subsystem_unavailable"), TEXT("error"),
				ActorPath, TEXT("ActorModifierCore is loaded but its public engine subsystem is unavailable."));
		}
		else if (UActorModifierCoreStack* Stack = Subsystem->GetActorModifierStack(Actor))
		{
			Out.bHasStack = true;
			Out.bStackFrozen = Stack->IsModifierStackFrozen();
			const TConstArrayView<UActorModifierCoreBase*> Modifiers = Stack->GetModifiers();
			if (Modifiers.Num() > FHyperAIStudioActorModifierContracts::MaxModifiers)
			{
				AddIssue(Issues, TEXT("modifier_bound_exceeded"), TEXT("error"), ActorPath,
					TEXT("The direct modifier stack exceeds the fixed inspection bound."));
			}
			else
			{
				Out.Modifiers.Reserve(Modifiers.Num());
				for (int32 Index = 0; Index < Modifiers.Num(); ++Index)
				{
					const UActorModifierCoreBase* Modifier = Modifiers[Index];
					if (!Modifier)
					{
						AddIssue(Issues, TEXT("null_modifier"), TEXT("error"), ActorPath,
							TEXT("The direct stack contains a null modifier entry."));
						Out.Modifiers.Reset();
						break;
					}
					FHyperAIActorModifierEntry& Entry = Out.Modifiers.AddDefaulted_GetRef();
					Entry.Index = Index;
					Entry.ModifierPath = Modifier->GetPathName();
					Entry.ModifierName = Modifier->GetModifierName().ToString();
					Entry.ClassPath = Modifier->GetClass()->GetPathName();
					Entry.bEnabled = Modifier->IsModifierEnabled();
					Entry.bApplied = Modifier->IsModifierApplied();
					Entry.bIdle = Modifier->IsModifierIdle();
					if (!IsSafeObjectPath(Entry.ModifierPath)
						|| Entry.ModifierName.IsEmpty()
						|| Entry.ModifierName.Len()
							> FHyperAIStudioActorModifierContracts::MaxNameCharacters
						|| Entry.ClassPath.Len()
							> FHyperAIStudioActorModifierContracts::MaxPathCharacters)
					{
						AddIssue(Issues, TEXT("modifier_identity_unbounded"), TEXT("error"),
							Entry.ModifierPath, TEXT("A direct modifier identity exceeds the closed DTO bounds."));
						Out.Modifiers.Reset();
						break;
					}
				}
				Out.bSemanticProjectionComplete =
					Out.Modifiers.Num() == Modifiers.Num();
			}
		}
		else
		{
			// The public subsystem authoritatively reports an empty stack for this actor.
			Out.bSemanticProjectionComplete = true;
		}

		const bool bDiskIdentity = PackageState == UE::AssetRegistry::EExists::Exists
			&& Out.DiskSize > 0 && !PackageData.GetPackageSavedHash().IsZero()
			&& !Out.PackageSavedHash.IsEmpty() && Out.bWasLoadedFromDisk
			&& !Out.bPackageDirty && Package && Package->GetName() == Out.PackageName;
		Out.bPersistedProjectionComplete = bDiskIdentity
			&& Out.bSemanticProjectionComplete;
		Out.PersistedFingerprint =
			FHyperAIStudioActorModifierContracts::ComputePersistedFingerprint(Out);
		Out.VolatileFingerprint =
			FHyperAIStudioActorModifierContracts::ComputeVolatileFingerprint(Out);
		return Out.bSemanticProjectionComplete;
	}

	FString ComputeAuthorityFingerprint(const TArray<FHyperAIActorModifierAuthorityRow>& Rows)
	{
		FString Canonical(TEXT("hyperai.actor-modifier.authority.v1|native:278|python:598|matches:0|"));
		for (const FHyperAIActorModifierAuthorityRow& Row : Rows)
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

FString FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioActorModifier.HyperAIStudioActorModifierToolset");
}

const TArray<FHyperAIStudioActorModifierManifestEntry>&
FHyperAIStudioActorModifierContracts::GetManifest()
{
	static const TArray<FHyperAIStudioActorModifierManifestEntry> Manifest = {
		{TEXT("hyper_actor_modifier_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_actor_modifier_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_actor_modifier_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

const TArray<FHyperAIActorModifierAuthorityRow>&
FHyperAIStudioActorModifierContracts::GetAuthorityRows()
{
	static const TArray<FHyperAIActorModifierAuthorityRow> Rows = {
		{TEXT("hyperai_requirement"), TEXT("capability.actor_modifier.stack"), TEXT("discover/edit"),
			TEXT("capability_gated"), TEXT("hyper_actor_modifier_inspect/apply_plan/validate"),
			TEXT("ActorModifier")}};
	return Rows;
}

FHyperAIActorModifierCapabilityStatus
FHyperAIStudioActorModifierContracts::GetCapabilityStatus()
{
	FHyperAIActorModifierCapabilityStatus Status;
	Status.AuthorityFingerprint =
		HyperAIStudio::ActorModifier::Private::ComputeAuthorityFingerprint(GetAuthorityRows());
	Status.SupportedCases = {
		TEXT("one exact already-loaded actor and its direct public ActorModifierCore stack"),
		TEXT("ordered modifier identity/enabled-state persisted and volatile seals"),
		TEXT("closed add/remove/move/enable transaction preflight and detached validation")};
	Status.UnsupportedCases = {
		TEXT("unloaded actor search, map loading, broad reflection, and nested private settings"),
		TEXT("mutation until the bounded journaled transaction/save backend is admitted")};
	Status.Remediation = TEXT("Enable ActorModifier, load and save the exact map actor, then inspect. Non-dry plans remain zero-effect until the shared bounded transaction/save executor is connected.");
	return Status;
}

bool FHyperAIStudioActorModifierContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioActorModifierManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3 || GetAuthorityRows().Num() != 1) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioActorModifierManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioActorModifierContracts::IsSafeActorPath(const FString& Path)
{
	return HyperAIStudio::ActorModifier::Private::IsSafeObjectPath(Path);
}

bool FHyperAIStudioActorModifierContracts::IsCanonicalSha256(const FString& Value)
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

bool FHyperAIStudioActorModifierContracts::IsSafeOperationId(const FString& Value)
{
	return !Value.IsEmpty() && Value.Len() <= FHyperAIStudioDomainLimits::MaxOperationIdChars
		&& Value.TrimStartAndEnd() == Value
		&& !HyperAIStudio::ActorModifier::Private::HasControlCharacter(Value);
}

FString FHyperAIStudioActorModifierContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	if (State == UE::AssetRegistry::EExists::Exists) return TEXT("exists");
	if (State == UE::AssetRegistry::EExists::DoesNotExist) return TEXT("does_not_exist");
	return TEXT("unknown");
}

FString FHyperAIStudioActorModifierContracts::ComputePersistedFingerprint(
	const FHyperAIActorModifierSnapshot& Snapshot)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FString Canonical(TEXT("hyperai.actor-modifier.persisted.v1|"));
	AppendToken(Canonical, Snapshot.ActorPath);
	AppendToken(Canonical, Snapshot.ActorClassPath);
	AppendToken(Canonical, Snapshot.PackageName);
	AppendToken(Canonical, Snapshot.DiskExistence);
	AppendToken(Canonical, Snapshot.PackageSavedHash);
	AppendToken(Canonical, LexToString(Snapshot.DiskSize));
	AppendToken(Canonical, BoolToken(Snapshot.bHasStack));
	AppendToken(Canonical, BoolToken(Snapshot.bStackFrozen));
	AppendToken(Canonical, BoolToken(Snapshot.bSemanticProjectionComplete));
	for (const FHyperAIActorModifierEntry& Entry : Snapshot.Modifiers)
	{
		AppendToken(Canonical, LexToString(Entry.Index));
		AppendToken(Canonical, Entry.ModifierPath);
		AppendToken(Canonical, Entry.ModifierName);
		AppendToken(Canonical, Entry.ClassPath);
		AppendToken(Canonical, BoolToken(Entry.bEnabled));
	}
	return HashCanonical(Canonical);
}

FString FHyperAIStudioActorModifierContracts::ComputeVolatileFingerprint(
	const FHyperAIActorModifierSnapshot& Snapshot)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FString Canonical(TEXT("hyperai.actor-modifier.volatile.v1|"));
	AppendToken(Canonical, Snapshot.ActorPath);
	AppendToken(Canonical, BoolToken(Snapshot.bActorLoaded));
	AppendToken(Canonical, BoolToken(Snapshot.bWasLoadedFromDisk));
	AppendToken(Canonical, BoolToken(Snapshot.bPackageDirty));
	for (const FHyperAIActorModifierEntry& Entry : Snapshot.Modifiers)
	{
		AppendToken(Canonical, Entry.ModifierPath);
		AppendToken(Canonical, BoolToken(Entry.bApplied));
		AppendToken(Canonical, BoolToken(Entry.bIdle));
	}
	return HashCanonical(Canonical);
}

bool FHyperAIStudioActorModifierContracts::ValidateOperations(
	const TArray<FHyperAIActorModifierPlanOperation>& Operations,
	FString& OutFingerprint, FString& OutError)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	OutFingerprint.Reset();
	OutError.Reset();
	if (Operations.IsEmpty() || Operations.Num() > MaxOperations
		|| Operations.GetAllocatedSize() > MaxContainerBytes)
	{
		OutError = TEXT("One to 32 bounded operations are required.");
		return false;
	}
	FString Canonical(TEXT("hyperai.actor-modifier.operations.v1|"));
	TSet<FString> ExactRows;
	for (const FHyperAIActorModifierPlanOperation& Operation : Operations)
	{
		if (Operation.Action.Len() > 24 || Operation.ModifierPath.Len() > MaxPathCharacters
			|| Operation.ModifierName.Len() > MaxNameCharacters
			|| Operation.AnchorPath.Len() > MaxPathCharacters
			|| HasControlCharacter(Operation.Action)
			|| HasControlCharacter(Operation.ModifierName))
		{
			OutError = TEXT("An operation exceeds its pre-copy scalar bound.");
			return false;
		}
		const bool bAdd = Operation.Action == TEXT("add");
		const bool bMove = Operation.Action == TEXT("move_before")
			|| Operation.Action == TEXT("move_after");
		const bool bExisting = bMove || Operation.Action == TEXT("remove")
			|| Operation.Action == TEXT("set_enabled");
		if ((!bAdd && !bExisting)
			|| (bAdd && (!Operation.ModifierPath.IsEmpty()
				|| Operation.ModifierName.IsEmpty() || !Operation.AnchorPath.IsEmpty()))
			|| (bExisting && (!IsSafeObjectPath(Operation.ModifierPath)
				|| !Operation.ModifierName.IsEmpty()))
			|| (bMove && (!IsSafeObjectPath(Operation.AnchorPath)
				|| Operation.AnchorPath == Operation.ModifierPath))
			|| (!bMove && !Operation.AnchorPath.IsEmpty()))
		{
			OutError = TEXT("Operation fields do not match the closed add/remove/move/set_enabled vocabulary.");
			return false;
		}
		FString Row;
		AppendToken(Row, Operation.Action);
		AppendToken(Row, Operation.ModifierPath);
		AppendToken(Row, Operation.ModifierName);
		AppendToken(Row, Operation.AnchorPath);
		AppendToken(Row, BoolToken(Operation.bEnabled));
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
	FString FHyperAIStudioActorModifierContracts::Name() \
	{ static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(TEXT(Literal)); return Value; }

HYPER_SCHEMA_FN(InspectPayloadSchemaFingerprint,
	"actor-modifier.inspect-request.v1|actor_path|deadline|max_output")
HYPER_SCHEMA_FN(ApplyPayloadSchemaFingerprint,
	"actor-modifier.plan-request.v1|operation|actor|persisted_cas|closed_operations|deadline|max_output")
HYPER_SCHEMA_FN(ValidatePayloadSchemaFingerprint,
	"actor-modifier.validate-request.v1|detached_snapshot|issue_bound|deadline|max_output")
HYPER_SCHEMA_FN(InspectResultSchemaFingerprint,
	"actor-modifier.inspect-result.v1|capability|snapshot|persisted|volatile|issues")
HYPER_SCHEMA_FN(ApplyResultSchemaFingerprint,
	"actor-modifier.apply-blocked-result.v1|prepared|zero_effect|hashes|effects|issues")
HYPER_SCHEMA_FN(ValidateResultSchemaFingerprint,
	"actor-modifier.validate-result.v1|valid|complete|recomputed_hashes|issues")

#undef HYPER_SCHEMA_FN

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioActorModifierContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.actor-modifier.public-core.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// ActorModifier is a blocking generated requirement, never a non-blocking hint.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_actor_modifier_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_actor_modifier_apply_plan"), ApplyVariantId,
			ApplyPayloadTypeId, ApplyPayloadSchemaFingerprint(), ApplyResultTypeId,
			ApplyResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_actor_modifier_validate"), ValidateVariantId,
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

FHyperAIActorModifierInspectReport FHyperAIStudioActorModifierContracts::Inspect(
	const FHyperAIActorModifierInspectRequest& Request)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FHyperAIActorModifierInspectReport Report;
	Report.Capability = GetCapabilityStatus();
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.ActorPath.Len() > MaxPathCharacters
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_envelope"),
			TEXT("Actor path, deadline, or output bounds are invalid."));
	}
	if (Request.ActorPath.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("capability_only");
		Report.Diagnostic = TEXT("Returned frozen source/Epic-review authority only; no UObject, subsystem, or Asset Registry state was accessed.");
		return Report;
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Exact loaded ActorModifierCore inspection is game-thread only."));
	}
	if (!IsSafeActorPath(Request.ActorPath))
	{
		return Reject(TEXT("invalid_actor_path"),
			TEXT("ActorPath must be one bounded canonical /Game soft object path."));
	}
	const int64 WorstCase = BaseReportBytes + MaxModifiers * EntryBytes + 8 * IssueBytes;
	if (WorstCase > Request.MaxOutputBytes)
	{
		return Reject(TEXT("worst_case_output_bound"),
			TEXT("MaxOutputBytes cannot hold the bounded direct-stack report."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	const bool bSemanticComplete = CaptureSnapshot(
		Request.ActorPath, Report.Snapshot, Report.Issues);
	const int64 ActualBytes = BaseReportBytes
		+ Report.Snapshot.Modifiers.Num() * EntryBytes
		+ Report.Issues.Num() * IssueBytes;
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
			TEXT("Loaded-only ActorModifierCore capture exceeded its monotonic deadline."));
	}
	Report.bOk = bSemanticComplete;
	Report.Status = bSemanticComplete ? TEXT("loaded_snapshot") : TEXT("snapshot_incomplete");
	Report.Diagnostic = bSemanticComplete
		? TEXT("Captured one exact loaded actor through Epic's public ActorModifierCore subsystem; no search, load, reflection walk, or mutation ran.")
		: TEXT("Loaded-only capture was incomplete; issues explain the missing package, actor, subsystem, or bounded semantic evidence.");
	return Report;
}

FHyperAIActorModifierValidateReport FHyperAIStudioActorModifierContracts::Validate(
	const FHyperAIActorModifierValidateRequest& Request)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FHyperAIActorModifierValidateReport Report;
	auto Reject = [&Report](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.DeadlineMs < 10 || Request.DeadlineMs > MaxDeadlineMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.Snapshot.Modifiers.Num() > MaxModifiers
		|| Request.Snapshot.Modifiers.GetAllocatedSize() > MaxContainerBytes)
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
		&& Request.Snapshot.ActorClassPath.Len() <= MaxPathCharacters
		&& Request.Snapshot.PackageName
			== FSoftObjectPath(Request.Snapshot.ActorPath).GetLongPackageName()
		&& (Request.Snapshot.DiskExistence == TEXT("exists")
			|| Request.Snapshot.DiskExistence == TEXT("does_not_exist")
			|| Request.Snapshot.DiskExistence == TEXT("unknown"));
	TSet<FString> Paths;
	for (int32 Index = 0; Index < Request.Snapshot.Modifiers.Num(); ++Index)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			return Reject(TEXT("validation_deadline_exceeded"),
				TEXT("Detached validation exceeded its monotonic deadline."));
		}
		const FHyperAIActorModifierEntry& Entry = Request.Snapshot.Modifiers[Index];
		const bool bEntry = Entry.Index == Index && IsSafeObjectPath(Entry.ModifierPath)
			&& !Paths.Contains(Entry.ModifierPath) && !Entry.ModifierName.IsEmpty()
			&& Entry.ModifierName.Len() <= MaxNameCharacters
			&& Entry.ClassPath.Len() <= MaxPathCharacters;
		if (!bEntry)
		{
			bShape = false;
			AddIssue(Report.Issues, TEXT("invalid_modifier_entry"), TEXT("error"),
				Entry.ModifierPath, TEXT("Detached modifier ordering, identity, uniqueness, or bounds are invalid."));
		}
		Paths.Add(Entry.ModifierPath);
	}
	if (Request.Snapshot.bHasStack == false && !Request.Snapshot.Modifiers.IsEmpty())
	{
		bShape = false;
		AddIssue(Report.Issues, TEXT("stack_presence_mismatch"), TEXT("error"),
			Request.Snapshot.ActorPath, TEXT("A detached snapshot without a stack cannot contain modifiers."));
	}
	const bool bExpectedPersistedComplete = Request.Snapshot.DiskExistence == TEXT("exists")
		&& Request.Snapshot.DiskSize > 0 && !Request.Snapshot.PackageSavedHash.IsEmpty()
		&& Request.Snapshot.bActorLoaded && Request.Snapshot.bWasLoadedFromDisk
		&& !Request.Snapshot.bPackageDirty && Request.Snapshot.bSemanticProjectionComplete;
	if (Request.Snapshot.bPersistedProjectionComplete != bExpectedPersistedComplete)
	{
		bShape = false;
		AddIssue(Report.Issues, TEXT("persisted_completeness_mismatch"), TEXT("error"),
			Request.Snapshot.ActorPath, TEXT("Persisted completeness does not follow from the detached evidence fields."));
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
	FString Canonical(TEXT("hyperai.actor-modifier.validator.v1|"));
	AppendToken(Canonical, Report.RecomputedPersistedFingerprint);
	AppendToken(Canonical, Report.RecomputedVolatileFingerprint);
	AppendToken(Canonical, BoolToken(Report.bValid));
	Report.ValidatorFingerprint = HashCanonical(Canonical);
	Report.Status = Report.bValid
		? (Report.bComplete ? TEXT("valid") : TEXT("valid_partial")) : TEXT("invalid");
	Report.Diagnostic = TEXT("Recomputed the detached ActorModifier snapshot only; no UObject, subsystem, Asset Registry, or editor state was accessed.");
	return Report;
}

FString FHyperAIStudioActorModifierContracts::ComputePlanSemanticFingerprint(
	const FHyperAIStudioActorModifierPlanPayload& Payload)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FString Canonical(TEXT("hyperai.actor-modifier.plan-semantic.v1|"));
	AppendToken(Canonical, Payload.ActorPath);
	AppendToken(Canonical, Payload.BasePersistedFingerprint);
	AppendToken(Canonical, Payload.OperationFingerprint);
	return HashCanonical(Canonical);
}

FHyperAIActorModifierApplyPlanReport FHyperAIStudioActorModifierContracts::BuildPlan(
	const FHyperAIActorModifierApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::ActorModifier::Private;
	FHyperAIActorModifierApplyPlanReport Report;
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
			TEXT("No effect ran. The public ActorModifierCore calls require one admitted journaled transaction, bounded rollback, one save, and fresh validation; this module did not add, remove, move, enable, transact, save, or stage anything."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Loaded semantic ActorModifierCore preflight is game-thread only."));
	}
	const double Deadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	FHyperAIActorModifierSnapshot Snapshot;
	if (!CaptureSnapshot(Request.ActorPath, Snapshot, Report.Issues)
		|| !Snapshot.bPersistedProjectionComplete)
	{
		return Reject(TEXT("persisted_semantic_cas_incomplete"),
			TEXT("Pure preflight requires one exact loaded, clean, disk-backed actor and complete direct-stack projection."));
	}
	Report.BasePersistedFingerprint = Snapshot.PersistedFingerprint;
	if (Snapshot.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("persisted_semantic_cas_mismatch"),
			TEXT("The fresh actor/stack persisted fingerprint differs from the caller assertion."));
	}
	TMap<FString, const FHyperAIActorModifierEntry*> Existing;
	for (const FHyperAIActorModifierEntry& Entry : Snapshot.Modifiers)
	{
		Existing.Add(Entry.ModifierPath, &Entry);
	}
	UActorModifierCoreSubsystem* Subsystem = UActorModifierCoreSubsystem::Get();
	for (const FHyperAIActorModifierPlanOperation& Operation : Request.Operations)
	{
		if (Operation.Action == TEXT("add"))
		{
			if (!Subsystem
				|| !Subsystem->GetRegisteredModifierClass(FName(*Operation.ModifierName)).Get())
			{
				return Reject(TEXT("modifier_not_registered"),
					TEXT("An add operation names a modifier not registered by Epic's public subsystem."));
			}
			continue;
		}
		const FHyperAIActorModifierEntry* const* Entry = Existing.Find(Operation.ModifierPath);
		if (!Entry)
		{
			return Reject(TEXT("modifier_cas_missing"),
				TEXT("An existing-operation modifier path is absent from the fresh direct stack."));
		}
		if ((Operation.Action == TEXT("move_before") || Operation.Action == TEXT("move_after"))
			&& !Existing.Contains(Operation.AnchorPath))
		{
			return Reject(TEXT("modifier_anchor_cas_missing"),
				TEXT("A move anchor is absent from the fresh direct stack."));
		}
		if (Operation.Action == TEXT("set_enabled")
			&& (*Entry)->bEnabled == Operation.bEnabled)
		{
			return Reject(TEXT("no_op_operation"),
				TEXT("A set_enabled operation already matches fresh loaded state."));
		}
	}
	if (FPlatformTime::Seconds() > Deadline)
	{
		return Reject(TEXT("preflight_deadline_exceeded"),
			TEXT("Loaded ActorModifierCore preflight exceeded its monotonic deadline."));
	}

	const TSharedRef<FHyperAIStudioActorModifierPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioActorModifierPlanPayload, ESPMode::ThreadSafe>();
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
			TEXT("The closed modifier operation payload could not be independently sealed."));
	}

	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_actor_modifier_apply_plan");
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
	Report.Diagnostic = TEXT("Pure Prepare sealed one loaded ActorModifierCore transaction/save intent; no editor effect, stage, transaction, or save ran.");
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

FString FHyperAIStudioActorModifierInspectPayload::GetTypeId() const
{
	return FHyperAIStudioActorModifierContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioActorModifierInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioActorModifierContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioActorModifierInspectPayload::GetBoundedByteSize() const
{
	return 128 + 2 * Request.ActorPath.Len();
}

FString FHyperAIStudioActorModifierValidatePayload::GetTypeId() const
{
	return FHyperAIStudioActorModifierContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioActorModifierValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioActorModifierContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioActorModifierValidatePayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 512 + Request.Snapshot.Modifiers.Num() * 640);
}

FString FHyperAIStudioActorModifierPlanPayload::GetTypeId() const
{
	return FHyperAIStudioActorModifierContracts::ApplyPayloadTypeId;
}

FString FHyperAIStudioActorModifierPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioActorModifierContracts::ApplyPayloadSchemaFingerprint();
}

int32 FHyperAIStudioActorModifierPlanPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 512 + Operations.Num() * 640);
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioActorModifierPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioActorModifierPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioActorModifierPlanPayload, ESPMode::ThreadSafe>();
	Clone->ActorPath = ActorPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->Operations = Operations;
	Clone->OperationFingerprint = OperationFingerprint;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioActorModifierInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioActorModifierContracts::InspectResultTypeId;
}

FString FHyperAIStudioActorModifierInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioActorModifierContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioActorModifierInspectResultPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 4096 + Report.Snapshot.Modifiers.Num() * 640
		+ Report.Issues.Num() * 512);
}

FString FHyperAIStudioActorModifierValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioActorModifierContracts::ValidateResultTypeId;
}

FString FHyperAIStudioActorModifierValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioActorModifierContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioActorModifierValidateResultPayload::GetBoundedByteSize() const
{
	return FMath::Min(MAX_int32, 4096 + Report.Issues.Num() * 512);
}

FHyperAIActorModifierInspectReport UHyperAIStudioActorModifierToolset::hyper_actor_modifier_inspect(
	const FHyperAIActorModifierInspectRequest& Request)
{
	return FHyperAIStudioActorModifierContracts::Inspect(Request);
}

FHyperAIActorModifierApplyPlanReport UHyperAIStudioActorModifierToolset::hyper_actor_modifier_apply_plan(
	const FHyperAIActorModifierApplyPlanRequest& Request)
{
	return FHyperAIStudioActorModifierContracts::BuildPlan(Request);
}

FHyperAIActorModifierValidateReport UHyperAIStudioActorModifierToolset::hyper_actor_modifier_validate(
	const FHyperAIActorModifierValidateRequest& Request)
{
	return FHyperAIStudioActorModifierContracts::Validate(Request);
}

FHyperAIStudioActorModifierDomainAdapter::FHyperAIStudioActorModifierDomainAdapter()
	: Descriptor(FHyperAIStudioActorModifierContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioActorModifierDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioActorModifierDomainAdapter::Execute(
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
			TEXT("Actor Modifier adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_actor_modifier_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioActorModifierContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioActorModifierContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioActorModifierContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioActorModifierInspectPayload& Typed =
			static_cast<const FHyperAIStudioActorModifierInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioActorModifierInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioActorModifierInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioActorModifierContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_actor_modifier_validate")
		&& Context.Binding.VariantId == FHyperAIStudioActorModifierContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioActorModifierContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioActorModifierContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioActorModifierValidatePayload& Typed =
			static_cast<const FHyperAIStudioActorModifierValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioActorModifierValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioActorModifierValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioActorModifierContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_actor_modifier_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioActorModifierContracts::ApplyVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioActorModifierContracts::ApplyPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioActorModifierContracts::ApplyPayloadSchemaFingerprint())
	{
		const FHyperAIStudioActorModifierPlanPayload& Typed =
			static_cast<const FHyperAIStudioActorModifierPlanPayload&>(Payload);
		FString RecomputedOperations;
		FString Error;
		if (!FHyperAIStudioActorModifierContracts::ValidateOperations(
			Typed.Operations, RecomputedOperations, Error)
			|| RecomputedOperations != Typed.OperationFingerprint
			|| FHyperAIStudioActorModifierContracts::ComputePlanSemanticFingerprint(Typed)
				!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Actor Modifier operation model or semantic seal drifted."));
		}
		return Reject(FHyperAIStudioActorModifierContracts::NonDryCallableState,
			TEXT("No effect ran; the admitted bounded transaction/save backend is not connected."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Actor Modifier adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioActorModifierRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioActorModifierRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioActorModifierRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioActorModifierRegistration::IsRegistered() const
{
	return FHyperAIStudioActorModifierContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioActorModifierToolset::StaticClass(),
			FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioActorModifierRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid();
}

void FHyperAIStudioActorModifierRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable()) return;
	if (!FHyperAIStudioActorModifierContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioActorModifier, Verbose,
			TEXT("Actor Modifier exact source cohort remains catalog fail-closed."));
		return;
	}
	Adapter = MakeShared<FHyperAIStudioActorModifierDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioActorModifier, Error,
			TEXT("Actor Modifier adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioActorModifierToolset::StaticClass(),
		FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioActorModifier, Error,
			TEXT("Actor Modifier atomic registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioActorModifierRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioActorModifierToolset::StaticClass(),
			FHyperAIStudioActorModifierContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioActorModifier, Error,
				TEXT("Actor Modifier owned-toolset rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioActorModifier, Error,
				TEXT("Actor Modifier adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
	}
	Adapter.Reset();
}
