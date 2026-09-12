// Games by Hyper 2026.

#include "HyperAIStudioUIToolset.h"

#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioUIDelegationMatrix.h"
#include "HyperAIStudioUIValueModel.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioUI, Log, All);

namespace HyperAIStudio::UI::Toolset::Private
{
	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical.AppendChar(TEXT('|'));
	}

	FString BoolToken(const bool bValue)
	{
		return bValue ? TEXT("1") : TEXT("0");
	}

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < 0x20 || Character == 0x7f) return true;
		}
		return false;
	}

	bool HasUnsafeTextControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if ((Character < 0x20 && Character != TEXT('\r')
				&& Character != TEXT('\n') && Character != TEXT('\t'))
				|| Character == 0x7f) return true;
		}
		return false;
	}

	bool IsSafeName(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioUIContracts::MaxNameCharacters
			|| HasControlCharacter(Value)) return false;
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeSubject(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 512 || HasControlCharacter(Value)) return false;
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_')
				|| Character == TEXT('-') || Character == TEXT(':'))) return false;
		}
		return true;
	}

	bool IsSafeWidgetSubject(const FString& Value)
	{
		if (Value.StartsWith(TEXT("name:"), ESearchCase::CaseSensitive))
		{
			return IsSafeName(Value.RightChop(5));
		}
		if (!Value.StartsWith(TEXT("guid:"), ESearchCase::CaseSensitive)
			|| Value.Len() != 37) return false;
		for (int32 Index = 5; Index < Value.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Value[Index])) return false;
		}
		return true;
	}

	bool IsSafeEndpoint(const FString& Value)
	{
		if (Value == TEXT("self")) return true;
		FString Prefix;
		FString Remainder;
		if (!Value.Split(TEXT(":"), &Prefix, &Remainder)
			|| (Prefix != TEXT("widget") && Prefix != TEXT("viewmodel")))
		{
			return false;
		}
		if (Prefix == TEXT("widget")) return IsSafeName(Remainder);
		if (Remainder.Len() != 32) return false;
		for (const TCHAR Character : Remainder)
		{
			if (!FChar::IsHexDigit(Character)) return false;
		}
		return true;
	}

	bool IsSafeFieldPath(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioUIContracts::MaxTextCharacters
			|| HasControlCharacter(Value)) return false;
		TArray<FString> Segments;
		Value.ParseIntoArray(Segments, TEXT("."), false);
		if (Segments.IsEmpty()
			|| Segments.Num() > FHyperAIStudioUIContracts::MaxMVVMPathSegments) return false;
		for (const FString& Segment : Segments)
		{
			if (!IsSafeName(Segment)) return false;
		}
		return true;
	}

	bool IsZeroFrameRange(const FHyperAIUIPlanOperation& Operation)
	{
		return Operation.StartFrame == 0 && Operation.EndFrame == 0;
	}

	bool HasNoScalar(const FHyperAIUIPlanOperation& Operation)
	{
		return Operation.StringValue.IsEmpty() && !Operation.bHasBoolValue
			&& !Operation.bHasNumberValue && !Operation.bHasColorValue;
	}

	bool HasNoBindingShape(const FHyperAIUIPlanOperation& Operation)
	{
		return Operation.SecondaryName.IsEmpty() && Operation.SourceEndpoint.IsEmpty()
			&& Operation.SourcePath.IsEmpty() && Operation.DestinationEndpoint.IsEmpty()
			&& Operation.DestinationPath.IsEmpty();
	}

	bool IsCreateKind(const EHyperAIStudioUIOperationKind Kind)
	{
		return Kind == EHyperAIStudioUIOperationKind::AnimationCreate
			|| Kind == EHyperAIStudioUIOperationKind::BindingCreateLegacy
			|| Kind == EHyperAIStudioUIOperationKind::BindingCreateMVVM;
	}

	bool IsDeleteKind(const EHyperAIStudioUIOperationKind Kind)
	{
		return Kind == EHyperAIStudioUIOperationKind::AnimationDelete
			|| Kind == EHyperAIStudioUIOperationKind::BindingDeleteLegacy
			|| Kind == EHyperAIStudioUIOperationKind::BindingDeleteMVVM;
	}

	bool IsReplaceKind(const EHyperAIStudioUIOperationKind Kind)
	{
		return Kind == EHyperAIStudioUIOperationKind::BindingReplaceLegacy
			|| Kind == EHyperAIStudioUIOperationKind::BindingReplaceMVVM;
	}

	FString RequestFingerprint(const FHyperAIUIInspectRequest& Request)
	{
		TArray<FString> Paths = Request.TargetPaths;
		Paths.Sort();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.ui.inspect-request.v1"));
		for (const FString& Path : Paths) AppendToken(Canonical, Path);
		AppendToken(Canonical, BoolToken(Request.bIncludeTree));
		AppendToken(Canonical, BoolToken(Request.bIncludeLayout));
		AppendToken(Canonical, BoolToken(Request.bIncludeAnimations));
		AppendToken(Canonical, BoolToken(Request.bIncludeLegacyBindings));
		AppendToken(Canonical, BoolToken(Request.bIncludeMVVM));
		AppendToken(Canonical, BoolToken(Request.bIncludeVolatile));
		AppendToken(Canonical, LexToString(Request.PageSize));
		AppendToken(Canonical, LexToString(Request.DeadlineMs));
		AppendToken(Canonical, LexToString(Request.MaxOutputBytes));
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString SafetyToken(const EHyperAIStudioUIPlanSafety Safety)
	{
		return Safety == EHyperAIStudioUIPlanSafety::Destructive
			? TEXT("destructive") : TEXT("edit");
	}

	FString OperationKindToken(const EHyperAIStudioUIOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioUIOperationKind::AnimationCreate: return TEXT("animation.create");
		case EHyperAIStudioUIOperationKind::AnimationRename: return TEXT("animation.rename");
		case EHyperAIStudioUIOperationKind::AnimationDelete: return TEXT("animation.delete");
		case EHyperAIStudioUIOperationKind::WidgetSetProperty: return TEXT("widget.set_property");
		case EHyperAIStudioUIOperationKind::WidgetSetStyle: return TEXT("widget.set_style");
		case EHyperAIStudioUIOperationKind::BindingCreateLegacy: return TEXT("binding.create_legacy");
		case EHyperAIStudioUIOperationKind::BindingUpdateLegacy: return TEXT("binding.update_legacy");
		case EHyperAIStudioUIOperationKind::BindingReplaceLegacy: return TEXT("binding.replace_legacy");
		case EHyperAIStudioUIOperationKind::BindingDeleteLegacy: return TEXT("binding.delete_legacy");
		case EHyperAIStudioUIOperationKind::BindingCreateMVVM: return TEXT("binding.create_mvvm");
		case EHyperAIStudioUIOperationKind::BindingUpdateMVVM: return TEXT("binding.update_mvvm");
		case EHyperAIStudioUIOperationKind::BindingReplaceMVVM: return TEXT("binding.replace_mvvm");
		case EHyperAIStudioUIOperationKind::BindingDeleteMVVM: return TEXT("binding.delete_mvvm");
		default: return TEXT("invalid");
		}
	}

	FString ResultSchemaFingerprint()
	{
		static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.ui.apply-result.v1|typed_prepared|zero_effect|safety|variant|base|desired|semantic|plan|authorization|capability|effect|effects|issues"));
		return Value;
	}

	bool HasError(const TArray<FHyperAIUIIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIUIIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	bool CopyIssuesWithinOutput(
		const TArray<FHyperAIUIIssue>& Source,
		const int32 MaxOutputBytes,
		int64& InOutEstimatedBytes,
		TArray<FHyperAIUIIssue>& OutIssues,
		bool& bOutTruncated)
	{
		for (const FHyperAIUIIssue& Issue : Source)
		{
			const int32 Bytes = FHyperAIStudioUIValueContracts::EstimateIssueBytes(Issue);
			if (OutIssues.Num() >= FHyperAIStudioUIContracts::MaxIssues
				|| InOutEstimatedBytes + Bytes > MaxOutputBytes)
			{
				bOutTruncated = true;
				return false;
			}
			InOutEstimatedBytes += Bytes;
			OutIssues.Add(Issue);
		}
		return true;
	}

	FHyperAIUIInspectReport Inspect(const FHyperAIUIInspectRequest& Request)
	{
		FHyperAIUIInspectReport Report;
		auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic.Left(1024);
			return Report;
		};
		if (Request.TargetPaths.Num() > FHyperAIStudioUIContracts::MaxTargetPaths
			|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioUIContracts::MaxPageSize
			|| Request.Cursor.Len() > FHyperAIStudioUIContracts::MaxCursorCharacters
			|| Request.DeadlineMs < 10 || Request.DeadlineMs > 2000
			|| Request.MaxOutputBytes < 4096
			|| Request.MaxOutputBytes > FHyperAIStudioUIContracts::MaxOutputBytes)
		{
			return Reject(TEXT("invalid_bounds"),
				TEXT("Target, paging, deadline, cursor, or output bounds are outside the closed inspect contract."));
		}
		if (!Request.bIncludeTree && Request.bIncludeLayout)
		{
			return Reject(TEXT("invalid_projection_flags"),
				TEXT("Layout projection requires the bounded WidgetTree projection."));
		}
		for (const FString& Path : Request.TargetPaths)
		{
			if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Path))
			{
				return Reject(TEXT("invalid_target_path"),
					TEXT("Every target must be one canonical exact /Game object path."));
			}
		}
		if (Request.bIncludeVolatile && !Request.Cursor.IsEmpty())
		{
			return Reject(TEXT("volatile_cursor_forbidden"),
				TEXT("Volatile observations cannot be continued with a cursor."));
		}
		const double AbsoluteDeadline = FPlatformTime::Seconds()
			+ static_cast<double>(Request.DeadlineMs) / 1000.0;
		Report.Scope = Request.TargetPaths.IsEmpty()
			? TEXT("loaded_only_bounded_prefix") : TEXT("loaded_only_exact_targets");
		Report.RequestFingerprint = RequestFingerprint(Request);
		if (!FHyperAIStudioUIContracts::IsCanonicalSha256(Report.RequestFingerprint))
		{
			return Reject(TEXT("request_fingerprint_failed"),
				TEXT("The bounded inspect request could not be canonically sealed."));
		}
		FHyperAIStudioUIValueSnapshot Snapshot;
		FString CaptureStatus;
		FString CaptureDiagnostic;
		if (!FHyperAIStudioUICapture::Capture(Request.TargetPaths, Request.bIncludeTree,
			Request.bIncludeLayout, Request.bIncludeAnimations, Request.bIncludeLegacyBindings,
			Request.bIncludeMVVM, Request.bIncludeVolatile, Request.DeadlineMs, Snapshot,
			CaptureStatus, CaptureDiagnostic))
		{
			return Reject(*CaptureStatus, CaptureDiagnostic);
		}
		if (FPlatformTime::Seconds() > AbsoluteDeadline)
		{
			return Reject(TEXT("deadline_exceeded"),
				TEXT("Inspect capture or fingerprinting exhausted the monotonic deadline."));
		}
		Report.PersistedFingerprint = Snapshot.PersistedFingerprint;
		Report.VolatileObservationFingerprint = Snapshot.VolatileObservationFingerprint;
		Report.bSnapshotComplete = Snapshot.bComplete;
		Report.bVolatileIncluded = Snapshot.bContainsVolatile;
		Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
		Report.TotalRecords = Snapshot.Records.Num();
		if (Snapshot.bContainsVolatile || !Snapshot.bComplete)
		{
			int64 OnePageBytes = 4096;
			for (const FHyperAIUIRecord& Record : Snapshot.Records)
			{
				OnePageBytes += FHyperAIStudioUIValueContracts::EstimateRecordBytes(Record);
			}
			for (const FHyperAIUIIssue& Issue : Snapshot.CaptureIssues)
			{
				OnePageBytes += FHyperAIStudioUIValueContracts::EstimateIssueBytes(Issue);
			}
			if (!Request.Cursor.IsEmpty() || Snapshot.Records.Num() > Request.PageSize
				|| OnePageBytes > Request.MaxOutputBytes)
			{
				return Reject(Snapshot.bContainsVolatile
					? TEXT("volatile_snapshot_not_pageable")
					: TEXT("incomplete_snapshot_not_pageable"),
					TEXT("Volatile or incomplete evidence must fit one bounded response and never receives a continuation cursor."));
			}
		}
		int32 Offset = 0;
		if (!FHyperAIStudioUIContracts::DecodeCursor(Request.Cursor,
			Report.RequestFingerprint, Report.PersistedFingerprint, Offset)
			|| Offset < 0 || Offset > Snapshot.Records.Num())
		{
			return Reject(TEXT("cursor_mismatch"),
				TEXT("Cursor identity does not match the exact request and fresh persisted fingerprint."));
		}
		int64 EstimatedBytes = 4096;
		for (int32 Index = Offset;
			Index < Snapshot.Records.Num() && Report.Records.Num() < Request.PageSize; ++Index)
		{
			const int32 Bytes = FHyperAIStudioUIValueContracts::EstimateRecordBytes(
				Snapshot.Records[Index]);
			if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			EstimatedBytes += Bytes;
			Report.Records.Add(Snapshot.Records[Index]);
		}
		Report.ReturnedRecords = Report.Records.Num();
		if (Report.ReturnedRecords == 0 && Offset < Snapshot.Records.Num())
		{
			return Reject(TEXT("output_bound_exceeded"),
				TEXT("The next bounded record does not fit the caller's output envelope."));
		}
		CopyIssuesWithinOutput(Snapshot.CaptureIssues, Request.MaxOutputBytes,
			EstimatedBytes, Report.Issues, Report.bTruncated);
		if (FPlatformTime::Seconds() > AbsoluteDeadline)
		{
			return Reject(TEXT("deadline_exceeded"),
				TEXT("Inspect paging exhausted the monotonic deadline."));
		}
		const int32 NextOffset = Offset + Report.ReturnedRecords;
		if (NextOffset < Snapshot.Records.Num())
		{
			Report.bTruncated = true;
			if (!Snapshot.bContainsVolatile && Snapshot.bComplete)
			{
				Report.NextCursor = FHyperAIStudioUIContracts::EncodeCursor(
					NextOffset, Report.RequestFingerprint, Report.PersistedFingerprint);
				if (Report.NextCursor.IsEmpty())
				{
					return Reject(TEXT("cursor_seal_failed"),
						TEXT("The stable continuation cursor could not be sealed."));
				}
			}
		}
		Report.bOk = true;
		Report.Status = CaptureStatus;
		Report.Diagnostic = CaptureDiagnostic;
		return Report;
	}

	FHyperAIUIValidateReport Validate(const FHyperAIUIValidateRequest& Request)
	{
		FHyperAIUIValidateReport Report;
		auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.bValid = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic.Left(1024);
			return Report;
		};
		if (Request.TargetPaths.Num() > FHyperAIStudioUIContracts::MaxTargetPaths
			|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioUIContracts::MaxIssues
			|| Request.DeadlineMs < 10 || Request.DeadlineMs > 2000
			|| Request.MaxOutputBytes < 4096
			|| Request.MaxOutputBytes > FHyperAIStudioUIContracts::MaxOutputBytes)
		{
			return Reject(TEXT("invalid_bounds"),
				TEXT("Target, issue, deadline, or output bounds are outside the closed validate contract."));
		}
		for (const FString& Path : Request.TargetPaths)
		{
			if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Path))
			{
				return Reject(TEXT("invalid_target_path"),
					TEXT("Every validation target must be one canonical exact /Game object path."));
			}
		}
		const double AbsoluteDeadline = FPlatformTime::Seconds()
			+ static_cast<double>(Request.DeadlineMs) / 1000.0;
		Report.Scope = Request.TargetPaths.IsEmpty()
			? TEXT("loaded_only_bounded_prefix") : TEXT("loaded_only_exact_targets");
		FHyperAIStudioUIValueSnapshot Snapshot;
		FString CaptureStatus;
		FString CaptureDiagnostic;
		if (!FHyperAIStudioUICapture::Capture(Request.TargetPaths, true,
			Request.bCheckLayout, Request.bCheckAnimations, Request.bCheckBindings,
			Request.bCheckMVVM, false, Request.DeadlineMs, Snapshot,
			CaptureStatus, CaptureDiagnostic))
		{
			return Reject(*CaptureStatus, CaptureDiagnostic);
		}
		if (FPlatformTime::Seconds() > AbsoluteDeadline)
		{
			return Reject(TEXT("deadline_exceeded"),
				TEXT("Validation capture or fingerprinting exhausted the monotonic deadline."));
		}
		Report.PersistedFingerprint = Snapshot.PersistedFingerprint;
		Report.VolatileObservationFingerprint = Snapshot.VolatileObservationFingerprint;
		Report.bSnapshotComplete = Snapshot.bComplete;
		Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
		FHyperAIStudioUIValidationOptions Options;
		Options.bCheckLayout = Request.bCheckLayout;
		Options.bCheckAccessibility = Request.bCheckAccessibility;
		Options.bCheckAnimations = Request.bCheckAnimations;
		Options.bCheckBindings = Request.bCheckBindings;
		Options.bCheckMVVM = Request.bCheckMVVM;
		Options.MaxIssues = Request.MaxIssues;
		bool bValidatorTruncated = false;
		const TArray<FHyperAIUIIssue> Issues =
			FHyperAIStudioUIValueValidator::Validate(Snapshot, Options, bValidatorTruncated);
		if (FPlatformTime::Seconds() > AbsoluteDeadline)
		{
			return Reject(TEXT("deadline_exceeded"),
				TEXT("Independent value validation exhausted the monotonic deadline."));
		}
		int64 EstimatedBytes = 4096;
		Report.bTruncated = bValidatorTruncated;
		CopyIssuesWithinOutput(Issues, Request.MaxOutputBytes, EstimatedBytes,
			Report.Issues, Report.bTruncated);
		for (const FHyperAIUIIssue& Issue : Report.Issues)
		{
			if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
			else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
			else ++Report.InfoCount;
		}
		Report.bValid = Snapshot.bComplete && !Report.bTruncated && Report.ErrorCount == 0;
		Report.bOk = !Report.bTruncated;
		Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
		Report.Diagnostic = Report.bValid
			? TEXT("Fresh detached loaded-only values satisfy the independent validator policy.")
			: TEXT("Fresh detached values are incomplete, truncated, or contain closed validation errors.");
		return Report;
	}
}

FString FHyperAIStudioUIContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioUI.HyperAIStudioUIToolset");
}

const TArray<FHyperAIStudioUIManifestEntry>& FHyperAIStudioUIContracts::GetManifest()
{
	static const TArray<FHyperAIStudioUIManifestEntry> Manifest = {
		{TEXT("hyper_ui_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_ui_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_ui_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioUIContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioUIManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3 || FHyperAIStudioUIEpicDelegationMatrix::Get().Num() != 46) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const FHyperAIStudioUIManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("\\")) || Path.Contains(TEXT(".."))
		|| Path.Contains(TEXT(":"))
		|| HyperAIStudio::UI::Toolset::Private::HasControlCharacter(Path)) return false;
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason)) return false;
	const FSoftObjectPath SoftPath(Path);
	return SoftPath.IsValid() && SoftPath.GetSubPathUtf8String().IsEmpty();
}

bool FHyperAIStudioUIContracts::IsCanonicalSha256(const FString& Value)
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

FString FHyperAIStudioUIContracts::ClassifyAssetRegistryExistence(const int32 StateValue)
{
	if (StateValue == static_cast<int32>(UE::AssetRegistry::EExists::Exists)) return TEXT("exists");
	if (StateValue == static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist))
	{
		return TEXT("does_not_exist");
	}
	return TEXT("unknown");
}

FString FHyperAIStudioUIContracts::EncodeCursor(
	const int32 Offset,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint)
{
	using namespace HyperAIStudio::UI::Toolset::Private;
	if (Offset < 0 || !IsCanonicalSha256(RequestFingerprint)
		|| !IsCanonicalSha256(PersistedFingerprint)) return FString();
	FString SealCanonical;
	AppendToken(SealCanonical, TEXT("hyperai.ui.cursor.v1"));
	AppendToken(SealCanonical, LexToString(Offset));
	AppendToken(SealCanonical, RequestFingerprint);
	AppendToken(SealCanonical, PersistedFingerprint);
	const FString Seal = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(SealCanonical);
	const FString Cursor = FString::Printf(TEXT("ui1|%d|%s|%s|%s"), Offset,
		*RequestFingerprint, *PersistedFingerprint, *Seal);
	return Cursor.Len() <= MaxCursorCharacters ? Cursor : FString();
}

bool FHyperAIStudioUIContracts::DecodeCursor(
	const FString& Cursor,
	const FString& RequestFingerprint,
	const FString& PersistedFingerprint,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.IsEmpty()) return true;
	if (Cursor.Len() > MaxCursorCharacters) return false;
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT("|"), false);
	if (Parts.Num() != 5 || Parts[0] != TEXT("ui1")
		|| Parts[2] != RequestFingerprint || Parts[3] != PersistedFingerprint
		|| !LexTryParseString(OutOffset, *Parts[1]) || OutOffset < 0) return false;
	return EncodeCursor(OutOffset, RequestFingerprint, PersistedFingerprint) == Cursor;
}

bool FHyperAIStudioUIContracts::ClassifyOperation(
	const FString& Kind,
	EHyperAIStudioUIOperationKind& OutKind,
	EHyperAIStudioUIPlanSafety& OutSafety)
{
	OutSafety = EHyperAIStudioUIPlanSafety::Edit;
	if (Kind == TEXT("animation.create")) OutKind = EHyperAIStudioUIOperationKind::AnimationCreate;
	else if (Kind == TEXT("animation.rename")) OutKind = EHyperAIStudioUIOperationKind::AnimationRename;
	else if (Kind == TEXT("animation.delete")) OutKind = EHyperAIStudioUIOperationKind::AnimationDelete;
	else if (Kind == TEXT("widget.set_property")) OutKind = EHyperAIStudioUIOperationKind::WidgetSetProperty;
	else if (Kind == TEXT("widget.set_style")) OutKind = EHyperAIStudioUIOperationKind::WidgetSetStyle;
	else if (Kind == TEXT("binding.create_legacy")) OutKind = EHyperAIStudioUIOperationKind::BindingCreateLegacy;
	else if (Kind == TEXT("binding.update_legacy")) OutKind = EHyperAIStudioUIOperationKind::BindingUpdateLegacy;
	else if (Kind == TEXT("binding.replace_legacy")) OutKind = EHyperAIStudioUIOperationKind::BindingReplaceLegacy;
	else if (Kind == TEXT("binding.delete_legacy")) OutKind = EHyperAIStudioUIOperationKind::BindingDeleteLegacy;
	else if (Kind == TEXT("binding.create_mvvm")) OutKind = EHyperAIStudioUIOperationKind::BindingCreateMVVM;
	else if (Kind == TEXT("binding.update_mvvm")) OutKind = EHyperAIStudioUIOperationKind::BindingUpdateMVVM;
	else if (Kind == TEXT("binding.replace_mvvm")) OutKind = EHyperAIStudioUIOperationKind::BindingReplaceMVVM;
	else if (Kind == TEXT("binding.delete_mvvm")) OutKind = EHyperAIStudioUIOperationKind::BindingDeleteMVVM;
	else return false;
	if (HyperAIStudio::UI::Toolset::Private::IsDeleteKind(OutKind)
		|| HyperAIStudio::UI::Toolset::Private::IsReplaceKind(OutKind))
	{
		OutSafety = EHyperAIStudioUIPlanSafety::Destructive;
	}
	return true;
}

bool FHyperAIStudioUIContracts::ValidateOperationShape(
	const FHyperAIUIPlanOperation& Operation,
	FHyperAIStudioUIBackendOperation& OutOperation,
	FString& OutError)
{
	using namespace HyperAIStudio::UI::Toolset::Private;
	OutOperation = {};
	OutError.Reset();
	if (Operation.Kind.Len() > 48 || Operation.SubjectId.Len() > 512
		|| Operation.ExpectedElementFingerprint.Len() > 71
		|| Operation.Name.Len() > MaxNameCharacters
		|| Operation.SecondaryName.Len() > MaxNameCharacters
		|| Operation.SourceEndpoint.Len() > MaxNameCharacters + 40
		|| Operation.DestinationEndpoint.Len() > MaxNameCharacters + 40
		|| Operation.SourcePath.Len() > MaxTextCharacters
		|| Operation.DestinationPath.Len() > MaxTextCharacters
		|| Operation.StringValue.Len() > MaxTextCharacters
		|| HasControlCharacter(Operation.Kind) || HasControlCharacter(Operation.SubjectId)
		|| HasControlCharacter(Operation.Name) || HasControlCharacter(Operation.SecondaryName)
		|| HasUnsafeTextControlCharacter(Operation.StringValue)
		|| (!Operation.bHasBoolValue && Operation.bBoolValue)
		|| (!Operation.bHasNumberValue && Operation.NumberValue != 0.0)
		|| (!Operation.bHasColorValue
			&& (Operation.ColorValue.R != 0.0f || Operation.ColorValue.G != 0.0f
				|| Operation.ColorValue.B != 0.0f || Operation.ColorValue.A != 0.0f))
		|| !FMath::IsFinite(Operation.NumberValue)
		|| !FMath::IsFinite(Operation.ColorValue.R) || !FMath::IsFinite(Operation.ColorValue.G)
		|| !FMath::IsFinite(Operation.ColorValue.B) || !FMath::IsFinite(Operation.ColorValue.A))
	{
		OutError = TEXT("operation_field_bound_or_finiteness_invalid");
		return false;
	}
	if (!ClassifyOperation(Operation.Kind, OutOperation.Kind, OutOperation.Safety))
	{
		OutError = TEXT("unknown_operation_kind");
		return false;
	}
	const bool bCreate = IsCreateKind(OutOperation.Kind);
	if (bCreate ? !Operation.ExpectedElementFingerprint.IsEmpty()
		: !IsCanonicalSha256(Operation.ExpectedElementFingerprint))
	{
		OutError = bCreate ? TEXT("create_cas_forbidden") : TEXT("element_cas_required");
		return false;
	}
	if (OutOperation.Kind == EHyperAIStudioUIOperationKind::AnimationCreate)
	{
		if (!Operation.SubjectId.IsEmpty() || !IsSafeName(Operation.Name)
			|| !HasNoBindingShape(Operation) || !HasNoScalar(Operation)
			|| Operation.StartFrame < 0 || Operation.StartFrame >= Operation.EndFrame)
		{
			OutError = TEXT("animation_create_shape_invalid");
			return false;
		}
	}
	else if (OutOperation.Kind == EHyperAIStudioUIOperationKind::AnimationRename)
	{
		if (!IsSafeName(Operation.SubjectId) || !IsSafeName(Operation.Name)
			|| !HasNoBindingShape(Operation) || !HasNoScalar(Operation)
			|| !IsZeroFrameRange(Operation))
		{
			OutError = TEXT("animation_rename_shape_invalid");
			return false;
		}
	}
	else if (OutOperation.Kind == EHyperAIStudioUIOperationKind::AnimationDelete)
	{
		if (!IsSafeName(Operation.SubjectId) || !Operation.Name.IsEmpty()
			|| !HasNoBindingShape(Operation) || !HasNoScalar(Operation)
			|| !IsZeroFrameRange(Operation))
		{
			OutError = TEXT("animation_delete_shape_invalid");
			return false;
		}
	}
	else if (OutOperation.Kind == EHyperAIStudioUIOperationKind::WidgetSetProperty
		|| OutOperation.Kind == EHyperAIStudioUIOperationKind::WidgetSetStyle)
	{
		if (!IsSafeWidgetSubject(Operation.SubjectId) || !HasNoBindingShape(Operation)
			|| !IsZeroFrameRange(Operation))
		{
			OutError = TEXT("widget_value_shape_invalid");
			return false;
		}
		if (OutOperation.Kind == EHyperAIStudioUIOperationKind::WidgetSetStyle)
		{
			if (!(Operation.Name == TEXT("color_and_opacity")
				|| Operation.Name == TEXT("brush_color")
				|| Operation.Name == TEXT("fill_color_and_opacity"))
				|| !Operation.bHasColorValue || Operation.bHasBoolValue
				|| Operation.bHasNumberValue || !Operation.StringValue.IsEmpty())
			{
				OutError = TEXT("widget_style_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("is_enabled"))
		{
			if (!Operation.bHasBoolValue || Operation.bHasNumberValue
				|| Operation.bHasColorValue || !Operation.StringValue.IsEmpty())
			{
				OutError = TEXT("widget_bool_property_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("percent"))
		{
			if (!Operation.bHasNumberValue || Operation.bHasBoolValue
				|| Operation.bHasColorValue || !Operation.StringValue.IsEmpty()
				|| Operation.NumberValue < 0.0 || Operation.NumberValue > 1.0)
			{
				OutError = TEXT("widget_number_property_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("value"))
		{
			if (!Operation.bHasNumberValue || Operation.bHasBoolValue
				|| Operation.bHasColorValue || !Operation.StringValue.IsEmpty())
			{
				OutError = TEXT("widget_number_property_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("visibility"))
		{
			static const TSet<FString> Values = {TEXT("visible"), TEXT("collapsed"),
				TEXT("hidden"), TEXT("hit_test_invisible"), TEXT("self_hit_test_invisible")};
			if (!Values.Contains(Operation.StringValue) || Operation.bHasBoolValue
				|| Operation.bHasNumberValue || Operation.bHasColorValue)
			{
				OutError = TEXT("widget_visibility_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("checked_state"))
		{
			if (!(Operation.StringValue == TEXT("checked")
				|| Operation.StringValue == TEXT("unchecked")
				|| Operation.StringValue == TEXT("undetermined"))
				|| Operation.bHasBoolValue || Operation.bHasNumberValue || Operation.bHasColorValue)
			{
				OutError = TEXT("widget_checked_state_shape_invalid");
				return false;
			}
		}
		else if (Operation.Name == TEXT("text"))
		{
			if (Operation.bHasBoolValue || Operation.bHasNumberValue || Operation.bHasColorValue)
			{
				OutError = TEXT("widget_text_shape_invalid");
				return false;
			}
		}
		else
		{
			OutError = TEXT("widget_property_not_allowlisted");
			return false;
		}
	}
	else if (OutOperation.Kind == EHyperAIStudioUIOperationKind::BindingDeleteLegacy
		|| OutOperation.Kind == EHyperAIStudioUIOperationKind::BindingDeleteMVVM)
	{
		if (!IsSafeSubject(Operation.SubjectId) || !Operation.Name.IsEmpty()
			|| !HasNoBindingShape(Operation) || !HasNoScalar(Operation)
			|| !IsZeroFrameRange(Operation))
		{
			OutError = TEXT("binding_delete_shape_invalid");
			return false;
		}
	}
	else
	{
		const bool bLegacy = OutOperation.Kind == EHyperAIStudioUIOperationKind::BindingCreateLegacy
			|| OutOperation.Kind == EHyperAIStudioUIOperationKind::BindingUpdateLegacy
			|| OutOperation.Kind == EHyperAIStudioUIOperationKind::BindingReplaceLegacy;
		if (!IsSafeSubject(Operation.SubjectId) || !HasNoScalar(Operation)
			|| !IsZeroFrameRange(Operation))
		{
			OutError = TEXT("binding_shape_invalid");
			return false;
		}
		if (bLegacy)
		{
			if (!IsSafeName(Operation.Name) || !IsSafeName(Operation.SecondaryName)
				|| !IsSafeName(Operation.SourcePath) || !Operation.SourceEndpoint.IsEmpty()
				|| !Operation.DestinationEndpoint.IsEmpty() || !Operation.DestinationPath.IsEmpty())
			{
				OutError = TEXT("legacy_binding_shape_invalid");
				return false;
			}
		}
		else if (!Operation.Name.IsEmpty() || !Operation.SecondaryName.IsEmpty()
			|| !IsSafeEndpoint(Operation.SourceEndpoint)
			|| !IsSafeFieldPath(Operation.SourcePath)
			|| !IsSafeEndpoint(Operation.DestinationEndpoint)
			|| !IsSafeFieldPath(Operation.DestinationPath))
		{
			OutError = TEXT("mvvm_binding_shape_invalid");
			return false;
		}
	}
	OutOperation.SubjectId = Operation.SubjectId;
	OutOperation.ExpectedElementFingerprint = Operation.ExpectedElementFingerprint;
	OutOperation.Name = Operation.Name;
	OutOperation.SecondaryName = Operation.SecondaryName;
	OutOperation.SourceEndpoint = Operation.SourceEndpoint;
	OutOperation.SourcePath = Operation.SourcePath;
	OutOperation.DestinationEndpoint = Operation.DestinationEndpoint;
	OutOperation.DestinationPath = Operation.DestinationPath;
	OutOperation.StringValue = Operation.StringValue;
	OutOperation.bHasBoolValue = Operation.bHasBoolValue;
	OutOperation.bBoolValue = Operation.bBoolValue;
	OutOperation.bHasNumberValue = Operation.bHasNumberValue;
	OutOperation.NumberValue = Operation.NumberValue;
	OutOperation.bHasColorValue = Operation.bHasColorValue;
	OutOperation.ColorValue = Operation.ColorValue;
	OutOperation.StartFrame = Operation.StartFrame;
	OutOperation.EndFrame = Operation.EndFrame;
	return true;
}

FString FHyperAIStudioUIContracts::PayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.payload.ui.compound-plan.v1|target_path|base_persisted|desired_persisted|validation_policy|semantic|derived_safety(edit,destructive)|operations[animation.create,animation.rename,animation.delete!,widget.set_property,widget.set_style,binding.create_legacy,binding.update_legacy,binding.replace_legacy!,binding.delete_legacy!,binding.create_mvvm,binding.update_mvvm,binding.replace_mvvm!,binding.delete_mvvm!]|delete_replace_are_destructive"));
	return Value;
}

FString FHyperAIStudioUIContracts::ComputePayloadSemanticFingerprint(
	const FHyperAIStudioUIPlanPayload& Payload)
{
	using namespace HyperAIStudio::UI::Toolset::Private;
	if (!IsCanonicalProjectObjectPath(Payload.TargetPath)
		|| !IsCanonicalSha256(Payload.BasePersistedFingerprint)
		|| !IsCanonicalSha256(Payload.DesiredPersistedFingerprint)
		|| !(Payload.ValidationPolicy == TEXT("structural")
			|| Payload.ValidationPolicy == TEXT("layout_accessibility")
			|| Payload.ValidationPolicy == TEXT("compile_ready"))
		|| Payload.Operations.IsEmpty() || Payload.Operations.Num() > MaxOperations) return FString();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.ui.compound-plan.semantic.v1"));
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BasePersistedFingerprint);
	AppendToken(Canonical, Payload.DesiredPersistedFingerprint);
	AppendToken(Canonical, Payload.ValidationPolicy);
	AppendToken(Canonical, SafetyToken(Payload.Safety));
	EHyperAIStudioUIPlanSafety DerivedSafety = EHyperAIStudioUIPlanSafety::Edit;
	for (const FHyperAIStudioUIBackendOperation& Operation : Payload.Operations)
	{
		const FString KindToken = OperationKindToken(Operation.Kind);
		const EHyperAIStudioUIPlanSafety ExpectedOperationSafety =
			IsDeleteKind(Operation.Kind) || IsReplaceKind(Operation.Kind)
				? EHyperAIStudioUIPlanSafety::Destructive : EHyperAIStudioUIPlanSafety::Edit;
		if (KindToken == TEXT("invalid") || Operation.Safety != ExpectedOperationSafety)
		{
			return FString();
		}
		if (ExpectedOperationSafety == EHyperAIStudioUIPlanSafety::Destructive)
		{
			DerivedSafety = EHyperAIStudioUIPlanSafety::Destructive;
		}
		AppendToken(Canonical, KindToken);
		AppendToken(Canonical, SafetyToken(Operation.Safety));
		AppendToken(Canonical, Operation.SubjectId);
		AppendToken(Canonical, Operation.ExpectedElementFingerprint);
		AppendToken(Canonical, Operation.Name);
		AppendToken(Canonical, Operation.SecondaryName);
		AppendToken(Canonical, Operation.SourceEndpoint);
		AppendToken(Canonical, Operation.SourcePath);
		AppendToken(Canonical, Operation.DestinationEndpoint);
		AppendToken(Canonical, Operation.DestinationPath);
		AppendToken(Canonical, Operation.StringValue);
		AppendToken(Canonical, BoolToken(Operation.bHasBoolValue));
		AppendToken(Canonical, BoolToken(Operation.bBoolValue));
		AppendToken(Canonical, BoolToken(Operation.bHasNumberValue));
		AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Operation.NumberValue));
		AppendToken(Canonical, BoolToken(Operation.bHasColorValue));
		AppendToken(Canonical, FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g"),
			Operation.ColorValue.R, Operation.ColorValue.G,
			Operation.ColorValue.B, Operation.ColorValue.A));
		AppendToken(Canonical, LexToString(Operation.StartFrame));
		AppendToken(Canonical, LexToString(Operation.EndFrame));
		if (Canonical.Len() > MaxCaptureCanonicalCharacters) return FString();
	}
	if (DerivedSafety != Payload.Safety) return FString();
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioUIContracts::GetPreparationDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.ui.preparation-only.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both UI prerequisite groups are authoritative blocking groups. They must
		// never be advertised through the non-blocking authority channel.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_ui_apply_plan"), EditVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), TEXT("hyperai.result.ui.compound-plan.v1"),
			HyperAIStudio::UI::Toolset::Private::ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_ui_apply_plan"), DestructiveVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), TEXT("hyperai.result.ui.compound-plan.v1"),
			HyperAIStudio::UI::Toolset::Private::ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Destructive});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FString FHyperAIStudioUIPlanPayload::GetTypeId() const
{
	return FHyperAIStudioUIContracts::PayloadTypeId;
}

FString FHyperAIStudioUIPlanPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioUIContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioUIPlanPayload::GetBoundedByteSize() const
{
	int64 Size = 256ll + 2ll * (TargetPath.Len() + BasePersistedFingerprint.Len()
		+ DesiredPersistedFingerprint.Len() + ValidationPolicy.Len() + SemanticFingerprint.Len());
	for (const FHyperAIStudioUIBackendOperation& Operation : Operations)
	{
		Size += 256ll + 2ll * (Operation.SubjectId.Len()
			+ Operation.ExpectedElementFingerprint.Len() + Operation.Name.Len()
			+ Operation.SecondaryName.Len() + Operation.SourceEndpoint.Len()
			+ Operation.SourcePath.Len() + Operation.DestinationEndpoint.Len()
			+ Operation.DestinationPath.Len() + Operation.StringValue.Len());
	}
	return static_cast<int32>(FMath::Min<int64>(Size, MAX_int32));
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioUIPlanPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedFingerprint = BasePersistedFingerprint;
	Clone->DesiredPersistedFingerprint = DesiredPersistedFingerprint;
	Clone->ValidationPolicy = ValidationPolicy;
	Clone->SemanticFingerprint = SemanticFingerprint;
	Clone->Safety = Safety;
	Clone->Operations.Reserve(Operations.Num());
	for (const FHyperAIStudioUIBackendOperation& Operation : Operations)
	{
		FHyperAIStudioUIBackendOperation& Copy = Clone->Operations.AddDefaulted_GetRef();
		Copy.Kind = Operation.Kind;
		Copy.Safety = Operation.Safety;
		Copy.SubjectId = Operation.SubjectId;
		Copy.ExpectedElementFingerprint = Operation.ExpectedElementFingerprint;
		Copy.Name = Operation.Name;
		Copy.SecondaryName = Operation.SecondaryName;
		Copy.SourceEndpoint = Operation.SourceEndpoint;
		Copy.SourcePath = Operation.SourcePath;
		Copy.DestinationEndpoint = Operation.DestinationEndpoint;
		Copy.DestinationPath = Operation.DestinationPath;
		Copy.StringValue = Operation.StringValue;
		Copy.bHasBoolValue = Operation.bHasBoolValue;
		Copy.bBoolValue = Operation.bBoolValue;
		Copy.bHasNumberValue = Operation.bHasNumberValue;
		Copy.NumberValue = Operation.NumberValue;
		Copy.bHasColorValue = Operation.bHasColorValue;
		Copy.ColorValue = Operation.ColorValue;
		Copy.StartFrame = Operation.StartFrame;
		Copy.EndFrame = Operation.EndFrame;
	}
	return Clone;
}

FHyperAIUIApplyPlanReport FHyperAIStudioUIContracts::BuildPlan(
	const FHyperAIUIApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::UI::Toolset::Private;
	FHyperAIUIApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic.Left(1024);
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("UI plan preparation requires one fresh bounded game-thread capture."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedFingerprint)
		|| Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 2000
		|| Request.MaxOutputBytes < 4096 || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_bounds_or_cas"),
			TEXT("Plan target, persisted CAS, operation count, deadline, or output bounds are invalid."));
	}
	if (!(Request.ValidationPolicy == TEXT("structural")
		|| Request.ValidationPolicy == TEXT("layout_accessibility")
		|| Request.ValidationPolicy == TEXT("compile_ready")))
	{
		return Reject(TEXT("invalid_validation_policy"),
			TEXT("Validation policy must be structural, layout_accessibility, or compile_ready."));
	}
	const double AbsoluteDeadline = FPlatformTime::Seconds()
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("Dry-run prohibits operation_id and expected_plan_hash."));
	}
	if (!Request.bDryRun
		&& (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
			|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_retry_envelope"),
			TEXT("Non-dry intent requires one journal-safe operation id and exact dry-run plan hash."));
	}
	TArray<FHyperAIStudioUIBackendOperation> Operations;
	Operations.Reserve(Request.Operations.Num());
	EHyperAIStudioUIPlanSafety PlanSafety = EHyperAIStudioUIPlanSafety::Edit;
	TSet<FString> MutatedSubjects;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioUIBackendOperation Backend;
		FString ShapeError;
		if (!ValidateOperationShape(Request.Operations[Index], Backend, ShapeError))
		{
			return Reject(TEXT("operation_shape_invalid"),
				FString::Printf(TEXT("Operation %d failed the closed schema: %s"), Index, *ShapeError));
		}
		if (!Backend.SubjectId.IsEmpty()
			&& !IsCreateKind(Backend.Kind) && MutatedSubjects.Contains(Backend.SubjectId))
		{
			return Reject(TEXT("duplicate_mutated_subject"),
				TEXT("v1 accepts at most one non-create operation per stable subject in a compound plan."));
		}
		if (!Backend.SubjectId.IsEmpty() && !IsCreateKind(Backend.Kind))
		{
			MutatedSubjects.Add(Backend.SubjectId);
		}
		if (Backend.Safety == EHyperAIStudioUIPlanSafety::Destructive)
		{
			PlanSafety = EHyperAIStudioUIPlanSafety::Destructive;
		}
		Operations.Add(MoveTemp(Backend));
	}
	Report.SafetyClass = SafetyToken(PlanSafety);
	Report.VariantId = PlanSafety == EHyperAIStudioUIPlanSafety::Destructive
		? DestructiveVariantId : EditVariantId;
	FHyperAIStudioUIValueSnapshot Base;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioUICapture::Capture({Request.TargetPath}, true, true, true, true,
		true, false, Request.DeadlineMs, Base, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, CaptureDiagnostic);
	}
	if (FPlatformTime::Seconds() > AbsoluteDeadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Plan base capture or fingerprinting exhausted the monotonic deadline."));
	}
	Report.BasePersistedFingerprint = Base.PersistedFingerprint;
	int64 EstimatedOutput = 8192;
	bool bIssueTruncated = false;
	if (!CopyIssuesWithinOutput(Base.CaptureIssues, Request.MaxOutputBytes,
		EstimatedOutput, Report.Issues, bIssueTruncated) || bIssueTruncated)
	{
		return Reject(TEXT("preflight_output_bound_exceeded"),
			TEXT("Fresh capture evidence does not fit the exact output envelope."));
	}
	if (!Base.bComplete)
	{
		return Reject(TEXT("persisted_projection_incomplete"),
			TEXT("Plan preparation requires a complete loaded-only projection; delegated track/source ambiguity remains fail-closed."));
	}
	if (!Base.bPackageEvidenceComplete || !Base.bAllTargetsLoadedFromDisk
		|| !Base.bAllTargetPackagesClean || !Base.bAllTargetClassesExact)
	{
		return Reject(TEXT("persisted_clean_loaded_base_required"),
			TEXT("Plan preparation requires one exact current-class WidgetBlueprint loaded from disk, a clean package, and non-blocking package evidence with nonzero saved hash and positive disk size."));
	}
	if (Base.PersistedFingerprint != Request.ExpectedPersistedFingerprint)
	{
		return Reject(TEXT("stale_persisted_fingerprint"),
			TEXT("The exact loaded Widget Blueprint projection changed after inspection."));
	}
	FHyperAIStudioUIValueSnapshot Desired;
	FHyperAIUIPlanEffects Effects;
	TArray<FHyperAIUIIssue> ShadowIssues;
	FString ShadowError;
	if (!FHyperAIStudioUIValueContracts::ReplayShadowPlan(
		Base, Operations, Desired, Effects, ShadowIssues, ShadowError))
	{
		return Reject(TEXT("shadow_replay_failed"), ShadowError);
	}
	if (FPlatformTime::Seconds() > AbsoluteDeadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Immutable shadow replay exhausted the monotonic deadline."));
	}
	if (Desired.PersistedFingerprint == Base.PersistedFingerprint)
	{
		return Reject(TEXT("plan_has_no_persisted_effect"),
			TEXT("The closed compound plan must change the detached persisted value model."));
	}
	FHyperAIStudioUIValidationOptions Validation;
	Validation.MaxIssues = MaxIssues;
	Validation.bCheckLayout = Request.ValidationPolicy != TEXT("structural");
	Validation.bCheckAccessibility = Request.ValidationPolicy != TEXT("structural");
	Validation.bCheckAnimations = true;
	Validation.bCheckBindings = true;
	Validation.bCheckMVVM = true;
	bool bValidationTruncated = false;
	const TArray<FHyperAIUIIssue> ValidationIssues =
		FHyperAIStudioUIValueValidator::Validate(Desired, Validation, bValidationTruncated);
	if (FPlatformTime::Seconds() > AbsoluteDeadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Desired-state validation exhausted the monotonic deadline."));
	}
	if (bValidationTruncated
		|| !CopyIssuesWithinOutput(ValidationIssues, Request.MaxOutputBytes,
			EstimatedOutput, Report.Issues, bIssueTruncated) || bIssueTruncated)
	{
		return Reject(TEXT("shadow_validation_output_bound_exceeded"),
			TEXT("Desired-state validation did not fit the closed issue/output bound."));
	}
	if (HasError(ValidationIssues))
	{
		return Reject(TEXT("shadow_validation_failed"),
			TEXT("The immutable desired-state replay violates the selected independent validation policy."));
	}
	Report.DesiredPersistedFingerprint = Desired.PersistedFingerprint;
	Report.Effects = Effects;
	const TSharedRef<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioUIPlanPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedFingerprint = Base.PersistedFingerprint;
	Payload->DesiredPersistedFingerprint = Desired.PersistedFingerprint;
	Payload->ValidationPolicy = Request.ValidationPolicy;
	Payload->Safety = PlanSafety;
	Payload->Operations = Operations;
	Payload->SemanticFingerprint = ComputePayloadSemanticFingerprint(*Payload);
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_identity_unavailable"),
			TEXT("The closed compound UI payload could not be semantically sealed."));
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (&Detached.Get() == &Payload.Get()
		|| Detached->GetTypeId() != Payload->GetTypeId()
		|| Detached->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
		|| Detached->GetSemanticFingerprint() != Payload->GetSemanticFingerprint()
		|| Detached->GetBoundedByteSize() != Payload->GetBoundedByteSize())
	{
		return Reject(TEXT("immutable_payload_clone_failed"),
			TEXT("The UI payload failed independent deep immutable clone verification."));
	}
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (ProjectId.IsEmpty())
	{
		return Reject(TEXT("canonical_project_identity_unavailable"),
			TEXT("Typed UI preparation requires the canonical current-project identity."));
	}
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetPreparationDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_ui_apply_plan");
	Binding.VariantId = Report.VariantId;
	Binding.ExpectedSafety = PlanSafety == EHyperAIStudioUIPlanSafety::Destructive
		? EHyperAIStudioDomainSafety::Destructive : EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.UMG"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("plugin.ModelViewViewModel"), EHyperAIStudioDomainPrerequisiteState::Available},
		{LiveProbeId, EHyperAIStudioDomainPrerequisiteState::Missing}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.bDestructiveAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = PayloadTypeId;
	Contract.ArtifactSchemaFingerprint = PayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Contract.EffectTarget = TEXT("widget_blueprint:") + Request.TargetPath
		+ TEXT(":") + Base.PersistedFingerprint;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = Operations.Num() + 4;
	Contract.MaxGameThreadMs = FMath::Min(Request.DeadlineMs, 250);
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bCompileOnce = true;
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), PrepareError);
	}
	if (FPlatformTime::Seconds() > AbsoluteDeadline)
	{
		return Reject(TEXT("deadline_exceeded"),
			TEXT("Pure typed-artifact preparation exhausted the monotonic deadline."));
	}
	Report.bTypedPrepared = true;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_execution_blocked");
		Report.Diagnostic = TEXT("Fresh CAS, closed typed operations, immutable shadow replay, independent desired-state validation, and public pure Prepare hashes are valid. No UObject mutation, compile, save, stage, submission, or external action occurred.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("expected_plan_hash_mismatch"),
			TEXT("Non-dry intent must echo the exact current dry-run plan hash."));
	}
	return Reject(NonDryCallableState,
		TEXT("Zero effects: no plan was staged or submitted and no Widget Blueprint was mutated, compiled, saved, or reloaded. UE 5.8 exposes no proven hard-bounded public mutation+compile+save+fresh route for this closed compound contract."));
}

FHyperAIUIInspectReport UHyperAIStudioUIToolset::hyper_ui_inspect(
	const FHyperAIUIInspectRequest& Request)
{
	return HyperAIStudio::UI::Toolset::Private::Inspect(Request);
}

FHyperAIUIApplyPlanReport UHyperAIStudioUIToolset::hyper_ui_apply_plan(
	const FHyperAIUIApplyPlanRequest& Request)
{
	return FHyperAIStudioUIContracts::BuildPlan(Request);
}

FHyperAIUIValidateReport UHyperAIStudioUIToolset::hyper_ui_validate(
	const FHyperAIUIValidateRequest& Request)
{
	return HyperAIStudio::UI::Toolset::Private::Validate(Request);
}

void FHyperAIStudioUIRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioUIRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioUIRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioUIRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioUIContracts::IsRegistrationAllowed(bDev) && bOwnsToolset
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioUIToolset::StaticClass(),
			FHyperAIStudioUIContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioUIRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()) return;
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioUIContracts::IsRegistrationAllowed(bDev))
	{
		UE_LOG(LogHyperAIStudioUI, Verbose,
			TEXT("UI exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	for (const TCHAR* ModuleName : {TEXT("UMG"), TEXT("UMGEditor"),
		TEXT("ModelViewViewModel"), TEXT("ModelViewViewModelBlueprint")})
	{
		if (!FModuleManager::Get().IsModuleLoaded(ModuleName)) return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioUIToolset::StaticClass(),
		FHyperAIStudioUIContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioUI, Error,
			TEXT("UI three-tool cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioUIRegistration::RollBackRegistration()
{
	if (!bOwnsToolset || !IsInGameThread() || !UObjectInitialized()) return;
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
		UHyperAIStudioUIToolset::StaticClass(),
		FHyperAIStudioUIContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioUI, Error,
			TEXT("UI owned-toolset rollback failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = false;
}
