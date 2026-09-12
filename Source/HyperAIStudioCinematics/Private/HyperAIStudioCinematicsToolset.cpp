// Games by Hyper 2026.

#include "HyperAIStudioCinematicsToolset.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/Engine.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MoviePipelineConfigBase.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineSetting.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneDoubleSection.h"
#include "Sections/MovieSceneEventRepeaterSection.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioCinematics, Log, All);

namespace HyperAIStudio::Cinematics::Private
{
	static constexpr int32 MaxFixedChannelsPerSection = 16;
	static constexpr int32 MaxIssueDetailCharacters = 512;
	static constexpr int32 BaseResponseBudgetBytes = 12288;

	bool HasControlCharacter(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character < 0x20 || Character == 0x7f)
			{
				return true;
			}
		}
		return false;
	}

	bool IsLowerHexString(const FString& Value, const int32 ExpectedLength)
	{
		if (Value.Len() != ExpectedLength) return false;
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

	int32 SaturatingJsonBytesFromCharacters(const int32 Characters)
	{
		if (Characters <= 0) return 0;
		return Characters > MAX_int32 / 6 ? MAX_int32 : Characters * 6;
	}

	int32 SaturatingAdd(const int32 A, const int32 B)
	{
		return A > MAX_int32 - B ? MAX_int32 : A + B;
	}

	bool AppendToken(FString& Canonical, const FString& Token)
	{
		const int64 Added = static_cast<int64>(Token.Len()) + 24;
		if (Added < 0 || static_cast<int64>(Canonical.Len()) + Added
			> FHyperAIStudioExtensionRuntime::MaxHashInputBytes / sizeof(TCHAR))
		{
			Canonical.Reset();
			return false;
		}
		Canonical += FString::Printf(TEXT("%d:"), Token.Len());
		Canonical += Token;
		Canonical += TEXT("|");
		return true;
	}

	FString HashCanonical(const FString& Canonical)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool MaterializeNameBounded(const FName Name, const int32 MaxCharacters, FString& Out)
	{
		Out.Reset();
		if (Name.IsNone() || Name.GetStringLength() > static_cast<uint32>(MaxCharacters))
		{
			return false;
		}
		Name.ToString(Out);
		return !Out.IsEmpty() && Out.Len() <= MaxCharacters && !HasControlCharacter(Out);
	}

	void AddIssue(
		TArray<FHyperAICinematicsIssue>& Issues,
		const FString& Code,
		const FString& Severity,
		const FString& StableId,
		const FString& Subject,
		const FString& Detail)
	{
		if (Issues.Num() >= FHyperAIStudioCinematicsContracts::MaxIssues)
		{
			return;
		}
		FHyperAICinematicsIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Code = Code.Left(FHyperAIStudioCinematicsContracts::MaxNameCharacters);
		Issue.Severity = Severity.Left(16);
		Issue.StableId = StableId.Left(FHyperAIStudioCinematicsContracts::MaxNameCharacters);
		Issue.Subject = Subject.Left(FHyperAIStudioCinematicsContracts::MaxPathCharacters);
		Issue.Detail = Detail.Left(MaxIssueDetailCharacters);
	}

	struct FCaptureContext
	{
		explicit FCaptureContext(const int32 MaxWorkMs)
			: DeadlineSeconds(FPlatformTime::Seconds()
				+ static_cast<double>(FMath::Clamp(MaxWorkMs, 1,
					FHyperAIStudioCinematicsContracts::MaxReadGameThreadMs)) / 1000.0)
		{
		}

		bool CheckDeadline(const FString& StableId, const FString& Subject)
		{
			if (bDeadlineExceeded) return false;
			if (FPlatformTime::Seconds() <= DeadlineSeconds)
			{
				return true;
			}
			bDeadlineExceeded = true;
			MarkIncomplete(TEXT("capture_deadline_exceeded"), StableId, Subject,
				TEXT("The bounded loaded-value capture deadline expired."));
			return false;
		}

		void MarkIncomplete(
			const FString& Code,
			const FString& StableId,
			const FString& Subject,
			const FString& Detail,
			const FString& Severity = TEXT("warning"))
		{
			bComplete = false;
			AddIssue(Issues, Code, Severity, StableId, Subject, Detail);
		}

		double DeadlineSeconds = 0.0;
		bool bComplete = true;
		bool bDeadlineExceeded = false;
		TArray<FHyperAICinematicsIssue> Issues;
	};

	FHyperAICinematicsFrameRange ToFrameRange(const TRange<FFrameNumber>& Range)
	{
		FHyperAICinematicsFrameRange Result;
		Result.bHasLowerBound = Range.HasLowerBound();
		if (Result.bHasLowerBound)
		{
			Result.LowerFrame = Range.GetLowerBoundValue().Value;
			Result.bLowerInclusive = Range.GetLowerBound().IsInclusive();
		}
		Result.bHasUpperBound = Range.HasUpperBound();
		if (Result.bHasUpperBound)
		{
			Result.UpperFrame = Range.GetUpperBoundValue().Value;
			Result.bUpperInclusive = Range.GetUpperBound().IsInclusive();
		}
		return Result;
	}

	bool IsRangeWellFormed(const FHyperAICinematicsFrameRange& Range)
	{
		if (!Range.bHasLowerBound || !Range.bHasUpperBound)
		{
			return true;
		}
		return Range.LowerFrame <= Range.UpperFrame;
	}

	bool ContainsFrame(const FHyperAICinematicsFrameRange& Range, const int32 Frame)
	{
		if (Range.bHasLowerBound
			&& (Frame < Range.LowerFrame
				|| (Frame == Range.LowerFrame && !Range.bLowerInclusive)))
		{
			return false;
		}
		if (Range.bHasUpperBound
			&& (Frame > Range.UpperFrame
				|| (Frame == Range.UpperFrame && !Range.bUpperInclusive)))
		{
			return false;
		}
		return true;
	}

	bool RangesEqual(
		const FHyperAICinematicsFrameRange& A,
		const FHyperAICinematicsFrameRange& B)
	{
		return A.bHasLowerBound == B.bHasLowerBound
			&& A.bLowerInclusive == B.bLowerInclusive
			&& A.LowerFrame == B.LowerFrame
			&& A.bHasUpperBound == B.bHasUpperBound
			&& A.bUpperInclusive == B.bUpperInclusive
			&& A.UpperFrame == B.UpperFrame;
	}

	bool IsHardBoundedChannelSection(const UMovieSceneSection* Section)
	{
		if (!Section) return false;
		const UClass* Class = Section->GetClass();
		if (Class == UMovieSceneDoubleSection::StaticClass()) return true;
		if (Class != UMovieSceneFloatSection::StaticClass()) return false;
		const FObjectPropertyBase* OverrideProperty = FindFProperty<FObjectPropertyBase>(
			Class, TEXT("OverrideRegistry"));
		return OverrideProperty
			&& OverrideProperty->GetObjectPropertyValue_InContainer(Section) == nullptr;
	}

	struct FPackageEvidence
	{
		FString PackageName;
		FString DiskExistence = TEXT("unknown");
		FString PackageSavedHash;
		int64 DiskSize = -1;
		bool bWasLoadedFromDisk = false;
		bool bPackageDirty = false;
		FString PersistedRevision;
		bool bPersistedRevisionComplete = false;
	};

	bool CapturePackageEvidence(
		const UObject* Object,
		FCaptureContext& Context,
		const FString& StableId,
		FPackageEvidence& Out)
	{
		Out = {};
		if (!Object || !Object->GetPackage())
		{
			Context.MarkIncomplete(TEXT("package_identity_missing"), StableId, FString(),
				TEXT("The loaded object has no package identity."));
			return false;
		}
		const UPackage* Package = Object->GetPackage();
		if (Package == GetTransientPackage()
			|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor)
			|| !MaterializeNameBounded(Package->GetFName(),
				FHyperAIStudioCinematicsContracts::MaxPathCharacters, Out.PackageName))
		{
			Context.MarkIncomplete(TEXT("transient_or_invalid_package_unsupported"), StableId,
				FString(), TEXT("Only bounded, non-PIE project packages are supported."));
			return false;
		}
		Out.bPackageDirty = Package->IsDirty();
		Out.bWasLoadedFromDisk = Object->HasAnyFlags(RF_WasLoaded)
			|| Package->HasAnyFlags(RF_WasLoaded);
		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Registry)
		{
			Context.MarkIncomplete(TEXT("asset_registry_not_initialized"), StableId,
				Out.PackageName, TEXT("Non-blocking package evidence is unavailable."));
		}
		else
		{
			FAssetPackageData PackageData;
			const UE::AssetRegistry::EExists Exists = Registry->TryGetAssetPackageData(
				Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
			Out.DiskExistence =
				FHyperAIStudioCinematicsContracts::ClassifyAssetRegistryExistence(Exists);
			if (Exists == UE::AssetRegistry::EExists::Exists)
			{
				Out.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
				Out.DiskSize = PackageData.DiskSize;
				if (!IsLowerHexString(Out.PackageSavedHash, 40))
				{
					Context.MarkIncomplete(TEXT("package_saved_hash_shape_invalid"), StableId,
						Out.PackageName, TEXT("Asset Registry returned a non-canonical saved hash."));
				}
			}
			else if (Exists == UE::AssetRegistry::EExists::Unknown)
			{
				Context.MarkIncomplete(TEXT("asset_registry_package_state_unknown"), StableId,
					Out.PackageName, TEXT("The package tri-state query could not acquire its read lock."));
			}
			else
			{
				Context.MarkIncomplete(TEXT("loaded_package_not_persisted"), StableId,
					Out.PackageName, TEXT("The loaded object has no known persisted package."));
			}
		}
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.persisted-package.v1"));
		AppendToken(Canonical, Out.PackageName);
		AppendToken(Canonical, Out.DiskExistence);
		AppendToken(Canonical, Out.PackageSavedHash);
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Out.DiskSize));
		Out.PersistedRevision = HashCanonical(Canonical);
		Out.bPersistedRevisionComplete = Out.DiskExistence == TEXT("exists")
			&& IsLowerHexString(Out.PackageSavedHash, 40) && Out.DiskSize >= 0
			&& Out.bWasLoadedFromDisk
			&& FHyperAIStudioCinematicsContracts::IsCanonicalSha256(Out.PersistedRevision);
		if (!Out.bPersistedRevisionComplete)
		{
			Context.MarkIncomplete(TEXT("persisted_revision_incomplete"), StableId,
				Out.PackageName,
				TEXT("Persisted CAS requires a loaded-from-disk package and complete non-blocking package evidence."));
		}
		return Out.bPersistedRevisionComplete;
	}

	FString HashBinding(const FHyperAICinematicsBindingRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.BindingId);
		AppendToken(Canonical, Record.BindingGuid);
		AppendToken(Canonical, Record.Name);
		AppendToken(Canonical, Record.Kind);
		AppendToken(Canonical, Record.bDetailProjectionComplete ? TEXT("detail_complete") : TEXT("detail_partial"));
		AppendToken(Canonical, Record.DetailFingerprint);
		AppendToken(Canonical, FString::FromInt(Record.TrackCount));
		return HashCanonical(Canonical);
	}

	FString HashTrack(const FHyperAICinematicsTrackRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.TrackId);
		AppendToken(Canonical, Record.BindingGuid);
		AppendToken(Canonical, Record.ClassName);
		AppendToken(Canonical, Record.bRootTrack ? TEXT("root") : TEXT("binding"));
		AppendToken(Canonical, Record.bCameraCutTrack ? TEXT("camera") : TEXT("ordinary"));
		AppendToken(Canonical, Record.bEventTrack ? TEXT("event") : TEXT("non_event"));
		AppendToken(Canonical, Record.bDetailProjectionComplete ? TEXT("detail_complete") : TEXT("detail_partial"));
		AppendToken(Canonical, Record.DetailFingerprint);
		AppendToken(Canonical, FString::FromInt(Record.SectionCount));
		return HashCanonical(Canonical);
	}

	void AppendRange(FString& Canonical, const FHyperAICinematicsFrameRange& Range)
	{
		AppendToken(Canonical, Range.bHasLowerBound ? TEXT("lower") : TEXT("open_lower"));
		AppendToken(Canonical, Range.bLowerInclusive ? TEXT("inclusive") : TEXT("exclusive"));
		AppendToken(Canonical, FString::FromInt(Range.LowerFrame));
		AppendToken(Canonical, Range.bHasUpperBound ? TEXT("upper") : TEXT("open_upper"));
		AppendToken(Canonical, Range.bUpperInclusive ? TEXT("inclusive") : TEXT("exclusive"));
		AppendToken(Canonical, FString::FromInt(Range.UpperFrame));
	}

	FString HashSection(const FHyperAICinematicsSectionRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.SectionId);
		AppendToken(Canonical, Record.TrackId);
		AppendToken(Canonical, Record.ClassName);
		AppendRange(Canonical, Record.Range);
		AppendToken(Canonical, FString::FromInt(Record.RowIndex));
		AppendToken(Canonical, FString::FromInt(Record.OverlapPriority));
		AppendToken(Canonical, FString::FromInt(Record.PreRollFrames));
		AppendToken(Canonical, FString::FromInt(Record.PostRollFrames));
		AppendToken(Canonical, Record.bActive ? TEXT("active") : TEXT("inactive"));
		AppendToken(Canonical, Record.bLocked ? TEXT("locked") : TEXT("unlocked"));
		AppendToken(Canonical, Record.bDetailProjectionComplete ? TEXT("detail_complete") : TEXT("detail_partial"));
		AppendToken(Canonical, Record.DetailFingerprint);
		AppendToken(Canonical, Record.bChannelProjectionSupported ? TEXT("channels_complete") : TEXT("channels_partial"));
		AppendToken(Canonical, Record.ChannelFingerprint);
		AppendToken(Canonical, FString::FromInt(Record.ChannelCount));
		AppendToken(Canonical, FString::FromInt(Record.KeyCount));
		return HashCanonical(Canonical);
	}

	FString HashKey(const FHyperAICinematicsKeyRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.KeyId);
		AppendToken(Canonical, Record.SectionId);
		AppendToken(Canonical, Record.ChannelType);
		AppendToken(Canonical, FString::FromInt(Record.ChannelIndex));
		AppendToken(Canonical, FString::FromInt(Record.KeyIndex));
		AppendToken(Canonical, FString::FromInt(Record.Frame));
		AppendToken(Canonical, Record.ValueFingerprint);
		return HashCanonical(Canonical);
	}

	FString HashCamera(const FHyperAICinematicsCameraRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.CameraCutId);
		AppendToken(Canonical, Record.SectionId);
		AppendToken(Canonical, Record.CameraBindingId);
		AppendToken(Canonical, FString::FromInt(Record.CameraBindingSequenceId));
		AppendToken(Canonical, FString::FromInt(Record.CameraBindingResolveParentIndex));
		AppendToken(Canonical, Record.bLockPreviousCamera ? TEXT("lock_previous") : TEXT("do_not_lock_previous"));
		AppendToken(Canonical, Record.BindingFingerprint);
		AppendRange(Canonical, Record.Range);
		return HashCanonical(Canonical);
	}

	FString HashEvent(const FHyperAICinematicsEventRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.EventId);
		AppendToken(Canonical, Record.SectionId);
		AppendToken(Canonical, Record.EventKind);
		AppendToken(Canonical, FString::FromInt(Record.Frame));
		AppendToken(Canonical, Record.CompiledFunctionName);
		AppendToken(Canonical, Record.BoundObjectClassName);
		AppendToken(Canonical, FString::FromInt(Record.PayloadVariableCount));
		AppendToken(Canonical, Record.bDetailProjectionComplete ? TEXT("detail_complete") : TEXT("detail_partial"));
		AppendToken(Canonical, Record.DetailFingerprint);
		return HashCanonical(Canonical);
	}

	FString HashMRQSetting(const FHyperAICinematicsMRQSettingRecord& Record)
	{
		FString Canonical;
		AppendToken(Canonical, Record.SettingId);
		AppendToken(Canonical, Record.ClassName);
		AppendToken(Canonical, Record.bEnabled ? TEXT("enabled") : TEXT("disabled"));
		AppendToken(Canonical, Record.bDetailProjectionComplete ? TEXT("complete") : TEXT("partial"));
		AppendToken(Canonical, Record.DetailFingerprint);
		return HashCanonical(Canonical);
	}

	FString ComputeSequenceVolatileRevision(const FHyperAIStudioCinematicsValueSnapshot& Snapshot)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.sequence-volatile.v1"));
		AppendToken(Canonical, Snapshot.Sequence.TargetPath);
		AppendToken(Canonical, Snapshot.Sequence.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
		AppendToken(Canonical, FString::FromInt(Snapshot.Sequence.TickResolutionNumerator));
		AppendToken(Canonical, FString::FromInt(Snapshot.Sequence.TickResolutionDenominator));
		AppendToken(Canonical, FString::FromInt(Snapshot.Sequence.DisplayRateNumerator));
		AppendToken(Canonical, FString::FromInt(Snapshot.Sequence.DisplayRateDenominator));
		AppendRange(Canonical, Snapshot.Sequence.PlaybackRange);
		for (const FHyperAICinematicsBindingRecord& Item : Snapshot.Bindings)
		{
			if (!AppendToken(Canonical, HashBinding(Item))) return FString();
		}
		for (const FHyperAICinematicsTrackRecord& Item : Snapshot.Tracks)
		{
			if (!AppendToken(Canonical, HashTrack(Item))) return FString();
		}
		for (const FHyperAICinematicsSectionRecord& Item : Snapshot.Sections)
		{
			if (!AppendToken(Canonical, HashSection(Item))) return FString();
		}
		for (const FHyperAICinematicsKeyRecord& Item : Snapshot.Keys)
		{
			if (!AppendToken(Canonical, HashKey(Item))) return FString();
		}
		for (const FHyperAICinematicsCameraRecord& Item : Snapshot.Cameras)
		{
			if (!AppendToken(Canonical, HashCamera(Item))) return FString();
		}
		for (const FHyperAICinematicsEventRecord& Item : Snapshot.Events)
		{
			if (!AppendToken(Canonical, HashEvent(Item))) return FString();
		}
		return HashCanonical(Canonical);
	}

	FString ComputeMRQVolatileRevision(const FHyperAIStudioCinematicsValueSnapshot& Snapshot)
	{
		if (!Snapshot.RenderConfig.bPresent) return FString();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.mrq-volatile.v1"));
		AppendToken(Canonical, Snapshot.RenderConfig.TargetPath);
		AppendToken(Canonical, Snapshot.RenderConfig.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.SettingsSerialNumber));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.SettingCount));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.ShotOverrideCount));
		AppendToken(Canonical, Snapshot.RenderConfig.bOutputSettingPresent
			? TEXT("output_present") : TEXT("output_absent"));
		AppendToken(Canonical, Snapshot.RenderConfig.OutputSettingFingerprint);
		AppendToken(Canonical, Snapshot.RenderConfig.OutputDirectory);
		AppendToken(Canonical, Snapshot.RenderConfig.FileNameFormat);
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.OutputWidth));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.OutputHeight));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.OutputRateNumerator));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.OutputRateDenominator));
		AppendToken(Canonical, Snapshot.RenderConfig.bUseCustomFrameRate
			? TEXT("custom_rate") : TEXT("sequence_rate"));
		AppendToken(Canonical, Snapshot.RenderConfig.bUseCustomPlaybackRange
			? TEXT("custom_range") : TEXT("sequence_range"));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.CustomStartFrame));
		AppendToken(Canonical, FString::FromInt(Snapshot.RenderConfig.CustomEndFrame));
		for (const FHyperAICinematicsMRQSettingRecord& Item : Snapshot.RenderSettings)
		{
			if (!AppendToken(Canonical, HashMRQSetting(Item))) return FString();
		}
		return HashCanonical(Canonical);
	}

	FString EventFunctionName(const FMovieSceneEvent& Event, bool& bComplete)
	{
		FString Name;
#if WITH_EDITORONLY_DATA
		if (!Event.CompiledFunctionName.IsNone())
		{
			if (!MaterializeNameBounded(Event.CompiledFunctionName,
				FHyperAIStudioCinematicsContracts::MaxNameCharacters, Name))
			{
				bComplete = false;
			}
		}
#endif
		if (Name.IsEmpty() && Event.Ptrs.Function)
		{
			if (!MaterializeNameBounded(Event.Ptrs.Function->GetFName(),
				FHyperAIStudioCinematicsContracts::MaxNameCharacters, Name))
			{
				bComplete = false;
			}
		}
		return Name;
	}

	FString EventBoundObjectClassName(const FMovieSceneEvent& Event, bool& bComplete)
	{
		UClass* Class = Event.GetBoundObjectPropertyClass();
		if (!Class) return FString();
		FString Name;
		if (!MaterializeNameBounded(Class->GetFName(),
			FHyperAIStudioCinematicsContracts::MaxNameCharacters, Name))
		{
			bComplete = false;
		}
		return Name;
	}

	bool AppendFiniteFloat(FString& Canonical, const float Value)
	{
		if (!FMath::IsFinite(Value)) return false;
		return AppendToken(Canonical, FString::Printf(TEXT("%.9g"),
			static_cast<double>(Value)));
	}

	bool AppendFiniteDouble(FString& Canonical, const double Value)
	{
		if (!FMath::IsFinite(Value)) return false;
		return AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Value));
	}

	bool AppendTangent(FString& Canonical, const FMovieSceneTangentData& Tangent)
	{
		return AppendFiniteFloat(Canonical, Tangent.ArriveTangent)
			&& AppendFiniteFloat(Canonical, Tangent.LeaveTangent)
			&& AppendFiniteFloat(Canonical, Tangent.ArriveTangentWeight)
			&& AppendFiniteFloat(Canonical, Tangent.LeaveTangentWeight)
			&& AppendToken(Canonical, FString::FromInt(
				static_cast<int32>(Tangent.TangentWeightMode.GetValue())));
	}

	FString HashFloatValue(const FMovieSceneFloatValue& Value)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.float-key-value.v1"));
		if (!AppendFiniteFloat(Canonical, Value.Value)
			|| !AppendTangent(Canonical, Value.Tangent)
			|| !AppendToken(Canonical, FString::FromInt(
				static_cast<int32>(Value.InterpMode.GetValue())))
			|| !AppendToken(Canonical, FString::FromInt(
				static_cast<int32>(Value.TangentMode.GetValue()))))
		{
			return FString();
		}
		return HashCanonical(Canonical);
	}

	FString HashDoubleValue(const FMovieSceneDoubleValue& Value)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.double-key-value.v1"));
		if (!AppendFiniteDouble(Canonical, Value.Value)
			|| !AppendTangent(Canonical, Value.Tangent)
			|| !AppendToken(Canonical, FString::FromInt(
				static_cast<int32>(Value.InterpMode.GetValue())))
			|| !AppendToken(Canonical, FString::FromInt(
				static_cast<int32>(Value.TangentMode.GetValue()))))
		{
			return FString();
		}
		return HashCanonical(Canonical);
	}

	bool AppendFloatChannelHeader(FString& Canonical, const FMovieSceneFloatChannel& Channel)
	{
		AppendToken(Canonical, FString::FromInt(
			static_cast<int32>(Channel.PreInfinityExtrap.GetValue())));
		AppendToken(Canonical, FString::FromInt(
			static_cast<int32>(Channel.PostInfinityExtrap.GetValue())));
		const TOptional<float> Default = Channel.GetDefault();
		AppendToken(Canonical, Default.IsSet() ? TEXT("has_default") : TEXT("no_default"));
		if (Default.IsSet() && !AppendFiniteFloat(Canonical, Default.GetValue())) return false;
		const FFrameRate Tick = Channel.GetTickResolution();
		AppendToken(Canonical, FString::FromInt(Tick.Numerator));
		AppendToken(Canonical, FString::FromInt(Tick.Denominator));
		return Tick.Numerator > 0 && Tick.Denominator > 0;
	}

	bool AppendDoubleChannelHeader(FString& Canonical, const FMovieSceneDoubleChannel& Channel)
	{
		AppendToken(Canonical, FString::FromInt(
			static_cast<int32>(Channel.PreInfinityExtrap.GetValue())));
		AppendToken(Canonical, FString::FromInt(
			static_cast<int32>(Channel.PostInfinityExtrap.GetValue())));
		const TOptional<double> Default = Channel.GetDefault();
		AppendToken(Canonical, Default.IsSet() ? TEXT("has_default") : TEXT("no_default"));
		if (Default.IsSet() && !AppendFiniteDouble(Canonical, Default.GetValue())) return false;
		const FFrameRate Tick = Channel.GetTickResolution();
		AppendToken(Canonical, FString::FromInt(Tick.Numerator));
		AppendToken(Canonical, FString::FromInt(Tick.Denominator));
		return Tick.Numerator > 0 && Tick.Denominator > 0;
	}

	bool BuildBaseSectionDetail(
		const UMovieSceneSection* Section,
		FCaptureContext& Context,
		const FString& SectionId,
		FString& OutFingerprint)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.section-detail.v1"));
		AppendToken(Canonical, FString::FromInt(
			static_cast<int32>(Section->EvalOptions.CompletionMode)));
		AppendToken(Canonical, Section->EvalOptions.bCanEditCompletionMode
			? TEXT("completion_editable") : TEXT("completion_fixed"));
		AppendToken(Canonical, FString::FromInt(Section->Easing.AutoEaseInDuration));
		AppendToken(Canonical, FString::FromInt(Section->Easing.AutoEaseOutDuration));
		AppendToken(Canonical, Section->Easing.bManualEaseIn
			? TEXT("manual_ease_in") : TEXT("auto_ease_in"));
		AppendToken(Canonical, FString::FromInt(Section->Easing.ManualEaseInDuration));
		AppendToken(Canonical, Section->Easing.bManualEaseOut
			? TEXT("manual_ease_out") : TEXT("auto_ease_out"));
		AppendToken(Canonical, FString::FromInt(Section->Easing.ManualEaseOutDuration));
		bool bComplete = true;
		if (Section->Easing.EaseIn.GetObject() || Section->Easing.EaseOut.GetObject())
		{
			bComplete = false;
			Context.MarkIncomplete(TEXT("custom_section_easing_projection_limited"),
				SectionId, Section->GetClass()->GetName(),
				TEXT("Custom easing UObject state is outside the bounded v1 section projection."));
		}
		if (Section->ConditionContainer.Condition)
		{
			bComplete = false;
			Context.MarkIncomplete(TEXT("section_condition_projection_limited"),
				SectionId, Section->GetClass()->GetName(),
				TEXT("Dynamic MovieScene condition UObject state is outside the bounded v1 projection."));
		}
		const FTimecode& Timecode = Section->TimecodeSource.Timecode;
		AppendToken(Canonical, FString::FromInt(Timecode.Hours));
		AppendToken(Canonical, FString::FromInt(Timecode.Minutes));
		AppendToken(Canonical, FString::FromInt(Timecode.Seconds));
		AppendToken(Canonical, FString::FromInt(Timecode.Frames));
		if (!AppendFiniteFloat(Canonical, Timecode.Subframe)) bComplete = false;
		AppendToken(Canonical, Timecode.bDropFrameFormat ? TEXT("drop") : TEXT("non_drop"));
		const FOptionalMovieSceneBlendType Blend = Section->GetBlendType();
		AppendToken(Canonical, Blend.IsValid() ? TEXT("blend") : TEXT("no_blend"));
		if (Blend.IsValid()) AppendToken(Canonical,
			FString::FromInt(static_cast<int32>(Blend.Get())));
		AppendToken(Canonical, FString::FromInt(Section->GetBlendingOrder()));
		AppendToken(Canonical, Section->GetSupportsInfiniteRange()
			? TEXT("supports_infinite") : TEXT("finite_only"));
#if WITH_EDITORONLY_DATA
		AppendToken(Canonical, Section->IsLocalEvalDisabled()
			? TEXT("local_eval_disabled") : TEXT("local_eval_enabled"));
		const FColor Tint = Section->GetColorTint();
		AppendToken(Canonical, FString::Printf(TEXT("%u,%u,%u,%u"),
			static_cast<uint32>(Tint.R), static_cast<uint32>(Tint.G),
			static_cast<uint32>(Tint.B), static_cast<uint32>(Tint.A)));
#endif
		OutFingerprint = HashCanonical(Canonical);
		return bComplete
			&& FHyperAIStudioCinematicsContracts::IsCanonicalSha256(OutFingerprint);
	}

	int32 EstimateRecordBytes(const FString& A, const FString& B, const FString& C = FString())
	{
		int32 Bytes = 384;
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(A.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(B.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(C.Len()));
		return Bytes;
	}

	int32 EstimateIssueBytes(const FHyperAICinematicsIssue& Issue)
	{
		int32 Bytes = 256;
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Issue.Code.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Issue.Severity.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Issue.StableId.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Issue.Subject.Len()));
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Issue.Detail.Len()));
		return Bytes;
	}

	int32 BoundedRequestSize(
		const std::initializer_list<const FString*> Strings,
		const int32 FixedBytes)
	{
		int32 Bytes = FixedBytes;
		for (const FString* Value : Strings)
		{
			if (!Value) return MAX_int32;
			Bytes = SaturatingAdd(Bytes,
				static_cast<int32>(FMath::Min<SIZE_T>(
					Value->GetAllocatedSize(), MAX_int32)));
		}
		return Bytes;
	}

	int32 EstimateCapabilityBytes(const FHyperAICinematicsCapabilityStatus& Capability)
	{
		int32 Bytes = 1024;
		auto AddString = [&Bytes](const FString& Value)
		{
			Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Value.Len()));
		};
		AddString(Capability.Family);
		AddString(Capability.State);
		AddString(Capability.Remediation);
		AddString(Capability.DelegationAuthorityFingerprint);
		for (const FString& Value : Capability.SupportedCases) AddString(Value);
		for (const FString& Value : Capability.UnsupportedCases) AddString(Value);
		for (const FHyperAICinematicsDelegationAuthority& Value : Capability.DelegationAuthority)
		{
			Bytes = SaturatingAdd(Bytes, 384);
			AddString(Value.AuthorityKind);
			AddString(Value.SourceGroup);
			AddString(Value.ReviewedFingerprint);
			AddString(Value.SourceCoordinate);
			AddString(Value.Disposition);
		}
		return Bytes;
	}

	int32 EstimateCapabilitiesBytes(const TArray<FHyperAICinematicsCapabilityStatus>& Capabilities)
	{
		int32 Bytes = 256;
		for (const FHyperAICinematicsCapabilityStatus& Capability : Capabilities)
		{
			Bytes = SaturatingAdd(Bytes, EstimateCapabilityBytes(Capability));
		}
		return Bytes;
	}

	int32 EstimateSequenceBytes(const FHyperAICinematicsSequenceRecord& Value)
	{
		int32 Bytes = 1536;
		for (const FString* Field : {&Value.TargetPath, &Value.PackageName, &Value.ClassName,
			&Value.DiskExistence, &Value.PackageSavedHash, &Value.PersistedRevision,
			&Value.VolatileRevision})
		{
			Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Field->Len()));
		}
		return Bytes;
	}

	int32 EstimateConfigBytes(const FHyperAICinematicsMRQConfigRecord& Value)
	{
		int32 Bytes = 1536;
		for (const FString* Field : {&Value.TargetPath, &Value.PackageName, &Value.DiskExistence,
			&Value.PackageSavedHash, &Value.PersistedRevision, &Value.VolatileRevision,
			&Value.OutputSettingFingerprint, &Value.OutputDirectory, &Value.FileNameFormat})
		{
			Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Field->Len()));
		}
		return Bytes;
	}

	FString RecomputePersistedRevision(
		const FString& PackageName,
		const FString& DiskExistence,
		const FString& PackageSavedHash,
		const int64 DiskSize)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.cinematics.persisted-package.v1"));
		AppendToken(Canonical, PackageName);
		AppendToken(Canonical, DiskExistence);
		AppendToken(Canonical, PackageSavedHash);
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), DiskSize));
		return HashCanonical(Canonical);
	}

	bool IsDetachedSnapshotEnvelopeBounded(
		const FHyperAIStudioCinematicsValueSnapshot& Snapshot,
		FString& OutError)
	{
		OutError.Reset();
		if (Snapshot.CaptureIssues.Num() < 0
			|| Snapshot.CaptureIssues.Num() > FHyperAIStudioCinematicsContracts::MaxIssues)
		{
			OutError = TEXT("capture_issue_count_exceeded");
			return false;
		}
		if (!FHyperAIStudioCinematicsContracts::AdmitCountsBeforeProjection(
			Snapshot.Bindings.Num(), Snapshot.Tracks.Num(), Snapshot.Sections.Num(),
			Snapshot.Sequence.ChannelCount, Snapshot.Keys.Num(), Snapshot.Cameras.Num(),
			Snapshot.Events.Num(), Snapshot.RenderSettings.Num(), OutError))
		{
			return false;
		}
		auto Text = [](const FString& Value, const int32 MaxCharacters)
		{
			return Value.Len() <= MaxCharacters && !HasControlCharacter(Value);
		};
		auto Name = [&](const FString& Value)
		{
			return Text(Value, FHyperAIStudioCinematicsContracts::MaxNameCharacters);
		};
		auto Path = [&](const FString& Value)
		{
			return Text(Value, FHyperAIStudioCinematicsContracts::MaxPathCharacters);
		};
		auto Seal = [&](const FString& Value)
		{
			return Text(Value, 71);
		};
		auto SavedHash = [&](const FString& Value)
		{
			return Text(Value, 40);
		};
		const FHyperAICinematicsSequenceRecord& Sequence = Snapshot.Sequence;
		if (!Path(Sequence.TargetPath) || !Path(Sequence.PackageName)
			|| !Name(Sequence.ClassName) || !Name(Sequence.DiskExistence)
			|| !SavedHash(Sequence.PackageSavedHash) || !Seal(Sequence.PersistedRevision)
			|| !Seal(Sequence.VolatileRevision) || !Seal(Snapshot.SnapshotFingerprint))
		{
			OutError = TEXT("sequence_text_envelope_exceeded");
			return false;
		}
		const FHyperAICinematicsMRQConfigRecord& Config = Snapshot.RenderConfig;
		if (!Path(Config.TargetPath) || !Path(Config.PackageName)
			|| !Name(Config.DiskExistence) || !SavedHash(Config.PackageSavedHash)
			|| !Seal(Config.PersistedRevision) || !Seal(Config.VolatileRevision)
			|| !Seal(Config.OutputSettingFingerprint) || !Path(Config.OutputDirectory)
			|| !Path(Config.FileNameFormat))
		{
			OutError = TEXT("render_config_text_envelope_exceeded");
			return false;
		}
		for (const FHyperAICinematicsBindingRecord& Value : Snapshot.Bindings)
		{
			if (!Name(Value.BindingId) || !Name(Value.BindingGuid) || !Name(Value.Name)
				|| !Name(Value.Kind) || !Seal(Value.DetailFingerprint))
			{
				OutError = TEXT("binding_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsTrackRecord& Value : Snapshot.Tracks)
		{
			if (!Name(Value.TrackId) || !Name(Value.BindingGuid) || !Name(Value.ClassName)
				|| !Seal(Value.DetailFingerprint))
			{
				OutError = TEXT("track_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsSectionRecord& Value : Snapshot.Sections)
		{
			if (!Name(Value.SectionId) || !Name(Value.TrackId) || !Name(Value.ClassName)
				|| !Seal(Value.DetailFingerprint) || !Seal(Value.ChannelFingerprint))
			{
				OutError = TEXT("section_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsKeyRecord& Value : Snapshot.Keys)
		{
			if (!Name(Value.KeyId) || !Name(Value.SectionId) || !Name(Value.ChannelType)
				|| !Seal(Value.ValueFingerprint))
			{
				OutError = TEXT("key_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsCameraRecord& Value : Snapshot.Cameras)
		{
			if (!Name(Value.CameraCutId) || !Name(Value.SectionId)
				|| !Name(Value.CameraBindingId) || !Seal(Value.BindingFingerprint))
			{
				OutError = TEXT("camera_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsEventRecord& Value : Snapshot.Events)
		{
			if (!Name(Value.EventId) || !Name(Value.SectionId) || !Name(Value.EventKind)
				|| !Name(Value.CompiledFunctionName) || !Name(Value.BoundObjectClassName)
				|| !Seal(Value.DetailFingerprint))
			{
				OutError = TEXT("event_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsMRQSettingRecord& Value : Snapshot.RenderSettings)
		{
			if (!Name(Value.SettingId) || !Name(Value.ClassName)
				|| !Seal(Value.DetailFingerprint))
			{
				OutError = TEXT("mrq_setting_text_envelope_exceeded");
				return false;
			}
		}
		for (const FHyperAICinematicsIssue& Value : Snapshot.CaptureIssues)
		{
			if (!Name(Value.Code) || !Text(Value.Severity, 16) || !Name(Value.StableId)
				|| !Path(Value.Subject) || !Text(Value.Detail, MaxIssueDetailCharacters))
			{
				OutError = TEXT("capture_issue_text_envelope_exceeded");
				return false;
			}
		}
		return true;
	}
}

using namespace HyperAIStudio::Cinematics::Private;

FString FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioCinematics.HyperAIStudioCinematicsToolset");
}

const TArray<FHyperAIStudioCinematicsManifestEntry>&
FHyperAIStudioCinematicsContracts::GetManifest()
{
	static const TArray<FHyperAIStudioCinematicsManifestEntry> Manifest = {
		{TEXT("hyper_cinematics_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_cinematics_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_cinematics_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioCinematicsContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	TArray<FString> Names;
	for (const FHyperAIStudioCinematicsManifestEntry& Entry : GetManifest()) Names.Add(Entry.Name);
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

const TArray<FHyperAICinematicsDelegationAuthority>&
FHyperAIStudioCinematicsContracts::GetDelegationAuthority()
{
	static const TArray<FHyperAICinematicsDelegationAuthority> Matrix = {
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerConditionTools"), 9,
			TEXT("sha256:f178ed9f5124751af8ff4eb8353bab65f5c3c009845181a86fcb11d1370dc816"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerControlRigTools"), 72,
			TEXT("sha256:fd3520b8e8ce7b7dcf81fd4ec757da8fb21ab5c35295388e79673f10e8d50521"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerCustomBindingTools"), 8,
			TEXT("sha256:7f902c1205d67ccbf122fa366e1b377f28d05065ea3a6e1570352241574be6ae"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerImportExportTools"), 6,
			TEXT("sha256:31642ee2536690fcd933fbf77cd62b9581cb7875b80349d0a9ce9552f12563cd"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerKeyframingTools"), 22,
			TEXT("sha256:74fc571ed14c4f86f96b7227ef57eff78fce44c0b4e0d64089f067989c34b3af"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerOutlinerTools"), 18,
			TEXT("sha256:51366ff2db474133d8fa4ae0f62dff2f41930afb7d8b746c76ec07dd4fefb37d"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("AnimationAssistantToolset.SequencerTools"), 140,
			TEXT("sha256:75b33a624997e75e58c0374bc7093cff74351805dc1f8a383b84879f4d986c85"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("epic_python_group"), TEXT("SequencerAnimMixerToolset.SequencerAnimMixerTools"), 21,
			TEXT("sha256:75a43e108740754ddc6eda42fe90938d4bfd6386e2d28da543dda6802e19ab03"),
			TEXT("Plan/CapabilityUnion/epic_python_access_review.json"), TEXT("epic_delegate")},
		{TEXT("ue58_public_api"), TEXT("LevelSequence identity"), 0,
			TEXT("sha256:dfe3cdf6777029f40819fc70cb50fa9194d99c0429e5f4c3e34164c4ed37242b"),
			TEXT("Engine/Source/Runtime/LevelSequence/Public/LevelSequence.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene structure"), 0,
			TEXT("sha256:6d93fb85c27ee8c43acd93b521471994ffd9757e73192c60a818ab23e805fbb1"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/MovieScene.h"), TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene binding values"), 0,
			TEXT("sha256:7141189a358e2cdffe65530736940b55894ade7d727d65d3199111af65482b3a"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/MovieSceneBinding.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene track values"), 0,
			TEXT("sha256:8301d4407dcda5ec056d2329974a7f33ed69108decedc96f92783a8c04c6eb26"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/MovieSceneTrack.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene section values"), 0,
			TEXT("sha256:5e77ffeaf6b1842b9c13772c2cd25b0124fb27db5e449d5a48c2d8fd7c1d7553"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/MovieSceneSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene bounded channel keys"), 0,
			TEXT("sha256:2721c71f65049b7878942ecbbfbb6e75d24d527c8b4a3d592840bad1a06e6889"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/Channels/MovieSceneChannel.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene float key values"), 0,
			TEXT("sha256:b9e37befaca1319cd6402cd33bb632c941aefe09c27649b9d77141313d1e152b"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/Channels/MovieSceneFloatChannel.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene double key values"), 0,
			TEXT("sha256:63661f7af2fbf09b6c486bf7bb6448ce79818cbc5a697af579257b60d9acc051"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/Channels/MovieSceneDoubleChannel.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene transform sections"), 0,
			TEXT("sha256:78ce745336e4c96bbe4e11eca069554cf3ea7484b73ac10cfaae2d8b99f004c5"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieScene3DTransformSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene float sections"), 0,
			TEXT("sha256:7dcb03a930fe93fc1278204d73654b1944d297c2a494438eb937302ad9a0fc22"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieSceneFloatSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene double sections"), 0,
			TEXT("sha256:d682734a2f2a67739a12c476983f365c4721da06c72308d4f86f0c5a1ce20c68"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieSceneDoubleSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene camera cuts"), 0,
			TEXT("sha256:1e8a4612b7cf40512f97e18cdcc784ef41af116bceecc0cee34b7be139c6409d"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieSceneCameraCutSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene event channels"), 0,
			TEXT("sha256:9d2a305405dbf02f9539528b7c6e64b0e6ba3cba6aceb604f6a4802f2c5a49f5"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieSceneEventTriggerSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene repeater events"), 0,
			TEXT("sha256:545186da11de95c71ea58080867d6b903ce87ab9f14709023e7eb39f1112101a"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Sections/MovieSceneEventRepeaterSection.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene event payload values"), 0,
			TEXT("sha256:7e89b2bb2d649d141b0b48f25a136dad2cf042b078d326efe87b0baf7f496cfe"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Channels/MovieSceneEvent.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene camera-cut track semantics"), 0,
			TEXT("sha256:9f637d6dd258f7252b67f7ac068bb60b55ac2db08f39c0cbee23b4be04933766"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Tracks/MovieSceneCameraCutTrack.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene event-track semantics"), 0,
			TEXT("sha256:5c6ccceddbe23f6b6c5448d6dfce6a15af820f26849cb7fb1f68c7abc34c9e2d"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Tracks/MovieSceneEventTrack.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieRenderPipeline primary config"), 0,
			TEXT("sha256:01de0433d3de65f726666d26e3442d878082414ffd24ddcf925478443e78b7aa"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Public/MoviePipelinePrimaryConfig.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieRenderPipeline config inventory"), 0,
			TEXT("sha256:0eb29b04245e11dd5c2a1beb0c40d4a66f8ae47817399421a2f28db37b9c945d"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Public/MoviePipelineConfigBase.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieRenderPipeline output values"), 0,
			TEXT("sha256:f7f9990b89efdad2c2f6554807dd91fa82cf9116a35daff6a2f330984aca1fa1"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Public/MoviePipelineOutputSetting.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("Asset Registry nonblocking package evidence"), 0,
			TEXT("sha256:3ea171eaf6a071caf8e1a93393fbab4b3a643224cff70ce87628cb3841885e08"),
			TEXT("Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/IAssetRegistry.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene channel proxy inventory"), 0,
			TEXT("sha256:29b652eff2fd15e2e384ff6d7b22366e5e80d0bc85da1456bc1068a80f11bbc8"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/Channels/MovieSceneChannelProxy.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene event channel values"), 0,
			TEXT("sha256:518a331ece80938137051adfaa10f54c52f22e164edc7eb30858d15280c99a44"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Public/Channels/MovieSceneEventChannel.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieScene object binding identity"), 0,
			TEXT("sha256:fa6eceea55d3ebc6cc09d3fbd0bfc4c51bdce90e3cafb225f23efa3cc19ab638"),
			TEXT("Engine/Source/Runtime/MovieScene/Public/MovieSceneObjectBindingID.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_public_api"), TEXT("MovieRenderPipeline setting state"), 0,
			TEXT("sha256:de2a44e45df855a877dda12e6145dc185945378b4047b323d1f3960f51d346b6"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Public/MoviePipelineSetting.h"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_delegation_api"), TEXT("Sequencer editor authoring surface"), 0,
			TEXT("sha256:e4b1d58e2b42982bfdde2ca6a149182e8095e21e65c15e81a5cef1087b42ec51"),
			TEXT("Engine/Source/Editor/Sequencer/Public/ISequencer.h"), TEXT("epic_delegate")},
		{TEXT("ue58_delegation_implementation"), TEXT("Sequencer editor authoring implementation"), 0,
			TEXT("sha256:93dd2d16c9f40b9be09e8e2bd2977e23f2cceb79f5c384ce073b9c99a28e73bc"),
			TEXT("Engine/Source/Editor/Sequencer/Private/Sequencer.cpp"), TEXT("epic_delegate")},
		{TEXT("ue58_source_implementation"), TEXT("Asset Registry nonblocking package implementation"), 0,
			TEXT("sha256:0200285704dd44d2a43e4143d9cb78c5918c9a8f5381bc8dc946aca1534aeafe"),
			TEXT("Engine/Source/Runtime/AssetRegistry/Private/AssetRegistry.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("LevelSequence implementation"), 0,
			TEXT("sha256:f89c13741c20b0f029ee0d3b56395fb0441f776f321bf2819025c36a4e50242e"),
			TEXT("Engine/Source/Runtime/LevelSequence/Private/LevelSequence.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene structure implementation"), 0,
			TEXT("sha256:0c6423544b3e254c7ba5c4591353300baa2f2ba297b540d72d3009f5056e67b1"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/MovieScene.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene binding implementation"), 0,
			TEXT("sha256:385bad4613a45b388a0aed6947a0afecfbe4304e25286c91a833a74468c1e4f2"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/MovieSceneBinding.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene track implementation"), 0,
			TEXT("sha256:f013333828917417c7fd73bd43cf61143f0fba694819a701ccdbc1740317cd91"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/MovieSceneTrack.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene section implementation"), 0,
			TEXT("sha256:61f4ee75cb573706da22f8a708463d3bcdbe517f32feca12bca78d50ca58579e"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/MovieSceneSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene channel implementation"), 0,
			TEXT("sha256:d14a02a8d1cc8c91465bc7d4b43ddfdb727fefcc9ef21383dcaf97c50e8e40e5"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/Channels/MovieSceneChannel.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene channel proxy implementation"), 0,
			TEXT("sha256:f2799ebf1e10646b348f2173fd8ade42959198c91faaba4a9c4535d1edae695d"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/Channels/MovieSceneChannelProxy.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene float channel implementation"), 0,
			TEXT("sha256:aa74023d7a803081f2ac04c808b628cf5ada5e4c97c6ecf0da2f8470fa58918a"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/Channels/MovieSceneFloatChannel.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene double channel implementation"), 0,
			TEXT("sha256:76176f15e5603a3327b19d90b453a29cd5f925074b8f2d2c0bd46fe5176f92a9"),
			TEXT("Engine/Source/Runtime/MovieScene/Private/Channels/MovieSceneDoubleChannel.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene event value implementation"), 0,
			TEXT("sha256:ab299aa6d01b2d0d7d40c738657c37b91a63f108a7f77d82bbcefdb60c617966"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Channels/MovieSceneEvent.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene event channel implementation"), 0,
			TEXT("sha256:4df8d333b6910e6a2de35b23b644cda4d5dece2775e1dc3612dcce0548650332"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Channels/MovieSceneEventChannel.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene transform section implementation"), 0,
			TEXT("sha256:12282d248f6da03bc61289674507b0503a8694bd8da74b805e2d2110877d105c"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieScene3DTransformSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene camera section implementation"), 0,
			TEXT("sha256:312631815dd12593a41aa2de783d66f1cb80b71050fb6cab3a45c855f0a00e24"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneCameraCutSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene double section implementation"), 0,
			TEXT("sha256:44e887810e9f320db7523a6492cb35460df73cbddab028935b325f149c80ef7b"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneDoubleSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene repeater section implementation"), 0,
			TEXT("sha256:089ce9e2f57e3050b0702a044dff0213bf8ce0fdcbc20534f8648644da8c0944"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneEventRepeaterSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene trigger section implementation"), 0,
			TEXT("sha256:a8accc398dbc001ed12f015f055a4e6ce52a66f7f9579c5250552c2b692430d2"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneEventTriggerSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene float section implementation"), 0,
			TEXT("sha256:50079947459064d785e216ad8c633602da10aaee19ef5e6d5a1c2b63cb93d277"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Sections/MovieSceneFloatSection.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene camera track implementation"), 0,
			TEXT("sha256:53375eb5271f93d39715e55861a2e302ca766d10506f820337ea971ab81830e9"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Tracks/MovieSceneCameraCutTrack.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieScene event track implementation"), 0,
			TEXT("sha256:183811ba0b546d8049f75432631d24567341e73f834490fcc137e2a3052bcf0b"),
			TEXT("Engine/Source/Runtime/MovieSceneTracks/Private/Tracks/MovieSceneEventTrack.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieRenderPipeline config inventory implementation"), 0,
			TEXT("sha256:62d49b4e77d465818c665c490188fd9f6dff8fc3e90acfea268e8ad1302090c1"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Private/MoviePipelineConfigBase.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieRenderPipeline primary config implementation"), 0,
			TEXT("sha256:d3be3aa6f12b4a8d037502c8874ae52f88b6144ace8c0ee5bb77a4c49876092e"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Private/MoviePipelinePrimaryConfig.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieRenderPipeline output implementation"), 0,
			TEXT("sha256:207d2d6839a677f97309864370232b2b969f4596d48e134285dbb55509cf986d"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Private/MoviePipelineOutputSetting.cpp"),
			TEXT("native_observation_authority")},
		{TEXT("ue58_source_implementation"), TEXT("MovieRenderPipeline setting implementation"), 0,
			TEXT("sha256:d9358716b8e21ce952c41ea3e82553c688faa9f019cf8bc2e038765322116321"),
			TEXT("Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Private/MoviePipelineSetting.cpp"),
			TEXT("native_observation_authority")}};
	return Matrix;
}

TArray<FHyperAICinematicsCapabilityStatus>
FHyperAIStudioCinematicsContracts::GetCapabilityMatrix()
{
	FHyperAICinematicsCapabilityStatus Status;
	Status.SupportedCases = {
		TEXT("exact already-loaded LevelSequence and MovieScene value snapshot"),
		TEXT("bounded bindings, tracks, sections, ranges, fixed-channel keys, camera cuts, and events"),
		TEXT("already-loaded MoviePipelinePrimaryConfig setting inventory and exact OutputSetting projection"),
		TEXT("non-blocking package tri-state plus separate persisted and loaded volatile revisions"),
		TEXT("detached independent structural and render-ready validation"),
		TEXT("closed immutable render preflight payload and pure typed-artifact hashes")};
	Status.UnsupportedCases = {
		TEXT("asset loading, editor opening, broad discovery, dependency expansion, and custom channel proxy execution"),
		TEXT("complete CAS for spawnable templates, possessable dynamic bindings, conditions, custom easing, and event endpoint payloads"),
		TEXT("complete state CAS for custom MoviePipeline settings without sealed public projections"),
		TEXT("Sequencer authoring already covered by the reviewed Epic Sequencer tool families"),
		TEXT("AnimMixer authoring already covered by Epic SequencerAnimMixerTools"),
		TEXT("render submission, queue mutation, compile, save, polling, cancellation, and output-frame verification")};
	FString AuthorityCanonical;
	AppendToken(AuthorityCanonical, TEXT("hyperai.cinematics.delegation-authority.v1"));
	const TArray<FHyperAICinematicsDelegationAuthority>& Authority = GetDelegationAuthority();
	Status.DelegationAuthorityCount = Authority.Num();
	for (const FHyperAICinematicsDelegationAuthority& Row : Authority)
	{
		AppendToken(AuthorityCanonical, Row.AuthorityKind);
		AppendToken(AuthorityCanonical, Row.SourceGroup);
		AppendToken(AuthorityCanonical, FString::FromInt(Row.CallableCount));
		AppendToken(AuthorityCanonical, Row.ReviewedFingerprint);
		AppendToken(AuthorityCanonical, Row.SourceCoordinate);
		AppendToken(AuthorityCanonical, Row.Disposition);
		if (Row.AuthorityKind == TEXT("epic_python_group"))
		{
			Status.DelegatedEpicCallableCount += Row.CallableCount;
		}
	}
	Status.DelegationAuthorityFingerprint = HashCanonical(AuthorityCanonical);
	Status.Remediation = TEXT("Use Epic's reviewed Sequencer and AnimMixer callables for equivalent authoring. HyperAI render execution remains unavailable until the core continuation host can pin one begin call, poll/cancel without blocking, reconcile late completion, and verify exact output evidence.");
	return {MoveTemp(Status)};
}

bool FHyperAIStudioCinematicsContracts::IsCanonicalPrimaryAssetPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| HasControlCharacter(Path) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(".."))
		|| Path.Contains(TEXT(":")) || Path.EndsWith(TEXT(".")))
	{
		return false;
	}
	const int32 Dot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 Slash = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (Dot <= Slash + 1 || Dot >= Path.Len() - 1 || Path.Find(TEXT(".")) != Dot)
	{
		return false;
	}
	const FString PackageLeaf = Path.Mid(Slash + 1, Dot - Slash - 1);
	const FString ObjectName = Path.Mid(Dot + 1);
	return PackageLeaf == ObjectName && PackageLeaf.Len() <= MaxNameCharacters
		&& FPackageName::IsValidObjectPath(Path);
}

bool FHyperAIStudioCinematicsContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"))) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR C = Value[Index];
		if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
	}
	return true;
}

bool FHyperAIStudioCinematicsContracts::IsSafeOperationId(const FString& Value)
{
	return FHyperAIStudioExtensionRuntime::IsValidOperationId(Value);
}

FString FHyperAIStudioCinematicsContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists: return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist: return TEXT("does_not_exist");
	default: return TEXT("unknown");
	}
}

bool FHyperAIStudioCinematicsContracts::AdmitCountsBeforeProjection(
	const int32 Bindings, const int32 Tracks, const int32 Sections, const int32 Channels,
	const int32 Keys, const int32 Cameras, const int32 Events, const int32 MRQSettings,
	FString& OutError)
{
	OutError.Reset();
	if (Bindings < 0 || Bindings > MaxBindings) OutError = TEXT("binding_count_exceeded");
	else if (Tracks < 0 || Tracks > MaxTracks) OutError = TEXT("track_count_exceeded");
	else if (Sections < 0 || Sections > MaxSections) OutError = TEXT("section_count_exceeded");
	else if (Channels < 0 || Channels > MaxChannels) OutError = TEXT("channel_count_exceeded");
	else if (Keys < 0 || Keys > MaxKeys) OutError = TEXT("key_count_exceeded");
	else if (Cameras < 0 || Cameras > MaxCameraCuts) OutError = TEXT("camera_cut_count_exceeded");
	else if (Events < 0 || Events > MaxEvents) OutError = TEXT("event_count_exceeded");
	else if (MRQSettings < 0 || MRQSettings > MaxMRQSettings) OutError = TEXT("mrq_setting_count_exceeded");
	return OutError.IsEmpty();
}

FString FHyperAIStudioCinematicsContracts::InspectPayloadSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.inspect.v1|sequence_path:string<=512|render_config_path:string<=512|page_size:i32|cursor:string<=256|max_game_thread_ms:i32|max_output_bytes:i32"));
}

FString FHyperAIStudioCinematicsContracts::RenderPayloadSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.render_preflight.v1|sequence_path|render_config_path|persisted_revision|volatile_revision|config_persisted_revision|config_volatile_revision|output_setting_fingerprint|lifecycle|terminal_state|deadline|require_camera|require_output|verify_frames"));
}

FString FHyperAIStudioCinematicsContracts::ValidatePayloadSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.validate.v1|sequence_path|render_config_path|expected_persisted_revision|expected_volatile_revision|expected_config_persisted_revision|expected_config_volatile_revision|policy|max_issues|max_game_thread_ms|max_output_bytes"));
}

FString FHyperAIStudioCinematicsContracts::InspectResultSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.inspect.result.v1|status|separate_persisted_volatile_revisions|bindings|track_section_detail_seals|channel_key_value_seals|camera_binding_identity|partial_event_values|mrq_output_values|issues|compact_authority|cursor|truncation"));
}

FString FHyperAIStudioCinematicsContracts::RenderResultSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.render_preflight.result.v1|status|zero_effect|sequence_persisted_volatile|mrq_persisted_volatile|semantic_fingerprint|plan_hash|authorization_plan_hash|capability_hash|effect_fingerprint|future_backend_effects|issues|compact_authority"));
}

FString FHyperAIStudioCinematicsContracts::ValidateResultSchemaFingerprint()
{
	return HashCanonical(TEXT("cinematics.validate.result.v1|valid|complete|policy|sequence_persisted_volatile|mrq_persisted_volatile|validator_fingerprint|referential_counts|value_seals|issues|compact_authority|truncation"));
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioCinematicsContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.cinematics.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_cinematics_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_cinematics_apply_plan"), MutationVariantId,
			RenderPayloadTypeId, RenderPayloadSchemaFingerprint(), RenderResultTypeId,
			RenderResultSchemaFingerprint(), EHyperAIStudioDomainSafety::ExternalEffect});
		Value.Variants.Add({TEXT("hyper_cinematics_validate"), ValidateVariantId,
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

FString FHyperAIStudioCinematicsInspectPayload::GetTypeId() const
{
	return FHyperAIStudioCinematicsContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioCinematicsInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCinematicsContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioCinematicsInspectPayload::GetBoundedByteSize() const
{
	return BoundedRequestSize(
		{&Request.SequencePath, &Request.RenderConfigPath, &Request.Cursor}, 64);
}

FString FHyperAIStudioCinematicsValidatePayload::GetTypeId() const
{
	return FHyperAIStudioCinematicsContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioCinematicsValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCinematicsContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioCinematicsValidatePayload::GetBoundedByteSize() const
{
	return BoundedRequestSize({&Request.SequencePath, &Request.RenderConfigPath,
		&Request.ExpectedPersistedRevision, &Request.ExpectedVolatileRevision,
		&Request.ExpectedRenderConfigPersistedRevision,
		&Request.ExpectedRenderConfigVolatileRevision, &Request.Policy}, 96);
}

FString FHyperAIStudioCinematicsRenderPayload::GetTypeId() const
{
	return FHyperAIStudioCinematicsContracts::RenderPayloadTypeId;
}

FString FHyperAIStudioCinematicsRenderPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCinematicsContracts::RenderPayloadSchemaFingerprint();
}

int32 FHyperAIStudioCinematicsRenderPayload::GetBoundedByteSize() const
{
	return BoundedRequestSize({&SequencePath, &RenderConfigPath, &BasePersistedRevision,
		&BaseVolatileRevision, &BaseRenderConfigPersistedRevision,
		&BaseRenderConfigVolatileRevision, &OutputSettingFingerprint,
		&Lifecycle, &ExpectedTerminalState, &SemanticFingerprint}, 128);
}

FString FHyperAIStudioCinematicsRenderPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioCinematicsRenderPayload::CloneImmutable() const
{
	return MakeShared<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe>(*this);
}

FString FHyperAIStudioCinematicsInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioCinematicsContracts::InspectResultTypeId;
}

FString FHyperAIStudioCinematicsInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCinematicsContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioCinematicsInspectResultPayload::GetBoundedByteSize() const
{
	int32 Bytes = BaseResponseBudgetBytes;
	for (const FString* Field : {&Report.Status, &Report.Diagnostic,
		&Report.SnapshotFingerprint, &Report.NextCursor})
	{
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Field->Len()));
	}
	Bytes = SaturatingAdd(Bytes, EstimateSequenceBytes(Report.Sequence));
	Bytes = SaturatingAdd(Bytes, EstimateConfigBytes(Report.RenderConfig));
	Bytes = SaturatingAdd(Bytes, EstimateCapabilitiesBytes(Report.Capabilities));
	for (const FHyperAICinematicsBindingRecord& Value : Report.Bindings)
	{
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.BindingId, Value.BindingGuid, Value.Name));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(Value.DetailFingerprint, FString()));
	}
	for (const FHyperAICinematicsTrackRecord& Value : Report.Tracks)
	{
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.TrackId, Value.BindingGuid, Value.ClassName));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(Value.DetailFingerprint, FString()));
	}
	for (const FHyperAICinematicsSectionRecord& Value : Report.Sections)
	{
		Bytes = SaturatingAdd(Bytes, SaturatingAdd(512, EstimateRecordBytes(
			Value.SectionId, Value.TrackId, Value.ClassName)));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.DetailFingerprint, Value.ChannelFingerprint));
	}
	for (const FHyperAICinematicsKeyRecord& Value : Report.Keys)
	{
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.KeyId, Value.SectionId, Value.ChannelType));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(Value.ValueFingerprint, FString()));
	}
	for (const FHyperAICinematicsCameraRecord& Value : Report.Cameras)
	{
		Bytes = SaturatingAdd(Bytes, SaturatingAdd(384, EstimateRecordBytes(
			Value.CameraCutId, Value.SectionId, Value.CameraBindingId)));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(Value.BindingFingerprint, FString()));
	}
	for (const FHyperAICinematicsEventRecord& Value : Report.Events)
	{
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.EventId, Value.SectionId, Value.EventKind));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.CompiledFunctionName, Value.BoundObjectClassName));
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(Value.DetailFingerprint, FString()));
	}
	for (const FHyperAICinematicsMRQSettingRecord& Value : Report.RenderSettings)
		Bytes = SaturatingAdd(Bytes, EstimateRecordBytes(
			Value.SettingId, Value.ClassName, Value.DetailFingerprint));
	for (const FHyperAICinematicsIssue& Value : Report.Issues)
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Value));
	return Bytes;
}

FString FHyperAIStudioCinematicsValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioCinematicsContracts::ValidateResultTypeId;
}

FString FHyperAIStudioCinematicsValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioCinematicsContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioCinematicsValidateResultPayload::GetBoundedByteSize() const
{
	int32 Bytes = BaseResponseBudgetBytes;
	for (const FString* Field : {&Report.Status, &Report.Diagnostic, &Report.Policy,
		&Report.PersistedRevision, &Report.VolatileRevision,
		&Report.RenderConfigPersistedRevision, &Report.RenderConfigVolatileRevision,
		&Report.ValidatorFingerprint})
	{
		Bytes = SaturatingAdd(Bytes, SaturatingJsonBytesFromCharacters(Field->Len()));
	}
	Bytes = SaturatingAdd(Bytes, EstimateCapabilitiesBytes(Report.Capabilities));
	for (const FHyperAICinematicsIssue& Value : Report.Issues)
		Bytes = SaturatingAdd(Bytes, EstimateIssueBytes(Value));
	return Bytes;
}

FString FHyperAIStudioCinematicsContracts::BuildCursor(
	const FString& SequencePath, const FString& RenderConfigPath,
	const FString& SnapshotFingerprint, const int32 PageSize, const int32 Offset)
{
	if (!IsCanonicalPrimaryAssetPath(SequencePath)
		|| (!RenderConfigPath.IsEmpty() && !IsCanonicalPrimaryAssetPath(RenderConfigPath))
		|| !IsCanonicalSha256(SnapshotFingerprint)
		|| PageSize < 1 || PageSize > MaxPageSize
		|| Offset < 0 || Offset > MaxSnapshotItems)
	{
		return FString();
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("cinematics.cursor.v1"));
	AppendToken(Canonical, SequencePath);
	AppendToken(Canonical, RenderConfigPath);
	AppendToken(Canonical, SnapshotFingerprint);
	AppendToken(Canonical, FString::FromInt(PageSize));
	AppendToken(Canonical, FString::FromInt(Offset));
	const FString Seal = HashCanonical(Canonical);
	return Seal.IsEmpty() ? FString()
		: FString::Printf(TEXT("v1:%d:%d:%s"), PageSize, Offset, *Seal.Mid(7));
}

bool FHyperAIStudioCinematicsContracts::ParseCursor(
	const FString& Cursor, const FString& SequencePath, const FString& RenderConfigPath,
	const FString& SnapshotFingerprint, const int32 PageSize, int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.IsEmpty()) return true;
	if (Cursor.Len() > MaxCursorCharacters) return false;
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() != 4 || Parts[0] != TEXT("v1") || Parts[3].Len() != 64) return false;
	int32 CursorPageSize = 0;
	if (!LexTryParseString(CursorPageSize, *Parts[1]) || CursorPageSize != PageSize
		|| !LexTryParseString(OutOffset, *Parts[2])
		|| OutOffset < 0 || OutOffset > MaxSnapshotItems)
	{
		OutOffset = 0;
		return false;
	}
	const FString Expected = BuildCursor(SequencePath, RenderConfigPath,
		SnapshotFingerprint, PageSize, OutOffset);
	return Expected == Cursor;
}

FString FHyperAIStudioCinematicsContracts::ComputeRenderSemanticFingerprint(
	const FHyperAIStudioCinematicsRenderPayload& Payload)
{
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.cinematics.render-preflight.semantic.v1"));
	AppendToken(Canonical, Payload.SequencePath);
	AppendToken(Canonical, Payload.RenderConfigPath);
	AppendToken(Canonical, Payload.BasePersistedRevision);
	AppendToken(Canonical, Payload.BaseVolatileRevision);
	AppendToken(Canonical, Payload.BaseRenderConfigPersistedRevision);
	AppendToken(Canonical, Payload.BaseRenderConfigVolatileRevision);
	AppendToken(Canonical, Payload.OutputSettingFingerprint);
	AppendToken(Canonical, Payload.Lifecycle);
	AppendToken(Canonical, Payload.ExpectedTerminalState);
	AppendToken(Canonical, FString::FromInt(Payload.AsyncDeadlineMs));
	AppendToken(Canonical, Payload.bRequireCameraCut ? TEXT("require_camera") : TEXT("camera_optional"));
	AppendToken(Canonical, Payload.bRequireOutputSetting ? TEXT("require_output") : TEXT("output_optional"));
	AppendToken(Canonical, Payload.bVerifyOutputFrames ? TEXT("verify_frames") : TEXT("no_frame_verify"));
	return HashCanonical(Canonical);
}

FString FHyperAIStudioCinematicsContracts::ComputeSnapshotFingerprint(
	const FHyperAIStudioCinematicsValueSnapshot& Snapshot)
{
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.cinematics.snapshot.v1"));
	AppendToken(Canonical, Snapshot.Sequence.TargetPath);
	AppendToken(Canonical, Snapshot.Sequence.PersistedRevision);
	AppendToken(Canonical, Snapshot.Sequence.VolatileRevision);
	AppendToken(Canonical, Snapshot.RenderConfig.TargetPath);
	AppendToken(Canonical, Snapshot.RenderConfig.PersistedRevision);
	AppendToken(Canonical, Snapshot.RenderConfig.VolatileRevision);
	AppendToken(Canonical, Snapshot.bComplete ? TEXT("complete") : TEXT("partial"));
	return HashCanonical(Canonical);
}

FString FHyperAIStudioCinematicsContracts::ComputePersistedRevisionForEvidence(
	const FString& PackageName,
	const FString& DiskExistence,
	const FString& PackageSavedHash,
	const int64 DiskSize)
{
	return RecomputePersistedRevision(
		PackageName, DiskExistence, PackageSavedHash, DiskSize);
}

FString FHyperAIStudioCinematicsContracts::ComputeSequenceVolatileRevisionForValues(
	const FHyperAIStudioCinematicsValueSnapshot& Snapshot)
{
	return ComputeSequenceVolatileRevision(Snapshot);
}

FString FHyperAIStudioCinematicsContracts::ComputeRenderConfigVolatileRevisionForValues(
	const FHyperAIStudioCinematicsValueSnapshot& Snapshot)
{
	return ComputeMRQVolatileRevision(Snapshot);
}

bool FHyperAIStudioCinematicsContracts::CaptureExact(
	const FString& SequencePath,
	const FString& RenderConfigPath,
	const int32 MaxWorkMs,
	FHyperAIStudioCinematicsValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic)
{
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded UObject cinematics capture is serialized on the game thread.");
		return false;
	}
	if (!IsCanonicalPrimaryAssetPath(SequencePath))
	{
		OutStatus = TEXT("invalid_sequence_path");
		OutDiagnostic = TEXT("SequencePath must be one exact canonical top-level /Game primary-object path.");
		return false;
	}
	if (!RenderConfigPath.IsEmpty() && !IsCanonicalPrimaryAssetPath(RenderConfigPath))
	{
		OutStatus = TEXT("invalid_render_config_path");
		OutDiagnostic = TEXT("RenderConfigPath must be empty or one exact canonical top-level /Game primary-object path.");
		return false;
	}

	FCaptureContext Context(MaxWorkMs);
	ULevelSequence* Sequence = Cast<ULevelSequence>(FSoftObjectPath(SequencePath).ResolveObject());
	if (!Sequence || Sequence->GetClass() != ULevelSequence::StaticClass())
	{
		OutStatus = TEXT("loaded_sequence_not_found");
		OutDiagnostic = TEXT("The exact LevelSequence is not already loaded as the sealed UE 5.8 class; no load or editor open was attempted.");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene || MovieScene->GetClass() != UMovieScene::StaticClass())
	{
		OutStatus = TEXT("loaded_movie_scene_unavailable");
		OutDiagnostic = TEXT("The loaded LevelSequence has no exact sealed MovieScene value object.");
		return false;
	}
	if (!Context.CheckDeadline(TEXT("sequence"), SequencePath))
	{
		OutStatus = TEXT("capture_deadline_exceeded");
		OutDiagnostic = TEXT("The capture deadline expired before structural preflight.");
		return false;
	}

	FHyperAICinematicsSequenceRecord& SequenceRecord = OutSnapshot.Sequence;
	SequenceRecord.TargetPath = SequencePath;
	SequenceRecord.bLoaded = true;
	if (!MaterializeNameBounded(Sequence->GetClass()->GetFName(), MaxNameCharacters,
		SequenceRecord.ClassName))
	{
		Context.MarkIncomplete(TEXT("sequence_class_name_unbounded"), TEXT("sequence"),
			SequencePath, TEXT("The loaded sequence class name exceeded the output contract."));
	}
	FPackageEvidence SequencePackage;
	CapturePackageEvidence(Sequence, Context, TEXT("sequence_package"), SequencePackage);
	SequenceRecord.PackageName = SequencePackage.PackageName;
	SequenceRecord.DiskExistence = SequencePackage.DiskExistence;
	SequenceRecord.PackageSavedHash = SequencePackage.PackageSavedHash;
	SequenceRecord.DiskSize = SequencePackage.DiskSize;
	SequenceRecord.bWasLoadedFromDisk = SequencePackage.bWasLoadedFromDisk;
	SequenceRecord.bPackageDirty = SequencePackage.bPackageDirty;
	SequenceRecord.PersistedRevision = SequencePackage.PersistedRevision;
	SequenceRecord.bPersistedRevisionComplete = SequencePackage.bPersistedRevisionComplete;

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	SequenceRecord.TickResolutionNumerator = TickResolution.Numerator;
	SequenceRecord.TickResolutionDenominator = TickResolution.Denominator;
	SequenceRecord.DisplayRateNumerator = DisplayRate.Numerator;
	SequenceRecord.DisplayRateDenominator = DisplayRate.Denominator;
	SequenceRecord.PlaybackRange = ToFrameRange(MovieScene->GetPlaybackRange());

	const TArray<FMovieSceneBinding>& BindingValues =
		static_cast<const UMovieScene*>(MovieScene)->GetBindings();
	const TArray<UMovieSceneTrack*>& RootTracks =
		static_cast<const UMovieScene*>(MovieScene)->GetTracks();
	if (BindingValues.Num() > MaxBindings || RootTracks.Num() > MaxTracks)
	{
		OutStatus = TEXT("structural_count_exceeded");
		OutDiagnostic = TEXT("Binding or root-track count exceeded its pre-copy hard bound.");
		return false;
	}
	int64 RawTrackReferences = RootTracks.Num();
	for (const FMovieSceneBinding& Binding : BindingValues)
	{
		RawTrackReferences += Binding.GetTracks().Num();
		if (RawTrackReferences > MaxTracks * 2LL)
		{
			OutStatus = TEXT("track_reference_count_exceeded");
			OutDiagnostic = TEXT("Track references exceeded the hard bound before pointer inventory allocation.");
			return false;
		}
	}
	UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack();
	if (CameraCutTrack) ++RawTrackReferences;
	if (RawTrackReferences > MaxTracks * 2LL)
	{
		OutStatus = TEXT("track_reference_count_exceeded");
		OutDiagnostic = TEXT("Camera and binding track references exceeded the hard bound.");
		return false;
	}

	struct FTrackWork
	{
		UMovieSceneTrack* Track = nullptr;
		FString BindingGuid;
		bool bRoot = false;
		bool bCameraCut = false;
		FString TrackId;
	};
	TArray<FTrackWork> TrackWork;
	TrackWork.Reserve(static_cast<int32>(RawTrackReferences));
	TSet<UMovieSceneTrack*> SeenTracks;
	SeenTracks.Reserve(static_cast<int32>(RawTrackReferences));
	auto AddTrack = [&](UMovieSceneTrack* Track, const FString& BindingGuid,
		const bool bRoot, const bool bCamera)
	{
		if (!Track)
		{
			Context.MarkIncomplete(TEXT("null_track_reference"), TEXT("track"), SequencePath,
				TEXT("MovieScene contained a null track reference."));
			return;
		}
		if (SeenTracks.Contains(Track))
		{
			for (FTrackWork& Existing : TrackWork)
			{
				if (Existing.Track == Track)
				{
					Existing.bCameraCut |= bCamera;
					return;
				}
			}
			return;
		}
		SeenTracks.Add(Track);
		FTrackWork& Work = TrackWork.AddDefaulted_GetRef();
		Work.Track = Track;
		Work.BindingGuid = BindingGuid;
		Work.bRoot = bRoot;
		Work.bCameraCut = bCamera;
	};
	for (UMovieSceneTrack* Track : RootTracks) AddTrack(Track, FString(), true, false);
	if (CameraCutTrack) AddTrack(CameraCutTrack, FString(), true, true);
	for (const FMovieSceneBinding& Binding : BindingValues)
	{
		const FString Guid = Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		for (UMovieSceneTrack* Track : Binding.GetTracks()) AddTrack(Track, Guid, false, false);
	}
	if (TrackWork.Num() > MaxTracks)
	{
		OutStatus = TEXT("unique_track_count_exceeded");
		OutDiagnostic = TEXT("Unique MovieScene tracks exceeded the hard bound.");
		return false;
	}
	for (int32 TrackIndex = 0; TrackIndex < TrackWork.Num(); ++TrackIndex)
	{
		TrackWork[TrackIndex].TrackId = FString::Printf(TEXT("track:%04d"), TrackIndex);
	}

	int64 RawSections = 0;
	for (const FTrackWork& Work : TrackWork)
	{
		RawSections += Work.Track->GetAllSections().Num();
		if (RawSections > MaxSections)
		{
			OutStatus = TEXT("section_count_exceeded");
			OutDiagnostic = TEXT("Section count exceeded its hard bound before section inventory allocation.");
			return false;
		}
	}
	struct FSectionWork
	{
		UMovieSceneSection* Section = nullptr;
		int32 TrackIndex = INDEX_NONE;
		int32 LocalIndex = INDEX_NONE;
		FString SectionId;
		bool bFixedChannels = false;
		int32 ChannelCount = 0;
		int32 KeyCount = 0;
	};
	TArray<FSectionWork> SectionWork;
	SectionWork.Reserve(static_cast<int32>(RawSections));
	for (int32 TrackIndex = 0; TrackIndex < TrackWork.Num(); ++TrackIndex)
	{
		const TArray<UMovieSceneSection*>& Sections = TrackWork[TrackIndex].Track->GetAllSections();
		for (int32 LocalIndex = 0; LocalIndex < Sections.Num(); ++LocalIndex)
		{
			UMovieSceneSection* Section = Sections[LocalIndex];
			if (!Section)
			{
				OutStatus = TEXT("null_section_reference");
				OutDiagnostic = TEXT("A bounded track contained a null section reference.");
				return false;
			}
			FSectionWork& Work = SectionWork.AddDefaulted_GetRef();
			Work.Section = Section;
			Work.TrackIndex = TrackIndex;
			Work.LocalIndex = LocalIndex;
			Work.SectionId = FString::Printf(TEXT("section:%04d"), SectionWork.Num() - 1);
			Work.bFixedChannels = IsHardBoundedChannelSection(Section);
		}
	}

	UMoviePipelinePrimaryConfig* RenderConfig = nullptr;
	const FArrayProperty* SettingsProperty = nullptr;
	const FObjectPropertyBase* SettingsInner = nullptr;
	const FObjectPropertyBase* OutputSettingProperty = nullptr;
	UMoviePipelineOutputSetting* PrimaryOutputSetting = nullptr;
	TUniquePtr<FScriptArrayHelper> SettingsHelper;
	int32 UserSettingCount = 0;
	int32 MRQSettingCount = 0;
	if (!RenderConfigPath.IsEmpty())
	{
		RenderConfig = Cast<UMoviePipelinePrimaryConfig>(
			FSoftObjectPath(RenderConfigPath).ResolveObject());
		if (!RenderConfig || RenderConfig->GetClass() != UMoviePipelinePrimaryConfig::StaticClass())
		{
			OutStatus = TEXT("loaded_render_config_not_found");
			OutDiagnostic = TEXT("The exact MoviePipelinePrimaryConfig is not already loaded; no load or editor open was attempted.");
			return false;
		}
		SettingsProperty = FindFProperty<FArrayProperty>(
			RenderConfig->GetClass(), TEXT("Settings"));
		SettingsInner = SettingsProperty
			? CastField<FObjectPropertyBase>(SettingsProperty->Inner) : nullptr;
		OutputSettingProperty = FindFProperty<FObjectPropertyBase>(
			RenderConfig->GetClass(), TEXT("OutputSetting"));
		if (!SettingsProperty || !SettingsInner || !OutputSettingProperty
			|| OutputSettingProperty->PropertyClass
				!= UMoviePipelineOutputSetting::StaticClass())
		{
			OutStatus = TEXT("mrq_settings_contract_unavailable");
			OutDiagnostic = TEXT("UE 5.8 MoviePipelineConfigBase.Settings or PrimaryConfig.OutputSetting no longer matches the reviewed reflected contract.");
			return false;
		}
		const void* SettingsValue = SettingsProperty->ContainerPtrToValuePtr<void>(RenderConfig);
		SettingsHelper = MakeUnique<FScriptArrayHelper>(SettingsProperty, SettingsValue);
		UserSettingCount = SettingsHelper->Num();
		PrimaryOutputSetting = Cast<UMoviePipelineOutputSetting>(
			OutputSettingProperty->GetObjectPropertyValue_InContainer(RenderConfig));
		if (!PrimaryOutputSetting
			|| PrimaryOutputSetting->GetClass()
				!= UMoviePipelineOutputSetting::StaticClass())
		{
			OutStatus = TEXT("mrq_output_setting_contract_unavailable");
			OutDiagnostic = TEXT("The loaded primary config has no exact reviewed OutputSetting subobject.");
			return false;
		}
		if (UserSettingCount < 0 || UserSettingCount >= MaxMRQSettings)
		{
			OutStatus = TEXT("mrq_setting_count_exceeded");
			OutDiagnostic = TEXT("Movie Render Pipeline setting count exceeded its pre-copy hard bound.");
			return false;
		}
		MRQSettingCount = UserSettingCount + 1;
	}

	int64 ChannelCount64 = 0;
	int64 KeyCount64 = 0;
	int64 CameraCount64 = 0;
	int64 EventCount64 = 0;
	for (FSectionWork& Work : SectionWork)
	{
		if (!Context.CheckDeadline(Work.SectionId, SequencePath))
		{
			OutStatus = TEXT("capture_deadline_exceeded");
			OutDiagnostic = TEXT("The capture deadline expired during channel-count preflight.");
			return false;
		}
		if (Work.Section->GetClass() == UMovieSceneCameraCutSection::StaticClass())
		{
			++CameraCount64;
		}
		if (Work.Section->GetClass() == UMovieSceneEventTriggerSection::StaticClass())
		{
			const UMovieSceneEventTriggerSection* EventSection =
				CastChecked<UMovieSceneEventTriggerSection>(Work.Section);
			const TMovieSceneChannelData<const FMovieSceneEvent> Data =
				EventSection->EventChannel.GetData();
			if (Data.GetTimes().Num() != Data.GetValues().Num())
			{
				OutStatus = TEXT("event_channel_not_closed");
				OutDiagnostic = TEXT("Event key times and values have different cardinality.");
				return false;
			}
			Work.ChannelCount = 1;
			Work.KeyCount = Data.GetTimes().Num();
			ChannelCount64 += 1;
			KeyCount64 += Work.KeyCount;
			EventCount64 += Work.KeyCount;
			if (Work.KeyCount > MaxEvents || Work.KeyCount > MaxKeys)
			{
				OutStatus = TEXT("event_count_exceeded");
				OutDiagnostic = TEXT("An event trigger channel exceeded its pre-copy event/key bound.");
				return false;
			}
#if WITH_EDITORONLY_DATA
			for (int32 EventIndex = 0; EventIndex < Data.GetValues().Num(); ++EventIndex)
			{
				if ((EventIndex & 63) == 0
					&& !Context.CheckDeadline(Work.SectionId, SequencePath))
				{
					OutStatus = TEXT("capture_deadline_exceeded");
					OutDiagnostic = TEXT("The capture deadline expired during event payload-count preflight.");
					return false;
				}
				if (Data.GetValues()[EventIndex].PayloadVariables.Num()
					> MaxEventPayloadVariables)
				{
					OutStatus = TEXT("event_payload_count_exceeded");
					OutDiagnostic = TEXT("An event payload map exceeded its pre-copy hard bound.");
					return false;
				}
			}
#endif
		}
		else if (Work.Section->GetClass() == UMovieSceneEventRepeaterSection::StaticClass())
		{
			EventCount64 += 1;
#if WITH_EDITORONLY_DATA
			const UMovieSceneEventRepeaterSection* EventSection =
				CastChecked<UMovieSceneEventRepeaterSection>(Work.Section);
			if (EventSection->Event.PayloadVariables.Num() > MaxEventPayloadVariables)
			{
				OutStatus = TEXT("event_payload_count_exceeded");
				OutDiagnostic = TEXT("A repeater event payload map exceeded its pre-copy hard bound.");
				return false;
			}
#endif
		}
		else if (Work.bFixedChannels)
		{
			FMovieSceneChannelProxy& Proxy = Work.Section->GetChannelProxy();
			const int32 NumChannels = Proxy.NumChannels();
			if (NumChannels < 0 || NumChannels > MaxFixedChannelsPerSection)
			{
				OutStatus = TEXT("fixed_channel_contract_exceeded");
				OutDiagnostic = TEXT("A sealed fixed-channel section exceeded its reviewed per-section channel cap.");
				return false;
			}
			Work.ChannelCount = NumChannels;
			ChannelCount64 += NumChannels;
			for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
			{
				for (FMovieSceneChannel* Channel : Entry.GetChannels())
				{
					if (!Channel)
					{
						OutStatus = TEXT("null_movie_scene_channel");
						OutDiagnostic = TEXT("A sealed channel proxy contained a null channel.");
						return false;
					}
					const int32 NumKeys = Channel->GetNumKeys();
					if (NumKeys < 0 || NumKeys > MaxKeys)
					{
						OutStatus = TEXT("channel_key_count_exceeded");
						OutDiagnostic = TEXT("A channel key count exceeded its pre-copy hard bound.");
						return false;
					}
					Work.KeyCount += NumKeys;
					KeyCount64 += NumKeys;
				}
			}
		}
		else
		{
			Context.MarkIncomplete(TEXT("section_channel_projection_unsupported"),
				Work.SectionId, SequencePath,
				TEXT("This exact section class has no reviewed fixed-allocation channel proxy contract; keys were not touched."));
		}
		if (ChannelCount64 > MaxChannels || KeyCount64 > MaxKeys
			|| CameraCount64 > MaxCameraCuts || EventCount64 > MaxEvents)
		{
			OutStatus = TEXT("cinematics_value_count_exceeded");
			OutDiagnostic = TEXT("Channels, keys, camera cuts, or events exceeded a hard bound during preflight.");
			return false;
		}
	}
	FString CountError;
	if (!AdmitCountsBeforeProjection(BindingValues.Num(), TrackWork.Num(), SectionWork.Num(),
		static_cast<int32>(ChannelCount64), static_cast<int32>(KeyCount64),
		static_cast<int32>(CameraCount64), static_cast<int32>(EventCount64),
		MRQSettingCount, CountError))
	{
		OutStatus = CountError;
		OutDiagnostic = TEXT("Cinematics aggregate counts failed the pre-copy hard-bound admission check.");
		return false;
	}

	OutSnapshot.Bindings.Reserve(BindingValues.Num());
	for (int32 BindingIndex = 0; BindingIndex < BindingValues.Num(); ++BindingIndex)
	{
		if (!Context.CheckDeadline(TEXT("binding"), SequencePath)) break;
		const FMovieSceneBinding& Binding = BindingValues[BindingIndex];
		FHyperAICinematicsBindingRecord& Record = OutSnapshot.Bindings.AddDefaulted_GetRef();
		Record.BindingId = FString::Printf(TEXT("binding:%04d"), BindingIndex);
		Record.BindingGuid = Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		const FString* BindingName = nullptr;
		if (const FMovieSceneSpawnable* Spawnable =
			MovieScene->FindSpawnable(Binding.GetObjectGuid()))
		{
			Record.Kind = TEXT("spawnable");
			BindingName = &Spawnable->GetName();
		}
		else if (const FMovieScenePossessable* Possessable =
			MovieScene->FindPossessable(Binding.GetObjectGuid()))
		{
			Record.Kind = TEXT("possessable");
			BindingName = &Possessable->GetName();
		}
		else
		{
			Record.Kind = TEXT("unknown");
		}
		if (Record.Kind == TEXT("unknown"))
		{
			Context.MarkIncomplete(TEXT("binding_kind_unknown"), Record.BindingId,
				Record.BindingGuid, TEXT("Binding is neither a current spawnable nor possessable."));
		}
		else if (BindingName->Len() <= MaxNameCharacters
			&& !HasControlCharacter(*BindingName))
		{
			Record.Name = *BindingName;
		}
		else
		{
			Context.MarkIncomplete(TEXT("binding_name_unbounded"), Record.BindingId,
				Record.BindingGuid, TEXT("Binding name exceeded the output contract."));
		}
		Record.TrackCount = Binding.GetTracks().Num();
		FString BindingDetail;
		AppendToken(BindingDetail, TEXT("hyperai.cinematics.binding-detail.v1"));
		AppendToken(BindingDetail, Record.BindingGuid);
		AppendToken(BindingDetail, Record.Name);
		AppendToken(BindingDetail, Record.Kind);
		AppendToken(BindingDetail, FString::FromInt(Record.TrackCount));
		Record.DetailFingerprint = HashCanonical(BindingDetail);
		Record.bDetailProjectionComplete = false;
		Context.MarkIncomplete(TEXT("binding_object_projection_limited"),
			Record.BindingId, Record.BindingGuid,
			TEXT("Spawnable templates, possessable class/dynamic-binding state, and decorations are outside the bounded v1 value projection."));
	}

	OutSnapshot.Tracks.Reserve(TrackWork.Num());
	for (const FTrackWork& Work : TrackWork)
	{
		if (!Context.CheckDeadline(Work.TrackId, SequencePath)) break;
		FHyperAICinematicsTrackRecord& Record = OutSnapshot.Tracks.AddDefaulted_GetRef();
		Record.TrackId = Work.TrackId;
		Record.BindingGuid = Work.BindingGuid;
		if (!MaterializeNameBounded(Work.Track->GetClass()->GetFName(), MaxNameCharacters,
			Record.ClassName))
		{
			Context.MarkIncomplete(TEXT("track_class_name_unbounded"), Record.TrackId,
				SequencePath, TEXT("Track class name exceeded the output contract."));
		}
		Record.bRootTrack = Work.bRoot;
		Record.bCameraCutTrack = Work.bCameraCut;
		Record.bEventTrack = Work.Track->GetClass() == UMovieSceneEventTrack::StaticClass();
		Record.SectionCount = Work.Track->GetAllSections().Num();
		FString TrackDetail;
		AppendToken(TrackDetail, TEXT("hyperai.cinematics.track-detail.v1"));
		AppendToken(TrackDetail, Work.Track->EvalOptions.bCanEvaluateNearestSection
			? TEXT("can_nearest") : TEXT("cannot_nearest"));
		AppendToken(TrackDetail, Work.Track->EvalOptions.bEvalNearestSection
			? TEXT("eval_nearest") : TEXT("do_not_eval_nearest"));
		AppendToken(TrackDetail, Work.Track->EvalOptions.bEvaluateInPreroll
			? TEXT("eval_preroll") : TEXT("no_preroll"));
		AppendToken(TrackDetail, Work.Track->EvalOptions.bEvaluateInPostroll
			? TEXT("eval_postroll") : TEXT("no_postroll"));
		AppendToken(TrackDetail, Work.Track->bAutoExpandToKeyframes
			? TEXT("auto_expand") : TEXT("no_auto_expand"));
		bool bTrackDetailComplete = true;
		if (Work.Track->ConditionContainer.Condition)
		{
			bTrackDetailComplete = false;
			Context.MarkIncomplete(TEXT("track_condition_projection_limited"),
				Record.TrackId, Record.ClassName,
				TEXT("Dynamic track-condition UObject state is outside the bounded v1 projection."));
		}
		if (Work.Track->ParentTrack)
		{
			const FTrackWork* ParentWork = TrackWork.FindByPredicate(
				[&](const FTrackWork& Candidate)
				{
					return Candidate.Track == Work.Track->ParentTrack;
				});
			if (!ParentWork)
			{
				bTrackDetailComplete = false;
				Context.MarkIncomplete(TEXT("track_parent_outside_projection"),
					Record.TrackId, Record.ClassName,
					TEXT("Parent track is outside the exact bounded track inventory."));
			}
			else
			{
				AppendToken(TrackDetail, ParentWork->TrackId);
			}
		}
		else
		{
			AppendToken(TrackDetail, TEXT("no_parent"));
		}
		if (Work.Track->GetClass() == UMovieSceneCameraCutTrack::StaticClass())
		{
			const UMovieSceneCameraCutTrack* CameraTrack =
				CastChecked<UMovieSceneCameraCutTrack>(Work.Track);
			AppendToken(TrackDetail, CameraTrack->bCanBlend
				? TEXT("camera_blend") : TEXT("camera_cut_only"));
			AppendToken(TrackDetail, CameraTrack->IsAutoManagingSections()
				? TEXT("auto_manage") : TEXT("manual_manage"));
		}
		else if (Work.Track->GetClass() == UMovieSceneEventTrack::StaticClass())
		{
			const UMovieSceneEventTrack* EventTrack =
				CastChecked<UMovieSceneEventTrack>(Work.Track);
			AppendToken(TrackDetail, EventTrack->bFireEventsWhenForwards
				? TEXT("fire_forward") : TEXT("no_forward"));
			AppendToken(TrackDetail, EventTrack->bFireEventsWhenBackwards
				? TEXT("fire_backward") : TEXT("no_backward"));
			AppendToken(TrackDetail,
				FString::FromInt(static_cast<int32>(EventTrack->EventPosition)));
		}
		else
		{
			bTrackDetailComplete = false;
			Context.MarkIncomplete(TEXT("track_detail_projection_unsupported"),
				Record.TrackId, Record.ClassName,
				TEXT("This exact derived track class has no reviewed v1 semantic projection."));
		}
		Record.DetailFingerprint = HashCanonical(TrackDetail);
		Record.bDetailProjectionComplete = bTrackDetailComplete
			&& IsCanonicalSha256(Record.DetailFingerprint);
	}

	OutSnapshot.Sections.Reserve(SectionWork.Num());
	OutSnapshot.Keys.Reserve(static_cast<int32>(KeyCount64));
	OutSnapshot.Cameras.Reserve(static_cast<int32>(CameraCount64));
	OutSnapshot.Events.Reserve(static_cast<int32>(EventCount64));
	int32 GlobalKeyIndex = 0;
	int32 GlobalCameraIndex = 0;
	int32 GlobalEventIndex = 0;
	for (const FSectionWork& Work : SectionWork)
	{
		if (!Context.CheckDeadline(Work.SectionId, SequencePath)) break;
		FHyperAICinematicsSectionRecord& Record = OutSnapshot.Sections.AddDefaulted_GetRef();
		Record.SectionId = Work.SectionId;
		Record.TrackId = TrackWork[Work.TrackIndex].TrackId;
		if (!MaterializeNameBounded(Work.Section->GetClass()->GetFName(), MaxNameCharacters,
			Record.ClassName))
		{
			Context.MarkIncomplete(TEXT("section_class_name_unbounded"), Record.SectionId,
				SequencePath, TEXT("Section class name exceeded the output contract."));
		}
		Record.Range = ToFrameRange(Work.Section->GetRange());
		Record.RowIndex = Work.Section->GetRowIndex();
		Record.OverlapPriority = Work.Section->GetOverlapPriority();
		Record.PreRollFrames = Work.Section->GetPreRollFrames();
		Record.PostRollFrames = Work.Section->GetPostRollFrames();
		Record.bActive = Work.Section->IsActive();
		Record.bLocked = Work.Section->IsLocked();
		Record.bDetailProjectionComplete = BuildBaseSectionDetail(
			Work.Section, Context, Record.SectionId, Record.DetailFingerprint);
		Record.bChannelProjectionSupported = Work.bFixedChannels
			|| Work.Section->GetClass() == UMovieSceneCameraCutSection::StaticClass();
		Record.ChannelCount = Work.ChannelCount;
		Record.KeyCount = Work.KeyCount;
		FString DerivedDetail;
		AppendToken(DerivedDetail, TEXT("hyperai.cinematics.derived-section-detail.v1"));
		AppendToken(DerivedDetail, Record.DetailFingerprint);
		AppendToken(DerivedDetail, Record.ClassName);
		if (Work.Section->GetClass() == UMovieScene3DTransformSection::StaticClass())
		{
			const UMovieScene3DTransformSection* Transform =
				CastChecked<UMovieScene3DTransformSection>(Work.Section);
			AppendToken(DerivedDetail, FString::Printf(TEXT("%u"),
				static_cast<uint32>(Transform->GetMask().GetChannels())));
			AppendToken(DerivedDetail, Transform->GetUseQuaternionInterpolation()
				? TEXT("quaternion") : TEXT("euler"));
			const int32 ConstraintCount = Transform->GetConstraintsChannels().Num();
			AppendToken(DerivedDetail, FString::FromInt(ConstraintCount));
			if (ConstraintCount != 0)
			{
				Record.bDetailProjectionComplete = false;
				Context.MarkIncomplete(TEXT("transform_constraint_projection_limited"),
					Record.SectionId, Record.ClassName,
					TEXT("Transform constraint channel values are outside the bounded v1 projection."));
			}
		}
		else if (Work.Section->GetClass() != UMovieSceneFloatSection::StaticClass()
			&& Work.Section->GetClass() != UMovieSceneDoubleSection::StaticClass()
			&& Work.Section->GetClass() != UMovieSceneCameraCutSection::StaticClass()
			&& Work.Section->GetClass() != UMovieSceneEventTriggerSection::StaticClass()
			&& Work.Section->GetClass() != UMovieSceneEventRepeaterSection::StaticClass())
		{
			Record.bDetailProjectionComplete = false;
			Context.MarkIncomplete(TEXT("section_detail_projection_unsupported"),
				Record.SectionId, Record.ClassName,
				TEXT("This exact derived section class has no reviewed v1 semantic projection."));
		}
		Record.DetailFingerprint = HashCanonical(DerivedDetail);
		Record.bDetailProjectionComplete = Record.bDetailProjectionComplete
			&& IsCanonicalSha256(Record.DetailFingerprint);

		if (Work.Section->GetClass() == UMovieSceneCameraCutSection::StaticClass())
		{
			const UMovieSceneCameraCutSection* CameraSection =
				CastChecked<UMovieSceneCameraCutSection>(Work.Section);
			FHyperAICinematicsCameraRecord& Camera = OutSnapshot.Cameras.AddDefaulted_GetRef();
			Camera.CameraCutId = FString::Printf(TEXT("camera:%04d"), GlobalCameraIndex++);
			Camera.SectionId = Record.SectionId;
			const FMovieSceneObjectBindingID& CameraBinding =
				CameraSection->GetCameraBindingID();
			Camera.CameraBindingId = CameraBinding.GetGuid().ToString(
				EGuidFormats::DigitsWithHyphensLower);
			Camera.CameraBindingSequenceId =
				static_cast<int32>(
					CameraBinding.GetRelativeSequenceID().GetInternalValue());
			const FIntProperty* ResolveParentProperty = FindFProperty<FIntProperty>(
				FMovieSceneObjectBindingID::StaticStruct(), TEXT("ResolveParentIndex"));
			if (!ResolveParentProperty)
			{
				Record.bDetailProjectionComplete = false;
				Record.bChannelProjectionSupported = false;
				Context.MarkIncomplete(TEXT("camera_binding_contract_unavailable"),
					Camera.CameraCutId, Record.SectionId,
					TEXT("UE 5.8 camera binding parent-index reflection no longer matches the reviewed contract."));
			}
			else
			{
				Camera.CameraBindingResolveParentIndex =
					ResolveParentProperty->GetPropertyValue_InContainer(&CameraBinding);
			}
			Camera.bLockPreviousCamera = CameraSection->bLockPreviousCamera;
			FString BindingCanonical;
			AppendToken(BindingCanonical, TEXT("hyperai.cinematics.camera-binding.v1"));
			AppendToken(BindingCanonical, Camera.CameraBindingId);
			AppendToken(BindingCanonical, FString::FromInt(Camera.CameraBindingSequenceId));
			AppendToken(BindingCanonical,
				FString::FromInt(Camera.CameraBindingResolveParentIndex));
			AppendToken(BindingCanonical, Camera.bLockPreviousCamera
				? TEXT("lock_previous") : TEXT("do_not_lock_previous"));
			Camera.BindingFingerprint = HashCanonical(BindingCanonical);
			Record.ChannelFingerprint = Camera.BindingFingerprint;
			Record.bChannelProjectionSupported = Record.bChannelProjectionSupported
				&& CameraBinding.IsValid()
				&& IsCanonicalSha256(Camera.BindingFingerprint);
			Record.bDetailProjectionComplete = Record.bDetailProjectionComplete
				&& Record.bChannelProjectionSupported;
			Camera.Range = Record.Range;
		}

		if (Work.Section->GetClass() == UMovieSceneEventTriggerSection::StaticClass())
		{
			Record.bDetailProjectionComplete = false;
			Record.bChannelProjectionSupported = false;
			Context.MarkIncomplete(TEXT("event_detail_projection_limited"),
				Record.SectionId, Record.ClassName,
				TEXT("Event endpoint, payload-map values, and weak graph identity are intentionally not an executable CAS surface in v1."));
			FString EventChannelCanonical;
			AppendToken(EventChannelCanonical, TEXT("hyperai.cinematics.event-trigger-channel.v1"));
			const UMovieSceneEventTriggerSection* EventSection =
				CastChecked<UMovieSceneEventTriggerSection>(Work.Section);
			const TMovieSceneChannelData<const FMovieSceneEvent> Data =
				EventSection->EventChannel.GetData();
			const TArrayView<const FFrameNumber> Times = Data.GetTimes();
			const TArrayView<const FMovieSceneEvent> Values = Data.GetValues();
			for (int32 Index = 0; Index < Times.Num(); ++Index)
			{
				if ((Index & 63) == 0
					&& !Context.CheckDeadline(Record.SectionId, SequencePath))
				{
					break;
				}
				FHyperAICinematicsKeyRecord& Key = OutSnapshot.Keys.AddDefaulted_GetRef();
				Key.KeyId = FString::Printf(TEXT("key:%06d"), GlobalKeyIndex++);
				Key.SectionId = Record.SectionId;
				Key.ChannelType = TEXT("MovieSceneEventChannel");
				Key.ChannelIndex = 0;
				Key.KeyIndex = Index;
				Key.Frame = Times[Index].Value;
				FHyperAICinematicsEventRecord& Event = OutSnapshot.Events.AddDefaulted_GetRef();
				Event.EventId = FString::Printf(TEXT("event:%04d"), GlobalEventIndex++);
				Event.SectionId = Record.SectionId;
				Event.EventKind = TEXT("trigger");
				Event.Frame = Times[Index].Value;
				bool bEventComplete = true;
				Event.CompiledFunctionName = EventFunctionName(Values[Index], bEventComplete);
				Event.BoundObjectClassName = EventBoundObjectClassName(Values[Index], bEventComplete);
#if WITH_EDITORONLY_DATA
				Event.PayloadVariableCount = Values[Index].PayloadVariables.Num();
#endif
				Event.bDetailProjectionComplete = false;
				FString EventDetailCanonical;
				AppendToken(EventDetailCanonical, TEXT("hyperai.cinematics.event-detail.v1"));
				AppendToken(EventDetailCanonical, Event.EventKind);
				AppendToken(EventDetailCanonical, FString::FromInt(Event.Frame));
				AppendToken(EventDetailCanonical, Event.CompiledFunctionName);
				AppendToken(EventDetailCanonical, Event.BoundObjectClassName);
				AppendToken(EventDetailCanonical,
					FString::FromInt(Event.PayloadVariableCount));
				Event.DetailFingerprint = HashCanonical(EventDetailCanonical);
				Key.ValueFingerprint = Event.DetailFingerprint;
				AppendToken(EventChannelCanonical, FString::FromInt(Key.Frame));
				AppendToken(EventChannelCanonical, Key.ValueFingerprint);
				if (!bEventComplete || Event.PayloadVariableCount > MaxEventPayloadVariables)
				{
					Context.MarkIncomplete(TEXT("event_projection_incomplete"), Event.EventId,
						Record.SectionId, TEXT("Event name, bound-object class, or payload map exceeded a hard bound."));
				}
			}
			Record.ChannelFingerprint = HashCanonical(EventChannelCanonical);
		}
		else if (Work.Section->GetClass() == UMovieSceneEventRepeaterSection::StaticClass())
		{
			Record.bDetailProjectionComplete = false;
			Record.bChannelProjectionSupported = false;
			Context.MarkIncomplete(TEXT("event_detail_projection_limited"),
				Record.SectionId, Record.ClassName,
				TEXT("Repeater endpoint, payload-map values, and weak graph identity are intentionally not an executable CAS surface in v1."));
			const UMovieSceneEventRepeaterSection* EventSection =
				CastChecked<UMovieSceneEventRepeaterSection>(Work.Section);
			FHyperAICinematicsEventRecord& Event = OutSnapshot.Events.AddDefaulted_GetRef();
			Event.EventId = FString::Printf(TEXT("event:%04d"), GlobalEventIndex++);
			Event.SectionId = Record.SectionId;
			Event.EventKind = TEXT("repeater");
			Event.Frame = Record.Range.bHasLowerBound ? Record.Range.LowerFrame : 0;
			bool bEventComplete = true;
			Event.CompiledFunctionName = EventFunctionName(EventSection->Event, bEventComplete);
			Event.BoundObjectClassName = EventBoundObjectClassName(EventSection->Event, bEventComplete);
#if WITH_EDITORONLY_DATA
			Event.PayloadVariableCount = EventSection->Event.PayloadVariables.Num();
#endif
			Event.bDetailProjectionComplete = false;
			FString EventDetailCanonical;
			AppendToken(EventDetailCanonical, TEXT("hyperai.cinematics.event-detail.v1"));
			AppendToken(EventDetailCanonical, Event.EventKind);
			AppendToken(EventDetailCanonical, FString::FromInt(Event.Frame));
			AppendToken(EventDetailCanonical, Event.CompiledFunctionName);
			AppendToken(EventDetailCanonical, Event.BoundObjectClassName);
			AppendToken(EventDetailCanonical, FString::FromInt(Event.PayloadVariableCount));
			Event.DetailFingerprint = HashCanonical(EventDetailCanonical);
			Record.ChannelFingerprint = Event.DetailFingerprint;
			if (!bEventComplete || Event.PayloadVariableCount > MaxEventPayloadVariables)
			{
				Context.MarkIncomplete(TEXT("event_projection_incomplete"), Event.EventId,
					Record.SectionId, TEXT("Repeater event projection exceeded a hard bound."));
			}
		}
		else if (Work.bFixedChannels)
		{
			FMovieSceneChannelProxy& Proxy = Work.Section->GetChannelProxy();
			FString SectionChannelCanonical;
			AppendToken(SectionChannelCanonical,
				TEXT("hyperai.cinematics.fixed-channels.v1"));
			int32 ChannelIndex = 0;
			for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
			{
				FString ChannelType;
				if (!MaterializeNameBounded(Entry.GetChannelTypeName(), MaxNameCharacters,
					ChannelType))
				{
					Record.bChannelProjectionSupported = false;
					Context.MarkIncomplete(TEXT("channel_type_name_unbounded"), Record.SectionId,
						SequencePath, TEXT("Channel type name exceeded the output contract."));
				}
				for (FMovieSceneChannel* Channel : Entry.GetChannels())
				{
					const int32 ExpectedKeys = Channel->GetNumKeys();
					FString ChannelCanonical;
					AppendToken(ChannelCanonical, ChannelType);
					AppendToken(ChannelCanonical, FString::FromInt(ChannelIndex));
					AppendToken(ChannelCanonical, FString::FromInt(ExpectedKeys));
					if (Entry.GetChannelTypeName()
						== FMovieSceneFloatChannel::StaticStruct()->GetFName())
					{
						const FMovieSceneFloatChannel* Typed =
							static_cast<const FMovieSceneFloatChannel*>(Channel);
						const TArrayView<const FFrameNumber> Times = Typed->GetTimes();
						const TArrayView<const FMovieSceneFloatValue> Values = Typed->GetValues();
						if (Times.Num() != ExpectedKeys || Values.Num() != ExpectedKeys
							|| !AppendFloatChannelHeader(ChannelCanonical, *Typed))
						{
							Record.bChannelProjectionSupported = false;
							Context.MarkIncomplete(TEXT("float_channel_value_projection_drift"),
								Record.SectionId, ChannelType,
								TEXT("Float key times, values, defaults, or rate no longer match bounded preflight."));
						}
						const int32 Count = FMath::Min(Times.Num(), Values.Num());
						for (int32 KeyIndex = 0; KeyIndex < Count
							&& OutSnapshot.Keys.Num() < MaxKeys; ++KeyIndex)
						{
							if ((KeyIndex & 63) == 0
								&& !Context.CheckDeadline(Record.SectionId, SequencePath))
							{
								Record.bChannelProjectionSupported = false;
								break;
							}
							FHyperAICinematicsKeyRecord& Key =
								OutSnapshot.Keys.AddDefaulted_GetRef();
							Key.KeyId = FString::Printf(TEXT("key:%06d"), GlobalKeyIndex++);
							Key.SectionId = Record.SectionId;
							Key.ChannelType = ChannelType;
							Key.ChannelIndex = ChannelIndex;
							Key.KeyIndex = KeyIndex;
							Key.Frame = Times[KeyIndex].Value;
							Key.ValueFingerprint = HashFloatValue(Values[KeyIndex]);
							if (!IsCanonicalSha256(Key.ValueFingerprint))
							{
								Record.bChannelProjectionSupported = false;
								Context.MarkIncomplete(TEXT("float_key_value_unsealable"),
									Key.KeyId, Record.SectionId,
									TEXT("A non-finite or over-budget float key value could not be sealed."));
							}
							AppendToken(ChannelCanonical, FString::FromInt(Key.Frame));
							AppendToken(ChannelCanonical, Key.ValueFingerprint);
						}
					}
					else if (Entry.GetChannelTypeName()
						== FMovieSceneDoubleChannel::StaticStruct()->GetFName())
					{
						const FMovieSceneDoubleChannel* Typed =
							static_cast<const FMovieSceneDoubleChannel*>(Channel);
						const TArrayView<const FFrameNumber> Times = Typed->GetTimes();
						const TArrayView<const FMovieSceneDoubleValue> Values = Typed->GetValues();
						if (Times.Num() != ExpectedKeys || Values.Num() != ExpectedKeys
							|| !AppendDoubleChannelHeader(ChannelCanonical, *Typed))
						{
							Record.bChannelProjectionSupported = false;
							Context.MarkIncomplete(TEXT("double_channel_value_projection_drift"),
								Record.SectionId, ChannelType,
								TEXT("Double key times, values, defaults, or rate no longer match bounded preflight."));
						}
						const int32 Count = FMath::Min(Times.Num(), Values.Num());
						for (int32 KeyIndex = 0; KeyIndex < Count
							&& OutSnapshot.Keys.Num() < MaxKeys; ++KeyIndex)
						{
							if ((KeyIndex & 63) == 0
								&& !Context.CheckDeadline(Record.SectionId, SequencePath))
							{
								Record.bChannelProjectionSupported = false;
								break;
							}
							FHyperAICinematicsKeyRecord& Key =
								OutSnapshot.Keys.AddDefaulted_GetRef();
							Key.KeyId = FString::Printf(TEXT("key:%06d"), GlobalKeyIndex++);
							Key.SectionId = Record.SectionId;
							Key.ChannelType = ChannelType;
							Key.ChannelIndex = ChannelIndex;
							Key.KeyIndex = KeyIndex;
							Key.Frame = Times[KeyIndex].Value;
							Key.ValueFingerprint = HashDoubleValue(Values[KeyIndex]);
							if (!IsCanonicalSha256(Key.ValueFingerprint))
							{
								Record.bChannelProjectionSupported = false;
								Context.MarkIncomplete(TEXT("double_key_value_unsealable"),
									Key.KeyId, Record.SectionId,
									TEXT("A non-finite or over-budget double key value could not be sealed."));
							}
							AppendToken(ChannelCanonical, FString::FromInt(Key.Frame));
							AppendToken(ChannelCanonical, Key.ValueFingerprint);
						}
					}
					else
					{
						Record.bChannelProjectionSupported = false;
						Context.MarkIncomplete(TEXT("channel_value_projection_unsupported"),
							Record.SectionId, ChannelType,
							TEXT("Only exact UE 5.8 float/double channel value layouts are projected."));
						TArray<FFrameNumber> Times;
						Times.Reserve(ExpectedKeys);
						Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
						for (int32 KeyIndex = 0; KeyIndex < Times.Num()
							&& OutSnapshot.Keys.Num() < MaxKeys; ++KeyIndex)
						{
							if ((KeyIndex & 63) == 0
								&& !Context.CheckDeadline(Record.SectionId, SequencePath))
							{
								break;
							}
							FHyperAICinematicsKeyRecord& Key =
								OutSnapshot.Keys.AddDefaulted_GetRef();
							Key.KeyId = FString::Printf(TEXT("key:%06d"), GlobalKeyIndex++);
							Key.SectionId = Record.SectionId;
							Key.ChannelType = ChannelType;
							Key.ChannelIndex = ChannelIndex;
							Key.KeyIndex = KeyIndex;
							Key.Frame = Times[KeyIndex].Value;
						}
					}
					AppendToken(SectionChannelCanonical, HashCanonical(ChannelCanonical));
					++ChannelIndex;
					if (Context.bDeadlineExceeded) break;
				}
				if (Context.bDeadlineExceeded) break;
			}
			Record.ChannelFingerprint = HashCanonical(SectionChannelCanonical);
			Record.bChannelProjectionSupported = Record.bChannelProjectionSupported
				&& IsCanonicalSha256(Record.ChannelFingerprint);
		}
	}

	const bool bSequenceProjectionComplete = Context.bComplete
		&& OutSnapshot.Bindings.Num() == BindingValues.Num()
		&& OutSnapshot.Tracks.Num() == TrackWork.Num()
		&& OutSnapshot.Sections.Num() == SectionWork.Num()
		&& OutSnapshot.Keys.Num() == KeyCount64
		&& OutSnapshot.Cameras.Num() == CameraCount64
		&& OutSnapshot.Events.Num() == EventCount64;
	SequenceRecord.BindingCount = OutSnapshot.Bindings.Num();
	SequenceRecord.TrackCount = OutSnapshot.Tracks.Num();
	SequenceRecord.SectionCount = OutSnapshot.Sections.Num();
	SequenceRecord.ChannelCount = static_cast<int32>(ChannelCount64);
	SequenceRecord.KeyCount = OutSnapshot.Keys.Num();
	SequenceRecord.CameraCutCount = OutSnapshot.Cameras.Num();
	SequenceRecord.EventCount = OutSnapshot.Events.Num();
	SequenceRecord.VolatileRevision = ComputeSequenceVolatileRevision(OutSnapshot);
	SequenceRecord.bVolatileRevisionComplete = bSequenceProjectionComplete
		&& IsCanonicalSha256(SequenceRecord.VolatileRevision);
	if (!SequenceRecord.bVolatileRevisionComplete)
	{
		Context.MarkIncomplete(TEXT("sequence_volatile_revision_incomplete"), TEXT("sequence"),
			SequencePath, TEXT("Unsupported or drifting loaded values prevent exact current-state CAS."));
	}

	bool bMRQProjectionComplete = RenderConfigPath.IsEmpty();
	if (RenderConfig)
	{
		FHyperAICinematicsMRQConfigRecord& ConfigRecord = OutSnapshot.RenderConfig;
		ConfigRecord.bPresent = true;
		ConfigRecord.TargetPath = RenderConfigPath;
		ConfigRecord.SettingsSerialNumber = RenderConfig->GetSettingsSerialNumber();
		ConfigRecord.SettingCount = MRQSettingCount;
		ConfigRecord.ShotOverrideCount = RenderConfig->PerShotConfigMapping.Num();
		FPackageEvidence ConfigPackage;
		CapturePackageEvidence(RenderConfig, Context, TEXT("mrq_package"), ConfigPackage);
		ConfigRecord.PackageName = ConfigPackage.PackageName;
		ConfigRecord.DiskExistence = ConfigPackage.DiskExistence;
		ConfigRecord.PackageSavedHash = ConfigPackage.PackageSavedHash;
		ConfigRecord.DiskSize = ConfigPackage.DiskSize;
		ConfigRecord.bWasLoadedFromDisk = ConfigPackage.bWasLoadedFromDisk;
		ConfigRecord.bPackageDirty = ConfigPackage.bPackageDirty;
		ConfigRecord.PersistedRevision = ConfigPackage.PersistedRevision;
		ConfigRecord.bPersistedRevisionComplete = ConfigPackage.bPersistedRevisionComplete;
		OutSnapshot.RenderSettings.Reserve(MRQSettingCount);
		bool bEverySettingProjected = ConfigRecord.ShotOverrideCount == 0;
		if (ConfigRecord.ShotOverrideCount < 0
			|| ConfigRecord.ShotOverrideCount > MaxMRQSettings)
		{
			OutStatus = TEXT("mrq_shot_override_count_exceeded");
			OutDiagnostic = TEXT("Per-shot MRQ override count exceeded its pre-copy hard bound.");
			return false;
		}
		if (ConfigRecord.ShotOverrideCount > 0)
		{
			Context.MarkIncomplete(TEXT("mrq_shot_override_projection_limited"),
				TEXT("mrq_config"), RenderConfigPath,
				TEXT("Per-shot MoviePipeline configs are not projected by the v1 render contract."));
		}
		for (int32 Index = 0; Index < MRQSettingCount; ++Index)
		{
			if (!Context.CheckDeadline(TEXT("mrq_setting"), RenderConfigPath))
			{
				bEverySettingProjected = false;
				break;
			}
			UMoviePipelineSetting* Setting = Index < UserSettingCount
				? Cast<UMoviePipelineSetting>(SettingsInner->GetObjectPropertyValue(
					SettingsHelper->GetRawPtr(Index)))
				: PrimaryOutputSetting;
			FHyperAICinematicsMRQSettingRecord& SettingRecord =
				OutSnapshot.RenderSettings.AddDefaulted_GetRef();
			SettingRecord.SettingId = FString::Printf(TEXT("mrq-setting:%03d"), Index);
			if (!Setting)
			{
				bEverySettingProjected = false;
				Context.MarkIncomplete(TEXT("null_mrq_setting"), SettingRecord.SettingId,
					RenderConfigPath, TEXT("MoviePipeline config contained a null setting."));
				continue;
			}
			if (!MaterializeNameBounded(Setting->GetClass()->GetFName(), MaxNameCharacters,
				SettingRecord.ClassName))
			{
				bEverySettingProjected = false;
				Context.MarkIncomplete(TEXT("mrq_setting_class_name_unbounded"),
					SettingRecord.SettingId, RenderConfigPath,
					TEXT("MoviePipeline setting class name exceeded the output contract."));
			}
			SettingRecord.bEnabled = Setting->UMoviePipelineSetting::IsEnabled();
			FString DetailCanonical;
			AppendToken(DetailCanonical, TEXT("hyperai.cinematics.mrq-setting.v1"));
			AppendToken(DetailCanonical, SettingRecord.ClassName);
			AppendToken(DetailCanonical, SettingRecord.bEnabled ? TEXT("enabled") : TEXT("disabled"));
			if (Setting->GetClass() == UMoviePipelineOutputSetting::StaticClass())
			{
				const UMoviePipelineOutputSetting* Output =
					CastChecked<UMoviePipelineOutputSetting>(Setting);
				if (ConfigRecord.bOutputSettingPresent)
				{
					bEverySettingProjected = false;
					Context.MarkIncomplete(TEXT("duplicate_mrq_output_setting"),
						SettingRecord.SettingId, RenderConfigPath,
						TEXT("More than one exact OutputSetting prevents a unique render contract."));
				}
				ConfigRecord.bOutputSettingPresent = true;
				if (Output->OutputDirectory.Path.Len() > MaxPathCharacters
					|| Output->FileNameFormat.Len() > MaxPathCharacters
					|| HasControlCharacter(Output->OutputDirectory.Path)
					|| HasControlCharacter(Output->FileNameFormat))
				{
					bEverySettingProjected = false;
					Context.MarkIncomplete(TEXT("mrq_output_string_unbounded"),
						SettingRecord.SettingId, RenderConfigPath,
						TEXT("Output directory or filename format exceeded the bounded string contract."));
				}
				else
				{
					ConfigRecord.OutputDirectory = Output->OutputDirectory.Path;
					ConfigRecord.FileNameFormat = Output->FileNameFormat;
				}
				ConfigRecord.OutputWidth = Output->OutputResolution.X;
				ConfigRecord.OutputHeight = Output->OutputResolution.Y;
				ConfigRecord.bUseCustomFrameRate = Output->bUseCustomFrameRate;
				const FFrameRate EffectiveOutputRate = Output->bUseCustomFrameRate
					? Output->OutputFrameRate : DisplayRate;
				ConfigRecord.OutputRateNumerator = EffectiveOutputRate.Numerator;
				ConfigRecord.OutputRateDenominator = EffectiveOutputRate.Denominator;
				ConfigRecord.bUseCustomPlaybackRange = Output->bUseCustomPlaybackRange;
				ConfigRecord.CustomStartFrame = Output->CustomStartFrame;
				ConfigRecord.CustomEndFrame = Output->CustomEndFrame;
				AppendToken(DetailCanonical, ConfigRecord.OutputDirectory);
				AppendToken(DetailCanonical, ConfigRecord.FileNameFormat);
				AppendToken(DetailCanonical, FString::FromInt(ConfigRecord.OutputWidth));
				AppendToken(DetailCanonical, FString::FromInt(ConfigRecord.OutputHeight));
				AppendToken(DetailCanonical, Output->bUseCustomFrameRate ? TEXT("custom_rate") : TEXT("sequence_rate"));
				AppendToken(DetailCanonical, FString::FromInt(Output->OutputFrameRate.Numerator));
				AppendToken(DetailCanonical, FString::FromInt(Output->OutputFrameRate.Denominator));
				AppendToken(DetailCanonical, Output->bOverrideExistingOutput ? TEXT("overwrite") : TEXT("no_overwrite"));
				AppendToken(DetailCanonical, FString::FromInt(Output->HandleFrameCount));
				AppendToken(DetailCanonical, FString::FromInt(Output->OutputFrameStep));
				AppendToken(DetailCanonical, Output->bUseCustomPlaybackRange ? TEXT("custom_range") : TEXT("sequence_range"));
				AppendToken(DetailCanonical, FString::FromInt(Output->CustomStartFrame));
				AppendToken(DetailCanonical, FString::FromInt(Output->CustomEndFrame));
				AppendToken(DetailCanonical, Output->bAutoVersion ? TEXT("auto_version") : TEXT("fixed_version"));
				AppendToken(DetailCanonical, FString::FromInt(Output->VersionNumber));
				AppendToken(DetailCanonical, FString::FromInt(Output->AdditionalSubmixOutputs.Num()));
				AppendToken(DetailCanonical, FString::FromInt(Output->DEBUG_OutputFrameStepOffset));
				AppendToken(DetailCanonical, FString::FromInt(Output->ZeroPadFrameNumbers));
				AppendToken(DetailCanonical, FString::FromInt(Output->FrameNumberOffset));
				AppendToken(DetailCanonical, Output->bFlushDiskWritesPerShot
					? TEXT("flush_per_shot") : TEXT("no_flush_per_shot"));
				if (!Output->AdditionalSubmixOutputs.IsEmpty())
				{
					bEverySettingProjected = false;
					Context.MarkIncomplete(TEXT("mrq_output_submix_projection_limited"),
						SettingRecord.SettingId, RenderConfigPath,
						TEXT("Output submix reference identities are intentionally not materialized by this v1 snapshot."));
				}
				SettingRecord.bDetailProjectionComplete =
					Output->AdditionalSubmixOutputs.IsEmpty()
					&& Output->OutputDirectory.Path.Len() <= MaxPathCharacters
					&& Output->FileNameFormat.Len() <= MaxPathCharacters;
			}
			else
			{
				bEverySettingProjected = false;
				Context.MarkIncomplete(TEXT("mrq_setting_detail_projection_limited"),
					SettingRecord.SettingId, SettingRecord.ClassName,
					TEXT("Only class and enabled state are exposed for this setting; exact persisted fields remain unprojected."));
			}
			SettingRecord.DetailFingerprint = HashCanonical(DetailCanonical);
			if (!IsCanonicalSha256(SettingRecord.DetailFingerprint))
			{
				SettingRecord.bDetailProjectionComplete = false;
				bEverySettingProjected = false;
			}
			if (Setting->GetClass() == UMoviePipelineOutputSetting::StaticClass())
			{
				ConfigRecord.OutputSettingFingerprint = SettingRecord.DetailFingerprint;
			}
		}
		ConfigRecord.VolatileRevision = ComputeMRQVolatileRevision(OutSnapshot);
		ConfigRecord.bVolatileRevisionComplete = bEverySettingProjected
			&& OutSnapshot.RenderSettings.Num() == MRQSettingCount
			&& ConfigRecord.bOutputSettingPresent
			&& IsCanonicalSha256(ConfigRecord.OutputSettingFingerprint)
			&& IsCanonicalSha256(ConfigRecord.VolatileRevision);
		bMRQProjectionComplete = ConfigRecord.bVolatileRevisionComplete;
		if (!bMRQProjectionComplete)
		{
			Context.MarkIncomplete(TEXT("mrq_volatile_revision_incomplete"), TEXT("mrq_config"),
				RenderConfigPath,
				TEXT("Unprojected MoviePipeline setting state prevents exact current-config CAS."));
		}
	}

	OutSnapshot.bComplete = Context.bComplete
		&& SequenceRecord.bPersistedRevisionComplete
		&& SequenceRecord.bVolatileRevisionComplete
		&& bMRQProjectionComplete;
	OutSnapshot.CaptureIssues = MoveTemp(Context.Issues);
	OutSnapshot.SnapshotFingerprint = ComputeSnapshotFingerprint(OutSnapshot);
	if (!IsCanonicalSha256(OutSnapshot.SnapshotFingerprint))
	{
		OutSnapshot.bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("snapshot_fingerprint_unavailable"),
			TEXT("warning"), TEXT("snapshot"), SequencePath,
			TEXT("The detached snapshot could not be sealed inside the bounded hash envelope."));
	}
	OutStatus = OutSnapshot.bComplete ? TEXT("ok_complete") : TEXT("ok_partial");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured a complete bounded already-loaded LevelSequence/MovieScene/MRQ value snapshot without loading assets, opening editors, or executing Sequencer/MRQ effects.")
		: TEXT("Captured bounded loaded values, but unknown package evidence, unsupported section channels, custom MRQ setting state, a deadline, or another bounded omission prevents complete CAS.");
	return true;
}

FHyperAICinematicsValidateReport FHyperAIStudioCinematicsContracts::ValidateSnapshot(
	const FHyperAIStudioCinematicsValueSnapshot& Snapshot,
	const FHyperAICinematicsValidateRequest& Request)
{
	FHyperAICinematicsValidateReport Report;
	Report.Policy = Request.Policy.Left(32);
	Report.Capabilities = GetCapabilityMatrix();
	FString EnvelopeError;
	if (!IsDetachedSnapshotEnvelopeBounded(Snapshot, EnvelopeError))
	{
		AddIssue(Report.Issues, TEXT("detached_snapshot_envelope_exceeded"),
			TEXT("error"), TEXT("snapshot_envelope"), EnvelopeError,
			TEXT("Detached validation rejected count or text bounds before copying, hashing, or allocating record indexes."));
		Report.ErrorCount = 1;
		Report.ValidatorFingerprint = HashCanonical(
			TEXT("hyperai.cinematics.detached-validator.envelope-rejected.v1"));
		Report.bTruncated = true;
		Report.bComplete = false;
		Report.bValid = false;
		Report.bOk = false;
		Report.Status = TEXT("validation_envelope_rejected");
		Report.Diagnostic = TEXT("Detached snapshot exceeded a hard count or text envelope and was rejected before value traversal.");
		return Report;
	}
	Report.PersistedRevision = Snapshot.Sequence.PersistedRevision;
	Report.VolatileRevision = Snapshot.Sequence.VolatileRevision;
	Report.RenderConfigPersistedRevision = Snapshot.RenderConfig.PersistedRevision;
	Report.RenderConfigVolatileRevision = Snapshot.RenderConfig.VolatileRevision;
	const int32 IssueLimit = FMath::Clamp(Request.MaxIssues, 1, MaxIssues);
	bool bIssueOverflow = false;
	auto Issue = [&](const TCHAR* Code, const TCHAR* Severity, const FString& StableId,
		const FString& Subject, const TCHAR* Detail)
	{
		if (Report.Issues.Num() >= IssueLimit)
		{
			bIssueOverflow = true;
			return;
		}
		AddIssue(Report.Issues, Code, Severity, StableId, Subject, Detail);
	};
	for (const FHyperAICinematicsIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (Report.Issues.Num() >= IssueLimit)
		{
			bIssueOverflow = true;
			break;
		}
		Report.Issues.Add(CaptureIssue);
	}

	if (Request.Policy != TEXT("structural") && Request.Policy != TEXT("render_ready"))
	{
		Issue(TEXT("unsupported_validation_policy"), TEXT("error"), TEXT("policy"),
			Request.Policy, TEXT("Policy must be exactly structural or render_ready."));
	}
	FString CountError;
	if (!AdmitCountsBeforeProjection(Snapshot.Bindings.Num(), Snapshot.Tracks.Num(),
		Snapshot.Sections.Num(), Snapshot.Sequence.ChannelCount, Snapshot.Keys.Num(),
		Snapshot.Cameras.Num(), Snapshot.Events.Num(), Snapshot.RenderSettings.Num(), CountError))
	{
		Issue(*CountError, TEXT("error"), TEXT("counts"), Snapshot.Sequence.TargetPath,
			TEXT("Detached snapshot counts exceed the closed cinematics envelope."));
	}
	if (!IsCanonicalPrimaryAssetPath(Snapshot.Sequence.TargetPath)
		|| Snapshot.Sequence.TargetPath != Request.SequencePath)
	{
		Issue(TEXT("sequence_identity_mismatch"), TEXT("error"), TEXT("sequence"),
			Snapshot.Sequence.TargetPath,
			TEXT("Detached sequence identity does not match the exact validation target."));
	}
	if (!Snapshot.Sequence.bLoaded || Snapshot.Sequence.ClassName != TEXT("LevelSequence"))
	{
		Issue(TEXT("sequence_class_not_sealed"), TEXT("error"), TEXT("sequence"),
			Snapshot.Sequence.ClassName,
			TEXT("Validation accepts only an exact already-loaded LevelSequence projection."));
	}
	if (Snapshot.Sequence.BindingCount != Snapshot.Bindings.Num()
		|| Snapshot.Sequence.TrackCount != Snapshot.Tracks.Num()
		|| Snapshot.Sequence.SectionCount != Snapshot.Sections.Num()
		|| Snapshot.Sequence.KeyCount != Snapshot.Keys.Num()
		|| Snapshot.Sequence.CameraCutCount != Snapshot.Cameras.Num()
		|| Snapshot.Sequence.EventCount != Snapshot.Events.Num())
	{
		Issue(TEXT("declared_count_mismatch"), TEXT("error"), TEXT("counts"),
			Snapshot.Sequence.TargetPath,
			TEXT("Sequence record cardinalities differ from detached value arrays."));
	}
	if (Snapshot.Sequence.TickResolutionNumerator <= 0
		|| Snapshot.Sequence.TickResolutionDenominator <= 0
		|| Snapshot.Sequence.DisplayRateNumerator <= 0
		|| Snapshot.Sequence.DisplayRateDenominator <= 0)
	{
		Issue(TEXT("invalid_frame_rate"), TEXT("error"), TEXT("sequence"),
			Snapshot.Sequence.TargetPath,
			TEXT("Tick resolution and display rate must both be positive rational values."));
	}
	if (!IsRangeWellFormed(Snapshot.Sequence.PlaybackRange))
	{
		Issue(TEXT("invalid_playback_range"), TEXT("error"), TEXT("sequence"),
			Snapshot.Sequence.TargetPath, TEXT("Playback range bounds are contradictory."));
	}

	const FString ExpectedPersisted = RecomputePersistedRevision(
		Snapshot.Sequence.PackageName, Snapshot.Sequence.DiskExistence,
		Snapshot.Sequence.PackageSavedHash, Snapshot.Sequence.DiskSize);
	if (!Snapshot.Sequence.bPersistedRevisionComplete
		|| !IsCanonicalSha256(Snapshot.Sequence.PersistedRevision)
		|| Snapshot.Sequence.PersistedRevision != ExpectedPersisted
		|| Snapshot.Sequence.DiskExistence != TEXT("exists")
		|| !IsLowerHexString(Snapshot.Sequence.PackageSavedHash, 40)
		|| Snapshot.Sequence.DiskSize < 0 || !Snapshot.Sequence.bWasLoadedFromDisk)
	{
		Issue(TEXT("sequence_persisted_revision_incomplete"), TEXT("error"),
			TEXT("sequence_persisted"), Snapshot.Sequence.PackageName,
			TEXT("Persisted sequence CAS is absent or inconsistent with package evidence."));
	}
	const FString ExpectedVolatile = ComputeSequenceVolatileRevision(Snapshot);
	if (!Snapshot.Sequence.bVolatileRevisionComplete
		|| !IsCanonicalSha256(Snapshot.Sequence.VolatileRevision)
		|| Snapshot.Sequence.VolatileRevision != ExpectedVolatile)
	{
		Issue(TEXT("sequence_volatile_revision_incomplete"), TEXT("error"),
			TEXT("sequence_volatile"), Snapshot.Sequence.TargetPath,
			TEXT("Loaded MovieScene values do not reproduce the asserted volatile CAS."));
	}
	if (!Request.ExpectedPersistedRevision.IsEmpty()
		&& Request.ExpectedPersistedRevision != Snapshot.Sequence.PersistedRevision)
	{
		Issue(TEXT("stale_sequence_persisted_revision"), TEXT("error"),
			TEXT("sequence_persisted_cas"), Snapshot.Sequence.TargetPath,
			TEXT("Fresh persisted sequence revision differs from the expected value."));
	}
	if (!Request.ExpectedVolatileRevision.IsEmpty()
		&& Request.ExpectedVolatileRevision != Snapshot.Sequence.VolatileRevision)
	{
		Issue(TEXT("stale_sequence_volatile_revision"), TEXT("error"),
			TEXT("sequence_volatile_cas"), Snapshot.Sequence.TargetPath,
			TEXT("Fresh loaded sequence revision differs from the expected value."));
	}

	TSet<FString> BindingIds;
	TSet<FString> BindingGuids;
	for (const FHyperAICinematicsBindingRecord& Binding : Snapshot.Bindings)
	{
		if (Binding.BindingId.IsEmpty() || Binding.BindingGuid.IsEmpty()
			|| BindingIds.Contains(Binding.BindingId) || BindingGuids.Contains(Binding.BindingGuid))
		{
			Issue(TEXT("invalid_or_duplicate_binding"), TEXT("error"), Binding.BindingId,
				Binding.BindingGuid, TEXT("Binding ids and GUIDs must be non-empty and unique."));
		}
		BindingIds.Add(Binding.BindingId);
		BindingGuids.Add(Binding.BindingGuid);
		if (!Binding.bDetailProjectionComplete
			|| !IsCanonicalSha256(Binding.DetailFingerprint))
		{
			Issue(TEXT("binding_detail_projection_incomplete"), TEXT("error"),
				Binding.BindingId, Binding.BindingGuid,
				TEXT("Binding object/template semantic detail is partial or unsealed."));
		}
		if (Binding.Kind != TEXT("spawnable") && Binding.Kind != TEXT("possessable"))
		{
			Issue(TEXT("binding_kind_unknown"), TEXT("error"), Binding.BindingId,
				Binding.BindingGuid, TEXT("Binding kind must be spawnable or possessable."));
		}
	}

	TSet<FString> TrackIds;
	TMap<FString, int32> TracksPerBinding;
	for (const FHyperAICinematicsTrackRecord& Track : Snapshot.Tracks)
	{
		if (Track.TrackId.IsEmpty() || TrackIds.Contains(Track.TrackId))
		{
			Issue(TEXT("invalid_or_duplicate_track"), TEXT("error"), Track.TrackId,
				Track.ClassName, TEXT("Track ids must be non-empty and unique."));
		}
		TrackIds.Add(Track.TrackId);
		if (!Track.bDetailProjectionComplete || !IsCanonicalSha256(Track.DetailFingerprint))
		{
			Issue(TEXT("track_detail_projection_incomplete"), TEXT("error"), Track.TrackId,
				Track.ClassName, TEXT("Track semantic detail is partial or unsealed."));
		}
		if (!Track.bRootTrack && !BindingGuids.Contains(Track.BindingGuid))
		{
			Issue(TEXT("track_binding_missing"), TEXT("error"), Track.TrackId,
				Track.BindingGuid, TEXT("A bound track references no detached binding GUID."));
		}
		if (!Track.bRootTrack)
		{
			++TracksPerBinding.FindOrAdd(Track.BindingGuid);
		}
	}
	for (const FHyperAICinematicsBindingRecord& Binding : Snapshot.Bindings)
	{
		if (Binding.TrackCount != TracksPerBinding.FindRef(Binding.BindingGuid))
		{
			Issue(TEXT("binding_track_count_mismatch"), TEXT("error"), Binding.BindingId,
				Binding.BindingGuid,
				TEXT("Binding track count differs from detached track references."));
		}
	}

	TSet<FString> SectionIds;
	TMap<FString, FHyperAICinematicsFrameRange> SectionRanges;
	TMap<FString, int32> SectionsPerTrack;
	int64 DeclaredChannelCount = 0;
	int64 DeclaredKeyCount = 0;
	for (const FHyperAICinematicsSectionRecord& Section : Snapshot.Sections)
	{
		if (Section.SectionId.IsEmpty() || SectionIds.Contains(Section.SectionId)
			|| !TrackIds.Contains(Section.TrackId))
		{
			Issue(TEXT("invalid_section_reference"), TEXT("error"), Section.SectionId,
				Section.TrackId, TEXT("Section id must be unique and reference a detached track."));
		}
		SectionIds.Add(Section.SectionId);
		SectionRanges.Add(Section.SectionId, Section.Range);
		++SectionsPerTrack.FindOrAdd(Section.TrackId);
		DeclaredChannelCount += Section.ChannelCount;
		DeclaredKeyCount += Section.KeyCount;
		if (!Section.bDetailProjectionComplete
			|| !IsCanonicalSha256(Section.DetailFingerprint))
		{
			Issue(TEXT("section_detail_projection_incomplete"), TEXT("error"),
				Section.SectionId, Section.ClassName,
				TEXT("Section semantic detail is partial or unsealed."));
		}
		if (!Section.bChannelProjectionSupported
			|| !IsCanonicalSha256(Section.ChannelFingerprint))
		{
			Issue(TEXT("section_channel_projection_incomplete"), TEXT("error"),
				Section.SectionId, Section.ClassName,
				TEXT("Section channel/default/key-value projection is partial or unsealed."));
		}
		if (!IsRangeWellFormed(Section.Range))
		{
			Issue(TEXT("invalid_section_range"), TEXT("error"), Section.SectionId,
				Section.ClassName, TEXT("Section range bounds are contradictory."));
		}
	}
	for (const FHyperAICinematicsTrackRecord& Track : Snapshot.Tracks)
	{
		if (Track.SectionCount != SectionsPerTrack.FindRef(Track.TrackId))
		{
			Issue(TEXT("track_section_count_mismatch"), TEXT("error"), Track.TrackId,
				Track.ClassName,
				TEXT("Track section count differs from detached section references."));
		}
	}
	if (DeclaredChannelCount != Snapshot.Sequence.ChannelCount
		|| DeclaredKeyCount != Snapshot.Keys.Num())
	{
		Issue(TEXT("section_value_count_mismatch"), TEXT("error"), TEXT("counts"),
			Snapshot.Sequence.TargetPath,
			TEXT("Section channel/key totals differ from sequence and detached-key counts."));
	}

	TSet<FString> KeyIds;
	TMap<FString, int32> KeysPerSection;
	for (const FHyperAICinematicsKeyRecord& Key : Snapshot.Keys)
	{
		if (Key.KeyId.IsEmpty() || KeyIds.Contains(Key.KeyId)
			|| !SectionIds.Contains(Key.SectionId))
		{
			Issue(TEXT("invalid_key_reference"), TEXT("error"), Key.KeyId,
				Key.SectionId, TEXT("Key id must be unique and reference a detached section."));
		}
		KeyIds.Add(Key.KeyId);
		++KeysPerSection.FindOrAdd(Key.SectionId);
		if (!IsCanonicalSha256(Key.ValueFingerprint))
		{
			Issue(TEXT("key_value_projection_incomplete"), TEXT("error"), Key.KeyId,
				Key.ChannelType, TEXT("A key value has no canonical detached value seal."));
		}
		if (const FHyperAICinematicsFrameRange* Range = SectionRanges.Find(Key.SectionId))
		{
			if (!ContainsFrame(*Range, Key.Frame))
			{
				Issue(TEXT("key_outside_section_range"), TEXT("warning"), Key.KeyId,
					Key.SectionId, TEXT("A projected key lies outside its section range."));
			}
		}
	}
	for (const FHyperAICinematicsSectionRecord& Section : Snapshot.Sections)
	{
		if (Section.KeyCount != KeysPerSection.FindRef(Section.SectionId))
		{
			Issue(TEXT("section_key_count_mismatch"), TEXT("error"), Section.SectionId,
				Section.ClassName,
				TEXT("Section key count differs from detached key references."));
		}
	}

	TSet<FString> CameraIds;
	for (const FHyperAICinematicsCameraRecord& Camera : Snapshot.Cameras)
	{
		if (Camera.CameraCutId.IsEmpty() || CameraIds.Contains(Camera.CameraCutId)
			|| !SectionIds.Contains(Camera.SectionId))
		{
			Issue(TEXT("invalid_camera_cut_reference"), TEXT("error"), Camera.CameraCutId,
				Camera.SectionId, TEXT("Camera-cut id must be unique and reference a section."));
		}
		CameraIds.Add(Camera.CameraCutId);
		if (const FHyperAICinematicsFrameRange* Range = SectionRanges.Find(Camera.SectionId))
		{
			if (!RangesEqual(*Range, Camera.Range))
			{
				Issue(TEXT("camera_cut_range_mismatch"), TEXT("error"),
					Camera.CameraCutId, Camera.SectionId,
					TEXT("Camera-cut summary range differs from its detached section."));
			}
		}
		if (!IsCanonicalSha256(Camera.BindingFingerprint))
		{
			Issue(TEXT("camera_binding_projection_incomplete"), TEXT("error"),
				Camera.CameraCutId, Camera.SectionId,
				TEXT("Camera binding GUID/sequence/parent identity is not sealed."));
		}
		if (Camera.CameraBindingId.IsEmpty())
		{
			Issue(TEXT("camera_binding_unresolved"), TEXT("warning"), Camera.CameraCutId,
				Camera.SectionId, TEXT("Camera cut has no local binding GUID."));
		}
	}

	TSet<FString> EventIds;
	for (const FHyperAICinematicsEventRecord& Event : Snapshot.Events)
	{
		if (Event.EventId.IsEmpty() || EventIds.Contains(Event.EventId)
			|| !SectionIds.Contains(Event.SectionId)
			|| (Event.EventKind != TEXT("trigger") && Event.EventKind != TEXT("repeater")))
		{
			Issue(TEXT("invalid_event_reference"), TEXT("error"), Event.EventId,
				Event.SectionId, TEXT("Event id, section reference, and kind must be closed."));
		}
		EventIds.Add(Event.EventId);
		if (const FHyperAICinematicsFrameRange* Range = SectionRanges.Find(Event.SectionId))
		{
			if (!ContainsFrame(*Range, Event.Frame))
			{
				Issue(TEXT("event_outside_section_range"), TEXT("warning"),
					Event.EventId, Event.SectionId,
					TEXT("Projected event frame lies outside its section range."));
			}
		}
		if (!Event.bDetailProjectionComplete
			|| !IsCanonicalSha256(Event.DetailFingerprint))
		{
			Issue(TEXT("event_detail_projection_incomplete"), TEXT("error"),
				Event.EventId, Event.SectionId,
				TEXT("Event endpoint and payload detail is partial or unsealed."));
		}
		if (Event.PayloadVariableCount < 0
			|| Event.PayloadVariableCount > MaxEventPayloadVariables)
		{
			Issue(TEXT("event_payload_count_exceeded"), TEXT("error"), Event.EventId,
				Event.SectionId, TEXT("Event payload-variable count exceeds the v1 bound."));
		}
	}

	if (Snapshot.RenderConfig.bPresent)
	{
		if (!IsCanonicalPrimaryAssetPath(Snapshot.RenderConfig.TargetPath)
			|| Snapshot.RenderConfig.TargetPath != Request.RenderConfigPath
			|| Snapshot.RenderConfig.SettingCount != Snapshot.RenderSettings.Num())
		{
			Issue(TEXT("render_config_identity_mismatch"), TEXT("error"), TEXT("mrq_config"),
				Snapshot.RenderConfig.TargetPath,
				TEXT("MRQ config identity or setting cardinality differs from the validation target."));
		}
		TSet<FString> SettingIds;
		int32 OutputSettingCount = 0;
		for (const FHyperAICinematicsMRQSettingRecord& Setting : Snapshot.RenderSettings)
		{
			if (Setting.SettingId.IsEmpty() || SettingIds.Contains(Setting.SettingId))
			{
				Issue(TEXT("invalid_or_duplicate_mrq_setting"), TEXT("error"),
					Setting.SettingId, Setting.ClassName,
					TEXT("MRQ setting ids must be non-empty and unique."));
			}
			SettingIds.Add(Setting.SettingId);
			if (!Setting.bDetailProjectionComplete
				|| !IsCanonicalSha256(Setting.DetailFingerprint))
			{
				Issue(TEXT("mrq_setting_detail_projection_incomplete"), TEXT("error"),
					Setting.SettingId, Setting.ClassName,
					TEXT("MRQ setting semantic values are partial or unsealed."));
			}
			if (Setting.ClassName == TEXT("MoviePipelineOutputSetting"))
			{
				++OutputSettingCount;
				if (Setting.DetailFingerprint
					!= Snapshot.RenderConfig.OutputSettingFingerprint)
				{
					Issue(TEXT("mrq_output_setting_fingerprint_mismatch"), TEXT("error"),
						Setting.SettingId, Snapshot.RenderConfig.TargetPath,
						TEXT("Output setting inventory and config summary seals differ."));
				}
			}
		}
		if (Snapshot.RenderConfig.bOutputSettingPresent != (OutputSettingCount == 1))
		{
			Issue(TEXT("mrq_output_setting_count_mismatch"), TEXT("error"),
				TEXT("mrq_output"), Snapshot.RenderConfig.TargetPath,
				TEXT("Output-setting presence must correspond to exactly one inventory record."));
		}
		const FString ExpectedConfigPersisted = RecomputePersistedRevision(
			Snapshot.RenderConfig.PackageName, Snapshot.RenderConfig.DiskExistence,
			Snapshot.RenderConfig.PackageSavedHash, Snapshot.RenderConfig.DiskSize);
		if (!Snapshot.RenderConfig.bPersistedRevisionComplete
			|| !IsCanonicalSha256(Snapshot.RenderConfig.PersistedRevision)
			|| Snapshot.RenderConfig.PersistedRevision != ExpectedConfigPersisted
			|| Snapshot.RenderConfig.DiskExistence != TEXT("exists")
			|| !IsLowerHexString(Snapshot.RenderConfig.PackageSavedHash, 40)
			|| Snapshot.RenderConfig.DiskSize < 0
			|| !Snapshot.RenderConfig.bWasLoadedFromDisk)
		{
			Issue(TEXT("render_config_persisted_revision_incomplete"), TEXT("error"),
				TEXT("mrq_persisted"), Snapshot.RenderConfig.PackageName,
				TEXT("Persisted MRQ CAS is absent or inconsistent with package evidence."));
		}
		const FString ExpectedConfigVolatile = ComputeMRQVolatileRevision(Snapshot);
		if (!Snapshot.RenderConfig.bVolatileRevisionComplete
			|| !IsCanonicalSha256(Snapshot.RenderConfig.VolatileRevision)
			|| Snapshot.RenderConfig.VolatileRevision != ExpectedConfigVolatile)
		{
			Issue(TEXT("render_config_volatile_revision_incomplete"), TEXT("error"),
				TEXT("mrq_volatile"), Snapshot.RenderConfig.TargetPath,
				TEXT("Loaded MRQ values do not reproduce the asserted volatile CAS."));
		}
		if (!Request.ExpectedRenderConfigPersistedRevision.IsEmpty()
			&& Request.ExpectedRenderConfigPersistedRevision
				!= Snapshot.RenderConfig.PersistedRevision)
		{
			Issue(TEXT("stale_render_config_persisted_revision"), TEXT("error"),
				TEXT("mrq_persisted_cas"), Snapshot.RenderConfig.TargetPath,
				TEXT("Fresh persisted MRQ revision differs from the expected value."));
		}
		if (!Request.ExpectedRenderConfigVolatileRevision.IsEmpty()
			&& Request.ExpectedRenderConfigVolatileRevision
				!= Snapshot.RenderConfig.VolatileRevision)
		{
			Issue(TEXT("stale_render_config_volatile_revision"), TEXT("error"),
				TEXT("mrq_volatile_cas"), Snapshot.RenderConfig.TargetPath,
				TEXT("Fresh loaded MRQ revision differs from the expected value."));
		}
	}
	else if (!Request.RenderConfigPath.IsEmpty() || Request.Policy == TEXT("render_ready"))
	{
		Issue(TEXT("render_config_required"), TEXT("error"), TEXT("mrq_config"),
			Request.RenderConfigPath,
			TEXT("render_ready requires one exact detached MoviePipelinePrimaryConfig."));
	}

	if (Request.Policy == TEXT("render_ready"))
	{
		if (Snapshot.Cameras.IsEmpty())
		{
			Issue(TEXT("camera_cut_required"), TEXT("error"), TEXT("camera_cuts"),
				Snapshot.Sequence.TargetPath,
				TEXT("Render-ready policy requires at least one bounded camera cut."));
		}
		if (!Snapshot.RenderConfig.bOutputSettingPresent
			|| !IsCanonicalSha256(Snapshot.RenderConfig.OutputSettingFingerprint))
		{
			Issue(TEXT("mrq_output_setting_required"), TEXT("error"), TEXT("mrq_output"),
				Snapshot.RenderConfig.TargetPath,
				TEXT("Render-ready policy requires one exact projected output setting."));
		}
		if (Snapshot.RenderConfig.ShotOverrideCount != 0)
		{
			Issue(TEXT("mrq_shot_overrides_unsupported"), TEXT("error"),
				TEXT("mrq_config"), Snapshot.RenderConfig.TargetPath,
				TEXT("Render-ready v1 does not project per-shot MoviePipeline configs."));
		}
		if (Snapshot.RenderConfig.OutputWidth <= 0 || Snapshot.RenderConfig.OutputHeight <= 0
			|| Snapshot.RenderConfig.OutputRateNumerator <= 0
			|| Snapshot.RenderConfig.OutputRateDenominator <= 0
			|| Snapshot.RenderConfig.OutputDirectory.IsEmpty()
			|| Snapshot.RenderConfig.FileNameFormat.IsEmpty())
		{
			Issue(TEXT("mrq_output_contract_invalid"), TEXT("error"), TEXT("mrq_output"),
				Snapshot.RenderConfig.TargetPath,
				TEXT("Output path, format, resolution, and frame-rate contract must be non-empty and positive."));
		}
		if (Snapshot.RenderConfig.bUseCustomPlaybackRange
			&& Snapshot.RenderConfig.CustomStartFrame >= Snapshot.RenderConfig.CustomEndFrame)
		{
			Issue(TEXT("mrq_custom_range_invalid"), TEXT("error"), TEXT("mrq_output"),
				Snapshot.RenderConfig.TargetPath,
				TEXT("Custom MRQ playback range must have a strictly increasing end frame."));
		}
	}

	if (!IsCanonicalSha256(Snapshot.SnapshotFingerprint)
		|| Snapshot.SnapshotFingerprint != ComputeSnapshotFingerprint(Snapshot))
	{
		Issue(TEXT("snapshot_fingerprint_mismatch"), TEXT("error"), TEXT("snapshot"),
			Snapshot.Sequence.TargetPath,
			TEXT("Detached snapshot seal does not match its revision and completeness values."));
	}

	for (const FHyperAICinematicsIssue& Value : Report.Issues)
	{
		if (Value.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Value.Severity == TEXT("warning")) ++Report.WarningCount;
	}
	FString ValidatorCanonical;
	AppendToken(ValidatorCanonical, TEXT("hyperai.cinematics.detached-validator.v1"));
	AppendToken(ValidatorCanonical, Snapshot.SnapshotFingerprint);
	AppendToken(ValidatorCanonical, Request.Policy);
	AppendToken(ValidatorCanonical, FString::FromInt(Report.ErrorCount));
	AppendToken(ValidatorCanonical, FString::FromInt(Report.WarningCount));
	for (const FHyperAICinematicsIssue& Value : Report.Issues)
	{
		AppendToken(ValidatorCanonical, Value.Code);
		AppendToken(ValidatorCanonical, Value.Severity);
		AppendToken(ValidatorCanonical, Value.StableId);
		AppendToken(ValidatorCanonical, Value.Subject);
	}
	Report.ValidatorFingerprint = HashCanonical(ValidatorCanonical);
	Report.bTruncated = bIssueOverflow;
	Report.bComplete = Snapshot.bComplete && !bIssueOverflow
		&& IsCanonicalSha256(Report.ValidatorFingerprint);
	Report.bValid = Report.bComplete && Report.ErrorCount == 0;
	Report.bOk = Report.bComplete;
	Report.Status = Report.bValid ? TEXT("valid")
		: Report.bComplete ? TEXT("invalid") : TEXT("validation_incomplete");
	Report.Diagnostic = Report.bValid
		? TEXT("Detached bounded cinematics values satisfy the independent closed validator policy.")
		: Report.bComplete
			? TEXT("Detached bounded cinematics values violate one or more closed validation rules.")
			: TEXT("Detached validation is incomplete because capture evidence, projection, issue capacity, or the validator seal is incomplete.");
	return Report;
}

FHyperAICinematicsValidateReport FHyperAIStudioCinematicsContracts::Validate(
	const FHyperAICinematicsValidateRequest& Request)
{
	FHyperAICinematicsValidateReport Report;
	Report.Policy = Request.Policy.Left(32);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.bValid = false;
		Report.bComplete = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsCanonicalPrimaryAssetPath(Request.SequencePath)
		|| (!Request.RenderConfigPath.IsEmpty()
			&& !IsCanonicalPrimaryAssetPath(Request.RenderConfigPath))
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| !IsCanonicalSha256(Request.ExpectedVolatileRevision)
		|| (Request.RenderConfigPath.IsEmpty()
			&& (!Request.ExpectedRenderConfigPersistedRevision.IsEmpty()
				|| !Request.ExpectedRenderConfigVolatileRevision.IsEmpty()))
		|| (!Request.RenderConfigPath.IsEmpty()
			&& (!IsCanonicalSha256(Request.ExpectedRenderConfigPersistedRevision)
				|| !IsCanonicalSha256(Request.ExpectedRenderConfigVolatileRevision)))
		|| (Request.Policy != TEXT("structural") && Request.Policy != TEXT("render_ready")))
	{
		return Reject(TEXT("invalid_validation_contract"),
			TEXT("Validation requires exact paths, separate complete persisted/volatile CAS values, and a closed policy."));
	}
	if (Request.Policy == TEXT("render_ready") && Request.RenderConfigPath.IsEmpty())
	{
		return Reject(TEXT("render_config_required"),
			TEXT("render_ready requires one exact already-loaded MRQ primary config."));
	}
	if (Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Issue, game-thread, or output bounds are outside the closed validator contract."));
	}
	FHyperAIStudioCinematicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.SequencePath, Request.RenderConfigPath,
		Request.MaxGameThreadMs, Snapshot, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report = ValidateSnapshot(Snapshot, Request);
	Report.bFreshCapture = true;
	int32 EstimatedOutput = SaturatingAdd(BaseResponseBudgetBytes,
		EstimateCapabilitiesBytes(Report.Capabilities));
	for (const FHyperAICinematicsIssue& Value : Report.Issues)
	{
		EstimatedOutput = SaturatingAdd(EstimatedOutput, EstimateIssueBytes(Value));
	}
	if (EstimatedOutput > Request.MaxOutputBytes)
	{
		Report.bOk = false;
		Report.bValid = false;
		Report.bComplete = false;
		Report.bTruncated = true;
		Report.Status = TEXT("validation_output_bound_exceeded");
		Report.Diagnostic = TEXT("Independent validation values do not fit the caller's closed output envelope.");
		Report.Issues.Reset();
	}
	return Report;
}

FHyperAICinematicsInspectReport FHyperAIStudioCinematicsContracts::Inspect(
	const FHyperAICinematicsInspectRequest& Request)
{
	FHyperAICinematicsInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.bComplete = false;
		Report.bCursorEligible = false;
		Report.NextCursor.Reset();
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsCanonicalPrimaryAssetPath(Request.SequencePath)
		|| (!Request.RenderConfigPath.IsEmpty()
			&& !IsCanonicalPrimaryAssetPath(Request.RenderConfigPath)))
	{
		return Reject(TEXT("invalid_exact_path"),
			TEXT("Inspect requires one exact canonical LevelSequence path and at most one exact MRQ primary-config path."));
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Page, cursor, game-thread, or output bounds are outside the closed cinematics contract."));
	}
	FHyperAIStudioCinematicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.SequencePath, Request.RenderConfigPath,
		Request.MaxGameThreadMs, Snapshot, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.bFreshCapture = true;
	Report.bComplete = Snapshot.bComplete;
	Report.SnapshotFingerprint = Snapshot.SnapshotFingerprint;
	Report.Sequence = Snapshot.Sequence;
	Report.RenderConfig = Snapshot.RenderConfig;
	const bool bStableForPaging = Snapshot.bComplete
		&& Snapshot.Sequence.bPersistedRevisionComplete
		&& Snapshot.Sequence.bVolatileRevisionComplete
		&& (!Snapshot.RenderConfig.bPresent
			|| (Snapshot.RenderConfig.bPersistedRevisionComplete
				&& Snapshot.RenderConfig.bVolatileRevisionComplete));
	Report.bCursorEligible = bStableForPaging;
	if (!Request.Cursor.IsEmpty() && !bStableForPaging)
	{
		return Reject(TEXT("volatile_snapshot_unpageable"),
			TEXT("A continuation cursor requires complete persisted and loaded-value revisions for every projected asset."));
	}
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Request.SequencePath, Request.RenderConfigPath,
		Snapshot.SnapshotFingerprint, Request.PageSize, Offset))
	{
		return Reject(TEXT("stale_or_invalid_cursor"),
			TEXT("Cursor identity does not match the freshly recaptured exact cinematics snapshot."));
	}
	const int32 TotalItems = Snapshot.Bindings.Num() + Snapshot.Tracks.Num()
		+ Snapshot.Sections.Num() + Snapshot.Keys.Num() + Snapshot.Cameras.Num()
		+ Snapshot.Events.Num() + Snapshot.RenderSettings.Num();
	if (Offset < 0 || Offset > TotalItems)
	{
		return Reject(TEXT("cursor_offset_out_of_range"),
			TEXT("Cursor offset is outside the fresh bounded cinematics inventory."));
	}
	int32 EstimatedOutput = BaseResponseBudgetBytes;
	EstimatedOutput = SaturatingAdd(EstimatedOutput, EstimateCapabilitiesBytes(Report.Capabilities));
	EstimatedOutput = SaturatingAdd(EstimatedOutput, EstimateSequenceBytes(Report.Sequence));
	EstimatedOutput = SaturatingAdd(EstimatedOutput, EstimateConfigBytes(Report.RenderConfig));
	if (EstimatedOutput > Request.MaxOutputBytes)
	{
		return Reject(TEXT("fixed_output_envelope_exceeded"),
			TEXT("The fixed exact sequence/config/capability envelope exceeds max_output_bytes."));
	}
	for (const FHyperAICinematicsIssue& Value : Snapshot.CaptureIssues)
	{
		const int32 Bytes = EstimateIssueBytes(Value);
		if (Report.Issues.Num() >= MaxIssues
			|| SaturatingAdd(EstimatedOutput, Bytes) > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutput = SaturatingAdd(EstimatedOutput, Bytes);
		Report.Issues.Add(Value);
	}

	const int32 RequestedEnd = FMath::Min(TotalItems, Offset + Request.PageSize);
	int32 NextOffset = Offset;
	for (int32 FlatIndex = Offset; FlatIndex < RequestedEnd; ++FlatIndex)
	{
		int32 Local = FlatIndex;
		int32 Bytes = 0;
		if (Local < Snapshot.Bindings.Num())
		{
			const FHyperAICinematicsBindingRecord& Value = Snapshot.Bindings[Local];
			Bytes = SaturatingAdd(EstimateRecordBytes(
				Value.BindingId, Value.BindingGuid, Value.Name),
				EstimateRecordBytes(Value.DetailFingerprint, FString()));
		}
		else if ((Local -= Snapshot.Bindings.Num()) < Snapshot.Tracks.Num())
		{
			const FHyperAICinematicsTrackRecord& Value = Snapshot.Tracks[Local];
			Bytes = SaturatingAdd(EstimateRecordBytes(
				Value.TrackId, Value.BindingGuid, Value.ClassName),
				EstimateRecordBytes(Value.DetailFingerprint, FString()));
		}
		else if ((Local -= Snapshot.Tracks.Num()) < Snapshot.Sections.Num())
		{
			const FHyperAICinematicsSectionRecord& Value = Snapshot.Sections[Local];
			Bytes = SaturatingAdd(SaturatingAdd(512,
				EstimateRecordBytes(Value.SectionId, Value.TrackId, Value.ClassName)),
				EstimateRecordBytes(Value.DetailFingerprint, Value.ChannelFingerprint));
		}
		else if ((Local -= Snapshot.Sections.Num()) < Snapshot.Keys.Num())
		{
			const FHyperAICinematicsKeyRecord& Value = Snapshot.Keys[Local];
			Bytes = SaturatingAdd(EstimateRecordBytes(
				Value.KeyId, Value.SectionId, Value.ChannelType),
				EstimateRecordBytes(Value.ValueFingerprint, FString()));
		}
		else if ((Local -= Snapshot.Keys.Num()) < Snapshot.Cameras.Num())
		{
			const FHyperAICinematicsCameraRecord& Value = Snapshot.Cameras[Local];
			Bytes = SaturatingAdd(SaturatingAdd(384,
				EstimateRecordBytes(Value.CameraCutId, Value.SectionId, Value.CameraBindingId)),
				EstimateRecordBytes(Value.BindingFingerprint, FString()));
		}
		else if ((Local -= Snapshot.Cameras.Num()) < Snapshot.Events.Num())
		{
			const FHyperAICinematicsEventRecord& Value = Snapshot.Events[Local];
			Bytes = SaturatingAdd(EstimateRecordBytes(Value.EventId, Value.SectionId,
				Value.EventKind), EstimateRecordBytes(Value.CompiledFunctionName,
				Value.BoundObjectClassName));
			Bytes = SaturatingAdd(Bytes,
				EstimateRecordBytes(Value.DetailFingerprint, FString()));
		}
		else
		{
			Local -= Snapshot.Events.Num();
			check(Local >= 0 && Local < Snapshot.RenderSettings.Num());
			const FHyperAICinematicsMRQSettingRecord& Value = Snapshot.RenderSettings[Local];
			Bytes = EstimateRecordBytes(Value.SettingId, Value.ClassName,
				Value.DetailFingerprint);
		}
		if (SaturatingAdd(EstimatedOutput, Bytes) > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutput = SaturatingAdd(EstimatedOutput, Bytes);
		Local = FlatIndex;
		if (Local < Snapshot.Bindings.Num()) Report.Bindings.Add(Snapshot.Bindings[Local]);
		else if ((Local -= Snapshot.Bindings.Num()) < Snapshot.Tracks.Num())
			Report.Tracks.Add(Snapshot.Tracks[Local]);
		else if ((Local -= Snapshot.Tracks.Num()) < Snapshot.Sections.Num())
			Report.Sections.Add(Snapshot.Sections[Local]);
		else if ((Local -= Snapshot.Sections.Num()) < Snapshot.Keys.Num())
			Report.Keys.Add(Snapshot.Keys[Local]);
		else if ((Local -= Snapshot.Keys.Num()) < Snapshot.Cameras.Num())
			Report.Cameras.Add(Snapshot.Cameras[Local]);
		else if ((Local -= Snapshot.Cameras.Num()) < Snapshot.Events.Num())
			Report.Events.Add(Snapshot.Events[Local]);
		else
		{
			Local -= Snapshot.Events.Num();
			Report.RenderSettings.Add(Snapshot.RenderSettings[Local]);
		}
		NextOffset = FlatIndex + 1;
	}
	if (NextOffset == Offset && Offset < TotalItems)
	{
		return Reject(TEXT("page_item_exceeds_output_bound"),
			TEXT("The next exact cinematics record cannot fit the caller envelope; no non-advancing cursor is issued."));
	}
	if (NextOffset < TotalItems)
	{
		Report.bTruncated = true;
		if (bStableForPaging)
		{
			Report.NextCursor = BuildCursor(Request.SequencePath, Request.RenderConfigPath,
				Snapshot.SnapshotFingerprint, Request.PageSize, NextOffset);
			Report.bCursorEligible = !Report.NextCursor.IsEmpty();
		}
		else
		{
			Report.bCursorEligible = false;
		}
	}
	Report.bOk = true;
	if (!Snapshot.bComplete)
	{
		Report.Status = TEXT("partial_loaded_snapshot_unpageable");
		Report.Diagnostic = CaptureDiagnostic;
		Report.NextCursor.Reset();
		Report.bCursorEligible = false;
	}
	else
	{
		Report.Status = Report.bTruncated
			? TEXT("exact_loaded_snapshot_page") : TEXT("exact_loaded_snapshot");
		Report.Diagnostic = CaptureDiagnostic;
	}
	return Report;
}

FHyperAICinematicsApplyPlanReport FHyperAIStudioCinematicsContracts::BuildPlan(
	const FHyperAICinematicsApplyPlanRequest& Request)
{
	FHyperAICinematicsApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		return Report;
	};
	if (!IsSafeOperationId(Request.OperationId)
		|| !IsCanonicalPrimaryAssetPath(Request.SequencePath)
		|| !IsCanonicalPrimaryAssetPath(Request.RenderConfigPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| !IsCanonicalSha256(Request.ExpectedVolatileRevision)
		|| !IsCanonicalSha256(Request.ExpectedRenderConfigPersistedRevision)
		|| !IsCanonicalSha256(Request.ExpectedRenderConfigVolatileRevision)
		|| (!Request.ExpectedPlanHash.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_apply_contract"),
			TEXT("Apply-plan requires an operation id, exact paths, and four separate canonical persisted/volatile CAS fingerprints."));
	}
	if (Request.Intent.Lifecycle != TEXT("render_validated_sequence")
		|| Request.Intent.ExpectedTerminalState != TEXT("render_outputs_verified"))
	{
		return Reject(TEXT("unsupported_render_lifecycle"),
			TEXT("The only closed lifecycle is render_validated_sequence -> render_outputs_verified."));
	}
	if (!Request.Intent.bRequireCameraCut || !Request.Intent.bRequireOutputSetting
		|| !Request.Intent.bVerifyOutputFrames)
	{
		return Reject(TEXT("unsafe_render_intent_unsupported"),
			TEXT("The closed v1 lifecycle always requires a camera cut, exact output setting, and independent output-frame verification."));
	}
	if (Request.DeadlineMs < MinAsyncDeadlineMs || Request.DeadlineMs > MaxAsyncDeadlineMs
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"),
			TEXT("Deadline, game-thread, or output bounds are outside the closed render-preflight contract."));
	}
	const int32 FixedPlanOutput = SaturatingAdd(BaseResponseBudgetBytes,
		EstimateCapabilitiesBytes(Report.Capabilities));
	if (FixedPlanOutput > Request.MaxOutputBytes)
	{
		return Reject(TEXT("fixed_output_envelope_exceeded"),
			TEXT("The fixed exact capability and render-plan envelope exceeds max_output_bytes."));
	}
	if (!Request.bDryRun && Request.ExpectedPlanHash.IsEmpty())
	{
		return Reject(TEXT("expected_plan_hash_required"),
			TEXT("Non-dry intent must echo one canonical plan hash from a fresh dry run."));
	}

	FHyperAIStudioCinematicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.SequencePath, Request.RenderConfigPath,
		Request.MaxGameThreadMs, Snapshot, CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, CaptureDiagnostic);
	}
	Report.BasePersistedRevision = Snapshot.Sequence.PersistedRevision;
	Report.BaseVolatileRevision = Snapshot.Sequence.VolatileRevision;
	Report.BaseRenderConfigPersistedRevision = Snapshot.RenderConfig.PersistedRevision;
	Report.BaseRenderConfigVolatileRevision = Snapshot.RenderConfig.VolatileRevision;

	FHyperAICinematicsValidateRequest ValidationRequest;
	ValidationRequest.SequencePath = Request.SequencePath;
	ValidationRequest.RenderConfigPath = Request.RenderConfigPath;
	ValidationRequest.ExpectedPersistedRevision = Request.ExpectedPersistedRevision;
	ValidationRequest.ExpectedVolatileRevision = Request.ExpectedVolatileRevision;
	ValidationRequest.ExpectedRenderConfigPersistedRevision =
		Request.ExpectedRenderConfigPersistedRevision;
	ValidationRequest.ExpectedRenderConfigVolatileRevision =
		Request.ExpectedRenderConfigVolatileRevision;
	ValidationRequest.Policy = TEXT("render_ready");
	ValidationRequest.MaxIssues = MaxIssues;
	ValidationRequest.MaxGameThreadMs = Request.MaxGameThreadMs;
	ValidationRequest.MaxOutputBytes = Request.MaxOutputBytes;
	const FHyperAICinematicsValidateReport Validation =
		ValidateSnapshot(Snapshot, ValidationRequest);
	int32 PlanOutput = FixedPlanOutput;
	for (const FHyperAICinematicsIssue& Value : Validation.Issues)
	{
		PlanOutput = SaturatingAdd(PlanOutput, EstimateIssueBytes(Value));
	}
	if (PlanOutput > Request.MaxOutputBytes)
	{
		return Reject(TEXT("render_preflight_output_bound_exceeded"),
			TEXT("Detached render-preflight issues do not fit max_output_bytes."));
	}
	Report.Issues = Validation.Issues;
	if (!Validation.bValid)
	{
		return Reject(Validation.Status == TEXT("invalid")
			? TEXT("render_preflight_invalid") : TEXT("render_preflight_incomplete"),
			Validation.Diagnostic);
	}
	if (Request.Intent.bRequireCameraCut && Snapshot.Cameras.IsEmpty())
	{
		return Reject(TEXT("camera_cut_required"),
			TEXT("The render intent requires one bounded camera cut."));
	}
	if (Request.Intent.bRequireOutputSetting
		&& !Snapshot.RenderConfig.bOutputSettingPresent)
	{
		return Reject(TEXT("mrq_output_setting_required"),
			TEXT("The render intent requires one exact MoviePipelineOutputSetting."));
	}

	const TSharedRef<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe>();
	Payload->SequencePath = Request.SequencePath;
	Payload->RenderConfigPath = Request.RenderConfigPath;
	Payload->BasePersistedRevision = Snapshot.Sequence.PersistedRevision;
	Payload->BaseVolatileRevision = Snapshot.Sequence.VolatileRevision;
	Payload->BaseRenderConfigPersistedRevision = Snapshot.RenderConfig.PersistedRevision;
	Payload->BaseRenderConfigVolatileRevision = Snapshot.RenderConfig.VolatileRevision;
	Payload->OutputSettingFingerprint = Snapshot.RenderConfig.OutputSettingFingerprint;
	Payload->Lifecycle = Request.Intent.Lifecycle;
	Payload->ExpectedTerminalState = Request.Intent.ExpectedTerminalState;
	Payload->AsyncDeadlineMs = Request.DeadlineMs;
	Payload->bRequireCameraCut = Request.Intent.bRequireCameraCut;
	Payload->bRequireOutputSetting = Request.Intent.bRequireOutputSetting;
	Payload->bVerifyOutputFrames = Request.Intent.bVerifyOutputFrames;
	Payload->SemanticFingerprint = ComputeRenderSemanticFingerprint(*Payload);
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_identity_unavailable"),
			TEXT("The closed render-preflight payload could not be semantically sealed."));
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
			TEXT("The render-preflight payload did not produce an independent exact immutable clone."));
	}
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (ProjectId.IsEmpty())
	{
		return Reject(TEXT("canonical_project_identity_unavailable"),
			TEXT("Typed render-preflight preparation requires the canonical project identity."));
	}
	const FHyperAIStudioDomainAdapterDescriptor& AdapterDescriptor = GetAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_cinematics_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = AdapterDescriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.Sequencer"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("plugin.MovieRenderPipeline"), EHyperAIStudioDomainPrerequisiteState::Available},
		{LiveProbeId, EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bExternalEffectAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = RenderPayloadTypeId;
	Contract.ArtifactSchemaFingerprint = RenderPayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Contract.EffectTarget = TEXT("cinematics:") + Snapshot.SnapshotFingerprint;
	Contract.DeadlineMs = FMath::Min(Request.DeadlineMs,
		FHyperAIStudioTypedArtifactLimits::MaxSynchronousDeadlineMs);
	Contract.MaxNativeOperations = 8;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
	// The shared synchronous sealer couples compile to save. This source candidate
	// seals preflight/CAS only and never asks that core contract to compile or save.
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = false;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.BeginCallCount = 1;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bWouldCompileOnce = true;
	Report.Effects.bWouldSubmitRenderOnce = true;
	Report.Effects.bWouldPollWithoutBlocking = true;
	Report.Effects.bWouldValidateFreshOnce = true;
	Report.Effects.bWouldVerifyOutputFrames = Request.Intent.bVerifyOutputFrames;
	Report.Effects.bWouldObserveTerminalState = true;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_execution_blocked");
		Report.Diagnostic = TEXT("Separate sequence/MRQ persisted and loaded CAS, detached render-ready validation, immutable typed payload, and pure plan hashes are valid. No compile, save, queue mutation, render submission, polling, staging, or execution occurred.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("expected_plan_hash_mismatch"),
			TEXT("Non-dry intent must echo the exact current dry-run plan hash."));
	}
	return Reject(NonDryCallableState,
		TEXT("Zero effects: no artifact was staged or submitted; no Sequencer compile, save, queue mutation, render, poll, cancellation, or output verification ran. A bounded pinned continuation host is required before this lifecycle can execute."));
}

FHyperAICinematicsInspectReport UHyperAIStudioCinematicsToolset::hyper_cinematics_inspect(
	const FHyperAICinematicsInspectRequest& Request)
{
	return FHyperAIStudioCinematicsContracts::Inspect(Request);
}

FHyperAICinematicsApplyPlanReport UHyperAIStudioCinematicsToolset::hyper_cinematics_apply_plan(
	const FHyperAICinematicsApplyPlanRequest& Request)
{
	return FHyperAIStudioCinematicsContracts::BuildPlan(Request);
}

FHyperAICinematicsValidateReport UHyperAIStudioCinematicsToolset::hyper_cinematics_validate(
	const FHyperAICinematicsValidateRequest& Request)
{
	return FHyperAIStudioCinematicsContracts::Validate(Request);
}

FHyperAIStudioCinematicsDomainAdapter::FHyperAIStudioCinematicsDomainAdapter()
	: Descriptor(FHyperAIStudioCinematicsContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioCinematicsDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioCinematicsDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
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
			TEXT("Cinematics adapter rejected pack, adapter, or bounded typed DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_cinematics_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioCinematicsContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioCinematicsContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCinematicsContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioCinematicsInspectPayload& Typed =
			static_cast<const FHyperAIStudioCinematicsInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioCinematicsInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioCinematicsInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioCinematicsContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_cinematics_validate")
		&& Context.Binding.VariantId == FHyperAIStudioCinematicsContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioCinematicsContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCinematicsContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioCinematicsValidatePayload& Typed =
			static_cast<const FHyperAIStudioCinematicsValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioCinematicsValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioCinematicsValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioCinematicsContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_cinematics_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioCinematicsContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::ExternalEffect
		&& Payload.GetTypeId() == FHyperAIStudioCinematicsContracts::RenderPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioCinematicsContracts::RenderPayloadSchemaFingerprint())
	{
		const FHyperAIStudioCinematicsRenderPayload& Typed =
			static_cast<const FHyperAIStudioCinematicsRenderPayload&>(Payload);
		if (FHyperAIStudioCinematicsContracts::ComputeRenderSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The exact typed render-preflight semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioCinematicsContracts::NonDryCallableState,
			TEXT("Zero effects: the synchronous cinematics adapter cannot compile, save, submit, poll, cancel, verify, or reconcile a render lifecycle."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Cinematics adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioCinematicsRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioCinematicsRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioCinematicsRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioCinematicsRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioCinematicsContracts::IsRegistrationAllowed(bDev)
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioCinematicsToolset::StaticClass(),
			FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioCinematicsRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioCinematicsRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioCinematicsContracts::IsRegistrationAllowed(bDev))
	{
		UE_LOG(LogHyperAIStudioCinematics, Verbose,
			TEXT("Cinematics exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("LevelSequence"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("MovieScene"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("MovieSceneTracks"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("Sequencer"))
		|| !FModuleManager::Get().IsModuleLoaded(TEXT("MovieRenderPipelineCore")))
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioCinematicsDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioCinematics, Error,
			TEXT("Cinematics adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioCinematicsContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioCinematics, Error,
			TEXT("Cinematics live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("LevelSequence"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("MovieScene"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("MovieSceneTracks"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("Sequencer"))
		&& FModuleManager::Get().IsModuleLoaded(TEXT("MovieRenderPipelineCore"))
		&& ULevelSequence::StaticClass() != nullptr
		&& UMovieScene::StaticClass() != nullptr
		&& UMoviePipelinePrimaryConfig::StaticClass() != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("Sequencer and Movie Render Pipeline public modules are already loaded; the probe loaded no asset, opened no editor, and ran no render action.")
		: TEXT("Cinematics source cohort remains unavailable without every already-loaded hard requirement.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioCinematics, Error,
			TEXT("Cinematics live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioCinematicsToolset::StaticClass(),
		FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioCinematics, Error,
			TEXT("Cinematics three-tool cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioCinematicsRegistration::RollBackRegistration()
{
	if (!IsInGameThread()) return;
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioCinematicsToolset::StaticClass(),
			FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioCinematics, Error,
				TEXT("Cinematics owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioCinematics, Error,
				TEXT("Cinematics probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioCinematics, Error,
				TEXT("Cinematics adapter rollback failed closed: %s"), *Error);
		}
		else
		{
			AdapterHandle = {};
			Adapter.Reset();
		}
	}
}
