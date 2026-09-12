// Games by Hyper 2026.

#include "HyperAIStudioEnhancedInputToolset.h"

// Every direct UE EnhancedInput dependency is isolated in this LoadingPhase=None editor module.

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedPlayerInput.h"
#include "EnhancedInputSubsystems.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Internationalization/Text.h"
#include "PlayerMappableKeySettings.h"
#include "ScopedTransaction.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioEnhancedInputToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioEnhancedInput, Log, All);

namespace HyperAIStudio::EnhancedInput::Private
{
	constexpr int32 MaxProfilesPerContext = 32;
	constexpr int32 MaxTextCharacters = 512;
	constexpr int32 MinOutputBytes = 2048;
	constexpr int32 MaxStagedArtifacts = 64;
	constexpr int32 MaxFastReplayRecords = 256;

	struct FFastReplayState
	{
		TMap<FString, FString> CompletedPlanHashes;
		TArray<FString> CompletionOrder;
	};

	FFastReplayState& GetFastReplayState()
	{
		static FFastReplayState State;
		return State;
	}

	void RecordFastCompletion(const FString& OperationId, const FString& PlanHash)
	{
		FFastReplayState& State = GetFastReplayState();
		if (!State.CompletedPlanHashes.Contains(OperationId))
		{
			State.CompletionOrder.Add(OperationId);
		}
		State.CompletedPlanHashes.Add(OperationId, PlanHash);
		while (State.CompletionOrder.Num() > MaxFastReplayRecords)
		{
			const FString Oldest = State.CompletionOrder[0];
			State.CompletionOrder.RemoveAt(0, 1, EAllowShrinking::No);
			State.CompletedPlanHashes.Remove(Oldest);
		}
	}

	void ResetFastReplayState()
	{
		GetFastReplayState() = {};
	}

	FString Clip(const FString& Value, const int32 MaxCharacters = MaxTextCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	bool TryGetPrimaryAssetPackageName(const FString& ObjectPath, FName& OutPackageName)
	{
		OutPackageName = NAME_None;
		if (!FHyperAIStudioEnhancedInputContracts::IsCanonicalProjectObjectPath(ObjectPath))
		{
			return false;
		}
		const FSoftObjectPath Reference(ObjectPath);
		const FString PackageName = Reference.GetLongPackageName();
		if (Reference.GetAssetName() != FPackageName::GetShortName(PackageName)
			|| !FPackageName::IsValidLongPackageName(PackageName))
		{
			return false;
		}
		OutPackageName = FName(*PackageName);
		return !OutPackageName.IsNone();
	}

	bool IsCanonicalSha256(const FString& Value)
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

	bool IsCanonicalProjectId(const FString& Value)
	{
		if (Value.Len() != 40)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	void AppendToken(FString& Buffer, const FString& Value)
	{
		Buffer += FString::FromInt(Value.Len());
		Buffer += TEXT(":");
		Buffer += Value;
		Buffer += TEXT(";");
	}

	void AppendBool(FString& Buffer, const bool Value)
	{
		AppendToken(Buffer, Value ? TEXT("1") : TEXT("0"));
	}

	void AppendInt(FString& Buffer, const int64 Value)
	{
		AppendToken(Buffer, LexToString(Value));
	}

	void AppendDouble(FString& Buffer, const double Value)
	{
		AppendToken(Buffer, FString::Printf(TEXT("%.17g"), Value));
	}

	void AppendVector(FString& Buffer, const FVector& Value)
	{
		AppendDouble(Buffer, Value.X);
		AppendDouble(Buffer, Value.Y);
		AppendDouble(Buffer, Value.Z);
	}

	void AddIssue(
		TArray<FHyperAIInputIssue>& Issues,
		const int32 Maximum,
		bool& bOutTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& AssetPath,
		const FString& StableId,
		const FString& Message)
	{
		if (Issues.Num() >= Maximum)
		{
			bOutTruncated = true;
			return;
		}
		FHyperAIInputIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.AssetPath = Clip(AssetPath, FHyperAIStudioEnhancedInputContracts::MaxPathCharacters);
		Issue.StableId = Clip(StableId, FHyperAIStudioEnhancedInputContracts::MaxPathCharacters);
		Issue.Message = Clip(Message);
	}

	bool HasError(const TArray<FHyperAIInputIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIInputIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	FString ValueTypeToken(const EInputActionValueType Type)
	{
		switch (Type)
		{
		case EInputActionValueType::Boolean: return TEXT("boolean");
		case EInputActionValueType::Axis1D: return TEXT("axis1d");
		case EInputActionValueType::Axis2D: return TEXT("axis2d");
		case EInputActionValueType::Axis3D: return TEXT("axis3d");
		default: return TEXT("unknown");
		}
	}

	FString AccumulationToken(const EInputActionAccumulationBehavior Value)
	{
		return Value == EInputActionAccumulationBehavior::Cumulative
			? TEXT("cumulative") : TEXT("highest_absolute");
	}

	FString RegistrationTrackingToken(const EMappingContextRegistrationTrackingMode Value)
	{
		return Value == EMappingContextRegistrationTrackingMode::CountRegistrations
			? TEXT("count_registrations") : TEXT("untracked");
	}

	FString InputModeFilterToken(const EMappingContextInputModeFilterOptions Value)
	{
		switch (Value)
		{
		case EMappingContextInputModeFilterOptions::UseProjectDefaultQuery:
			return TEXT("project_default");
		case EMappingContextInputModeFilterOptions::UseCustomQuery:
			return TEXT("custom_query");
		case EMappingContextInputModeFilterOptions::DoNotFilter:
			return TEXT("none");
		default:
			return TEXT("unknown");
		}
	}

	FString DeadZoneMode(const EDeadZoneType Value)
	{
		switch (Value)
		{
		case EDeadZoneType::Axial: return TEXT("axial");
		case EDeadZoneType::Radial: return TEXT("radial");
		case EDeadZoneType::UnscaledRadial: return TEXT("unscaled_radial");
		default: return TEXT("unknown");
		}
	}

	FString SwizzleMode(const EInputAxisSwizzle Value)
	{
		switch (Value)
		{
		case EInputAxisSwizzle::YXZ: return TEXT("yxz");
		case EInputAxisSwizzle::ZYX: return TEXT("zyx");
		case EInputAxisSwizzle::XZY: return TEXT("xzy");
		case EInputAxisSwizzle::YZX: return TEXT("yzx");
		case EInputAxisSwizzle::ZXY: return TEXT("zxy");
		default: return TEXT("unknown");
		}
	}

	FString SmoothMode(const ENormalizeInputSmoothingType Value)
	{
		switch (Value)
		{
		case ENormalizeInputSmoothingType::Lerp: return TEXT("lerp");
		case ENormalizeInputSmoothingType::Interp_To: return TEXT("interp_to");
		case ENormalizeInputSmoothingType::Interp_Constant_To: return TEXT("interp_constant_to");
		case ENormalizeInputSmoothingType::Interp_Circular_In: return TEXT("circular_in");
		case ENormalizeInputSmoothingType::Interp_Circular_Out: return TEXT("circular_out");
		case ENormalizeInputSmoothingType::Interp_Circular_In_Out: return TEXT("circular_in_out");
		case ENormalizeInputSmoothingType::Interp_Ease_In: return TEXT("ease_in");
		case ENormalizeInputSmoothingType::Interp_Ease_Out: return TEXT("ease_out");
		case ENormalizeInputSmoothingType::Interp_Ease_In_Out: return TEXT("ease_in_out");
		case ENormalizeInputSmoothingType::Interp_Expo_In: return TEXT("expo_in");
		case ENormalizeInputSmoothingType::Interp_Expo_Out: return TEXT("expo_out");
		case ENormalizeInputSmoothingType::Interp_Expo_In_Out: return TEXT("expo_in_out");
		case ENormalizeInputSmoothingType::Interp_Sin_In: return TEXT("sin_in");
		case ENormalizeInputSmoothingType::Interp_Sin_Out: return TEXT("sin_out");
		case ENormalizeInputSmoothingType::Interp_Sin_In_Out: return TEXT("sin_in_out");
		default: return TEXT("unknown");
		}
	}

	FString FovMode(const EFOVScalingType Value)
	{
		return Value == EFOVScalingType::UE4_BackCompat ? TEXT("ue4_backcompat") : TEXT("standard");
	}

	FString TimedMode(const UInputTriggerTimedBase* Trigger)
	{
		return Trigger && Trigger->bAffectedByTimeDilation ? TEXT("dilated_time") : TEXT("real_time");
	}

	FString ComponentCanonical(const FHyperAIInputComponentSpec& Spec)
	{
		FString Canonical;
		AppendToken(Canonical, Spec.Kind);
		AppendDouble(Canonical, Spec.ActuationThreshold);
		AppendDouble(Canonical, Spec.DurationSeconds);
		AppendDouble(Canonical, Spec.SecondarySeconds);
		AppendInt(Canonical, Spec.Count);
		AppendDouble(Canonical, Spec.LowerThreshold);
		AppendDouble(Canonical, Spec.UpperThreshold);
		AppendBool(Canonical, Spec.bHasVector);
		AppendVector(Canonical, Spec.Vector);
		AppendBool(Canonical, Spec.bHasAxes);
		AppendBool(Canonical, Spec.bX);
		AppendBool(Canonical, Spec.bY);
		AppendBool(Canonical, Spec.bZ);
		AppendBool(Canonical, Spec.bHasOption);
		AppendBool(Canonical, Spec.bOption);
		AppendToken(Canonical, Spec.Mode);
		AppendDouble(Canonical, Spec.Speed);
		AppendDouble(Canonical, Spec.Exponent);
		AppendDouble(Canonical, Spec.Scale);
		return Canonical;
	}

	FHyperAIInputComponentView DescribeTrigger(const UInputTrigger* Trigger)
	{
		FHyperAIInputComponentView View;
		if (!Trigger)
		{
			View.Kind = TEXT("missing");
			View.Config.Kind = View.Kind;
			return View;
		}
		View.ClassPath = Trigger->GetClass()->GetPathName();
		View.Config.ActuationThreshold = Trigger->ActuationThreshold;
		if (Cast<UInputTriggerDown>(Trigger))
		{
			View.Kind = TEXT("trigger.down");
		}
		else if (Cast<UInputTriggerPressed>(Trigger))
		{
			View.Kind = TEXT("trigger.pressed");
		}
		else if (Cast<UInputTriggerReleased>(Trigger))
		{
			View.Kind = TEXT("trigger.released");
		}
		else if (const UInputTriggerHold* Hold = Cast<UInputTriggerHold>(Trigger))
		{
			View.Kind = TEXT("trigger.hold");
			View.Config.DurationSeconds = Hold->HoldTimeThreshold;
			View.Config.Mode = TimedMode(Hold);
			View.Config.bHasOption = true;
			View.Config.bOption = Hold->bIsOneShot;
		}
		else if (const UInputTriggerHoldAndRelease* HoldRelease = Cast<UInputTriggerHoldAndRelease>(Trigger))
		{
			View.Kind = TEXT("trigger.hold_and_release");
			View.Config.DurationSeconds = HoldRelease->HoldTimeThreshold;
			View.Config.Mode = TimedMode(HoldRelease);
		}
		else if (const UInputTriggerTap* Tap = Cast<UInputTriggerTap>(Trigger))
		{
			View.Kind = TEXT("trigger.tap");
			View.Config.DurationSeconds = Tap->TapReleaseTimeThreshold;
			View.Config.Mode = TimedMode(Tap);
		}
		else if (const UInputTriggerRepeatedTap* Repeated = Cast<UInputTriggerRepeatedTap>(Trigger))
		{
			View.Kind = TEXT("trigger.repeated_tap");
			View.Config.DurationSeconds = Repeated->TapReleaseTimeThreshold;
			View.Config.SecondarySeconds = Repeated->RepeatDelay;
			View.Config.Count = Repeated->NumberOfTapsWhichTriggerRepeat;
			View.Config.Mode = TimedMode(Repeated);
		}
		else if (const UInputTriggerPulse* Pulse = Cast<UInputTriggerPulse>(Trigger))
		{
			View.Kind = TEXT("trigger.pulse");
			View.Config.DurationSeconds = Pulse->Interval;
			View.Config.Count = Pulse->TriggerLimit;
			View.Config.Mode = TimedMode(Pulse);
			View.Config.bHasOption = true;
			View.Config.bOption = Pulse->bTriggerOnStart;
		}
		else
		{
			View.Kind = TEXT("unsupported");
			View.Config.Kind = View.Kind;
			return View;
		}
		View.bSupported = true;
		View.Config.Kind = View.Kind;
		return View;
	}

	FHyperAIInputComponentView DescribeModifier(const UInputModifier* Modifier)
	{
		FHyperAIInputComponentView View;
		if (!Modifier)
		{
			View.Kind = TEXT("missing");
			View.Config.Kind = View.Kind;
			return View;
		}
		View.ClassPath = Modifier->GetClass()->GetPathName();
		if (const UInputModifierDeadZone* DeadZone = Cast<UInputModifierDeadZone>(Modifier))
		{
			View.Kind = TEXT("modifier.dead_zone");
			View.Config.LowerThreshold = DeadZone->LowerThreshold;
			View.Config.UpperThreshold = DeadZone->UpperThreshold;
			View.Config.Mode = DeadZoneMode(DeadZone->Type);
		}
		else if (const UInputModifierScalar* Scalar = Cast<UInputModifierScalar>(Modifier))
		{
			View.Kind = TEXT("modifier.scalar");
			View.Config.bHasVector = true;
			View.Config.Vector = Scalar->Scalar;
		}
		else if (const UInputModifierNegate* Negate = Cast<UInputModifierNegate>(Modifier))
		{
			View.Kind = TEXT("modifier.negate");
			View.Config.bHasAxes = true;
			View.Config.bX = Negate->bX;
			View.Config.bY = Negate->bY;
			View.Config.bZ = Negate->bZ;
		}
		else if (const UInputModifierSwizzleAxis* Swizzle = Cast<UInputModifierSwizzleAxis>(Modifier))
		{
			View.Kind = TEXT("modifier.swizzle");
			View.Config.Mode = SwizzleMode(Swizzle->Order);
		}
		else if (Cast<UInputModifierSmooth>(Modifier))
		{
			View.Kind = TEXT("modifier.smooth");
		}
		else if (const UInputModifierSmoothDelta* SmoothDelta = Cast<UInputModifierSmoothDelta>(Modifier))
		{
			View.Kind = TEXT("modifier.smooth_delta");
			View.Config.Mode = SmoothMode(SmoothDelta->SmoothingMethod);
			View.Config.Speed = SmoothDelta->Speed;
			View.Config.Exponent = SmoothDelta->EasingExponent;
		}
		else if (const UInputModifierResponseCurveExponential* Response =
			Cast<UInputModifierResponseCurveExponential>(Modifier))
		{
			View.Kind = TEXT("modifier.response_exponential");
			View.Config.bHasVector = true;
			View.Config.Vector = Response->CurveExponent;
		}
		else if (Cast<UInputModifierScaleByDeltaTime>(Modifier))
		{
			View.Kind = TEXT("modifier.scale_by_delta_time");
		}
		else if (const UInputModifierFOVScaling* Fov = Cast<UInputModifierFOVScaling>(Modifier))
		{
			View.Kind = TEXT("modifier.fov_scaling");
			View.Config.Scale = Fov->FOVScale;
			View.Config.Mode = FovMode(Fov->FOVScalingType);
		}
		else if (Cast<UInputModifierToWorldSpace>(Modifier))
		{
			View.Kind = TEXT("modifier.to_world_space");
		}
		else
		{
			View.Kind = TEXT("unsupported");
			View.Config.Kind = View.Kind;
			return View;
		}
		View.bSupported = true;
		View.Config.Kind = View.Kind;
		return View;
	}

	FString MappingCanonical(
		const FString& ContextPath,
		const FString& ProfileId,
		const int32 MappingIndex,
		const FEnhancedActionKeyMapping& Mapping,
		TArray<FHyperAIInputComponentView>& OutTriggers,
		TArray<FHyperAIInputComponentView>& OutModifiers,
		bool& bOutComplete)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.enhanced-input-mapping.v1"));
		AppendToken(Canonical, ContextPath);
		AppendToken(Canonical, ProfileId);
		AppendInt(Canonical, MappingIndex);
		AppendToken(Canonical, GetPathNameSafe(Mapping.Action));
		AppendToken(Canonical, Mapping.Key.GetFName().ToString());
		AppendBool(Canonical, Mapping.IsPlayerMappable());
		const FString MappingName = Mapping.GetMappingName().ToString();
		const FString& DisplayName = FTextInspector::GetDisplayString(Mapping.GetDisplayName());
		const FString& DisplayCategory =
			FTextInspector::GetDisplayString(Mapping.GetDisplayCategory());
		if (MappingName.Len() > 128 || DisplayName.Len() > MaxTextCharacters
			|| DisplayCategory.Len() > MaxTextCharacters)
		{
			bOutComplete = false;
		}
		AppendToken(Canonical, MappingName.Left(128));
		AppendToken(Canonical, DisplayName.Left(MaxTextCharacters));
		AppendToken(Canonical, DisplayCategory.Left(MaxTextCharacters));
		if (const UPlayerMappableKeySettings* Settings = Mapping.GetPlayerMappableKeySettings())
		{
			AppendToken(Canonical, GetPathNameSafe(Settings->Metadata));
			const TArray<FString>& SourceProfiles = Settings->SupportedKeyProfileIds;
			if (SourceProfiles.Num() > MaxProfilesPerContext)
			{
				bOutComplete = false;
			}
			TArray<FString> Profiles;
			const int32 ProfileCount = FMath::Min(SourceProfiles.Num(), MaxProfilesPerContext);
			Profiles.Reserve(ProfileCount);
			for (int32 Index = 0; Index < ProfileCount; ++Index)
			{
				const FString& Profile = SourceProfiles[Index];
				if (Profile.IsEmpty()
					|| Profile.Len() > FHyperAIStudioEnhancedInputContracts::MaxProfileCharacters)
				{
					bOutComplete = false;
					continue;
				}
				Profiles.Add(Profile);
			}
			Profiles.Sort();
			for (const FString& Profile : Profiles)
			{
				AppendToken(Canonical, Profile);
			}
		}
		if (Mapping.Triggers.Num() > FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner
			|| Mapping.Modifiers.Num() > FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner)
		{
			bOutComplete = false;
		}
		const int32 TriggerCount = FMath::Min(
			Mapping.Triggers.Num(), FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner);
		for (int32 Index = 0; Index < TriggerCount; ++Index)
		{
			FHyperAIInputComponentView View = DescribeTrigger(Mapping.Triggers[Index]);
			bOutComplete &= View.bSupported;
			AppendToken(Canonical, View.ClassPath);
			AppendToken(Canonical, ComponentCanonical(View.Config));
			OutTriggers.Add(MoveTemp(View));
		}
		const int32 ModifierCount = FMath::Min(
			Mapping.Modifiers.Num(), FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner);
		for (int32 Index = 0; Index < ModifierCount; ++Index)
		{
			FHyperAIInputComponentView View = DescribeModifier(Mapping.Modifiers[Index]);
			bOutComplete &= View.bSupported;
			AppendToken(Canonical, View.ClassPath);
			AppendToken(Canonical, ComponentCanonical(View.Config));
			OutModifiers.Add(MoveTemp(View));
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	UObject* ResolveLoadedInputAsset(
		const FString& AssetPath,
		FString& OutStatus,
		FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		if (!IsInGameThread())
		{
			OutStatus = TEXT("game_thread_required");
			OutDiagnostic = TEXT("Enhanced Input object capture is serialized on Unreal's game thread.");
			return nullptr;
		}
		if (!FHyperAIStudioEnhancedInputContracts::IsCanonicalProjectObjectPath(AssetPath))
		{
			OutStatus = TEXT("invalid_asset_path");
			OutDiagnostic = TEXT("Enhanced Input mutations and exact reads require one canonical /Game object path.");
			return nullptr;
		}
		const FSoftObjectPath Reference(AssetPath);
		UObject* Object = Reference.ResolveObject();
		if (!Object)
		{
			OutStatus = TEXT("asset_not_loaded");
			OutDiagnostic = TEXT("Open the exact Input Action or Mapping Context first; HyperAI never synchronously loads it on the MCP game thread.");
			return nullptr;
		}
		if (Object->GetPathName() != AssetPath
			|| (!Object->IsA<UInputAction>() && !Object->IsA<UInputMappingContext>()))
		{
			OutStatus = TEXT("wrong_loaded_type");
			OutDiagnostic = TEXT("The exact loaded object is not a UInputAction or UInputMappingContext.");
			return nullptr;
		}
		return Object;
	}

	struct FCaptureOptions
	{
		bool bIncludeProfiles = true;
		bool bIncludeRuntimeBindings = true;
	};

	void AddCaptureIssue(
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& AssetPath,
		const FString& StableId,
		const FString& Message)
	{
		bool bIgnored = false;
		AddIssue(Snapshot.CaptureIssues, FHyperAIStudioEnhancedInputContracts::MaxIssues,
			bIgnored, Code, Severity, AssetPath, StableId, Message);
		if (FCString::Strcmp(Severity, TEXT("error")) == 0)
		{
			Snapshot.bComplete = false;
		}
	}

	void CapturePlayerMappableSettings(
		const UPlayerMappableKeySettings* Settings,
		FHyperAIInputRecord& Record,
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
	{
		if (!Settings)
		{
			return;
		}
		Record.bPlayerMappable = true;
		Record.MappingName = Clip(Settings->GetMappingName().ToString(), 128);
		const FString& DisplayName = FTextInspector::GetDisplayString(Settings->DisplayName);
		const FString& DisplayCategory =
			FTextInspector::GetDisplayString(Settings->DisplayCategory);
		if (DisplayName.Len() > MaxTextCharacters || DisplayCategory.Len() > MaxTextCharacters)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("player_mapping_text_bound"), TEXT("error"),
				Record.AssetPath, Record.StableId,
				TEXT("Player-mappable display text exceeds the exact revision/output bound."));
		}
		Record.DisplayName = Clip(DisplayName);
		Record.DisplayCategory = Clip(DisplayCategory);
		Record.MetadataPath = GetPathNameSafe(Settings->Metadata);
		const TArray<FString>& SourceProfiles = Settings->SupportedKeyProfileIds;
		if (SourceProfiles.Num() > MaxProfilesPerContext)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("supported_profile_bound"), TEXT("error"),
				Record.AssetPath, Record.StableId,
				TEXT("Player-mappable supported-profile count exceeds the hard bound."));
		}
		const int32 ProfileCount = FMath::Min(SourceProfiles.Num(), MaxProfilesPerContext);
		Record.SupportedProfiles.Reserve(ProfileCount);
		bool bInvalidProfile = false;
		for (int32 Index = 0; Index < ProfileCount; ++Index)
		{
			const FString& Profile = SourceProfiles[Index];
			if (Profile.IsEmpty() || Profile.Len() > FHyperAIStudioEnhancedInputContracts::MaxProfileCharacters)
			{
				bInvalidProfile = true;
				continue;
			}
			Record.SupportedProfiles.Add(Profile);
		}
		Record.SupportedProfiles.Sort();
		if (bInvalidProfile)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("invalid_supported_profile"), TEXT("error"),
				Record.AssetPath, Record.StableId,
				TEXT("A player-mappable supported-profile identifier is empty or out of bounds."));
		}
	}

	void AddActionToSnapshot(
		const UInputAction* Action,
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
	{
		if (!Action || Snapshot.ActionPaths.Contains(Action->GetPathName()))
		{
			return;
		}
		const FString Path = Action->GetPathName();
		if (Snapshot.Records.Num() >= FHyperAIStudioEnhancedInputContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		Snapshot.ActionPaths.Add(Path);
		Snapshot.ActionValueTypes.Add(Path, ValueTypeToken(Action->ValueType));
		FHyperAIInputRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
		Record.Kind = TEXT("action");
		Record.StableId = TEXT("action:") + Path;
		Record.AssetPath = Path;
		Record.ValueType = ValueTypeToken(Action->ValueType);
		const FString ActionDescription = Action->ActionDescription.ToString();
		if (ActionDescription.Len() > MaxTextCharacters)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("action_description_bound"), TEXT("error"), Path,
				Record.StableId, TEXT("Action description exceeds the exact revision/output bound."));
		}
		Record.Description = Clip(ActionDescription);
		Record.bPackageDirty = Action->GetOutermost() && Action->GetOutermost()->IsDirty();
		CapturePlayerMappableSettings(Action->GetPlayerMappableKeySettings(), Record, Snapshot);
		Record.bTriggerWhenPaused = Action->bTriggerWhenPaused;
		Record.bConsumeInput = Action->bConsumeInput;
		Record.bReserveAllMappings = Action->bReserveAllMappings;
		Record.bConsumesLegacyMappings = Action->bConsumesActionAndAxisMappings;
		Record.LegacyConsumeEvents = Action->TriggerEventsThatConsumeLegacyKeys;
		Record.AccumulationBehavior = AccumulationToken(Action->AccumulationBehavior);
		if (Action->Triggers.Num() > FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner
			|| Action->Modifiers.Num() > FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("component_bound_exceeded"), TEXT("error"), Path,
				Record.StableId, TEXT("Action component count exceeds the hard capture bound."));
		}
		const int32 TriggerCount = FMath::Min(
			Action->Triggers.Num(), FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner);
		for (int32 Index = 0; Index < TriggerCount; ++Index)
		{
			FHyperAIInputComponentView View = DescribeTrigger(Action->Triggers[Index]);
			if (!View.bSupported)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("unsupported_trigger"), TEXT("warning"), Path,
					Record.StableId, TEXT("A custom/unsupported trigger prevents an exact mutation revision."));
			}
			Record.Triggers.Add(MoveTemp(View));
		}
		const int32 ModifierCount = FMath::Min(
			Action->Modifiers.Num(), FHyperAIStudioEnhancedInputContracts::MaxComponentsPerOwner);
		for (int32 Index = 0; Index < ModifierCount; ++Index)
		{
			FHyperAIInputComponentView View = DescribeModifier(Action->Modifiers[Index]);
			if (!View.bSupported)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("unsupported_modifier"), TEXT("warning"), Path,
					Record.StableId, TEXT("A custom/unsupported modifier prevents an exact mutation revision."));
			}
			Record.Modifiers.Add(MoveTemp(View));
		}
	}

	void AddMappingToSnapshot(
		const UInputMappingContext* Context,
		const FString& ProfileId,
		const int32 MappingIndex,
		const FEnhancedActionKeyMapping& Mapping,
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
	{
		if (Snapshot.Mappings.Num() >= FHyperAIStudioEnhancedInputContracts::MaxMappings
			|| Snapshot.Records.Num() >= FHyperAIStudioEnhancedInputContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		if (Mapping.Action)
		{
			AddActionToSnapshot(Mapping.Action, Snapshot);
		}
		FHyperAIInputRecord Record;
		Record.Kind = ProfileId.IsEmpty() ? TEXT("mapping") : TEXT("profile_mapping");
		Record.AssetPath = Context->GetPathName();
		Record.ParentPath = Context->GetPathName();
		Record.ActionPath = GetPathNameSafe(Mapping.Action);
		Record.Key = Mapping.Key.GetFName().ToString();
		Record.ValueType = Mapping.Action ? ValueTypeToken(Mapping.Action->ValueType) : FString();
		Record.ProfileId = ProfileId;
		Record.MappingIndex = MappingIndex;
		Record.bPlayerMappable = Mapping.IsPlayerMappable();
		Record.MappingName = Clip(Mapping.GetMappingName().ToString(), 128);
		Record.DisplayName = Clip(Mapping.GetDisplayName().ToString());
		Record.DisplayCategory = Clip(Mapping.GetDisplayCategory().ToString());
		Record.bPackageDirty = Context->GetOutermost() && Context->GetOutermost()->IsDirty();
		CapturePlayerMappableSettings(Mapping.GetPlayerMappableKeySettings(), Record, Snapshot);
		bool bMappingComplete = true;
		Record.MappingFingerprint = MappingCanonical(Context->GetPathName(), ProfileId, MappingIndex,
			Mapping, Record.Triggers, Record.Modifiers, bMappingComplete);
		Record.StableId = TEXT("mapping:") + Context->GetPathName() + TEXT(":")
			+ (ProfileId.IsEmpty() ? TEXT("default") : ProfileId) + TEXT(":")
			+ FString::FromInt(MappingIndex) + TEXT(":") + Record.MappingFingerprint.Mid(7, 16);
		if (!bMappingComplete || !IsCanonicalSha256(Record.MappingFingerprint))
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("mapping_revision_incomplete"), TEXT("warning"),
				Context->GetPathName(), Record.StableId,
				TEXT("Mapping contains unsupported/unbounded component state; mutation is fail-closed."));
		}

		FHyperAIStudioEnhancedInputMappingValue& Value = Snapshot.Mappings.AddDefaulted_GetRef();
		Value.ContextPath = Context->GetPathName();
		Value.ProfileId = ProfileId;
		Value.MappingIndex = MappingIndex;
		Value.ActionPath = Record.ActionPath;
		Value.Key = Record.Key;
		Value.ActionValueType = Record.ValueType;
		Value.MappingFingerprint = Record.MappingFingerprint;
		Value.bKeyValid = Mapping.Key.IsValid();
		Value.bRevisionComplete = bMappingComplete;
		for (const FHyperAIInputComponentView& Trigger : Record.Triggers)
		{
			Value.Triggers.Add(Trigger.Config);
		}
		for (const FHyperAIInputComponentView& Modifier : Record.Modifiers)
		{
			Value.Modifiers.Add(Modifier.Config);
		}
		Snapshot.Records.Add(MoveTemp(Record));
	}

	void AddContextToSnapshot(
		const UInputMappingContext* Context,
		const FCaptureOptions& Options,
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
	{
		if (!Context || Snapshot.ContextPaths.Contains(Context->GetPathName()))
		{
			return;
		}
		const FString Path = Context->GetPathName();
		if (Snapshot.Records.Num() >= FHyperAIStudioEnhancedInputContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return;
		}
		Snapshot.ContextPaths.Add(Path);
		const int32 ContextRecordIndex = Snapshot.Records.AddDefaulted();
		FHyperAIInputRecord& ContextRecord = Snapshot.Records[ContextRecordIndex];
		ContextRecord.Kind = TEXT("mapping_context");
		ContextRecord.StableId = TEXT("context:") + Path;
		ContextRecord.AssetPath = Path;
		const FString ContextDescription = Context->ContextDescription.ToString();
		const FString QueryDescription = Context->GetInputModeQuery().GetDescription();
		if (ContextDescription.Len() > MaxTextCharacters || QueryDescription.Len() > MaxTextCharacters)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("context_text_bound"), TEXT("error"), Path,
				ContextRecord.StableId,
				TEXT("Context description/query exceeds the exact revision/output bound."));
		}
		ContextRecord.Description = Clip(ContextDescription);
		ContextRecord.bPackageDirty = Context->GetOutermost() && Context->GetOutermost()->IsDirty();
		ContextRecord.RegistrationTrackingMode = RegistrationTrackingToken(Context->GetRegistrationTrackingMode());
		ContextRecord.InputModeFilter = InputModeFilterToken(Context->GetInputModeFilterOptions());
		ContextRecord.InputModeQueryDescription = Clip(QueryDescription);
		const TArray<FEnhancedActionKeyMapping>& Defaults = Context->GetMappings();
		ContextRecord.MappingCount = Defaults.Num();
		if (Defaults.Num() > FHyperAIStudioEnhancedInputContracts::MaxMappings)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("mapping_bound_exceeded"), TEXT("error"), Path,
				ContextRecord.StableId, TEXT("Default mapping count exceeds the hard capture bound."));
		}
		const int32 DefaultCount = FMath::Min(Defaults.Num(), FMath::Max(0,
			FHyperAIStudioEnhancedInputContracts::MaxMappings - Snapshot.Mappings.Num()));
		for (int32 Index = 0; Index < DefaultCount; ++Index)
		{
			AddMappingToSnapshot(Context, FString(), Index, Defaults[Index], Snapshot);
		}
		if (!Options.bIncludeProfiles)
		{
			return;
		}

		TArray<FString> Profiles = Context->GetProfilesWithOverridenMappings();
		Snapshot.Records[ContextRecordIndex].ProfileCount = Profiles.Num();
		if (Profiles.Num() > MaxProfilesPerContext)
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("profile_bound_exceeded"), TEXT("error"), Path,
				Snapshot.Records[ContextRecordIndex].StableId, TEXT("Profile count exceeds the hard expansion bound."));
			return;
		}
		for (const FString& ProfileId : Profiles)
		{
			if (ProfileId.IsEmpty() || ProfileId.Len() > FHyperAIStudioEnhancedInputContracts::MaxProfileCharacters)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("invalid_profile_id"), TEXT("error"), Path,
					Snapshot.Records[ContextRecordIndex].StableId, TEXT("Profile identifier is empty or exceeds the contract bound."));
			}
		}
		Profiles.Sort();
		for (const FString& ProfileId : Profiles)
		{
			if (ProfileId.IsEmpty()
				|| ProfileId.Len() > FHyperAIStudioEnhancedInputContracts::MaxProfileCharacters)
			{
				continue;
			}
			const TArray<FEnhancedActionKeyMapping>& Mappings =
				Context->GetMappingsForProfile(ProfileId);
			if (Mappings.Num() > FHyperAIStudioEnhancedInputContracts::MaxMappings)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("profile_mapping_bound_exceeded"), TEXT("error"), Path,
					Snapshot.Records[ContextRecordIndex].StableId, TEXT("Profile mapping count exceeds the hard capture bound."));
			}
			const int32 Count = FMath::Min(Mappings.Num(), FMath::Max(0,
				FHyperAIStudioEnhancedInputContracts::MaxMappings - Snapshot.Mappings.Num()));
			for (int32 Index = 0; Index < Count; ++Index)
			{
				AddMappingToSnapshot(Context, ProfileId, Index, Mappings[Index], Snapshot);
			}
		}
	}

	bool TryGetAppliedContextPriority(
		const UEnhancedPlayerInput* ExpectedPlayerInput,
		const UInputMappingContext* Context,
		int32& OutPriority)
	{
		OutPriority = INDEX_NONE;
		if (!ExpectedPlayerInput || !Context)
		{
			return false;
		}
		int32 OwnersScanned = 0;
		auto TrySubsystem = [ExpectedPlayerInput, Context, &OutPriority, &OwnersScanned](
			IEnhancedInputSubsystemInterface* Subsystem)
		{
			if (!Subsystem
				|| OwnersScanned >= FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned)
			{
				return false;
			}
			++OwnersScanned;
			return Subsystem->GetPlayerInput() == ExpectedPlayerInput
				&& Subsystem->HasMappingContext(Context, OutPriority);
		};
		for (TObjectIterator<UEnhancedInputLocalPlayerSubsystem> It;
			It && OwnersScanned < FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned; ++It)
		{
			if (TrySubsystem(*It))
			{
				return true;
			}
		}
		for (TObjectIterator<UEnhancedInputWorldSubsystem> It;
			It && OwnersScanned < FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned; ++It)
		{
			if (TrySubsystem(*It))
			{
				return true;
			}
		}
		OutPriority = INDEX_NONE;
		return false;
	}

	void AddRuntimeBindings(
		const FCaptureOptions& Options,
		FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
	{
		if (!Options.bIncludeRuntimeBindings)
		{
			return;
		}
		TArray<FString> ContextPaths = Snapshot.ContextPaths.Array();
		ContextPaths.Sort();
		int32 OwnersScanned = 0;
		auto AppendFromSubsystem = [&Options, &Snapshot, &ContextPaths, &OwnersScanned](
			IEnhancedInputSubsystemInterface* Subsystem)
		{
			if (!Subsystem
				|| OwnersScanned >= FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned)
			{
				return;
			}
			UEnhancedPlayerInput* PlayerInput = Subsystem->GetPlayerInput();
			++OwnersScanned;
			if (!PlayerInput || PlayerInput->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				return;
			}
			int32 BindingIndex = 0;
			for (const FString& ContextPath : ContextPaths)
			{
				if (Snapshot.Records.Num() >= FHyperAIStudioEnhancedInputContracts::MaxRecords
					|| BindingIndex >= FHyperAIStudioEnhancedInputContracts::MaxMappings)
				{
					Snapshot.bComplete = false;
					break;
				}
				const UInputMappingContext* Context = Cast<UInputMappingContext>(
					FSoftObjectPath(ContextPath).ResolveObject());
				int32 Priority = INDEX_NONE;
				if (!Context || !Subsystem->HasMappingContext(Context, Priority))
				{
					continue;
				}
				AddContextToSnapshot(Context, Options, Snapshot);
				FHyperAIInputRecord& Record = Snapshot.Records.AddDefaulted_GetRef();
				Record.Kind = TEXT("runtime_binding");
				Record.StableId = TEXT("binding:") + PlayerInput->GetPathName() + TEXT(":")
					+ ContextPath;
				Record.AssetPath = ContextPath;
				Record.ParentPath = PlayerInput->GetPathName();
				Record.Priority = Priority;
				// UE 5.8 exposes applied priority publicly through the subsystem, but the
				// registration count remains protected on UEnhancedPlayerInput.
				Record.RegistrationCount = 0;
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("runtime_registration_count_not_public"),
					TEXT("info"), ContextPath, Record.StableId,
					TEXT("Applied priority is public; registration count is unavailable through the UE 5.8 public API."));
				++BindingIndex;
			}
		};
		for (TObjectIterator<UEnhancedInputLocalPlayerSubsystem> It;
			It && OwnersScanned < FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned; ++It)
		{
			AppendFromSubsystem(*It);
		}
		for (TObjectIterator<UEnhancedInputWorldSubsystem> It;
			It && OwnersScanned < FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned; ++It)
		{
			AppendFromSubsystem(*It);
		}
	}

	FHyperAIStudioEnhancedInputValueSnapshot CaptureSnapshot(
		const TArray<FString>& AssetPaths,
		const FCaptureOptions& Options)
	{
		FHyperAIStudioEnhancedInputValueSnapshot Snapshot;
		bool bIssueTruncated = false;
		AddIssue(Snapshot.CaptureIssues, FHyperAIStudioEnhancedInputContracts::MaxIssues,
			bIssueTruncated, TEXT("loaded_only_scope"), TEXT("info"), FString(), FString(),
			TEXT("Existing assets are inspected only when already loaded; no synchronous load occurs."));
		if (!AssetPaths.IsEmpty())
		{
			TSet<FString> Seen;
			for (const FString& Path : AssetPaths)
			{
				if (Seen.Contains(Path))
				{
					continue;
				}
				Seen.Add(Path);
				++Snapshot.LoadedObjectsScanned;
				FString Status;
				FString Diagnostic;
				UObject* Object = ResolveLoadedInputAsset(Path, Status, Diagnostic);
				if (const UInputAction* Action = Cast<UInputAction>(Object))
				{
					AddActionToSnapshot(Action, Snapshot);
				}
				else if (const UInputMappingContext* Context = Cast<UInputMappingContext>(Object))
				{
					AddContextToSnapshot(Context, Options, Snapshot);
				}
				else
				{
					AddCaptureIssue(Snapshot, *Status, TEXT("error"), Path, Path, Diagnostic);
				}
			}
		}
		else
		{
			int32 Scanned = 0;
			for (TObjectIterator<UInputAction> It;
				It && Scanned < FHyperAIStudioEnhancedInputContracts::MaxLoadedObjectsScanned; ++It)
			{
				++Scanned;
				const UInputAction* Action = *It;
				if (Action && !Action->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
					&& Action->GetPathName().StartsWith(TEXT("/Game/")))
				{
					AddActionToSnapshot(Action, Snapshot);
				}
			}
			for (TObjectIterator<UInputMappingContext> It;
				It && Scanned < FHyperAIStudioEnhancedInputContracts::MaxLoadedObjectsScanned; ++It)
			{
				++Scanned;
				const UInputMappingContext* Context = *It;
				if (Context && !Context->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
					&& Context->GetPathName().StartsWith(TEXT("/Game/")))
				{
					AddContextToSnapshot(Context, Options, Snapshot);
				}
			}
			Snapshot.LoadedObjectsScanned = Scanned;
			if (Scanned >= FHyperAIStudioEnhancedInputContracts::MaxLoadedObjectsScanned)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("loaded_object_scan_bound"), TEXT("warning"), FString(), FString(),
					TEXT("Loaded-object enumeration reached its hard scan bound."));
			}
		}
		AddRuntimeBindings(Options, Snapshot);

		TMap<FString, int32> References;
		for (const FHyperAIStudioEnhancedInputMappingValue& Mapping : Snapshot.Mappings)
		{
			if (!Mapping.ActionPath.IsEmpty())
			{
				++References.FindOrAdd(Mapping.ActionPath);
			}
		}
		for (FHyperAIInputRecord& Record : Snapshot.Records)
		{
			if (Record.Kind == TEXT("action"))
			{
				Record.ReferenceCount = References.FindRef(Record.AssetPath);
			}
		}
		FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(Snapshot);
		return Snapshot;
	}

	int32 EstimateRecordBytes(const FHyperAIInputRecord& Record)
	{
		int32 Characters = 512 + Record.Kind.Len() + Record.StableId.Len() + Record.AssetPath.Len()
			+ Record.ParentPath.Len() + Record.ActionPath.Len() + Record.Key.Len() + Record.ValueType.Len()
			+ Record.Description.Len() + Record.ProfileId.Len() + Record.MappingFingerprint.Len()
			+ Record.MappingName.Len() + Record.DisplayName.Len() + Record.DisplayCategory.Len()
			+ Record.MetadataPath.Len()
			+ Record.RegistrationTrackingMode.Len() + Record.InputModeFilter.Len()
			+ Record.InputModeQueryDescription.Len() + Record.AccumulationBehavior.Len();
		for (const FString& Profile : Record.SupportedProfiles)
		{
			Characters += 24 + Profile.Len();
		}
		for (const FHyperAIInputComponentView& Component : Record.Triggers)
		{
			Characters += 320 + Component.Kind.Len() + Component.ClassPath.Len() + Component.Config.Mode.Len();
		}
		for (const FHyperAIInputComponentView& Component : Record.Modifiers)
		{
			Characters += 320 + Component.Kind.Len() + Component.ClassPath.Len() + Component.Config.Mode.Len();
		}
		return Characters * static_cast<int32>(sizeof(TCHAR));
	}

	FString MakeRequestFingerprint(const FHyperAIInputInspectRequest& Request)
	{
		TArray<FString> Paths = Request.AssetPaths;
		Paths.Sort();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.enhanced-input-inspect-request.v1"));
		for (const FString& Path : Paths) AppendToken(Canonical, Path);
		AppendBool(Canonical, Request.bIncludeMappings);
		AppendBool(Canonical, Request.bIncludeProfiles);
		AppendBool(Canonical, Request.bIncludeComponents);
		AppendBool(Canonical, Request.bIncludeReferenceEvidence);
		AppendBool(Canonical, Request.bIncludeRuntimeBindings);
		AppendInt(Canonical, Request.PageSize);
		AppendInt(Canonical, Request.MaxOutputBytes);
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString MakeCursor(const FString& RequestHash, const FString& Revision, const int32 Offset)
	{
		if (!IsCanonicalSha256(RequestHash) || !IsCanonicalSha256(Revision) || Offset < 0)
		{
			return FString();
		}
		return RequestHash.Mid(7) + TEXT(":") + Revision.Mid(7, 16) + TEXT(":") + FString::FromInt(Offset);
	}

	bool ParseCursor(
		const FString& Cursor,
		const FString& RequestHash,
		const FString& Revision,
		const int32 Total,
		int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty())
		{
			return true;
		}
		if (Cursor.Len() > FHyperAIStudioEnhancedInputContracts::MaxCursorCharacters)
		{
			return false;
		}
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT(":"), false);
		if (Parts.Num() != 3 || Parts[0] != RequestHash.Mid(7)
			|| Parts[1] != Revision.Mid(7, 16) || !LexTryParseString(OutOffset, *Parts[2])
			|| OutOffset < 0 || OutOffset > Total)
		{
			OutOffset = 0;
			return false;
		}
		return true;
	}

	bool IsDefaultConfigFields(const FHyperAIInputPlanOperation& Operation)
	{
		return !Operation.bSetTriggerWhenPaused && !Operation.bTriggerWhenPaused
			&& !Operation.bSetConsumeInput && Operation.bConsumeInput
			&& !Operation.bSetReserveAllMappings && !Operation.bReserveAllMappings
			&& !Operation.bSetAccumulationBehavior && Operation.AccumulationBehavior.IsEmpty()
			&& Operation.ValueType.IsEmpty();
	}

	bool IsDefaultMappingFields(const FHyperAIInputPlanOperation& Operation)
	{
		return Operation.ActionPath.IsEmpty() && Operation.Key.IsEmpty() && Operation.ProfileId.IsEmpty()
			&& Operation.MappingIndex == -1 && Operation.ExpectedMappingFingerprint.IsEmpty();
	}

	bool IsDefaultRuntimeFields(const FHyperAIInputPlanOperation& Operation)
	{
		return Operation.RuntimeOwnerPath.IsEmpty() && Operation.Priority == 0;
	}

	bool IsDefaultComponents(const FHyperAIInputPlanOperation& Operation)
	{
		return Operation.Triggers.IsEmpty() && Operation.Modifiers.IsEmpty();
	}

	int64 NowUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000ll + Now.GetMillisecond();
	}
}

FString FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioEnhancedInput.HyperAIStudioEnhancedInputToolset");
}

const TArray<FHyperAIStudioEnhancedInputManifestEntry>&
FHyperAIStudioEnhancedInputContracts::GetManifest()
{
	static const TArray<FHyperAIStudioEnhancedInputManifestEntry> Manifest = {
		{TEXT("hyper_input_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_input_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_input_validate"), GetQualifiedToolsetName()}
	};
	return Manifest;
}

bool FHyperAIStudioEnhancedInputContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioEnhancedInputManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TArray<FString> Names;
	TSet<FString> UniqueNames;
	for (const FHyperAIStudioEnhancedInputManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| UniqueNames.Contains(Entry.Name))
		{
			return false;
		}
		UniqueNames.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioEnhancedInputContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || Path.Contains(TEXT(".."))
		|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidObjectPath(Path))
	{
		return false;
	}
	const FSoftObjectPath Reference(Path);
	const FString PackageName = Reference.GetLongPackageName();
	const FString AssetName = Reference.GetAssetName();
	return Reference.IsValid() && FPackageName::IsValidLongPackageName(PackageName)
		&& !AssetName.IsEmpty() && Path == PackageName + TEXT(".") + AssetName;
}

const TCHAR* FHyperAIStudioEnhancedInputContracts::SafetyToString(
	const EHyperAIStudioEnhancedInputSafety Safety)
{
	switch (Safety)
	{
	case EHyperAIStudioEnhancedInputSafety::Edit: return TEXT("edit");
	case EHyperAIStudioEnhancedInputSafety::Destructive: return TEXT("destructive");
	case EHyperAIStudioEnhancedInputSafety::ExternalEffect: return TEXT("external_effect");
	default: return TEXT("unknown");
	}
}

bool FHyperAIStudioEnhancedInputContracts::ValidateAuthorizationEnvelope(
	const EHyperAIStudioEnhancedInputSafety Safety,
	const bool bDryRun,
	const FString& AuthorizationToken,
	FString& OutError)
{
	OutError.Reset();
	if (AuthorizationToken.Len() > MaxAuthorizationTokenCharacters)
	{
		OutError = TEXT("Authorization token exceeds the hard envelope bound.");
		return false;
	}
	if (bDryRun || Safety == EHyperAIStudioEnhancedInputSafety::Edit)
	{
		if (!AuthorizationToken.IsEmpty())
		{
			OutError = bDryRun
				? TEXT("Dry-run requests must not carry a bearer authorization token.")
				: TEXT("Edit plans must not carry a destructive/external authorization token.");
			return false;
		}
		return true;
	}
	if (AuthorizationToken.IsEmpty())
	{
		OutError = TEXT("Destructive/external staging requires an opaque trusted grant; no client boolean is authority.");
		return false;
	}
	return true;
}

const TCHAR* FHyperAIStudioEnhancedInputContracts::TypedOperationType(
	const EHyperAIStudioEnhancedInputSafety Safety)
{
	switch (Safety)
	{
	case EHyperAIStudioEnhancedInputSafety::Edit: return EditOperationType;
	case EHyperAIStudioEnhancedInputSafety::Destructive: return DestructiveOperationType;
	case EHyperAIStudioEnhancedInputSafety::ExternalEffect: return ExternalOperationType;
	default: return TEXT("");
	}
}

bool FHyperAIStudioEnhancedInputContracts::ValidateComponentSpec(
	const FHyperAIInputComponentSpec& Spec,
	const bool bExpectTrigger,
	FHyperAIStudioEnhancedInputBackendComponent& OutComponent,
	FString& OutError)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	OutComponent = {};
	OutError.Reset();
	if (Spec.Kind.IsEmpty() || Spec.Kind.Len() > 64 || !IsFinite(Spec.ActuationThreshold)
		|| !IsFinite(Spec.DurationSeconds) || !IsFinite(Spec.SecondarySeconds)
		|| !IsFinite(Spec.LowerThreshold) || !IsFinite(Spec.UpperThreshold)
		|| !IsFinite(Spec.Speed) || !IsFinite(Spec.Exponent) || !IsFinite(Spec.Scale)
		|| !IsFiniteVector(Spec.Vector) || Spec.Mode.Len() > 64)
	{
		OutError = TEXT("Component fields are empty, non-finite, or exceed their string bounds.");
		return false;
	}
	auto IsUnusedTriggerFields = [&Spec]()
	{
		return Spec.LowerThreshold == -1.0 && Spec.UpperThreshold == -1.0
			&& !Spec.bHasVector && Spec.Vector.IsNearlyZero()
			&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ
			&& Spec.Speed == -1.0 && Spec.Exponent == -1.0 && Spec.Scale == -1.0;
	};
	auto IsUnusedModifierTiming = [&Spec]()
	{
		return Spec.ActuationThreshold == -1.0 && Spec.DurationSeconds == -1.0
			&& Spec.SecondarySeconds == -1.0 && Spec.Count == -1 && !Spec.bHasOption
			&& !Spec.bOption;
	};
	auto IsBareModifier = [&Spec, &IsUnusedModifierTiming]()
	{
		return IsUnusedModifierTiming() && Spec.LowerThreshold == -1.0
			&& Spec.UpperThreshold == -1.0 && !Spec.bHasVector && Spec.Vector.IsNearlyZero()
			&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ && Spec.Mode.IsEmpty()
			&& Spec.Speed == -1.0 && Spec.Exponent == -1.0 && Spec.Scale == -1.0;
	};
	auto ValidTimedMode = [&Spec]()
	{
		return Spec.Mode == TEXT("real_time") || Spec.Mode == TEXT("dilated_time");
	};
	auto Set = [&OutComponent, &Spec](const EHyperAIStudioEnhancedInputComponentKind Kind)
	{
		OutComponent.Kind = Kind;
		OutComponent.Config = Spec;
	};

	const bool bIsTrigger = Spec.Kind.StartsWith(TEXT("trigger."), ESearchCase::CaseSensitive);
	const bool bIsModifier = Spec.Kind.StartsWith(TEXT("modifier."), ESearchCase::CaseSensitive);
	if ((bExpectTrigger && !bIsTrigger) || (!bExpectTrigger && !bIsModifier))
	{
		OutError = TEXT("Trigger and modifier arrays are separate; component kind is in the wrong array.");
		return false;
	}
	if (bIsTrigger)
	{
		if (!IsUnusedTriggerFields() || Spec.ActuationThreshold < 0.0
			|| Spec.ActuationThreshold > 1.0)
		{
			OutError = TEXT("Trigger threshold or modifier-only fields violate the closed trigger schema.");
			return false;
		}
		const bool bNoTiming = Spec.DurationSeconds == -1.0 && Spec.SecondarySeconds == -1.0
			&& Spec.Count == -1 && !Spec.bHasOption && !Spec.bOption && Spec.Mode.IsEmpty();
		if (Spec.Kind == TEXT("trigger.down") && bNoTiming)
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerDown);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.pressed") && bNoTiming)
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerPressed);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.released") && bNoTiming)
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerReleased);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.hold") && Spec.DurationSeconds >= 0.0
			&& Spec.DurationSeconds <= 120.0 && Spec.SecondarySeconds == -1.0
			&& Spec.Count == -1 && Spec.bHasOption && ValidTimedMode())
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerHold);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.hold_and_release") && Spec.DurationSeconds >= 0.0
			&& Spec.DurationSeconds <= 120.0 && Spec.SecondarySeconds == -1.0
			&& Spec.Count == -1 && !Spec.bHasOption && !Spec.bOption && ValidTimedMode())
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerHoldAndRelease);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.tap") && Spec.DurationSeconds >= 0.0
			&& Spec.DurationSeconds <= 10.0 && Spec.SecondarySeconds == -1.0
			&& Spec.Count == -1 && !Spec.bHasOption && !Spec.bOption && ValidTimedMode())
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerTap);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.repeated_tap") && Spec.DurationSeconds >= 0.0
			&& Spec.DurationSeconds <= 10.0 && Spec.SecondarySeconds >= 0.0
			&& Spec.SecondarySeconds <= 10.0 && Spec.Count >= 2 && Spec.Count <= 16
			&& !Spec.bHasOption && !Spec.bOption && ValidTimedMode())
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerRepeatedTap);
			return true;
		}
		if (Spec.Kind == TEXT("trigger.pulse") && Spec.DurationSeconds >= 0.001
			&& Spec.DurationSeconds <= 60.0 && Spec.SecondarySeconds == -1.0
			&& Spec.Count >= 0 && Spec.Count <= 10000 && Spec.bHasOption && ValidTimedMode())
		{
			Set(EHyperAIStudioEnhancedInputComponentKind::TriggerPulse);
			return true;
		}
		OutError = TEXT("Trigger kind or its exact per-kind fields are not in the allowlist.");
		return false;
	}

	if (!IsUnusedModifierTiming())
	{
		OutError = TEXT("Modifier contains trigger-only timing/option fields.");
		return false;
	}
	if (Spec.Kind == TEXT("modifier.dead_zone") && Spec.LowerThreshold >= 0.0
		&& Spec.UpperThreshold <= 1.0 && Spec.LowerThreshold < Spec.UpperThreshold
		&& (Spec.Mode == TEXT("axial") || Spec.Mode == TEXT("radial")
			|| Spec.Mode == TEXT("unscaled_radial"))
		&& !Spec.bHasVector && Spec.Vector.IsNearlyZero() && !Spec.bHasAxes
		&& !Spec.bX && !Spec.bY && !Spec.bZ && Spec.Speed == -1.0
		&& Spec.Exponent == -1.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierDeadZone);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.scalar") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && Spec.bHasVector && IsFiniteVector(Spec.Vector)
		&& FMath::Abs(Spec.Vector.X) <= 1000.0 && FMath::Abs(Spec.Vector.Y) <= 1000.0
		&& FMath::Abs(Spec.Vector.Z) <= 1000.0 && !Spec.bHasAxes && !Spec.bX && !Spec.bY
		&& !Spec.bZ && Spec.Mode.IsEmpty() && Spec.Speed == -1.0
		&& Spec.Exponent == -1.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierScalar);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.negate") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && !Spec.bHasVector && Spec.Vector.IsNearlyZero()
		&& Spec.bHasAxes && Spec.Mode.IsEmpty() && Spec.Speed == -1.0
		&& Spec.Exponent == -1.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierNegate);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.swizzle") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && !Spec.bHasVector && Spec.Vector.IsNearlyZero()
		&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ
		&& (Spec.Mode == TEXT("yxz") || Spec.Mode == TEXT("zyx") || Spec.Mode == TEXT("xzy")
			|| Spec.Mode == TEXT("yzx") || Spec.Mode == TEXT("zxy"))
		&& Spec.Speed == -1.0 && Spec.Exponent == -1.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierSwizzle);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.smooth") && IsBareModifier())
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierSmooth);
		return true;
	}
	static const TSet<FString> SmoothModes = {
		TEXT("lerp"), TEXT("interp_to"), TEXT("interp_constant_to"), TEXT("circular_in"),
		TEXT("circular_out"), TEXT("circular_in_out"), TEXT("ease_in"), TEXT("ease_out"),
		TEXT("ease_in_out"), TEXT("expo_in"), TEXT("expo_out"), TEXT("expo_in_out"),
		TEXT("sin_in"), TEXT("sin_out"), TEXT("sin_in_out")};
	if (Spec.Kind == TEXT("modifier.smooth_delta") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && !Spec.bHasVector && Spec.Vector.IsNearlyZero()
		&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ && SmoothModes.Contains(Spec.Mode)
		&& Spec.Speed >= 0.0 && Spec.Speed <= 1000.0
		&& Spec.Exponent >= 0.0 && Spec.Exponent <= 100.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierSmoothDelta);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.response_exponential") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && Spec.bHasVector && IsFiniteVector(Spec.Vector)
		&& Spec.Vector.X > 0.0 && Spec.Vector.Y > 0.0 && Spec.Vector.Z > 0.0
		&& Spec.Vector.X <= 100.0 && Spec.Vector.Y <= 100.0 && Spec.Vector.Z <= 100.0
		&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ && Spec.Mode.IsEmpty()
		&& Spec.Speed == -1.0 && Spec.Exponent == -1.0 && Spec.Scale == -1.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierResponseExponential);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.scale_by_delta_time") && IsBareModifier())
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierScaleByDeltaTime);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.fov_scaling") && Spec.LowerThreshold == -1.0
		&& Spec.UpperThreshold == -1.0 && !Spec.bHasVector && Spec.Vector.IsNearlyZero()
		&& !Spec.bHasAxes && !Spec.bX && !Spec.bY && !Spec.bZ
		&& (Spec.Mode == TEXT("standard") || Spec.Mode == TEXT("ue4_backcompat"))
		&& Spec.Speed == -1.0 && Spec.Exponent == -1.0
		&& Spec.Scale >= -100.0 && Spec.Scale <= 100.0)
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierFovScaling);
		return true;
	}
	if (Spec.Kind == TEXT("modifier.to_world_space") && IsBareModifier())
	{
		Set(EHyperAIStudioEnhancedInputComponentKind::ModifierToWorldSpace);
		return true;
	}
	OutError = TEXT("Modifier kind or its exact per-kind fields are not in the allowlist.");
	return false;
}

bool FHyperAIStudioEnhancedInputContracts::ValidateOperationShape(
	const FHyperAIInputPlanOperation& Operation,
	FHyperAIStudioEnhancedInputBackendOperation& OutOperation,
	FString& OutError)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	OutOperation = {};
	OutError.Reset();
	if (Operation.Type.IsEmpty() || Operation.Type.Len() > 64
		|| !IsCanonicalProjectObjectPath(Operation.TargetPath)
		|| Operation.ProfileId.Len() > MaxProfileCharacters
		|| Operation.RuntimeOwnerPath.Len() > MaxPathCharacters
		|| Operation.Triggers.Num() > MaxComponentsPerOwner
		|| Operation.Modifiers.Num() > MaxComponentsPerOwner)
	{
		OutError = TEXT("Operation type/path/profile/component bounds are invalid.");
		return false;
	}
	if (!Operation.ProfileId.IsEmpty())
	{
		OutError = TEXT("UE 5.8 exposes profile overrides read-only; profile editing is not dispatched.");
		return false;
	}
	for (const FHyperAIInputComponentSpec& Spec : Operation.Triggers)
	{
		FHyperAIStudioEnhancedInputBackendComponent Component;
		if (!ValidateComponentSpec(Spec, true, Component, OutError))
		{
			return false;
		}
		OutOperation.Triggers.Add(MoveTemp(Component));
	}
	for (const FHyperAIInputComponentSpec& Spec : Operation.Modifiers)
	{
		FHyperAIStudioEnhancedInputBackendComponent Component;
		if (!ValidateComponentSpec(Spec, false, Component, OutError))
		{
			return false;
		}
		OutOperation.Modifiers.Add(MoveTemp(Component));
	}
	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.ActionPath = Operation.ActionPath;
	OutOperation.MappingIndex = Operation.MappingIndex;
	OutOperation.ExpectedMappingFingerprint = Operation.ExpectedMappingFingerprint;
	OutOperation.RuntimeOwnerPath = Operation.RuntimeOwnerPath;
	OutOperation.Priority = Operation.Priority;
	OutOperation.bSetTriggerWhenPaused = Operation.bSetTriggerWhenPaused;
	OutOperation.bTriggerWhenPaused = Operation.bTriggerWhenPaused;
	OutOperation.bSetConsumeInput = Operation.bSetConsumeInput;
	OutOperation.bConsumeInput = Operation.bConsumeInput;
	OutOperation.bSetReserveAllMappings = Operation.bSetReserveAllMappings;
	OutOperation.bReserveAllMappings = Operation.bReserveAllMappings;
	OutOperation.bSetAccumulationBehavior = Operation.bSetAccumulationBehavior;
	OutOperation.bCumulative = Operation.AccumulationBehavior == TEXT("cumulative");
	if (!Operation.Key.IsEmpty())
	{
		OutOperation.Key = FName(*Operation.Key);
	}
	auto ParseValueType = [&Operation, &OutOperation]()
	{
		if (Operation.ValueType == TEXT("boolean"))
		{
			OutOperation.ValueType = EHyperAIStudioEnhancedInputValueType::Boolean;
		}
		else if (Operation.ValueType == TEXT("axis1d"))
		{
			OutOperation.ValueType = EHyperAIStudioEnhancedInputValueType::Axis1D;
		}
		else if (Operation.ValueType == TEXT("axis2d"))
		{
			OutOperation.ValueType = EHyperAIStudioEnhancedInputValueType::Axis2D;
		}
		else if (Operation.ValueType == TEXT("axis3d"))
		{
			OutOperation.ValueType = EHyperAIStudioEnhancedInputValueType::Axis3D;
		}
		else
		{
			return false;
		}
		OutOperation.bHasValueType = true;
		return true;
	};
	auto NoMapping = [&Operation]() { return IsDefaultMappingFields(Operation); };
	auto NoRuntime = [&Operation]() { return IsDefaultRuntimeFields(Operation); };
	auto NoComponents = [&Operation]() { return IsDefaultComponents(Operation); };
	auto NoConfig = [&Operation]() { return IsDefaultConfigFields(Operation); };
	auto HasExpectedRevision = [&Operation]() { return IsCanonicalSha256(Operation.ExpectedRevision); };
	auto MappingSelectorValid = [&Operation]()
	{
		return Operation.MappingIndex >= 0 && Operation.MappingIndex < MaxMappings
			&& IsCanonicalSha256(Operation.ExpectedMappingFingerprint);
	};
	auto ActionConfigValid = [&Operation, &ParseValueType]()
	{
		const bool bHasChange = !Operation.ValueType.IsEmpty() || Operation.bSetTriggerWhenPaused
			|| Operation.bSetConsumeInput || Operation.bSetReserveAllMappings
			|| Operation.bSetAccumulationBehavior;
		if (!bHasChange
			|| (Operation.bSetAccumulationBehavior
				&& Operation.AccumulationBehavior != TEXT("highest_absolute")
				&& Operation.AccumulationBehavior != TEXT("cumulative"))
			|| (!Operation.bSetAccumulationBehavior && !Operation.AccumulationBehavior.IsEmpty())
			|| (!Operation.bSetTriggerWhenPaused && Operation.bTriggerWhenPaused)
			|| (!Operation.bSetConsumeInput && !Operation.bConsumeInput)
			|| (!Operation.bSetReserveAllMappings && Operation.bReserveAllMappings))
		{
			return false;
		}
		return Operation.ValueType.IsEmpty() || ParseValueType();
	};

	if (Operation.Type == TEXT("create_action") && Operation.ExpectedRevision.IsEmpty()
		&& [&Operation]()
		{
			FName PackageName;
			return TryGetPrimaryAssetPackageName(Operation.TargetPath, PackageName);
		}()
		&& NoMapping() && NoRuntime() && !Operation.ValueType.IsEmpty() && ParseValueType()
		&& !Operation.bSetTriggerWhenPaused && !Operation.bTriggerWhenPaused
		&& !Operation.bSetConsumeInput && Operation.bConsumeInput
		&& !Operation.bSetReserveAllMappings && !Operation.bReserveAllMappings
		&& !Operation.bSetAccumulationBehavior && Operation.AccumulationBehavior.IsEmpty())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::CreateAction;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("create_context") && Operation.ExpectedRevision.IsEmpty()
		&& [&Operation]()
		{
			FName PackageName;
			return TryGetPrimaryAssetPackageName(Operation.TargetPath, PackageName);
		}()
		&& NoMapping() && NoRuntime() && NoConfig() && NoComponents())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::CreateContext;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("set_action_config") && HasExpectedRevision()
		&& NoMapping() && NoRuntime() && NoComponents() && ActionConfigValid())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::SetActionConfig;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("add_action_trigger") && HasExpectedRevision()
		&& NoMapping() && NoRuntime() && NoConfig()
		&& Operation.Triggers.Num() == 1 && Operation.Modifiers.IsEmpty())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::AddActionTrigger;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("add_action_modifier") && HasExpectedRevision()
		&& NoMapping() && NoRuntime() && NoConfig()
		&& Operation.Triggers.IsEmpty() && Operation.Modifiers.Num() == 1)
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::AddActionModifier;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("add_mapping") && Operation.MappingIndex == -1
		&& Operation.ExpectedMappingFingerprint.IsEmpty() && !Operation.ActionPath.IsEmpty()
		&& Operation.ActionPath.Len() <= MaxPathCharacters && !Operation.Key.IsEmpty()
		&& Operation.Key.Len() <= 128 && NoRuntime() && NoConfig())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::AddMapping;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("append_mapping_trigger") && HasExpectedRevision()
		&& MappingSelectorValid() && Operation.ActionPath.IsEmpty() && Operation.Key.IsEmpty()
		&& NoRuntime() && NoConfig() && Operation.Triggers.Num() == 1
		&& Operation.Modifiers.IsEmpty())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::AppendMappingTrigger;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("append_mapping_modifier") && HasExpectedRevision()
		&& MappingSelectorValid() && Operation.ActionPath.IsEmpty() && Operation.Key.IsEmpty()
		&& NoRuntime() && NoConfig() && Operation.Triggers.IsEmpty()
		&& Operation.Modifiers.Num() == 1)
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::AppendMappingModifier;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Edit;
		return true;
	}
	if (Operation.Type == TEXT("replace_mapping_key") && HasExpectedRevision()
		&& MappingSelectorValid() && Operation.ActionPath.IsEmpty() && !Operation.Key.IsEmpty()
		&& Operation.Key.Len() <= 128 && NoRuntime() && NoConfig() && NoComponents())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Destructive;
		return true;
	}
	if (Operation.Type == TEXT("remove_mapping") && HasExpectedRevision()
		&& MappingSelectorValid() && Operation.ActionPath.IsEmpty() && Operation.Key.IsEmpty()
		&& NoRuntime() && NoConfig() && NoComponents())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::RemoveMapping;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Destructive;
		return true;
	}
	if (Operation.Type == TEXT("delete_asset") && HasExpectedRevision()
		&& NoMapping() && NoRuntime() && NoConfig() && NoComponents())
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::DeleteAsset;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::Destructive;
		return true;
	}
	if (Operation.Type == TEXT("set_runtime_priority") && HasExpectedRevision()
		&& NoMapping() && NoConfig() && NoComponents() && !Operation.RuntimeOwnerPath.IsEmpty()
		&& Operation.Priority >= -10000 && Operation.Priority <= 10000)
	{
		OutOperation.Kind = EHyperAIStudioEnhancedInputOperationKind::SetRuntimePriority;
		OutOperation.Safety = EHyperAIStudioEnhancedInputSafety::ExternalEffect;
		return true;
	}
	OutError = TEXT("Operation fields do not exactly match any closed Enhanced Input variant.");
	return false;
}

FString FHyperAIStudioEnhancedInputContracts::ComputeSnapshotRevision(
	FHyperAIStudioEnhancedInputValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	TArray<FHyperAIInputRecord> Records = Snapshot.Records;
	Records.Sort([](const FHyperAIInputRecord& Left, const FHyperAIInputRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		if (Left.Kind != Right.Kind) return Left.Kind < Right.Kind;
		return Left.AssetPath < Right.AssetPath;
	});
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.enhanced-input-snapshot.v1"));
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.ActionPaths.Num());
	AppendInt(Canonical, Snapshot.ContextPaths.Num());
	AppendInt(Canonical, Snapshot.Mappings.Num());
	for (const FHyperAIInputRecord& Record : Records)
	{
		FString Item;
		AppendToken(Item, Record.Kind);
		AppendToken(Item, Record.StableId);
		AppendToken(Item, Record.AssetPath);
		AppendToken(Item, Record.ParentPath);
		AppendToken(Item, Record.ActionPath);
		AppendToken(Item, Record.Key);
		AppendToken(Item, Record.ValueType);
		AppendToken(Item, Record.Description);
		AppendToken(Item, Record.ProfileId);
		AppendToken(Item, Record.MappingFingerprint);
		AppendToken(Item, Record.MappingName);
		AppendToken(Item, Record.DisplayName);
		AppendToken(Item, Record.DisplayCategory);
		AppendToken(Item, Record.MetadataPath);
		for (const FString& Profile : Record.SupportedProfiles)
		{
			AppendToken(Item, Profile);
		}
		AppendToken(Item, Record.RegistrationTrackingMode);
		AppendToken(Item, Record.InputModeFilter);
		AppendToken(Item, Record.InputModeQueryDescription);
		AppendInt(Item, Record.MappingIndex);
		AppendInt(Item, Record.MappingCount);
		AppendInt(Item, Record.ProfileCount);
		AppendInt(Item, Record.ReferenceCount);
		AppendInt(Item, Record.Priority);
		AppendInt(Item, Record.RegistrationCount);
		AppendBool(Item, Record.bPlayerMappable);
		AppendBool(Item, Record.bPackageDirty);
		AppendBool(Item, Record.bTriggerWhenPaused);
		AppendBool(Item, Record.bConsumeInput);
		AppendBool(Item, Record.bReserveAllMappings);
		AppendBool(Item, Record.bConsumesLegacyMappings);
		AppendInt(Item, Record.LegacyConsumeEvents);
		AppendToken(Item, Record.AccumulationBehavior);
		for (const FHyperAIInputComponentView& Component : Record.Triggers)
		{
			AppendToken(Item, Component.ClassPath);
			AppendBool(Item, Component.bSupported);
			AppendToken(Item, ComponentCanonical(Component.Config));
		}
		for (const FHyperAIInputComponentView& Component : Record.Modifiers)
		{
			AppendToken(Item, Component.ClassPath);
			AppendBool(Item, Component.bSupported);
			AppendToken(Item, ComponentCanonical(Component.Config));
		}
		const FString ItemHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Item);
		if (!IsCanonicalSha256(ItemHash))
		{
			Snapshot.Revision.Reset();
			Snapshot.bComplete = false;
			return Snapshot.Revision;
		}
		AppendToken(Canonical, ItemHash);
	}
	if (Records.IsEmpty())
	{
		TArray<FHyperAIStudioEnhancedInputMappingValue> Mappings = Snapshot.Mappings;
		Mappings.Sort([](const FHyperAIStudioEnhancedInputMappingValue& Left,
			const FHyperAIStudioEnhancedInputMappingValue& Right)
		{
			if (Left.ContextPath != Right.ContextPath) return Left.ContextPath < Right.ContextPath;
			if (Left.ProfileId != Right.ProfileId) return Left.ProfileId < Right.ProfileId;
			if (Left.MappingIndex != Right.MappingIndex) return Left.MappingIndex < Right.MappingIndex;
			if (Left.ActionPath != Right.ActionPath) return Left.ActionPath < Right.ActionPath;
			return Left.Key < Right.Key;
		});
		for (const FHyperAIStudioEnhancedInputMappingValue& Mapping : Mappings)
		{
			FString Item;
			AppendToken(Item, Mapping.ContextPath);
			AppendToken(Item, Mapping.ProfileId);
			AppendInt(Item, Mapping.MappingIndex);
			AppendToken(Item, Mapping.ActionPath);
			AppendToken(Item, Mapping.Key);
			AppendToken(Item, Mapping.ActionValueType);
			AppendToken(Item, Mapping.MappingFingerprint);
			AppendBool(Item, Mapping.bKeyValid);
			AppendBool(Item, Mapping.bRevisionComplete);
			for (const FHyperAIInputComponentSpec& Component : Mapping.Triggers)
			{
				AppendToken(Item, ComponentCanonical(Component));
			}
			for (const FHyperAIInputComponentSpec& Component : Mapping.Modifiers)
			{
				AppendToken(Item, ComponentCanonical(Component));
			}
			const FString ItemHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Item);
			if (!IsCanonicalSha256(ItemHash))
			{
				Snapshot.Revision.Reset();
				Snapshot.bComplete = false;
				return Snapshot.Revision;
			}
			AppendToken(Canonical, ItemHash);
		}
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsCanonicalSha256(Snapshot.Revision))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
	}
	return Snapshot.Revision;
}

TArray<FHyperAIInputIssue> FHyperAIStudioEnhancedInputContracts::ValidateValueSnapshot(
	const FHyperAIStudioEnhancedInputValueSnapshot& Snapshot,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	TArray<FHyperAIInputIssue> Issues;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	for (const FHyperAIInputIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *CaptureIssue.Code, *CaptureIssue.Severity,
			CaptureIssue.AssetPath, CaptureIssue.StableId, CaptureIssue.Message);
	}
	AddIssue(Issues, Maximum, bOutTruncated, TEXT("references_loaded_only"), TEXT("info"),
		FString(), FString(),
		TEXT("Reference/orphan evidence covers loaded Input Actions, contexts, profiles, and player inputs; on-disk referencers are not expanded."));

	TSet<FString> ExactMappings;
	TMap<FString, FString> KeyOwners;
	TMap<FString, int32> ActionReferences;
	for (const FHyperAIStudioEnhancedInputMappingValue& Mapping : Snapshot.Mappings)
	{
		const FString StableId = TEXT("mapping:") + Mapping.ContextPath + TEXT(":")
			+ (Mapping.ProfileId.IsEmpty() ? TEXT("default") : Mapping.ProfileId)
			+ TEXT(":") + FString::FromInt(Mapping.MappingIndex);
		if (!Mapping.bKeyValid)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("invalid_key"), TEXT("error"),
				Mapping.ContextPath, StableId, TEXT("Mapping uses an invalid or unregistered FKey."));
		}
		if (Mapping.ActionPath.IsEmpty())
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("missing_action"), TEXT("error"),
				Mapping.ContextPath, StableId, TEXT("Mapping has no Input Action reference."));
		}
		else
		{
			++ActionReferences.FindOrAdd(Mapping.ActionPath);
			if (!Snapshot.ActionPaths.Contains(Mapping.ActionPath))
			{
				AddIssue(Issues, Maximum, bOutTruncated, TEXT("action_not_in_loaded_snapshot"), TEXT("error"),
					Mapping.ContextPath, StableId,
					TEXT("Referenced Input Action is not part of the exact loaded snapshot."));
			}
		}
		const FString ExactKey = Mapping.ContextPath + TEXT("\n") + Mapping.ProfileId + TEXT("\n")
			+ Mapping.ActionPath + TEXT("\n") + Mapping.Key;
		if (ExactMappings.Contains(ExactKey))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("duplicate_mapping"), TEXT("warning"),
				Mapping.ContextPath, StableId,
				TEXT("The same context/profile/action/key mapping occurs more than once."));
		}
		ExactMappings.Add(ExactKey);
		const FString ConflictKey = Mapping.ContextPath + TEXT("\n") + Mapping.ProfileId + TEXT("\n") + Mapping.Key;
		if (const FString* Owner = KeyOwners.Find(ConflictKey))
		{
			if (*Owner != Mapping.ActionPath)
			{
				AddIssue(Issues, Maximum, bOutTruncated, TEXT("conflicting_key"), TEXT("warning"),
					Mapping.ContextPath, StableId,
					TEXT("One key maps to multiple actions in the same context/profile; consumption and priority decide the winner."));
			}
		}
		else
		{
			KeyOwners.Add(ConflictKey, Mapping.ActionPath);
		}

		const FKey Key(FName(*Mapping.Key));
		if (Mapping.bKeyValid
			&& ((Key.IsAxis3D() && Mapping.ActionValueType != TEXT("axis3d"))
				|| (Key.IsAxis2D() && Mapping.ActionValueType != TEXT("axis2d")
					&& Mapping.ActionValueType != TEXT("axis3d"))))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("value_type_dimension_loss"), TEXT("warning"),
				Mapping.ContextPath, StableId,
				TEXT("Key dimension exceeds the Input Action value type and will discard components."));
		}
		for (const FHyperAIInputComponentSpec& Trigger : Mapping.Triggers)
		{
			FHyperAIStudioEnhancedInputBackendComponent Parsed;
			FString Error;
			if (!ValidateComponentSpec(Trigger, true, Parsed, Error))
			{
				AddIssue(Issues, Maximum, bOutTruncated, TEXT("invalid_trigger"), TEXT("error"),
					Mapping.ContextPath, StableId, Error);
			}
		}
		for (const FHyperAIInputComponentSpec& Modifier : Mapping.Modifiers)
		{
			FHyperAIStudioEnhancedInputBackendComponent Parsed;
			FString Error;
			if (!ValidateComponentSpec(Modifier, false, Parsed, Error))
			{
				AddIssue(Issues, Maximum, bOutTruncated, TEXT("invalid_modifier"), TEXT("error"),
					Mapping.ContextPath, StableId, Error);
			}
			if (Mapping.ActionValueType == TEXT("boolean")
				&& (Modifier.Kind == TEXT("modifier.dead_zone")
					|| Modifier.Kind == TEXT("modifier.scalar")
					|| Modifier.Kind == TEXT("modifier.swizzle")
					|| Modifier.Kind == TEXT("modifier.response_exponential")))
			{
				AddIssue(Issues, Maximum, bOutTruncated, TEXT("modifier_value_type_incompatible"), TEXT("warning"),
					Mapping.ContextPath, StableId,
					TEXT("Axis modifier on a Boolean action is ineffective or loses its intended dimensional behavior."));
			}
		}
		if (!Mapping.bRevisionComplete || !IsCanonicalSha256(Mapping.MappingFingerprint))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("mapping_revision_incomplete"), TEXT("error"),
				Mapping.ContextPath, StableId,
				TEXT("Mapping cannot participate in revision-CAS because its exact state was not captured."));
		}
	}

	for (const FString& ActionPath : Snapshot.ActionPaths)
	{
		if (ActionReferences.FindRef(ActionPath) == 0)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("orphan_loaded_action"), TEXT("warning"),
				ActionPath, TEXT("action:") + ActionPath,
				TEXT("No loaded default/profile mapping references this Input Action; on-disk references were not scanned."));
		}
	}
	for (const FHyperAIInputRecord& Record : Snapshot.Records)
	{
		if (Record.Kind == TEXT("mapping_context") && Record.MappingCount == 0 && Record.ProfileCount == 0)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("empty_mapping_context"), TEXT("warning"),
				Record.AssetPath, Record.StableId, TEXT("Mapping Context has no default or profile mappings."));
		}
		if (Record.Kind == TEXT("action"))
		{
			for (const FHyperAIInputComponentView& Trigger : Record.Triggers)
			{
				if (!Trigger.bSupported)
				{
					AddIssue(Issues, Maximum, bOutTruncated, TEXT("unsupported_action_trigger"), TEXT("warning"),
						Record.AssetPath, Record.StableId,
						TEXT("Custom/deprecated action trigger is inspectable but not mutable through the allowlist."));
				}
			}
			for (const FHyperAIInputComponentView& Modifier : Record.Modifiers)
			{
				if (!Modifier.bSupported)
				{
					AddIssue(Issues, Maximum, bOutTruncated, TEXT("unsupported_action_modifier"), TEXT("warning"),
						Record.AssetPath, Record.StableId,
						TEXT("Custom action modifier is inspectable but not mutable through the allowlist."));
				}
				if (Record.ValueType == TEXT("boolean")
					&& (Modifier.Kind == TEXT("modifier.dead_zone")
						|| Modifier.Kind == TEXT("modifier.scalar")
						|| Modifier.Kind == TEXT("modifier.swizzle")
						|| Modifier.Kind == TEXT("modifier.response_exponential")))
				{
					AddIssue(Issues, Maximum, bOutTruncated, TEXT("action_modifier_value_type_incompatible"), TEXT("warning"),
						Record.AssetPath, Record.StableId,
						TEXT("Axis modifier on a Boolean Input Action has no meaningful axis value to transform."));
				}
			}
		}
	}
	return Issues;
}

FHyperAIInputInspectReport UHyperAIStudioEnhancedInputToolset::hyper_input_inspect(
	const FHyperAIInputInspectRequest& Request)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	FHyperAIInputInspectReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Enhanced Input capture must run on Unreal's serialized game thread.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioEnhancedInputContracts::MaxAssetPaths
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioEnhancedInputContracts::MaxPageSize
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioEnhancedInputContracts::MaxOutputBytes
		|| Request.Cursor.Len() > FHyperAIStudioEnhancedInputContracts::MaxCursorCharacters)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("Asset, page, cursor, or output bounds exceed the strict inspect contract.");
		return Report;
	}
	TSet<FString> UniquePaths;
	for (const FString& Path : Request.AssetPaths)
	{
		if (!FHyperAIStudioEnhancedInputContracts::IsCanonicalProjectObjectPath(Path)
			|| UniquePaths.Contains(Path))
		{
			Report.Status = TEXT("invalid_asset_paths");
			Report.Diagnostic = TEXT("asset_paths must be unique canonical /Game object paths.");
			return Report;
		}
		UniquePaths.Add(Path);
	}
	FCaptureOptions Options;
	Options.bIncludeProfiles = Request.bIncludeProfiles;
	Options.bIncludeRuntimeBindings = Request.bIncludeRuntimeBindings;
	FHyperAIStudioEnhancedInputValueSnapshot Snapshot = CaptureSnapshot(Request.AssetPaths, Options);
	if (!IsCanonicalSha256(Snapshot.Revision))
	{
		Report.Status = TEXT("snapshot_hash_unavailable");
		Report.Diagnostic = TEXT("The bounded loaded-state snapshot could not produce a canonical revision.");
		return Report;
	}

	TArray<FHyperAIInputRecord> Projection;
	for (FHyperAIInputRecord Record : Snapshot.Records)
	{
		if (!Request.bIncludeMappings
			&& (Record.Kind == TEXT("mapping") || Record.Kind == TEXT("profile_mapping")))
		{
			continue;
		}
		if (!Request.bIncludeProfiles && Record.Kind == TEXT("profile_mapping"))
		{
			continue;
		}
		if (!Request.bIncludeRuntimeBindings && Record.Kind == TEXT("runtime_binding"))
		{
			continue;
		}
		if (!Request.bIncludeComponents)
		{
			Record.Triggers.Reset();
			Record.Modifiers.Reset();
		}
		if (!Request.bIncludeReferenceEvidence && Record.Kind == TEXT("action"))
		{
			Record.ReferenceCount = 0;
		}
		Projection.Add(MoveTemp(Record));
	}
	Projection.Sort([](const FHyperAIInputRecord& Left, const FHyperAIInputRecord& Right)
	{
		if (Left.Kind != Right.Kind) return Left.Kind < Right.Kind;
		return Left.StableId < Right.StableId;
	});

	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
	Report.TotalRecords = Projection.Num();
	for (const FHyperAIInputRecord& Record : Projection)
	{
		if (Record.Kind == TEXT("action")) ++Report.ActionCount;
		else if (Record.Kind == TEXT("mapping_context")) ++Report.ContextCount;
		else if (Record.Kind == TEXT("mapping")) ++Report.MappingCount;
		else if (Record.Kind == TEXT("profile_mapping")) ++Report.ProfileMappingCount;
		else if (Record.Kind == TEXT("runtime_binding")) ++Report.RuntimeBindingCount;
	}
	Report.Issues = Snapshot.CaptureIssues;
	if (Report.Issues.Num() > FHyperAIStudioEnhancedInputContracts::MaxIssues)
	{
		Report.Issues.SetNum(FHyperAIStudioEnhancedInputContracts::MaxIssues);
		Report.bTruncated = true;
	}
	const FString RequestHash = MakeRequestFingerprint(Request);
	int32 Offset = 0;
	if (!IsCanonicalSha256(RequestHash)
		|| !ParseCursor(Request.Cursor, RequestHash, Snapshot.Revision, Projection.Num(), Offset))
	{
		Report.Status = TEXT("cursor_mismatch");
		Report.Diagnostic = TEXT("Cursor is malformed or bound to a different request/snapshot revision.");
		return Report;
	}
	int32 OutputBytes = 1024;
	int32 Index = Offset;
	while (Index < Projection.Num() && Report.Records.Num() < Request.PageSize)
	{
		const int32 RecordBytes = EstimateRecordBytes(Projection[Index]);
		if (OutputBytes + RecordBytes > Request.MaxOutputBytes)
		{
			if (Report.Records.IsEmpty())
			{
				Report.Status = TEXT("output_budget_too_small");
				Report.Diagnostic = TEXT("max_output_bytes cannot contain the next bounded record; increase it to make cursor progress.");
				return Report;
			}
			Report.bTruncated = true;
			break;
		}
		OutputBytes += RecordBytes;
		Report.Records.Add(Projection[Index]);
		++Index;
	}
	Report.ReturnedRecords = Report.Records.Num();
	if (Index < Projection.Num())
	{
		Report.bTruncated = true;
		Report.NextCursor = MakeCursor(RequestHash, Snapshot.Revision, Index);
	}
	Report.bTruncated |= !Snapshot.bComplete;
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("partial") : TEXT("ok");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a bounded immutable view of already-loaded actions, contexts, mappings, profiles, and runtime bindings.")
		: TEXT("Returned bounded loaded-state evidence; one or more exact-revision or scan bounds remain incomplete.");
	return Report;
}

FHyperAIInputValidateReport UHyperAIStudioEnhancedInputToolset::hyper_input_validate(
	const FHyperAIInputValidateRequest& Request)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	FHyperAIInputValidateReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Enhanced Input validation must capture UObject state on the game thread.");
		return Report;
	}
	if (Request.AssetPaths.Num() > FHyperAIStudioEnhancedInputContracts::MaxAssetPaths
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioEnhancedInputContracts::MaxIssues)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("Asset or issue bounds exceed the strict validation contract.");
		return Report;
	}
	TSet<FString> Paths;
	for (const FString& Path : Request.AssetPaths)
	{
		if (!FHyperAIStudioEnhancedInputContracts::IsCanonicalProjectObjectPath(Path)
			|| Paths.Contains(Path))
		{
			Report.Status = TEXT("invalid_asset_paths");
			Report.Diagnostic = TEXT("asset_paths must be unique canonical /Game object paths.");
			return Report;
		}
		Paths.Add(Path);
	}
	FCaptureOptions Options;
	Options.bIncludeProfiles = Request.bIncludeProfiles;
	Options.bIncludeRuntimeBindings = Request.bIncludeRuntimeBindings;
	FHyperAIStudioEnhancedInputValueSnapshot Snapshot = CaptureSnapshot(Request.AssetPaths, Options);
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete && IsCanonicalSha256(Snapshot.Revision);
	Report.Issues = FHyperAIStudioEnhancedInputContracts::ValidateValueSnapshot(
		Snapshot, Request.MaxIssues, Report.bTruncated);
	for (const FHyperAIInputIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	Report.bOk = IsCanonicalSha256(Snapshot.Revision);
	Report.bValid = Report.bOk && Snapshot.bComplete && Report.ErrorCount == 0;
	Report.Status = Report.bValid ? TEXT("valid")
		: (Report.bOk ? TEXT("issues_found") : TEXT("snapshot_hash_unavailable"));
	Report.Diagnostic = Report.bValid
		? TEXT("Loaded Enhanced Input assets passed bounded conflict, key, reference, value-type, and component validation.")
		: TEXT("Validation completed but exact-revision gaps or actionable issues prevent a valid result.");
	return Report;
}

namespace HyperAIStudio::EnhancedInput::Private
{
	const TCHAR* OperationKindToken(const EHyperAIStudioEnhancedInputOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioEnhancedInputOperationKind::CreateAction: return TEXT("create_action");
		case EHyperAIStudioEnhancedInputOperationKind::CreateContext: return TEXT("create_context");
		case EHyperAIStudioEnhancedInputOperationKind::SetActionConfig: return TEXT("set_action_config");
		case EHyperAIStudioEnhancedInputOperationKind::AddActionTrigger: return TEXT("add_action_trigger");
		case EHyperAIStudioEnhancedInputOperationKind::AddActionModifier: return TEXT("add_action_modifier");
		case EHyperAIStudioEnhancedInputOperationKind::AddMapping: return TEXT("add_mapping");
		case EHyperAIStudioEnhancedInputOperationKind::AppendMappingTrigger: return TEXT("append_mapping_trigger");
		case EHyperAIStudioEnhancedInputOperationKind::AppendMappingModifier: return TEXT("append_mapping_modifier");
		case EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey: return TEXT("replace_mapping_key");
		case EHyperAIStudioEnhancedInputOperationKind::RemoveMapping: return TEXT("remove_mapping");
		case EHyperAIStudioEnhancedInputOperationKind::DeleteAsset: return TEXT("delete_asset");
		case EHyperAIStudioEnhancedInputOperationKind::SetRuntimePriority: return TEXT("set_runtime_priority");
		default: return TEXT("unknown");
		}
	}

	FString ValueTypeBackendToken(const FHyperAIStudioEnhancedInputBackendOperation& Operation)
	{
		if (!Operation.bHasValueType)
		{
			return FString();
		}
		switch (Operation.ValueType)
		{
		case EHyperAIStudioEnhancedInputValueType::Boolean: return TEXT("boolean");
		case EHyperAIStudioEnhancedInputValueType::Axis1D: return TEXT("axis1d");
		case EHyperAIStudioEnhancedInputValueType::Axis2D: return TEXT("axis2d");
		case EHyperAIStudioEnhancedInputValueType::Axis3D: return TEXT("axis3d");
		default: return TEXT("unknown");
		}
	}

	FString OperationCanonical(const FHyperAIStudioEnhancedInputBackendOperation& Operation)
	{
		FString Canonical;
		AppendToken(Canonical, OperationKindToken(Operation.Kind));
		AppendToken(Canonical, FHyperAIStudioEnhancedInputContracts::SafetyToString(Operation.Safety));
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.ActionPath);
		AppendToken(Canonical, Operation.Key.ToString());
		AppendInt(Canonical, Operation.MappingIndex);
		AppendToken(Canonical, Operation.ExpectedMappingFingerprint);
		AppendBool(Canonical, Operation.bHasValueType);
		AppendToken(Canonical, ValueTypeBackendToken(Operation));
		AppendBool(Canonical, Operation.bSetTriggerWhenPaused);
		AppendBool(Canonical, Operation.bTriggerWhenPaused);
		AppendBool(Canonical, Operation.bSetConsumeInput);
		AppendBool(Canonical, Operation.bConsumeInput);
		AppendBool(Canonical, Operation.bSetReserveAllMappings);
		AppendBool(Canonical, Operation.bReserveAllMappings);
		AppendBool(Canonical, Operation.bSetAccumulationBehavior);
		AppendBool(Canonical, Operation.bCumulative);
		AppendToken(Canonical, Operation.RuntimeOwnerPath);
		AppendInt(Canonical, Operation.Priority);
		for (const FHyperAIStudioEnhancedInputBackendComponent& Component : Operation.Triggers)
		{
			AppendToken(Canonical, ComponentCanonical(Component.Config));
		}
		for (const FHyperAIStudioEnhancedInputBackendComponent& Component : Operation.Modifiers)
		{
			AppendToken(Canonical, ComponentCanonical(Component.Config));
		}
		return Canonical;
	}

	bool CheckCreationAbsence(const FString& Path, FString& OutError)
	{
		OutError.Reset();
		if (FSoftObjectPath(Path).ResolveObject())
		{
			OutError = TEXT("Creation target is already loaded.");
			return false;
		}
		FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(
			TEXT("AssetRegistry"));
		if (!Module)
		{
			OutError = TEXT("AssetRegistry is not already loaded; absence cannot be proven without changing module state.");
			return false;
		}
		FName PackageName;
		if (!TryGetPrimaryAssetPackageName(Path, PackageName))
		{
			OutError = TEXT("Creation target must use the primary object name matching its package short name.");
			return false;
		}
		IAssetRegistry& Registry = Module->Get();
		if (Registry.IsGathering() || Registry.IsLoadingAssets())
		{
			OutError = TEXT("AssetRegistry discovery is incomplete; creation-target absence is unknown.");
			return false;
		}
		FAssetPackageData PackageData;
		const UE::AssetRegistry::EExists Exists = Registry.TryGetAssetPackageData(
			PackageName, PackageData, true /* bFailIfLockHeld */);
		if (Exists == UE::AssetRegistry::EExists::Exists)
		{
			OutError = TEXT("Creation target package already exists on disk.");
			return false;
		}
		if (Exists == UE::AssetRegistry::EExists::Unknown)
		{
			OutError = TEXT("AssetRegistry has not proven the creation target package absent without waiting; retry after discovery completes.");
			return false;
		}
		return true;
	}

	UEnhancedPlayerInput* FindLoadedPlayerInputBounded(const FString& ExactPath)
	{
		int32 Scanned = 0;
		for (TObjectIterator<UEnhancedPlayerInput> It;
			It && Scanned < FHyperAIStudioEnhancedInputContracts::MaxRuntimeOwnersScanned; ++It)
		{
			++Scanned;
			UEnhancedPlayerInput* PlayerInput = *It;
			if (PlayerInput && !PlayerInput->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
				&& PlayerInput->GetPathName() == ExactPath)
			{
				return PlayerInput;
			}
		}
		return nullptr;
	}

	const FHyperAIInputRecord* FindAssetRecord(
		const FHyperAIStudioEnhancedInputValueSnapshot& Snapshot,
		const FString& Path,
		const FString& Kind)
	{
		return Snapshot.Records.FindByPredicate([&Path, &Kind](const FHyperAIInputRecord& Record)
		{
			return Record.AssetPath == Path && Record.Kind == Kind;
		});
	}

	bool IsContextOperation(const EHyperAIStudioEnhancedInputOperationKind Kind)
	{
		return Kind == EHyperAIStudioEnhancedInputOperationKind::CreateContext
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::AddMapping
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::AppendMappingTrigger
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::AppendMappingModifier
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::RemoveMapping
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::SetRuntimePriority;
	}

	bool IsActionOperation(const EHyperAIStudioEnhancedInputOperationKind Kind)
	{
		return Kind == EHyperAIStudioEnhancedInputOperationKind::CreateAction
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::SetActionConfig
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::AddActionTrigger
			|| Kind == EHyperAIStudioEnhancedInputOperationKind::AddActionModifier;
	}

	void CountEffect(
		const FHyperAIStudioEnhancedInputBackendOperation& Operation,
		FHyperAIInputPlanEffects& Effects)
	{
		++Effects.OperationCount;
		switch (Operation.Kind)
		{
		case EHyperAIStudioEnhancedInputOperationKind::CreateAction:
			++Effects.ActionsCreated;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::CreateContext:
			++Effects.ContextsCreated;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::SetActionConfig:
		case EHyperAIStudioEnhancedInputOperationKind::AddActionTrigger:
		case EHyperAIStudioEnhancedInputOperationKind::AddActionModifier:
			++Effects.ActionsUpdated;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::AddMapping:
			++Effects.MappingsAdded;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::AppendMappingTrigger:
		case EHyperAIStudioEnhancedInputOperationKind::AppendMappingModifier:
		case EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey:
			++Effects.MappingsUpdated;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::RemoveMapping:
			++Effects.MappingsRemoved;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::DeleteAsset:
			++Effects.AssetsDeleted;
			break;
		case EHyperAIStudioEnhancedInputOperationKind::SetRuntimePriority:
			++Effects.RuntimePrioritiesChanged;
			break;
		default:
			break;
		}
	}

	enum class EFastEditAttempt : uint8
	{
		Applied,
		Unsupported,
		Failed
	};

	bool CanExecuteFastReversibleEdit(
		const TArray<FHyperAIStudioEnhancedInputBackendOperation>& Operations)
	{
		if (Operations.IsEmpty())
		{
			return false;
		}
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
		{
			if (Operation.Safety != EHyperAIStudioEnhancedInputSafety::Edit
				|| (Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::SetActionConfig
					&& (Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::AddMapping
						|| !Operation.Triggers.IsEmpty() || !Operation.Modifiers.IsEmpty())))
			{
				return false;
			}
		}
		return true;
	}

	EInputActionValueType ToEngineValueType(
		const EHyperAIStudioEnhancedInputValueType ValueType)
	{
		switch (ValueType)
		{
		case EHyperAIStudioEnhancedInputValueType::Axis1D:
			return EInputActionValueType::Axis1D;
		case EHyperAIStudioEnhancedInputValueType::Axis2D:
			return EInputActionValueType::Axis2D;
		case EHyperAIStudioEnhancedInputValueType::Axis3D:
			return EInputActionValueType::Axis3D;
		case EHyperAIStudioEnhancedInputValueType::Boolean:
		default:
			return EInputActionValueType::Boolean;
		}
	}

	EFastEditAttempt ExecuteFastReversibleEdit(
		const TArray<FHyperAIStudioEnhancedInputBackendOperation>& Operations,
		FString& OutDiagnostic)
	{
		OutDiagnostic.Reset();
		TMap<FString, UInputAction*> Actions;
		TMap<FString, UInputMappingContext*> Contexts;
		TMap<FString, int32> InitialMappingCounts;
		TMap<FString, int32> AddedMappingCounts;

		// Resolve and validate every pointer before opening the transaction. Fast mode never
		// partially applies a plan merely because one later operation needs the strict backend.
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
		{
			if (Operation.Safety != EHyperAIStudioEnhancedInputSafety::Edit)
			{
				return EFastEditAttempt::Unsupported;
			}
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::SetActionConfig)
			{
				UInputAction* Action = Cast<UInputAction>(
					FSoftObjectPath(Operation.TargetPath).ResolveObject());
				if (!Action || Action->GetPathName() != Operation.TargetPath)
				{
					OutDiagnostic = TEXT("Fast edit lost its exact loaded Input Action before execution.");
					return EFastEditAttempt::Failed;
				}
				Actions.Add(Operation.TargetPath, Action);
				continue;
			}
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::AddMapping
				&& Operation.Triggers.IsEmpty() && Operation.Modifiers.IsEmpty())
			{
				UInputMappingContext* Context = Cast<UInputMappingContext>(
					FSoftObjectPath(Operation.TargetPath).ResolveObject());
				UInputAction* Action = Cast<UInputAction>(
					FSoftObjectPath(Operation.ActionPath).ResolveObject());
				if (!Context || Context->GetPathName() != Operation.TargetPath
					|| !Action || Action->GetPathName() != Operation.ActionPath
					|| !FKey(Operation.Key).IsValid())
				{
					OutDiagnostic = TEXT("Fast edit lost an exact loaded mapping context, action, or key before execution.");
					return EFastEditAttempt::Failed;
				}
				Contexts.Add(Operation.TargetPath, Context);
				Actions.Add(Operation.ActionPath, Action);
				InitialMappingCounts.FindOrAdd(Operation.TargetPath, Context->GetMappings().Num());
				++AddedMappingCounts.FindOrAdd(Operation.TargetPath);
				continue;
			}

			// Asset creation, component instancing, deletes and runtime effects remain on their
			// specialized paths. The common config/mapping edits above cover the fast daily case.
			return EFastEditAttempt::Unsupported;
		}

		FScopedTransaction Transaction(FText::FromString(
			TEXT("HyperAIStudio Enhanced Input Fast Edit")));
		TSet<UObject*> ModifiedObjects;
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
		{
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::SetActionConfig)
			{
				UInputAction* Action = Actions.FindRef(Operation.TargetPath);
				if (!ModifiedObjects.Contains(Action))
				{
					Action->Modify();
					ModifiedObjects.Add(Action);
				}
				if (Operation.bHasValueType)
				{
					Action->ValueType = ToEngineValueType(Operation.ValueType);
				}
				if (Operation.bSetTriggerWhenPaused)
				{
					Action->bTriggerWhenPaused = Operation.bTriggerWhenPaused;
				}
				if (Operation.bSetConsumeInput)
				{
					Action->bConsumeInput = Operation.bConsumeInput;
				}
				if (Operation.bSetReserveAllMappings)
				{
					Action->bReserveAllMappings = Operation.bReserveAllMappings;
				}
				if (Operation.bSetAccumulationBehavior)
				{
					Action->AccumulationBehavior = Operation.bCumulative
						? EInputActionAccumulationBehavior::Cumulative
						: EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
				}
			}
			else
			{
				UInputMappingContext* Context = Contexts.FindRef(Operation.TargetPath);
				if (!ModifiedObjects.Contains(Context))
				{
					Context->Modify();
					ModifiedObjects.Add(Context);
				}
				Context->MapKey(Actions.FindRef(Operation.ActionPath), FKey(Operation.Key));
			}
		}

		for (UObject* Object : ModifiedObjects)
		{
			Object->PostEditChange();
			Object->MarkPackageDirty();
		}

		for (const TPair<FString, int32>& Added : AddedMappingCounts)
		{
			const UInputMappingContext* Context = Contexts.FindRef(Added.Key);
			if (!Context || Context->GetMappings().Num()
				!= InitialMappingCounts.FindRef(Added.Key) + Added.Value)
			{
				OutDiagnostic = TEXT("Enhanced Input mapping count did not reach the exact postcondition.");
				return EFastEditAttempt::Failed;
			}
		}

		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
		{
			if (Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::SetActionConfig)
			{
				continue;
			}
			const UInputAction* Action = Actions.FindRef(Operation.TargetPath);
			if ((Operation.bHasValueType
					&& Action->ValueType != ToEngineValueType(Operation.ValueType))
				|| (Operation.bSetTriggerWhenPaused
					&& Action->bTriggerWhenPaused != Operation.bTriggerWhenPaused)
				|| (Operation.bSetConsumeInput
					&& Action->bConsumeInput != Operation.bConsumeInput)
				|| (Operation.bSetReserveAllMappings
					&& Action->bReserveAllMappings != Operation.bReserveAllMappings)
				|| (Operation.bSetAccumulationBehavior
					&& (Action->AccumulationBehavior == EInputActionAccumulationBehavior::Cumulative)
						!= Operation.bCumulative))
			{
				OutDiagnostic = TEXT("Enhanced Input action config did not reach the exact postcondition.");
				return EFastEditAttempt::Failed;
			}
		}

		return EFastEditAttempt::Applied;
	}
}

FHyperAIInputApplyPlanReport FHyperAIStudioEnhancedInputContracts::BuildPlan(
	const FHyperAIInputApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	FHyperAIInputApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Enhanced Input plan capture and CAS validation require Unreal's game thread.");
		return Report;
	}
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.OperationId.Len() > 128
		|| Request.AuthorizationToken.Len() > MaxAuthorizationTokenCharacters)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("Plan needs 1..128 operations and bounded operation/auth fields.");
		return Report;
	}

	TArray<FHyperAIStudioEnhancedInputBackendOperation> Operations;
	TOptional<EHyperAIStudioEnhancedInputSafety> CohortSafety;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		FHyperAIStudioEnhancedInputBackendOperation Operation;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Operation, Error))
		{
			bool bIgnored = false;
			AddIssue(Report.Issues, MaxIssues, bIgnored, TEXT("invalid_operation_shape"), TEXT("error"),
				Request.Operations[Index].TargetPath, FString::FromInt(Index), Error);
			Report.Status = TEXT("invalid_plan");
			Report.Diagnostic = TEXT("One operation does not match its closed discriminated schema.");
			return Report;
		}
		if (!CohortSafety.IsSet())
		{
			CohortSafety = Operation.Safety;
		}
		else if (CohortSafety.GetValue() != Operation.Safety)
		{
			bool bIgnored = false;
			AddIssue(Report.Issues, MaxIssues, bIgnored, TEXT("mixed_safety_cohort"), TEXT("error"),
				Operation.TargetPath, FString::FromInt(Index),
				TEXT("Edit, destructive, and runtime-external operations must be submitted as separate exact plans."));
			Report.Status = TEXT("mixed_safety_cohort");
			Report.Diagnostic = TEXT("A single opaque grant and journal record cannot authorize mixed safety classes.");
			return Report;
		}
		CountEffect(Operation, Report.Effects);
		Operations.Add(MoveTemp(Operation));
	}
	check(CohortSafety.IsSet());
	const EHyperAIStudioEnhancedInputSafety Safety = CohortSafety.GetValue();
	Report.SafetyClass = SafetyToString(Safety);
	Report.TypedOperationType = TypedOperationType(Safety);
	Report.bRequiresTrustedAuthorization = Safety != EHyperAIStudioEnhancedInputSafety::Edit;
	FString AuthorizationError;
	if (!ValidateAuthorizationEnvelope(Safety, Request.bDryRun, Request.AuthorizationToken,
		AuthorizationError))
	{
		Report.Status = TEXT("invalid_authorization_envelope");
		Report.Diagnostic = AuthorizationError;
		return Report;
	}
	Report.Effects.bTransactionOnce = Safety != EHyperAIStudioEnhancedInputSafety::ExternalEffect;
	Report.Effects.bSaveOnce = Safety != EHyperAIStudioEnhancedInputSafety::ExternalEffect;
	Report.Effects.bValidateOnce = true;
	Report.Effects.bFreshVerifyOnce = true;
	if (Safety == EHyperAIStudioEnhancedInputSafety::Edit && !Request.AuthorizationToken.IsEmpty())
	{
		Report.Status = TEXT("authorization_not_applicable");
		Report.Diagnostic = TEXT("Edit plans reject bearer tokens; destructive authority cannot be smuggled into an edit route.");
		return Report;
	}
	if (Request.bDryRun && !Request.AuthorizationToken.IsEmpty())
	{
		Report.Status = TEXT("authorization_not_accepted_in_dry_run");
		Report.Diagnostic = TEXT("Dry-run hashing never accepts or retains an opaque bearer token.");
		return Report;
	}
	const bool bFastReversibleEdit = Safety == EHyperAIStudioEnhancedInputSafety::Edit
		&& !FHyperAIStudioExtensionRuntime::UsesStrictNativeSafety(
			EHyperAIStudioDomainSafety::Edit)
		&& CanExecuteFastReversibleEdit(Operations);
	if (bFastReversibleEdit)
	{
		Report.bRequiresJournalExecution = false;
		Report.Effects.bSaveOnce = false;
	}
	if (!Request.bDryRun && bFastReversibleEdit
		&& FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		if (const FString* CompletedHash = GetFastReplayState().CompletedPlanHashes.Find(
			Request.OperationId))
		{
			Report.PlanHash = Request.ExpectedPlanHash;
			Report.bRequiresJournalExecution = false;
			if (*CompletedHash != Request.ExpectedPlanHash)
			{
				Report.Status = TEXT("operation_id_conflict");
				Report.Diagnostic = TEXT("Fast-mode operation_id was already completed with another plan hash.");
				return Report;
			}
			Report.bOk = true;
			Report.bReplay = true;
			Report.bExecutionSubmitted = true;
			Report.Status = TEXT("already_applied");
			Report.Diagnostic = TEXT("Fast-mode bounded idempotency cache confirms this exact plan already completed.");
			return Report;
		}
	}

	TMap<FString, EHyperAIStudioEnhancedInputOperationKind> Creates;
	TMap<FString, FString> CreatedActionValueTypes;
	TSet<FString> DeleteTargets;
	for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
	{
		if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::CreateAction
			|| Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::CreateContext)
		{
			if (Creates.Contains(Operation.TargetPath))
			{
				bool bIgnored = false;
				AddIssue(Report.Issues, MaxIssues, bIgnored, TEXT("duplicate_create_target"), TEXT("error"),
					Operation.TargetPath, Operation.TargetPath, TEXT("One exact object path can be created only once."));
				continue;
			}
			Creates.Add(Operation.TargetPath, Operation.Kind);
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::CreateAction)
			{
				CreatedActionValueTypes.Add(Operation.TargetPath, ValueTypeBackendToken(Operation));
			}
		}
		if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::DeleteAsset)
		{
			if (DeleteTargets.Contains(Operation.TargetPath))
			{
				bool bIgnored = false;
				AddIssue(Report.Issues, MaxIssues, bIgnored, TEXT("duplicate_delete_target"), TEXT("error"),
					Operation.TargetPath, Operation.TargetPath, TEXT("One exact asset can be deleted only once."));
			}
			DeleteTargets.Add(Operation.TargetPath);
		}
	}
	for (const TPair<FString, EHyperAIStudioEnhancedInputOperationKind>& Create : Creates)
	{
		if (DeleteTargets.Contains(Create.Key))
		{
			bool bIgnored = false;
			AddIssue(Report.Issues, MaxIssues, bIgnored, TEXT("create_delete_same_target"), TEXT("error"),
				Create.Key, Create.Key, TEXT("Creating and deleting one path in the same plan is forbidden."));
		}
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("invalid_plan");
		Report.Diagnostic = TEXT("Create/delete identity conflicts prevent planning.");
		return Report;
	}

	TMap<FString, FHyperAIStudioEnhancedInputValueSnapshot> ExistingSnapshots;
	TArray<FString> BaseTokens;
	auto CaptureExisting = [&ExistingSnapshots, &BaseTokens](
		const FString& Path) -> FHyperAIStudioEnhancedInputValueSnapshot*
	{
		if (FHyperAIStudioEnhancedInputValueSnapshot* Existing = ExistingSnapshots.Find(Path))
		{
			return Existing;
		}
		FCaptureOptions Options;
		Options.bIncludeProfiles = true;
		Options.bIncludeRuntimeBindings = false;
		FHyperAIStudioEnhancedInputValueSnapshot Snapshot = CaptureSnapshot({Path}, Options);
		BaseTokens.Add(TEXT("asset:") + Path + TEXT(":") + Snapshot.Revision);
		return &ExistingSnapshots.Add(Path, MoveTemp(Snapshot));
	};
	auto AddPlanIssue = [&Report](
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Path,
		const FString& Id,
		const FString& Message)
	{
		bool bIgnored = false;
		AddIssue(Report.Issues, FHyperAIStudioEnhancedInputContracts::MaxIssues, bIgnored,
			Code, Severity, Path, Id, Message);
	};

	for (const TPair<FString, EHyperAIStudioEnhancedInputOperationKind>& Create : Creates)
	{
		FString Error;
		if (!CheckCreationAbsence(Create.Key, Error))
		{
			AddPlanIssue(TEXT("creation_absence_unproven"), TEXT("error"), Create.Key, Create.Key, Error);
		}
		else
		{
			BaseTokens.Add(TEXT("absent:") + Create.Key);
		}
	}

	FHyperAIStudioEnhancedInputValueSnapshot SimulatedMappings;
	SimulatedMappings.bComplete = true;
	TSet<FString> SelectedMappings;
	TSet<FString> SimulatedExistingContexts;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const FHyperAIStudioEnhancedInputBackendOperation& Operation = Operations[Index];
		const FString StableId = FString::FromInt(Index) + TEXT(":") + OperationKindToken(Operation.Kind);
		const bool bTargetCreated = Creates.Contains(Operation.TargetPath);
		FHyperAIStudioEnhancedInputValueSnapshot* TargetSnapshot = nullptr;
		if (Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::CreateAction
			&& Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::CreateContext)
		{
			if (bTargetCreated)
			{
				if (Operation.Kind != EHyperAIStudioEnhancedInputOperationKind::AddMapping
					|| !Operation.ExpectedRevision.IsEmpty())
				{
					AddPlanIssue(TEXT("invalid_created_target_followup"), TEXT("error"),
						Operation.TargetPath, StableId,
						TEXT("Only add_mapping may target a context created in this plan, with no base revision."));
					continue;
				}
			}
			else
			{
				TargetSnapshot = CaptureExisting(Operation.TargetPath);
				if (!TargetSnapshot->bComplete || !IsCanonicalSha256(TargetSnapshot->Revision))
				{
					AddPlanIssue(TEXT("target_revision_incomplete"), TEXT("error"),
						Operation.TargetPath, StableId,
						TEXT("Existing target must be already loaded with an exact complete revision."));
					continue;
				}
				if (Operation.ExpectedRevision != TargetSnapshot->Revision)
				{
					AddPlanIssue(TEXT("revision_mismatch"), TEXT("error"),
						Operation.TargetPath, StableId,
						TEXT("expected_revision does not equal the current exact loaded asset revision."));
					continue;
				}
			}
		}

		if (IsActionOperation(Operation.Kind))
		{
			const EHyperAIStudioEnhancedInputOperationKind* CreatedKind = Creates.Find(Operation.TargetPath);
			const bool bCreatedAsAction = CreatedKind
				&& *CreatedKind == EHyperAIStudioEnhancedInputOperationKind::CreateAction;
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::CreateAction)
			{
				continue;
			}
			if (bCreatedAsAction || !TargetSnapshot
				|| !TargetSnapshot->ActionPaths.Contains(Operation.TargetPath))
			{
				AddPlanIssue(TEXT("target_not_input_action"), TEXT("error"),
					Operation.TargetPath, StableId, TEXT("Operation target is not the exact loaded Input Action."));
				continue;
			}
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::SetActionConfig
				&& Operation.bHasValueType)
			{
				AddPlanIssue(TEXT("value_type_change_requires_blueprint_validation"), TEXT("warning"),
					Operation.TargetPath, StableId,
					TEXT("Value-type change can invalidate Blueprint pins; central postconditions must validate referencers before save."));
			}
		}
		else if (IsContextOperation(Operation.Kind))
		{
			const EHyperAIStudioEnhancedInputOperationKind* CreatedKind = Creates.Find(Operation.TargetPath);
			const bool bCreatedAsContext = CreatedKind
				&& *CreatedKind == EHyperAIStudioEnhancedInputOperationKind::CreateContext;
			if (!bCreatedAsContext && (!TargetSnapshot
				|| !TargetSnapshot->ContextPaths.Contains(Operation.TargetPath)))
			{
				AddPlanIssue(TEXT("target_not_mapping_context"), TEXT("error"),
					Operation.TargetPath, StableId,
					TEXT("Operation target is not the exact loaded Input Mapping Context."));
				continue;
			}
		}
		else if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::DeleteAsset)
		{
			if (!TargetSnapshot || (!TargetSnapshot->ActionPaths.Contains(Operation.TargetPath)
				&& !TargetSnapshot->ContextPaths.Contains(Operation.TargetPath)))
			{
				AddPlanIssue(TEXT("delete_target_wrong_type"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("delete_asset accepts only an exact loaded Input Action or Mapping Context."));
				continue;
			}
			if (const FHyperAIInputRecord* ActionRecord = FindAssetRecord(
				*TargetSnapshot, Operation.TargetPath, TEXT("action")))
			{
				if (ActionRecord->ReferenceCount > 0)
				{
					AddPlanIssue(TEXT("delete_has_loaded_references"), TEXT("warning"), Operation.TargetPath,
						StableId, TEXT("Loaded Mapping Contexts reference this action; central deletion validation must prove intent."));
				}
			}
		}

		if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::AddMapping)
		{
			if (!IsCanonicalProjectObjectPath(Operation.ActionPath))
			{
				AddPlanIssue(TEXT("invalid_action_path"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("add_mapping action_path must be a canonical /Game object path."));
				continue;
			}
			FString ActionValueType;
			if (const FString* CreatedType = CreatedActionValueTypes.Find(Operation.ActionPath))
			{
				ActionValueType = *CreatedType;
				BaseTokens.Add(TEXT("created-action:") + Operation.ActionPath + TEXT(":") + ActionValueType);
			}
			else
			{
				FHyperAIStudioEnhancedInputValueSnapshot* ActionSnapshot = CaptureExisting(Operation.ActionPath);
				if (!ActionSnapshot->bComplete || !ActionSnapshot->ActionPaths.Contains(Operation.ActionPath)
					|| !IsCanonicalSha256(ActionSnapshot->Revision))
				{
					AddPlanIssue(TEXT("action_not_exact_loaded"), TEXT("error"), Operation.ActionPath,
						StableId, TEXT("add_mapping action must be already loaded or created in this exact plan."));
					continue;
				}
				ActionValueType = ActionSnapshot->ActionValueTypes.FindRef(Operation.ActionPath);
			}
			const FKey Key(Operation.Key);
			if (!Key.IsValid())
			{
				AddPlanIssue(TEXT("invalid_key"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("add_mapping key is not registered by InputCore."));
				continue;
			}
			FHyperAIStudioEnhancedInputMappingValue Mapping;
			Mapping.ContextPath = Operation.TargetPath;
			Mapping.MappingIndex = TargetSnapshot ? TargetSnapshot->Mappings.Num() : 0;
			Mapping.ActionPath = Operation.ActionPath;
			Mapping.Key = Operation.Key.ToString();
			Mapping.ActionValueType = ActionValueType;
			Mapping.bKeyValid = true;
			Mapping.bRevisionComplete = true;
			for (const FHyperAIStudioEnhancedInputBackendComponent& Component : Operation.Triggers)
			{
				Mapping.Triggers.Add(Component.Config);
			}
			for (const FHyperAIStudioEnhancedInputBackendComponent& Component : Operation.Modifiers)
			{
				Mapping.Modifiers.Add(Component.Config);
			}
			FString MappingCanonicalValue = OperationCanonical(Operation);
			Mapping.MappingFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
				MappingCanonicalValue);
			SimulatedMappings.ActionPaths.Add(Operation.ActionPath);
			SimulatedMappings.ActionValueTypes.Add(Operation.ActionPath, ActionValueType);
			SimulatedMappings.Mappings.Add(MoveTemp(Mapping));
			if (TargetSnapshot && !SimulatedExistingContexts.Contains(Operation.TargetPath))
			{
				SimulatedExistingContexts.Add(Operation.TargetPath);
				for (const FString& ActionPath : TargetSnapshot->ActionPaths)
				{
					SimulatedMappings.ActionPaths.Add(ActionPath);
					SimulatedMappings.ActionValueTypes.Add(
						ActionPath, TargetSnapshot->ActionValueTypes.FindRef(ActionPath));
				}
				SimulatedMappings.Mappings.Append(TargetSnapshot->Mappings);
			}
		}

		if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::AppendMappingTrigger
			|| Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::AppendMappingModifier
			|| Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey
			|| Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::RemoveMapping)
		{
			const FHyperAIStudioEnhancedInputMappingValue* Mapping = TargetSnapshot
				? TargetSnapshot->Mappings.FindByPredicate([&Operation](
					const FHyperAIStudioEnhancedInputMappingValue& Candidate)
				{
					return Candidate.ProfileId.IsEmpty()
						&& Candidate.MappingIndex == Operation.MappingIndex;
				}) : nullptr;
			if (!Mapping || Mapping->MappingFingerprint != Operation.ExpectedMappingFingerprint)
			{
				AddPlanIssue(TEXT("mapping_cas_mismatch"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("Default mapping index/fingerprint does not match the exact current mapping."));
				continue;
			}
			const FString Selection = Operation.TargetPath + TEXT(":")
				+ FString::FromInt(Operation.MappingIndex);
			if (SelectedMappings.Contains(Selection))
			{
				AddPlanIssue(TEXT("mapping_selected_twice"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("One mapping may be changed/removed only once per plan."));
			}
			SelectedMappings.Add(Selection);
			if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::ReplaceMappingKey
				&& !FKey(Operation.Key).IsValid())
			{
				AddPlanIssue(TEXT("invalid_replacement_key"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("Replacement key is not registered by InputCore."));
			}
			BaseTokens.Add(TEXT("mapping:") + Operation.TargetPath + TEXT(":")
				+ Operation.ExpectedMappingFingerprint);
		}

		if (Operation.Kind == EHyperAIStudioEnhancedInputOperationKind::SetRuntimePriority)
		{
			UEnhancedPlayerInput* PlayerInput = FindLoadedPlayerInputBounded(Operation.RuntimeOwnerPath);
			if (!PlayerInput)
			{
				AddPlanIssue(TEXT("runtime_owner_not_loaded"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("runtime_owner_path is not an exact loaded UEnhancedPlayerInput within the scan bound."));
				continue;
			}
			const UInputMappingContext* Context = Cast<UInputMappingContext>(
				FSoftObjectPath(Operation.TargetPath).ResolveObject());
			int32 AppliedPriority = INDEX_NONE;
			if (!TryGetAppliedContextPriority(PlayerInput, Context, AppliedPriority))
			{
				AddPlanIssue(TEXT("runtime_context_not_applied"), TEXT("error"), Operation.TargetPath,
					StableId, TEXT("The exact mapping context is not applied to that loaded player input."));
				continue;
			}
			BaseTokens.Add(TEXT("runtime:") + Operation.RuntimeOwnerPath + TEXT(":")
				+ Operation.TargetPath + TEXT(":") + FString::FromInt(AppliedPriority));
		}
	}

	if (!SimulatedMappings.Mappings.IsEmpty())
	{
		SimulatedMappings.Revision = ComputeSnapshotRevision(SimulatedMappings);
		bool bValidationTruncated = false;
		const TArray<FHyperAIInputIssue> ConflictIssues = ValidateValueSnapshot(
			SimulatedMappings, MaxIssues, bValidationTruncated);
		for (const FHyperAIInputIssue& Issue : ConflictIssues)
		{
			if (Issue.Code == TEXT("references_loaded_only"))
			{
				continue;
			}
			AddPlanIssue(*Issue.Code, *Issue.Severity, Issue.AssetPath, Issue.StableId, Issue.Message);
		}
	}
	if (HasError(Report.Issues))
	{
		Report.Status = TEXT("plan_state_invalid");
		Report.Diagnostic = TEXT("Revision, type, key, mapping, creation, or runtime preconditions failed.");
		return Report;
	}

	BaseTokens.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.enhanced-input-base.v1"));
	for (const FString& Token : BaseTokens) AppendToken(BaseCanonical, Token);
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	if (!IsCanonicalSha256(Report.BaseRevision))
	{
		Report.Status = TEXT("base_revision_unavailable");
		Report.Diagnostic = TEXT("Bounded base-state fingerprint could not be produced.");
		return Report;
	}
	FString PlanCanonical;
	AppendToken(PlanCanonical, TEXT("hyperai.enhanced-input-plan.v1"));
	AppendToken(PlanCanonical, SafetyToString(Safety));
	AppendToken(PlanCanonical, Report.BaseRevision);
	AppendToken(PlanCanonical, Report.TypedOperationType);
	for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Operations)
	{
		const FString ItemHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			OperationCanonical(Operation));
		if (!IsCanonicalSha256(ItemHash))
		{
			Report.Status = TEXT("operation_hash_unavailable");
			Report.Diagnostic = TEXT("A bounded typed operation could not be hashed.");
			return Report;
		}
		AppendToken(PlanCanonical, ItemHash);
	}
	Report.PlanHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(PlanCanonical);
	FString EffectCanonical;
	AppendToken(EffectCanonical, TEXT("hyperai.enhanced-input-effect.v1"));
	AppendToken(EffectCanonical, Report.PlanHash);
	AppendInt(EffectCanonical, Report.Effects.OperationCount);
	AppendInt(EffectCanonical, Report.Effects.ActionsCreated);
	AppendInt(EffectCanonical, Report.Effects.ContextsCreated);
	AppendInt(EffectCanonical, Report.Effects.ActionsUpdated);
	AppendInt(EffectCanonical, Report.Effects.MappingsAdded);
	AppendInt(EffectCanonical, Report.Effects.MappingsUpdated);
	AppendInt(EffectCanonical, Report.Effects.MappingsRemoved);
	AppendInt(EffectCanonical, Report.Effects.AssetsDeleted);
	AppendInt(EffectCanonical, Report.Effects.RuntimePrioritiesChanged);
	AppendBool(EffectCanonical, Report.Effects.bTransactionOnce);
	AppendBool(EffectCanonical, Report.Effects.bSaveOnce);
	AppendBool(EffectCanonical, Report.Effects.bValidateOnce);
	AppendBool(EffectCanonical, Report.Effects.bFreshVerifyOnce);
	Report.EffectFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(EffectCanonical);
	if (!IsCanonicalSha256(Report.PlanHash) || !IsCanonicalSha256(Report.EffectFingerprint))
	{
		Report.Status = TEXT("semantic_hash_unavailable");
		Report.Diagnostic = TEXT("Plan/effect semantic hashes could not be produced.");
		return Report;
	}

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = Report.bRequiresTrustedAuthorization
			? TEXT("valid_requires_trusted_authorization") : TEXT("valid_dry_run");
		Report.Diagnostic = Report.bRequiresTrustedAuthorization
			? TEXT("Exact plan is valid without mutation; execution requires a trusted opaque grant bound by the central executor.")
			: TEXT("Exact loaded revisions, closed operations, effects, and one-finalize lifecycle validated without mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Non-dry-run staging requires one bounded operation_id for the durable central journal.");
		return Report;
	}
	if (!IsCanonicalSha256(Request.ExpectedPlanHash)
		|| Request.ExpectedPlanHash != Report.PlanHash)
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Mutation must echo the exact semantic plan hash returned by dry-run.");
		return Report;
	}
	if (bFastReversibleEdit)
	{
		FString FastDiagnostic;
		const EFastEditAttempt Attempt = ExecuteFastReversibleEdit(Operations, FastDiagnostic);
		if (Attempt == EFastEditAttempt::Applied)
		{
			RecordFastCompletion(Request.OperationId, Report.PlanHash);
			Report.bOk = true;
			Report.bStaged = false;
			Report.bExecutionSubmitted = true;
			Report.bRequiresJournalExecution = false;
			Report.Effects.bSaveOnce = false;
			Report.Status = TEXT("applied_fast_transaction");
			Report.Diagnostic = TEXT("Reversible Enhanced Input edit applied in one Unreal transaction; affected packages are dirty for normal Save/source-control workflow.");
			return Report;
		}
		if (Attempt == EFastEditAttempt::Failed)
		{
			Report.Status = TEXT("fast_transaction_failed");
			Report.Diagnostic = Clip(FastDiagnostic);
			return Report;
		}
	}
	FHyperAIStudioStagedEnhancedInputPlanArtifact Artifact;
	Artifact.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	Artifact.OperationId = Request.OperationId;
	Artifact.PlanHash = Report.PlanHash;
	Artifact.EffectFingerprint = Report.EffectFingerprint;
	Artifact.BaseRevision = Report.BaseRevision;
	Artifact.TypedOperationType = Report.TypedOperationType;
	Artifact.Safety = Safety;
	Artifact.ExpiresUtcMs = NowUtcMs() + ArtifactLifetimeMs;
	Artifact.bRequiresSaveOnce = Report.Effects.bSaveOnce;
	Artifact.bRequiresValidateOnce = Report.Effects.bValidateOnce;
	Artifact.bRequiresFreshVerifyOnce = Report.Effects.bFreshVerifyOnce;
	Artifact.Operations = MoveTemp(Operations);
	FString StageError;
	if (!FHyperAIStudioEnhancedInputPlanStagingService::Stage(Artifact, Report.bReplay, StageError))
	{
		Report.Status = TEXT("stage_rejected");
		Report.Diagnostic = Clip(StageError);
		return Report;
	}
	// Staging is side-effect free but is not successful execution. Keep the MCP result fail-closed
	// until the shared executor exposes an admitted typed Enhanced Input backend.
	Report.bOk = false;
	Report.bStaged = true;
	Report.bExecutionSubmitted = false;
	Report.Status = TEXT("staged_backend_required");
	Report.Diagnostic = TEXT("Typed artifact is staged idempotently; no mutation occurs until PlanExecutionService adds the exact journal/auth/transaction/save/fresh backend.");
	return Report;
}

FHyperAIInputApplyPlanReport UHyperAIStudioEnhancedInputToolset::hyper_input_apply_plan(
	const FHyperAIInputApplyPlanRequest& Request)
{
	return FHyperAIStudioEnhancedInputContracts::BuildPlan(Request);
}

namespace HyperAIStudio::EnhancedInput::Private
{
	struct FStagingState
	{
		FCriticalSection Mutex;
		TMap<FString, FHyperAIStudioStagedEnhancedInputPlanArtifact> Artifacts;
	};

	FStagingState& GetStagingState()
	{
		static FStagingState State;
		return State;
	}

	FString ArtifactKey(const FString& CanonicalProjectId, const FString& OperationId)
	{
		return CanonicalProjectId + TEXT("\n") + OperationId;
	}

	void PruneExpired(FStagingState& State, const int64 Now)
	{
		for (auto It = State.Artifacts.CreateIterator(); It; ++It)
		{
			if (It.Value().ExpiresUtcMs <= Now)
			{
				It.RemoveCurrent();
			}
		}
	}

	FString ComputeArtifactPlanHash(const FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.enhanced-input-plan.v1"));
		AppendToken(Canonical, FHyperAIStudioEnhancedInputContracts::SafetyToString(Artifact.Safety));
		AppendToken(Canonical, Artifact.BaseRevision);
		AppendToken(Canonical, Artifact.TypedOperationType);
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Artifact.Operations)
		{
			const FString Item = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
				OperationCanonical(Operation));
			if (!IsCanonicalSha256(Item))
			{
				return FString();
			}
			AppendToken(Canonical, Item);
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString ComputeArtifactEffectFingerprint(
		const FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact)
	{
		FHyperAIInputPlanEffects Effects;
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Artifact.Operations)
		{
			CountEffect(Operation, Effects);
		}
		Effects.bTransactionOnce = Artifact.Safety != EHyperAIStudioEnhancedInputSafety::ExternalEffect;
		Effects.bSaveOnce = Artifact.bRequiresSaveOnce;
		Effects.bValidateOnce = Artifact.bRequiresValidateOnce;
		Effects.bFreshVerifyOnce = Artifact.bRequiresFreshVerifyOnce;
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.enhanced-input-effect.v1"));
		AppendToken(Canonical, Artifact.PlanHash);
		AppendInt(Canonical, Effects.OperationCount);
		AppendInt(Canonical, Effects.ActionsCreated);
		AppendInt(Canonical, Effects.ContextsCreated);
		AppendInt(Canonical, Effects.ActionsUpdated);
		AppendInt(Canonical, Effects.MappingsAdded);
		AppendInt(Canonical, Effects.MappingsUpdated);
		AppendInt(Canonical, Effects.MappingsRemoved);
		AppendInt(Canonical, Effects.AssetsDeleted);
		AppendInt(Canonical, Effects.RuntimePrioritiesChanged);
		AppendBool(Canonical, Effects.bTransactionOnce);
		AppendBool(Canonical, Effects.bSaveOnce);
		AppendBool(Canonical, Effects.bValidateOnce);
		AppendBool(Canonical, Effects.bFreshVerifyOnce);
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool ValidateArtifact(
		const FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact,
		FString& OutError)
	{
		OutError.Reset();
		if (!IsCanonicalProjectId(Artifact.CanonicalProjectId)
			|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(Artifact.OperationId)
			|| !IsCanonicalSha256(Artifact.PlanHash)
			|| !IsCanonicalSha256(Artifact.EffectFingerprint)
			|| !IsCanonicalSha256(Artifact.BaseRevision)
			|| Artifact.TypedOperationType
				!= FHyperAIStudioEnhancedInputContracts::TypedOperationType(Artifact.Safety)
			|| Artifact.Operations.IsEmpty()
			|| Artifact.Operations.Num() > FHyperAIStudioEnhancedInputContracts::MaxOperations
			|| Artifact.ExpiresUtcMs <= NowUtcMs()
			|| Artifact.ExpiresUtcMs > NowUtcMs()
				+ FHyperAIStudioEnhancedInputContracts::ArtifactLifetimeMs + 1000ll
			|| !Artifact.bRequiresValidateOnce || !Artifact.bRequiresFreshVerifyOnce
			|| (Artifact.Safety == EHyperAIStudioEnhancedInputSafety::ExternalEffect
				&& Artifact.bRequiresSaveOnce)
			|| (Artifact.Safety != EHyperAIStudioEnhancedInputSafety::ExternalEffect
				&& !Artifact.bRequiresSaveOnce))
		{
			OutError = TEXT("Artifact identity, lifetime, safety, or one-finalize lifecycle is invalid.");
			return false;
		}
		for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Artifact.Operations)
		{
			if (Operation.Safety != Artifact.Safety)
			{
				OutError = TEXT("Artifact contains an operation outside its exact safety cohort.");
				return false;
			}
		}
		if (ComputeArtifactPlanHash(Artifact) != Artifact.PlanHash
			|| ComputeArtifactEffectFingerprint(Artifact) != Artifact.EffectFingerprint)
		{
			OutError = TEXT("Artifact plan/effect fingerprints do not bind its exact typed operations.");
			return false;
		}
		return true;
	}
}

bool FHyperAIStudioEnhancedInputContracts::FinalizeArtifactFingerprints(
	FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact,
	FString& OutError)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	OutError.Reset();
	const FString ExactType = TypedOperationType(Artifact.Safety);
	if (!IsCanonicalProjectId(Artifact.CanonicalProjectId)
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(Artifact.OperationId)
		|| !IsCanonicalSha256(Artifact.BaseRevision)
		|| ExactType.IsEmpty()
		|| (!Artifact.TypedOperationType.IsEmpty() && Artifact.TypedOperationType != ExactType)
		|| Artifact.Operations.IsEmpty() || Artifact.Operations.Num() > MaxOperations
		|| Artifact.ExpiresUtcMs <= NowUtcMs()
		|| Artifact.ExpiresUtcMs > NowUtcMs() + ArtifactLifetimeMs + 1000ll
		|| !Artifact.bRequiresValidateOnce || !Artifact.bRequiresFreshVerifyOnce
		|| (Artifact.Safety == EHyperAIStudioEnhancedInputSafety::ExternalEffect
			&& Artifact.bRequiresSaveOnce)
		|| (Artifact.Safety != EHyperAIStudioEnhancedInputSafety::ExternalEffect
			&& !Artifact.bRequiresSaveOnce))
	{
		OutError = TEXT("Artifact identity, lifetime, type, bounds, or one-finalize lifecycle is invalid.");
		return false;
	}
	for (const FHyperAIStudioEnhancedInputBackendOperation& Operation : Artifact.Operations)
	{
		if (Operation.Safety != Artifact.Safety)
		{
			OutError = TEXT("Artifact operations must belong to one exact safety cohort.");
			return false;
		}
	}
	Artifact.TypedOperationType = ExactType;
	Artifact.PlanHash = ComputeArtifactPlanHash(Artifact);
	Artifact.EffectFingerprint = ComputeArtifactEffectFingerprint(Artifact);
	if (!IsCanonicalSha256(Artifact.PlanHash) || !IsCanonicalSha256(Artifact.EffectFingerprint))
	{
		Artifact.PlanHash.Reset();
		Artifact.EffectFingerprint.Reset();
		OutError = TEXT("Bounded deterministic artifact hashes could not be produced.");
		return false;
	}
	return true;
}

bool FHyperAIStudioEnhancedInputPlanStagingService::Stage(
	const FHyperAIStudioStagedEnhancedInputPlanArtifact& Artifact,
	bool& bOutReplay,
	FString& OutError)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	bOutReplay = false;
	if (!ValidateArtifact(Artifact, OutError))
	{
		return false;
	}
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowUtcMs());
	const FString Key = ArtifactKey(Artifact.CanonicalProjectId, Artifact.OperationId);
	if (const FHyperAIStudioStagedEnhancedInputPlanArtifact* Existing = State.Artifacts.Find(Key))
	{
		if (Existing->PlanHash != Artifact.PlanHash
			|| Existing->EffectFingerprint != Artifact.EffectFingerprint
			|| Existing->BaseRevision != Artifact.BaseRevision
			|| Existing->TypedOperationType != Artifact.TypedOperationType
			|| Existing->Safety != Artifact.Safety)
		{
			OutError = TEXT("operation_id is already staged with a different exact plan/effect binding.");
			return false;
		}
		bOutReplay = true;
		return true;
	}
	if (State.Artifacts.Num() >= MaxStagedArtifacts)
	{
		OutError = TEXT("Bounded Enhanced Input staging store is full; wait for expiry or executor claim.");
		return false;
	}
	State.Artifacts.Add(Key, Artifact);
	return true;
}

bool FHyperAIStudioEnhancedInputPlanStagingService::ClaimExact(
	const FString& CanonicalProjectId,
	const FString& OperationId,
	const FString& PlanHash,
	FHyperAIStudioStagedEnhancedInputPlanArtifact& OutArtifact,
	FString& OutError)
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	OutArtifact = {};
	OutError.Reset();
	if (!IsCanonicalProjectId(CanonicalProjectId)
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(OperationId)
		|| !IsCanonicalSha256(PlanHash))
	{
		OutError = TEXT("Claim identity is malformed.");
		return false;
	}
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowUtcMs());
	const FString Key = ArtifactKey(CanonicalProjectId, OperationId);
	FHyperAIStudioStagedEnhancedInputPlanArtifact* Existing = State.Artifacts.Find(Key);
	if (!Existing || Existing->PlanHash != PlanHash)
	{
		OutError = TEXT("No exact unexpired Enhanced Input artifact matches the project/operation/plan claim.");
		return false;
	}
	OutArtifact = MoveTemp(*Existing);
	State.Artifacts.Remove(Key);
	return true;
}

void FHyperAIStudioEnhancedInputPlanStagingService::Shutdown()
{
	using namespace HyperAIStudio::EnhancedInput::Private;
	ResetFastReplayState();
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	State.Artifacts.Reset();
}

void FHyperAIStudioEnhancedInputRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioEnhancedInputRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioEnhancedInputRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioEnhancedInputToolset::StaticClass(),
			FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioEnhancedInput, Warning,
				TEXT("Could not unregister the owned Enhanced Input toolset: %s"), *Error);
		}
	}
	FHyperAIStudioEnhancedInputPlanStagingService::Shutdown();
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioEnhancedInputRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(
			FHyperAIStudioEnhancedInputContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioEnhancedInputToolset::StaticClass(),
			FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioEnhancedInputRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioEnhancedInputContracts::IsRegistrationAllowed(
			FHyperAIStudioEnhancedInputContracts::IsPendingTestRegistrationEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioEnhancedInputToolset::StaticClass(),
		FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioEnhancedInputToolset::StaticClass(),
			FHyperAIStudioEnhancedInputContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioEnhancedInput, Error,
				TEXT("ToolsetRegistry rejected the owned three-tool Enhanced Input cohort: %s"),
				*Error);
		}
	}
}
